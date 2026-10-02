#include "measure.h"

#include "counters.h"
#include "stats.h"

#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Instruction-count tolerance above the minimum.  The counter syscall path
 * itself varies by a few instructions; an interrupt adds thousands. */
#define INS_TOLERANCE 64
/* Slack when comparing the counted instructions with the generator's. */
#define COUNT_TOLERANCE 12
/* Steady states (measure.h).  A run is in the fastest state if it took at
 * most STATE_TOL longer than the fastest time that three clean runs of the
 * same length reach (ua_fastest_state).  The states seen on the M5 are 3 %
 * or more apart, the scatter inside one well under 1 %. */
#define STATE_TOL 0.01
#define STATE_MIN 5
/* Plausible fixed cost per run as a fraction of a run at n1.  The counter
 * reads cost about 600 cycles of the 40 000 a run is sized for (1.5 %).
 * Runs at n1 in a state d slower (relative) than the runs at 2n1 move the
 * implied value by 2d, so a mix of states 2 % or more apart lands outside. */
#define FIXED_LO (-0.01)
#define FIXED_HI 0.05

static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_guard_armed;
static volatile sig_atomic_t g_fault_sig;
static ua_run_totals g_totals;
static mach_timebase_info_data_t g_tb;

const char *ua_status_name(ua_status s)
{
    switch (s) {
    case UA_OK:         return "ok";
    case UA_NOISY:      return "noisy";
    case UA_FAULT:      return "fault";
    case UA_NOCOUNTERS: return "no-counters";
    case UA_WRONGLEVEL: return "wrong-level";
    case UA_BADCOUNT:   return "bad-count";
    }
    return "?";
}

static void on_fault(int sig)
{
    if (g_guard_armed) {
        g_fault_sig = sig;
        siglongjmp(g_jmp, 1);
    }
    /* Not ours: restore the default action and re-raise. */
    signal(sig, SIG_DFL);
    raise(sig);
}

void ua_measure_init(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fault;
    sigemptyset(&sa.sa_mask);
    static const int sigs[] = {SIGILL, SIGSEGV, SIGBUS, SIGFPE, SIGTRAP, SIGSYS};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        sigaction(sigs[i], &sa, NULL);
    mach_timebase_info(&g_tb);
}

int ua_run_guarded(const void *code, ua_regs *regs, uint64_t iters)
{
    if (sigsetjmp(g_jmp, 1)) {
        g_guard_armed = 0;
        /* The fault may have hit while generated code had the arena in an
         * odd state; nothing else to unwind, the trampoline frame is gone. */
        return g_fault_sig;
    }
    g_guard_armed = 1;
    ua_tramp(code, regs, iters);
    g_guard_armed = 0;
    return 0;
}

ua_mopts ua_mopts_default(int level)
{
    ua_mopts o;
    memset(&o, 0, sizeof o);
    o.level = level;
    o.min_clean = 7;
    o.max_runs = 96;
    o.target_cycles = 40000;
    return o;
}

/* Burn a little CPU and report which performance level it was counted on
 * (-1 if it was split across levels). */
static int probe_level(void)
{
    ua_counts a, b;
    volatile uint64_t sink = 0;
    ua_counters_read(&a);
    for (int i = 0; i < 20000; i++)
        sink += (uint64_t)i * 2654435761u;
    ua_counters_read(&b);
    int nl = ua_counters_nlevels(), active = -1, n = 0;
    for (int i = 0; i < nl; i++)
        if (b.cyc[i] != a.cyc[i]) {
            active = i;
            n++;
        }
    return n == 1 ? active : -1;
}

/* Spin until the thread is observed on `level` three times in a row, or
 * until `budget_ms` has passed.  Returns 1 if it got there. */
static int settle_on_level(int level, int budget_ms)
{
    uint64_t deadline = mach_absolute_time() +
                        (uint64_t)budget_ms * 1000000ull * g_tb.denom / (g_tb.numer ? g_tb.numer : 1);
    int streak = 0;
    while (streak < 3) {
        if (probe_level() == level) {
            streak++;
        } else {
            streak = 0;
            if (mach_absolute_time() > deadline)
                return 0;
            sched_yield();
        }
    }
    return 1;
}

int ua_level_request(int level)
{
    int n = ua_counters_nlevels();
    qos_class_t q;
    if (!g_tb.denom)
        mach_timebase_info(&g_tb);
    if (level <= 0)
        q = QOS_CLASS_USER_INTERACTIVE;
    else if (level >= n - 1)
        q = QOS_CLASS_BACKGROUND;
    else
        q = QOS_CLASS_UTILITY;
    pthread_set_qos_class_self_np(q, 0);
    if (ua_counters_backend() == UA_CTR_NONE)
        return 0;
    /* QoS is a request, not a pin: wait until the counters show that the
     * thread really moved. */
    return settle_on_level(level, 500);
}

