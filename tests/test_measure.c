/*
 * test_measure.c - the measurement layer against real counters.
 * Exits 77 (skipped) when the machine exposes none, as virtual machines do.
 */
#include "check.h"
#include "counters.h"
#include "enc.h"
#include "exp.h"
#include "jit.h"
#include "measure.h"
#include "sysinfo.h"

#include <signal.h>

static const void *chain(unsigned adds)
{
    ua_jit_loop_open(NULL, 0);
    for (unsigned i = 0; i < adds; i++)
        ua_jit_put(a64_add_imm(0, 0, 1));
    return ua_jit_loop_close(NULL, 0);
}

int main(void)
{
    ua_sysinfo si;
    ua_sysinfo_get(&si);
    if (ua_jit_init() != 0 || ua_counters_init(UA_CTR_NONE) == UA_CTR_NONE) {
        printf("test_measure: SKIP (no unprivileged cycle counter%s)\n",
               si.is_vm ? "; this is a virtual machine" : "");
        return 77;
    }
    ua_measure_init();
    CHECK(ua_counters_nlevels() >= 1);

    /* Counters are monotonic and per level. */
    ua_counts a, b;
    ua_counters_read(&a);
    ua_counters_read(&b);
    for (int i = 0; i < ua_counters_nlevels(); i++) {
        CHECK(b.cyc[i] >= a.cyc[i]);
        CHECK(b.ins[i] >= a.ins[i]);
    }

    int measured = 0;
    for (int level = 0; level < ua_counters_nlevels(); level++) {
        if (!ua_level_request(level)) {
            printf("level %d not reachable right now, skipped\n", level);
            continue;
        }
        measured++;
        ua_regs regs;
        ua_regs_default(&regs);
        ua_mopts o = ua_mopts_default(level);

        /* A chain of 8 and of 24 adds: one cycle per add, exact counts. */
        o.expect_ins = 10;
        ua_meas m8 = ua_measure(chain(8), &regs, &o);
        CHECK(m8.status == UA_OK);
        CHECK(m8.level == level);
        CHECK_NEAR(m8.cyc, 8.0, 0.1);
        CHECK_NEAR(m8.ins, 10.0, 0.01);
        CHECK(m8.clean >= 3);
        o.expect_ins = 26;
        ua_meas m24 = ua_measure(chain(24), &regs, &o);
        CHECK(m24.status == UA_OK);
        CHECK_NEAR(m24.cyc - m8.cyc, 16.0, 0.2);
        CHECK(m24.ghz > 0.2 && m24.ghz < 10);
        CHECK(m24.ns > 0);

        /* A wrong expected instruction count is noticed. */
        o.expect_ins = 25;
        ua_meas bad = ua_measure(chain(24), &regs, &o);
        CHECK(bad.status == UA_BADCOUNT);

        /* ... even a single instruction per iteration at the smallest
         * iteration count (regression: the tolerance used to hide it). */
        o.expect_ins = 25;
        o.n1 = 16;
        bad = ua_measure(chain(24), &regs, &o);
        CHECK(bad.status == UA_BADCOUNT);
        o.n1 = 0;

        /* A faulting loop is reported, and measuring still works afterwards. */
        ua_jit_loop_open(NULL, 0);
        ua_jit_put(0x00000000u);
        const void *ill = ua_jit_loop_close(NULL, 0);
        o.expect_ins = 0;
        ua_meas f = ua_measure(ill, &regs, &o);
        CHECK(f.status == UA_FAULT && f.fault_sig == SIGILL);
        ua_meas again = ua_measure(chain(8), &regs, &o);
        CHECK(again.status == UA_OK);
        CHECK_NEAR(again.cyc, 8.0, 0.1);

        /* The same loop compared with itself gives a ratio of one. */
        const void *code = chain(16);
        ua_regs ra, rb;
        ua_regs_default(&ra);
        rb = ra;
        ua_pair p = ua_exp_pair(code, &ra, 0, code, &rb, 0, 0, 2000, level, 5, 40);
        CHECK(p.ok);
        CHECK_NEAR(p.ratio, 1.0, 0.03);

        /* ua_run_counted sees the right number of instructions. */
        ua_sample s1 = ua_run_counted(code, &ra, 1000), s2 = ua_run_counted(code, &ra, 3000);
        if (s1.level == level && s2.level == level && s2.ins > s1.ins) {
            long long d = (long long)(s2.ins - s1.ins) - 2000 * 18;
            CHECK(d > -200 && d < 20000); /* exact unless an interrupt landed */
        }
    }
    CHECK(ua_measure(NULL, NULL, NULL).status == UA_BADCOUNT);
    if (!measured) {
        printf("test_measure: SKIP (no performance level reachable)\n");
        return 77;
    }
    return test_finish("test_measure");
}
