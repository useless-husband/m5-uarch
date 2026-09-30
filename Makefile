# m5-uarch - build, test and measure.
#
#   make            build build/uarch
#   make test       unit, encoder cross-check and smoke tests
#   make lint       strict warnings as errors + clang static analyser
#   make measure    measure this machine and write results/<chip>/
#   make site       regenerate site/ from results/
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
           src/exp_window.c src/exp_elim.c src/exp_stubs.c
LIB_OBJ := $(LIB_SRC:src/%.c=$(BUILD)/%.o) $(BUILD)/tramp.o
GEN_OBJ := $(BUILD)/insns_gen.o $(BUILD)/insns_code.o
APP_SRC := src/main.c
APP_OBJ := $(APP_SRC:src/%.c=$(BUILD)/%.o)
HDRS    := $(wildcard src/*.h)

.PHONY: all test lint measure site bench clean
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

clean:
	rm -rf $(BUILD)
