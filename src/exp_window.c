/*
 * exp_window.c - sizes of the out-of-order window's structures.
 *
 * Henry Wong's method.  One loop iteration is
 *
 *      load A  (cache miss, pointer chase)        <- blocks retirement
 *      F filler instructions
 *      load B  (cache miss, independent chase)
 *      F filler instructions
 *
 * While A waits for memory nothing behind it can retire.  If load B fits in
 * the window together with the F fillers, the two misses overlap and an
 * iteration costs one miss.  Once the fillers exhaust some structure (reorder
 * buffer, a physical register file, the load or store queue, ...) B cannot
 * enter until A retires and an iteration costs two misses.  The F at which
 * the time doubles is the capacity of whichever structure the chosen filler
 * consumes first.
 *
 * Each point is measured as a ratio against a control loop in which B hits
 * the L1 cache, so the cost of executing the fillers themselves cancels.
 */
#include "enc.h"
#include "exp.h"
#include "stats.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define NODE_STRIDE 128u

typedef void (*fill_fn)(unsigned j);

static void f_nop(unsigned j)
{
    (void)j;
    ua_jit_put(a64_nop());
}
static void f_int(unsigned j) { ua_jit_put(a64_add(3 + j % 8, 20, 21)); }
static void f_fp(unsigned j) { ua_jit_put(a64_fadd_d(j % 8, 20, 21)); }
static void f_flags(unsigned j)
{
    (void)j;
    ua_jit_put(a64_cmp(20, 21));
}
/* FP loads: they take a load-queue entry but no integer register. */
static void f_load(unsigned j) { ua_jit_put(a64_ldr_d(j % 8, UA_REG_MEM, 8 * (j % 16))); }
static void f_store(unsigned j) { ua_jit_put(a64_str(20, UA_REG_MEM, 8 * (j % 32))); }
static void f_branch(unsigned j)
{
    (void)j;
    ua_jit_put(a64_cbz(20, 1));
}

/* A blend of fillers in proportion to the capacities measured for each
 * kind, so that no single structure other than the reorder buffer fills
 * first.  g_mix[j % MIX_LEN] selects the kind of the j-th filler. */
#define MIX_LEN 64
static unsigned char g_mix[MIX_LEN];

static void f_mix(unsigned j)
{
    switch (g_mix[j % MIX_LEN]) {
    case 0:  ua_jit_put(a64_add(3 + j % 8, 20, 21)); break;
    case 1:  ua_jit_put(a64_fadd_d(j % 8, 20, 21)); break;
    case 2:  ua_jit_put(a64_cmp(20, 21)); break;
    case 3:  ua_jit_put(a64_cbz(20, 1)); break;
    default: ua_jit_put(a64_str(20, UA_REG_MEM, 8 * (j % 32))); break;
    }
}

/* Fill g_mix with kinds in proportion to cap[], spread evenly (largest
 * accumulated deficit first).  Returns the sum of the capacities. */
static double mix_plan(const double cap[5])
{
    double total = 0, credit[5] = {0, 0, 0, 0, 0};
    for (int k = 0; k < 5; k++)
        if (cap[k] > 0)
            total += cap[k];
    if (total <= 0) {
        for (int i = 0; i < MIX_LEN; i++)
            g_mix[i] = (unsigned char)(i & 1);
        return 0;
    }
    for (int i = 0; i < MIX_LEN; i++) {
        int best = 0;
        for (int k = 0; k < 5; k++) {
            if (cap[k] > 0)
                credit[k] += cap[k] / total;
            if (credit[k] > credit[best])
                best = k;
        }
        credit[best] -= 1.0;
        g_mix[i] = (unsigned char)best;
    }
    return total;
}

typedef struct {
    fill_fn fill;
    unsigned f;
    unsigned depth;
    int control;
} win_ctx;

