# Changelog

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
