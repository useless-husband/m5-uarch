/*
 * exp_branch.c - branch misprediction penalty.
 *
 * One loop, two data sets.  Each iteration branches on the low bit of a byte
 * read from a long array; both sides of the branch execute the same number
 * of instructions.  With alternating bytes the branch is predicted
 * perfectly, with random bytes about half the time.  Half the difference in
 * cycles per iteration is the cost of one misprediction.
 *
 * The byte for iteration i+1 is loaded in iteration i, before that
 * iteration's branch in program order.  A misprediction flushes only what
 * comes after the branch, so the next branch always finds its condition
 * ready and the penalty measured is the pipeline's, not the load's.
 *
 * Without PMU events the misprediction count cannot be read; "half" is the
 * assumption that random bits cannot be predicted.  The array is long enough
 * that no byte is used twice.
 */
#include "enc.h"
#include "exp.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_BYTES (8u * 1024u * 1024u)
#define ITERS 4000u

static void emit_loop(void *ctx)
{
    (void)ctx;
    ua_jit_put(a64_mov(4, 7));          /* bit decided one iteration ago   */
    ua_jit_put(a64_ldrb_post(7, 1, 1)); /* fetch the next one              */
    ua_jit_put(a64_tbz(4, 0, 3));
    ua_jit_put(a64_add_imm(5, 5, 1));   /* bit set */
    ua_jit_put(a64_b(3));
    ua_jit_put(a64_add_imm(6, 6, 1));   /* bit clear */
    ua_jit_put(a64_nop());
}

static double loop_cycles(uint8_t *array, int level, ua_meas *out)
{
    ua_regs regs;
    ua_regs_default(&regs);
    regs.x[1] = (uint64_t)(uintptr_t)array;
    regs.x[7] = 0;
    ua_mopts o = ua_mopts_default(level);
    o.n1 = ITERS;
    o.carry = 1u << 1 | 1u << 7; /* keep reading forward: never the same bytes twice */
    o.max_runs = 48;             /* 48 * 3 * ITERS bytes + warm-up stays inside the array */
    /* Either side of the branch retires five instructions, plus the loop's
     * two: checked on every run, whichever way the bits fall. */
    o.expect_ins = 7;
    ua_meas m = ua_exp_measure(emit_loop, NULL, &regs, level, 0, &o);
    if (out)
        *out = m;
    return m.status == UA_OK ? m.cyc : NAN;
}

void ua_exp_branch(int level, ua_exp_list *out)
{
    ua_exp_result *r = ua_exp_add(out, "mispredict_penalty", "Branch misprediction penalty",
                                  "cycles", level);
    uint8_t *buf = ua_big_buffer();
    if (!buf) {
        ua_exp_note(r, "Could not map the data buffer.");
        return;
    }
    uint8_t *random = buf, *pattern = buf + ARRAY_BYTES;
    uint64_t x = 0x2545f4914f6cdd1dull;
    for (size_t i = 0; i < ARRAY_BYTES; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        random[i] = (uint8_t)(x >> 33);
        pattern[i] = (uint8_t)(i & 1);
    }
    ua_meas mp, mr;
    double pred = loop_cycles(pattern, level, &mp);
    double rnd = loop_cycles(random, level, &mr);
    if (isnan(pred) || isnan(rnd)) {
        ua_exp_note(r, "Measurement was not clean.");
        return;
    }
    double penalty = 2.0 * (rnd - pred);
    r->status = UA_EXP_OK;
    r->value = penalty;
    r->lo = r->hi = penalty;
    snprintf(r->confidence, sizeof r->confidence, "medium");
    ua_exp_note(r,
                "One iteration takes %.2f cycles when the branch alternates and %.2f when it "
                "follows random bits; twice the difference is the cost of one misprediction, "
                "assuming half of the random branches are mispredicted (that rate cannot be "
                "read without PMU events, hence the confidence). The branch's condition is "
                "ready before it is renamed, so this is the shortest penalty the pipeline has.",
                pred, rnd);
}
