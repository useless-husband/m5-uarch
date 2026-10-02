#!/usr/bin/env python3
"""Tests for tools/uarch_validate.py: every rule, the bot comment, the issue and PR flows."""

import contextlib
import io
import json
import random
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import uarch_fixtures as F  # noqa: E402
import uarch_results as ur  # noqa: E402
import uarch_submit as us  # noqa: E402
import uarch_validate as uv  # noqa: E402
from test_submission import form_body  # noqa: E402

SP = F.spec()


def override(**changes):
    """value() for the fixtures: override (kind, core, name) -> value."""
    def value(kind, core, name, v):
        return changes.get(f"{kind}:{core}:{name}", v)
    return value


def dataset(sub: dict, spec=SP) -> tuple:
    did = us.dataset_id(sub)
    return did, us.rebuild(sub, spec, did, {"kind": "pr"}), sub


def check(sub, existing=(), spec=SP) -> uv.Report:
    return uv.validate_submission(sub, list(existing), spec)


def rules(rep: uv.Report, level: str) -> set:
    return {f.rule for f in rep.findings if f.level == level}


class GoodDataTests(unittest.TestCase):
    def test_clean_submission_is_accepted(self):
        rep = check(F.packed(SP))
        self.assertEqual(rep.verdict, "accepted", rep.findings)
        self.assertEqual(rep.findings, [])
        self.assertEqual(rep.chip, "apple-m5")
        self.assertEqual(rep.results["source"], {})
        self.assertEqual(ur.check(rep.results), [])

    def test_committed_tree_passes(self):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(uv.run_tree(F.ROOT / "results"), 0)
        self.assertIn("apple-m5/apple-m5: ok", out.getvalue())


class FormatTests(unittest.TestCase):
    def mutate(self, fn) -> uv.Report:
        sub = F.packed(SP)
        fn(sub)
        return check(sub)

    def test_shape_and_privacy(self):
        cases = {
            "extra top-level field": lambda s: s.update(hostname="x"),
            "serial number in machine": lambda s: s["machine"].update(serial_number="C02XL0AAJG5J"),
            "brand with markup": lambda s: s["machine"].update(brand="Apple M5 <img src=x>"),
            "model with a path": lambda s: s["machine"].update(model="/Users/me"),
            "user name in a note": lambda s: s["structure"]["P"][0].update(note="by me@example.com"),
            "two runs": lambda s: s.update(runs=s["runs"][:2]),
            "missing field": lambda s: s.pop("helpers"),
            "run with an extra field": lambda s: s["runs"][0].update(cwd="/tmp"),
            "bad level": lambda s: s["machine"]["levels"][0].update(cores=-1),
            "not a schema": lambda s: s.update(schema="something/1"),
            "unreadable cell": lambda s: s["instructions"]["P"].__setitem__(0, "abc|def"),
            "values for too few runs": lambda s: s["instructions"]["P"].__setitem__(0, "7000|1000|1000"),
            "control characters": lambda s: s["structure"]["P"][0].update(title="a\x1b[31mb"),
            "bool as a number": lambda s: s["runs"][0].update(clean=True),
        }
        for what, fn in cases.items():
            with self.subTest(what):
                rep = self.mutate(fn)
                self.assertEqual(rep.verdict, "rejected")
                self.assertIn("format", rules(rep, "reject"))

    def test_not_an_object(self):
        self.assertEqual(check([1, 2]).verdict, "rejected")

    def test_nothing_measured_is_not_vacuously_accepted(self):
        # Regression: with no core measured there is no value for an anchor to fail on.
        sub = F.packed(SP)
        for r in sub["runs"]:
            r["measured"], r["ghz_observed"] = [], {}
        sub.update(helpers={}, instructions={}, structure={})
        rep = check(sub)
        self.assertEqual(rep.verdict, "rejected")
        self.assertIn("no core type was measured", [f.text for f in rep.findings])

    def test_every_measured_core_needs_its_values(self):
        sub = F.packed(SP)
        del sub["instructions"]["E"]
        self.assertIn("format", rules(check(sub), "reject"))

    def test_long_lists_of_problems_are_cut_short_in_the_comment(self):
        sub = F.packed(SP)
        for c in ("P", "E"):
            for x in sub["structure"][c]:
                x.update(title="\x07", unit="\x07", note="\x07", xlabel="\x07", ylabel="\x07")
        rep = check(sub)
        self.assertGreater(len(rep.findings), 30)
        self.assertIn("... and", uv.render_comment([rep], "issue"))

    def test_tool_version_and_instruction_table(self):
        for what, fn in {
            "unsupported version": lambda s: s.update(tool_version="0.0.9"),
            "other instruction table": lambda s: s.update(spec_sha256="0" * 64),
            "cells missing": lambda s: s["instructions"]["P"].pop(),
        }.items():
            with self.subTest(what):
                rep = self.mutate(fn)
                self.assertEqual(rep.verdict, "rejected")
                self.assertEqual(rules(rep, "reject"), {"version"})