typedef struct {
    int level;      /* level that ran, -1 if mixed or none */
    uint64_t cyc;
    uint64_t ins;
    uint64_t ticks; /* mach_absolute_time delta */
} run_sample;

static int g_flip_nzcv;
static uint32_t g_carry;

static void carry_regs(ua_regs *regs)
{
    for (int i = 0; g_carry >> i; i++)
        if (i < 31 && (g_carry >> i & 1))
            regs->x[i] = regs->out_x[i];
}

/* One counted run.  Assumes the fault guard is armed by the caller. */
static run_sample counted_run(const void *code, ua_regs *regs, uint64_t n)
{
    ua_counts a, b;
    run_sample s = {-1, 0, 0, 0};
    if (g_flip_nzcv) {
        /* Show every conditional in the loop the opposite outcome, so that
         * nothing in the core can come to treat the condition as constant. */
        regs->nzcv ^= 0xf0000000u;
        ua_tramp(code, regs, 4);
        regs->nzcv ^= 0xf0000000u;
    }
    uint64_t t0 = mach_absolute_time();
    ua_counters_read(&a);
    ua_tramp(code, regs, n);
    ua_counters_read(&b);
    s.ticks = mach_absolute_time() - t0;
    carry_regs(regs);
    int nl = ua_counters_nlevels(), active = -1, nactive = 0;
    for (int i = 0; i < nl; i++) {
        if (b.cyc[i] != a.cyc[i] || b.ins[i] != a.ins[i]) {
            active = i;
            nactive++;
        }
    }
    if (nactive == 1) {
        s.level = active;
        s.cyc = b.cyc[active] - a.cyc[active];
        s.ins = b.ins[active] - a.ins[active];
    }
    g_totals.runs++;
    return s;
}

typedef struct {
    double cyc[2][256];
    double ns[2][256];
    double ghz[256];
    size_t n[2];
    size_t nghz;
    int64_t ref[2];
    int migrated, disturbed;
} clean_set;

/* Pick the clean runs out of the samples collected so far. */
static void select_clean(const run_sample *s[2], int runs, int want_level, int *level_out,
                         clean_set *cs)
{
    memset(cs, 0, sizeof *cs);

    /* Decide which level the series ran on: the requested one, or the most
     * common single level when the caller does not care. */
    int level = want_level;
    if (level < 0) {
        int votes[UA_MAX_LEVELS] = {0};
        for (int k = 0; k < 2; k++)
            for (int r = 0; r < runs; r++)
                if (s[k][r].level >= 0)
                    votes[s[k][r].level]++;
        level = 0;
        for (int i = 1; i < UA_MAX_LEVELS; i++)
            if (votes[i] > votes[level])
                level = i;
    }
    *level_out = level;

    for (int k = 0; k < 2; k++) {
        /* Reference count: the smallest one seen.  Kernel work can only add
         * instructions, so the minimum is the undisturbed loop even when
         * most runs on a busy machine take an interrupt. */
        int64_t ref = INT64_MAX;
        for (int r = 0; r < runs; r++)
            if (s[k][r].level == level && (int64_t)s[k][r].ins < ref)
                ref = (int64_t)s[k][r].ins;
        cs->ref[k] = ref == INT64_MAX ? 0 : ref;
        for (int r = 0; r < runs; r++) {
            if (s[k][r].level != level) {
                cs->migrated++;
                continue;
            }
            if ((int64_t)s[k][r].ins - ref > INS_TOLERANCE) {
                cs->disturbed++;
                continue;
            }
            double ns = (double)s[k][r].ticks * g_tb.numer / g_tb.denom;
            cs->ns[k][cs->n[k]] = ns;
            cs->cyc[k][cs->n[k]++] = (double)s[k][r].cyc;
            if (k == 1 && s[k][r].ticks)
                cs->ghz[cs->nghz++] = (double)s[k][r].cyc / ns;
        }
    }
}

/* The runs a result is computed from: every clean run, or with `fastest`
 * only those in the fastest steady state.  Returns 1 if the selection is
 * usable: at least `want` runs at each length and, with `fastest`, a
 * plausible implied fixed cost. */
typedef struct {
    double cyc[2][256], ns[2][256];
    size_t n[2];
    double fixed;
} chosen_runs;

