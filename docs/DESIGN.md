# Design

This document explains how m5-uarch is put together, which problems turned out to be hard, how
they were solved, and what was tried and rejected. The numbers quoted are from the Apple M5
(Mac17,2); the reasoning applies to any Apple Silicon Mac.

## Constraints

1. **No privileges.** An ordinary process on macOS cannot program the performance monitoring
   unit; the `kpc` interface that other tools use needs root. What the kernel does give every
   process is its own thread's cycle and retired-instruction counts.
2. **Two core types.** The scheduler decides where a thread runs. A measurement is useless unless
   it is known which kind of core executed it.
3. **A noisy machine.** The data was taken on a Mac shared with other busy jobs. Interrupts,
   preemption, migration, clock changes and memory traffic from other cores all happen.
4. **A core that speculates on data.** Recent Apple cores predict load values, prefetch through
   pointers and shortcut some selects. Each of these breaks a classic measurement recipe.

## Architecture

```
insns/*.def        instruction templates (text)
tools/gen_insns.py templates -> assembly text + C table      build time
cc (assembler)     assembly text -> machine words             build time
src/insn.c         copies words into a JIT loop, measures
src/enc.h          tiny encoder for run-time-computed code (loop control, experiments)
src/jit.c, tramp.S MAP_JIT arena; trampoline that loads and stores every register
src/counters.c     thread_selfcounts / proc_pidinfo / proc_pid_rusage
src/measure.c      two-point measurement, clean-run filter, fault guard
src/exp_*.c        structure experiments
src/report.c       JSON
tools/uarch_results.py   merge runs, check anchors, CSV, site data
site/              static viewer
```

The C code never encodes a table instruction. A template such as

```
madd_x | madd {W0:x}, {R0:x}, {R1:x}, {R2:x}
```

is expanded by the generator into a throughput block (18 instances with distinct destinations)
and one latency chain per input (`madd x0, x0, x20, x21`, `madd x0, x19, x0, x21`, ...), written
out as assembly, assembled by the system assembler into a read-only data section, and linked into
the tool. Marker words between the blocks let the tool verify at start-up that the layout is what
the generator assumed. At run time the blocks are copied into the JIT arena between a loop head
and `sub x28, x28, #1 ; cbnz x28, top`.

## Counters

`thread_selfcounts(2, buf, size)` fills one `{instructions, cycles}` pair per performance level
for the calling thread. It is exported by libsystem_kernel without a header, so it is looked up
with `dlsym`; if it is missing or returns counters that do not advance, the tool falls back to
`proc_pidinfo(PROC_PIDTHREADCOUNTS)` and then to `proc_pid_rusage(RUSAGE_INFO_V6)`, and if
nothing works it says so and exits with status 77. A pair of reads costs about 1800 instructions
and 600 cycles, which is why nothing is ever measured as a single difference.

The per-level split is what makes core-type verification possible. QoS class "user interactive"
gets the thread onto a P-core and "background" onto an E-core almost always, but it is a request.
The tool waits until the counters show the thread on the requested level, and then accepts a run
only if the other level's counters did not move during it. In the published data 70 of 998 000
runs were rejected for this.

## Measuring one loop

A measurement is a series of paired runs of the same code at n and 2n iterations.

- **The difference cancels fixed costs**: counter system calls, trampoline, loop entry and exit.
- **A run is clean only if its instruction count is the smallest seen for that n.** The generator
  knows exactly how many instructions one iteration retires, and the kernel charges the work it
  does on a thread's behalf (interrupt handlers, context switches) to that thread. So disturbance
  shows up as extra instructions: a few thousand per interrupt. The first implementation used the
  *most common* count as the reference; on a busy machine the most common run can be one with an
  interrupt in it, which produced wrong results that looked clean. The minimum cannot be fooled
  that way, because kernel work only ever adds.
- The instructions per iteration derived from the two lengths must equal the generator's count,
  otherwise the measurement is rejected (`bad-count`). This catches generator bugs and any
  instruction that does not execute as written.
- The result is the difference of the medians of the clean runs; the difference of the minima and
  the inter-quartile spread are kept next to it.

Generated code runs under a `sigsetjmp` guard, so an instruction the CPU does not implement is
reported as unsupported instead of killing the run.