static void emit_window(void *p)
{
    const win_ctx *c = p;
    for (unsigned d = 0; d < c->depth; d++)
        ua_jit_put(a64_ldr_idx3(1, UA_REG_BIG, 1));
    for (unsigned j = 0; j < c->f; j++)
        c->fill(j);
    for (unsigned d = 0; d < c->depth; d++)
        ua_jit_put(c->control ? a64_ldr(2, UA_REG_MEM, 256) : a64_ldr_idx3(2, UA_REG_BIG, 2));
    for (unsigned j = 0; j < c->f; j++)
        c->fill(c->f + j);
}

static uint64_t g_start_a, g_start_b;

static int chase_ready(void)
{
    static int state; /* 0 untried, 1 ok, -1 failed */
    if (state == 0) {
        uint8_t *buf = ua_big_buffer();
        state = -1;
        if (buf) {
            g_start_a = ua_build_chase(buf, UA_BIG_BYTES / NODE_STRIDE, NODE_STRIDE, 0, 0,
                                       0x243f6a8885a308d3ull, &g_start_b);
            if (g_start_a != UINT64_MAX)
                state = 1;
        }
    }
    return state == 1;
}

#define CONTROL_OFFSET (1u << 18) /* words: the control loop's place in the arena */

static const void *build(const win_ctx *c, size_t offset)
{
    ua_jit_loop_open_at(offset, NULL, 0);
    emit_window((void *)(uintptr_t)c);
    return ua_jit_loop_close(NULL, 0);
}

/* Time of the test loop divided by the time of the control loop. */
static double ratio_once(fill_fn fill, unsigned f, unsigned depth, int level)
{
    win_ctx test = {fill, f, depth, 0}, control = {fill, f, depth, 1};
    ua_regs ra, rb;
    ua_regs_default(&ra);
    ra.x[UA_REG_BIG] = (uint64_t)(uintptr_t)ua_big_buffer();
    ra.x[1] = g_start_a;
    ra.x[2] = g_start_b;
    ra.x[20] = 3; /* non-zero: the cbz filler is never taken */
    rb = ra;
    const void *code_b = build(&control, CONTROL_OFFSET);
    const void *code_a = build(&test, 0);
    /* Both loops keep walking from where the last run stopped: restarting
     * would re-read lines that are now cached.  Chain A is advanced by the
     * two loops in turn, chain B only by the test loop. */
    ua_pair p = ua_exp_pair(code_a, &ra, 1u << 2, code_b, &rb, 0, 1u << 1, 600 / depth, level, 5,
                            24);
    g_start_a = ra.x[1];
    g_start_b = ra.x[2];
    return p.ok ? p.ratio : NAN;
}

/* The ratio can only lie between 1 (overlapped) and 2 (serialised).  A
 * value well outside that range means one of the two loops was disturbed
 * in a way the run filter cannot see (memory traffic from other cores), so
 * measure again. */
static double ratio_at(fill_fn fill, unsigned f, unsigned depth, int level)
{
    double v = NAN;
    for (int attempt = 0; attempt < 4; attempt++) {
        v = ratio_once(fill, f, depth, level);
        if (!isnan(v) && v > 0.9 && v < 2.2)
            return v;
    }
    return NAN;
}

typedef struct {
    int found;
    double lo, hi;       /* last overlapped / first serialised filler count */
    double ratio_lo, ratio_hi;
} knee;

/* One complete search: coarse geometric scan, then two rounds of refinement
 * between the last overlapped and the first serialised point.  Points are
 * added to `r` if it is not NULL. */
