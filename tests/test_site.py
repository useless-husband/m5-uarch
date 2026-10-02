#!/usr/bin/env python3
"""Tests for tools/uarch_site.py: one chip, several datasets per chip, disagreement, local data."""

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import uarch_fixtures as F  # noqa: E402
import uarch_results as ur  # noqa: E402
import uarch_site as site  # noqa: E402
import uarch_submit as us  # noqa: E402
from test_validate import override  # noqa: E402

SP = F.spec()


def load(outdir: Path) -> tuple[dict, dict]:
    text = (outdir / "data.js").read_text()
    assert text.startswith("window.UARCH_INDEX=") and text.endswith(";\n")
    index = json.loads(text[len("window.UARCH_INDEX="):-2])
    tables = {}
    for p in (outdir / "data").glob("*.js"):
        t = p.read_text()
        assert t.startswith("window.UARCH_CHIP(") and t.endswith(");\n")
        tables[p.stem] = json.loads(t[len("window.UARCH_CHIP("):-3])
    return index, tables


def write(results: Path, sub: dict, source: dict, chip: str = "apple-m5", local=False) -> str:
    did = us.dataset_id(sub)
    d = results / ("local" if local else "") / chip
    d.mkdir(parents=True, exist_ok=True)
    (d / f"{did}.samples.json").write_text(us.dump_samples(sub))
    (d / f"{did}.json").write_text(ur.dump_results(us.rebuild(sub, SP, did, source)))
    return did


def cell(table: dict, name: str, core: str, kind: str = "tp", k: int = 0):
    ins = next(i for i in table["instructions"] if i["name"] == name)
    return ins[core]["tp"] if kind == "tp" else ins[core]["lat"][k]


class SiteTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        self.results = self.d / "results"

    def tearDown(self):
        self.tmp.cleanup()

    def build(self) -> tuple[dict, dict]:
        site.build([self.results], self.d / "site", None)
        return load(self.d / "site")

    def test_committed_m5_alone(self):
        site.build([F.ROOT / "results"], self.d / "site", F.ROOT / "reference")
        index, tables = load(self.d / "site")
        self.assertNotIn("</", (self.d / "site" / "data" / "apple-m5.js").read_text())
        (chip,) = index["chips"]
        self.assertEqual((chip["slug"], chip["status"], chip["published"]), ("apple-m5", "single", 1))
        ds = chip["datasets"][0]
        self.assertEqual((ds["id"], ds["status"], ds["compared"], ds["source"]),
                         ("apple-m5", "accepted", 0, {"kind": "commit", "commit": "c8751e3"}))
        self.assertEqual(ds["file"], "results/apple-m5/apple-m5.json")
        res = ur.load_results(F.ROOT / "results/apple-m5/apple-m5.json")
        width = next(r for r in chip["structure"]["P"] if r["id"] == "width")
        ew = next(e for e in res["structure"]["P"] if e["id"] == "width")
        self.assertEqual(width["cell"], [ew["value"], ew["min"], ew["max"], 1, 0])
        table = tables["apple-m5"]
        self.assertEqual(len(table["instructions"]), len(res["instructions"]))
        add = next(i for i in res["instructions"] if i["name"] == "add_x_reg")
        self.assertEqual(cell(table, "add_x_reg", "P"), add["P"]["tp"] + [1, 0])
        self.assertTrue(len(index["references"]) >= 3)

    def test_three_datasets_one_disagrees_on_one_value(self):
        write(self.results, F.packed(SP, seed=1), {"kind": "commit", "commit": "abc1234"})
        write(self.results, F.packed(SP, seed=2), {"kind": "issue", "number": 3})
        odd = write(self.results, F.packed(SP, seed=3, value=override(**{"tp:P:mul_x": 4.5})),
                    {"kind": "pr"})
        index, tables = self.build()
        (chip,) = index["chips"]
        self.assertEqual([d["source"].get("kind") for d in chip["datasets"]], ["commit", "issue", "pr"])
        marked = {d["id"]: d["marked"] for d in chip["datasets"]}
        self.assertEqual(marked[odd], 1)
        self.assertEqual(sum(marked.values()), 1)
        c = cell(tables["apple-m5"], "mul_x", "P")
        self.assertEqual(c[3], 2, "the outlying dataset is left out")
        self.assertTrue(c[4] & site.F_MARK)
        self.assertLess(c[0], 3.1)
        add = cell(tables["apple-m5"], "add_x_reg", "P")
        self.assertEqual((add[3], add[4]), (3, 0))
        self.assertLessEqual(add[1], add[0])
        self.assertLessEqual(add[0], add[2])
        # One value of ~50 in this small table counts as a flagged dataset; two agree.
        self.assertEqual(chip["status"], "verified")

    def test_two_datasets_that_disagree_broadly(self):
        write(self.results, F.packed(SP, seed=1), {"kind": "issue", "number": 1})
        slow = override(**{f"tp:P:{n}": 2.0 for n in ("add_x_reg", "sub_x_reg", "eor_x_reg", "mul_x")})
        write(self.results, F.packed(SP, seed=2, value=slow), {"kind": "issue", "number": 2})
        index, tables = self.build()
        (chip,) = index["chips"]
        self.assertEqual([d["status"] for d in chip["datasets"]], ["flagged", "flagged"])
        self.assertEqual(chip["status"], "flagged")
        c = cell(tables["apple-m5"], "add_x_reg", "P")
        self.assertEqual(c[3], 2)       # nobody agrees: both shown, marked
        self.assertTrue(c[4] & site.F_MARK)

    def test_several_chips_and_local_data(self):
        write(self.results, F.packed(SP, seed=1), {"kind": "issue", "number": 1})
        write(self.results, F.packed(SP, seed=2, brand="Apple M4"), {"kind": "issue", "number": 2},
              chip="apple-m4")
        write(self.results, F.packed(SP, seed=3), {"kind": "pr"}, local=True)
        index, tables = self.build()
        self.assertEqual([c["slug"] for c in index["chips"]], ["apple-m4", "apple-m5"])
        m5 = index["chips"][1]
        self.assertEqual(m5["published"], 1)
        self.assertEqual(m5["status"], "single")
        self.assertEqual([d["status"] for d in m5["datasets"]], ["accepted", "local"])
        self.assertEqual(m5["datasets"][1]["source"], {"kind": "local"})
        self.assertEqual(cell(tables["apple-m5"], "add_x_reg", "P")[3], 1)  # local data not mixed in
        self.assertEqual(set(tables), {"apple-m4", "apple-m5"})
        rt = cell(tables["apple-m5"], "cmp_x", "P", "lat")
        self.assertIsNotNone(rt)
        sve = next(i for i in tables["apple-m5"]["instructions"] if i["name"] == "sve_add")
        self.assertEqual(sve["P"], {"unsupported": True, "signal": 4})
        fmla = cell(tables["apple-m5"], "fmla_2d", "P")
        self.assertTrue(fmla[4] & site.F_CHAIN)

    def test_stale_chip_files_are_removed(self):
        write(self.results, F.packed(SP, seed=1), {"kind": "pr"})
        (self.d / "site" / "data").mkdir(parents=True)
        (self.d / "site" / "data" / "gone.js").write_text("x")
        _, tables = self.build()
        self.assertEqual(set(tables), {"apple-m5"})


if __name__ == "__main__":
    unittest.main()
