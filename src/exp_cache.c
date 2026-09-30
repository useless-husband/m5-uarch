/*
 * exp_cache.c - cache capacities, load-to-use latencies and TLB reach.
 *
 * Both experiments chase pointers through a random single cycle, so every
 * load depends on the one before and nothing can be fetched ahead of need.
 *
 * Cache: the cycle covers a buffer of S bytes with one cell per 64 bytes.
 * While the buffer fits in a cache level the latency is that level's
 * load-to-use latency; the size at which it rises is the capacity.
 *
 * TLB: the same number of cells is laid out twice, once densely (64 bytes
 * apart) and once with one cell per page.  Both touch the same number of
 * cache lines, so the difference in latency is the cost of translation, and
 * the page counts at which that difference rises are the TLB capacities.
 *
 * The OS's view (sysctl hw.perflevelN.*) is quoted next to each cache size
 * as a cross-check; it is not used to produce the result.
 */
#include "enc.h"
#include "exp.h"
#include "stats.h"
#include "sysinfo.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LOADS 8u

static void emit_chase(void *ctx)
{
    (void)ctx;
    for (unsigned i = 0; i < LOADS; i++)
        ua_jit_put(a64_ldr(1, 1, 0));
}

typedef struct {
    double cycles; /* per load */
    double ns;     /* per load */
} chase_time;

/* Build a cycle with the given layout in the big buffer, walk it once to
 * bring it into whatever cache will hold it, and time one load. */
static chase_time chase(size_t nodes, size_t stride, size_t off_step, size_t off_mod, int level)
{
    chase_time t = {NAN, NAN};
    uint8_t *buf = ua_big_buffer();
    uint64_t first = ua_build_chase(buf, nodes, stride, off_step, off_mod,
                                    0x9e3779b97f4a7c15ull ^ (nodes * 0x100000001b3ull), NULL);
    if (first == UINT64_MAX)
        return t;
    ua_regs regs;
    ua_regs_default(&regs);
    regs.x[1] = ua_chase_to_pointers(buf, nodes, stride, off_step, off_mod, first);

    ua_jit_loop_open(NULL, 0);
    emit_chase(NULL);
    const void *code = ua_jit_loop_close(NULL, 0);
    /* Two laps (at most ~1.2 million loads) before measuring. */
    size_t warm = 2 * nodes / LOADS + 1;
    if (warm > 150000)
        warm = 150000;
    if (ua_run_guarded(code, &regs, warm) != 0)
        return t;
    regs.x[1] = regs.out_x[1];

    ua_mopts o = ua_mopts_default(level);
    o.expect_ins = LOADS + 2;
    o.carry = 1u << 1;
    o.target_cycles = 250000;
    o.min_clean = 5;
    ua_meas m = ua_measure(code, &regs, &o);
    if (m.status != UA_OK)
        return t;
    /* Memory noise only adds time. */
    t.cycles = m.cyc_min / LOADS;
    t.ns = m.ghz > 0 ? t.cycles / m.ghz : NAN;
    return t;
}

/* First index i >= from with y[i] >= limit and y[i+1] >= limit (or last). */
static size_t first_above(const double *y, size_t n, size_t from, double limit)
{
    for (size_t i = from; i < n; i++)
        if (y[i] >= limit && (i + 1 == n || y[i + 1] >= limit))
            return i;
    return n;
}

static ua_exp_result *simple(ua_exp_list *out, int level, const char *id, const char *title,
                             const char *unit, double value, double lo, double hi,
                             const char *confidence)
{
    ua_exp_result *r = ua_exp_add(out, id, title, unit, level);
    if (isnan(value))
        return r;
    r->status = UA_EXP_OK;
    r->value = value;
    r->lo = lo;
    r->hi = hi;
    snprintf(r->confidence, sizeof r->confidence, "%s", confidence);
    return r;
}