static int choose_runs(const clean_set *cs, int fastest, size_t want, chosen_runs *ch)
{
    static size_t idx[256];
    for (int k = 0; k < 2; k++) {
        size_t m;
        if (fastest) {
            m = ua_fastest_state(cs->cyc[k], cs->n[k], STATE_TOL, idx);
        } else {
            m = cs->n[k];
            for (size_t i = 0; i < m; i++)
                idx[i] = i;
        }
        for (size_t i = 0; i < m; i++) {
            ch->cyc[k][i] = cs->cyc[k][idx[i]];
            ch->ns[k][i] = cs->ns[k][idx[i]];
        }
        ch->n[k] = m;
    }
    ch->fixed = NAN;
    if (ch->n[0] < want || ch->n[1] < want)
        return 0;
    double med1 = ua_median(ch->cyc[0], ch->n[0]), med2 = ua_median(ch->cyc[1], ch->n[1]);
    ch->fixed = 2.0 * med1 - med2; /* n2 = 2 n1 */
    return !fastest || (ch->fixed >= FIXED_LO * med1 && ch->fixed <= FIXED_HI * med1);
}

ua_meas ua_measure(const void *code, ua_regs *regs, const ua_mopts *opts_in)
{
    ua_mopts o = opts_in ? *opts_in : ua_mopts_default(-1);
    ua_meas m;
    memset(&m, 0, sizeof m);
    m.level = -1;
    if (o.min_clean <= 0)
        o.min_clean = 7;
    if (o.max_runs <= 0)
        o.max_runs = 96;
    if (o.max_runs > 256)
        o.max_runs = 256;
    if (o.min_clean > o.max_runs)
        o.min_clean = o.max_runs;
    if (!o.target_cycles)
        o.target_cycles = 40000;

    if (ua_counters_backend() == UA_CTR_NONE) {
        m.status = UA_NOCOUNTERS;
        return m;
    }
    if (!code) {
        m.status = UA_BADCOUNT;
        return m;
    }

    static run_sample s1[256], s2[256];
    static chosen_runs ch;
    const run_sample *s[2] = {s1, s2};
    clean_set cs;
    volatile int runs = 0;

    if (sigsetjmp(g_jmp, 1)) {
        g_guard_armed = 0;
        /* Locals written since sigsetjmp are indeterminate: start over. */
        memset(&m, 0, sizeof m);
        m.level = -1;
        m.status = UA_FAULT;
        m.fault_sig = g_fault_sig;
        return m;
    }
    g_guard_armed = 1;

    g_flip_nzcv = o.flip_nzcv;
    g_carry = o.carry;
    /* Warm up: code, predictors, caches, and the frequency governor. */
    ua_tramp(code, regs, 64);
    carry_regs(regs);
    uint64_t n1 = o.n1;
    if (!n1) {
        run_sample a = counted_run(code, regs, 64);
        run_sample b = counted_run(code, regs, 1024);
        double per = b.cyc > a.cyc ? (double)(b.cyc - a.cyc) / 960.0 : 1.0;
        if (per < 0.05)
            per = 0.05;
        double want = (double)o.target_cycles / per;
        n1 = want < 16 ? 16 : want > (double)(1u << 26) ? (1u << 26) : (uint64_t)want;
    }
    uint64_t n2 = 2 * n1;
    m.n1 = n1;
    ua_tramp(code, regs, n1);
    carry_regs(regs);

    int level = -1;
    while (runs < o.max_runs) {
        /* Alternate the order so that slow drifts cancel. */
        if (runs & 1) {
            s2[runs] = counted_run(code, regs, n2);
            s1[runs] = counted_run(code, regs, n1);
        } else {
            s1[runs] = counted_run(code, regs, n1);
            s2[runs] = counted_run(code, regs, n2);
        }
        if (o.level >= 0 && (s1[runs].level != o.level || s2[runs].level != o.level))
            settle_on_level(o.level, 50);
        runs++;
        if (runs >= o.min_clean) {
            select_clean(s, runs, o.level, &level, &cs);
            if (cs.n[0] >= (size_t)o.min_clean && cs.n[1] >= (size_t)o.min_clean &&
                (!o.fastest || choose_runs(&cs, 1, STATE_MIN, &ch)))
                break;
        }
    }
    g_guard_armed = 0;

    select_clean(s, runs, o.level, &level, &cs);
    m.runs = runs;
    m.level = level;
    m.clean = (int)cs.n[1];
    m.migrated = cs.migrated;
    m.disturbed = cs.disturbed;
    g_totals.clean += cs.n[0] + cs.n[1];
    g_totals.migrated += (uint64_t)cs.migrated;
    g_totals.disturbed += (uint64_t)cs.disturbed;

    if (cs.n[0] < 3 || cs.n[1] < 3) {
        /* Distinguish "never on the right core" from "always interrupted". */
        m.status = (cs.migrated >= 2 * runs - 4) ? UA_WRONGLEVEL : UA_NOISY;
        return m;
    }
    int consistent = choose_runs(&cs, o.fastest, 3, &ch);
    if (ch.n[0] < 3 || ch.n[1] < 3) {
        m.status = UA_NOISY;
        return m;
    }

    double dn = (double)(n2 - n1);
    double med1 = ua_median(ch.cyc[0], ch.n[0]), med2 = ua_median(ch.cyc[1], ch.n[1]);
    m.cyc = (med2 - med1) / dn;
    m.cyc_min = (ua_min(cs.cyc[1], cs.n[1]) - ua_min(cs.cyc[0], cs.n[0])) / dn;
    m.fixed = ch.fixed;
    m.ins = (double)(cs.ref[1] - cs.ref[0]) / dn;
    m.ghz = cs.nghz ? ua_median(cs.ghz, cs.nghz) : 0;
    m.ns = (ua_median(ch.ns[1], ch.n[1]) - ua_median(ch.ns[0], ch.n[0])) / dn;
    m.ns_min = (ua_min(cs.ns[1], cs.n[1]) - ua_min(cs.ns[0], cs.n[0])) / dn;

    ua_sort(ch.cyc[0], ch.n[0]);
    ua_sort(ch.cyc[1], ch.n[1]);
    double iqr = (ua_quantile_sorted(ch.cyc[0], ch.n[0], 0.75) -
                  ua_quantile_sorted(ch.cyc[0], ch.n[0], 0.25)) +
                 (ua_quantile_sorted(ch.cyc[1], ch.n[1], 0.75) -
                  ua_quantile_sorted(ch.cyc[1], ch.n[1], 0.25));
    m.spread = (med2 > med1) ? iqr / (med2 - med1) : 0;

    if (cs.n[0] < (size_t)o.min_clean || cs.n[1] < (size_t)o.min_clean || !consistent)
        m.status = UA_NOISY;
    if (o.expect_ins) {
        /* Compare absolute counts.  The counter system call's own path
         * varies by a few instructions, so allow that much and no more: one
         * instruction per iteration too many or too few must be noticed even
         * at the smallest iteration count (16). */
        int64_t want = (int64_t)(o.expect_ins * (n2 - n1));
        int64_t got = cs.ref[1] - cs.ref[0];
        if (got - want > COUNT_TOLERANCE || want - got > COUNT_TOLERANCE)
            m.status = UA_BADCOUNT;
    }
    return m;
}

