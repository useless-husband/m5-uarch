#!/usr/bin/env python3
"""Build the results site's data from every dataset in results/.

    uarch_site.py results -o site [--reference reference]
    (also reached as `uarch_results.py site ...` and `make site`)

Writes site/data.js (chip list, datasets, structure figures; loaded at once)
and site/data/<chip>.js (the instruction table of one chip; loaded when the
chip is shown).  Several datasets of one chip are combined value by value:
each dataset is compared with the others by the outlier rule of
uarch_validate.py (against the median of all of them once there are three; with two, against
each other), values marked there are left out of the
chip's figure, and the figure is the median of the remaining datasets with
their smallest and largest value as the spread.  With one dataset the spread
is its run-to-run range.  Only the standard library is used.
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import uarch_results as ur  # noqa: E402
import uarch_submit as us  # noqa: E402
import uarch_validate as uv  # noqa: E402

SITE_SCHEMA = "m5-uarch-site/2"
F_RT, F_MARK, F_CHAIN = 1, 2, 4
SOURCE_ORDER = {"commit": 0, "issue": 1, "pr": 2, "local": 3}


def _json(payload) -> str:
    body = json.dumps(payload, separators=(",", ":"), ensure_ascii=False)
    # "</" cannot end a script element early if it never appears.
    return body.replace("</", "<\\/")


def find_datasets(roots: list[Path]) -> list[tuple[Path, dict, bool]]:
    """(path, results, local?) for every results file under the given
    directories (results/ and, if present, results/local/)."""
    found = []
    for root in roots:
        files = [root] if root.is_file() else sorted(root.glob("*/*.json"))
        if root.is_dir() and (root / "local").is_dir():
            files += sorted((root / "local").glob("*/*.json"))
        for p in files:
            if p.name.endswith(".samples.json"):
                continue
            local = "local" in p.parts[-3:-1]
            found.append((p, ur.load_results(p), local))
    return found


def _sort_key(item) -> tuple:
    path, res, local = item
    src = res.get("source") or {"kind": "local"}
    kind = "local" if local else src.get("kind", "pr")
    return (SOURCE_ORDER.get(kind, 2), src.get("number", 0), path.stem)


def _cell(triples: list, flagged: list[bool], extra_flags: int = 0) -> list | None:
    """Combine one value over datasets -> [value, lo, hi, n, flags]."""
    have = [(t, f) for t, f in zip(triples, flagged) if t]
    if not have:
        return None
    use = [t for t, f in have if not f] or [t for t, _ in have]
    flags = extra_flags | (F_MARK if any(f for _, f in have) else 0)
    if any(len(t) > 3 for t, _ in have):
        flags |= F_RT
    if len(use) == 1:
        t = use[0]
        return [t[0], t[1], t[2], 1, flags]
    meds = [t[0] for t in use]
    return [ur._round(statistics.median(meds)), min(meds), max(meds), len(use), flags]


def _marked(o, others: list) -> dict:
    """A marked value, described against the other datasets only."""
    meds = [v[o.key].med for v in others if o.key in v] or [o.center]
    return {"label": o.label, "value": o.value, "center": ur._round(statistics.median(meds)),
            "lo": min(meds), "hi": max(meds)}


def build_chip(items: list[tuple[Path, dict, bool]], root: Path) -> tuple[dict, dict]:
    items = sorted(items, key=_sort_key)
    published = [i for i in items if not i[2]]
    vals = [uv.values(res) for _, res, _ in items]
    pub_idx = [k for k, i in enumerate(items) if not i[2]] or list(range(len(items)))
    flagged_keys: list[set] = []
    datasets = []
    for k, (path, res, local) in enumerate(items):
        # With three or more datasets each one is compared with the median of
        # all of them, which one outlier cannot drag; with two, with the other.
        if k in pub_idx and len(pub_idx) >= 3:
            others = [vals[j] for j in pub_idx]
        else:
            others = [vals[j] for j in pub_idx if j != k]
        found, compared = uv.outliers(vals[k], others)
        keys = {o.key for o in found}
        flagged_keys.append(keys)
        share = len(found) / compared if compared else 0.0
        src = res.get("source") or {}
        loads = [r["load_average"][0] for r in res["runs"]]
        try:
            rel = path.resolve().relative_to(root.resolve().parent).as_posix()
        except ValueError:
            rel = path.name
        datasets.append({
            "id": path.stem, "file": rel, "local": local,
            "model": res["machine"]["model"],
            "os": f"{res['machine']['os_version']} ({res['machine']['os_build']})",
            "tool": res.get("tool_version", ""), "runs": len(res["runs"]),
            "load": [min(loads), max(loads)],
            "ghz": {lv["label"]: lv.get("ghz_observed") for lv in res["machine"]["levels"]
                    if lv.get("ghz_observed")},
            "source": {"kind": "local"} if local else src,
            "status": "local" if local else ("flagged" if share > uv.FLAG_SHARE else "accepted"),
            "compared": compared, "marked": len(found),
            "marked_list": [_marked(o, [vals[j] for j in pub_idx if j != k]) for o in found[:40]],
        })
    # Only published datasets make the chip's figures, unless there are none.
    use = pub_idx
    accepted = [d for d in datasets if d["status"] == "accepted"]
    if not published:
        status = "local"
    elif len(published) == 1:
        status = "single"
    elif len(accepted) >= 2:
        status = "verified"
    else:
        status = "flagged"
    first = items[use[0]][1]
    cores: list[str] = []
    for j in use:
        for c in items[j][1]["cores"]:
            if c not in cores:
                cores.append(c)

    # Structure: per core, per experiment.
    structure: dict = {}
    for c in cores:
        ids: list[str] = []
        for j in use:
            for e in items[j][1]["structure"].get(c, []):
                if e["id"] not in ids:
                    ids.append(e["id"])
        rows = []
        for eid in ids:
            per = []
            exps = []
            for j in use:
                e = next((x for x in items[j][1]["structure"].get(c, []) if x["id"] == eid), None)
                exps.append(e)
                ok = e is not None and e["status"] == "ok" and e["value"] is not None
                per.append([j, e["value"] if ok else None, int(f"{c}|st|{eid}" in flagged_keys[j]),
                            e["confidence"] if e else None])
            base = next(e for e in exps if e)
            trip = [[e["value"], e.get("min", e["value"]), e.get("max", e["value"])]
                    if e is not None and e["status"] == "ok" and e["value"] is not None else None
                    for e in exps]
            cell = _cell(trip, [bool(p[2]) for p in per])
            row = {"id": eid, "title": base["title"], "unit": base["unit"], "per": per}
            if cell is None:
                row.update(status=base["status"], note=base.get("note", ""))
            else:
                # The dataset closest to the combined value supplies note and curve.
                cand = [e for e, t in zip(exps, trip) if t]
                rep = min(cand, key=lambda e: abs(e["value"] - cell[0]))
                confs = [e["confidence"] for e, t, p in zip(exps, trip, per) if t and not p[2]] or \
                        [e["confidence"] for e in cand]
                row.update(status="ok", cell=cell, note=rep.get("note", ""),
                           confidence=min(confs, key=ur.CONFIDENCE.index))
                for k in ("xlabel", "ylabel", "curve"):
                    if k in rep:
                        row[k] = rep[k]
            rows.append(row)
        structure[c] = rows

    # Instructions.
    order: list[str] = []
    by_name: list[dict] = []
    for j in use:
        idx = {i["name"]: i for i in items[j][1]["instructions"]}
        by_name.append(idx)
        order += [n for n in idx if n not in order]
    insns = []
    for name in order:
        entries = [idx.get(name) for idx in by_name]
        base = next(e for e in entries if e)
        out = {"name": name, "group": base["group"], "ext": base.get("ext", ""), "asm": base["asm"]}
        if base.get("note"):
            out["note"] = base["note"]
        paths = next((e["paths"] for e in entries if e and e.get("paths")), None)
        if paths:
            out["paths"] = paths
        for c in cores:
            per = [(j, e.get(c)) for j, e in zip(use, entries) if e and e.get(c)]
            if not per:
                continue
            if all(x.get("unsupported") for _, x in per):
                out[c] = {"unsupported": True, "signal": per[0][1].get("signal")}
                continue
            per = [(j, x) for j, x in per if not x.get("unsupported")]
            cc: dict = {}
            chain = F_CHAIN if any(x.get("tp_chain_bound") for _, x in per) else 0
            tp = _cell([x.get("tp") for _, x in per],
                       [f"{c}|tp|{name}" in flagged_keys[j] for j, _ in per], chain)
            if tp:
                cc["tp"] = tp
            elif per[0][1].get("tp_status"):
                cc["tp_status"] = per[0][1]["tp_status"]
            n_lat = max(len(x.get("lat") or []) for _, x in per)
            lats = [x.get("lat") or [] for _, x in per]
            cc["lat"] = [_cell([lat[k] if k < len(lat) else None for lat in lats],
                               [f"{c}|lat|{name}|{k}" in flagged_keys[j] for j, _ in per])
                         for k in range(n_lat)]
            out[c] = cc
        insns.append(out)

    m = first["machine"]
    slug = us.chip_slug(m["brand"])
    chip = {
        "slug": slug, "brand": m["brand"], "status": status,
        "published": len(published),
        "models": sorted({d["model"] for d in datasets}),
        "cores": cores,
        "levels": [{k: lv[k] for k in ("label", "name", "cores", "l1d_bytes", "l2_bytes")
                    if k in lv} for lv in m["levels"]],
        "pauth_inactive": m.get("pauth_keys_active") is False,
        "datasets": datasets,
        "structure": structure,
        "insns_file": f"data/{slug}.js",
        "n_insns": len(insns),
    }
    return chip, {"slug": slug, "instructions": insns}


def build(roots: list[Path], outdir: Path, reference_dir: Path | None) -> list[Path]:
    items = find_datasets(roots)
    groups: dict[str, list] = {}
    for it in items:
        groups.setdefault(us.chip_slug(it[1]["machine"]["brand"]), []).append(it)
    refs = []
    if reference_dir and reference_dir.is_dir():
        refs = [json.loads(p.read_text()) for p in sorted(reference_dir.glob("*.json"))]
    root = roots[0] if roots and roots[0].is_dir() else Path("results")
    chips, tables = [], []
    for slug in sorted(groups):
        chip, table = build_chip(groups[slug], root)
        chips.append(chip)
        tables.append(table)
    outdir.mkdir(parents=True, exist_ok=True)
    datadir = outdir / "data"
    datadir.mkdir(exist_ok=True)
    for old in datadir.glob("*.js"):
        old.unlink()
    written = []
    index = {"schema": SITE_SCHEMA, "repo": us.REPO, "chips": chips, "references": refs,
             "flag_share": uv.FLAG_SHARE}
    p = outdir / "data.js"
    p.write_text("window.UARCH_INDEX=" + _json(index) + ";\n")
    written.append(p)
    for t in tables:
        q = datadir / f"{t['slug']}.js"
        q.write_text("window.UARCH_CHIP(" + _json(t) + ");\n")
        written.append(q)
    return written


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="build the results site's data files")
    ap.add_argument("results", nargs="+", type=Path)
    ap.add_argument("-o", "--outdir", type=Path, required=True)
    ap.add_argument("--reference", type=Path, default=None)
    args = ap.parse_args(argv)
    try:
        for p in build(args.results, args.outdir, args.reference):
            print(f"wrote {p}")
    except ur.ResultError as e:
        print(f"uarch_site: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
