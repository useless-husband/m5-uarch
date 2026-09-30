/*
 * exp.h - core-structure experiments.
 *
 * Each experiment builds its own loops with the small encoder (enc.h),
 * measures them with ua_measure() and reports a number together with the
 * curve it was read from and a confidence note.  Without configurable PMU
 * events everything here is inferred from cycle counts alone.
 */
#ifndef UA_EXP_H
#define UA_EXP_H

#include "measure.h"

#include <stddef.h>

#define UA_EXP_MAX_POINTS 128

typedef enum {
    UA_EXP_OK = 0,
    UA_EXP_INCONCLUSIVE, /* measured, but the data does not decide the question */
    UA_EXP_FAILED,       /* could not be measured (noise, no memory, ...)        */
} ua_exp_status;

typedef struct {
    char id[48];         /* stable key, e.g. "rob_nop"                          */
    char title[96];
    char unit[24];       /* "entries", "cycles", "per cycle", "bytes", ...      */
    int level;
    ua_exp_status status;
    double value;        /* headline number, NaN if none                        */
    double lo, hi;       /* bracket the value was read from, NaN if n/a         */
    char confidence[12]; /* "high", "medium" or "low"                           */
    char note[400];      /* how to read the number, caveats                     */
    char xlabel[40];
    char ylabel[40];
    int n;               /* points on the curve                                 */
    double x[UA_EXP_MAX_POINTS];
    double y[UA_EXP_MAX_POINTS];
} ua_exp_result;

typedef struct {
    ua_exp_result *r;
    int n, cap;
} ua_exp_list;

/* Append a result (zero-initialised apart from the given fields). */
ua_exp_result *ua_exp_add(ua_exp_list *l, const char *id, const char *title, const char *unit,
                          int level);
void ua_exp_point(ua_exp_result *r, double x, double y);
void ua_exp_note(ua_exp_result *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ua_exp_list_free(ua_exp_list *l);

typedef struct {
    const char *name;    /* command-line name                                   */
    const char *summary;
    void (*run)(int level, ua_exp_list *out);
} ua_experiment;

extern const ua_experiment ua_experiments[];
extern const size_t ua_n_experiments;

/* Shared helpers -------------------------------------------------------- */

typedef void (*ua_emit_fn)(void *ctx);

/* Build `init`-less loop around emit(ctx) and measure it.  If `straight` is
 * set the body is straight-line code and the instruction count is checked. */
ua_meas ua_exp_measure(ua_emit_fn emit, void *ctx, ua_regs *regs, int level, int straight,
                       const ua_mopts *opts);

/* Cycles per iteration, or NaN if the measurement was not clean. */
double ua_exp_cycles(ua_emit_fn emit, void *ctx, ua_regs *regs, int level);

/*
 * Compare two loops under identical conditions.  Runs of A and B alternate,
 * so both see the same clock frequency and the same memory traffic, and a
 * pair counts only if both runs were clean (right performance level, no
 * extra instructions) and ran at the same frequency.  The result is the
 * median over the clean pairs of cycles(A) / cycles(B).
 *
 * carry_a / carry_b: registers that continue from the previous run of that
 * loop (see ua_mopts.carry).  share: registers handed from each run to the
 * next run of the *other* loop, for a pointer chase that both loops advance.
 */
typedef struct {
    int ok;
    double ratio;      /* median of per-pair cycles(A)/cycles(B)              */
    double a_cyc;      /* median cycles per iteration of A in clean pairs     */
    double b_cyc;
    int pairs;         /* clean pairs used                                    */
} ua_pair;

ua_pair ua_exp_pair(const void *code_a, ua_regs *regs_a, uint32_t carry_a, const void *code_b,
                    ua_regs *regs_b, uint32_t carry_b, uint32_t share, uint64_t iters, int level,
                    int want_pairs, int max_pairs);

/* The large buffer used for cache-miss experiments: `bytes` of anonymous
 * memory, mapped on first use and touched.  NULL if it cannot be mapped. */
#define UA_BIG_BYTES (256ull * 1024 * 1024)
uint8_t *ua_big_buffer(void);

/*
 * Write a random single cycle over `nodes` cells for a chase of the form
 *
 *     ldr x1, [xBASE, x1, lsl #3]
 *
 * The k-th cell sits at base + k*stride + offset(k), with offset(k) =
 * (k * off_step) % off_mod (off_mod 0: no offset), and holds the *index* of
 * the next cell: its byte offset from `base` divided by 8.
 *
 * Indices, not pointers, on purpose.  Apple's data-memory-dependent
 * prefetcher dereferences values that look like pointers as soon as the line
 * holding them arrives, which would run the chase ahead of the core and hide
 * the very cache misses these experiments rely on.
 *
 * Returns the first index (UINT64_MAX on failure); *halfway, if not NULL,
 * receives the index half a cycle later, the start of an independent walk.
 */
#define UA_REG_BIG 26 /* holds the big buffer's base in chase loops */
uint64_t ua_build_chase(uint8_t *base, size_t nodes, size_t stride, size_t off_step,
                        size_t off_mod, uint64_t seed, uint64_t *halfway);

/* Individual experiments (exp_*.c). */
void ua_exp_width(int level, ua_exp_list *out);
void ua_exp_window(int level, ua_exp_list *out);
void ua_exp_elim(int level, ua_exp_list *out);
void ua_exp_fusion(int level, ua_exp_list *out);
void ua_exp_branch(int level, ua_exp_list *out);
void ua_exp_cache(int level, ua_exp_list *out);
void ua_exp_tlb(int level, ua_exp_list *out);
void ua_exp_spec(int level, ua_exp_list *out);

#endif