void ua_exp_cache(int level, ua_exp_list *out)
{
    ua_sysinfo si;
    ua_sysinfo_get(&si);
    const ua_syslevel *sys = &si.level[level < si.nlevels ? level : 0];
    if (!ua_big_buffer()) {
        ua_exp_result *r = ua_exp_add(out, "l1d_size", "L1 data cache capacity", "KiB", level);
        ua_exp_note(r, "Could not map the data buffer.");
        return;
    }
    ua_freq_settle(300);

    /* 16 KiB to 128 MiB in steps of sqrt(2). */
    double x[UA_EXP_MAX_POINTS], y[UA_EXP_MAX_POINTS], ns[UA_EXP_MAX_POINTS];
    size_t n = 0;
    for (double kib = 16; kib <= 131072.0 * 1.01 && n < 40; kib *= 1.4142135623730951) {
        size_t nodes = (size_t)(kib * 1024.0 / 64.0);
        chase_time t = chase(nodes, 64, 0, 0, level);
        if (isnan(t.cycles))
            continue;
        x[n] = (double)nodes * 64.0 / 1024.0;
        y[n] = t.cycles;
        ns[n] = t.ns;
        n++;
    }
    if (n < 12) {
        ua_exp_result *r = ua_exp_add(out, "l1d_size", "L1 data cache capacity", "KiB", level);
        ua_exp_note(r, "Too few clean measurements.");
        return;
    }

    /* L1: the plateau at the small end. */
    double l1 = ua_median(y, 3);
    size_t k1 = first_above(y, n, 0, l1 + 0.6);
    ua_exp_result *r;
    r = simple(out, level, "l1_latency", "L1 load-to-use latency (pointer chase)", "cycles", l1, l1,
               l1, "high");
    ua_exp_note(r, "ldr x1, [x1] over a buffer of 16 to 32 KiB. Register-offset addressing adds "
                   "a cycle (see the instruction table).");
    for (size_t i = 0; i < n; i++)
        ua_exp_point(r, x[i], y[i]);
    snprintf(r->xlabel, sizeof r->xlabel, "buffer size, KiB");
    snprintf(r->ylabel, sizeof r->ylabel, "cycles per load");

    if (k1 == 0 || k1 >= n) {
        r = ua_exp_add(out, "l1d_size", "L1 data cache capacity", "KiB", level);
        r->status = UA_EXP_INCONCLUSIVE;
        ua_exp_note(r, "No rise in latency found.");
        return;
    }
    /* Refine the L1 boundary between the two grid points. */
    double lo = x[k1 - 1], hi = x[k1];
    for (int i = 1; i < 8; i++) {
        double kib = x[k1 - 1] + (x[k1] - x[k1 - 1]) * i / 8.0;
        chase_time t = chase((size_t)(kib * 1024.0 / 64.0), 64, 0, 0, level);
        if (isnan(t.cycles))
            continue;
        if (t.cycles < l1 + 0.6 && kib > lo)
            lo = kib;
        else if (t.cycles >= l1 + 0.6 && kib < hi)
            hi = kib;
    }
    r = simple(out, level, "l1d_size", "L1 data cache capacity", "KiB", floor(lo), floor(lo),
               ceil(hi), hi - lo < 0.2 * lo ? "high" : "medium");
    ua_exp_note(r, "Largest buffer that still loads in %.1f cycles; the next size tried (%.0f "
                   "KiB) is slower. The OS reports %llu KiB.",
                l1, hi, (unsigned long long)(sys->l1d >> 10));

    /* L2: the latency just past the L1 size, the latency of a footprint of
     * several megabytes, and the size at which memory takes over. */
    size_t a = k1 + 1 < n ? k1 + 1 : k1, b = a + 3 < n ? a + 3 : n;
    double l2 = ua_median(y + a, b - a);
    r = simple(out, level, "l2_latency", "L2 load-to-use latency, small footprint", "cycles", l2,
               ua_min(y + a, b - a), ua_max(y + a, b - a), "medium");
    ua_exp_note(r, "Pointer chase over %.0f to %.0f KiB: past the L1 cache and still covered by "
                   "the first-level TLB.", x[a], x[b - 1]);
    /* Footprints of 16 to 64 times the L1 size. */
    size_t c = n, d = 0;
    for (size_t i = 0; i < n; i++) {
        if (x[i] >= 16 * x[k1 - 1] && x[i] <= 64 * x[k1 - 1]) {
            if (c == n)
                c = i;
            d = i + 1;
        }
    }
    if (c < d) {
        double far = ua_median(y + c, d - c);
        r = simple(out, level, "l2_latency_large", "L2 load-to-use latency, large footprint",
                   "cycles", far, ua_min(y + c, d - c), ua_max(y + c, d - c), "medium");
        ua_exp_note(r, "Pointer chase over %.0f to %.0f KiB. Includes first-level TLB misses "
                       "where the footprint exceeds the TLB's reach.", x[c], x[d - 1]);
        size_t k2 = first_above(y, n, d, 2.0 * far);
        if (k2 < n) {
            r = simple(out, level, "l2_size", "L2 cache capacity seen by one core", "KiB",
                       x[k2 - 1], x[k2 - 1], x[k2], "medium");
            ua_exp_note(r, "Latency passes twice the large-footprint L2 latency between %.0f and "
                           "%.0f KiB. The OS reports %llu KiB shared by %d cores; the other "
                           "cores were busy, so one core sees less than the whole cache.",
                        x[k2 - 1], x[k2], (unsigned long long)(sys->l2 >> 10), sys->cpus_per_l2);
        } else {
            r = ua_exp_add(out, "l2_size", "L2 cache capacity seen by one core", "KiB", level);
            r->status = UA_EXP_INCONCLUSIVE;
            ua_exp_note(r, "Latency never rose clearly above the L2 plateau.");
        }
    }
    r = simple(out, level, "mem_latency", "Load latency from memory (128 MiB random chase)", "ns",
               ns[n - 1], ns[n - 1], ns[n - 1], "medium");
    ua_exp_note(r, "%.0f cycles at the clock observed; includes TLB misses (16 KiB pages). "
                   "Depends on the memory fitted and on what else uses it.", y[n - 1]);
}

