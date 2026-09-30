/*
 * exp_spec.c - speculation that changes what a latency measurement sees.
 *
 * Three behaviours of recent Apple cores break the textbook assumption that
 * a dependency chain runs at the sum of its latencies.  Each is measured
 * here in the simplest setting that shows it, next to a control that does
 * not show it.
 *
 *   load value    a load that keeps returning the same value is predicted;
 *                 consumers no longer wait for it
 *   select        a csel whose condition never changes behaves like a
 *                 register move
 *   prefetch      data that looks like a pointer is dereferenced by a
 *                 prefetcher before the program asks for it
 *
 * Predictor state is per code address, and earlier tests have run other code
 * at the start of the JIT arena (the instruction table deliberately shows
 * every conditional select both outcomes).  These experiments therefore
 * place their loops at addresses nothing else has used.
 */
#include "enc.h"
#include "exp.h"
#include "stats.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Word offsets into the 8 MiB arena (2^21 words) reserved for this file. */
#define OFF_LVP   (3u << 18)
#define OFF_CSEL  (4u << 18)
#define OFF_DMP_A (6u << 18)
#define OFF_DMP_B (7u << 18)

static size_t g_next_csel = OFF_CSEL;

/* ---- load value prediction -------------------------------------------- */

/* `loads` dependent loads per iteration, walking a ring of `nodes` cells
 * (64 bytes apart) in the scratch buffer.  Returns cycles per iteration. */
static double ring_cycles(unsigned nodes, unsigned loads, int level)
{
    uint8_t *mid = ua_scratch_mid();
    /* Visit the cells in a shuffled order (fixed seed), so that the load
     * addresses have no stride an address predictor could follow: only the
     * number of distinct values matters. */
    static uint16_t order[512];
    uint64_t x = 0x9e3779b97f4a7c15ull;
    if (nodes > 512)
        return NAN;
    for (unsigned k = 0; k < nodes; k++)
        order[k] = (uint16_t)k;
    for (unsigned k = nodes - 1; k > 0; k--) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        unsigned j = (unsigned)(x % (k + 1));
        uint16_t t = order[k];
        order[k] = order[j];
        order[j] = t;
    }
    for (unsigned k = 0; k < nodes; k++) {
        uint64_t next = (uint64_t)(uintptr_t)(mid + 64 * order[(k + 1) % nodes]);
        memcpy(mid + 64 * order[k], &next, 8);
    }
    ua_regs regs;
    ua_regs_default(&regs);
    regs.x[0] = (uint64_t)(uintptr_t)(mid + 64 * order[0]);
    ua_jit_loop_open_at(OFF_LVP, NULL, 0);
    for (unsigned i = 0; i < loads; i++)
        ua_jit_put(a64_ldr(0, 0, 0));
    const void *code = ua_jit_loop_close(NULL, 0);
    ua_mopts o = ua_mopts_default(level);
    o.expect_ins = loads + 2;
    o.carry = 1u; /* x0 keeps walking */
    ua_meas m = ua_measure(code, &regs, &o);
    return m.status == UA_OK ? m.cyc : NAN;
}

/* 32 against 64 load sites: with an odd ring, every site sees every cell in
 * turn in both loops, so the two lengths differ only in chain length. */
static double ring_latency(unsigned nodes, int level)
{
    double a = ring_cycles(nodes, 32, level), b = ring_cycles(nodes, 64, level);
    return isnan(a) || isnan(b) ? NAN : (b - a) / 32.0;
}

static void load_value(int level, ua_exp_list *out)
{
    ua_exp_result *r = ua_exp_add(out, "lvp_const_load",
                                  "Latency of a load that always returns the same value", "cycles",
                                  level);
    snprintf(r->xlabel, sizeof r->xlabel, "distinct values each load returns in turn");
    snprintf(r->ylabel, sizeof r->ylabel, "cycles per load");
    /* Odd ring sizes only (see ring_latency).  1 is the classic self-pointing
     * cell; 509 is far more values than a per-address predictor remembers. */
    static const unsigned sizes[] = {1, 3, 5, 7, 9, 11, 15, 17, 23, 31, 33, 63, 127, 509};
    double one = NAN, many = NAN;
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        double v = ring_latency(sizes[i], level);
        if (isnan(v))
            continue;
        ua_exp_point(r, sizes[i], v);
        if (sizes[i] == 1)
            one = v;
        if (sizes[i] == 509)
            many = v;
    }
    if (isnan(one) || isnan(many))
        return;
    r->status = UA_EXP_OK;
    r->value = one;
    r->lo = r->hi = one;
    snprintf(r->confidence, sizeof r->confidence, "high");
    ua_exp_note(r,
                "ldr x0, [x0] chasing a cell that points to itself takes %.2f cycles per load; "
                "the same load walking a ring of 509 cells takes %.2f. %s",
                one, many,
                one < many - 0.5
                    ? "The constant value is predicted, so the chain no longer waits for the "
                      "load: a self-pointing cell cannot be used to measure load latency here. "
                      "The curve shows how many alternating values are still predicted."
                    : "No value prediction is visible: both agree.");
}

