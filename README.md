# m5-uarch

**Measured microarchitecture data for the Apple M5 CPU, and the rootless tool that produced it.**

Instruction latency and throughput tables for about 940 instruction forms, and the sizes of the
core's internal structures, for both the performance ("Super") and the efficiency cores of the M5.
Everything is measured on real hardware from an ordinary unprivileged process: no root, no kernel
extension, no entitlements. The same tool runs on any Apple Silicon Mac with one command.

[繁體中文說明](README.zh-TW.md) · [Design notes](docs/DESIGN.md) ·
[Browse the results](https://useless-husband.github.io/m5-uarch/) ·
[JSON](results/apple-m5/apple-m5.json) · [CSV](results/apple-m5/)

## Why this exists

Tables of this kind exist for x86 (uops.info) and for the Apple M1 (Dougall Johnson's Firestorm
tables). For later Apple chips there are a few write-ups, all made with tools that need root to
program the performance counters. To my knowledge nothing comparable had been published for the
M5 when this was written (September 2026), and LLVM still schedules `apple-m5` with the model of
the 2013 Cyclone core. See [Related work](#related-work).

The idea that makes a rootless tool possible: macOS keeps per-thread cycle and retired-instruction
counts, split by core type, and hands them to the owning process through `thread_selfcounts()`.
There are no configurable events without root, so everything beyond cycles and instructions is
inferred from timing experiments; where that inference is indirect, the result says so.

## What the M5 looks like

Measured on a Mac17,2 (Apple M5, 4 P + 6 E cores, macOS 27.0), median of five runs; a range means
the runs disagreed. "Published" columns quote other people's measurements of other chips.

| | M5 P-core | M5 E-core | M4 P / E (published) | M1 P (published) |
|---|---|---|---|---|
| Pipeline width (NOPs per cycle) | 10 | 6 | 10 / 5 | 8 |
| Integer ALUs | 8 | 4 | 8 / 4 | 6 |
| Flag-setting ALUs | 4 | 4 | | 3 |
| Integer multipliers | 3 | 1 | | 2 |
| Loads / stores per cycle | 3 / 2 | 2 / 1 | 3 / 2, 2 / 1 | |
| FP/SIMD units | 4 | 3 | 4 / 3 | 4 |
| Taken branches per cycle | 2 | 1 | 2 / 1 | 1 |
| Reorder buffer, in NOPs | 3367 | 1069 | ~3184 / ~513 | ~2310 |
| Reorder buffer, blend of ordinary instructions | 1330 | 466–487 | | |
| Integer renames in flight (64-bit / 32-bit writes) | 396 / 812 | 185 / 185 | ~360 / ~720 | ~350–360 |
| FP/SIMD renames in flight | 837 | 202 | | ~400 |
| Flag renames in flight | 167 | 73 | ~175 | ~128 |
| Loads in flight | 486 | 65 | | ~130 |
| Stores in flight | 137 | 55 | | ~60 |
| Unresolved branches in flight | 194 | 73 | | ~144 |
| Branch misprediction penalty, cycles | 16.7 | 11.8 | | |
| L1D capacity / load-to-use latency | 128 KiB / 3 | 64 KiB / 3 | 128 KiB / 3, 64 KiB / 3 | |
| L2 load-to-use latency, cycles | 12 (≤1 MiB), 29–34 (2–8 MiB) | 15–19 | | |
| First-level data TLB entries | 162 | 133–193 | 160 / 192 | |
| Second-level TLB entries | 3101 | 2241–2492 | 3072 / 1024 | |

Things that stood out:

- **The E-core grew more than the P-core.** Against the published M4 figures the P-core window is
  about 6 % deeper (3367 vs ~3184 NOPs), while the E-core is a full instruction wider (6 vs 5) and
  its window holds twice as many NOPs (1069 vs ~513).
- **A load that always returns the same value takes 0.34 cycles on the P-core, not 3.** Its value
  is predicted and the dependency disappears; with three or more alternating values it is 3.00
  again. A pointer chase over one self-pointing cell, the textbook way to measure load latency,
  therefore reports nonsense. The tool chases a random 509-node cycle instead and gets 3 cycles
  (4 with an index register). The E-core shows no such prediction.
  (`uarch structure -e spec`, `lvp_const_load`)
- **`csel` whose condition never changes is predicted on the P-core.** A chain running through
  the input that `csel` does *not* select should take 2 cycles per `add`+`csel` step; it takes
  0.31, so that dependency is gone. After 64 iterations of the opposite outcome the same code
  takes exactly 2.00. `csinc` takes 2.00 throughout, and so does everything on the E-core. The
  instruction tables hold the data-flow latency (1): the tool runs every conditional select with
  inverted flags between measurements to make sure. (`csel_unselected_input` and neighbours)
- **Pointer-looking data is prefetched.** Two cache misses that should be serialised cost 1.67
  times one miss when the chased cells hold indices, but only 1.35 times when they hold pointers:
  a data-memory-dependent prefetcher fetches the second one early. On the E-core both are 1.59.
  The window experiment therefore chases indices. (`dmp_pointer_chase`, low confidence: memory
  timing on a shared machine)
- **Two 32-bit results share one physical register on the P-core**: 812 `add w` results fit in
  flight against 396 `add x`. On the E-core both are 185.
- **Store-to-load forwarding is almost free on the P-core**: store, load back and add takes 1.66
  cycles per round, so the store→load part costs 0.66 cycles (3.6 on the E-core). Forwarding a
  byte, or loading wider than the store, takes 6.
- **Register moves are eliminated, but not chains of them.** `mov` fed by a real operation has
  zero latency on the P-core; a `mov` fed by another `mov` costs 0.90 cycles: one in ten is
  eliminated, which matches one per 10-wide rename group (0.83 on the 6-wide E-core).
  `add x, x, #0` is eliminated like a move. `eor x, x, x` is *not* a zero idiom: it keeps its
  dependency and takes a cycle.
- **Compare-and-branch fusion is broader than compare.** `cmp`, `adds`, `subs`, `ands`, `tst`
  followed by `b.cond`, `add`/`and` followed by `cbz`, and `adrp`+`add` all run measurably faster
  adjacent than separated by one instruction, on both core types; two pairs that cannot fuse come
  out at 1.00 to 1.03 in the same test. `aese`+`aesmc` runs as one 2.2-cycle operation (4.3 when
  a NOP separates them).
- **ALU throughput on the P-core depends on what the instructions read.** A stream of
  `add xN, xM, #1` sustains 7.7 per cycle (7.9 when diluted with NOPs: 8 units), two-register
  adds 6.9, and `add xN, x19, x19`, where every instruction reads the same register twice, only
  3.2. The E-core runs all of them at exactly 4.
- **FP add has a fractional latency**: 2.12 cycles on the P-core and 2.50 on the E-core in a
  dependency chain (M1: 3). Vector integer adds are exactly 2 on both, FP multiply 3.00 on the
  P-core and 3.50 on the E-core.
- **Crossing between the integer and FP register files is slow**: `fmov d, x` followed by
  `fmov x, d` takes 10 cycles on the P-core and 8 on the E-core.
- Integer divide takes 7 cycles whatever the operands; the P-core starts one every two cycles,
  the E-core one every seven. FP divide (double) takes 9 cycles at one per cycle on the P-core.
  `pacga` takes 7.

All of this is in the [results](results/apple-m5/) with run-to-run ranges, and every structure
number links to the curve it was read from on the results site.

## Measure your own Mac

```sh
git clone https://github.com/useless-husband/m5-uarch && cd m5-uarch
make measure
```

Requirements: an Apple Silicon Mac, the Xcode command line tools (`cc`, `make`) and `python3`.
No `sudo`. It takes about a minute, needs roughly 300 MB of memory for the cache-miss
experiments, and writes `results/<chip>/<chip>.json` plus two CSV files.
[CONTRIBUTING.md](CONTRIBUTING.md) explains how to send the results.

What a run looks like:

```console
$ build/uarch info
chip        Apple M5 (Mac17,2)
os          macOS 27.0 (26A428)
page size   16384 bytes
counters    thread_selfcounts
level 0     P: "Super", 4 cores, L1I 192 KiB, L1D 128 KiB, L2 16384 KiB shared by 4
level 1     E: "Efficiency", 6 cores, L1I 128 KiB, L1D 64 KiB, L2 6144 KiB shared by 6
pauth       keys inactive in this process: pac*/aut* pass their operand through
reach P     yes (confirmed by the per-level counters)
reach E     yes (confirmed by the per-level counters)

$ build/uarch selftest
P-core  ok   cmp + csinc round trip = 2.001 cycles (want 2: two one-cycle operations)
P-core  ok   add_x_reg    latency = 1.000 cycles, 7 clean runs, instructions per iteration as generated (a 64-bit add takes one cycle)
...
E-core  ok   sub_x_reg    latency = 0.999 cycles, 7 clean runs, instructions per iteration as generated (a 64-bit sub takes one cycle)
784 runs, 0 discarded for migration, 9 discarded for interrupts
selftest passed

$ build/uarch insn -l P -f ldr_x_idx
ldr_x_idx                ldr x0, [x27, x20]                       tp  3.00/c   A0>W0 3.00  R1>W0 4.00
ldr_x_idx_lsl3           ldr x0, [x27, x20, lsl #3]               tp  3.00/c   A0>W0 3.00  R1>W0 4.00

$ build/uarch structure -l P -e spec
  lvp_const_load                    0.34 cycles      (high)  Latency of a load that always returns the same value
  csel_unselected_input             0.34 cycles     [0.34 .. 0.61]  (high)  add + csel chained through the input csel does not select
  csel_unselected_after_flip        2.00 cycles     [2.00 .. 2.00]  (high)  The same chain after one opposite outcome
  csinc_unselected_input            2.00 cycles     [2.00 .. 2.00]  (high)  The same chain with csinc (control)
  ...
```

`tp` is instances per cycle; `A0>W0 3.00` means three cycles from the address operand to the
loaded value. The experiments are `width`, `window`, `elim`, `fusion`, `branch`, `cache`, `tlb`
and `spec` (`uarch structure -e help`).

## How it works

```
 insns/*.def ──► tools/gen_insns.py ──► assembly text ──► system assembler ──► words ─┐
 (templates)     one throughput block and one latency chain per input→output path     │
                                                                                      ▼
   src/enc.h (small encoder, cross-checked against the assembler) ──► JIT loop in a MAP_JIT page
                                                                                      │
   thread_selfcounts(): cycles and instructions per core type ◄── run at n and 2n ◄───┘
                        │
                        ▼
   keep a run only if (a) every cycle was counted on the requested core type and
                      (b) the instruction count is exactly what the loop should retire
                        │
                        ▼
   median of the clean runs ──► JSON per run ──► tools/uarch_results.py: merge N runs
                                                 (median, min, max) ──► JSON, CSV, site
```

- **Counters.** `thread_selfcounts()` (exported by libsystem_kernel, no header) returns cycles and
  retired instructions for the calling thread, one pair per performance level. QoS classes steer
  the thread to P- or E-cores, but QoS is a request, not a pin, so each run is accepted only if
  the counters of the other core type did not move.
- **Clean runs.** The loop retires a number of instructions the generator knows exactly. Kernel
  work on the thread's behalf (an interrupt, a preemption) can only add instructions, so a run
  whose count exceeds the smallest one seen is discarded. On this shared machine that removed
  about 3 % of the runs; what remains repeats to three or four digits.
- **Two lengths.** Every quantity is a difference between a loop of n and 2n iterations, and
  between a body of k and 2k (or 3k) copies, which cancels the counter system calls, the loop
  branch and one-per-iteration effects.
- **Encodings** of table instructions come from the system assembler at build time; the C code
  only copies words. The few instructions the harness builds with computed operands come from a
  small encoder that the test suite compares, case by case, with the assembler's output.
- **Latency** is measured per input operand with a dependency chain. When the output is in another
  register file than the input (flags, FP), a second instruction closes the chain: `csinc` and
  `cmp` for the flags (one cycle each, which the 2.00-cycle round trip proves), `fmov` for FP.
  The `fmov` share cannot be separated by timing, so those latencies are published as round trips
  and marked `rt`.
- **Structure sizes** use Henry Wong's method: two cache misses separated by a growing number of
  filler instructions overlap until the fillers exhaust some structure. See
  [docs/DESIGN.md](docs/DESIGN.md) for each experiment and for what had to be changed to make
  them work on this core.

## Validation

- **Anchors.** `uarch selftest` and `tools/uarch_results.py check` require what must be true on any
  AArch64 core: add, sub and eor take 1 cycle, a compare feeding a `csinc` takes 2, no single
  instruction exceeds the pipeline width. `make measure` stops before measuring if they fail.
- **Exact instruction counts.** Every measurement checks that the counted instructions per
  iteration equal the number of instructions generated.
- **Controls inside the experiments.** The fusion test includes pairs that cannot fuse (result:
  1.00 to 1.03). The speculation tests pair each effect with a control (`csinc`, a shuffled ring,
  indices instead of pointers). Cache sizes are compared with what the OS reports (L1D 128 and 64
  KiB measured and reported; L2 16 MiB measured and reported on the P-cores).
- **Repeatability.** Five runs, 998 000 timed loops; 3.0 % were discarded for interrupts and 70
  (0.007 %) for migrating between core types. Across the runs the median spread of a latency is
  0.05 % on the P-core and 0.12 % on the E-core (99th percentile: 1.1 % and 1.6 %). E-core
  throughput is as stable (99th percentile 4 %). P-core throughput is not: one figure in ten moves
  by more than 6 % between runs, because integer throughput there depends on scheduler balancing
  (see above). Every published number carries its own minimum and maximum.
- **Against published figures.** Where the M4 or M1 were measured by others with PMU counters, the
  M5 numbers are plausible successors or identical (table above: TLB sizes, L1 size and latency,
  unit counts, 32-bit register sharing). Where they differ sharply (E-core width and window, FP
  add latency) the difference reproduces in every run.

## Limitations

- Only cycles and retired instructions are observable. There are no micro-op counts and no port
  assignments; unit counts are inferred from sustained rates, fusion from rates under pressure.
- Latencies that cross register files are round trips (`rt`), not single-instruction numbers.
- A non-integer latency is an average over a dependency chain, which is how the core really
  behaved, but a different instruction mix may see a different average.
- "Reorder buffer in ordinary instructions" is the capacity for one particular blend (integer
  adds, FP adds, compares, branches and stores in proportion to their own limits). Apple's reorder
  buffer packs several instructions per entry, so there is no single size; the NOP figure is the
  upper bound, the blend a realistic one. On the E-core the blend result varies between runs.
- "Loads in flight" on the P-core (486) is what FP loads reach before something stops them; it is
  larger than expected and may not be the load queue itself.
- The misprediction penalty assumes that half of the branches on random bits are mispredicted;
  the rate itself cannot be read.
- The measuring process is plain arm64, in which macOS leaves the pointer-authentication keys
  disabled, so `pacia` and friends are measured as the moves they behave as (`uarch info` reports
  this). `pacga` and `xpac*` are real.
- Instructions whose timing depends on operand values were measured for the values stated in each
  entry only.
- The data was collected on a machine shared with other busy jobs (load average about 2). The run
  filter removes what interrupts touch, not cache or memory contention, which is why the
  memory-based experiments repeat their searches, report a confidence, and are the least
  repeatable part: L2 size, memory latency, the E-core TLB sizes and the prefetch test are marked
  low confidence.
- The P-core `csel` and not-taken-branch figures depend on code placement in ways these tests do
  not fully explain; they are reported as ranges.
- SME/SME2 and MTE instructions are not covered. They were a stretch goal and were left out
  rather than published without the same scrutiny as the rest.

## Related work

- **Dougall Johnson, [Apple M1 Firestorm/Icestorm tables](https://dougallj.github.io/applecpu/firestorm.html)**
  (2021–2023): the model for this project; M1 only, measured with kernel PMU access.
- **Jiajie Chen, [Apple M4 microarchitecture](https://jia.je/hardware/2025/05/21/apple-m4/)** and
  [cpu-micro-benchmarks](https://github.com/jiegec/cpu-micro-benchmarks): structure sizes for M1
  to M4; its macOS path uses the private `kpc` interface and needs root. No instruction tables.
- **Fabian Sidler, [Benchmarking M-series Apple CPUs](https://acl.inf.ethz.ch/teaching/fastcode/2025/benchmarking_m_series_apple_cpus.pdf)**
  (ETH Zürich semester thesis, 2025): instruction latency and throughput on M1 Pro and M3 Max
  using kernel functions.
- **[ocxtal/insn_bench_aarch64](https://github.com/ocxtal/insn_bench_aarch64)**: latency tables
  from timing; published results for M1.
- **uops.info** (Abel and Reineke): the methodology for x86, including automatic chain
  construction, which this tool follows where AArch64 allows.
- **Henry Wong, "Measuring Reorder Buffer Capacity"** (2013): the two-miss method.
- **Augury / GoFetch** (data-memory-dependent prefetcher) and **SLAP / FLOP** (load address and
  load value prediction on Apple CPUs): the security literature that documents the speculation
  this tool had to work around.
- **LLVM** defines `apple-m5` with `CycloneModel`
  ([commit f85494f](https://github.com/llvm/llvm-project/commit/f85494f6afeb)).

What is different here: it covers the M5, both core types; it needs no privileges; and it
publishes the run-to-run spread of every number.

## Build and test

```sh
make            # build/uarch
make test       # unit, property, encoder cross-check, functional and smoke tests
make lint       # warnings as errors, clang static analyser, Python byte-compile
make bench      # selftest plus every structure experiment (about ten seconds)
make measure    # three full runs, merged into results/<chip>/
make site       # regenerate site/data.js from results/
```

Tests that need the counters print why and skip on machines without them (virtual machines,
including GitHub's macOS runners); the rest of the suite runs there.

## Licence

MIT. See [LICENSE](LICENSE).
