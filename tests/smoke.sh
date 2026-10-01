#!/bin/sh
# End-to-end smoke test: the real binary, a real (small) measurement, and the
# whole results pipeline.  Exits 77 (skipped) when the machine has no
# unprivileged cycle counters, which is the case on virtualised CI runners.
set -u
UARCH=${UARCH:-build/uarch}
PYTHON=${PYTHON:-python3}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/uarch-smoke.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

fail() { echo "smoke: FAIL: $*" >&2; exit 1; }

"$UARCH" version >/dev/null || fail "version"
"$UARCH" list -f add_x_reg | grep -q "add x0, x19, x20" || fail "list"
"$UARCH" info >"$TMP/info.txt"; rc=$?
cat "$TMP/info.txt"
[ $rc -eq 0 ] || [ $rc -eq 77 ] || fail "info exited $rc"
"$UARCH" nonsense >/dev/null 2>&1 && fail "unknown command accepted"
"$UARCH" insn -l 9 >/dev/null 2>&1 && fail "bad level accepted"

# The path a virtual machine (GitHub's macOS runners) takes: no counters.
# Every measuring command must exit 77 and say why, not fail or hang.
UARCH_COUNTERS=none "$UARCH" info >"$TMP/vm-info.txt"; rc=$?
[ $rc -eq 77 ] || fail "info without counters exited $rc, want 77"
grep -q "^counters    none" "$TMP/vm-info.txt" || fail "info without counters: no 'counters none' line"
grep -q "No cycle counter is available" "$TMP/vm-info.txt" || fail "info without counters: no reason"
UARCH_COUNTERS=none "$UARCH" selftest >"$TMP/vm-selftest.txt"; rc=$?
[ $rc -eq 77 ] || fail "selftest without counters exited $rc, want 77"
grep -q "^SKIP: no unprivileged cycle counter" "$TMP/vm-selftest.txt" || fail "selftest without counters: no reason"
for args in "insn -f add_x_reg" "structure -e width" "all -q"; do
    # shellcheck disable=SC2086
    UARCH_COUNTERS=none "$UARCH" $args >/dev/null 2>"$TMP/vm-err.txt"; rc=$?
    [ $rc -eq 77 ] || fail "'$args' without counters exited $rc, want 77"
    grep -q "exposes no cycle counter" "$TMP/vm-err.txt" || fail "'$args' without counters: no reason"
done

"$UARCH" selftest; rc=$?
if [ $rc -eq 77 ]; then
    echo "smoke: SKIP measurement: this machine exposes no cycle counters to unprivileged processes"
    exit 77
fi
[ $rc -eq 0 ] || fail "selftest exited $rc"

# Two small runs through the whole pipeline.
for i in 1 2; do
    "$UARCH" insn -q -f _x_reg -o "$TMP/run$i.json" || fail "insn run $i"
    "$UARCH" structure -q -e width,elim -o "$TMP/s$i.json" || fail "structure run $i"
    "$PYTHON" -c 'import json,sys; json.load(open(sys.argv[1])); json.load(open(sys.argv[2]))' \
        "$TMP/run$i.json" "$TMP/s$i.json" || fail "output is not valid JSON"
done
"$PYTHON" tools/uarch_results.py merge "$TMP/run1.json" "$TMP/run2.json" -o "$TMP/merged.json" \
    || fail "merge"
"$PYTHON" tools/uarch_results.py check "$TMP/merged.json" || fail "anchors"
"$PYTHON" tools/uarch_results.py csv "$TMP/merged.json" -o "$TMP" || fail "csv"
grep -q "^add_x_reg,.*latency,R0,W0,1.0," "$TMP/instructions.csv" || fail "add latency is not 1"
"$PYTHON" tools/uarch_results.py merge "$TMP/s1.json" "$TMP/s2.json" -o "$TMP/smerged.json" \
    || fail "merge structure"
"$PYTHON" tools/uarch_results.py site "$TMP/merged.json" -o "$TMP/site" || fail "site"
test -s "$TMP/site/data.js" || fail "site data missing"
echo "smoke: passed"
