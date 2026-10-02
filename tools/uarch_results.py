#!/usr/bin/env python3
"""Turn raw `uarch` runs into the published results.

    uarch_results.py merge run1.json run2.json ... -o results/apple-m5/apple-m5.json
    uarch_results.py csv   results/apple-m5/apple-m5.json -o results/apple-m5
    uarch_results.py site  results -o site [--reference reference]
    uarch_results.py check results/apple-m5/apple-m5.json

`merge` takes several runs of the same machine and keeps, for every number,
the median together with the smallest and largest value seen, so that the
run-to-run spread is part of the published data.  `check` re-validates the
anchors on a results file (add = 1 cycle, and so on).

Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import csv
import json
import statistics
import sys
from pathlib import Path

RAW_SCHEMA = "m5-uarch/1"
RESULT_SCHEMA = "m5-uarch-results/1"
CONFIDENCE = ["low", "medium", "high"]


class ResultError(Exception):
    pass


def _round(v: float, digits: int = 3) -> float:
    r = round(v, digits)
    return 0.0 if r == 0 else r  # no "-0.0"


def _triple(values: list[float]) -> list[float] | None:
    """[median, min, max] or None if there is nothing to summarise."""
    vals = [v for v in values if v is not None]
    if not vals:
        return None
    return [_round(statistics.median(vals)), _round(min(vals)), _round(max(vals))]


def _short(label: str) -> str:
    """'R0:x0' -> 'R0'."""
    return label.split(":", 1)[0]


def load_raw(path: Path) -> dict:
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as e:
        raise ResultError(f"{path}: {e}") from None
    if data.get("schema") != RAW_SCHEMA:
        raise ResultError(f"{path}: not a raw uarch run (schema {data.get('schema')!r})")
    return data


def merge(runs: list[dict]) -> dict:
    if not runs:
        raise ResultError("nothing to merge")
    brands = {r["machine"]["brand"] for r in runs}
    if len(brands) != 1:
        raise ResultError(f"runs come from different chips: {sorted(brands)}")
    first = runs[0]
    # Copy the level records too: they are rewritten below (median clock),
    # and the per-run values of the first run must survive for "runs".
    machine = dict(first["machine"])
    machine["levels"] = [dict(lv) for lv in machine["levels"]]
    cores = [lv["label"] for lv in machine["levels"]
             if any(_level(r, lv["label"]).get("measured") for r in runs)]
    for lv in machine["levels"]:
        ghz = [_level(r, lv["label"]).get("ghz_observed") for r in runs]
        ghz = [g for g in ghz if g]
        lv.pop("ghz_observed", None)
        lv["measured"] = lv["label"] in cores
        if ghz:
            lv["ghz_observed"] = _round(statistics.median(ghz), 2)

    out: dict = {
        "schema": RESULT_SCHEMA,
        "tool_version": first.get("tool_version", ""),
        "machine": machine,
        "cores": cores,
        "runs": [{
            # A submission leaves the start time out (see uarch_submit.py).
            **({"started": r["started"]} if r.get("started") else {}),
            "seconds": r.get("seconds"),
            "counters": r["run"]["counters"],
            "timed_runs": r["run"]["runs"],
            "clean": r["run"]["clean"],
            "discarded_migrated": r["run"]["discarded_migrated"],
            "discarded_disturbed": r["run"]["discarded_disturbed"],
            "load_average": r["run"]["load_average"],
            # The clock each core type ran at in this run (median over its
            # measurements): memory timing depends on it, and it varies.
            "ghz_observed": {lv["label"]: lv["ghz_observed"] for lv in r["machine"]["levels"]
                             if lv.get("ghz_observed")},
        } for r in runs],
        "helpers": {},
        "instructions": [],
        "structure": {},
    }

    for core in cores:
        keys = ["cmp_csinc_roundtrip", "fmov_roundtrip", "fcmp_fcsel_roundtrip"]
        merged = {}
        for k in keys:
            t = _triple([r.get("helpers", {}).get(core, {}).get(k) for r in runs])
            if t:
                merged[k] = t
        out["helpers"][core] = merged

    # Instructions: keep the order of the first run that has each name.
    order: list[str] = []
    by_name: list[dict[str, dict]] = []
    for r in runs:
        index = {i["name"]: i for i in r.get("instructions", [])}
        by_name.append(index)
        for name in index:
            if name not in order:
                order.append(name)
    seen = set()
    for name in order:
        if name in seen:
            continue
        seen.add(name)
        entries = [idx[name] for idx in by_name if name in idx]
        base = entries[0]
        item: dict = {"name": name, "group": base["group"], "ext": base.get("ext", ""),
                      "asm": base["asm"]}
        if base.get("note"):
            item["note"] = base["note"]
        paths = None
        for core in cores:
            per_core = [e[core] for e in entries if core in e]
            if not per_core:
                continue
            if all(p.get("supported") is False for p in per_core):
                item[core] = {"unsupported": True, "signal": per_core[0].get("signal")}
                continue
            per_core = [p for p in per_core if p.get("supported") is not False]
            c: dict = {}
            tps = [p["tp"] for p in per_core if "tp" in p]
            if tps:
                ok = [t["per_cycle"] for t in tps if t["status"] == "ok"]
                c["tp"] = _triple(ok)
                if c["tp"] is None:
                    c["tp_status"] = tps[0]["status"]
                if any(t.get("chain_bound") for t in tps):
                    c["tp_chain_bound"] = True
            lats = [p.get("lat", []) for p in per_core]
            n_paths = max((len(x) for x in lats), default=0)
            if n_paths and paths is None:
                ref = max(lats, key=len)
                paths = [{
                    "from": _short(x["from"]), "to": _short(x["to"]),
                    **({"via": x["via"]} if x.get("via") else {}),
                    **({"tied": True} if x.get("tied") else {}),
                    "chain": x["chain"],
                } for x in ref]
            c["lat"] = []
            for k in range(n_paths):
                cells = [x[k] for x in lats if k < len(x)]
                t = _triple([x["cycles"] for x in cells if x["status"] == "ok"])
                if t is None:
                    c["lat"].append(None)
                elif any(x.get("roundtrip") for x in cells):
                    c["lat"].append(t + ["rt"])
                else:
                    c["lat"].append(t)
            item[core] = c
        if paths:
            item["paths"] = paths
        out["instructions"].append(item)

    for core in cores:
        ids: list[str] = []
        for r in runs:
            for e in r.get("structure", {}).get(core, []):
                if e["id"] not in ids:
                    ids.append(e["id"])
        merged_exps = []
        for eid in ids:
            seen_exps = [e for r in runs for e in r.get("structure", {}).get(core, [])
                         if e["id"] == eid]
            ok = [e for e in seen_exps if e["status"] == "ok" and e["value"] is not None]
            base = seen_exps[0]
            m: dict = {"id": eid, "title": base["title"], "unit": base["unit"]}
            if not conclusive(len(ok), len(seen_exps)):
                m["status"] = "inconclusive"
                m["value"] = None
                m["confidence"] = "low"
                m["note"] = (f"Conclusive in only {len(ok)} of {len(seen_exps)} runs. "
                             + base.get("note", ""))
                if ok:
                    m["seen"] = sorted(_round(e["value"]) for e in ok)
            else:
                values = [e["value"] for e in ok]
                med = statistics.median(values)
                pick = ok[pick_index(values)]
                m["status"] = "ok"
                m["value"] = _round(med)
                m["min"] = _round(min(values))
                m["max"] = _round(max(values))
                conf = min(CONFIDENCE.index(e.get("confidence", "low")) for e in ok)
                spread = (max(values) - min(values)) / abs(med) if med else 0.0
                if spread > 0.10:
                    conf = 0
                elif spread > 0.03:
                    conf = min(conf, 1)
                if len(ok) < len(seen_exps):
                    conf = min(conf, 1)
                m["confidence"] = CONFIDENCE[conf]
                m["runs_ok"] = len(ok)
                m["note"] = pick.get("note", "")
                if pick.get("curve"):
                    m["xlabel"] = pick.get("xlabel", "")
                    m["ylabel"] = pick.get("ylabel", "")
                    m["curve"] = sorted([_round(x), _round(y, 4)] for x, y in pick["curve"])
            merged_exps.append(m)
        if merged_exps:
            out["structure"][core] = merged_exps
    return out


def conclusive(n_ok: int, n_seen: int) -> bool:
    """A structure experiment counts if at least half of the runs that tried it got a value."""
    return n_ok > 0 and n_ok * 2 >= n_seen


def pick_index(values: list[float]) -> int:
    """The run whose value is closest to the median: its note and curve are published."""
    med = statistics.median(values)
    return min(range(len(values)), key=lambda i: abs(values[i] - med))


def _level(run: dict, label: str) -> dict:
    for lv in run["machine"]["levels"]:
        if lv["label"] == label:
            return lv
    return {}


def dump_results(data: dict) -> str:
    """Compact JSON with one instruction / experiment per line, so that a
    re-measurement produces a readable diff."""
    def one(obj) -> str:
        return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)

    lines = ["{"]
    scalar_keys = [k for k in data if k not in ("instructions", "structure")]
    for k in scalar_keys:
        lines.append(f" {json.dumps(k)}:{one(data[k])},")
    lines.append(' "instructions":[')
    for i, ins in enumerate(data["instructions"]):
        lines.append("  " + one(ins) + ("," if i + 1 < len(data["instructions"]) else ""))
    lines.append(" ],")
    lines.append(' "structure":{')
    cores = list(data["structure"])
    for ci, core in enumerate(cores):
        lines.append(f"  {json.dumps(core)}:[")
        exps = data["structure"][core]
        for i, e in enumerate(exps):
            lines.append("   " + one(e) + ("," if i + 1 < len(exps) else ""))
        lines.append("  ]" + ("," if ci + 1 < len(cores) else ""))
    lines.append(" }")
    lines.append("}")
    return "\n".join(lines) + "\n"


def load_results(path: Path) -> dict:
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as e:
        raise ResultError(f"{path}: {e}") from None
    if data.get("schema") != RESULT_SCHEMA:
        raise ResultError(f"{path}: not a merged results file (schema {data.get('schema')!r})")
    return data


# --------------------------------------------------------------------------
# CSV


def write_csv(data: dict, outdir: Path) -> list[Path]:
    outdir.mkdir(parents=True, exist_ok=True)
    ins_path = outdir / "instructions.csv"
    with ins_path.open("w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["name", "asm", "group", "extension", "core", "measure", "from", "to",
                    "value", "run_min", "run_max", "unit", "flags"])
        for ins in data["instructions"]:
            for core in data["cores"]:
                c = ins.get(core)
                if c is None:
                    continue
                common = [ins["name"], ins["asm"], ins["group"], ins.get("ext", ""), core]
                if c.get("unsupported"):
                    w.writerow(common + ["unsupported", "", "", "", "", "", "", ""])
                    continue
                if c.get("tp"):
                    flags = "limited-by-own-chain" if c.get("tp_chain_bound") else ""
                    w.writerow(common + ["throughput", "", ""] + c["tp"][:3] + ["per cycle", flags])
                for path, lat in zip(ins.get("paths", []), c.get("lat", [])):
                    if lat is None:
                        continue
                    flags = []
                    if len(lat) > 3:
                        flags.append("round-trip-via-" + path.get("via", "helper").replace(" ", ""))
                    if path.get("tied"):
                        flags.append("tied-with-destination")
                    w.writerow(common + ["latency", path["from"], path["to"]] + lat[:3]
                               + ["cycles", " ".join(flags)])
    st_path = outdir / "structure.csv"
    with st_path.open("w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["core", "id", "title", "status", "value", "run_min", "run_max", "unit",
                    "confidence"])
        for core in data["cores"]:
            for e in data["structure"].get(core, []):
                w.writerow([core, e["id"], e["title"], e["status"],
                            "" if e.get("value") is None else e["value"],
                            e.get("min", ""), e.get("max", ""), e["unit"], e["confidence"]])
    return [ins_path, st_path]


# --------------------------------------------------------------------------
# Anchors


def check(data: dict) -> list[str]:
    """Facts that must hold in any believable results file."""
    problems: list[str] = []
    by_name = {i["name"]: i for i in data["instructions"]}

    def lat(name: str, core: str, path: int = 0):
        ins = by_name.get(name)
        if not ins or core not in ins or not ins[core].get("lat"):
            return None
        cell = ins[core]["lat"][path]
        return cell[0] if cell else None

    for core in data["cores"]:
        for name in ("add_x_reg", "sub_x_reg", "eor_x_reg"):
            v = lat(name, core)
            if v is None:
                problems.append(f"{core}: {name} latency missing")
            elif abs(v - 1.0) > 0.03:
                problems.append(f"{core}: {name} latency is {v}, expected 1")
        rt = data["helpers"].get(core, {}).get("cmp_csinc_roundtrip")
        if not rt or abs(rt[0] - 2.0) > 0.05:
            problems.append(f"{core}: cmp+csinc round trip is {rt}, expected 2")
        for ins in data["instructions"]:
            c = ins.get(core, {})
            tp = c.get("tp")
            if tp and not (0 < tp[0] < 64):
                problems.append(f"{core}: {ins['name']} throughput {tp[0]} is not plausible")
            for cell in c.get("lat", []) or []:
                if cell and not (-0.2 < cell[0] < 400):
                    problems.append(f"{core}: {ins['name']} latency {cell[0]} is not plausible")
        problems += width_problems(data, core)
    return problems


# No instruction stream retires faster than NOPs.  On the M5 P-core no loop
# that was timed for this (immediate and register moves, NOPs, adds, address
# generation, and mixes of them) retired more than 10.03 instructions per
# cycle, the loop's own two included.  The slack covers the loop edge: the
# fastest state of the immediate moves measures 10.06, a register move 10.12
# in some runs (DESIGN.md, "Steady states").
WIDTH_SLACK = 1.06


def width_problems(data: dict, core: str) -> list[str]:
    """Single instructions faster than the pipeline width: a measurement artefact."""
    width = next((e for e in data["structure"].get(core, []) if e["id"] == "width"), None)
    if not width or width["status"] != "ok":
        return []
    out = []
    for ins in data["instructions"]:
        tp = ins.get(core, {}).get("tp")
        # One instance may be several instructions; only single ones are bounded.
        if tp and ";" not in ins["asm"] and tp[0] > width["value"] * WIDTH_SLACK:
            out.append(f"{core}: {ins['name']} throughput {tp[0]} exceeds the pipeline width "
                       f"{width['value']}; nothing retires faster than NOPs, so runs in different "
                       "steady states were combined (tool 0.3.0 measures one state)")
    return out


# --------------------------------------------------------------------------
# Site


def build_site(roots: list[Path], outdir: Path, reference_dir: Path | None) -> list[Path]:
    """site/data.js and site/data/<chip>.js; see uarch_site.py."""
    import uarch_site  # imports this module, so not at the top

    return uarch_site.build(roots, outdir, reference_dir)


# --------------------------------------------------------------------------


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="merge, export and publish uarch results")
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("merge", help="merge raw runs into one results file")
    m.add_argument("runs", nargs="+", type=Path)
    m.add_argument("-o", "--output", type=Path, required=True)
    c = sub.add_parser("csv", help="write instructions.csv and structure.csv")
    c.add_argument("results", type=Path)
    c.add_argument("-o", "--outdir", type=Path, required=True)
    s = sub.add_parser("site", help="write the site's data files from results/")
    s.add_argument("results", nargs="+", type=Path)
    s.add_argument("-o", "--outdir", type=Path, required=True)
    s.add_argument("--reference", type=Path, default=None)
    k = sub.add_parser("check", help="validate the anchors of a results file")
    k.add_argument("results", type=Path)
    args = ap.parse_args(argv)

    try:
        if args.cmd == "merge":
            data = merge([load_raw(p) for p in args.runs])
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(dump_results(data))
            print(f"merged {len(args.runs)} runs: {len(data['instructions'])} instructions, "
                  f"cores {', '.join(data['cores'])} -> {args.output}")
        elif args.cmd == "csv":
            for p in write_csv(load_results(args.results), args.outdir):
                print(f"wrote {p}")
        elif args.cmd == "site":
            for p in build_site(args.results, args.outdir, args.reference):
                print(f"wrote {p}")
        elif args.cmd == "check":
            problems = check(load_results(args.results))
            for line in problems:
                print("FAIL " + line)
            if problems:
                return 1
            print("anchors hold")
    except ResultError as e:
        print(f"uarch_results: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
