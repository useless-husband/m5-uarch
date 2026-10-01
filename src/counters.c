#include "counters.h"

#include <dlfcn.h>
#include <libproc.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>

/*
 * thread_selfcounts() lives in libsystem_kernel but has no public header.
 * Kind 2 ("CPI per perf level") fills an array of {instructions, cycles}
 * pairs, one per performance level, for the calling thread.  We resolve it
 * with dlsym so that a system without the symbol degrades to a slower
 * backend instead of failing to link.
 */
#define THSC_CPI_PER_PERF_LEVEL 2
struct thsc_cpi {
    uint64_t instructions;
    uint64_t cycles;
};
typedef int (*selfcounts_fn)(int kind, void *buf, size_t size);

/* proc_pidinfo flavour from xnu's bsd/sys/proc_info_private.h. */
#define UA_PROC_PIDTHREADCOUNTS 34
struct ua_threadcounts_data {
    uint64_t instructions;
    uint64_t cycles;
    uint64_t user_time_mach;
    uint64_t system_time_mach;
    uint64_t energy_nj;
};
struct ua_threadcounts {
    uint16_t len;
    uint16_t reserved0;
    uint32_t reserved1;
    struct ua_threadcounts_data counts[UA_MAX_LEVELS];
};

static ua_ctr_backend g_backend = UA_CTR_NONE;
static int g_nlevels = 1;
static selfcounts_fn g_selfcounts;
static uint64_t g_tid;
static pid_t g_pid;

static void read_selfcounts(ua_counts *out)
{
    struct thsc_cpi buf[UA_MAX_LEVELS];
    memset(buf, 0, sizeof buf);
    memset(out, 0, sizeof *out);
    if (g_selfcounts(THSC_CPI_PER_PERF_LEVEL, buf, sizeof(buf[0]) * (size_t)g_nlevels) != 0)
        return;
    for (int i = 0; i < g_nlevels; i++) {
        out->cyc[i] = buf[i].cycles;
        out->ins[i] = buf[i].instructions;
    }
}

static void read_pidinfo(ua_counts *out)
{
    struct ua_threadcounts tc;
    uint64_t tid = g_tid;
    memset(&tc, 0, sizeof tc);
    memset(out, 0, sizeof *out);
    /* The thread id is cached at init; other threads must not use this
     * backend (the tool is single-threaded by design). */
    int size = (int)(8 + sizeof(struct ua_threadcounts_data) * (size_t)g_nlevels);
    if (proc_pidinfo(g_pid, UA_PROC_PIDTHREADCOUNTS, tid, &tc, size) <= 0)
        return;
    for (int i = 0; i < g_nlevels && i < tc.len; i++) {
        out->cyc[i] = tc.counts[i].cycles;
        out->ins[i] = tc.counts[i].instructions;
    }
}

static void read_rusage(ua_counts *out)
{
    struct rusage_info_v6 ri;
    memset(out, 0, sizeof *out);
    if (proc_pid_rusage(g_pid, RUSAGE_INFO_V6, (rusage_info_t *)&ri) != 0)
        return;
    /* Process-wide.  Level 0 is "P-core only"; everything else is lumped
     * into the last level.  Exact only for a single-threaded process on a
     * two-level chip, which is what this tool is. */
    out->cyc[0] = ri.ri_pcycles;
    out->ins[0] = ri.ri_pinstructions;
    if (g_nlevels > 1) {
        out->cyc[g_nlevels - 1] = ri.ri_cycles - ri.ri_pcycles;
        out->ins[g_nlevels - 1] = ri.ri_instructions - ri.ri_pinstructions;
    } else {
        out->cyc[0] = ri.ri_cycles;
        out->ins[0] = ri.ri_instructions;
    }
}

static void read_with(ua_ctr_backend b, ua_counts *out)
{
    switch (b) {
    case UA_CTR_SELFCOUNTS: read_selfcounts(out); break;
    case UA_CTR_PIDINFO:    read_pidinfo(out); break;
    case UA_CTR_RUSAGE:     read_rusage(out); break;
    default:                memset(out, 0, sizeof *out); break;
    }
}

/* A backend is usable if the counters are non-zero and advance when the
 * thread does work.  Virtual machines typically return all zeros. */
static int backend_works(ua_ctr_backend b)
{
    ua_counts a, c;
    volatile uint64_t sink = 0;
    if (b == UA_CTR_SELFCOUNTS && !g_selfcounts)
        return 0;
    read_with(b, &a);
    for (int i = 0; i < 200000; i++)
        sink += (uint64_t)i * 2654435761u;
    read_with(b, &c);
    uint64_t dc = 0, di = 0;
    for (int i = 0; i < g_nlevels; i++) {
        dc += c.cyc[i] - a.cyc[i];
        di += c.ins[i] - a.ins[i];
    }
    /* The loop above retires well over 200k instructions. */
    return dc > 1000 && di > 200000;
}

ua_ctr_backend ua_counters_init(ua_ctr_backend force)
{
    int n = 1;
    size_t len = sizeof n;
    if (sysctlbyname("hw.nperflevels", &n, &len, NULL, 0) != 0 || n < 1)
        n = 1;
    g_nlevels = n > UA_MAX_LEVELS ? UA_MAX_LEVELS : n;
    g_pid = getpid();
    pthread_threadid_np(NULL, &g_tid);
    g_selfcounts = (selfcounts_fn)dlsym(RTLD_DEFAULT, "thread_selfcounts");

    static const ua_ctr_backend order[] = {UA_CTR_SELFCOUNTS, UA_CTR_PIDINFO, UA_CTR_RUSAGE};
    g_backend = UA_CTR_NONE;
    const char *env = getenv("UARCH_COUNTERS");
    if (env && strcmp(env, "none") == 0)
        return g_backend; /* behave like a machine without counters (tests) */
    for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
        if (force != UA_CTR_NONE && force != order[i])
            continue;
        if (backend_works(order[i])) {
            g_backend = order[i];
            break;
        }
    }
    return g_backend;
}

ua_ctr_backend ua_counters_backend(void) { return g_backend; }

const char *ua_counters_backend_name(ua_ctr_backend b)
{
    switch (b) {
    case UA_CTR_SELFCOUNTS: return "thread_selfcounts";
    case UA_CTR_PIDINFO:    return "proc_pidinfo(PROC_PIDTHREADCOUNTS)";
    case UA_CTR_RUSAGE:     return "proc_pid_rusage(RUSAGE_INFO_V6)";
    default:                return "none";
    }
}

int ua_counters_nlevels(void) { return g_nlevels; }

void ua_counters_read(ua_counts *out) { read_with(g_backend, out); }
