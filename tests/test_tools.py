#!/usr/bin/env python3
"""Tests for tools/gen_insns.py and tools/uarch_results.py (standard library only)."""

import json
import random
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import gen_insns as g  # noqa: E402
import uarch_results as u  # noqa: E402

SEED = 20260930


def expand(text: str):
    return [g.expand(s) for s in g.parse_defs(text, "test")]


def regs_in(line: str, prefix: str) -> list[int]:
    return [int(m) for m in re.findall(rf"\b{prefix}(\d+)\b", line)]


class ParseTests(unittest.TestCase):
    def test_loops_and_groups(self):
        specs = g.parse_defs(
            "# comment\n"
            "group G1\n"
            "ext LSE\n"
            "for s,n in x:64 w:32\n"
            "  for k in 1 2\n"
            "add_$s_$k | add {W0:$s}, {R0:$s}, #$k | note=\"$n bit\"\n"
            "  end\n"
            "end\n"
            "ext -\n"
            "nop | nop | nolat\n")
        self.assertEqual([s.name for s in specs], ["add_x_1", "add_x_2", "add_w_1", "add_w_2", "nop"])
        self.assertEqual(specs[0].group, "G1")
        self.assertEqual(specs[0].ext, "LSE")
        self.assertEqual(specs[-1].ext, "")
        self.assertEqual(specs[2].attrs["note"], "32 bit")

    def test_hash_is_an_immediate_not_a_comment(self):
        (spec,) = g.parse_defs("a | add {W0:x}, {R0:x}, #1")
        self.assertEqual(spec.alts, [["add {W0:x}, {R0:x}, #1"]])

    def test_alternatives_and_sequences(self):
        (spec,) = g.parse_defs("p | ldr {W0:x}, [{RW1:x}], #8 || ldr {W0:x}, [{RW1:x}], #-8 | ptr=RW1")
        self.assertEqual(len(spec.alts), 2)
        (spec,) = g.parse_defs("s | str {R0:x}, [{A0}] ; ldr {W0:x}, [{A0}]")
        self.assertEqual(len(spec.alts[0]), 2)

    def test_errors(self):
        bad = [
            "a | add {W0:x}, {R0:x}\na | add {W0:x}, {R0:x}",   # duplicate name
            "a | add {W0}, {R0:x}",                              # missing class
            "a | add {W0:k}, {R0:x}",                            # unknown class
            "a | add {W0:x}, {R0:x} | bogus=1",                  # unknown attribute
            "a | add {W0:x}, {R0:x} | flags=q",                  # bad flags
            "a | add {W0:x}, {Q0:x}",                            # unknown placeholder
            "for s in x\na | add {W0:$s}, {R0:$s}",              # for without end
            "end",                                               # end without for
            "a b | add {W0:x}, {R0:x}",                          # bad name
            "a | add {W0:x}, {R0:v}.16b, {R0:x}",                # operand in two families
            "a",                                                 # no template
        ]
        for text in bad:
            with self.assertRaises(g.DefError, msg=text):
                expand(text)

    def test_shipped_definitions_load(self):
        insns = g.load(sorted((ROOT / "insns").glob("*.def")))
        self.assertGreater(len(insns), 500)
        names = [i.spec.name for i in insns]
        self.assertEqual(len(names), len(set(names)))
        for need in ("add_x_reg", "rt_cmp_csinc", "rt_fmov_x_d", "rt_fcmp_fcsel", "pacia"):
            self.assertIn(need, names)


