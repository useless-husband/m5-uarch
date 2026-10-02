#!/usr/bin/env python3
"""Check submitted results automatically, and say why.

    uarch_validate.py issue   --event EVENT.json --results results --out DIR
    uarch_validate.py pr      --event EVENT.json --changes changes.tsv
                              (--git-ref REF | --head-dir DIR) --results results --out DIR
    uarch_validate.py tree    results
    uarch_validate.py install DIR REPO_ROOT

`issue` reads a submission from a GitHub issue body, `pr` reads the dataset
files a pull request adds, `tree` re-checks every committed dataset (CI), and
`install` copies an accepted issue submission into results/ (the publishing
job).  `issue` and `pr` write verdict.json and comment.md into DIR, and for an
accepted or flagged issue submission the files of the new dataset.

Everything submitted is treated as data: it is parsed, never executed, and
no submitted string reaches the bot's comment unless it matched a strict
pattern first.  Only the standard library is used.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import shutil
import statistics
import subprocess
import sys
import traceback
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import uarch_results as ur  # noqa: E402
import uarch_submit as us  # noqa: E402

CHECKS = [
    ("licence", "Licence box ticked"),
    ("format", "Format and privacy"),
    ("version", "Tool version and instruction table"),
    ("statistics", "Statistics recomputed from the per-run values"),
    ("anchors", "Physical anchors (add = 1 cycle, cmp + csinc = 2, nothing wider than the core)"),
    ("consistency", "Internal consistency (unit counts, P- against E-core, cache sizes)"),
    ("quality", "Run quality (disturbed loops, inconclusive experiments)"),
    ("duplicate", "Not submitted before"),
    ("outliers", "Comparison with earlier datasets of this chip"),
]
MAX_FILE = 2 << 20
DATASET_FILE_RE = re.compile(r"results/([a-z0-9][a-z0-9-]{0,39})/([a-z0-9][a-z0-9-]{0,39})"
                             r"(\.samples)?\.json")
STRUCT_STATUS = ("ok", "inconclusive", "failed")
CELL_RE = re.compile(r"[!0-9a-z,|:x-]{0,600}")
TEXT_RE = re.compile(r"[^\x00-\x08\x0b-\x1f\x7f]{0,600}")

# Outlier rule: a value is flagged when it lies further from the median of the
# earlier datasets than  max(K * 1.4826 * MAD, rel * |median|, floor)  plus
# half of its own run-to-run range plus half of theirs.  MAD needs three
# earlier datasets; with fewer only the relative and absolute floors apply.
K_MAD = 5.0
TOLERANCE = {"lat": (0.05, 0.05), "tp": (0.15, 0.10), "st": (0.15, 0.0)}
# Honest repeat measurements leave a few values outside the rule (on the M5,
# at most 3 of about 4750 per comparison; docs/DESIGN.md).  A whole submission
# is flagged when clearly more than that are.
FLAG_SHARE = 0.005


def _safe(s) -> str:
    """A submitted string, only if it is plainly harmless in Markdown."""
    s = str(s)
    return s if re.fullmatch(r"[A-Za-z0-9_ .,:()+/-]{0,60}", s) else "(not shown)"


def _num(v) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)


def _fmt(v, digits: int = 3) -> str:
    if v is None:
        return "-"
    s = f"{v:.{digits}f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


@dataclass
class Finding:
    level: str  # "reject", "flag", or "note" (reported, does not change the verdict)
    rule: str
    text: str


@dataclass
class Outlier:
    key: str
    label: str
    value: float
    lo: float
    hi: float
    center: float
    prev_lo: float
    prev_hi: float
    n: int


@dataclass
class Report:
    findings: list = field(default_factory=list)
    ran: set = field(default_factory=set)
    sub: dict | None = None
    results: dict | None = None
    dataset: str | None = None
    chip: str | None = None
    outliers: list = field(default_factory=list)
    compared: int = 0
    n_prev: int = 0

    def add(self, level: str, rule: str, text: str) -> None:
        self.findings.append(Finding(level, rule, text))

    @property
    def verdict(self) -> str:
        if any(f.level == "reject" for f in self.findings):
            return "rejected"
        if any(f.level == "flag" for f in self.findings):
            return "flagged"
        return "accepted"


# ---------------------------------------------------------------------------
# Format and privacy


def check_format(sub) -> list[str]:
    """Strict shape check of a submission.  Every key is on an allowlist."""
    p: list[str] = []
    if not isinstance(sub, dict):
        return ["the submission is not a JSON object"]
    extra = sorted(set(sub) - set(us.TOP_KEYS))
    missing = sorted(set(us.TOP_KEYS) - set(sub))
    if extra:
        p.append("fields that are not part of the format: " + ", ".join(map(_safe, extra[:5])))
    if missing:
        p.append("missing fields: " + ", ".join(missing))
    if p:
        return p
    if sub["schema"] != us.SCHEMA:
        p.append(f"format is {_safe(sub['schema'])}, expected {us.SCHEMA}")
    if not isinstance(sub["tool_version"], str) or not re.fullmatch(r"[0-9]{1,3}(\.[0-9]{1,3}){1,2}",
                                                                    sub["tool_version"]):
        p.append("tool_version is not a version number")
    for k in ("spec_sha256", "stats_sha256"):
        if not isinstance(sub[k], str) or not re.fullmatch(r"[0-9a-f]{64}", sub[k]):
            p.append(f"{k} is not a SHA-256 digest")

    m = sub["machine"]
    labels: list[str] = []
    if not isinstance(m, dict):
        p.append("machine is not an object")
    else:
        extra = sorted(set(m) - set(us.MACHINE_KEYS) - {"levels"})
        if extra:
            p.append("machine has fields that are never sent: " + ", ".join(map(_safe, extra[:5])))
        for k in ("brand", "model", "os_version", "os_build"):
            if not isinstance(m.get(k), str) or not us.PATTERNS[k].fullmatch(m[k]):
                p.append(f"machine.{k} is missing or does not look like one")
        if m.get("page_size") not in (4096, 16384, 65536):
            p.append("machine.page_size is not a page size")
        if not isinstance(m.get("virtual_machine"), bool):
            p.append("machine.virtual_machine is missing")
        if "pauth_keys_active" in m and not isinstance(m["pauth_keys_active"], bool):
            p.append("machine.pauth_keys_active is not true or false")
        levels = m.get("levels")
        if not isinstance(levels, list) or not 1 <= len(levels) <= 3:
            p.append("machine.levels must list one to three core types")
        else:
            for lv in levels:
                if not isinstance(lv, dict) or set(lv) != set(us.LEVEL_KEYS):
                    p.append("machine.levels has an entry with the wrong fields")
                    continue
                if not isinstance(lv["label"], str) or not us.PATTERNS["label"].fullmatch(lv["label"]):
                    p.append("machine.levels has an unknown label")
                    continue
                if not isinstance(lv["name"], str) or not us.PATTERNS["name"].fullmatch(lv["name"]):
                    p.append("machine.levels has an unreadable name")
                for k, hi in (("cores", 64), ("l1i_bytes", 1 << 30), ("l1d_bytes", 1 << 30),
                              ("l2_bytes", 1 << 34), ("cores_per_l2", 64)):
                    if not isinstance(lv[k], int) or isinstance(lv[k], bool) or not 0 <= lv[k] <= hi:
                        p.append(f"machine.levels {lv['label']}: {k} is out of range")
                labels.append(lv["label"])
            if len(set(labels)) != len(labels):
                p.append("machine.levels repeats a label")

    runs = sub["runs"]
    n = len(runs) if isinstance(runs, list) else 0
    if not isinstance(runs, list) or not us.MIN_RUNS <= n <= us.MAX_RUNS:
        p.append(f"a submission has {us.MIN_RUNS} to {us.MAX_RUNS} runs")
        return p
    for i, r in enumerate(runs):
        if not isinstance(r, dict) or set(r) != set(us.RUN_KEYS):
            p.append(f"run {i + 1} has the wrong fields")
            continue
        if not (_num(r["seconds"]) and 0 <= r["seconds"] <= 86400):
            p.append(f"run {i + 1}: seconds out of range")
        if not isinstance(r["counters"], str) or not us.PATTERNS["counters"].fullmatch(r["counters"]):
            p.append(f"run {i + 1}: counters is unreadable")
        for k in ("timed_runs", "clean", "discarded_migrated", "discarded_disturbed"):
            if not isinstance(r[k], int) or isinstance(r[k], bool) or not 0 <= r[k] <= 10**9:
                p.append(f"run {i + 1}: {k} out of range")
        if isinstance(r["clean"], int) and isinstance(r["timed_runs"], int) and r["clean"] > r["timed_runs"]:
            p.append(f"run {i + 1}: more clean loops than loops")
        la = r["load_average"]
        if not (isinstance(la, list) and len(la) == 3 and all(_num(x) and 0 <= x < 1000 for x in la)):
            p.append(f"run {i + 1}: load_average is not three numbers")
        g = r["ghz_observed"]
        if not (isinstance(g, dict) and all(k in labels and _num(v) and 0.2 <= v <= 10
                                            for k, v in g.items())):
            p.append(f"run {i + 1}: ghz_observed is unreadable")
        ms = r["measured"]
        if not (isinstance(ms, list) and all(x in labels for x in ms) and len(set(ms)) == len(ms)):
            p.append(f"run {i + 1}: measured names unknown core types")
    if p:
        return p
    measured = {x for r in runs for x in r["measured"]}
    if not measured:
        return ["no core type was measured"]
    for k in ("helpers", "instructions"):
        if isinstance(sub[k], dict) and set(sub[k]) != measured:
            p.append(f"{k} must cover every measured core type")

    h = sub["helpers"]
    if not isinstance(h, dict) or not set(h) <= measured:
        p.append("helpers is not keyed by measured core type")
    else:
        for c, hs in h.items():
            if not isinstance(hs, dict) or set(hs) != set(us.HELPER_KEYS) or not all(
                    isinstance(v, list) and len(v) == n and all(x is None or _num(x) for x in v)
                    for v in hs.values()):
                p.append(f"helpers {c}: wrong shape")

    ins = sub["instructions"]
    if not isinstance(ins, dict) or not set(ins) <= measured:
        p.append("instructions is not keyed by measured core type")
    else:
        for c, cells in ins.items():
            if not isinstance(cells, list) or len(cells) > 5000 or not all(
                    x is None or (isinstance(x, str) and CELL_RE.fullmatch(x)) for x in cells):
                p.append(f"instructions {c}: unreadable cells")

    st = sub["structure"]
    if not isinstance(st, dict) or not set(st) <= measured:
        p.append("structure is not keyed by measured core type")
    else:
        for c, exps in st.items():
            if not isinstance(exps, list) or len(exps) > 300:
                p.append(f"structure {c}: not a list of experiments")
                continue
            for x in exps:
                if not isinstance(x, dict) or not set(us.EXP_KEYS) <= set(x) or not set(x) <= set(
                        us.EXP_KEYS + us.EXP_CURVE_KEYS):
                    p.append(f"structure {c}: an experiment has the wrong fields")
                    break
                if not isinstance(x["id"], str) or not re.fullmatch(r"[a-z0-9_]{1,40}", x["id"]):
                    p.append(f"structure {c}: unreadable experiment id")
                    break
                for k in ("title", "unit", "note", "xlabel", "ylabel"):
                    if k in x and not (isinstance(x[k], str) and TEXT_RE.fullmatch(x[k])):
                        p.append(f"structure {c} {x['id']}: {k} is not plain text")
                if "curve" in x and not (isinstance(x["curve"], list) and len(x["curve"]) <= 600 and all(
                        isinstance(pt, list) and len(pt) == 2 and all(_num(v) for v in pt)
                        for pt in x["curve"])):
                    p.append(f"structure {c} {x['id']}: unreadable curve")
                rs = x["runs"]
                if not (isinstance(rs, list) and len(rs) == n and all(
                        rv is None or (isinstance(rv, list) and len(rv) == 3 and rv[0] in STRUCT_STATUS
                                       and (rv[1] is None or _num(rv[1]))
                                       and rv[2] in ur.CONFIDENCE) for rv in rs)):
                    p.append(f"structure {c} {x['id']}: unreadable per-run values")
    p += us.privacy_problems(sub)
    return p


# ---------------------------------------------------------------------------
# Rules on the merged results


def _st(m: dict, core: str, eid: str):
    for e in m.get("structure", {}).get(core, []):
        if e["id"] == eid:
            return e["value"] if e["status"] == "ok" else None
    return None


def _tp(m: dict, core: str, name: str):
    for ins in m["instructions"]:
        if ins["name"] == name:
            t = ins.get(core, {}).get("tp")
            return t[0] if t else None
    return None


def consistency(m: dict, rep: Report) -> None:
    """Relations that hold on any Apple core; a hard limit rejects, a surprise flags."""
    levels = {lv["label"]: lv for lv in m["machine"]["levels"]}
    for c in m["cores"]:
        w = _st(m, c, "width")
        if w is not None and not 2 <= w <= 16:
            rep.add("reject", "consistency", f"{c}-core pipeline width {_fmt(w)} is outside 2 to 16")
        for e in m.get("structure", {}).get(c, []):
            if e["id"].startswith("units_") and e["status"] == "ok" and w and e["value"] > w * 1.06:
                rep.add("reject", "consistency", f"{c}-core {e['id']} = {_fmt(e['value'])} exceeds "
                        f"the pipeline width {_fmt(w)}")
        l1 = _st(m, c, "l1_latency")
        if l1 is not None and not 2 <= l1 <= 8:
            rep.add("reject", "consistency", f"{c}-core L1 load-to-use latency {_fmt(l1)} cycles "
                    "is outside 2 to 8")
        rob = _st(m, c, "rob_nop")
        if rob is not None and not 64 <= rob <= 16384:
            rep.add("reject", "consistency", f"{c}-core reorder buffer {_fmt(rob)} is outside 64 to 16384")
        mp = _st(m, c, "mispredict_penalty")
        if mp is not None and not 4 <= mp <= 40:
            rep.add("flag", "consistency", f"{c}-core misprediction penalty {_fmt(mp)} cycles is "
                    "outside 4 to 40")
        nop = _tp(m, c, "nop")
        if nop is not None and w and abs(nop - w) / w > 0.10:
            rep.add("flag", "consistency", f"{c}-core: NOP throughput in the table ({_fmt(nop)}) and "
                    f"the width experiment ({_fmt(w)}) measure the same thing but disagree")
        alu, add = _st(m, c, "units_alu"), _tp(m, c, "add_x_imm")
        if alu and add is not None and add > alu * 1.06:
            rep.add("flag", "consistency", f"{c}-core: add throughput {_fmt(add)} exceeds the "
                    f"ALU count {_fmt(alu)}")
        l1d, lv = _st(m, c, "l1d_size"), levels.get(c, {})
        if l1d and lv.get("l1d_bytes") and not 0.5 <= l1d * 1024 / lv["l1d_bytes"] <= 2:
            rep.add("flag", "consistency", f"{c}-core: measured L1D {_fmt(l1d)} KiB, macOS reports "
                    f"{lv['l1d_bytes'] // 1024} KiB")
    if "P" in m["cores"] and "E" in m["cores"]:
        for eid in ("width", "rob_nop", "units_alu", "units_load", "units_fp_add", "prf_int"):
            p, e = _st(m, "P", eid), _st(m, "E", eid)
            if p is not None and e is not None and p < e * 0.95:
                rep.add("flag", "consistency", f"{eid}: the P-core ({_fmt(p)}) is smaller than the "
                        f"E-core ({_fmt(e)})")
        gp, ge = levels["P"].get("ghz_observed"), levels["E"].get("ghz_observed")
        if gp and ge and gp < ge * 0.9:
            rep.add("flag", "consistency", f"P-cores ran at {gp} GHz, slower than the E-cores ({ge})")


def quality(m: dict, rep: Report) -> None:
    total = sum(r["timed_runs"] for r in m["runs"]) or 1
    dist = sum(r["discarded_disturbed"] for r in m["runs"]) / total
    mig = sum(r["discarded_migrated"] for r in m["runs"]) / total
    if dist > 0.25:
        rep.add("flag", "quality", f"{dist:.0%} of the timed loops were disturbed: the machine was "
                "busy; quit other programs and measure again")
    if mig > 0.05:
        rep.add("flag", "quality", f"{mig:.0%} of the timed loops migrated between core types")
    exps = [e for c in m["cores"] for e in m.get("structure", {}).get(c, [])]
    bad = sum(1 for e in exps if e["status"] != "ok")
    if exps and bad > 0.2 * len(exps):
        rep.add("flag", "quality", f"{bad} of {len(exps)} structure experiments were inconclusive")


# ---------------------------------------------------------------------------
# Outliers against other datasets of the same chip


@dataclass(frozen=True)
class Val:
    med: float
    lo: float
    hi: float
    kind: str
    conf: str | None
    label: str


def values(m: dict) -> dict[str, Val]:
    """Every comparable number in a results file, keyed so that datasets line up."""
    out: dict[str, Val] = {}
    for ins in m["instructions"]:
        paths = ins.get("paths", [])
        for c in m["cores"]:
            x = ins.get(c)
            if not x or x.get("unsupported"):
                continue
            if x.get("tp"):
                t = x["tp"]
                out[f"{c}|tp|{ins['name']}"] = Val(t[0], t[1], t[2], "tp", None,
                                                    f"{c} throughput {ins['name']}")
            for k, t in enumerate(x.get("lat") or []):
                if t:
                    p = paths[k] if k < len(paths) else {"from": "?", "to": "?"}
                    out[f"{c}|lat|{ins['name']}|{k}"] = Val(
                        t[0], t[1], t[2], "lat", None,
                        f"{c} latency {ins['name']} {p['from']}->{p['to']}")
    for c, hs in m.get("helpers", {}).items():
        for k, t in hs.items():
            out[f"{c}|helper|{k}"] = Val(t[0], t[1], t[2], "lat", None, f"{c} {k}")
    for c, exps in m.get("structure", {}).items():
        for e in exps:
            if e["status"] == "ok" and e["value"] is not None:
                out[f"{c}|st|{e['id']}"] = Val(e["value"], e.get("min", e["value"]),
                                               e.get("max", e["value"]), "st", e["confidence"],
                                               f"{c} {e['id']}")
    return out


def outliers(new: dict[str, Val], others: list[dict[str, Val]]) -> tuple[list[Outlier], int]:
    found: list[Outlier] = []
    compared = 0
    for key, v in new.items():
        if v.conf == "low":
            continue
        prev = [o[key] for o in others if key in o and o[key].conf != "low"]
        if not prev:
            continue
        compared += 1
        meds = [p.med for p in prev]
        center = statistics.median(meds)
        scale = 1.4826 * statistics.median([abs(x - center) for x in meds]) if len(prev) >= 3 else 0.0
        rel, floor = TOLERANCE[v.kind]
        slack = (v.hi - v.lo) / 2 + statistics.median([(p.hi - p.lo) / 2 for p in prev])
        tol = max(K_MAD * scale, rel * abs(center), floor) + slack
        if abs(v.med - center) > tol:
            found.append(Outlier(key, v.label, v.med, v.lo, v.hi, center, min(meds), max(meds),
                                 len(prev)))
    return found, compared


# ---------------------------------------------------------------------------
# Datasets on disk


def load_datasets(chipdir: Path) -> list[tuple[str, dict, dict | None]]:
    """(id, results, samples or None) for every dataset in results/<chip>/."""
    out = []
    if not chipdir.is_dir():
        return out
    for p in sorted(chipdir.glob("*.json")):
        if p.name.endswith(".samples.json"):
            continue
        res = ur.load_results(p)
        sp = p.with_name(p.stem + ".samples.json")
        out.append((p.stem, res, json.loads(sp.read_text()) if sp.is_file() else None))
    return out


def validate_submission(sub, existing: list, spec: us.Spec, dataset: str | None = None,
                        source: dict | None = None, rep: Report | None = None) -> Report:
    """Run every rule on one submission.  `existing` are the datasets already
    published for the same chip (see load_datasets)."""
    rep = rep or Report()
    rep.sub = sub
    rep.ran.add("format")
    for msg in check_format(sub):
        rep.add("reject", "format", msg)
    if rep.verdict == "rejected":
        return rep
    rep.chip = us.chip_slug(sub["machine"]["brand"])
    rep.ran.add("version")
    if sub["tool_version"] not in us.SUPPORTED_TOOL_VERSIONS:
        rep.add("reject", "version", f"tool version {sub['tool_version']} is not accepted "
                f"({', '.join(us.SUPPORTED_TOOL_VERSIONS)}); update the tool and measure again")
    if sub["spec_sha256"] != spec.digest:
        rep.add("reject", "version", "measured with a different instruction table than this "
                "repository's insns/*.def (modified or out of date): update, run make, measure again")
    for c, cells in sub["instructions"].items():
        if len(cells) != len(spec.entries):
            rep.add("reject", "version", f"{c}: {len(cells)} instruction cells, the table has "
                    f"{len(spec.entries)}")
    if rep.verdict == "rejected":
        return rep
    rep.ran.add("statistics")
    try:
        merged = ur.merge(us.expand(sub, spec))
    except (us.SubmissionError, ur.ResultError) as e:
        rep.add("reject", "format", _safe_error(e))
        return rep
    except Exception:  # crafted input that the shape check let through; never crash the bot
        traceback.print_exc()
        rep.add("reject", "format", "the data could not be processed (details in the workflow log)")
        return rep
    if us.stats_digest(merged) != sub["stats_sha256"]:
        rep.add("reject", "statistics", "the statistics recomputed from the per-run values differ "
                "from the ones the tool computed when packing (stats_sha256): the values were "
                "changed after packing")
    rep.dataset = dataset or us.dataset_id(sub)
    rep.results = us.rebuild(sub, spec, rep.dataset, source)
    for k in ("anchors", "consistency", "quality", "duplicate", "outliers"):
        rep.ran.add(k)
    for msg in ur.check(merged)[:12]:
        rep.add("reject", "anchors", msg)
    consistency(merged, rep)
    quality(merged, rep)
    did, sd = us.dataset_id(sub), sub["stats_sha256"]
    others = []
    for eid, res, samples in existing:
        if samples is not None and (us.dataset_id(samples) == did or samples.get("stats_sha256") == sd):
            rep.add("reject", "duplicate", f"the same measurements are already published as "
                    f"dataset {eid}")
        others.append(values(res))
    rep.n_prev = len(others)
    if others:
        rep.outliers, rep.compared = outliers(values(merged), others)
        if rep.outliers:
            share = len(rep.outliers) / max(rep.compared, 1)
            text = (f"{len(rep.outliers)} of {rep.compared} values differ from the "
                    f"{len(others)} earlier dataset(s) of this chip by more than the outlier rule "
                    f"allows ({share:.2%})")
            if share > FLAG_SHARE:
                rep.add("flag", "outliers", text + "; that is more than run-to-run noise explains "
                        "(a different chip variant, a disturbed machine, or wrong data?)")
            else:
                rep.add("note", "outliers", text + "; within what run-to-run noise produces, so "
                        "the submission is not flagged, but these values are marked on the site")
    return rep


def _safe_error(e: Exception) -> str:
    # Error texts can quote submitted names; keep only the harmless ones.
    s = str(e)
    if re.fullmatch(r"[ -~]{0,300}", s) and not re.search(r"[\[\]<>@\\]", s):
        return s
    return "the data could not be read"


# ---------------------------------------------------------------------------
# Bot comment


def _section(rep: Report, title: str | None) -> list[str]:
    lines: list[str] = []
    if title:
        lines += [f"#### {title}", ""]
    if rep.sub and rep.chip:
        m = rep.sub["machine"]
        lines.append(f"{m['brand']} ({m['model']}), macOS {m['os_version']} ({m['os_build']}), "
                     f"tool {rep.sub['tool_version']}, {len(rep.sub['runs'])} runs"
                     + (f"; dataset `{rep.dataset}`" if rep.dataset else "") + ".")
        lines.append("")
    lines += ["| Check | Result |", "|---|---|"]
    for rule, text in CHECKS:
        fs = [f for f in rep.findings if f.rule == rule]
        if rule == "licence" and rule not in rep.ran:
            continue
        if any(f.level != "note" for f in fs):
            res = ("**failed**" if any(f.level == "reject" for f in fs)
                   else f"flagged ({len([f for f in fs if f.level == 'flag'])})")
        elif fs:
            n = len(rep.outliers)
            res = f"passed; {n} value{'' if n == 1 else 's'} marked"
        elif rule == "outliers" and rule in rep.ran and not rep.n_prev:
            res = "first dataset of this chip: nothing to compare with yet"
        elif rule == "outliers" and rule in rep.ran:
            res = f"passed ({rep.compared} values against {rep.n_prev} dataset(s))"
        elif rule in rep.ran:
            res = "passed"
        else:
            res = "not run"
        lines.append(f"| {text} | {res} |")
    if rep.findings:
        lines += ["", "Details:", ""]
        for f in rep.findings[:30]:
            word = {"reject": "Rejected", "flag": "Flagged", "note": "Note"}[f.level]
            lines.append(f"- {word} ({f.rule}): {f.text}")
        if len(rep.findings) > 30:
            lines.append(f"- ... and {len(rep.findings) - 30} more")
    if rep.outliers:
        lines += ["", "| Value | This dataset (runs) | Earlier datasets |", "|---|---|---|"]
        for o in rep.outliers[:25]:
            lines.append(f"| `{o.label}` | {_fmt(o.value)} ({_fmt(o.lo)} to {_fmt(o.hi)}) | "
                         f"{_fmt(o.center)} ({_fmt(o.prev_lo)} to {_fmt(o.prev_hi)}, {o.n}) |")
        if len(rep.outliers) > 25:
            lines.append(f"| ... and {len(rep.outliers) - 25} more | | |")
    lines.append("")
    return lines


NEXT = {
    ("issue", "accepted"): "A pull request adding this dataset has been prepared; the maintainer "
    "reviews and merges it. Nothing is published before that.",
    ("issue", "flagged"): "The data passed every hard check, but the values above are unusual. A "
    "pull request has been prepared and marked as flagged; the maintainer decides. If the machine "
    "was busy, you can measure again and replace the submission text in this issue.",
    ("issue", "rejected"): "Nothing was published. Fix the problem, run `make submit` again and "
    "replace the submission text in this issue (edit it): the check runs again.",
    ("pr", "accepted"): "The maintainer reviews and merges the pull request.",
    ("pr", "flagged"): "The data passed every hard check, but the values above are unusual; the "
    "maintainer decides.",
    ("pr", "rejected"): "Please fix the problem and push again; the check runs on every push.",
}


def render_comment(reports: list[Report], kind: str, general: list[Finding] | None = None) -> str:
    general = general or []
    verdicts = [r.verdict for r in reports]
    if any(f.level == "reject" for f in general):
        verdicts.append("rejected")
    elif general:
        verdicts.append("flagged")
    verdict = ("rejected" if "rejected" in verdicts else "flagged" if "flagged" in verdicts
               else "accepted")
    lines = [f"**Submission check: {verdict}**", ""]
    for f in general:
        lines.append(f"- {'Rejected' if f.level == 'reject' else 'Flagged'}: {f.text}")
    if general:
        lines.append("")
    for r in reports:
        lines += _section(r, f"Dataset `{r.dataset}`" if kind == "pr" and r.dataset else None)
    lines += [NEXT[(kind, verdict)], "",
              "These checks catch mistakes and naive forgery. A determined forger can still "
              "fabricate consistent data; independent submissions of the same chip are the real "
              "defence, and the results site shows how many each chip has.",
              "", "<!-- m5-uarch-check -->", ""]
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Commands


def run_issue(event: dict, results_dir: Path, out: Path, spec: us.Spec | None = None) -> str:
    spec = spec or us.load_spec()
    issue = event.get("issue") or {}
    number = issue.get("number")
    if not isinstance(number, int):
        raise SystemExit("event has no issue number")
    rep = Report()
    rep.ran.add("licence")
    body = issue.get("body") or ""
    if not us.AGREE_RE.search(body):
        rep.add("reject", "licence", "tick the box agreeing that the data is published "
                "under the MIT licence")
    try:
        parsed = us.parse_issue(body)
    except us.SubmissionError as e:
        rep.ran.add("format")
        rep.add("reject", "format", _safe_error(e))
        parsed = None
    if parsed is not None:
        sub = parsed.submission
        existing = []
        brand = sub.get("machine", {}).get("brand") if isinstance(sub.get("machine"), dict) else None
        if isinstance(brand, str) and us.PATTERNS["brand"].fullmatch(brand):
            existing = load_datasets(results_dir / us.chip_slug(brand))
        try:
            validate_submission(sub, existing, spec, source={"kind": "issue", "number": number},
                                rep=rep)
        except Exception:  # never crash the bot on crafted input
            traceback.print_exc()
            rep.add("reject", "format", "the data could not be processed (details in the log)")
    verdict = rep.verdict
    out.mkdir(parents=True, exist_ok=True)
    (out / "comment.md").write_text(render_comment([rep], "issue"))
    info = {"verdict": verdict, "issue": number}
    if verdict != "rejected":
        m = rep.sub["machine"]
        info.update({"chip": rep.chip, "dataset": rep.dataset, "brand": m["brand"],
                     "model": m["model"]})
        d = out / "files" / "results" / rep.chip
        d.mkdir(parents=True, exist_ok=True)
        (d / f"{rep.dataset}.samples.json").write_text(us.dump_samples(rep.sub))
        (d / f"{rep.dataset}.json").write_text(ur.dump_results(rep.results))
        title = f"data: {m['brand']} ({m['model']}) from issue #{number}"
        (out / "commit.txt").write_text(
            f"{title}\n\nSubmitted in #{number}; automatic check: {verdict}.\n")
        (out / "pr-title.txt").write_text(title + (" [flagged]" if verdict == "flagged" else "") + "\n")
        (out / "pr.md").write_text(
            f"Adds dataset `{rep.dataset}` from #{number}.\n\nCloses #{number}\n\n"
            + render_comment([rep], "issue"))
    (out / "verdict.json").write_text(json.dumps(info, indent=1) + "\n")
    return verdict


def read_changes(path: Path) -> list[tuple[str, str]]:
    """`gh api .../pulls/N/files --jq '.[] | [.status, .filename] | @tsv'` output."""
    out = []
    for line in path.read_text().splitlines():
        if line.strip():
            status, _, name = line.partition("\t")
            out.append((status, name))
    return out


def git_reader(ref: str):
    if not re.fullmatch(r"[A-Za-z0-9/_.-]{1,100}", ref):
        raise SystemExit("bad --git-ref")

    def read(path: str) -> bytes:
        size = subprocess.run(["git", "cat-file", "-s", f"{ref}:{path}"], capture_output=True,
                              text=True, check=True).stdout.strip()
        if int(size) > MAX_FILE:
            raise ValueError("file too large")
        return subprocess.run(["git", "show", f"{ref}:{path}"], capture_output=True,
                              check=True).stdout
    return read


def dir_reader(root: Path):
    def read(path: str) -> bytes:
        p = root / path
        if p.stat().st_size > MAX_FILE:
            raise ValueError("file too large")
        return p.read_bytes()
    return read


def run_pr(changes: list[tuple[str, str]], read, results_dir: Path, out: Path,
           spec: us.Spec | None = None) -> str:
    spec = spec or us.load_spec()
    general: list[Finding] = []
    pairs: dict[tuple[str, str], dict[str, bytes]] = {}
    for status, name in changes:
        if not name.startswith("results/"):
            continue
        m = DATASET_FILE_RE.fullmatch(name)
        if not m:
            general.append(Finding("flag", "format", f"changes `{_safe(name)}`, which is not a "
                                   "dataset file: needs a maintainer"))
            continue
        if status not in ("added",):
            general.append(Finding("flag", "format", f"{status} `{name}`: changing or removing a "
                                   "published dataset needs a maintainer"))
            if status == "removed":
                continue
        try:
            data = read(name)
        except (OSError, ValueError, subprocess.CalledProcessError):
            general.append(Finding("reject", "format", f"`{name}` could not be read (too large?)"))
            continue
        pairs.setdefault((m.group(1), m.group(2)), {})["samples" if m.group(3) else "results"] = data
    reports = []
    for (slug, did), files in sorted(pairs.items()):
        rep = Report(dataset=did)
        reports.append(rep)
        if set(files) != {"samples", "results"}:
            rep.ran.add("format")
            rep.add("reject", "format", f"`results/{slug}/{did}`: add both {did}.json and "
                    f"{did}.samples.json (make submit-pr writes them)")
            continue
        try:
            sub = json.loads(files["samples"])
            res_text = files["results"].decode()
            res = json.loads(res_text)
        except (UnicodeDecodeError, json.JSONDecodeError):
            rep.ran.add("format")
            rep.add("reject", "format", f"`results/{slug}/{did}`: not valid JSON")
            continue
        existing = [d for d in load_datasets(results_dir / slug) if d[0] != did]
        try:
            validate_submission(sub, existing, spec, dataset=did, source={"kind": "pr"}, rep=rep)
        except Exception:  # never crash the bot on crafted input
            traceback.print_exc()
            rep.add("reject", "format", "the data could not be processed (details in the log)")
            continue
        if rep.chip and rep.chip != slug:
            rep.add("reject", "format", f"the data is from {rep.sub['machine']['brand']}; it "
                    f"belongs in results/{rep.chip}/")
        if rep.results is None:
            continue
        if did != us.dataset_id(sub):
            rep.add("reject", "format", f"the files must be named after the data's id, "
                    f"{us.dataset_id(sub)} (make submit-pr does this)")
        if ur.dump_results(rep.results) != res_text:
            diffs = _differences(res, rep.results)
            rep.add("reject", "statistics", f"results/{slug}/{did}.json does not follow from "
                    f"its samples file" + (": " + "; ".join(diffs) if diffs else
                                           " (fields or layout differ)"))
    if not reports and not general:
        general.append(Finding("flag", "format", "no dataset files found in this pull request"))
    out.mkdir(parents=True, exist_ok=True)
    (out / "comment.md").write_text(render_comment(reports, "pr", general))
    verdicts = [r.verdict for r in reports] + [
        "rejected" if f.level == "reject" else "flagged" for f in general]
    verdict = ("rejected" if "rejected" in verdicts else "flagged" if "flagged" in verdicts
               else "accepted")
    (out / "verdict.json").write_text(json.dumps({"verdict": verdict}, indent=1) + "\n")
    return verdict


def _differences(got, want) -> list[str]:
    """First few numbers that differ between a submitted and a recomputed results file."""
    if not isinstance(got, dict) or got.get("schema") != ur.RESULT_SCHEMA:
        return ["it is not a results file"]
    try:
        a, b = values(got), values(want)
    except (KeyError, TypeError, IndexError, AttributeError):
        return ["it does not have the shape of a results file"]
    out = []
    for k, v in b.items():
        g = a.get(k)
        if g is None or (g.med, g.lo, g.hi) != (v.med, v.lo, v.hi):
            shown = "missing" if g is None else f"{_fmt(g.med)} [{_fmt(g.lo)}, {_fmt(g.hi)}]"
            out.append(f"`{v.label}` is {shown}, the runs give {_fmt(v.med)} "
                       f"[{_fmt(v.lo)}, {_fmt(v.hi)}]")
            if len(out) == 5:
                break
    return out


def run_tree(results_dir: Path, spec: us.Spec | None = None) -> int:
    """Re-check every committed dataset.  Exit status 1 if one would be rejected."""
    spec = spec or us.load_spec()
    bad = 0
    chips = sorted(p for p in results_dir.iterdir() if p.is_dir() and p.name != "local")
    for chipdir in chips:
        sets = load_datasets(chipdir)
        vals = {eid: values(res) for eid, res, _ in sets}
        for eid, res, samples in sets:
            rep = Report(dataset=eid)
            if samples is None:
                rep.add("reject", "format", "no samples file next to it")
            elif samples.get("spec_sha256") != spec.digest:
                print(f"{chipdir.name}/{eid}: older instruction table; anchors checked, "
                      "statistics not recomputed")
                for msg in ur.check(res):
                    rep.add("reject", "anchors", msg)
            else:
                validate_submission(samples, [], spec, dataset=eid, source=res.get("source"), rep=rep)
                if rep.results is not None:
                    if us.chip_slug(samples["machine"]["brand"]) != chipdir.name:
                        rep.add("reject", "format", "stored under the wrong chip")
                    if ur.dump_results(rep.results) != (chipdir / f"{eid}.json").read_text():
                        rep.add("reject", "statistics", "results file does not follow from its "
                                "samples file: " + "; ".join(_differences(res, rep.results)))
            found, n = outliers(vals[eid], [v for k, v in vals.items() if k != eid])
            rejects = [f for f in rep.findings if f.level == "reject"]
            flags = [f for f in rep.findings if f.level == "flag"]
            line = f"{chipdir.name}/{eid}: " + ("REJECTED" if rejects else "ok")
            if flags:
                line += f", {len(flags)} flags"
            if n:
                line += f", {len(found)} of {n} values outside the other datasets"
            print(line)
            for f in rejects + flags:
                print(f"  {f.level}: {f.text}")
            bad += bool(rejects)
    return 1 if bad else 0


def install(outdir: Path, root: Path) -> str:
    """Copy an accepted or flagged issue submission into results/; print the paths."""
    info = json.loads((outdir / "verdict.json").read_text())
    if info.get("verdict") not in ("accepted", "flagged"):
        raise SystemExit("nothing to install: the submission was rejected")
    slug, did = info.get("chip", ""), info.get("dataset", "")
    if not (re.fullmatch(r"[a-z0-9][a-z0-9-]{0,39}", slug) and re.fullmatch(r"[0-9a-f]{12}", did)):
        raise SystemExit("verdict.json names an invalid chip or dataset")
    dest = root / "results" / slug
    dest.mkdir(parents=True, exist_ok=True)
    names = []
    for suffix in (".json", ".samples.json"):
        src, dst = outdir / "files" / "results" / slug / (did + suffix), dest / (did + suffix)
        if dst.exists():
            raise SystemExit(f"{dst} exists already")
        shutil.copyfile(src, dst)
        names.append(f"results/{slug}/{did}{suffix}")
    return " ".join(names)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="check submitted m5-uarch results")
    sub = ap.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("issue")
    i.add_argument("--event", type=Path, required=True)
    i.add_argument("--results", type=Path, default=Path("results"))
    i.add_argument("--out", type=Path, required=True)
    p = sub.add_parser("pr")
    p.add_argument("--changes", type=Path, required=True)
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--git-ref")
    g.add_argument("--head-dir", type=Path)
    p.add_argument("--results", type=Path, default=Path("results"))
    p.add_argument("--out", type=Path, required=True)
    t = sub.add_parser("tree")
    t.add_argument("results", type=Path)
    n = sub.add_parser("install")
    n.add_argument("outdir", type=Path)
    n.add_argument("root", type=Path)
    args = ap.parse_args(argv)
    if args.cmd == "issue":
        print(run_issue(json.loads(args.event.read_text()), args.results, args.out))
    elif args.cmd == "pr":
        read = git_reader(args.git_ref) if args.git_ref else dir_reader(args.head_dir)
        print(run_pr(read_changes(args.changes), read, args.results, args.out))
    elif args.cmd == "tree":
        return run_tree(args.results)
    elif args.cmd == "install":
        print(install(args.outdir, args.root))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
