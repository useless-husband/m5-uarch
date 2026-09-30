/*
 * measure.h - turn a generated loop into "cycles per iteration".
 *
 * One measurement is a series of paired runs at n and 2n iterations.  The
 * difference cancels every fixed cost (counter syscalls, trampoline, loop
 * entry and exit).  A run is kept only if it is *clean*:
 *
 *   - all of its cycles were counted on the requested performance level
 *     (no migration between P- and E-cores), and
 *   - its retired-instruction count equals the smallest count seen for that
 *     n.  The loop retires a fixed number of instructions, so any excess is
 *     kernel work done on the thread's behalf: an interrupt or a preemption.
 *
 * What is left is summarised by the median, with the minimum and the
 * inter-quartile spread reported next to it.
 */
#ifndef UA_MEASURE_H
#define UA_MEASURE_H

#include "jit.h"

#include <stdint.h>

typedef enum {
    UA_OK = 0,
    UA_NOISY,       /* could not collect enough clean runs                   */
    UA_FAULT,       /* the generated code raised a signal (e.g. SIGILL)      */
    UA_NOCOUNTERS,  /* no usable counter backend                             */
    UA_WRONGLEVEL,  /* never ran on the requested performance level          */
    UA_BADCOUNT,    /* instructions per iteration differ from the generator  */
} ua_status;

const char *ua_status_name(ua_status s);

typedef struct {
    int level;              /* performance level to run on; -1 = whichever    */
    int min_clean;          /* clean runs wanted at each n (default 7)        */
    int max_runs;           /* give up after this many pairs (default 96)     */
    uint64_t target_cycles; /* auto-size n so that one run is about this long */
    uint64_t n1;            /* explicit iteration count (0 = auto)            */
    uint64_t expect_ins;    /* instructions per iteration (0 = do not check)  */
    int flip_nzcv;          /* before every counted run, execute a few
                               iterations with NZCV inverted (uncounted)      */
    uint32_t carry;         /* bit i: x[i] continues from its value at the end
                               of the previous run (pointer chases that must
                               not revisit what is already cached)            */
} ua_mopts;

typedef struct {
    ua_status status;
    double cyc;      /* cycles per iteration: median-based                     */
    double cyc_min;  /* cycles per iteration: minimum-based                    */
    double spread;   /* (IQR at n + IQR at 2n) / (median difference)           */
    double ins;      /* instructions per iteration                             */
    double ghz;      /* cycles per nanosecond observed during the clean runs   */
    double ns;       /* nanoseconds per iteration, median-based (wall clock)   */
    double ns_min;   /* nanoseconds per iteration, minimum-based               */
    int level;       /* performance level the clean runs executed on           */
    int clean;       /* clean runs at 2n                                       */
    int runs;        /* pairs attempted                                        */
    int migrated;    /* runs discarded: wrong or mixed performance level       */
    int disturbed;   /* runs discarded: more instructions than the loop has    */
    int fault_sig;   /* signal number when status == UA_FAULT                  */
    uint64_t n1;
} ua_meas;

ua_mopts ua_mopts_default(int level);

/* Ask the scheduler for a performance level via the thread's QoS class
 * (level 0: user-interactive, last level: background), then wait until the
 * per-level counters show the thread running there.  Returns 1 on success,
 * 0 if the thread did not arrive within half a second.  QoS is a request,
 * not a pin, so ua_measure() still verifies every single run. */
int ua_level_request(int level);

/* Install the fault handlers used to survive unsupported instructions. */
void ua_measure_init(void);

ua_meas ua_measure(const void *code, ua_regs *regs, const ua_mopts *opts);

/* Run `code` once under the fault guard.  Returns 0, or the signal number. */
int ua_run_guarded(const void *code, ua_regs *regs, uint64_t iters);

/* One counted run, for experiments that need their own sampling plan (for
 * example alternating two loops so that both see the same clock frequency). */
typedef struct {
    int level;       /* performance level it ran on, -1 if split or faulted   */
    int fault_sig;   /* non-zero if the code raised a signal                  */
    uint64_t cyc;    /* cycles, including the fixed entry/exit overhead       */
    uint64_t ins;    /* instructions, likewise                                */
    double ns;       /* wall-clock nanoseconds                                */
} ua_sample;

ua_sample ua_run_counted(const void *code, ua_regs *regs, uint64_t iters);

/* Keep the thread busy until the cluster's clock frequency stops changing
 * (at most `budget_ms`).  Returns the frequency in GHz.  Cycle counts do not
 * depend on the frequency, but anything that involves memory does: a cache
 * miss lasts a fixed time, not a fixed number of cycles. */
double ua_freq_settle(int budget_ms);

/* Totals since start, for the run summary. */
typedef struct {
    uint64_t runs, clean, migrated, disturbed;
} ua_run_totals;
ua_run_totals ua_measure_totals(void);
/* For experiments that classify their own runs (ua_run_counted). */
void ua_measure_account(uint64_t clean, uint64_t migrated, uint64_t disturbed);

#endif