/* ---- conditional select ------------------------------------------------ */

#define SEL_STEPS 20

/*
 * One step:   add x1, x0, #1 ; nop x pad ; <select> x0, x1, x21, ne
 *
 * A chain of add + 1-cycle select takes 2 cycles per step; if the select is
 * free it takes 1.  The padding moves the select to a different position
 * relative to the add and to the rename groups, which turns out to matter.
 */
static const void *build_select(int kind, unsigned pad, size_t offset)
{
    ua_jit_loop_open_at(offset, NULL, 0);
    for (unsigned i = 0; i < SEL_STEPS; i++) {
        ua_jit_put(a64_add_imm(1, 0, 1));
        for (unsigned j = 0; j < pad; j++)
            ua_jit_put(a64_nop());
        switch (kind) {
        case 0:  ua_jit_put(a64_csel(0, 1, 21, A64_NE)); break;
        case 1:  ua_jit_put(a64_csinc(0, 1, 21, A64_NE)); break;
        case 2:  ua_jit_put(a64_mov(0, 1)); break;
        case 3:  ua_jit_put(a64_csel(0, 21, 1, A64_NE)); break;  /* x1 is NOT selected */
        default: ua_jit_put(a64_csinc(0, 21, 1, A64_NE)); break; /* control for 3 */
        }
    }
    return ua_jit_loop_close(NULL, 0);
}

/* Latency of the select (cycles per step minus the add) at an address no
 * other code has used, after 200000 iterations of training.  If `flip` is
 * set, 64 iterations with the opposite condition follow the training. */
static double select_latency(int kind, int flip, unsigned pad, int level)
{
    size_t at = g_next_csel;
    g_next_csel += 1024;
    if (g_next_csel >= OFF_DMP_A)
        return NAN;
    const void *code = build_select(kind, pad, at);
    ua_regs regs;
    ua_regs_default(&regs);
    regs.nzcv = 0; /* Z clear: ne */
    /* Give whatever learns the condition plenty of executions first. */
    if (ua_run_guarded(code, &regs, 200000) != 0)
        return NAN;
    if (flip) {
        regs.nzcv = 0x40000000u; /* Z set: the select takes the other input */
        if (ua_run_guarded(code, &regs, 64) != 0)
            return NAN;
        regs.nzcv = 0;
    }
    ua_mopts o = ua_mopts_default(level);
    o.expect_ins = (uint64_t)SEL_STEPS * (pad + 2) + 2;
    ua_meas m = ua_measure(code, &regs, &o);
    return m.status == UA_OK ? m.cyc / SEL_STEPS - 1.0 : NAN;
}

/* Sweep the padding; report the mean with the smallest and largest value. */
static void select_sweep(ua_exp_list *out, int level, const char *id, const char *title, int kind,
                         int flip, const char *note)
{
    ua_exp_result *r = ua_exp_add(out, id, title, "cycles", level);
    snprintf(r->xlabel, sizeof r->xlabel, "NOPs between the add and the select");
    snprintf(r->ylabel, sizeof r->ylabel, "latency of the select, cycles");
    double lo = INFINITY, hi = -INFINITY, sum = 0;
    int n = 0;
    /* Up to 4 NOPs: the step stays narrower than the narrowest pipeline, so
     * the front end never limits the loop. */
    for (unsigned pad = 0; pad <= 4; pad++) {
        double v = select_latency(kind, flip, pad, level);
        if (isnan(v))
            continue;
        if (fabs(v) < 0.005)
            v = 0;
        ua_exp_point(r, pad, v);
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
        sum += v;
        n++;
    }
    if (n < 3) {
        ua_exp_note(r, "Measurement was not clean.");
        return;
    }
    r->status = UA_EXP_OK;
    r->value = lo;
    r->lo = lo;
    r->hi = hi;
    snprintf(r->confidence, sizeof r->confidence, hi - lo < 0.05 ? "high" : "medium");
    ua_exp_note(r,
                "%s Smallest of five placements of the select; they ranged from %.2f to %.2f "
                "(mean %.2f). 1 is ordinary data flow; less than 1 means that some of the selects "
                "did not cost a cycle.",
                note, lo, hi, sum / n);
}

