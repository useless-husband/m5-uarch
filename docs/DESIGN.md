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
src/measure.c      two-point measurement, clean-run filter, steady states, fault guard
src/exp_*.c        structure experiments
src/report.c       JSON
tools/uarch_results.py   merge runs, check anchors, CSV
tools/uarch_submit.py    pack runs into a submission (privacy filter, text armour), read it back
tools/uarch_validate.py  the automatic checks; issue, pull-request and tree modes; bot comment
tools/uarch_site.py      combine all datasets per chip into the site's data files
site/              static viewer (several chips side by side)
.github/workflows/ submission.yml (issues), results-pr.yml (pull requests), ci.yml, pages.yml
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
only if the other level's counters did not move during it. In the published data 660 of
1 094 794 runs were rejected for this.

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

**Select prediction.** A chain through `csel` measured between 0.25 and 0.85 cycles on the P-core
in most runs whenever the condition never changed (`csel_const_cond`). Conditional selects are therefore
measured with `flip`: before every counted run the loop executes a few iterations with NZCV
inverted, so no select ever sees a constant condition; and the flag-to-result chain is built so
that the condition alternates from step to step, with an odd number of steps so that it also
alternates at each code address. The helper for flag chains is `csinc`, which shows no such effect.

The shortcut itself is measured separately (`uarch structure -e spec`), at code addresses nothing
else has used, because predictor history is per address and the instruction table has just
deliberately spoiled it.

## Throughput

A block of independent instances, each writing a different register, repeated to at least 320
instances per iteration.

**Shared sources.** With every instance reading the same source registers, integer throughput on
the P-core came out low and erratic (`add xN, x19, x19`: 3.01 per cycle undiluted, see
`units_alu_same_src`), while the E-core was exact (4.00). The numbers depend on which registers the
instructions read, not on the instruction. In the table, read-only integer operands therefore
rotate through eight registers so that no register is read by every instance. The sensitivity is
reported as a finding (`units_alu_same_src`), and the unit counts come from a different experiment
(below).

**Read-write operands.** For instructions such as `fmla` each destination register carries its
own dependency chain across iterations. With c chains and latency L, throughput cannot exceed c/L
whatever the hardware has. When the measured value is within 6 % of that bound the result is
flagged as limited by its own chain.

### Steady states (a move at 13 per cycle)

A fresh `make measure` on an idle M5 failed its own anchor: `mov x0, #0x123400000000` (one
MOVZ) at 13.06 per cycle and `mov x0, #0x5555555555555555` (one ORR) at 11.19, on a P-core whose
NOPs run at 10. The same instructions had been published at 8.3 and 8.2. Either something in the
core sustains more than NOPs do, or the measurement was wrong. These checks were run on the
P-core with the tool's own counters and loop builder (100 to 400 counted runs per case; the
throughput block as in the table: 324 or 648 copies, 18 destination registers):

