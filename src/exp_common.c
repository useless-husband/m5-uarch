#include "exp.h"

#include "enc.h"
#include "stats.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

ua_exp_result *ua_exp_add(ua_exp_list *l, const char *id, const char *title, const char *unit,
                          int level)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        ua_exp_result **v = realloc(l->r, (size_t)cap * sizeof *v);
        if (!v)
            abort();
        l->r = v;
        l->cap = cap;
    }
    /* One allocation per result: growing the index above must not move the
     * results themselves, because callers hold pointers to earlier ones. */
    ua_exp_result *r = calloc(1, sizeof *r);
    if (!r)
        abort();
    l->r[l->n++] = r;
    snprintf(r->id, sizeof r->id, "%s", id);
    snprintf(r->title, sizeof r->title, "%s", title);
    snprintf(r->unit, sizeof r->unit, "%s", unit);
    snprintf(r->confidence, sizeof r->confidence, "low");
    r->level = level;
    r->status = UA_EXP_FAILED;
    r->value = r->lo = r->hi = NAN;
    return r;
}

void ua_exp_point(ua_exp_result *r, double x, double y)
{
    if (r->n < UA_EXP_MAX_POINTS) {
        r->x[r->n] = x;
        r->y[r->n] = y;
        r->n++;
    }
}

void ua_exp_note(ua_exp_result *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->note, sizeof r->note, fmt, ap);
    va_end(ap);
}

void ua_exp_list_free(ua_exp_list *l)
{
    for (int i = 0; i < l->n; i++)
        free(l->r[i]);
    free(l->r);
    memset(l, 0, sizeof *l);
}

ua_meas ua_exp_measure(ua_emit_fn emit, void *ctx, ua_regs *regs, int level, int straight,
                       const ua_mopts *opts)
{
    ua_mopts o = opts ? *opts : ua_mopts_default(level);
    o.level = level;
    ua_jit_loop_open(NULL, 0);
    size_t start = ua_jit_pos();
    emit(ctx);
    size_t words = ua_jit_pos() - start;
    const void *code = ua_jit_loop_close(NULL, 0);
    if (straight)
        o.expect_ins = words + 2;
    return ua_measure(code, regs, &o);
}

double ua_exp_cycles(ua_emit_fn emit, void *ctx, ua_regs *regs, int level)
{
    ua_meas m = ua_exp_measure(emit, ctx, regs, level, 1, NULL);
    return m.status == UA_OK ? m.cyc : NAN;
}

double ua_exp_rate(ua_emit_fn emit, void *ctx1, void *ctx2, double work, int level)
{
    ua_mopts o = ua_mopts_default(level);
    o.fastest = 1;
    for (int attempt = 0; attempt < UA_TP_ATTEMPTS; attempt++) {
        ua_regs regs;
        ua_regs_default(&regs);
        ua_meas m1 = ua_exp_measure(emit, ctx1, &regs, level, 1, &o);
        ua_regs_default(&regs);
        ua_meas m2 = ua_exp_measure(emit, ctx2, &regs, level, 1, &o);
        if (m1.status == UA_OK && m2.status == UA_OK && ua_loop_cost_plausible(m1.cyc, m2.cyc))
            return m2.cyc > m1.cyc ? work / (m2.cyc - m1.cyc) : NAN;
        if (attempt == UA_TP_ATTEMPTS - 1 && m2.status == UA_OK && m2.cyc > 0)
            return 2.0 * work / m2.cyc; /* the longer loop's own rate (see insn.c) */
    }
    return NAN;
}

static void carry(ua_regs *regs, uint32_t mask)
{
    for (int i = 0; i < 31; i++)
        if (mask >> i & 1)
            regs->x[i] = regs->out_x[i];
}

static void hand_over(ua_regs *to, const ua_regs *from, uint32_t mask)
{
    for (int i = 0; i < 31; i++)
        if (mask >> i & 1)
            to->x[i] = from->out_x[i];
}