class StatisticsTests(unittest.TestCase):
    def test_sample_changed_after_packing(self):
        sub = F.packed(SP)
        cells = sub["instructions"]["P"]
        cells[3] = cells[3].replace("77", "99", 1)
        rep = check(sub)
        self.assertEqual(rep.verdict, "rejected")
        self.assertIn("statistics", rules(rep, "reject"))

    def test_structure_value_changed_after_packing(self):
        sub = F.packed(SP)
        sub["structure"]["P"][2]["runs"][0][1] = 9999
        self.assertIn("statistics", rules(check(sub), "reject"))


class AnchorAndConsistencyTests(unittest.TestCase):
    def test_broken_anchors(self):
        for what, ov in {
            "add takes 1.4 cycles": {"lat:P:add_x_reg": 1.4},
            "eor takes 2 cycles": {"lat:E:eor_x_reg": 2},
            "throughput above the width": {"tp:E:add_x_reg": 9.0},
        }.items():
            with self.subTest(what):
                rep = check(F.packed(SP, value=override(**ov)))
                self.assertEqual(rep.verdict, "rejected")
                self.assertIn("anchors", rules(rep, "reject"))

    def test_hard_inconsistencies_reject(self):
        for what, ov in {
            "more ALUs than the width": {"st:P:units_alu": 12},
            "L1 latency out of range": {"st:E:l1_latency": 12},
            "absurd reorder buffer": {"st:P:rob_nop": 40},
        }.items():
            with self.subTest(what):
                rep = check(F.packed(SP, value=override(**ov)))
                self.assertEqual(rep.verdict, "rejected", rep.findings)
                self.assertIn("consistency", rules(rep, "reject"))

    def test_surprises_flag(self):
        for what, ov in {
            "P-core window smaller than the E-core's": {"st:P:rob_nop": 900},
            "NOP rate disagrees with the width": {"tp:P:nop": 8.5},
            "L1D far from what macOS reports": {"st:P:l1d_size": 512},
            "adds faster than the ALU count": {"tp:E:add_x_imm": 4.6},
        }.items():
            with self.subTest(what):
                rep = check(F.packed(SP, value=override(**ov)))
                self.assertEqual(rep.verdict, "flagged", rep.findings)
                self.assertEqual(rules(rep, "flag"), {"consistency"})

    def test_disturbed_runs_flag(self):
        runs = F.make_runs(SP, 3)
        for r in runs:
            r["run"]["discarded_disturbed"] = 400
        rep = check(us.pack(runs, SP))
        self.assertEqual(rep.verdict, "flagged")
        self.assertEqual(rules(rep, "flag"), {"quality"})


