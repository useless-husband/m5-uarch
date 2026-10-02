#!/usr/bin/env python3
"""Tests for tools/uarch_submit.py: packer, privacy filter, text armour, issue parser."""

import base64
import gzip
import hashlib
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import uarch_fixtures as F  # noqa: E402
import uarch_results as ur  # noqa: E402
import uarch_submit as us  # noqa: E402

SP = F.spec()


def strip_unsent(merged: dict) -> dict:
    m = F.clone(merged)
    m["machine"].pop("memory_bytes", None)
    for r in m["runs"]:
        r.pop("started", None)
    return m


def form_body(text: str, agreed: bool = True, crlf: bool = False) -> str:
    """An issue body the way GitHub renders the submission form."""
    body = ("### Submission\n\n```text\n" + text + "```\n\n### Notes (optional)\n\n_No response_"
            "\n\n### Licence\n\n- [" + ("X" if agreed else " ") + "] I measured this on my own "
            "Mac, and I agree that the data is published in this repository under its MIT "
            "licence.\n")
    return body.replace("\n", "\r\n") if crlf else body


class PackTests(unittest.TestCase):
    def test_pack_reproduces_the_merge_of_the_raw_runs(self):
        runs = F.make_runs(SP, 3)
        sub = us.pack(runs, SP)
        rebuilt = ur.merge(us.expand(sub, SP))
        self.assertEqual(ur.dump_results(rebuilt), ur.dump_results(strip_unsent(ur.merge(runs))))
        self.assertEqual(sub["stats_sha256"], us.stats_digest(rebuilt))
        self.assertEqual(sub["spec_sha256"], SP.digest)

    def test_pack_of_the_expansion_is_the_same_submission(self):
        sub = F.packed(SP, 4)
        again = us.pack(us.expand(sub, SP), SP)
        self.assertEqual(us.canonical(again), us.canonical(sub))
        self.assertEqual(us.dataset_id(again), us.dataset_id(sub))

    def test_flags_statuses_and_unsupported_survive(self):
        runs = F.make_runs(SP, 3)
        runs[1]["instructions"][0]["P"]["tp"]["status"] = "noisy"
        runs[2]["instructions"][1]["E"]["lat"][0]["status"] = "bad-count"
        sub = us.pack(runs, SP)
        cell = sub["instructions"]["P"][0]
        self.assertIn(",noisy,", cell)
        self.assertTrue(sub["instructions"]["P"][6].startswith("4"), "fmla tp")
        self.assertIn(":c", sub["instructions"]["P"][6])
        self.assertEqual(sub["instructions"]["E"][7], "!4")
        m = ur.merge(us.expand(sub, SP))
        sve = [i for i in m["instructions"] if i["name"] == "sve_add"][0]
        self.assertEqual(sve["P"], {"unsupported": True, "signal": 4})

    def test_whole_numbers_stay_integers(self):
        # The tool prints 2, not 2.000; the canonical results text depends on it.
        self.assertEqual(us._parse_value("2000", "t"), ("ok", 2, ""))
        self.assertEqual(us._parse_value("-150", "t"), ("ok", -0.15, ""))
        self.assertEqual(us._parse_value("x:r", "t"), ("ok", None, "r"))
        self.assertEqual(us._parse_value("noisy:c", "t"), ("noisy", None, "c"))
        with self.assertRaises(us.SubmissionError):
            us._parse_value("1.5", "t")

    def test_pack_refuses_unusable_runs(self):
        cases = {
            "two runs": lambda rs: rs[:2],
            "mixed versions": lambda rs: [rs[0], rs[1], {**rs[2], "tool_version": "0.1.1"}],
            "old version": lambda rs: [{**r, "tool_version": "0.0.9"} for r in rs],
            "different machines": lambda rs: [rs[0], rs[1], _with(rs[2], "machine.model", "Mac16,1")],
            "virtual machine": lambda rs: [_with(r, "machine.virtual_machine", True) for r in rs],
            "modified table": lambda rs: [_with(r, "instructions.0.asm", "add x1, x2, x3") for r in rs],
            "not a raw run": lambda rs: [{**r, "schema": "x"} for r in rs],
            "inconsistent support": lambda rs: [rs[0], rs[1], _with(
                rs[2], "instructions.0.P", {"supported": False, "signal": 4})],
        }
        for what, change in cases.items():
            with self.subTest(what), self.assertRaises(us.SubmissionError):
                us.pack(change(F.make_runs(SP, 3)), SP)


def _with(run: dict, path: str, value) -> dict:
    run = F.clone(run)
    obj = run
    keys = path.split(".")
    for k in keys[:-1]:
        obj = obj[int(k)] if k.isdigit() else obj[k]
    obj[keys[-1]] = value
    return run