/* The chain runs through the input that the select does NOT choose.  In
 * data-flow terms that input is still needed (2 cycles per step with the
 * add).  If the select is predicted, the dependency is gone and the loop
 * runs at throughput speed. */
static void select_unselected(ua_exp_list *out, int level, const char *id, const char *title,
                              int kind, int flip, const char *note)
{
    ua_exp_result *r = ua_exp_add(out, id, title, "cycles", level);
    snprintf(r->xlabel, sizeof r->xlabel, "NOPs between the add and the select");
    snprintf(r->ylabel, sizeof r->ylabel, "cycles per step (add + select)");
    double lo = INFINITY, hi = -INFINITY, sum = 0;
    int n = 0;
    for (unsigned pad = 0; pad <= 4; pad++) {
        double v = select_latency(kind, flip, pad, level);
        if (isnan(v))
            continue;
        v += 1.0; /* whole step */
        ua_exp_point(r, pad, v);
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
        sum += v;
        n++;
    }
    if (n < 3) {
        ua_exp_note(r, "Measurement was not clean.");
        return;
    }
    r->status = UA_EXP_OK;
    r->value = lo;
    r->lo = lo;
    r->hi = hi;
    snprintf(r->confidence, sizeof r->confidence, hi < 1.2 || lo > 1.9 ? "high" : "medium");
    ua_exp_note(r,
                "%s Cycles per step, smallest of five placements (%.2f to %.2f, mean %.2f). 2 "
                "means the select waited for the input it did not choose; well below 1 means "
                "that dependency no longer exists and the loop runs at throughput speed.",
                note, lo, hi, sum / n);
}

static void conditional_select(int level, ua_exp_list *out)
{
    select_unselected(out, level, "csel_unselected_input",
                      "add + csel chained through the input csel does not select", 3, 0,
                      "add x1, x0, #1 ; csel x0, x21, x1, ne with the flags never written, so "
                      "x21 is always chosen.");
    select_unselected(out, level, "csel_unselected_after_flip",
                      "The same chain after one opposite outcome", 3, 1,
                      "Identical code at new addresses, with 64 iterations of the opposite condition "
                      "between training and measurement.");
    select_unselected(out, level, "csinc_unselected_input",
                      "The same chain with csinc (control)", 4, 0,
                      "add x1, x0, #1 ; csinc x0, x21, x1, ne.");

    select_sweep(out, level, "csel_const_cond",
                 "Latency of csel when its condition has never changed", 0, 0,
                 "Chain of add x1, x0, #1 and csel x0, x1, x21, ne with the flags never written.");
    select_sweep(out, level, "csel_after_flip",
                 "Latency of the same csel after it has seen the other outcome once", 0, 1,
                 "Identical code at new addresses; after the same training it runs 64 iterations "
                 "with the opposite condition, then is measured with the usual one.");
    select_sweep(out, level, "csinc_const_cond",
                 "Latency of csinc under the same conditions (control)", 1, 0,
                 "csinc cannot be replaced by a move.");
    select_sweep(out, level, "mov_same_shape",
                 "Latency of mov x0, x1 in the same loops (reference)", 2, 0,
                 "What a fully eliminated move looks like in this loop.");
}

/* ---- data-memory-dependent prefetch ------------------------------------ */

#define NODE 128u
#define FILL 1000u /* integer adds: far more than fit in the window */

typedef struct {
    int pointers; /* chase raw pointers instead of indices */
    int control;  /* second chain hits the L1 cache        */
} dmp_ctx;

