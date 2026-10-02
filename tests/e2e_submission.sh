#!/bin/sh
# End-to-end test of the submission path, without GitHub.
#
# It runs the same commands as .github/workflows/submission.yml and
# results-pr.yml: a "second Mac" (the committed M5 runs with every throughput
# moved by up to 0.3 %) is packed with `uarch_submit.py pack`, pasted into an
# issue body the way GitHub renders the form, checked by the first job,
# published by the second job's script into a local bare repository with a
# stand-in `gh`, and the result is re-checked and turned into site data.  A
# damaged paste must be rejected without pushing anything, and the
# pull-request check must read a branch's files through `git show`.
set -eu
PYTHON=${PYTHON:-python3}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/uarch-e2e.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
fail() { echo "e2e: FAIL: $*" >&2; exit 1; }

# 1. The repository as the Actions runner sees it: main checked out, an
#    origin to push to.
REPO="$TMP/repo"
mkdir -p "$REPO/.github"
cp -R "$ROOT/tools" "$ROOT/insns" "$ROOT/results" "$ROOT/reference" "$REPO/"
cp -R "$ROOT/.github/scripts" "$REPO/.github/"
rm -rf "$REPO/results/local" "$REPO/tools/__pycache__"
git init -q -b main "$REPO"
git -C "$REPO" config user.name e2e
git -C "$REPO" config user.email e2e@invalid
git -C "$REPO" add -A
git -C "$REPO" commit -q -m base
git init -q --bare "$TMP/origin.git"
git -C "$REPO" remote add origin "$TMP/origin.git"
git -C "$REPO" push -q origin main
base=$(git -C "$REPO" rev-parse main)

# 2. Raw runs from "another Mac" of the same chip.
make_runs() {  # seed outdir
    "$PYTHON" - "$REPO" "$1" "$2" <<'EOF'
import json, random, sys
from pathlib import Path
repo, seed, out = Path(sys.argv[1]), int(sys.argv[2]), Path(sys.argv[3])
sys.path.insert(0, str(repo / "tools"))
import uarch_submit as us
sub = json.loads((repo / "results/apple-m5/apple-m5.samples.json").read_text())
runs = us.expand(sub, us.load_spec())[:3]
rng = random.Random(seed)
for r in runs:
    for ins in r["instructions"]:
        for core in ("P", "E"):
            tp = ins.get(core, {}).get("tp")
            if tp and tp.get("per_cycle"):
                tp["per_cycle"] = round(tp["per_cycle"] * rng.uniform(0.997, 1.003), 3)
out.mkdir(parents=True)
for i, r in enumerate(runs):
    (out / f"run-{i + 1}.json").write_text(json.dumps(r))
EOF
}
make_runs 1 "$TMP/runs"
(cd "$REPO" && "$PYTHON" tools/uarch_submit.py pack "$TMP"/runs/run-*.json -o "$TMP/sub") >"$TMP/pack.log" ||
    fail "pack: $(cat "$TMP/pack.log")"
grep -q "^not sent " "$TMP/pack.log" || fail "pack did not say what is left out"
dataset=$("$PYTHON" "$REPO/tools/uarch_submit.py" show "$TMP/sub/submission.txt" | sed -n 's/^dataset id  //p')
[ -n "$dataset" ] || fail "no dataset id"

# 3. The issue as GitHub renders the form, inside an `issues` event.
make_event() {  # submission.txt number event.json
    "$PYTHON" - "$1" "$2" "$3" <<'EOF'
import json, sys
text, number, out = open(sys.argv[1]).read(), int(sys.argv[2]), sys.argv[3]
body = ("### Submission\n\n```text\n" + text + "```\n\n### Notes (optional)\n\nPlugged in, "
        "nothing else running.\n\n### Licence\n\n- [X] I measured this on my own Mac, and I "
        "agree that the data is published in this repository under its MIT licence.\n")
event = {"action": "opened", "issue": {"number": number, "state": "open", "body": body,
         "labels": [{"name": "submission"}], "user": {"login": "someone"}}}
json.dump(event, open(out, "w"))
EOF
}
make_event "$TMP/sub/submission.txt" 7 "$TMP/event.json"

# 4. Job 1 of submission.yml.
verdict=$(cd "$REPO" && "$PYTHON" tools/uarch_validate.py issue --event "$TMP/event.json" \
              --results results --out "$TMP/check")
[ "$verdict" = accepted ] || fail "verdict $verdict: $(cat "$TMP/check/comment.md")"

