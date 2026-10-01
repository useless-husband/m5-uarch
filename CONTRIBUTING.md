# Contributing

## Results for another chip

Any Apple Silicon Mac will do; chips other than the M5 are especially welcome.

```sh
make measure          # builds, checks the anchors, runs three times, merges
```

1. Close what you can. The tool copes with a busy machine (it discards disturbed runs), but the
   memory experiments are cleaner on a quiet one. Laptops: plug in.
2. `make measure` writes `results/<chip>/<chip>.json`, `instructions.csv` and `structure.csv`.
   `<chip>` is derived from the CPU brand string, for example `apple-m4-pro`.
3. `make site` regenerates `site/data.js`; open `site/index.html` to look at your numbers.
4. Open a pull request with the `results/<chip>/` directory and `site/data.js`. Say in the
   description what the machine is (model identifier from `build/uarch info`), what else was
   running, and anything that looked wrong.

`tools/uarch_results.py check results/<chip>/<chip>.json` must print `anchors hold`. If it does
not, please open an issue with the output of `build/uarch selftest` instead of a pull request:
that is a bug in the tool, not in your chip.

If `build/uarch info` says `counters none`, the machine exposes no cycle counters to unprivileged
processes (virtual machines do this) and nothing can be measured.

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
point, and its numbers repeat.

## Code

- C11, no dependencies beyond the macOS SDK. `make lint` must pass (warnings are errors, and the
  clang static analyser runs over `src/`).
- Anything that emits machine code at run time goes through `src/enc.h`, and every function there
  needs a case in `tests/enc_gen.c`, which compares it with the system assembler.
- An experiment must report how it arrived at its number (the curve) and must be able to say
  "inconclusive". A plausible-looking number from a broken experiment is the failure mode this
  project exists to avoid.
- Tests that need counters exit with status 77 and a reason when there are none.
  `UARCH_COUNTERS=none make test` runs the suite the way a virtual machine (CI) sees it.