class PrivacyTests(unittest.TestCase):
    SECRETS = {
        "hostname": "Johns-MacBook-Pro.local",
        "user": "johnsmith",
        "serial_number": "C02XL0AAJG5J",
        "platform_uuid": "4C4C4544-0039-3010-8044-B4C04F4E4A32",
        "cwd": "/Users/johnsmith/src/m5-uarch",
        "email": "john@example.com",
    }

    def test_identifying_fields_are_never_packed(self):
        runs = F.make_runs(SP, 3)
        for r in runs:
            r.update(self.SECRETS)
            r["machine"].update(self.SECRETS)
            r["run"].update(self.SECRETS)
            r["machine"]["levels"][0]["uuid"] = self.SECRETS["platform_uuid"]
        sub = us.pack(runs, SP)
        text = us.canonical(sub)
        for k, v in self.SECRETS.items():
            self.assertNotIn(v, text, k)
            self.assertNotIn(f'"{k}"', text, k)
        for dropped in ("started", "memory_bytes", "17179869184", "2026-10-02T"):
            self.assertNotIn(dropped, text)
        self.assertEqual(set(sub), set(us.TOP_KEYS))
        self.assertEqual(set(sub["machine"]) - {"levels"}, set(us.MACHINE_KEYS))
        for lv in sub["machine"]["levels"]:
            self.assertEqual(set(lv), set(us.LEVEL_KEYS))
        for r in sub["runs"]:
            self.assertEqual(set(r), set(us.RUN_KEYS))
        self.assertEqual(us.privacy_problems(sub), [])

    def test_privacy_scan_finds_each_kind(self):
        for what, s in [("path", "see /Users/someone/x"), ("path", "~/Desktop"),
                        ("address", "mail a@b.org"), ("UUID", self.SECRETS["platform_uuid"]),
                        ("serial", "serial C02XL0AAJG5J here"), ("host", "on mymac.local")]:
            with self.subTest(what):
                self.assertTrue(us.privacy_problems({"structure": {"P": [{"note": s}]}}))
        for s in ("Pipeline width (sustained NOPs per cycle)", "26A428", "Mac17,2",
                  "loads / stores", "thread_selfcounts"):
            self.assertEqual(us.privacy_problems({"k": s}), [], s)

    def test_packer_refuses_tool_text_that_looks_identifying(self):
        runs = F.make_runs(SP, 3)
        for r in runs:
            r["structure"]["P"][0]["note"] = "written by /Users/someone/bin/uarch"
        with self.assertRaises(us.SubmissionError):
            us.pack(runs, SP)

    def test_committed_m5_data_is_clean(self):
        sub, _ = F.committed_m5()
        self.assertEqual(us.privacy_problems(sub), [])
        self.assertNotIn("memory_bytes", json.dumps(sub))
        self.assertNotIn("started", json.dumps(sub))