# 5. Job 2 of submission.yml, with a stand-in gh that records its calls.
mkdir "$TMP/bin"
cat >"$TMP/bin/gh" <<'EOF'
#!/bin/sh
echo "gh $*" >>"$GH_LOG"
case "$1 $2" in
"pr create") echo "https://github.com/useless-husband/m5-uarch/pull/8" ;;
esac
exit 0
EOF
chmod +x "$TMP/bin/gh"
publish() {  # issue checkdir
    (cd "$REPO" && git checkout -q main &&
        PATH="$TMP/bin:$PATH" GH_LOG="$TMP/gh.log" ISSUE=$1 CHECK=$2 \
            GITHUB_REPOSITORY=useless-husband/m5-uarch sh .github/scripts/publish-submission.sh)
}
publish 7 "$TMP/check" >"$TMP/publish.log" 2>&1 || fail "publish: $(cat "$TMP/publish.log")"
files=$(git -C "$TMP/origin.git" ls-tree -r --name-only submission/issue-7 -- results/apple-m5)
echo "$files" | grep -qx "results/apple-m5/$dataset.json" || fail "branch lacks the dataset: $files"
echo "$files" | grep -qx "results/apple-m5/$dataset.samples.json" || fail "branch lacks the samples"
[ "$(git -C "$TMP/origin.git" rev-parse main)" = "$base" ] || fail "main was changed"
git -C "$TMP/origin.git" log -1 --format=%s submission/issue-7 | grep -q "from issue #7" ||
    fail "commit message"
grep -q "^gh pr create --base main --head submission/issue-7" "$TMP/gh.log" || fail "no pull request"
grep -q "^gh issue comment 7 --body-file" "$TMP/gh.log" || fail "no comment"
grep -q "^gh issue edit 7 --add-label accepted" "$TMP/gh.log" || fail "no label"
grep -q "Submission check: accepted" "$TMP/check/comment.md" || fail "comment text"
grep -q "pull/8" "$TMP/check/comment.md" || fail "comment lacks the pull request"

# 6. After the maintainer merges: every dataset passes, and the site shows
#    the chip as verified by two datasets.
git -C "$REPO" checkout -q submission/issue-7
(cd "$REPO" && "$PYTHON" tools/uarch_validate.py tree results) >"$TMP/tree.log" ||
    fail "tree: $(cat "$TMP/tree.log")"
(cd "$REPO" && "$PYTHON" tools/uarch_results.py site results -o "$TMP/site" --reference reference) >/dev/null
"$PYTHON" - "$TMP/site/data.js" "$dataset" <<'EOF' || fail "site data"
import json, sys
text = open(sys.argv[1]).read()
chip = json.loads(text[len("window.UARCH_INDEX="):-2])["chips"][0]
ids = [d["id"] for d in chip["datasets"]]
assert chip["status"] == "verified" and ids == ["apple-m5", sys.argv[2]], (chip["status"], ids)
assert chip["datasets"][1]["source"] == {"kind": "issue", "number": 7}
EOF

# 7. A damaged paste: rejected, commented, nothing pushed.
"$PYTHON" - "$TMP/sub/submission.txt" "$TMP/damaged.txt" <<'EOF'
import sys
lines = open(sys.argv[1]).read().splitlines()
i = len(lines) // 2
lines[i] = lines[i][:10] + ("B" if lines[i][10] == "A" else "A") + lines[i][11:]
open(sys.argv[2], "w").write("\n".join(lines) + "\n")
EOF
make_event "$TMP/damaged.txt" 9 "$TMP/event9.json"
git -C "$REPO" checkout -q main
verdict=$(cd "$REPO" && "$PYTHON" tools/uarch_validate.py issue --event "$TMP/event9.json" \
              --results results --out "$TMP/check9")
[ "$verdict" = rejected ] || fail "damaged paste was $verdict"
publish 9 "$TMP/check9" >"$TMP/publish9.log" 2>&1 || fail "publish rejected: $(cat "$TMP/publish9.log")"
git -C "$TMP/origin.git" rev-parse -q --verify submission/issue-9 >/dev/null && fail "rejected data pushed"
grep -q "^gh issue comment 9" "$TMP/gh.log" || fail "rejection not commented"
grep -q "^gh issue edit 9 --add-label rejected" "$TMP/gh.log" || fail "rejection not labelled"

# 8. The pull-request path: files from `make submit-pr` on a branch, read by
#    the check through git (as results-pr.yml does after fetching the PR).
make_runs 2 "$TMP/runs2"
(cd "$REPO" && "$PYTHON" tools/uarch_submit.py pack "$TMP"/runs2/run-*.json -o "$TMP/sub2" \
    --into results) >"$TMP/pack2.log" || fail "pack --into: $(cat "$TMP/pack2.log")"
git -C "$REPO" checkout -q -b contributor
git -C "$REPO" add results
git -C "$REPO" commit -q -m "data: Apple M5 (Mac17,2)"
git -C "$REPO" diff --name-status main contributor | awk -F'\t' '{print ($1=="A" ? "added" : "modified") "\t" $2}' \
    >"$TMP/changes.tsv"
git -C "$REPO" checkout -q main
verdict=$(cd "$REPO" && "$PYTHON" tools/uarch_validate.py pr --changes "$TMP/changes.tsv" \
              --git-ref contributor --results results --out "$TMP/check-pr")
[ "$verdict" = accepted ] || fail "pull request: $verdict: $(cat "$TMP/check-pr/comment.md")"
grep -q "against 1 dataset" "$TMP/check-pr/comment.md" || fail "pull request not compared with the M5"

echo "e2e: ok (issue accepted and published to a branch, damaged paste rejected, pull request accepted)"