ua_pair ua_exp_pair(const void *code_a, ua_regs *regs_a, uint32_t carry_a, const void *code_b,
                    ua_regs *regs_b, uint32_t carry_b, uint32_t share, uint64_t iters, int level,
                    int want_pairs, int max_pairs)
{
    enum { MAXP = 64 };
    static ua_sample sa[MAXP], sb[MAXP];
    static double ratio[MAXP], ca[MAXP], cb[MAXP];
    ua_pair out = {0, NAN, NAN, NAN, 0};
    if (!code_a || !code_b || iters == 0)
        return out;
    if (max_pairs > MAXP)
        max_pairs = MAXP;
    if (want_pairs > max_pairs)
        want_pairs = max_pairs;

    /* Warm up both loops (and advance any chase past cached nodes). */
    ua_sample w = ua_run_counted(code_a, regs_a, iters / 4 + 1);
    if (w.fault_sig)
        return out;
    carry(regs_a, carry_a);
    hand_over(regs_b, regs_a, share);
    w = ua_run_counted(code_b, regs_b, iters / 4 + 1);
    if (w.fault_sig)
        return out;
    carry(regs_b, carry_b);
    hand_over(regs_a, regs_b, share);

    int n = 0;
    while (n < max_pairs) {
        sa[n] = ua_run_counted(code_a, regs_a, iters);
        carry(regs_a, carry_a);
        hand_over(regs_b, regs_a, share);
        sb[n] = ua_run_counted(code_b, regs_b, iters);
        carry(regs_b, carry_b);
        hand_over(regs_a, regs_b, share);
        if (sa[n].fault_sig || sb[n].fault_sig)
            return out;
        n++;
        if (n < want_pairs)
            continue;
        /* Reference instruction counts: the smallest seen on the level. */
        uint64_t ref_a = UINT64_MAX, ref_b = UINT64_MAX;
        for (int i = 0; i < n; i++) {
            if (sa[i].level == level && sa[i].ins < ref_a)
                ref_a = sa[i].ins;
            if (sb[i].level == level && sb[i].ins < ref_b)
                ref_b = sb[i].ins;
        }
        int clean = 0, migrated = 0, disturbed = 0;
        for (int i = 0; i < n; i++) {
            if (sa[i].level != level || sb[i].level != level) {
                migrated++;
                continue;
            }
            if (sa[i].ins - ref_a > 64 || sb[i].ins - ref_b > 64) {
                disturbed++;
                continue;
            }
            if (sa[i].ns <= 0 || sb[i].ns <= 0)
                continue;
            double fa = (double)sa[i].cyc / sa[i].ns, fb = (double)sb[i].cyc / sb[i].ns;
            if (fa < fb * 0.97 || fa > fb * 1.03)
                continue; /* the clock changed between the two runs */
            ca[clean] = (double)sa[i].cyc / (double)iters;
            cb[clean] = (double)sb[i].cyc / (double)iters;
            ratio[clean] = ca[clean] / cb[clean];
            clean++;
        }
        if (clean >= want_pairs || n == max_pairs) {
            ua_measure_account(2 * (uint64_t)clean, 2 * (uint64_t)migrated,
                               2 * (uint64_t)disturbed);
            if (clean < 3)
                return out;
            out.ok = 1;
            out.pairs = clean;
            out.ratio = ua_median(ratio, (size_t)clean);
            out.a_cyc = ua_median(ca, (size_t)clean);
            out.b_cyc = ua_median(cb, (size_t)clean);
            return out;
        }
    }
    return out;
}

uint8_t *ua_big_buffer(void)
{
    static uint8_t *buf;
    static int tried;
    if (!tried) {
        tried = 1;
        void *p = mmap(NULL, UA_BIG_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p != MAP_FAILED) {
            buf = p;
            /* Touch every page now so that no run pays for a page fault. */
            for (size_t i = 0; i < UA_BIG_BYTES; i += 16384)
                buf[i] = 1;
        }
    }
    return buf;
}

uint64_t ua_build_chase(uint8_t *base, size_t nodes, size_t stride, size_t off_step,
                        size_t off_mod, uint64_t seed, uint64_t *halfway)
{
    uint32_t *perm = malloc(nodes * sizeof *perm);
    if (!perm || nodes < 2) {
        free(perm);
        return UINT64_MAX;
    }
    uint64_t x = seed ? seed : 0x9e3779b97f4a7c15ull;
    for (size_t i = 0; i < nodes; i++)
        perm[i] = (uint32_t)i;
    /* Sattolo: a uniformly random permutation with a single cycle. */
    for (size_t i = nodes - 1; i > 0; i--) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        size_t j = (size_t)(x % i);
        uint32_t t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }
    /* Byte offset of node k's cell from `base`; always a multiple of 8. */
#define CELL_OFF(k) ((size_t)(k) * stride + (off_mod ? ((size_t)(k) * off_step) % off_mod : 0))
    for (size_t i = 0; i < nodes; i++) {
        uint64_t next = CELL_OFF(perm[(i + 1) % nodes]) / 8;
        memcpy(base + CELL_OFF(perm[i]), &next, 8);
    }
    uint64_t first = CELL_OFF(perm[0]) / 8;
    if (halfway)
        *halfway = CELL_OFF(perm[nodes / 2]) / 8;
#undef CELL_OFF
    free(perm);
    return first;
}

uint64_t ua_chase_to_pointers(uint8_t *base, size_t nodes, size_t stride, size_t off_step,
                              size_t off_mod, uint64_t first)
{
    for (size_t k = 0; k < nodes; k++) {
        uint8_t *cell = base + k * stride + (off_mod ? (k * off_step) % off_mod : 0);
        uint64_t v;
        memcpy(&v, cell, 8);
        v = (uint64_t)(uintptr_t)base + v * 8;
        memcpy(cell, &v, 8);
    }
    return (uint64_t)(uintptr_t)base + first * 8;
}
