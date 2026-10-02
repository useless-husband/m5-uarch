# Contributing

[繁體中文](CONTRIBUTING.zh-TW.md)

## Add your Mac's results

Any Apple Silicon Mac will do. Chips other than the M5 are especially welcome, and so is a
second M5: a chip counts as *verified* on the [results site](https://useless-husband.github.io/m5-uarch/)
only when two independent submissions agree.

You need the Xcode command line tools (`xcode-select --install` if `cc` is missing; they include
`make`, `git` and `python3`) and a GitHub account. No `sudo`, and no git knowledge.

### With a GitHub account only

1. Get the code: `git clone https://github.com/useless-husband/m5-uarch`, or on the GitHub page
   click **Code → Download ZIP** and unzip it. Open Terminal in that folder.
2. Quit what you can and, on a laptop, plug in. Then run:

   ```sh
   make submit
   ```

   It builds the tool, measures three times (a minute or two; if you already ran
   `make measure`, it reuses that), packs the result, copies it to the clipboard and opens the
   "new issue" page of this repository in your browser.
3. Click into the **Submission** box and paste (Cmd-V). Tick the licence box. Click **Create**.
4. Within a few minutes a bot answers on the issue: **accepted**, **flagged** or **rejected**,
   with the reason for each check. Accepted and flagged data goes into a pull request; when the
   maintainer merges it, the site shows your chip.

If it was rejected, the comment says why. Fix that, run `make submit` again, then edit the issue
and replace the old block with the new one: the check runs again on every edit.

If the browser did not open, the link is printed in the terminal and the text is in
`build/submission/submission.txt`.

### With a pull request

```sh
make submit-pr
```

does the same measuring and packing, then adds two files under `results/<chip>/` (named after
the data's content: `<id>.json` and `<id>.samples.json`) and prints the git commands for a
branch. Push it to your fork and open a pull request. A bot checks the two files on every push
and comments. Do not edit them by hand: the check recomputes every statistic from the samples
file and rejects any difference.

### What is sent

Only what is needed to recompute and check the results. `build/submission/submission.json`
shows the same data as the pasted block, readably.

| Field | Example | Why it is needed |
|---|---|---|
| `machine.brand` | `Apple M5` | which chip |
| `machine.model` | `Mac17,2` | which Mac; one chip ships in several |
| `machine.os_version`, `os_build` | `27.0`, `26A428` | macOS can change timing |
| `machine.page_size` | `16384` | the TLB experiments depend on it |
| `machine.virtual_machine` | `false` | a virtual machine cannot measure |
| `machine.pauth_keys_active` | `false` | explains the `pac*` figures |
| `machine.levels[]` | `P`, `Super`, 4 cores, L1/L2 sizes | what "P-core" and "E-core" mean on this chip; the cache experiments are checked against it |
| `tool_version`, `spec_sha256` | `0.2.0`, a digest | which tool and which instruction table measured |
| `runs[]` | seconds, counter interface, timed / clean / discarded loops, load average, clock per core type | conditions of each run, for the quality checks |
| `helpers`, `instructions`, `structure` | every run's value of every figure | the measurements; all statistics are recomputed from these |
| `stats_sha256` | a digest | proves the statistics follow from the values |

**Not sent:** host name, user name, serial number, hardware UUID, file paths, memory size, and
the time of the run. The packer copies only the fields listed above and refuses to pack any text
that looks like a path, an e-mail address, a UUID, a serial number or a `.local` host name; the
check on GitHub rejects the same. The issue itself is public and shows your GitHub account, like
any issue.

### What the bot checks

Format and privacy, tool version and instruction table, the statistics recomputed from the
per-run values, the physical anchors (add, sub and eor take 1 cycle; a compare feeding `csinc`
takes 2; nothing runs faster than the core is wide), internal consistency (unit counts within
the width, P-core against E-core, measured against reported cache size), run quality, duplicates,
and, when the chip already has data, a per-value comparison with it. Details and the limits of
what this can prove: [docs/DESIGN.md](docs/DESIGN.md#open-submissions).

### Measuring without submitting

`make measure` writes `results/local/<chip>/` (not tracked by git): the merged JSON and two CSV
files. `make site` then builds the site with your numbers next to the published chips, marked
"local"; open `site/index.html`.

If `build/uarch info` says `counters none`, the machine exposes no cycle counters to unprivileged
processes (virtual machines do this) and nothing can be measured. If `make measure` stops
because an anchor fails, please open an ordinary issue with the output of `build/uarch selftest`:
that is a bug in the tool, not in your chip.

## Instructions

An instruction is one line in `insns/*.def`:

```
name | assembly template | attributes
```

The header of `tools/gen_insns.py` documents the template syntax. After adding an entry:

```sh
make test                       # every entry is executed once; it must run or raise SIGILL
build/uarch insn -f <name>      # look at the numbers on both core types
```

A new entry is acceptable when its latency chains are real dependencies (check the `chain` text
in the JSON), its values do not drift into special cases (zero, infinity, NaN) unless that is the
point, and its numbers repeat. Changing the table changes `spec_sha256`: submissions measured
with the old table are then refused until the accepted tool versions are updated in
`tools/uarch_submit.py`.

## Code

- C11, no dependencies beyond the macOS SDK. `make lint` must pass (warnings are errors, and the
  clang static analyser runs over `src/`).
- Python tools use the standard library only.
- Anything that emits machine code at run time goes through `src/enc.h`, and every function there
  needs a case in `tests/enc_gen.c`, which compares it with the system assembler.
- An experiment must report how it arrived at its number (the curve) and must be able to say
  "inconclusive". A plausible-looking number from a broken experiment is the failure mode this
  project exists to avoid.
- Tests that need counters exit with status 77 and a reason when there are none.
  `UARCH_COUNTERS=none make test` runs the suite the way a virtual machine (CI) sees it.

## For the maintainer

- Labels used by the bots: `submission` (applied by the issue form), `accepted`, `flagged`,
  `rejected`.
- Settings → Actions → General → Workflow permissions: allow GitHub Actions to create pull
  requests. Without it the bot still pushes `submission/issue-<n>` and comments a compare link.
- Pull requests opened by the bot do not trigger other workflows (a GitHub rule for its token);
  the verdict is in the pull request text, and CI runs on `main` after the merge.
- A flagged submission needs a decision: merge it (the site marks its outlying values and
  leaves them out of the chip's figures) or close the pull request with a comment.
- Protect `main` so that nothing lands there without a review; the bots never push to it.