| Question | Experiment | Result |
|---|---|---|
| Do the counted instructions match what ran? | instructions of every run, fast and slow | identical in both: the loop's count plus 1965 for the counter calls |
| Does the cycle counter misbehave? | cycles per nanosecond of wall clock, per run | the same in fast and slow runs (2.59 and 2.58 GHz, 2.99 and 2.98, 3.42 and 3.41 in three series); both kinds of run at every clock from 1.4 to 4.5 GHz |
| Does any loop exceed the width? | instructions retired per cycle over whole runs, counter cost removed | NOPs 10.03 (326 instructions in 32.5 cycles, as if the loop's own two took one slot); 64-bit immediate moves either about 8.95 or 9.87 to 9.99, never more; no loop above 10.03 |
| Is it one particular core? | `pthread_cpu_number_np` before and after 400 runs | all on one P-core: 189 fast, 211 slow |
| Warm-up, run order? | up to 64 times a run's length of the same loop before each counted run | fast share unchanged (9 to 26 % at 648 copies); consecutive runs switch at random; longer runs are more often fast (45 % of 4 000-cycle runs, 91 % of 4-million-cycle runs) |
| Unroll, destinations | 36 to 2592 copies; 1 to 26 destination registers | the same two states; the fast share falls from 91 % at 72 copies to 15 to 29 % at 648 and more; destinations make no difference |
| Mixed with other work | 1:1 with NOPs, with `mov x, x`, with `mov x, #0`; with adds | 10.01 per cycle together; with adds 8.98 |
| Other zero-source instructions | MOVZ with shifts 0 to 48, MOVN, ORR bitmask, `mov x, xzr`, `adr`, `adrp` | MOVZ, MOVN, ORR: the same two states; `mov w, #imm` always 8.93; `mov x, #0` and `mov x, x` always 10; `mov x, xzr` 7.70 like an add; `adr`, `adrp` 5.00 |

So the 13 was an artefact, and the width of 10 stands. What is real: a run of 64-bit immediate
moves on the P-core settles in one of two steady states, about 8.9 per cycle (more than the eight
integer ALUs) or the full width, drawn afresh each time the loop is entered. Zeroing moves and
register moves always run at the width, consistent with being handled at rename; 32-bit immediate
moves never do. What selects the state is not visible to these counters. Other loops have states
too: a vector loop (`ssra`) stayed in a slower one for about 200 consecutive runs and then
switched; integer adds have a few 1 to 3 % apart.

The old estimator assumed one state. It subtracted the medians of the runs at n and 2n iterations,
then the results for k and 2k copies; four sets of runs, each free to be in either state. A
k-copy loop in the slow state (36.2 cycles per iteration) against a 2k-copy loop in the fast one
(65.2) gives 324 / 29.0 = 11.2 per cycle, and a mixed n/2n pair amplifies it further (2a − b).
Because the fast share depends on the loop length, the busy machine's five runs converged on 8.2,
which is neither state. Simulated on 300 recorded runs per set, the old estimator gives 6.5 to
17.4 for `mov_x_imm48` from seven pairs and 8.20 from 96. The same mixing put P-core SIMD
operations at 4.1 to 4.9 per cycle on four units, and a post-indexed load at 3.43 on three load
units.

Throughput measurements (the table, width and unit counts) now work like this:

- **One state.** Only the runs in the fastest state that at least three runs reach (within 1 %)
  are used (`ua_fastest_state`), and sampling continues, up to 96 pairs, until both lengths have
  five of them. A lone faster run is not a state.
- **The fixed cost must be plausible.** `2·median(n) − median(2n)` is the cost of a run apart
  from its iterations, about 600 cycles or 1.5 % of a run. Runs at n in a state d slower than
  those at 2n move it by 2d, so it must lie between −1 % and 5 % of a run.
- **The loop's own cost must be plausible.** `2·c(k) − c(2k)` is what the loop branch costs per
  iteration: within 1 % or a cycle below zero and 2 % plus two cycles above. Otherwise the two
  loops were in different states, and both are measured again, up to three times (states that last
  milliseconds).
- **If they keep disagreeing**, the cost per copy depends on the loop length: branch-dense loops,
  where the branch predictors see twice as many sites, and some vector and atomic loops. The result
  is then the longer loop's own rate, loop branch included, a rate the core really sustained,
  marked `~` in the text output. In one full run that was 37 of the 1 848 throughput figures;
  the results file does not mark them.

Simulated on the same recorded runs, this gives 10.03 to 10.07 for `mov_x_imm48`. The fastest
state measures 10.06, not 10.00, because the two loops do not lose quite the same at the loop
edge: the 324 extra copies take 32.2 cycles where 10 per cycle needs 32.4. The anchor allows 6 %.
Re-measured, the P-core SIMD and FP arithmetic in the table that the old data put above four per
cycle is at 4.00 or below (it was above in 30 entries, the most at 4.89), the immediate moves at 10.06, and the share of P-core
throughput figures whose five runs differ by more than 6 % fell from 17.9 % to 2.9 %. Latencies
are measured as before (a predicted fast state there would be a different number, not a better
one); none of them moved by more than 3 %.

Rejected: the minimum instead of the median. From seven pairs one of the four sets often has no
run in the fast state (6.9 to 12.8 per cycle), and runs that change state halfway make even 96
pairs give up to 10.22.

## Structure experiments

### Width and unit counts

NOPs need no execution unit, so their sustained rate is the pipeline width. For unit counts, a
pure stream of one instruction kind does not reach the number of units on the P-core (adds: 7.7
per cycle), because uops are assigned to schedulers at dispatch and the assignment is not
perfectly balanced. Diluting the stream with NOPs, r instructions per group of W, gives the
balancer slack; the best rate over all r is reported (adds: 7.93, that is 8 units). Both rates
come from the fastest steady state (above).

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
E-cluster's clock is not fixed: the median E-core clock of the five published runs ranged from
1.6 to 1.9 GHz (`runs[].ghz_observed` in the results file; 2.0 to 3.0 in the earlier dataset). Comparing a loop measured at one
frequency with a baseline measured at another gave knees at random places. Now each point
alternates single runs of the test loop and of a control loop (the same code with the second
chase replaced by cache hits), keeps only pairs in which both runs were clean and ran at the same
frequency, and uses the median ratio. The ratio goes from 1.0 to about 1.7 at the knee.

*Every search twice.* Memory traffic from other cores can spoil a whole search without showing in
any per-run filter. Each knee is located twice; if the two disagree a third search decides and
the confidence drops.

The "reorder buffer in ordinary instructions" uses a blend of fillers in proportion to the
capacities just measured for each kind, so that none of the individual structures fills first.
On the P-core it stops at 1330 to 1334, well short of the roughly 1730 that the individual capacities
would allow, so something shared ran out. It is not a plain entry count: Apple's reorder buffer
holds several instructions per entry (NOPs: 3367), so the number depends on the blend and is
labelled that way.

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

A published dataset is two files in `results/<chip>/`: the samples file (the submission, below)
and the results file rebuilt from it, with a `source` (issue, pull request or commit). The
committed M5 dataset rebuilds from its samples file byte for byte; a test checks it.

## Open submissions

The goal: anyone with an Apple Silicon Mac and a GitHub account measures their chip, submits it
with one command and a paste, has it checked automatically, and finds it on the site next to the
other chips. Free to run: GitHub Issues, pull requests, Actions and Pages, no server.

### What is sent, and why it fits in an issue

The constraints: the results file is 370 KB (52 KB gzipped); an issue body holds 65 536
characters; a prefilled new-issue URL holds a few KB. So the submission is not the results file.
It is what `merge` consumes, and nothing else:

- every run's value of every number, as integer thousandths (the tool prints three decimals, so
  this is lossless), with the runs of one number side by side in one string,
  `"3584,3601,3550|1013,1013,1012"` (throughput, then each latency path; `:c` and `:r` carry the
  own-chain and round-trip flags, a word such as `noisy` a failed measurement), so that gzip sees
  the repeats;