class ExpandTests(unittest.TestCase):
    def test_throughput_block_is_independent(self):
        (i,) = expand("a | add {W0:x}, {R0:x}, {R1:x}")
        self.assertEqual(i.tp_inst, 18)
        dests = [regs_in(line, "x")[0] for line in i.tp_lines]
        self.assertEqual(len(set(dests)), 18)
        for line in i.tp_lines:
            d, a, b = regs_in(line, "x")
            self.assertIn(d, g.GPR_POOL)
            self.assertIn(a, g.GPR_ROT)
            self.assertIn(b, g.GPR_ROT)
            self.assertNotEqual(a, b)          # two sources never coincide
            self.assertNotIn(d, (a, b))        # and never alias the destination
        # Sources rotate: no single register is read by every instance.
        self.assertGreater(len({regs_in(line, "x")[1] for line in i.tp_lines}), 4)

    def test_reserved_registers_never_appear(self):
        rng = random.Random(SEED)
        insns = g.load(sorted((ROOT / "insns").glob("*.def")))
        for ins in rng.sample(insns, 200):
            lines = list(ins.tp_lines)
            for ch in ins.chains:
                lines += ch.lines
            for line in lines:
                if ins.spec.name in ("pacia1716", "xpaclri", "bl_ret", "blr_ret", "casp_x_same"):
                    continue
                used = set(regs_in(line, "x")) | set(regs_in(line, "w"))
                self.assertFalse(used & {18, 28, 29, 30}, f"{ins.spec.name}: {line}")

    def test_latency_chains(self):
        (i,) = expand("a | add {W0:x}, {R0:x}, {R1:x}")
        self.assertEqual([(c.src, c.dst) for c in i.chains],
                         [("R0:x0", "W0:x0"), ("R1:x0", "W0:x0")])
        self.assertEqual(i.chains[0].lines, ["add x0, x0, x20"])
        self.assertEqual(i.chains[1].lines, ["add x0, x19, x0"])
        self.assertEqual(i.chains[0].helper, g.HELP_NONE)

    def test_flag_chains_use_helpers(self):
        (i,) = expand("c | cmp {R0:x}, {R1:x} | flags=w")
        self.assertEqual(i.chains[0].helper, g.HELP_CSINC)
        self.assertEqual(i.chains[0].lines, ["cmp x0, x20", "csinc x0, x23, x24, ne"])
        (i,) = expand("c | adc {W0:x}, {R0:x}, {R1:x} | flags=r")
        flag_chain = [c for c in i.chains if c.src.startswith("nzcv")][0]
        self.assertEqual(flag_chain.helper, g.HELP_CMP)
        self.assertEqual(flag_chain.lines[-1], "cmp x0, x23")
        (i,) = expand("c | adcs {W0:x}, {R0:x}, {R1:x} | flags=rw")
        self.assertTrue(i.serial)
        direct = [c for c in i.chains if c.src.startswith("nzcv") and c.dst.startswith("nzcv")]
        self.assertEqual(direct[0].helper, g.HELP_NONE)

    def test_cross_domain_chains(self):
        (i,) = expand("c | scvtf {W0:d}, {R0:x}")
        self.assertEqual(i.chains[0].helper, g.HELP_FMOV_XD)
        self.assertEqual(i.chains[0].lines, ["scvtf d0, x0", "fmov x0, d0"])
        (i,) = expand("c | fcvtzs {W0:x}, {R0:d}")
        self.assertEqual(i.chains[0].helper, g.HELP_FMOV_DX)
        (i,) = expand("c | fcmp {R0:d}, {R1:d} | flags=w")
        self.assertEqual(i.chains[0].helper, g.HELP_FCSEL)

    def test_read_write_operands(self):
        (i,) = expand("m | fmla {RW0:v}.4s, {R0:v}.4s, {R1:v}.4s | fp=s")
        self.assertEqual(i.tp_chains, len(g.VEC_POOL))
        self.assertEqual([c.tied for c in i.chains], [False, True, True])
        self.assertEqual(i.chains[1].lines, ["fmla v0.4s, v0.4s, v13.4s"])
        # Vector sources stay below v16 (needed by the 16-bit indexed forms).
        for line in i.tp_lines:
            self.assertTrue(all(r < 16 for r in regs_in(line, "v")[1:]))

    def test_memory_chains_need_a_cell(self):
        (i,) = expand("l | ldr {W0:x}, [{A0}, {R1:x}] | init=R1:16")
        self.assertEqual(i.chains, [])          # no cell: no chain through the address
        (i,) = expand("l | ldr {W0:x}, [{A0}, {R1:x}] | init=R1:16 cell=A0:16,R1:0:1")
        self.assertEqual(len(i.chains), 2)
        kinds = [[x.kind for x in c.inits] for c in i.chains]
        self.assertIn("CYCLE_PTR", kinds[0])
        self.assertIn("CYCLE_IDX", kinds[1])
        # Throughput block uses the scratch base register.
        self.assertTrue(all("[x27, x20]" in line for line in i.tp_lines))

    def test_writeback_base_is_never_the_data_register(self):
        (i,) = expand("s | str {R0:x}, [{RW1:x}, #8]! || str {R0:x}, [{RW1:x}, #-8]! | ptr=RW1")
        for ch in i.chains:
            for line in ch.lines:
                data, base = regs_in(line, "x")[:2]
                self.assertNotEqual(data, base)
        self.assertEqual(i.tp_inst, 2 * len(g.GPR_POOL))
        self.assertEqual(i.chains[0].steps, 2)

    def test_literal_braces_and_offsets(self):
        (i,) = expand("t | tbl {W0:v}.16b, {{ {R0:v}.16b }}, {R1:v}.16b | fp=i")
        self.assertEqual(i.tp_lines[0], "tbl v0.16b, { v12.16b }, v13.16b")
        (i,) = expand("s | str {R0:x}, [{A0}, #{off:8:16}] | nolat")
        self.assertIn("#16]", i.tp_lines[0])
        self.assertIn("#24]", i.tp_lines[1])

    def test_rotating_pointers_use_distinct_cache_lines(self):
        (i,) = expand("a | ldadd {R0:x}, {W0:x}, [{R1:x}] | ptr=R1 nolat")
        ptrs = [x for x in i.tp_inits if x.kind == "GPR_PTR"]
        self.assertEqual(sorted(x.reg for x in ptrs), g.GPR_ROT)
        self.assertEqual(sorted(x.val for x in ptrs), [64 * k for k in range(8)])

    def test_emit_counts_words(self):
        insns = expand("a | add {W0:x}, {R0:x}, {R1:x}\nb | cbz {R0:x}, #4 | nolat")
        asm, c = g.emit(insns)
        marks = asm.count(".long 0x0bad")
        self.assertEqual(marks, 4)             # tp + 2 chains, then tp
        total = int(re.search(r"ua_code_words = (\d+);", c).group(1))
        n_instr = sum(1 for line in asm.splitlines()
                      if line.startswith("    ") and not line.strip().startswith((".", "//")))
        self.assertEqual(total, n_instr + marks)
        self.assertEqual(int(re.search(r"ua_n_code_marks = (\d+);", c).group(1)), marks)


