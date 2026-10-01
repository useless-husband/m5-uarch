# Changelog

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