- per structure experiment, each run's status, value and confidence, plus the note and curve of
  the run that `merge` publishes (the one closest to the median);
- the chip description and each run's conditions (the allowlist in CONTRIBUTING.md);
- two digests: `spec_sha256` of the instruction table and `stats_sha256` of the results text that
  the packer computed.

The instruction text (assembly, chains, groups: 17 KB compressed) is not sent. It follows from
`insns/*.def` at the same tool version, so the receiving side regenerates it with
`gen_insns.py`; the packer refuses runs whose metadata differs from the local definitions (a
stale build), and the validator refuses a `spec_sha256` other than its own. A test checks that
the Python regeneration equals what the C tool wrote into the committed results.

The armoured text is gzip + base64 between `-----BEGIN M5-UARCH SUBMISSION-----` and `-----END`
lines, with a header (format, chip, SHA-256 of the JSON). The M5's five runs come to 48 376
characters, three runs (the default) to about 40 000; the packer refuses more than 60 000, which
leaves room for the rest of the form. The reader finds the block anywhere in the issue body, so
the form's code fence, Windows line ends or indentation do not matter, and it caps decompression
at 4 MB.

Mechanisms considered and rejected:

- **A prefilled issue URL.** Even the compressed data is several times the limit.
- **An attached file.** GitHub stores issue attachments under `user-attachments` URLs. Whether a
  workflow can download them anonymously and reliably is not documented and has changed before,
  the workflow would fetch a URL chosen by the submitter, and none of it can be tested offline.
- **A gist or any other link.** One more account step for the submitter, and the bot would fetch
  third-party content.
- **A web form with a server.** Not free, and a service to keep running.

Pasting needs one text box, the data stays in the issue (public, archived, auditable), and the bot
reads it from the event payload without any network request.