Throughput and latency both use a second differencing, in the *body length*: k against 2k copies
for throughput, 31 against 93 chain steps for latency. This removes the loop branch, the fetch
bubble after it, and anything else that happens once per iteration.

## Latency chains

Latency from input i to the output is measured by making the output feed input i of the next
copy. Four complications:

**The loop counter must not touch the flags.** The first version used `subs`/`b.ne`. That cut
every chain running through NZCV at the loop edge and produced "latencies" below one cycle.
`sub`/`cbnz` leaves NZCV alone.

**Different register files.** When the output is in the flags or an FP register and the input is
an integer register (or the reverse), a helper closes the chain: `cmp` (integer to flags), `csinc`
(flags to integer), `fmov` (both ways between integer and FP), `fcmp`, `fcsel`. `cmp` + `csinc`
round trip in 2.00 cycles, and two operations that cannot be eliminated cannot take less than a
cycle each, so each is exactly 1 and is subtracted. The `fmov` round trip takes 10 cycles on the
P-core and cannot be split from timing alone, so every latency measured through it is published
as a round trip and marked.

**Value prediction.** `ldr x0, [x0]` on a cell that points to itself is the standard way to
measure load latency. On the M5 P-core it reports 0.34 cycles: the load always returns the same
value, the value is predicted, and consumers stop waiting for the load. The tool instead builds a
random single cycle of 509 nodes in its scratch buffer (Sattolo's algorithm, fixed seed) and
chases that. 509 is prime, so each of the 31 or 93 load sites in the loop sees all 509 values in
turn rather than a short repeating subset. Index-register forms chase a cycle of indices, byte
loads a cycle of 127 byte values.

**Select prediction.** A chain through `csel` measured between 0.3 and 0.9 cycles on the P-core
whenever the condition never changed. Conditional selects are therefore measured with `flip`:
before every counted run the loop executes a few iterations with NZCV inverted, so no select ever
sees a constant condition; and the flag-to-result chain is built so that the condition alternates
from step to step, with an odd number of steps so that it also alternates at each code address.
The helper for flag chains is `csinc`, which shows no such effect.

The shortcut itself is measured separately (`uarch structure -e spec`), at code addresses nothing
else has used, because predictor history is per address and the instruction table has just
deliberately spoiled it.

## Throughput

A block of independent instances, each writing a different register, repeated to at least 320
instances per iteration.

**Shared sources.** With every instance reading the same two source registers, integer throughput
on the P-core came out low and erratic (`add x, x19, x20`: 5.5 per cycle; `add x, x19, x19`:
3.0), while the E-core was exact. The numbers depend on which registers the instructions read,
not on the instruction. In the table, read-only integer operands therefore rotate through eight
registers so that no register is read by every instance. The sensitivity is reported as a finding
(`units_alu_same_src`), and the unit counts come from a different experiment (below).

**Read-write operands.** For instructions such as `fmla` each destination register carries its
own dependency chain across iterations. With c chains and latency L, throughput cannot exceed c/L
whatever the hardware has. When the measured value is within 6 % of that bound the result is
flagged as limited by its own chain.

## Structure experiments

### Width and unit counts

NOPs need no execution unit, so their sustained rate is the pipeline width. For unit counts, a
pure stream of one instruction kind does not reach the number of units on the P-core (adds: 7.7
per cycle), because uops are assigned to schedulers at dispatch and the assignment is not
perfectly balanced. Diluting the stream with NOPs, r instructions per group of W, gives the
balancer slack; the best rate over all r is reported (adds: 7.92, that is 8 units).

### Window sizes (Henry Wong's method)

Two independent pointer chases that miss the cache, separated by F filler instructions. While
both misses fit in the out-of-order window they overlap; when the fillers exhaust some structure,
the second miss cannot start until the first retires. The F at which time per iteration jumps is
the capacity of whatever the fillers consume: NOPs for the reorder buffer, register-writing
instructions for the rename registers, loads, stores, branches for their queues.

Three things had to change for this to work here.

*Indices, not pointers.* With cells holding pointers, no jump appeared at all on the P-core. The
data-memory-dependent prefetcher dereferences pointer-looking values as soon as their cache line
arrives, so the "blocked" second chase found its data already fetched. The chase now reads
`ldr x1, [x26, x1, lsl #3]` and the cells hold indices. The `spec` experiment measures the
difference directly.

*Ratios of alternating runs.* A miss lasts a fixed time, not a fixed number of cycles, and the
E-cluster's clock ramped from 1 to 3 GHz during the experiment. Comparing a loop measured at one
frequency with a baseline measured at another gave knees at random places. Now each point
alternates single runs of the test loop and of a control loop (the same code with the second
chase replaced by cache hits), keeps only pairs in which both runs were clean and ran at the same
frequency, and uses the median ratio. The ratio goes from 1.0 to about 1.7 at the knee.

*Every search twice.* Memory traffic from other cores can spoil a whole search without showing in
any per-run filter. Each knee is located twice; if the two disagree a third search decides and
the confidence drops.

The "reorder buffer in ordinary instructions" uses a blend of fillers in proportion to the
capacities just measured for each kind, so that none of the individual structures fills first.
On the P-core it stops at 1330, well short of the 1731 the individual capacities would allow,
so something shared ran out. It is not a plain entry count: Apple's reorder buffer holds several
instructions per entry (NOPs: 3367), so the number depends on the blend and is labelled that way.

### Moves, zero idioms, fusion

Moves are tested as extra latency inside an add chain; zero idioms by placing the candidate
between the links of a multiply chain (if the dependency is broken, the chain falls apart).

Fusion cannot be seen as a micro-op count. The test builds blocks of "pair + k independent adds"
in two orders: pair adjacent, and pair separated by one of the adds. Same instructions, same
count; if the adjacent form is faster under unit pressure, the pair took one slot instead of two.
Two pairs that cannot fuse are run through the same test as controls and come out at 1.00 to
1.03.

### Branch misprediction

One loop, two data sets: alternating bits (predicted) and random bits (half mispredicted, by
assumption). Both sides of the branch execute the same number of instructions. The bit for the
next iteration is loaded before the current branch, so a flush never delays the next branch's
condition and the measured penalty is the pipeline's own.

### Caches and TLBs

Pointer chases over random single cycles. Cache: one cell per 64 bytes over buffers of 16 KiB to
128 MiB. TLB: the same number of cells laid out once per page and once densely; the difference in
latency is the cost of translation, so cache effects cancel.

## Results pipeline

One `uarch all` run writes a raw JSON file. `tools/uarch_results.py merge` combines several runs
of the same machine into the published file, keeping `[median, min, max]` for every number, so
the run-to-run spread is part of the data rather than a claim in a README. Structure results lose
confidence if the runs disagree by more than 3 % and become `low` beyond 10 %, and are marked
inconclusive if fewer than half the runs produced a value. `check` re-validates the anchors on the
merged file. The file is written one instruction per line so that a re-measurement gives a
readable diff.

## Rejected alternatives

- **Timing with the wall clock.** Frequency scaling makes nanoseconds meaningless for anything
  that is not memory-bound, and the cycle counter exists.
- **A long-latency arithmetic chain instead of cache misses** as the blocker in the window
  experiment (no memory, no noise). The chain's own instructions occupy window and scheduler
  entries, which smears the knee by the chain length and measures the scheduler when the chain
  is long. Cache misses block with a single instruction.
- **Modal instruction count as the clean-run reference.** Wrong on a busy machine (see above).
- **Writing encodings by hand for the table.** A thousand encodings with no independent check;
  the assembler is already installed.
- **Pinning threads.** macOS has no affinity API for P/E selection; QoS plus verification is the
  only rootless option.
- **Reporting rounded latencies.** Rounding 2.12 to 2 would hide that the core alternates; the
  raw average with its run-to-run range is published instead.
- **SME and MTE in this version.** Streaming mode changes the register state the harness relies
  on, and MTE needs a tagged mapping; both deserve their own validation.

## Testing

See `tests/`. The encoder is compared with the assembler on 2800 randomised cases; every table
entry is executed once and must run or raise SIGILL, and pointer chases must stay inside their
cycle; statistics have seeded property tests (one of which found a real bug in the step
detector); the measurement layer is tested against the counters where they exist; the Python
tools have their own tests, including a check that the committed results are canonical and pass
the anchors.