static knee locate(fill_fn fill, unsigned depth, unsigned fmax, int level, ua_exp_result *r)
{
    knee k = {0, NAN, NAN, NAN, NAN};
    double x[UA_EXP_MAX_POINTS], y[UA_EXP_MAX_POINTS];
    size_t n = 0;
    for (double f = 8; f <= fmax && n < 48; f *= 1.25) {
        double v = ratio_at(fill, (unsigned)f, depth, level);
        if (isnan(v))
            continue;
        x[n] = floor(f);
        y[n] = v;
        n++;
        if (r)
            ua_exp_point(r, floor(f), v);
    }
    ua_step s = ua_find_step(x, y, n, 1.3);
    if (!s.found)
        return k;
    double mid = 0.5 * (s.lo + s.hi);
    double lo = s.last_lo, hi = s.first_hi;
    for (int pass = 0; pass < 2 && hi - lo > 2; pass++) {
        double step = (hi - lo) / 9.0;
        double new_lo = lo, new_hi = hi;
        for (int i = 1; i < 9; i++) {
            double f = floor(lo + step * i);
            if (f <= new_lo || f >= new_hi)
                continue;
            double v = ratio_at(fill, (unsigned)f, depth, level);
            if (isnan(v))
                continue;
            if (r)
                ua_exp_point(r, f, v);
            if (v < mid)
                new_lo = f; /* still overlapped */
            else if (f < new_hi)
                new_hi = f;
        }
        lo = new_lo;
        hi = new_hi;
    }
    k.found = 1;
    k.lo = lo;
    k.hi = hi;
    k.ratio_lo = s.lo;
    k.ratio_hi = s.hi;
    return k;
}

/* Returns the measured capacity, or NaN.  *res receives the result record. */
static double window(ua_exp_list *out, int level, const char *id, const char *title, fill_fn fill,
                     unsigned depth, unsigned fmax, int blocker_counts, const char *what,
                     ua_exp_result **res)
{
    ua_exp_result *r = ua_exp_add(out, id, title, "entries", level);
    if (res)
        *res = r;
    snprintf(r->xlabel, sizeof r->xlabel, "filler instructions between the two misses");
    snprintf(r->ylabel, sizeof r->ylabel, "time relative to overlapped misses");
    if (!chase_ready()) {
        ua_exp_note(r, "Could not map the %llu MiB buffer needed for cache misses.",
                    (unsigned long long)(UA_BIG_BYTES >> 20));
        return NAN;
    }

    /* Search twice; if the two disagree, a third search decides.  Memory
     * traffic from other cores can derail a whole search, and that kind of
     * disturbance does not show in the run filter. */
    knee k[3];
    double v[3];
    int n = 0, found = 0;
    for (int attempt = 0; attempt < 5 && found < 3; attempt++) {
        k[n] = locate(fill, depth, fmax, level, attempt == 0 ? r : NULL);
        if (k[n].found) {
            v[found++] = 0.5 * (k[n].lo + k[n].hi);
            n++;
        }
        if (found == 2 && fabs(v[0] - v[1]) <= 2 + 0.02 * v[0])
            break;
    }
    if (found < 2) {
        r->status = UA_EXP_INCONCLUSIVE;
        ua_exp_note(r, "No reproducible doubling found up to %u fillers: either the structure "
                       "is larger, or the misses could not be told apart on this machine.", fmax);
        return NAN;
    }
    ua_sort(v, (size_t)found);
    /* With three searches, trust the two that lie closest together. */
    double vlo = v[0], vhi = v[found - 1], outlier = NAN;
    if (found == 3) {
        if (v[1] - v[0] <= v[2] - v[1]) {
            vhi = v[1];
            outlier = v[2];
        } else {
            vlo = v[1];
            outlier = v[0];
        }
    }
    double centre = 0.5 * (vlo + vhi);
    int agree = vhi - vlo <= 2 + 0.02 * centre;
    /* Report the search closest to the centre. */
    int best = 0;
    for (int i = 1; i < n; i++)
        if (fabs(0.5 * (k[i].lo + k[i].hi) - centre) < fabs(0.5 * (k[best].lo + k[best].hi) - centre))
            best = i;

    /* Entries in the window when B's first load must enter: the rest of the
     * blocker behind the oldest load, plus the fillers. */
    double extra = blocker_counts ? (double)depth : 0.0;
    r->status = UA_EXP_OK;
    r->lo = vlo - 0.5 + extra;
    r->hi = vhi + 0.5 + extra;
    r->value = floor(0.5 * (k[best].lo + k[best].hi) + extra);
    double contrast = k[best].ratio_hi / k[best].ratio_lo;
    snprintf(r->confidence, sizeof r->confidence,
             !agree ? "low" : (found == 3 || contrast <= 1.5) ? "medium" : "high");
    char tail[96] = "";
    if (found == 3)
        snprintf(tail, sizeof tail, "; a third search gave %.0f and was set aside", outlier + extra);
    ua_exp_note(r,
                "%s Overlap is lost between %.0f and %.0f fillers (time ratio %.2f -> %.2f)%s%s.",
                what, k[best].lo, k[best].hi, k[best].ratio_lo, k[best].ratio_hi,
                blocker_counts ? "; the blocker's own instructions are added to the count" : "",
                tail);
    return r->value;
}

