#!/usr/bin/env python3
"""Pack measurements for submission, and read submissions back.

    uarch_submit.py usable build/runs                 exit 0 if those runs can be submitted
    uarch_submit.py pack build/runs/run-*.json -o build/submission [--copy] [--open]
    uarch_submit.py pack build/runs/run-*.json -o build/submission --into results
    uarch_submit.py show build/submission/submission.txt
    uarch_submit.py rebuild results/x/y.samples.json -o y.json --source pr

A submission is everything `merge` needs to recompute the published
statistics, and nothing else: the chip description, each run's conditions,
and every run's value of every measured number.  The text that describes the
instructions (assembly, chains, groups) is not sent: it follows from
insns/*.def at the same tool version, so the receiving side regenerates it
and the submission carries a digest of it (`spec_sha256`) to prove they match.

The text form ("armour") is gzip + base64 between BEGIN/END lines, small
enough to paste into one GitHub issue field.  Only the standard library is
used.
"""

from __future__ import annotations

import argparse
import base64
import gzip
import hashlib
import json
import re
import shutil
import subprocess
import sys
import urllib.parse
import zlib
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import gen_insns  # noqa: E402
import uarch_results as ur  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
REPO = "useless-husband/m5-uarch"
SCHEMA = "m5-uarch-submission/1"
# Tool versions whose instruction table and measurement code produce
# comparable data.  The instruction table itself is checked by digest.
SUPPORTED_TOOL_VERSIONS = ("0.1.1", "0.2.0")
MIN_RUNS, MAX_RUNS = 3, 9
# An issue body holds 65 536 characters; the form's headings, the licence
# line and a few notes need the rest.
MAX_TEXT = 60_000
# Decompressed size limit: a real submission is about 150 KB.
MAX_JSON = 4 << 20
BEGIN = "-----BEGIN M5-UARCH SUBMISSION-----"
END = "-----END M5-UARCH SUBMISSION-----"
LINE = 76

HELPER_KEYS = ("cmp_csinc_roundtrip", "fmov_roundtrip", "fcmp_fcsel_roundtrip")
VIA = {
    gen_insns.HELP_CMP: "cmp", gen_insns.HELP_CSINC: "csinc",
    gen_insns.HELP_FMOV_XD: "fmov x,d", gen_insns.HELP_FMOV_DX: "fmov d,x",
    gen_insns.HELP_FCMP: "fcmp", gen_insns.HELP_FCSEL: "fcsel",
}

# ---------------------------------------------------------------------------
# What may be sent.  Everything is an allowlist: a key that is not named here
# is dropped by the packer and rejected by the validator.

MACHINE_KEYS = ("brand", "model", "os_version", "os_build", "page_size", "virtual_machine",
                "pauth_keys_active")
LEVEL_KEYS = ("label", "name", "cores", "l1i_bytes", "l1d_bytes", "l2_bytes", "cores_per_l2")
RUN_KEYS = ("seconds", "counters", "timed_runs", "clean", "discarded_migrated",
            "discarded_disturbed", "load_average", "ghz_observed", "measured")
EXP_KEYS = ("id", "title", "unit", "note", "runs")
EXP_CURVE_KEYS = ("xlabel", "ylabel", "curve")
TOP_KEYS = ("schema", "tool_version", "spec_sha256", "machine", "runs", "helpers",
            "instructions", "structure", "stats_sha256")

PATTERNS = {
    "brand": re.compile(r"Apple [A-Z][0-9]{1,2}[A-Z]?( [A-Z][a-z]{1,8}){0,2}"),
    "model": re.compile(r"[A-Za-z]{2,16}[0-9]{1,3},[0-9]{1,3}"),
    "os_version": re.compile(r"[0-9]{1,3}(\.[0-9]{1,3}){0,2}"),
    "os_build": re.compile(r"[0-9]{2}[A-Z][0-9]{1,5}[a-z]?"),
    "label": re.compile(r"[PEM]"),
    "name": re.compile(r"[A-Za-z][A-Za-z ]{0,23}"),
    "counters": re.compile(r"[a-z_]{1,32}"),
}

