/*
 * counters.h - unprivileged cycle / instruction counters on macOS.
 *
 * Apple Silicon has no user-readable PMU and configuring one (kpc) needs
 * root.  The kernel does, however, keep per-thread fixed counters (cycles
 * and retired instructions) and exposes them to the owning process, split by
 * performance level (P-cores / E-cores).  That split is what lets us verify,
 * after the fact, which core type a measurement really executed on.
 */
#ifndef UA_COUNTERS_H
#define UA_COUNTERS_H

#include <stdint.h>

#define UA_MAX_LEVELS 4

typedef struct {
    uint64_t cyc[UA_MAX_LEVELS];
    uint64_t ins[UA_MAX_LEVELS];
} ua_counts;

typedef enum {
    UA_CTR_NONE = 0,   /* nothing usable (e.g. virtual machine returning zeros) */
    UA_CTR_SELFCOUNTS, /* thread_selfcounts(): per thread, per perf level      */
    UA_CTR_PIDINFO,    /* proc_pidinfo(PROC_PIDTHREADCOUNTS): same data, slower */
    UA_CTR_RUSAGE,     /* proc_pid_rusage V6: whole process, P vs. total        */
} ua_ctr_backend;

/* Probe the backends in order of preference and pick the first that returns
 * non-zero, advancing counters.  `force` may be UA_CTR_NONE for automatic.
 * With UARCH_COUNTERS=none in the environment no backend is used, as on a
 * virtual machine; the test suite uses this to exercise the skip path that
 * CI runners take. */
ua_ctr_backend ua_counters_init(ua_ctr_backend force);
ua_ctr_backend ua_counters_backend(void);
const char *ua_counters_backend_name(ua_ctr_backend b);

/* Number of performance levels (hw.nperflevels), clamped to UA_MAX_LEVELS. */
int ua_counters_nlevels(void);

/* Read the calling thread's counters.  Async-signal-safe for SELFCOUNTS. */
void ua_counters_read(ua_counts *out);

#endif