/* Extra cycles per load of `pages` cells laid out one per page. */
static double tlb_cost(size_t pages, size_t page, int level)
{
    chase_time sparse = chase(pages, page, 64, page, level);
    chase_time dense = chase(pages, 64, 0, 0, level);
    return sparse.cycles - dense.cycles; /* NaN if either failed */
}

/* Narrow [*lo, *hi] around the page count at which the cost passes `limit`. */
static void refine(double *lo, double *hi, double limit, size_t page, int level)
{
    double a = *lo, b = *hi;
    for (int i = 1; i < 8; i++) {
        double p = floor(a + (b - a) * i / 8.0);
        if (p <= *lo || p >= *hi)
            continue;
        double c = tlb_cost((size_t)p, page, level);
        if (isnan(c))
            continue;
        if (c < limit)
            *lo = p;
        else
            *hi = p;
    }
}

void ua_exp_tlb(int level, ua_exp_list *out)
{
    ua_sysinfo si;
    ua_sysinfo_get(&si);
    size_t page = si.page_size ? (size_t)si.page_size : 16384;
    ua_exp_result *r1 = ua_exp_add(out, "dtlb_l1", "First-level data TLB entries", "entries", level);
    ua_exp_result *r2 = ua_exp_add(out, "dtlb_l2", "Second-level TLB entries", "entries", level);
    snprintf(r1->xlabel, sizeof r1->xlabel, "pages touched");
    snprintf(r1->ylabel, sizeof r1->ylabel, "extra cycles per load (one cell per page)");
    if (!ua_big_buffer()) {
        ua_exp_note(r1, "Could not map the data buffer.");
        ua_exp_note(r2, "Could not map the data buffer.");
        return;
    }
    ua_freq_settle(200);

    double x[UA_EXP_MAX_POINTS], cost[UA_EXP_MAX_POINTS];
    size_t n = 0, max_pages = UA_BIG_BYTES / page;
    for (double p = 16; p <= (double)max_pages && n < 60; p *= 1.189207115002721) {
        size_t pages = (size_t)p;
        /* One cell per page, each at a different line offset so that the
         * cells spread over the cache sets, against the same number of
         * cells packed together. */
        double c = tlb_cost(pages, page, level);
        if (isnan(c))
            continue;
        x[n] = (double)pages;
        cost[n] = c;
        ua_exp_point(r1, x[n], cost[n]);
        n++;
    }
    if (n < 12) {
        ua_exp_note(r1, "Too few clean measurements.");
        ua_exp_note(r2, "Too few clean measurements.");
        return;
    }
    size_t k1 = first_above(cost, n, 0, 0.7);
    if (k1 == 0 || k1 >= n) {
        r1->status = r2->status = UA_EXP_INCONCLUSIVE;
        ua_exp_note(r1, "Translation never cost more than 0.7 cycles per load up to %zu pages.",
                    max_pages);
        ua_exp_note(r2, "No first-level knee to start from.");
        return;
    }
    double lo1 = x[k1 - 1], hi1 = x[k1];
    refine(&lo1, &hi1, 0.7, page, level);
    r1->status = UA_EXP_OK;
    r1->lo = lo1;
    r1->hi = hi1;
    r1->value = lo1;
    snprintf(r1->confidence, sizeof r1->confidence, "medium");
    ua_exp_note(r1, "One cell per %zu KiB page against the same cells packed together: "
                    "translation is free up to %.0f pages and costs more from %.0f on.",
                page >> 10, lo1, hi1);

    /* Second level: the plateau after the first knee, then the next rise. */
    size_t a = k1 + 1 < n ? k1 + 1 : k1, b = a + 4 < n ? a + 4 : n;
    double c1 = ua_median(cost + a, b - a);
    double limit = c1 + (c1 > 3 ? c1 : 3);
    size_t k2 = first_above(cost, n, b, limit);
    if (k2 >= n) {
        r2->status = UA_EXP_INCONCLUSIVE;
        ua_exp_note(r2, "A first-level miss costs about %.1f cycles; no second rise was found up "
                        "to %zu pages.", c1, max_pages);
        return;
    }
    double lo2 = x[k2 - 1], hi2 = x[k2];
    refine(&lo2, &hi2, limit, page, level);
    r2->status = UA_EXP_OK;
    r2->lo = lo2;
    r2->hi = hi2;
    r2->value = lo2;
    snprintf(r2->confidence, sizeof r2->confidence, "medium");
    ua_exp_note(r2, "A first-level TLB miss that the second level catches costs about %.1f "
                    "cycles. From %.0f pages on (%.0f were still fine) the cost passes %.1f "
                    "cycles: page walks. See the first-level entry for the curve.",
                c1, hi2, lo2, limit);
}
