"""Synthetic raw runs for the submission, validation and site tests.

A small instruction table is expanded by the real generator, so the runs
carry exactly the metadata the C tool would write for it; values come from a
function the test controls.  Randomness uses a fixed seed.
"""

from __future__ import annotations

import copy
import json
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import gen_insns as g  # noqa: E402
import uarch_results as ur  # noqa: E402
import uarch_submit as us  # noqa: E402

SEED = 20261002

DEFS = """\
group Integer
add_x_reg | add {W0:x}, {R0:x}, {R1:x}
sub_x_reg | sub {W0:x}, {R0:x}, {R1:x}
eor_x_reg | eor {W0:x}, {R0:x}, {R1:x}
add_x_imm | add {W0:x}, {R0:x}, #1
mul_x | mul {W0:x}, {R0:x}, {R1:x}
cmp_x | cmp {R0:x}, {R1:x} | flags=w
group Vector
fmla_2d | fmla {RW0:v}.2d, {R1:v}.2d, {R2:v}.2d
ext SVE
sve_add | add {W0:z}.d, {R0:z}.d, {R1:z}.d
ext -
group Other
nop | nop | nolat
mov_x_imm48 | mov {W0:x}, #0x123400000000
"""


def spec() -> us.Spec:
    return us.spec_from_insns([g.expand(s) for s in g.parse_defs(DEFS, "fixture")])


# Per core: (latency per instruction, throughput per instruction)
LAT = {"add_x_reg": 1, "sub_x_reg": 1, "eor_x_reg": 1, "add_x_imm": 1, "mul_x": 3, "cmp_x": 1,
       "fmla_2d": 4}
# mov_x_imm48: the fastest steady state of an immediate move on the M5 P-core
# measures 10.065 per cycle against a NOP rate of 10 (the loop edge).
TP = {"P": {"add_x_reg": 7.0, "sub_x_reg": 7.0, "eor_x_reg": 7.0, "add_x_imm": 7.7, "mul_x": 3,
            "cmp_x": 3.9, "fmla_2d": 4, "nop": 10, "mov_x_imm48": 10.065},
      "E": {"add_x_reg": 4, "sub_x_reg": 4, "eor_x_reg": 4, "add_x_imm": 4, "mul_x": 1,
            "cmp_x": 3.9, "fmla_2d": 2, "nop": 6, "mov_x_imm48": 6}}
STRUCT = {"P": {"width": 10, "units_alu": 7.9, "rob_nop": 3367, "l1_latency": 3, "l1d_size": 128},
          "E": {"width": 6, "units_alu": 4, "rob_nop": 1072, "l1_latency": 3, "l1d_size": 64}}


def _r(v: float) -> float:
    r = round(v, 3)
    return int(r) if r == int(r) else r


def make_run(sp: us.Spec, i: int = 0, brand: str = "Apple M5", noise: float = 0.002,
             value=None, seed: int = SEED, unsupported: tuple = ("sve_add",)) -> dict:
    """One raw run.  value(kind, core, name, default) may override any number."""
    rng = random.Random(f"{seed}-{i}")
    value = value or (lambda kind, core, name, v: v)

    def jitter(v: float) -> float:
        return _r(v * (1 + rng.uniform(-noise, noise)))

    insns = []
    for e in sp.entries:
        ins = {"name": e["name"], "group": e["group"], "ext": e["ext"], "asm": e["asm"]}
        if e["note"]:
            ins["note"] = e["note"]
        for c in ("P", "E"):
            if e["name"] in unsupported:
                ins[c] = {"supported": False, "signal": 4}
                continue
            x: dict = {}
            if e["tp"]:
                v = value("tp", c, e["name"], jitter(TP[c][e["name"]]))
                x["tp"] = {"status": "ok", "per_cycle": v, "cycles": _r(1 / v), "spread": 0.001}
                if e["name"] == "fmla_2d":
                    x["tp"]["chain_bound"] = True
            x["lat"] = []
            for k, p in enumerate(e["paths"]):
                v = value("lat", c, e["name"], jitter(LAT[e["name"]]) if LAT[e["name"]] > 1
                          else LAT[e["name"]])
                lat = {"from": p["from"], "to": p["to"], "status": "ok", "cycles": v,
                       "spread": 0.001}
                if "via" in p:
                    lat["via"] = p["via"]
                if p.get("tied"):
                    lat["tied"] = True
                lat["chain"] = p["chain"]
                x["lat"].append(lat)
            ins[c] = x
        insns.append(ins)
    structure = {}
    for c in ("P", "E"):
        exps = []
        for eid, v in STRUCT[c].items():
            v = value("st", c, eid, jitter(v) if eid not in ("width", "l1_latency") else v)
            e = {"id": eid, "title": eid.replace("_", " "), "unit": "per cycle", "status": "ok",
                 "value": v, "lo": v, "hi": v, "confidence": "high", "note": f"{eid} note"}
            if eid == "rob_nop":
                e.update(unit="entries", xlabel="fillers", ylabel="time ratio",
                         curve=[[256, 1.0], [512, 1.01], [4096, 1.7]])
            exps.append(e)
        structure[c] = exps
    return {
        "schema": ur.RAW_SCHEMA, "tool_version": "0.2.0", "started": f"2026-10-02T00:0{i}:00Z",
        "seconds": 10.0 + i,
        "machine": {"brand": brand, "model": "Mac17,2", "os_version": "27.0",
                    "os_build": "26A428", "page_size": 16384, "memory_bytes": 17179869184,
                    "virtual_machine": False, "pauth_keys_active": False,
                    "levels": [
                        {"label": "P", "name": "Super", "cores": 4, "l1i_bytes": 196608,
                         "l1d_bytes": 131072, "l2_bytes": 16777216, "cores_per_l2": 4,
                         "measured": True, "ghz_observed": 4.5},
                        {"label": "E", "name": "Efficiency", "cores": 6, "l1i_bytes": 131072,
                         "l1d_bytes": 65536, "l2_bytes": 6291456, "cores_per_l2": 6,
                         "measured": True, "ghz_observed": 2.6}]},
        "run": {"counters": "thread_selfcounts", "runs": 1000, "clean": 900,
                "discarded_migrated": 1, "discarded_disturbed": 60,
                "load_average": [1.5, 1.6, 1.7]},
        "helpers": {c: {"cmp_csinc_roundtrip": 2, "fmov_roundtrip": 10 if c == "P" else 8,
                        "fcmp_fcsel_roundtrip": 4} for c in ("P", "E")},
        "instructions": insns,
        "structure": structure,
    }


def make_runs(sp: us.Spec, n: int = 3, **kw) -> list[dict]:
    return [make_run(sp, i, **kw) for i in range(n)]


def packed(sp: us.Spec, n: int = 3, **kw) -> dict:
    return us.pack(make_runs(sp, n, **kw), sp)


def committed_m5() -> tuple[dict, str]:
    """The committed M5 samples and its results text."""
    d = ROOT / "results" / "apple-m5"
    return (json.loads((d / "apple-m5.samples.json").read_text()),
            (d / "apple-m5.json").read_text())


def clone(x):
    return copy.deepcopy(x)