### The checks

`tools/uarch_validate.py` runs the same rules on an issue, on the files of a pull request, and on
every committed dataset (`make validate`, CI). A rule either rejects (the data cannot be right or
cannot be read) or flags (surprising; a person decides). Rejected if anything rejects, flagged if
anything flags, accepted otherwise.

| Rule | Rejects | Flags |
|---|---|---|
| Licence | box not ticked | |
| Format and privacy | unknown fields anywhere, wrong types, strings that are not chip/model/OS patterns, anything that looks like a path, e-mail address, UUID, serial number or `.local` host name | |
| Tool version | version not in the accepted list; instruction-table digest differs | |
| Statistics | statistics rebuilt from the per-run values differ from `stats_sha256` (issue) or from the results file (pull request; the first differing values are named) | |
| Anchors | add, sub, eor not 1 cycle; cmp + csinc not 2; implausible values; a single instruction more than 6 % faster than the width (nothing retires faster than NOPs: such a number mixes steady states) | |
| Consistency | width outside 2–16; a unit count above the width; L1 latency outside 2–8; reorder buffer outside 64–16 384 | misprediction penalty outside 4–40; NOP throughput and the width experiment more than 10 % apart; adds faster than the ALU count; L1D more than ×2 from what macOS reports; P-core smaller than E-core (width, window, ALUs, loads, FP adds, integer renames, by more than 5 %); P-cores clocked below the E-cores |
| Run quality | | more than 25 % of loops disturbed, 5 % migrated, or 20 % of experiments inconclusive |
| Duplicate | same content id or same statistics as a published dataset | |
| Outliers | | more than 0.5 % of the values outside the outlier rule (below) |

**The outlier rule.** For each value (every throughput, latency path, helper round trip and
conclusive structure figure; low-confidence ones are skipped), let c be the median over the
chip's earlier datasets. The value is an outlier if it is further from c than

    max(5 × 1.4826 × MAD, r × |c|, a) + half its own run range + the median half run range of the others

where MAD (the median absolute deviation) needs three earlier datasets, and r, a are 5 % and
0.05 cycles for latencies, 15 % and 0.10 per cycle for throughputs (in the earlier dataset one P-core
throughput figure in five moved by more than 6 % between runs; since tool 0.3.0, which measures
one steady state, one in 35), 15 % for structure figures. The run ranges
count as slack because a value whose own runs disagreed is weak evidence either way.

Calibration on real data: the M5's five runs split into a 2-run and a 3-run group, all ten
splits, each side checked against the other: 32 of 95 062 comparisons fell outside the rule, at
most 7 of about 4 750 values in one comparison (0.15 %; 24 and 3 in the earlier dataset). So honest repeat measurements do leave a
few values outside. A submission is therefore flagged as a whole only when more than 0.5 % of its
values are outliers; below that the values are listed in the comment as a note and marked on the
site, and the verdict is unchanged. A test perturbs every M5 throughput by up to 0.3 %, makes one
latency wrong, and expects "accepted" with exactly that kind of note.

**What this cannot do.** These checks catch mistakes, broken runs and naive forgery (edited
numbers, statistics that do not follow from the samples, physically impossible values). Someone
determined can still fabricate a consistent dataset: run the packer on invented runs that respect
every anchor. No automatic check can tell that apart from a real measurement. Independent
submissions of the same chip are the real defence: the site shows, for every chip, how many
datasets it has and where each came from, and calls a chip verified only when two independent
ones agree.

### Combining datasets on the site

`tools/uarch_site.py` builds the site's data from every dataset. For each value: the median of
the datasets that agree on it, with the range of their medians as the spread and their number;
with a single dataset, the run-to-run range. Which datasets "agree" uses the same outlier rule:
with three or more, each dataset is compared with the median of all of them (one outlier cannot
drag that reference); with two, each with the other, so both are marked when they disagree,
because nothing can tell which is right. A dataset is flagged when more than 0.5 % of its values
are marked; a chip is verified with two accepted datasets, a single submission with one.

The site data is generated when Pages deploys, not committed: a merged submission adds two files
under `results/` and nothing else, so two submissions never conflict. `data.js` holds the chip
list, the datasets and the structure figures (74 KB for the M5); each chip's instruction table is
`data/<chip>.js` (311 KB), loaded by a script tag when the chip is shown, which also works when
the page is opened from disk.