class OutlierTests(unittest.TestCase):
    def earlier(self, n: int) -> list:
        return [dataset(F.packed(SP, seed=100 + i)) for i in range(n)]

    def test_duplicate_is_rejected(self):
        sub = F.packed(SP)
        rep = check(sub, [dataset(sub)])
        self.assertIn("duplicate", rules(rep, "reject"))

    def test_agreeing_submission_is_accepted(self):
        rep = check(F.packed(SP, seed=7), self.earlier(4))
        self.assertEqual(rep.verdict, "accepted", rep.findings)
        self.assertEqual(rep.n_prev, 4)
        self.assertGreater(rep.compared, 40)
        self.assertEqual(rep.outliers, [])

    def test_one_outlier_among_several_fake_submissions(self):
        rep = check(F.packed(SP, seed=7, value=override(**{"tp:P:mul_x": 4.5})), self.earlier(4))
        self.assertEqual([o.key for o in rep.outliers], ["P|tp|mul_x"])
        # One value of ~50 in this small table is over the 0.5 % share.
        self.assertEqual(rep.verdict, "flagged")
        self.assertEqual(rules(rep, "flag"), {"outliers"})
        comment = uv.render_comment([rep], "issue")
        self.assertIn("`P throughput mul_x`", comment)
        self.assertIn("4.5", comment)

    def test_outlier_against_a_single_earlier_dataset(self):
        earlier = self.earlier(1)
        near = check(F.packed(SP, seed=7, value=override(**{"tp:P:mul_x": 3.2})), earlier)
        self.assertEqual(near.outliers, [])
        far = check(F.packed(SP, seed=7, value=override(**{"tp:P:mul_x": 4.0})), earlier)
        self.assertEqual([o.key for o in far.outliers], ["P|tp|mul_x"])

    def test_mad_rule_uses_the_spread_of_earlier_datasets(self):
        def v(med, lo=None, hi=None, kind="tp", conf=None):
            return uv.Val(med, med if lo is None else lo, med if hi is None else hi, kind, conf, "x")
        prev = [{"k": v(x)} for x in (5.0, 5.2, 4.8, 5.4, 4.6)]   # MAD 0.2 -> 5 * 0.297 = 1.48
        self.assertEqual(uv.outliers({"k": v(6.2)}, prev)[0], [])
        self.assertEqual(len(uv.outliers({"k": v(6.6)}, prev)[0]), 1)
        # A wide run-to-run range in the new value is slack, not evidence.
        self.assertEqual(uv.outliers({"k": v(6.6, 5.0, 8.2)}, prev)[0], [])
        # Low-confidence structure values are not compared at all.
        self.assertEqual(uv.outliers({"k": v(60, kind="st", conf="low")},
                                     [{"k": v(5, kind="st", conf="high")}]), ([], 0))

    def test_real_m5_noise_is_noted_not_flagged(self):
        sub, results_text = F.committed_m5()
        spec = us.load_spec()
        runs = us.expand(sub, spec)
        rng = random.Random(F.SEED)
        for r in runs:          # a "second machine": every value moved by up to 0.3 %
            for ins in r["instructions"]:
                for c in ("P", "E"):
                    x = ins.get(c, {})
                    if x.get("tp", {}).get("per_cycle"):
                        x["tp"]["per_cycle"] = round(x["tp"]["per_cycle"] * rng.uniform(0.997, 1.003), 3)
                    if ins["name"] == "fdiv_d" and c == "P" and x.get("lat"):
                        x["lat"][0]["cycles"] = 15.0      # one value clearly wrong
        new = us.pack(runs, spec)
        existing = [("apple-m5", json.loads(results_text), sub)]
        rep = uv.validate_submission(new, existing, spec)
        self.assertEqual(rep.verdict, "accepted", [f.text for f in rep.findings])
        self.assertIn("P|lat|fdiv_d|0", [o.key for o in rep.outliers])
        self.assertLess(len(rep.outliers), 5)
        self.assertEqual(rules(rep, "note"), {"outliers"})


class CommentTests(unittest.TestCase):
    def test_comment_never_echoes_submitted_markup(self):
        sub = F.packed(SP)
        sub["machine"]["brand"] = "Apple M5 <img src=x onerror=alert(1)>"
        sub["evil<script>"] = 1
        comment = uv.render_comment([check(sub)], "issue")
        self.assertIn("**Submission check: rejected**", comment)
        self.assertNotIn("<img", comment)
        self.assertNotIn("<script", comment)
        self.assertIn("(not shown)", comment)

    def test_accepted_comment(self):
        comment = uv.render_comment([check(F.packed(SP))], "issue")
        self.assertIn("**Submission check: accepted**", comment)
        self.assertIn("first dataset of this chip", comment)
        self.assertIn("determined forger", comment)
        self.assertIn("<!-- m5-uarch-check -->", comment)
        self.assertNotIn("not run", comment)