# Things that identify a person or a machine and must never appear in any
# string of a submission, whatever key it sits under.
FORBIDDEN = [
    ("a file path", re.compile(r"(^|[\s\"'(=:])(/Users/|/home/|/private/|/var/folders/|~/)")),
    ("an e-mail address", re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+\.[A-Za-z0-9.-]+")),
    ("a UUID", re.compile(r"[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-"
                          r"[0-9A-Fa-f]{12}")),
    ("a serial number", re.compile(r"\b(?=[A-Z0-9]*[0-9][A-Z0-9]*[0-9][A-Z0-9]*[0-9])"
                                   r"(?=[A-Z0-9]*[A-Z][A-Z0-9]*[A-Z][A-Z0-9]*[A-Z])[A-Z0-9]{10,12}\b")),
    ("a host name", re.compile(r"\b[\w-]+\.local\b")),
]

# One run's value of one number: milli-units, "x" (ok but no value) or a
# status word, optionally followed by ":c" (throughput limited by its own
# chain) or ":r" (latency is a round trip).
VALUE_RE = re.compile(r"(-?[0-9]{1,7}|x|[a-z][a-z-]{0,15})(:[cr])?")
STATUS_RE = re.compile(r"[a-z][a-z-]{0,15}")


class SubmissionError(Exception):
    """A submission (or the runs to be packed) is unusable; the message says why."""


# ---------------------------------------------------------------------------
# The instruction table, regenerated from insns/*.def


@dataclass(frozen=True)
class Spec:
    entries: tuple
    digest: str


def spec_from_insns(insns: list) -> Spec:
    entries = []
    for ins in insns:
        s = ins.spec
        paths = []
        for ch in ins.chains:
            p = {"from": ch.src, "to": ch.dst}
            if ch.helper:
                p["via"] = VIA[ch.helper]
            if ch.tied:
                p["tied"] = True
            p["chain"] = "; ".join(ch.lines)
            paths.append(p)
        entries.append({"name": s.name, "group": s.group, "ext": s.ext, "asm": ins.text,
                        "note": s.attrs.get("note", ""), "tp": bool(ins.tp_lines),
                        "paths": paths})
    text = json.dumps(entries, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return Spec(tuple(entries), hashlib.sha256(text.encode()).hexdigest())


@lru_cache(maxsize=4)
def load_spec(defs_dir: str = str(ROOT / "insns")) -> Spec:
    paths = sorted(Path(defs_dir).glob("*.def"))
    if not paths:
        raise SubmissionError(f"no instruction definitions in {defs_dir}")
    return spec_from_insns(gen_insns.load(paths))


def check_run_against_spec(run: dict, spec: Spec) -> list[str]:
    """Does this run's instruction metadata come from the same table?"""
    problems: list[str] = []
    pos = {e["name"]: i for i, e in enumerate(spec.entries)}
    last = -1
    for ins in run.get("instructions", []):
        i = pos.get(ins.get("name"))
        if i is None:
            problems.append(f"instruction {ins.get('name')!r} is not in insns/*.def")
            continue
        if i <= last:
            problems.append(f"instruction {ins['name']!r} is out of order")
        last = i
        e = spec.entries[i]
        for k in ("group", "ext", "asm"):
            if ins.get(k) != e[k]:
                problems.append(f"{ins['name']}: {k} differs from insns/*.def")
        if ins.get("note", "") != e["note"]:
            problems.append(f"{ins['name']}: note differs from insns/*.def")
        for core, c in ins.items():
            if not isinstance(c, dict) or c.get("supported") is False:
                continue
            if ("tp" in c) != e["tp"]:
                problems.append(f"{ins['name']} {core}: throughput presence differs")
            lat = c.get("lat", [])
            if len(lat) != len(e["paths"]):
                problems.append(f"{ins['name']} {core}: {len(lat)} latency paths, "
                                f"insns/*.def has {len(e['paths'])}")
                continue
            for got, want in zip(lat, e["paths"]):
                if (got.get("from"), got.get("to"), got.get("chain"), got.get("via"),
                        bool(got.get("tied"))) != (want["from"], want["to"], want["chain"],
                                                   want.get("via"), bool(want.get("tied"))):
                    problems.append(f"{ins['name']} {core}: chain {got.get('from')}->"
                                    f"{got.get('to')} differs from insns/*.def")
    return problems


# ---------------------------------------------------------------------------
# Packing (the privacy filter is here: only allowlisted keys are copied)


def _machine(run: dict) -> dict:
    m = run["machine"]
    out = {k: m[k] for k in MACHINE_KEYS if k in m}
    out["levels"] = [{k: lv[k] for k in LEVEL_KEYS if k in lv} for lv in m.get("levels", [])]
    return out


def _milli(v: float, what: str) -> str:
    m = round(v * 1000)
    if abs(v * 1000 - m) > 1e-6:
        raise SubmissionError(f"{what}: {v} has more than three decimals")
    return str(m)


def _value(status, v, flag: str, what: str) -> str:
    if status == "ok":
        s = "x" if v is None else _milli(v, what)
    elif isinstance(status, str) and STATUS_RE.fullmatch(status) and status != "x":
        s = status
    else:
        raise SubmissionError(f"{what}: unexpected status {status!r}")
    return s + (":" + flag if flag else "")


def _cell(name: str, core: str, per_run: list) -> str | None:
    if all(p is None for p in per_run):
        return None
    if any(p is None for p in per_run):
        raise SubmissionError(f"{name} {core}: measured in some runs only")
    supported = [p.get("supported") is not False for p in per_run]
    if not any(supported):
        return "!" + str(int(per_run[0].get("signal") or 0))
    if not all(supported):
        raise SubmissionError(f"{name} {core}: the runs disagree on whether it executes")
    if len({"tp" in p for p in per_run}) != 1 or len({len(p.get("lat", [])) for p in per_run}) != 1:
        raise SubmissionError(f"{name} {core}: the runs measured different things")
    parts = [""]
    if "tp" in per_run[0]:
        parts[0] = ",".join(_value(p["tp"].get("status"), p["tp"].get("per_cycle"),
                                   "c" if p["tp"].get("chain_bound") else "", name)
                            for p in per_run)
    for k in range(len(per_run[0].get("lat", []))):
        parts.append(",".join(_value(p["lat"][k].get("status"), p["lat"][k].get("cycles"),
                                     "r" if p["lat"][k].get("roundtrip") else "", name)
                              for p in per_run))
    return "|".join(parts)


def _structure(runs: list[dict], core: str) -> list[dict]:
    ids: list[str] = []
    for r in runs:
        for e in r.get("structure", {}).get(core, []):
            if e["id"] not in ids:
                ids.append(e["id"])
    out = []
    for eid in ids:
        per = [next((e for e in r.get("structure", {}).get(core, []) if e["id"] == eid), None)
               for r in runs]
        seen = [e for e in per if e is not None]
        ok = [e for e in seen if e["status"] == "ok" and e["value"] is not None]
        base = seen[0]
        x = {"id": eid, "title": base["title"], "unit": base["unit"]}
        if ur.conclusive(len(ok), len(seen)):
            pick = ok[ur.pick_index([e["value"] for e in ok])]
            x["note"] = pick.get("note", "")
            if pick.get("curve"):
                x["xlabel"] = pick.get("xlabel", "")
                x["ylabel"] = pick.get("ylabel", "")
                x["curve"] = sorted([ur._round(a), ur._round(b, 4)] for a, b in pick["curve"])
        else:
            x["note"] = base.get("note", "")
        x["runs"] = [None if e is None else [e["status"], e["value"], e.get("confidence", "low")]
                     for e in per]
        out.append(x)
    return out


def pack(runs: list[dict], spec: Spec | None = None) -> dict:
    """Raw `uarch all` runs of one machine -> a submission (allowlisted fields only)."""
    spec = spec or load_spec()
    if not MIN_RUNS <= len(runs) <= MAX_RUNS:
        raise SubmissionError(f"{len(runs)} runs; a submission needs {MIN_RUNS} to {MAX_RUNS} "
                              f"(make measure takes 3)")
    for r in runs:
        if r.get("schema") != ur.RAW_SCHEMA:
            raise SubmissionError("not a raw uarch run (write runs with `uarch all -o`)")
    versions = {r.get("tool_version") for r in runs}
    if len(versions) != 1:
        raise SubmissionError(f"runs from different tool versions: {sorted(map(str, versions))}")
    version = versions.pop()
    if version not in SUPPORTED_TOOL_VERSIONS:
        raise SubmissionError(f"tool version {version} is not accepted "
                              f"({', '.join(SUPPORTED_TOOL_VERSIONS)}); update and measure again")
    machine = _machine(runs[0])
    for r in runs[1:]:
        if _machine(r) != machine:
            raise SubmissionError("the runs come from different machines or macOS versions")
    if machine.get("virtual_machine"):
        raise SubmissionError("measured in a virtual machine")
    for r in runs:
        problems = check_run_against_spec(r, spec)
        if problems:
            raise SubmissionError("this build does not match insns/*.def (run `make` and "
                                  "measure again): " + problems[0])
    labels = [lv["label"] for lv in machine["levels"]]
    cores = [lb for lb in labels
             if any(ur._level(r, lb).get("measured") for r in runs)]
    sub_runs = []
    for r in runs:
        rr = r["run"]
        sub_runs.append({
            "seconds": r.get("seconds"),
            "counters": rr["counters"],
            "timed_runs": rr["runs"],
            "clean": rr["clean"],
            "discarded_migrated": rr["discarded_migrated"],
            "discarded_disturbed": rr["discarded_disturbed"],
            "load_average": rr["load_average"],
            "ghz_observed": {lv["label"]: lv["ghz_observed"] for lv in r["machine"]["levels"]
                             if lv.get("ghz_observed")},
            "measured": [lv["label"] for lv in r["machine"]["levels"] if lv.get("measured")],
        })
    helpers = {c: {k: [r.get("helpers", {}).get(c, {}).get(k) for r in runs] for k in HELPER_KEYS}
               for c in cores}
    indexes = [{i["name"]: i for i in r.get("instructions", [])} for r in runs]
    insns = {}
    for c in cores:
        insns[c] = [_cell(e["name"], c, [ix.get(e["name"], {}).get(c) for ix in indexes])
                    for e in spec.entries]
    sub = {
        "schema": SCHEMA,
        "tool_version": version,
        "spec_sha256": spec.digest,
        "machine": machine,
        "runs": sub_runs,
        "helpers": helpers,
        "instructions": insns,
        "structure": {c: _structure(runs, c) for c in cores
                      if any(r.get("structure", {}).get(c) for r in runs)},
    }
    merged = ur.merge(expand(sub, spec))
    # Guard against a packing bug: the submission must reproduce exactly what
    # merging the raw runs gives, minus the fields that are not sent.
    direct = ur.merge(runs)
    dm = direct["machine"]
    direct["machine"] = {k: dm[k] for k in MACHINE_KEYS if k in dm}
    direct["machine"]["levels"] = [{k: lv[k] for k in LEVEL_KEYS + ("measured", "ghz_observed")
                                    if k in lv} for lv in dm["levels"]]
    for r in direct["runs"]:
        r.pop("started", None)
    # (Compared as values: a raw run may say 9.0 where the tool prints 9.)
    if direct != merged:
        raise SubmissionError("internal error: the packed data does not reproduce the merge")
    sub["stats_sha256"] = stats_digest(merged)
    problems = privacy_problems(sub)
    if problems:
        raise SubmissionError("refusing to pack: " + problems[0])
    return sub


# ---------------------------------------------------------------------------
# Unpacking


def _parse_value(tok: str, what: str):
    m = VALUE_RE.fullmatch(tok)
    if not m:
        raise SubmissionError(f"{what}: unreadable value {tok[:20]!r}")
    v, flag = m.group(1), (m.group(2) or "")[1:]
    if v == "x":
        return "ok", None, flag
    if v[0] in "-0123456789":
        # The tool prints whole numbers without a decimal point (2, not
        # 2.000), and the canonical results text keeps that difference.
        q, rem = divmod(int(v), 1000)
        return "ok", (q if rem == 0 else int(v) / 1000), flag
    if v == "ok":
        raise SubmissionError(f"{what}: status 'ok' without a value")
    return v, None, flag


def _decode_cell(cell: str, entry: dict, n_runs: int, what: str) -> list[dict]:
    if cell.startswith("!"):
        if not re.fullmatch(r"![0-9]{1,3}", cell):
            raise SubmissionError(f"{what}: unreadable cell")
        return [{"supported": False, "signal": int(cell[1:])} for _ in range(n_runs)]
    parts = cell.split("|")
    if len(parts) != 1 + len(entry["paths"]):
        raise SubmissionError(f"{what}: {len(parts) - 1} latency paths, the instruction table "
                              f"has {len(entry['paths'])}")
    if bool(parts[0]) != entry["tp"]:
        raise SubmissionError(f"{what}: throughput presence differs from the instruction table")

    def per_run(part: str) -> list:
        toks = part.split(",")
        if len(toks) != n_runs:
            raise SubmissionError(f"{what}: {len(toks)} values for {n_runs} runs")
        return [_parse_value(t, what) for t in toks]

    out = [{} for _ in range(n_runs)]
    if parts[0]:
        for i, (st, v, flag) in enumerate(per_run(parts[0])):
            if flag not in ("", "c"):
                raise SubmissionError(f"{what}: flag :{flag} on a throughput")
            tp = {"status": st, "per_cycle": v}
            if flag:
                tp["chain_bound"] = True
            out[i]["tp"] = tp
    for i in range(n_runs):
        out[i]["lat"] = []
    for k, part in enumerate(parts[1:]):
        p = entry["paths"][k]
        for i, (st, v, flag) in enumerate(per_run(part)):
            if flag not in ("", "r"):
                raise SubmissionError(f"{what}: flag :{flag} on a latency")
            lat = {"from": p["from"], "to": p["to"], "status": st, "cycles": v}
            if "via" in p:
                lat["via"] = p["via"]
            if flag:
                lat["roundtrip"] = True
            if p.get("tied"):
                lat["tied"] = True
            lat["chain"] = p["chain"]
            out[i]["lat"].append(lat)
    return out


def expand(sub: dict, spec: Spec) -> list[dict]:
    """A submission -> raw-run records that `uarch_results.merge` accepts.

    Assumes the shape has been checked (`uarch_validate.check_format`)."""
    n = len(sub["runs"])
    runs = []
    # Records are built in a fixed key order: the canonical results text (and
    # so stats_sha256) must not depend on the order keys arrived in.
    for i, r in enumerate(sub["runs"]):
        levels = []
        for lv in sub["machine"]["levels"]:
            x = {k: lv[k] for k in LEVEL_KEYS}
            x["measured"] = lv["label"] in r["measured"]
            g = r["ghz_observed"].get(lv["label"])
            if g:
                x["ghz_observed"] = g
            levels.append(x)
        machine = {k: sub["machine"][k] for k in MACHINE_KEYS if k in sub["machine"]}
        machine["levels"] = levels
        runs.append({
            "schema": ur.RAW_SCHEMA, "tool_version": sub["tool_version"], "seconds": r["seconds"],
            "machine": machine,
            "run": {"counters": r["counters"], "runs": r["timed_runs"], "clean": r["clean"],
                    "discarded_migrated": r["discarded_migrated"],
                    "discarded_disturbed": r["discarded_disturbed"],
                    "load_average": r["load_average"]},
            "helpers": {c: {k: v[i] for k, v in hs.items() if v[i] is not None}
                        for c, hs in sub["helpers"].items()},
            "instructions": [],
            "structure": {},
        })
    cells = {c: cs for c, cs in sub["instructions"].items()}
    for j, e in enumerate(spec.entries):
        per_core = {}
        for c, cs in cells.items():
            if cs[j] is not None:
                per_core[c] = _decode_cell(cs[j], e, n, f"{e['name']} {c}")
        if not per_core:
            continue
        for i in range(n):
            ins = {"name": e["name"], "group": e["group"], "ext": e["ext"], "asm": e["asm"]}
            if e["note"]:
                ins["note"] = e["note"]
            for c, rs in per_core.items():
                ins[c] = rs[i]
            runs[i]["instructions"].append(ins)
    for c, exps in sub["structure"].items():
        for x in exps:
            for i, rv in enumerate(x["runs"]):
                if rv is None:
                    continue
                e = {"id": x["id"], "title": x["title"], "unit": x["unit"], "status": rv[0],
                     "value": rv[1], "confidence": rv[2], "note": x["note"]}
                for k in EXP_CURVE_KEYS:
                    if k in x:
                        e[k] = x[k]
                runs[i]["structure"].setdefault(c, []).append(e)
    return runs


def stats_digest(merged: dict) -> str:
    core = {k: v for k, v in merged.items() if k not in ("dataset", "source")}
    return hashlib.sha256(ur.dump_results(core).encode()).hexdigest()


def rebuild(sub: dict, spec: Spec, dataset: str | None = None, source: dict | None = None) -> dict:
    """The published results file for a submission."""
    merged = ur.merge(expand(sub, spec))
    if dataset is None:
        return merged
    out = {"schema": merged["schema"], "dataset": dataset, "source": source or {}}
    out.update((k, v) for k, v in merged.items() if k != "schema")
    return out


def _ordered(d, keys) -> dict:
    """d with the known keys first, in the format's order (unknown keys kept, last)."""
    if not isinstance(d, dict):
        return d
    out = {k: d[k] for k in keys if k in d}
    out.update((k, v) for k, v in d.items() if k not in out)
    return out


def normalize(sub: dict) -> dict:
    """The key order the packer writes (JSON text from elsewhere may be sorted)."""
    sub = _ordered(sub, TOP_KEYS)
    m = sub.get("machine")
    if isinstance(m, dict):
        m = _ordered(m, MACHINE_KEYS + ("levels",))
        if isinstance(m.get("levels"), list):
            m["levels"] = [_ordered(lv, LEVEL_KEYS) for lv in m["levels"]]
        sub["machine"] = m
    levels = [lv.get("label") for lv in (m or {}).get("levels", []) if isinstance(lv, dict)] \
        if isinstance(m, dict) else []
    if isinstance(sub.get("runs"), list):
        sub["runs"] = [_ordered(r, RUN_KEYS) for r in sub["runs"]]
        for r in sub["runs"]:
            if isinstance(r, dict) and isinstance(r.get("ghz_observed"), dict):
                r["ghz_observed"] = _ordered(r["ghz_observed"], levels)
    if isinstance(sub.get("helpers"), dict):
        sub["helpers"] = {c: _ordered(h, HELPER_KEYS) for c, h in sub["helpers"].items()}
    for key in ("helpers", "instructions", "structure"):
        if isinstance(sub.get(key), dict):
            sub[key] = _ordered(sub[key], levels)
    if isinstance(sub.get("structure"), dict):
        for c, exps in sub["structure"].items():
            if isinstance(exps, list):
                sub["structure"][c] = [_ordered(x, EXP_KEYS[:4] + EXP_CURVE_KEYS + ("runs",))
                                       for x in exps]
    return sub


def canonical(sub: dict) -> str:
    return json.dumps(sub, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def dataset_id(sub: dict) -> str:
    """Content address: the same data submitted twice gets the same id."""
    return hashlib.sha256(canonical(sub).encode()).hexdigest()[:12]


def chip_slug(brand: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", brand.lower()).strip("-")


def privacy_problems(obj, where: str = "") -> list[str]:
    """Strings anywhere in obj that look like a path, address, UUID, serial or host name."""
    found: list[str] = []
    if isinstance(obj, dict):
        for k, v in obj.items():
            found += privacy_problems(k, where)
            found += privacy_problems(v, f"{where}.{k}" if where else str(k))
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            found += privacy_problems(v, f"{where}[{i}]")
    elif isinstance(obj, str):
        for what, rx in FORBIDDEN:
            if rx.search(obj):
                found.append(f"{where or 'a key'} contains what looks like {what}")
    return found


# ---------------------------------------------------------------------------
# Files: the samples file (stored next to the results) and the text armour


def dump_samples(sub: dict) -> str:
    """One instruction cell / experiment per line, like the results files."""
    def one(obj) -> str:
        return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)

    lines = ["{"]
    for k in sub:
        if k in ("instructions", "structure"):
            continue
        lines.append(f" {json.dumps(k)}:{one(sub[k])},")
    for key, last in (("instructions", False), ("structure", True)):
        lines.append(f" {json.dumps(key)}:{{")
        cores = list(sub[key])
        for ci, c in enumerate(cores):
            lines.append(f"  {json.dumps(c)}:[")
            items = sub[key][c]
            for i, x in enumerate(items):
                lines.append("   " + one(x) + ("," if i + 1 < len(items) else ""))
            lines.append("  ]" + ("," if ci + 1 < len(cores) else ""))
        lines.append(" }" + ("" if last else ","))
    lines.append("}")
    return "\n".join(lines) + "\n"


def encode(sub: dict) -> str:
    raw = canonical(sub).encode()
    b64 = base64.b64encode(gzip.compress(raw, 9, mtime=0)).decode()
    m = sub["machine"]
    head = [BEGIN, f"format: {SCHEMA}", f"chip: {m['brand']} ({m['model']})",
            f"sha256: {hashlib.sha256(raw).hexdigest()}"]
    return "\n".join(head + [b64[i:i + LINE] for i in range(0, len(b64), LINE)] + [END]) + "\n"


def decode(text: str) -> dict:
    """Find the armoured block in any text (an issue body) and return the submission."""
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    starts = [m.start() for m in re.finditer(re.escape(BEGIN), text)]
    if not starts:
        raise SubmissionError("no submission found: paste the whole text that `make submit` "
                              f"copied, from {BEGIN} to {END}")
    if len(starts) > 1:
        raise SubmissionError("more than one submission in the text; send one per issue")
    end = text.find(END, starts[0])
    if end < 0:
        raise SubmissionError(f"the submission is cut off: the {END} line is missing")
    lines = [ln.strip() for ln in text[starts[0] + len(BEGIN):end].split("\n")]
    lines = [ln for ln in lines if ln]
    head: dict[str, str] = {}
    while lines and re.fullmatch(r"[a-z0-9]{1,16}: .{1,200}", lines[0]):
        k, v = lines.pop(0).split(": ", 1)
        head[k] = v
    if head.get("format") != SCHEMA:
        raise SubmissionError(f"unknown submission format {head.get('format')!r}; "
                              f"this repository reads {SCHEMA}")
    if not re.fullmatch(r"[0-9a-f]{64}", head.get("sha256", "")):
        raise SubmissionError("the sha256 line is missing")
    body = "".join(lines)
    if not re.fullmatch(r"[A-Za-z0-9+/]*={0,2}", body):
        raise SubmissionError("the submission text contains characters that do not belong "
                              "to it; paste it unchanged")
    try:
        gz = base64.b64decode(body, validate=True)
    except ValueError:
        raise SubmissionError("the submission text is damaged (base64)") from None
    d = zlib.decompressobj(wbits=31)
    try:
        raw = d.decompress(gz, MAX_JSON + 1)
    except zlib.error:
        raise SubmissionError("the submission text is damaged (gzip)") from None
    if len(raw) > MAX_JSON or d.unconsumed_tail:
        raise SubmissionError("the submission is larger than any real one; refused")
    if not d.eof:
        raise SubmissionError("the submission is incomplete: part of the text is missing")
    if hashlib.sha256(raw).hexdigest() != head["sha256"]:
        raise SubmissionError("the submission does not match its sha256 line: it was changed "
                              "after packing")
    try:
        sub = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise SubmissionError("the submission is not valid JSON") from None
    if not isinstance(sub, dict):
        raise SubmissionError("the submission is not a JSON object")
    return normalize(sub)


@dataclass
class IssueSubmission:
    submission: dict
    agreed: bool


AGREE_RE = re.compile(r"^\s*[-*]\s*\[[xX]\][^\n]*\bMIT\b", re.M)


def parse_issue(body: str) -> IssueSubmission:
    """An issue body (from the submission form, or typed by hand) -> its submission."""
    if not isinstance(body, str) or not body.strip():
        raise SubmissionError("the issue is empty")
    if len(body) > 70_000:
        raise SubmissionError("the issue body is longer than GitHub allows")
    return IssueSubmission(decode(body), bool(AGREE_RE.search(body)))


def issue_url(sub: dict) -> str:
    m = sub["machine"]
    q = urllib.parse.urlencode({"template": "submission.yml",
                                "title": f"Submission: {m['brand']} ({m['model']})"})
    return f"https://github.com/{REPO}/issues/new?{q}"


# ---------------------------------------------------------------------------


def _load_runs(paths: list[Path]) -> list[dict]:
    try:
        return [ur.load_raw(p) for p in sorted(paths)]
    except ur.ResultError as e:
        raise SubmissionError(str(e)) from None


def write_submission(sub: dict, outdir: Path, spec: Spec) -> dict[str, Path]:
    """submission.txt (for the issue), submission.json (to read) and the two
    files a pull request adds under results/<chip>/."""
    text = encode(sub)
    if len(text) > MAX_TEXT:
        raise SubmissionError(f"the submission is {len(text)} characters, more than fits in an "
                              f"issue ({MAX_TEXT}); measure with fewer runs (RUNS=3) or use the "
                              f"pull-request path in CONTRIBUTING.md")
    outdir.mkdir(parents=True, exist_ok=True)
    slug, did = chip_slug(sub["machine"]["brand"]), dataset_id(sub)
    chipdir = outdir / "results" / slug
    if chipdir.is_dir():
        shutil.rmtree(chipdir)
    chipdir.mkdir(parents=True)
    paths = {"text": outdir / "submission.txt", "json": outdir / "submission.json",
             "samples": chipdir / f"{did}.samples.json", "results": chipdir / f"{did}.json"}
    paths["text"].write_text(text)
    paths["json"].write_text(json.dumps(sub, indent=1, ensure_ascii=False) + "\n")
    paths["samples"].write_text(dump_samples(sub))
    paths["results"].write_text(ur.dump_results(rebuild(sub, spec, did, {"kind": "pr"})))
    return paths


def _summary(sub: dict, text: str) -> str:
    m = sub["machine"]
    n_vals = sum(1 for cs in sub["instructions"].values() for c in cs if c
                 for part in c.split("|") if part and not c.startswith("!"))
    levels = " + ".join(f"{lv['cores']} {lv['label']}" for lv in m["levels"])
    return "\n".join([
        f"chip        {m['brand']} ({m['model']}), {levels} cores",
        f"macOS       {m['os_version']} ({m['os_build']})",
        f"tool        {sub['tool_version']}, {len(sub['runs'])} runs",
        f"values      {n_vals} instruction figures per run, "
        f"{sum(len(x) for x in sub['structure'].values())} structure experiments",
        f"size        {len(text)} characters (an issue holds 65536)",
        "not sent    host name, user name, serial number, UUIDs, file paths, memory size, "
        "start times",
    ])


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="pack and read m5-uarch submissions")
    sub = ap.add_subparsers(dest="cmd", required=True)
    u = sub.add_parser("usable", help="exit 0 if the runs in DIR can be submitted as they are")
    u.add_argument("dir", type=Path)
    p = sub.add_parser("pack", help="pack raw runs into a submission")
    p.add_argument("runs", nargs="+", type=Path)
    p.add_argument("-o", "--outdir", type=Path, required=True)
    p.add_argument("--copy", action="store_true", help="copy the text to the clipboard (pbcopy)")
    p.add_argument("--open", action="store_true", help="open the new-issue page in a browser")
    p.add_argument("--into", type=Path, help="also add the dataset files to this results/ "
                   "directory, for a pull request")
    s = sub.add_parser("show", help="decode a submission (text or issue body) and summarise it")
    s.add_argument("file", type=Path)
    r = sub.add_parser("rebuild", help="results file from a samples file")
    r.add_argument("samples", type=Path)
    r.add_argument("-o", "--output", type=Path, required=True)
    r.add_argument("--source", default="pr", help="pr, issue:N or commit:SHA")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "usable":
            files = list(args.dir.glob("run-*.json"))
            if not files:
                raise SubmissionError(f"no earlier measurement in {args.dir}")
            runs = _load_runs(files)
            pack(runs)
            print(f"{len(runs)} runs in {args.dir} can be submitted")
        elif args.cmd == "pack":
            spec = load_spec()
            subm = pack(_load_runs(args.runs), spec)
            paths = write_submission(subm, args.outdir, spec)
            text = paths["text"].read_text()
            print(_summary(subm, text))
            print(f"\nwrote {paths['text']} (what you paste) and {paths['json']} (the same, "
                  f"readable)\n      {paths['results'].parent}/ (for a pull request)")
            if args.into:
                dest = args.into / paths["results"].parent.name
                dest.mkdir(parents=True, exist_ok=True)
                added = []
                for key in ("results", "samples"):
                    target = dest / paths[key].name
                    if target.exists():
                        raise SubmissionError(f"{target} exists already: this data was added before")
                    shutil.copyfile(paths[key], target)
                    added.append(str(target))
                m, branch = subm["machine"], f"results-{dest.name}-{dataset_id(subm)}"
                print("\nAdded " + " and ".join(added) + ". For a pull request:")
                print(f"  git checkout -b {branch}")
                print(f"  git add {' '.join(added)}")
                print(f"  git commit -m \"data: {m['brand']} ({m['model']})\"")
                print(f"  git push -u origin {branch}        (origin = your fork)")
                print("then open the pull request on GitHub. A bot checks the files and comments.")
                return 0
            url = issue_url(subm)
            copied = False
            if args.copy and shutil.which("pbcopy"):
                copied = subprocess.run(["pbcopy"], input=text.encode()).returncode == 0
            print("\nTo submit with a GitHub account:")
            print("  1. Open " + url)
            print("  2. Click into the Submission box and paste (Cmd-V)"
                  + ("; the text is on the clipboard." if copied
                     else f": the text is in {paths['text']}."))
            print("  3. Tick the licence box and click Create.")
            print("A bot checks the data within a few minutes and answers on the issue.")
            if args.open and shutil.which("open"):
                subprocess.run(["open", url], check=False)
        elif args.cmd == "show":
            subm = decode(args.file.read_text())
            print(_summary(subm, encode(subm)))
            print(f"dataset id  {dataset_id(subm)}")
        elif args.cmd == "rebuild":
            subm = json.loads(args.samples.read_text())
            src = args.source
            if src == "pr":
                source = {"kind": "pr"}
            elif re.fullmatch(r"issue:[0-9]{1,7}", src):
                source = {"kind": "issue", "number": int(src.split(":")[1])}
            elif re.fullmatch(r"commit:[0-9a-f]{7,40}", src):
                source = {"kind": "commit", "commit": src.split(":")[1]}
            else:
                raise SubmissionError(f"bad --source {src!r}")
            did = args.samples.name.removesuffix(".samples.json")
            args.output.write_text(ur.dump_results(rebuild(subm, load_spec(), did, source)))
            print(f"wrote {args.output}")
    except (SubmissionError, gen_insns.DefError) as e:
        print(f"uarch_submit: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