static void emit_dmp(const dmp_ctx *c)
{
    for (int d = 0; d < 2; d++)
        ua_jit_put(c->pointers ? a64_ldr(1, 1, 0) : a64_ldr_idx3(1, UA_REG_BIG, 1));
    for (unsigned j = 0; j < FILL; j++)
        ua_jit_put(a64_add(3 + j % 8, 20, 21));
    for (int d = 0; d < 2; d++) {
        if (c->control)
            ua_jit_put(a64_ldr(2, UA_REG_MEM, 256));
        else
            ua_jit_put(c->pointers ? a64_ldr(2, 2, 0) : a64_ldr_idx3(2, UA_REG_BIG, 2));
    }
    for (unsigned j = 0; j < FILL; j++)
        ua_jit_put(a64_add(3 + j % 8, 20, 21));
}

/* Serialised two-miss loop against its one-miss control (see exp_window.c),
 * chasing either indices or pointers.  Returns the time ratio. */
static double dmp_ratio(int pointers, uint64_t *start_a, uint64_t *start_b, int level)
{
    dmp_ctx test = {pointers, 0}, control = {pointers, 1};
    ua_jit_loop_open_at(OFF_DMP_B, NULL, 0);
    emit_dmp(&control);
    const void *code_b = ua_jit_loop_close(NULL, 0);
    ua_jit_loop_open_at(OFF_DMP_A, NULL, 0);
    emit_dmp(&test);
    const void *code_a = ua_jit_loop_close(NULL, 0);
    ua_regs ra, rb;
    ua_regs_default(&ra);
    ra.x[UA_REG_BIG] = (uint64_t)(uintptr_t)ua_big_buffer();
    ra.x[1] = *start_a;
    ra.x[2] = *start_b;
    rb = ra;
    double v[3];
    int n = 0;
    for (int attempt = 0; attempt < 5 && n < 3; attempt++) {
        ua_pair p = ua_exp_pair(code_a, &ra, 1u << 2, code_b, &rb, 0, 1u << 1, 300, level, 7, 32);
        if (p.ok && p.ratio > 0.8 && p.ratio < 2.4)
            v[n++] = p.ratio;
    }
    *start_a = ra.x[1];
    *start_b = ra.x[2];
    return n ? ua_median(v, (size_t)n) : NAN;
}

static void prefetch(int level, ua_exp_list *out)
{
    ua_exp_result *r = ua_exp_add(out, "dmp_pointer_chase",
                                  "Two serialised cache misses when the data are pointers",
                                  "x one miss", level);
    uint8_t *buf = ua_big_buffer();
    if (!buf) {
        ua_exp_note(r, "Could not map the buffer needed for cache misses.");
        return;
    }
    size_t nodes = UA_BIG_BYTES / NODE;
    ua_freq_settle(300);

    /* Indices in the first word of every node. */
    uint64_t ia, ib;
    ia = ua_build_chase(buf, nodes, NODE, 0, 0, 0x13198a2e03707344ull, &ib);
    double idx = ia == UINT64_MAX ? NAN : dmp_ratio(0, &ia, &ib, level);

    /* The same layout holding absolute pointers. */
    uint64_t pa, pb;
    pa = ua_build_chase(buf, nodes, NODE, 0, 0, 0xa4093822299f31d0ull, &pb);
    double ptr = NAN;
    if (pa != UINT64_MAX) {
        pb = (uint64_t)(uintptr_t)buf + pb * 8;
        pa = ua_chase_to_pointers(buf, nodes, NODE, 0, 0, pa);
        ptr = dmp_ratio(1, &pa, &pb, level);
    }
    if (isnan(idx) || isnan(ptr)) {
        ua_exp_note(r, "Measurement was not clean.");
        return;
    }
    r->status = UA_EXP_OK;
    r->value = ptr;
    r->lo = ptr < idx ? ptr : idx;
    r->hi = ptr < idx ? idx : ptr;
    snprintf(r->confidence, sizeof r->confidence, idx > 1.4 ? "medium" : "low");
    ua_exp_note(r,
                "Two independent chases, each missing the cache, with %u adds between them so "
                "that the second cannot enter the window until the first retires. Relative to a "
                "loop with one chase this costs %.2fx when the cells hold indices and %.2fx when "
                "they hold pointers. %s",
                FILL, idx, ptr,
                idx - ptr > 0.2
                    ? "The pointers are being dereferenced ahead of the program: a "
                      "data-memory-dependent prefetcher is at work, and it hides the second miss."
                    : "No difference: nothing prefetches through the pointers here.");
}

void ua_exp_spec(int level, ua_exp_list *out)
{
    load_value(level, out);
    conditional_select(level, out);
    prefetch(level, out);
}
