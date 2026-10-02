# Changelog

## 0.3.0 (2026-10-02)

Throughput from one steady state; the M5 re-measured.

- A fresh measurement on an idle M5 failed its own anchor: `mov x0, #0x123400000000` at 13.06 per
  cycle with NOPs at 10. Experiments (DESIGN.md, "Steady states") showed that no loop retires
  more than 10.03 instructions per cycle, that the counted instructions and the cycle counter are
  right, and that a loop of 64-bit immediate moves on the P-core settles, per run, in one of two
  steady states (about 8.9 or 10 per cycle). The old estimator combined runs in different states.
- Throughput (instruction table, width, unit counts) now uses the runs in the fastest state that
  at least three runs reach, requires both loop lengths to agree on the fixed cost and on the
  loop's own cost, measures again when they do not, and otherwise reports the longer loop's own
  rate (marked `~`). Latencies are measured as before.
- The published M5 dataset is re-measured with this version (five runs). 122 throughput figures
  moved by more than 3 %, among them the x-register immediate moves (8.2-8.8 to 10.06), 30
  P-core SIMD and FP operations that were above their four units (up to 4.89, now 4.00 or
  below), and a post-indexed load above the three load units (3.43 to 3.00). P-core throughput
  figures whose runs differ by more than 6 %: 17.9 % before, 2.9 % now. Latencies: none moved by
  more than 3 %.
- The width anchor is unchanged (nothing retires faster than NOPs) and now says why a violation
  is an artefact; tests cover the 13-per-cycle case. Submissions must come from tool 0.3.0,
  because earlier versions produce the mixed numbers.

## 0.2.0 (2026-10-02)

Results from any Apple Silicon Mac: submit, check automatically, compare.

- `make submit` measures (or reuses the last measurement) and packs it into a block of text that
  fits in one GitHub issue: about 42 000 characters for three runs, where the results file itself
  is 370 KB. It sends the per-run values, not the statistics; the instruction text is regenerated
  on the other side and proved identical by a digest. It copies the text, opens the issue form
  and says what is not sent. `make submit-pr` writes the same data as two files under
  `results/<chip>/` for a pull request.
- Privacy: only allowlisted fields are sent. Host name, user name, serial number, UUIDs, file
  paths, memory size and start times never are, and text that looks like them is refused.
- Automatic checks (`tools/uarch_validate.py`) on issues (`submission.yml`), on pull requests
  (`results-pr.yml`) and on every committed dataset (`make validate`, CI): format, tool version
  and instruction-table digest, statistics recomputed from the per-run values, the anchors,
  internal consistency, run quality, duplicates, and a per-value outlier test against the chip's
  earlier datasets, calibrated on the M5's own runs. A bot comments accepted, flagged or rejected
  with the reasons, and puts accepted data into a pull request; nothing reaches `main` without the
  maintainer.
- Workflows read untrusted input only as data, with least-privilege tokens, never check out
  pull-request code under `pull_request_target`, and pin every action to a commit SHA (CI and
  Pages too); `tests/test_workflows.py` checks these rules. `tests/e2e_submission.sh` runs the
  workflows' commands on a sample issue without GitHub.
- Site: a chip list with dataset counts and status, several chips side by side for structure and
  instructions, the spread of each value across datasets, a source link for every dataset, and
  what "verified" and "flagged" mean. The site data is built when Pages deploys instead of being
  committed, and each chip's instruction table loads when the chip is shown.
- `make measure` writes `results/local/<chip>/`, which git does not track.
- The M5 dataset has its samples file (`results/apple-m5/apple-m5.samples.json`) and a source;
  its results file is rebuilt from it and unchanged except that the memory size and the start
  times of the runs are no longer stored.
- `test_measure`: the instruction-count check takes the best of five tries; a single try failed
  once on a busy machine.
- Tool version 0.2.0. The measurement code is unchanged, and data from 0.1.1 is accepted.

## 0.1.1 (2026-10-01)

Fixes found in review, and the M5 data measured again with them.

- Experiment results are allocated one by one, so pointers to earlier results stay valid
  (`ua_exp_tlb` wrote through freed memory when other experiments ran before it).
- The window experiment's two chases walk separate cycles; on one shared cycle they could lap
  each other and turn misses into hits.
- A loop asked for zero iterations no longer wraps its counter and runs 2^64 times. This is
  handled in C: a branch in the generated code for the same purpose was tried and moved the
  P-core integer-rename knee by one rename group.
- The generated-versus-counted instruction check catches one instruction per iteration too many
  even at the smallest loop count; the branch experiment checks its count too.
- The reorder-buffer blend leaves out filler kinds whose capacity could not be measured.
- `uarch_results.py merge` no longer overwrites the first run's clock with the median, and keeps
  every run's observed clock in the results file.
- `UARCH_COUNTERS=none` simulates a machine without counters; the smoke test uses it to check
  that every measuring command skips with status 77 and a reason, as on CI's virtual machines.
- Clearer titles for the `csel` experiments after 64 opposite outcomes.
- Results for the Apple M5 re-measured (five runs). Every structure figure's run-to-run range
  overlaps its 0.1.0 range; P-core `csel` after the opposite outcome is now 2.00 in every run
  (0.1.0: one run at 0.41).

## 0.1.0 (2026-10-01)

First version.

- Rootless measurement engine: per-thread, per-core-type cycle and instruction counters through
  `thread_selfcounts()`, with fallbacks; two-point measurement; runs filtered by exact instruction
  count and by the core type they actually ran on.
- Instruction table of 944 entries (integer, loads and stores, store-to-load forwarding, scalar FP,
  Advanced SIMD, AES/SHA/PMULL, LSE atomics, pointer authentication, branches, system), encoded by
  the system assembler at build time; latency per input operand and throughput for each.
- Structure experiments: pipeline width and unit counts; reorder buffer, register files, load and
  store queues, branch capacity (Wong's method); move elimination and zero idioms; fused pairs;
  branch misprediction penalty; L1/L2 size and latency; TLB sizes; load value prediction, `csel`
  prediction and pointer prefetching.
- Results pipeline: merge of several runs with run-to-run ranges, anchor check, CSV export,
  static results site.
- Measured data for the Apple M5 (Mac17,2, macOS 27.0): five runs, P- and E-cores.

Known gaps: no SME/SME2 or MTE instructions; pointer authentication measured with inactive keys;
cross-register-file latencies are round trips.