class ArmourTests(unittest.TestCase):
    def setUp(self):
        self.sub = F.packed(SP)
        self.text = us.encode(self.sub)

    def test_roundtrip_and_header(self):
        self.assertEqual(us.decode(self.text), self.sub)
        # Regression: the armour sorts keys; the decoded submission must come
        # back in the packer's order, or the rebuilt results text (and its
        # digest) differ.
        self.assertEqual(json.dumps(us.decode(self.text)), json.dumps(self.sub))
        rebuilt = us.rebuild(us.decode(self.text), SP)
        self.assertEqual(us.stats_digest(rebuilt), self.sub["stats_sha256"])
        lines = self.text.splitlines()
        self.assertEqual(lines[0], us.BEGIN)
        self.assertEqual(lines[-1], us.END)
        self.assertIn("chip: Apple M5 (Mac17,2)", lines)
        self.assertTrue(all(len(ln) <= us.LINE for ln in lines[4:-1]))

    def test_found_inside_any_text_and_with_crlf(self):
        self.assertEqual(us.decode("hello\r\n" + self.text.replace("\n", "\r\n") + "bye"), self.sub)
        self.assertEqual(us.decode("  " + self.text.replace("\n", "\n   ")), self.sub)

    def test_damage_is_reported(self):
        lines = self.text.splitlines()
        mid = len(lines) // 2
        flipped = lines[mid][:5] + ("A" if lines[mid][5] != "A" else "B") + lines[mid][6:]
        cases = {
            "no block": "nothing here",
            "two blocks": self.text + self.text,
            "no end": "\n".join(lines[:-1]),
            "cut short": "\n".join(lines[:mid] + lines[-1:]),
            "foreign characters": "\n".join(lines[:mid] + ["<b>hi</b>"] + lines[mid:]),
            "changed character": "\n".join(lines[:mid] + [flipped] + lines[mid + 1:]),
            "unknown format": self.text.replace(us.SCHEMA, "m5-uarch-submission/9"),
            "missing sha": "\n".join(ln for ln in lines if not ln.startswith("sha256:")),
        }
        for what, text in cases.items():
            with self.subTest(what), self.assertRaises(us.SubmissionError):
                us.decode(text)

    def test_values_changed_after_packing_fail_the_digest(self):
        raw = us.canonical(self.sub).encode()
        changed = us.canonical({**self.sub, "tool_version": "0.1.1"}).encode()
        b64 = base64.b64encode(gzip.compress(changed, mtime=0)).decode()
        text = "\n".join([us.BEGIN, f"format: {us.SCHEMA}",
                          f"sha256: {hashlib.sha256(raw).hexdigest()}", b64, us.END])
        with self.assertRaisesRegex(us.SubmissionError, "sha256"):
            us.decode(text)

    def test_decompression_bomb_is_refused(self):
        raw = b"[" + b"0," * (us.MAX_JSON // 2 + 10) + b"0]"
        b64 = base64.b64encode(gzip.compress(raw, mtime=0)).decode()
        text = "\n".join([us.BEGIN, f"format: {us.SCHEMA}",
                          f"sha256: {hashlib.sha256(raw).hexdigest()}", b64, us.END])
        self.assertLess(len(text), 70_000)
        with self.assertRaisesRegex(us.SubmissionError, "larger"):
            us.decode(text)

    def test_not_json_or_not_an_object(self):
        for raw in (b"not json", b"[1, 2]"):
            b64 = base64.b64encode(gzip.compress(raw, mtime=0)).decode()
            text = "\n".join([us.BEGIN, f"format: {us.SCHEMA}",
                              f"sha256: {hashlib.sha256(raw).hexdigest()}", b64, us.END])
            with self.subTest(raw), self.assertRaises(us.SubmissionError):
                us.decode(text)

    def test_real_m5_submission_fits_in_an_issue(self):
        sub, _ = F.committed_m5()
        text = us.encode(sub)
        self.assertLess(len(text), us.MAX_TEXT)
        self.assertLess(len(form_body(text)), 65536)
        self.assertEqual(us.decode(form_body(text)), sub)


class IssueTests(unittest.TestCase):
    def test_form_body(self):
        sub = F.packed(SP)
        got = us.parse_issue(form_body(us.encode(sub)))
        self.assertEqual(got.submission, sub)
        self.assertTrue(got.agreed)
        got = us.parse_issue(form_body(us.encode(sub), crlf=True))
        self.assertEqual(got.submission, sub)
        self.assertTrue(got.agreed)

    def test_unticked_licence(self):
        self.assertFalse(us.parse_issue(form_body(us.encode(F.packed(SP)), agreed=False)).agreed)

    def test_empty_or_oversized(self):
        for body in ("", "   ", None, "x" * 70_001):
            with self.subTest(len(body or "")), self.assertRaises(us.SubmissionError):
                us.parse_issue(body)

    def test_issue_url_and_slug(self):
        url = us.issue_url(F.packed(SP))
        self.assertTrue(url.startswith(f"https://github.com/{us.REPO}/issues/new?template=submission.yml"))
        self.assertIn("Apple+M5+%28Mac17%2C2%29", url)
        self.assertEqual(us.chip_slug("Apple M4 Pro"), "apple-m4-pro")


class CommittedDataTests(unittest.TestCase):
    def test_samples_file_is_canonical_and_rebuilds_the_results(self):
        sub, results_text = F.committed_m5()
        path = F.ROOT / "results" / "apple-m5" / "apple-m5.samples.json"
        self.assertEqual(us.dump_samples(sub), path.read_text())
        spec = us.load_spec()
        self.assertEqual(sub["spec_sha256"], spec.digest)
        res = json.loads(results_text)
        rebuilt = us.rebuild(sub, spec, "apple-m5", res["source"])
        self.assertEqual(ur.dump_results(rebuilt), results_text)
        self.assertEqual(us.stats_digest(rebuilt), sub["stats_sha256"])

    def test_generator_spec_matches_what_the_tool_reported(self):
        # The instruction metadata in the committed results was written by the
        # C tool; the spec is regenerated from insns/*.def in Python.
        _, results_text = F.committed_m5()
        spec = {e["name"]: e for e in us.load_spec().entries}
        for ins in json.loads(results_text)["instructions"]:
            e = spec[ins["name"]]
            self.assertEqual((ins["group"], ins["ext"], ins["asm"], ins.get("note", "")),
                             (e["group"], e["ext"], e["asm"], e["note"]), ins["name"])
            for p, q in zip(ins.get("paths", []), e["paths"]):
                self.assertEqual(p["chain"], q["chain"], ins["name"])


if __name__ == "__main__":
    unittest.main()