int ua_loop_cost_plausible(double c1, double c2)
{
    double extra = 2.0 * c1 - c2;
    return extra >= -1.0 - 0.01 * c1 && extra <= 2.0 + 0.02 * c1;
}

ua_sample ua_run_counted(const void *code, ua_regs *regs, uint64_t iters)
{
    ua_sample out = {-1, 0, 0, 0, 0};
    if (sigsetjmp(g_jmp, 1)) {
        g_guard_armed = 0;
        out.level = -1;
        out.fault_sig = g_fault_sig;
        return out;
    }
    g_guard_armed = 1;
    g_flip_nzcv = 0;
    g_carry = 0;
    run_sample s = counted_run(code, regs, iters);
    g_guard_armed = 0;
    out.level = s.level;
    out.cyc = s.cyc;
    out.ins = s.ins;
    out.ns = (double)s.ticks * g_tb.numer / g_tb.denom;
    return out;
}

double ua_freq_settle(int budget_ms)
{
    if (!g_tb.denom)
        mach_timebase_info(&g_tb);
    uint64_t start = mach_absolute_time();
    uint64_t budget = (uint64_t)budget_ms * 1000000ull * g_tb.denom / g_tb.numer;
    double prev = 0, ghz = 0;
    int stable = 0;
    volatile uint64_t sink = 0;
    while (mach_absolute_time() - start < budget) {
        ua_counts a, b;
        uint64_t t0 = mach_absolute_time();
        ua_counters_read(&a);
        for (int i = 0; i < 2000000; i++)
            sink += (uint64_t)i * 2654435761u;
        ua_counters_read(&b);
        double ns = (double)(mach_absolute_time() - t0) * g_tb.numer / g_tb.denom;
        uint64_t cyc = 0;
        for (int i = 0; i < ua_counters_nlevels(); i++)
            cyc += b.cyc[i] - a.cyc[i];
        ghz = ns > 0 ? (double)cyc / ns : 0;
        /* Stable: three consecutive windows within 2 % of each other. */
        if (prev > 0 && ghz > prev * 0.98 && ghz < prev * 1.02) {
            if (++stable >= 3)
                break;
        } else {
            stable = 0;
        }
        prev = ghz;
    }
    return ghz;
}

ua_run_totals ua_measure_totals(void) { return g_totals; }

void ua_measure_account(uint64_t clean, uint64_t migrated, uint64_t disturbed)
{
    g_totals.clean += clean;
    g_totals.migrated += migrated;
    g_totals.disturbed += disturbed;
}
