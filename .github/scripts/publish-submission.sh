#!/bin/sh
# Second half of the submission workflow (.github/workflows/submission.yml).
# It runs with a token that can push branches and comment, on files that the
# trusted validator from main generated in the first job.  Nothing from the
# issue reaches this script except through verdict.json, whose chip and
# dataset names `uarch_validate.py install` checks against strict patterns.
# It never pushes to main: the data goes to submission/issue-<n> and into a
# pull request that the maintainer merges.
#
# Environment: ISSUE (number), CHECK (directory with verdict.json,
# comment.md, ...), GH_TOKEN; GITHUB_SERVER_URL and GITHUB_REPOSITORY as set
# by Actions.  tests/e2e_submission.sh runs it with a stand-in `gh`.
set -eu
: "${ISSUE:?}" "${CHECK:?}"
case "$ISSUE" in '' | *[!0-9]*) echo "bad issue number" >&2; exit 1 ;; esac
verdict=$(python3 -c 'import json, sys
v = json.load(open(sys.argv[1])).get("verdict")
print(v if v in ("accepted", "flagged", "rejected") else "")' "$CHECK/verdict.json")
[ -n "$verdict" ] || { echo "verdict.json has no valid verdict" >&2; exit 1; }
comment="$CHECK/comment.md"

if [ "$verdict" != rejected ]; then
    branch="submission/issue-$ISSUE"
    git config user.name "github-actions[bot]"
    git config user.email "41898282+github-actions[bot]@users.noreply.github.com"
    git checkout -q -B "$branch"
    files=$(python3 tools/uarch_validate.py install "$CHECK" .)
    # shellcheck disable=SC2086  # the names were checked by install
    git add $files
    git commit -q -F "$CHECK/commit.txt"
    git push -q --force origin "$branch"
    pr=$(gh pr list --head "$branch" --state open --json url --jq '.[0].url // ""')
    if [ -z "$pr" ]; then
        pr=$(gh pr create --base main --head "$branch" --title "$(cat "$CHECK/pr-title.txt")" \
                 --body-file "$CHECK/pr.md") || pr=""
    fi
    if [ -n "$pr" ]; then
        printf '\nPull request with the data: %s\n' "$pr" >>"$comment"
    else
        # Creating pull requests from Actions is a repository setting; without
        # it the branch is still there for the maintainer.
        printf '\nThe data is on branch `%s`. Maintainer: %s/%s/compare/main...%s\n' \
            "$branch" "${GITHUB_SERVER_URL:-https://github.com}" "${GITHUB_REPOSITORY:-}" \
            "$branch" >>"$comment"
    fi
fi

gh issue comment "$ISSUE" --body-file "$comment"
for label in accepted flagged rejected; do
    if [ "$label" != "$verdict" ]; then
        gh issue edit "$ISSUE" --remove-label "$label" >/dev/null 2>&1 || true
    fi
done
gh issue edit "$ISSUE" --add-label "$verdict" >/dev/null 2>&1 ||
    echo "could not add the label '$verdict' (create it in the repository)"
echo "issue #$ISSUE: $verdict"
