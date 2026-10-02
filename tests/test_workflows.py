#!/usr/bin/env python3
"""Static checks of the workflow security rules in docs/DESIGN.md ("Threat model").

No YAML library (standard library only): the checks read the files line by
line, which is enough for the shapes these workflows use.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORKFLOWS = sorted((ROOT / ".github" / "workflows").glob("*.yml"))


def run_blocks(text: str) -> list[str]:
    """The text of every `run:` step, single-line or block."""
    lines = text.splitlines()
    out = []
    i = 0
    while i < len(lines):
        m = re.match(r"^(\s*)(?:- )?run:\s*(.*)$", lines[i])
        if not m:
            i += 1
            continue
        indent, rest = len(m.group(1)), m.group(2)
        if rest.strip() in ("|", ">", "|-", ">-"):
            body = []
            i += 1
            while i < len(lines) and (not lines[i].strip() or
                                      len(lines[i]) - len(lines[i].lstrip()) > indent):
                body.append(lines[i])
                i += 1
            out.append("\n".join(body))
        else:
            out.append(rest)
            i += 1
    return out


class WorkflowTests(unittest.TestCase):
    def test_there_are_workflows(self):
        names = {p.name for p in WORKFLOWS}
        self.assertTrue({"ci.yml", "pages.yml", "submission.yml", "results-pr.yml"} <= names)

    def test_actions_are_pinned_to_commit_shas(self):
        for p in WORKFLOWS:
            for line in p.read_text().splitlines():
                m = re.search(r"uses:\s*(\S+)", line)
                if m:
                    self.assertRegex(m.group(1), r"^[\w.-]+/[\w./-]+@[0-9a-f]{40}$", f"{p.name}: {line}")

    def test_no_expressions_inside_scripts(self):
        # Values reach scripts through env: or files, never by ${{ }} expansion.
        for p in WORKFLOWS:
            for block in run_blocks(p.read_text()):
                self.assertNotIn("${{", block, f"{p.name}: {block[:80]}")

    def test_every_workflow_sets_permissions(self):
        for p in WORKFLOWS:
            self.assertRegex(p.read_text(), r"(?m)^permissions:", p.name)

    def test_untrusted_input_workflows(self):
        sub = (ROOT / ".github/workflows/submission.yml").read_text()
        self.assertRegex(sub, r"(?m)^permissions: \{\}$")
        check_job = sub.split("  publish:")[0]
        self.assertIn("contents: read", check_job)
        self.assertNotIn("write", check_job.split("jobs:")[1])
        self.assertIn("persist-credentials: false", check_job)
        self.assertIn('--event "$GITHUB_EVENT_PATH"', sub)

        pr = (ROOT / ".github/workflows/results-pr.yml").read_text()
        self.assertIn("pull_request_target", pr)
        # Never check out the pull request's code under pull_request_target.
        self.assertNotRegex(pr, r"ref:\s*\$\{\{\s*github\.event\.pull_request")
        self.assertNotIn("head.sha", pr)
        for block in run_blocks(pr):
            for line in block.splitlines():
                if "pr/head" in line:
                    self.assertTrue("git fetch" in line or "--git-ref" in line, line)
        check_job = pr.split("  comment:")[0].split("jobs:")[1]
        self.assertNotIn("write", check_job)

    def test_run_block_parser(self):
        text = "steps:\n  - run: echo a\n  - name: x\n    run: |\n      echo b\n      echo c\n  - uses: y\n"
        self.assertEqual(run_blocks(text), ["echo a", "      echo b\n      echo c"])


if __name__ == "__main__":
    unittest.main()