def raw_run(add_lat=1.0, tp=4.0, rob=600.0, rob_status="ok", brand="Apple Test", width=8.0,
            asm="add x0, x19, x20"):
    ins = {
        "name": "add_x_reg", "group": "Integer", "ext": "", "asm": asm,
        "P": {"tp": {"status": "ok", "per_cycle": tp, "cycles": 1 / tp, "spread": 0.001},
              "lat": [{"from": "R0:x0", "to": "W0:x0", "status": "ok", "cycles": add_lat,
                       "spread": 0.0, "chain": "add x0, x0, x20"}]},
    }
    others = [{
        "name": n, "group": "Integer", "ext": "", "asm": n,
        "P": {"lat": [{"from": "R0:x0", "to": "W0:x0", "status": "ok", "cycles": 1.0,
                       "spread": 0.0, "chain": n}]},
    } for n in ("sub_x_reg", "eor_x_reg")]
    return {
        "schema": u.RAW_SCHEMA, "tool_version": "0", "started": "t", "seconds": 1.0,
        "machine": {"brand": brand, "model": "X", "os_version": "1", "os_build": "b",
                    "page_size": 16384, "memory_bytes": 1, "virtual_machine": False,
                    "levels": [{"label": "P", "name": "Performance", "cores": 4, "l1i_bytes": 1,
                                "l1d_bytes": 1, "l2_bytes": 1, "cores_per_l2": 4,
                                "measured": True, "ghz_observed": 3.2}]},
        "run": {"counters": "c", "runs": 10, "clean": 9, "discarded_migrated": 0,
                "discarded_disturbed": 1, "load_average": [1.0, 1.0, 1.0]},
        "helpers": {"P": {"cmp_csinc_roundtrip": 2.0, "fmov_roundtrip": 8.0,
                          "fcmp_fcsel_roundtrip": 4.0}},
        "instructions": [ins] + others,
        "structure": {"P": [
            {"id": "width", "title": "Width", "unit": "per cycle", "status": "ok", "value": width,
             "lo": width, "hi": width, "confidence": "high", "note": "n"},
            {"id": "rob", "title": "ROB", "unit": "entries", "status": rob_status,
             "value": rob if rob_status == "ok" else None, "lo": rob, "hi": rob,
             "confidence": "high", "note": "n", "xlabel": "x", "ylabel": "y",
             "curve": [[2, 1.0], [1, 1.0], [3, 2.0]]},
        ]},
    }