class IssueFlowTests(unittest.TestCase):
    def run_issue(self, body: str, results: Path, out: Path) -> str:
        event = {"issue": {"number": 12, "body": body, "state": "open"}}
        return uv.run_issue(event, results, out, SP)

    def test_accepted_issue_writes_the_dataset_and_installs(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            sub = F.packed(SP)
            self.assertEqual(self.run_issue(form_body(us.encode(sub)), d / "results", d / "out"),
                             "accepted")
            info = json.loads((d / "out" / "verdict.json").read_text())
            did = us.dataset_id(sub)
            self.assertEqual(info, {"verdict": "accepted", "issue": 12, "chip": "apple-m5",
                                    "dataset": did, "brand": "Apple M5", "model": "Mac17,2"})
            res = json.loads((d / "out/files/results/apple-m5" / f"{did}.json").read_text())
            self.assertEqual(res["source"], {"kind": "issue", "number": 12})
            self.assertIn("Closes #12", (d / "out" / "pr.md").read_text())
            self.assertIn("from issue #12", (d / "out" / "commit.txt").read_text())
            names = uv.install(d / "out", d / "repo")
            self.assertEqual(names.split(), [f"results/apple-m5/{did}.json",
                                             f"results/apple-m5/{did}.samples.json"])
            with self.assertRaises(SystemExit):
                uv.install(d / "out", d / "repo")     # never overwrites
            # Submitting the same data again is a duplicate.
            self.assertEqual(self.run_issue(form_body(us.encode(sub)), d / "repo" / "results",
                                            d / "out2"), "rejected")
            self.assertIn("already published", (d / "out2" / "comment.md").read_text())

    def test_rejected_issues(self):
        good = us.encode(F.packed(SP))
        for what, body, needle in [
            ("licence not ticked", form_body(good, agreed=False), "MIT licence"),
            ("nothing pasted", "### Submission\n\n_No response_\n", "no submission found"),
            ("text cut short", form_body("\n".join(good.splitlines()[:9])), "cut off"),
        ]:
            with self.subTest(what), tempfile.TemporaryDirectory() as d:
                d = Path(d)
                self.assertEqual(self.run_issue(body, d / "results", d / "out"), "rejected")
                self.assertIn(needle, (d / "out" / "comment.md").read_text())
                self.assertFalse((d / "out" / "files").exists())
                with self.assertRaises(SystemExit):
                    uv.install(d / "out", d / "repo")


    def test_crash_in_processing_becomes_a_rejection(self):
        from unittest import mock
        body = form_body(us.encode(F.packed(SP)))
        with tempfile.TemporaryDirectory() as d, \
                mock.patch.object(uv.ur, "merge", side_effect=TypeError("boom")), \
                contextlib.redirect_stderr(io.StringIO()):
            d = Path(d)
            self.assertEqual(self.run_issue(body, d / "results", d / "out"), "rejected")
            self.assertIn("could not be processed", (d / "out" / "comment.md").read_text())

    def test_licence_row_reflects_the_body_even_if_the_data_is_unreadable(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            body = form_body("nothing useful\n")
            self.run_issue(body, d / "results", d / "out")
            comment = (d / "out" / "comment.md").read_text()
            self.assertIn("| Licence box ticked | passed |", comment)
            self.run_issue(form_body("x\n", agreed=False), d / "results", d / "out2")
            self.assertIn("| Licence box ticked | **failed** |", (d / "out2" / "comment.md").read_text())


class PullRequestTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        self.sub = F.packed(SP)
        self.did = us.dataset_id(self.sub)
        head = self.d / "head" / "results" / "apple-m5"
        head.mkdir(parents=True)
        (head / f"{self.did}.samples.json").write_text(us.dump_samples(self.sub))
        self.res_path = head / f"{self.did}.json"
        self.res_path.write_text(ur.dump_results(us.rebuild(self.sub, SP, self.did, {"kind": "pr"})))
        self.files = [("added", f"results/apple-m5/{self.did}.json"),
                      ("added", f"results/apple-m5/{self.did}.samples.json")]

    def tearDown(self):
        self.tmp.cleanup()

    def run_pr(self, changes) -> tuple[str, str]:
        out = self.d / "out"
        v = uv.run_pr(changes, uv.dir_reader(self.d / "head"), self.d / "base", out, SP)
        return v, (out / "comment.md").read_text()

    def test_good_pull_request(self):
        v, comment = self.run_pr(self.files + [("modified", "README.md")])
        self.assertEqual(v, "accepted", comment)
        self.assertIn(f"Dataset `{self.did}`", comment)

    def test_tampered_statistics_name_the_value(self):
        res = json.loads(self.res_path.read_text())
        ins = next(i for i in res["instructions"] if i["name"] == "mul_x")
        ins["P"]["tp"][0] = 3.5
        self.res_path.write_text(ur.dump_results(res))
        v, comment = self.run_pr(self.files)
        self.assertEqual(v, "rejected")
        self.assertIn("does not follow from its samples file", comment)
        self.assertIn("`P throughput mul_x` is 3.5", comment)

    def test_layout_problems(self):
        v, comment = self.run_pr(self.files[:1])
        self.assertEqual(v, "rejected")
        self.assertIn("add both", comment)
        v, comment = self.run_pr(self.files + [("added", "results/apple-m5/notes.txt")])
        self.assertEqual(v, "flagged")
        v, comment = self.run_pr(self.files + [("removed", "results/apple-m5/apple-m5.json")])
        self.assertEqual(v, "flagged")
        self.assertIn("needs a maintainer", comment)
        v, comment = self.run_pr([("modified", "README.md")])
        self.assertEqual(v, "flagged")
        self.assertIn("no dataset files", comment)

    def test_wrong_names(self):
        base = self.d / "head" / "results"
        (base / "apple-m4").mkdir()
        for p in (base / "apple-m5").iterdir():
            (base / "apple-m4" / p.name).write_text(p.read_text())
            (base / "apple-m5" / p.name.replace(self.did, "mydata")).write_text(p.read_text())
        v, comment = self.run_pr([(s, n.replace("apple-m5", "apple-m4")) for s, n in self.files])
        self.assertEqual(v, "rejected")
        self.assertIn("belongs in results/apple-m5/", comment)
        v, comment = self.run_pr([(s, n.replace(self.did, "mydata")) for s, n in self.files])
        self.assertEqual(v, "rejected")
        self.assertIn("named after the data's id", comment)


if __name__ == "__main__":
    unittest.main()
