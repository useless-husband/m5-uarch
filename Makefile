# m5-uarch - build, test and measure.
#
#   make            build build/uarch
#   make test       unit, encoder cross-check and smoke tests
#   make lint       strict warnings as errors + clang static analyser
#   make measure    measure this machine; results in results/local/<chip>/ (untracked)
#   make submit     pack the measurement for a GitHub issue (measures first if needed)
#   make submit-pr  the same, as files under results/<chip>/ for a pull request
#   make validate   re-check every committed dataset (what the bots check)
#   make site       build site/data.js and site/data/ from results/
#   make bench      the README's headline numbers, straight from the tool

CC      ?= cc
PYTHON  ?= python3
CFLAGS  ?= -O2 -g
WARN    := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
           -Wmissing-prototypes -Wconversion -Wno-sign-conversion
BUILD   := build

DEFS    := $(sort $(wildcard insns/*.def))
LIB_SRC := src/counters.c src/jit.c src/stats.c src/measure.c src/sysinfo.c src/insn.c \
           src/json.c src/report.c src/exp.c src/exp_common.c src/exp_width.c \
           src/exp_window.c src/exp_elim.c src/exp_spec.c src/exp_branch.c src/exp_cache.c src/exp_fusion.c
LIB_OBJ := $(LIB_SRC:src/%.c=$(BUILD)/%.o) $(BUILD)/tramp.o
GEN_OBJ := $(BUILD)/insns_gen.o $(BUILD)/insns_code.o
APP_SRC := src/main.c
APP_OBJ := $(APP_SRC:src/%.c=$(BUILD)/%.o)
HDRS    := $(wildcard src/*.h)

.PHONY: all test lint measure submit submit-pr validate site bench clean
all: $(BUILD)/uarch

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -Isrc -c $< -o $@

$(BUILD)/tramp.o: src/tramp.S | $(BUILD)
	$(CC) -c $< -o $@

# The instruction table: text in, assembler-encoded words out.
# (GNU Make 3.81 has no grouped targets, hence the empty second rule.)
$(BUILD)/insns_code.S: $(DEFS) tools/gen_insns.py | $(BUILD)
	$(PYTHON) tools/gen_insns.py $(DEFS) --asm $(BUILD)/insns_code.S --c $(BUILD)/insns_gen.c
$(BUILD)/insns_gen.c: $(BUILD)/insns_code.S ;

# Every extension that appears in insns/*.def must be enabled for the
# assembler; whether the CPU implements it is decided at run time.
ASM_MARCH ?= armv9.2-a+sme2+sme-i16i64+sme-f64f64+crypto+sha3+sm4+memtag+bf16+i8mm+fp16+cssc+hbc

$(BUILD)/insns_code.o: $(BUILD)/insns_code.S
	$(CC) -march=$(ASM_MARCH) -c $< -o $@

$(BUILD)/insns_gen.o: $(BUILD)/insns_gen.c $(HDRS)
	$(CC) $(CFLAGS) $(WARN) -Isrc -c $< -o $@

$(BUILD)/uarch: $(APP_OBJ) $(LIB_OBJ) $(GEN_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

# ---- tests ---------------------------------------------------------------
TESTS := $(BUILD)/test_stats $(BUILD)/test_json $(BUILD)/test_enc $(BUILD)/test_jit \
         $(BUILD)/test_measure

$(BUILD)/test_stats: tests/test_stats.c tests/check.h $(BUILD)/stats.o
	$(CC) $(CFLAGS) $(WARN) -Isrc -o $@ $< $(BUILD)/stats.o

$(BUILD)/test_json: tests/test_json.c tests/check.h $(BUILD)/json.o
	$(CC) $(CFLAGS) $(WARN) -Isrc -o $@ $< $(BUILD)/json.o

# Encoder cross-check: enc_gen prints each encoder's output next to the same
# instruction as text; the assembler encodes the text; enc_check compares.
$(BUILD)/enc_gen: tests/enc_gen.c src/enc.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -Isrc -o $@ $<
$(BUILD)/enc_cases.S: $(BUILD)/enc_gen
	$(BUILD)/enc_gen $(BUILD)/enc_cases.S $(BUILD)/enc_cases.inc
$(BUILD)/test_enc: tests/enc_check.c tests/check.h $(BUILD)/enc_cases.S
	$(CC) $(CFLAGS) $(WARN) -Isrc -I$(BUILD) -o $@ tests/enc_check.c $(BUILD)/enc_cases.S

$(BUILD)/test_jit: tests/test_jit.c tests/check.h $(LIB_OBJ) $(GEN_OBJ)
	$(CC) $(CFLAGS) $(WARN) -Isrc -o $@ $< $(LIB_OBJ) $(GEN_OBJ)

$(BUILD)/test_measure: tests/test_measure.c tests/check.h $(LIB_OBJ) $(GEN_OBJ)
	$(CC) $(CFLAGS) $(WARN) -Isrc -o $@ $< $(LIB_OBJ) $(GEN_OBJ)

# Exit status 77 means "skipped for a stated reason" (no counters in a VM).
test: all $(TESTS)
	@fail=0; for t in $(TESTS); do \
	    $$t; rc=$$?; \
	    if [ $$rc -eq 77 ]; then echo "$$t: skipped"; \
	    elif [ $$rc -ne 0 ]; then echo "$$t: FAILED ($$rc)"; fail=1; fi; \
	done; \
	UARCH_COUNTERS=none $(BUILD)/test_measure >/dev/null; rc=$$?; \
	if [ $$rc -ne 77 ]; then echo "test_measure without counters: exit $$rc, want 77 (skip)"; fail=1; fi; \
	$(PYTHON) -W error -m unittest -q tests/test_tools.py tests/test_submission.py \
	    tests/test_validate.py tests/test_site.py tests/test_workflows.py || fail=1; \
	PYTHON=$(PYTHON) sh tests/e2e_submission.sh || fail=1; \
	UARCH=$(BUILD)/uarch PYTHON=$(PYTHON) sh tests/smoke.sh; rc=$$?; \
	if [ $$rc -ne 0 ] && [ $$rc -ne 77 ]; then fail=1; fi; \
	if [ $$fail -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; echo "all tests passed"

# ---- lint ----------------------------------------------------------------
lint:
	@mkdir -p $(BUILD)/lint
	$(MAKE) BUILD=$(BUILD)/lint CFLAGS="-O2 -Werror" all $(TESTS:$(BUILD)/%=$(BUILD)/lint/%)
	$(CC) --analyze -Xanalyzer -analyzer-output=text -Isrc $(LIB_SRC) $(APP_SRC) 2>&1 | \
	    (! grep -E "warning|error") || (echo "static analyser reported problems" && exit 1)
	$(PYTHON) -W error -m py_compile tools/*.py tests/*.py
	sh -n tests/e2e_submission.sh .github/scripts/publish-submission.sh

# The README's headline numbers, straight from the tool.
bench: $(BUILD)/uarch
	$(BUILD)/uarch selftest
	$(BUILD)/uarch structure

# ---- measuring and publishing -------------------------------------------
# One command for any Apple Silicon Mac: `make measure`.
CHIP ?= $(shell sysctl -n machdep.cpu.brand_string | tr '[:upper:]' '[:lower:]' | tr -cs 'a-z0-9' '-' | sed 's/-$$//')
RUNS ?= 3
LOCAL := results/local/$(CHIP)
RESULT := $(LOCAL)/$(CHIP).json
SUBMISSION := $(BUILD)/submission

measure: $(BUILD)/uarch
	@$(BUILD)/uarch selftest
	@mkdir -p $(BUILD)/runs && rm -f $(BUILD)/runs/run-*.json
	@for i in $$(seq 1 $(RUNS)); do \
	    echo "run $$i of $(RUNS) ..."; \
	    $(BUILD)/uarch all -q -o $(BUILD)/runs/run-$$i.json || exit $$?; \
	done
	$(PYTHON) tools/uarch_results.py merge $(BUILD)/runs/run-*.json -o $(RESULT)
	$(PYTHON) tools/uarch_results.py check $(RESULT)
	$(PYTHON) tools/uarch_results.py csv $(RESULT) -o $(LOCAL)
	@echo "Results are in $(LOCAL)/ (not tracked by git). 'make site' shows them next to"
	@echo "the published chips; 'make submit' sends them (CONTRIBUTING.md)."

# Reuse the runs in build/runs if they can be submitted as they are, else measure.
submit: $(BUILD)/uarch
	@$(PYTHON) tools/uarch_submit.py usable $(BUILD)/runs || \
	    { echo "Measuring first ($(RUNS) runs, a minute or two) ..."; $(MAKE) --no-print-directory measure; }
	@$(PYTHON) tools/uarch_submit.py pack $(BUILD)/runs/run-*.json -o $(SUBMISSION) --copy \
	    $(if $(NO_OPEN),,--open)

submit-pr: $(BUILD)/uarch
	@$(PYTHON) tools/uarch_submit.py usable $(BUILD)/runs || \
	    { echo "Measuring first ($(RUNS) runs, a minute or two) ..."; $(MAKE) --no-print-directory measure; }
	@$(PYTHON) tools/uarch_submit.py pack $(BUILD)/runs/run-*.json -o $(SUBMISSION) --into results

validate:
	$(PYTHON) tools/uarch_validate.py tree results

site:
	$(PYTHON) tools/uarch_results.py site results -o site --reference reference

clean:
	rm -rf $(BUILD)