class ResultsTests(unittest.TestCase):
    def test_merge_takes_median_and_range(self):
        m = u.merge([raw_run(tp=4.0, rob=600), raw_run(tp=5.0, rob=610), raw_run(tp=4.2, rob=603)])
        add = m["instructions"][0]
        self.assertEqual(add["P"]["tp"], [4.2, 4.0, 5.0])
        self.assertEqual(add["P"]["lat"], [[1.0, 1.0, 1.0]])
        self.assertEqual(add["paths"], [{"from": "R0", "to": "W0", "chain": "add x0, x0, x20"}])
        rob = [e for e in m["structure"]["P"] if e["id"] == "rob"][0]
        self.assertEqual((rob["value"], rob["min"], rob["max"]), (603, 600, 610))
        self.assertEqual(rob["curve"], [[1, 1.0], [2, 1.0], [3, 2.0]])   # sorted by x
        self.assertEqual(len(m["runs"]), 3)
        self.assertEqual(m["runs"][0]["ghz_observed"], {"P": 3.2})  # per run, not only median
        self.assertEqual(m["cores"], ["P"])

    def test_merge_keeps_each_runs_clock_and_leaves_inputs_alone(self):
        # Regression: merge() used to overwrite the first run's level record
        # with the median clock, so that run's own clock was lost.
        runs = [raw_run(), raw_run(), raw_run()]
        for r, ghz in zip(runs, (3.0, 2.0, 2.5)):
            r["machine"]["levels"][0]["ghz_observed"] = ghz
        before = json.dumps(runs, sort_keys=True)
        m = u.merge(runs)
        self.assertEqual([r["ghz_observed"] for r in m["runs"]], [{"P": 3.0}, {"P": 2.0}, {"P": 2.5}])
        self.assertEqual(m["machine"]["levels"][0]["ghz_observed"], 2.5)
        self.assertEqual(json.dumps(runs, sort_keys=True), before)

    def test_merge_property_median_within_range(self):
        rng = random.Random(SEED)
        for _ in range(50):
            vals = [round(rng.uniform(0.5, 9), 3) for _ in range(rng.randint(1, 7))]
            m = u.merge([raw_run(tp=v) for v in vals])
            med, lo, hi = m["instructions"][0]["P"]["tp"]
            self.assertEqual((lo, hi), (min(vals), max(vals)))
            self.assertTrue(lo <= med <= hi)

    def test_merge_downgrades_unstable_results(self):
        m = u.merge([raw_run(rob=600), raw_run(rob=700), raw_run(rob=605)])
        rob = [e for e in m["structure"]["P"] if e["id"] == "rob"][0]
        self.assertEqual(rob["confidence"], "low")
        m = u.merge([raw_run(rob_status="inconclusive"), raw_run(rob_status="inconclusive"),
                     raw_run(rob=600)])
        rob = [e for e in m["structure"]["P"] if e["id"] == "rob"][0]
        self.assertEqual(rob["status"], "inconclusive")
        self.assertIsNone(rob["value"])

    def test_merge_rejects_mixed_chips_and_wrong_files(self):
        with self.assertRaises(u.ResultError):
            u.merge([raw_run(), raw_run(brand="Other")])
        with self.assertRaises(u.ResultError):
            u.merge([])
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "x.json"
            p.write_text("{}")
            with self.assertRaises(u.ResultError):
                u.load_raw(p)
            p.write_text("not json")
            with self.assertRaises(u.ResultError):
                u.load_results(p)

    def test_dump_roundtrip_csv_site_and_check(self):
        m = u.merge([raw_run(), raw_run(tp=4.1)])
        text = u.dump_results(m)
        self.assertEqual(json.loads(text), m)
        self.assertEqual(u.check(m), [])
        with tempfile.TemporaryDirectory() as d:
            out = Path(d)
            (out / "r.json").write_text(text)
            paths = u.write_csv(u.load_results(out / "r.json"), out)
            rows = paths[0].read_text().splitlines()
            self.assertEqual(rows[0].split(",")[:5], ["name", "asm", "group", "extension", "core"])
            self.assertTrue(any("throughput" in r for r in rows))
            self.assertTrue(any(",latency,R0,W0,1.0," in r for r in rows))
            chipdir = out / "results" / "apple-test"
            chipdir.mkdir(parents=True)
            (chipdir / "r.json").write_text(text)
            written = u.build_site([out / "results"], out / "site", None)
            js = written[0].read_text()
            self.assertTrue(js.startswith("window.UARCH_INDEX="))
            self.assertNotIn("</", js)
            payload = json.loads(js[len("window.UARCH_INDEX="):].rstrip().rstrip(";"))
            self.assertEqual(payload["chips"][0]["brand"], "Apple Test")
            self.assertEqual(written[1].name, "apple-test.js")

    def test_check_catches_broken_data(self):
        self.assertTrue(any("add_x_reg" in p for p in u.check(u.merge([raw_run(add_lat=1.4)]))))
        self.assertTrue(any("exceeds" in p for p in u.check(u.merge([raw_run(tp=11.0)]))))
        m = u.merge([raw_run()])
        m["helpers"]["P"]["cmp_csinc_roundtrip"] = [3.0, 3.0, 3.0]
        self.assertTrue(any("round trip" in p for p in u.check(m)))

    def test_width_bound_is_the_nop_rate(self):
        # Tool 0.2.0 on an idle M5: one immediate move at 13 per cycle, NOPs at 10.  Nothing
        # retires faster than NOPs (DESIGN.md, "Steady states"), so it is rejected, with why.
        probs = u.check(u.merge([raw_run(tp=13.058, width=10.0)]))
        self.assertEqual(len(probs), 1, probs)
        self.assertIn("exceeds the pipeline width 10.0", probs[0])
        self.assertIn("steady states", probs[0])
        # The fastest state of the same move measures 10.065 (the loop edge): accepted.
        self.assertEqual(u.check(u.merge([raw_run(tp=10.065, width=10.0)])), [])
        # The slack is 6 %, and not more.
        self.assertEqual(u.width_problems(u.merge([raw_run(tp=10.59, width=10.0)]), "P"), [])
        self.assertEqual(len(u.width_problems(u.merge([raw_run(tp=10.61, width=10.0)]), "P")), 1)
        # Several instructions per instance are not bounded by the width.
        multi = u.merge([raw_run(tp=13.0, width=10.0, asm="add x0, x19, x20 ; nop")])
        self.assertEqual(u.width_problems(multi, "P"), [])
        # Without a conclusive width experiment there is nothing to compare with.
        broken = u.merge([raw_run(tp=13.0, width=10.0)])
        broken["structure"]["P"][0]["status"] = "inconclusive"
        self.assertEqual(u.width_problems(broken, "P"), [])

    def test_committed_results_are_valid(self):
        paths = [p for p in sorted((ROOT / "results").glob("*/*.json"))
                 if not p.name.endswith(".samples.json")]
        self.assertTrue(paths)
        for path in paths:
            data = u.load_results(path)
            self.assertEqual(u.check(data), [], path)
            self.assertEqual(u.dump_results(data), path.read_text(), f"{path} is not canonical")


if __name__ == "__main__":
    unittest.main()