/* Downgrade a result that turned out to be limited by another structure. */
static void not_separable(ua_exp_result *r, const char *why)
{
    char old[sizeof r->note];
    snprintf(old, sizeof old, "%s", r->note);
    r->status = UA_EXP_INCONCLUSIVE;
    snprintf(r->confidence, sizeof r->confidence, "low");
    snprintf(r->note, sizeof r->note, "%s (Measured %.0f.) %s", why, r->value, old);
}

void ua_exp_window(int level, ua_exp_list *out)
{
    ua_exp_result *r;
    /* A miss lasts a fixed time, not a fixed number of cycles, so let the
     * cluster's clock settle first. */
    ua_freq_settle(400);

    window(out, level, "rob_nop", "Reorder buffer capacity in NOPs", f_nop, 4, 9000, 1,
           "NOPs need no register and no scheduler entry, only room in the reorder buffer. "
           "Apple cores pack several such instructions into one reorder-buffer entry, so this "
           "is far more than the number of ordinary instructions that fit.", NULL);
    double cap[5];
    cap[0] = window(out, level, "prf_int", "Integer register renames in flight", f_int, 2, 1200, 1,
                    "Each filler writes an integer register, so each holds a fresh physical "
                    "register until it retires. Add the 31 architectural registers for the "
                    "file's total size.", NULL);
    cap[1] = window(out, level, "prf_fp", "FP/SIMD register renames in flight", f_fp, 2, 1800, 0,
                    "Each filler writes a vector register. Add the 32 architectural registers "
                    "for the file's total size.", NULL);
    cap[2] = window(out, level, "prf_flags", "Flag renames in flight", f_flags, 2, 800, 0,
                    "Each filler (cmp) writes only NZCV.", NULL);
    cap[3] = window(out, level, "branches", "Unresolved-branch capacity", f_branch, 2, 800, 0,
                    "Fillers are never-taken cbz instructions.", NULL);
    cap[4] = window(out, level, "sq", "Store queue", f_store, 2, 600, 0,
                    "Fillers are stores, which hold a store-queue entry until after retirement.",
                    NULL);
    double lq = window(out, level, "lq", "Load queue", f_load, 2, 1000, 1,
                       "Fillers are FP loads that hit the L1 cache; they finish at once but hold "
                       "their load-queue entry until retirement.", &r);
    if (!isnan(lq) && !isnan(cap[1]) && lq > 0.97 * cap[1])
        not_separable(r, "Not smaller than the FP rename capacity, which each of these loads "
                         "also consumes, so the load queue itself was not reached.");

    /* Reorder buffer for ordinary instructions: blend the filler kinds in
     * proportion to their capacities.  If the blend stops well short of the
     * sum of those capacities, something they all share ran out first. */
    for (int k = 0; k < 5; k++)
        if (isnan(cap[k]))
            cap[k] = 0;
    double total = mix_plan(cap);
    if (total > 0) {
        double rob = window(out, level, "rob", "Reorder buffer capacity in ordinary instructions",
                            f_mix, 2, (unsigned)(total * 1.4) + 64, 1,
                            "Fillers are a blend of integer adds, FP adds, compares, never-taken "
                            "branches and stores, in proportion to the capacity measured for "
                            "each, so that none of those structures fills before the others.",
                            &r);
        if (!isnan(rob) && rob > 0.95 * total)
            not_separable(r, "The blend reached the sum of the individual capacities, so the "
                             "reorder buffer is at least this large but was not the limit.");
    }
}