## Threat model

Who can attack: anyone who can open an issue or a pull request. What is at stake: the contents of
`main` and of the site, the repository's workflow token, and the submitters' privacy.

| Threat | Mitigation |
|---|---|
| Script injection through the issue body or title | No `${{ }}` expression appears inside a `run:` script (a test checks every workflow). The validator reads the issue from `$GITHUB_EVENT_PATH`; other values reach scripts through `env:`. |
| Executing submitted content | Submissions are parsed as JSON with the standard library, never evaluated; base64 is strict, decompression and file sizes are capped. |
| "Pwn request": `pull_request_target` running a pull request's code with a write token | `results-pr.yml` checks out `main` only, fetches the pull request's commit as objects, and reads the changed dataset files with `git show`, as data, after their names matched `results/<chip>/<id>[.samples].json`. Nothing from the pull request is built or run there. The pull request's own code runs in `ci.yml` under `pull_request`, with a read-only token. |
| A token that can do too much | Every workflow starts from `permissions: {}` or read-only. The job that parses untrusted input has `contents: read` and no stored credentials. The job that writes only handles files the validator from `main` produced, re-checks their names (`install`), refuses to overwrite, and pushes only `submission/issue-<n>`; it never pushes to `main`, and a person merges. |
| A misleading bot comment (Markdown or HTML from the submission) | The comment is built from fixed text and numbers. Chip and model names appear only after matching strict patterns; any other submitted string is replaced by "(not shown)". A test submits `<img>` markup. |
| Compromised third-party actions | Pinned to full commit SHAs (a test checks); the bots' own code is standard-library Python. |
| Path tricks in pull-request file names | Strict name patterns before any read; files elsewhere under `results/` are flagged for a person. |
| Floods and resource use | Ten-minute timeouts; one run per issue or pull request at a time (`concurrency`, cancel in progress); a rejected submission costs one short run and a comment and creates no branch. A flood of valid-looking submissions would create branches; the maintainer can disable the workflow or lock issues. |
| Forged data | Not preventable by checks (above); independent submissions, sources and counts on the site. |
| Leaking the submitter's identity | Allowlisted fields only, a scan for identifying strings in the packer and the validator, tests that inject host name, user name, serial number, UUID, path and e-mail address. The GitHub account that opens the issue is public, as with any issue. |

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
- **Reporting rounded latencies.** Rounding 2.11 to 2 would hide that the core alternates; the
  raw average with its run-to-run range is published instead.
- **Sending the results file** with a submission. It does not fit in an issue, and the
  statistics in it are only worth something if they can be recomputed, which needs the per-run
  values anyway.
- **Committing the site data.** Every submission would change the same file and conflict with
  the next one; the data is now built when the site is deployed.
- **SME and MTE in this version.** Streaming mode changes the register state the harness relies
  on, and MTE needs a tagged mapping; both deserve their own validation.

## Testing

See `tests/`. The encoder is compared with the assembler on 2800 randomised cases; every table
entry is executed once and must run or raise SIGILL, and pointer chases must stay inside their
cycle; statistics have seeded property tests (one of which found a real bug in the step
detector), the steady-state selection among them, and a regression test on the real counters
checks that the immediate moves never measure above the NOP rate; the measurement layer is tested against the counters where they exist; the Python
tools have their own tests, including a check that the committed results are canonical and pass
the anchors. The submission tools are tested rule by rule (`tests/test_submission.py`,
`tests/test_validate.py`, `tests/test_site.py`): the packer round trip, the privacy filter with
injected identifying fields, damaged and oversized pastes, every check with good, tampered,
broken and inconsistent data, outliers among several fake submissions of one chip, the issue and
pull-request flows and the bot comment. `tests/test_workflows.py` checks the workflow security
rules, and `tests/e2e_submission.sh` runs the workflows' commands on a sample issue without
GitHub: check, publish to a local bare repository with a stand-in `gh`, rebuild the site, reject a
damaged paste, and check a pull request's files through `git show`. The path a virtual machine takes (no counters; simulated with
`UARCH_COUNTERS=none`) is part of the smoke test: every measuring command must exit with status
77 and say why.
