/*
 * exp_elim.c - what the renamer removes: register moves and zero idioms.
 *
 * Moves.  A dependency chain of adds takes one cycle per add.  Inserting a
 * register move between two adds lengthens the chain by the move's latency,
 * which is zero if the renamer simply points the destination at the source's
 * physical register.  Two shapes are measured because they differ on Apple
 * cores: a move fed by a real operation, and a move fed by another move.
 *
 * Zero idioms.  `eor x0, x0, x0` always produces zero.  A core that
 * recognises this breaks the dependency on the old x0.  The test puts the
 * candidate between the links of a slow multiply chain: if the dependency is
 * broken the chain falls apart and the loop runs at throughput speed.
 */
#include "enc.h"
#include "exp.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define STEPS 24

typedef void (*step_fn)(int arg);

typedef struct {
    step_fn step;
    int arg;
    int steps;
} chain_ctx;

static void emit_chain(void *p)
{
    const chain_ctx *c = p;
    for (int i = 0; i < c->steps; i++)
        c->step(c->arg);
}

/* Cycles per step, by differencing two chain lengths. */
static double per_step(step_fn step, int arg, int level)
{
    double cyc[2];
    for (int k = 0; k < 2; k++) {
        ua_regs regs;
        chain_ctx c = {step, arg, STEPS * (k + 1)};
        ua_regs_default(&regs);
        cyc[k] = ua_exp_cycles(emit_chain, &c, &regs, level);
    }
    if (isnan(cyc[0]) || isnan(cyc[1]))
        return NAN;
    return (cyc[1] - cyc[0]) / STEPS;
}

/* ---- integer moves ---- */
static void s_add_only(int a)
{
    (void)a;
    ua_jit_put(a64_add_imm(0, 0, 1));
}
static void s_add_mov(int a)
{
    (void)a;
    ua_jit_put(a64_add_imm(1, 0, 1));
    ua_jit_put(a64_mov(0, 1));
}
/* add, then k moves through distinct registers and back. */
static void s_add_movs(int k)
{
    ua_jit_put(a64_add_imm(0, 0, 1));
    for (int j = 0; j < k; j++)
        ua_jit_put(a64_mov(j + 1, j));
    ua_jit_put(a64_mov(0, k));
}
static void s_add_mov_self(int a)
{
    (void)a;
    ua_jit_put(a64_add_imm(0, 0, 1));
    ua_jit_put(a64_mov(0, 0));
}
static void s_add_add0(int a)
{
    (void)a;
    ua_jit_put(a64_add_imm(1, 0, 1));
    ua_jit_put(a64_add_imm(0, 1, 0));
}

/* ---- FP / SIMD moves ---- */
static void s_fadd_only(int a)
{
    (void)a;
    ua_jit_put(a64_vadd_2d(0, 0, 12));
}
static void s_vadd_fmov(int a)
{
    (void)a;
    ua_jit_put(a64_vadd_2d(1, 0, 12));
    ua_jit_put(a64_fmov_dd(0, 1));
}
static void s_vadd_vmov(int a)
{
    (void)a;
    ua_jit_put(a64_vadd_2d(1, 0, 12));
    ua_jit_put(a64_vmov(0, 1));
}
static void s_vadd_vmovs(int k)
{
    ua_jit_put(a64_vadd_2d(0, 0, 12));
    for (int j = 0; j < k; j++)
        ua_jit_put(a64_vmov(j + 1, j));
    ua_jit_put(a64_vmov(0, k));
}

/* ---- zero idioms between the links of a multiply chain ---- */
static void s_mul_only(int a)
{
    (void)a;
    ua_jit_put(a64_mul(0, 0, 20));
}
static void s_mul_idiom(int which)
{
    ua_jit_put(a64_mul(0, 0, 20));
    switch (which) {
    case 0: ua_jit_put(a64_eor(0, 0, 0)); break;      /* eor x0, x0, x0 */
    case 1: ua_jit_put(a64_sub(0, 0, 0)); break;      /* sub x0, x0, x0 */
    case 2: ua_jit_put(a64_and(0, 0, A64_ZR)); break; /* and x0, x0, xzr */
    case 3: ua_jit_put(a64_movz(0, 0, 0)); break;     /* mov x0, #0 */
    }
}
static void s_fmul_only(int a)
{
    (void)a;
    ua_jit_put(a64_fmul_d(0, 0, 12));
}
static void s_fmul_idiom(int which)
{
    ua_jit_put(a64_fmul_d(0, 0, 12));
    if (which == 0)
        ua_jit_put(a64_veor(0, 0, 0)); /* eor v0.16b, v0.16b, v0.16b */
    else
        ua_jit_put(a64_movi_zero(0));  /* movi v0.2d, #0 */
}

static void report(ua_exp_list *out, int level, const char *id, const char *title, double value,
                   const char *note_fmt, double a, double b)
{
    ua_exp_result *r = ua_exp_add(out, id, title, "cycles", level);
    if (isnan(value)) {
        ua_exp_note(r, "Measurement was not clean.");
        return;
    }
    if (fabs(value) < 0.005)
        value = 0; /* no "-0.00" */
    r->status = UA_EXP_OK;
    r->value = value;
    r->lo = r->hi = value;
    snprintf(r->confidence, sizeof r->confidence, "high");
    ua_exp_note(r, note_fmt, a, b);
}

void ua_exp_elim(int level, ua_exp_list *out)
{
    double add = per_step(s_add_only, 0, level);
    double add_mov = per_step(s_add_mov, 0, level);
    double self = per_step(s_add_mov_self, 0, level);
    double add0 = per_step(s_add_add0, 0, level);
    double m2 = per_step(s_add_movs, 2, level);
    double m8 = per_step(s_add_movs, 8, level);

    report(out, level, "mov_after_op", "Latency of mov x, x fed by an add", add_mov - add,
           "Chain of add + mov takes %.2f cycles per step against %.2f for the add alone. Zero "
           "means the move is eliminated at rename.", add_mov, add);
    report(out, level, "mov_after_mov", "Latency of mov x, x fed by another mov", (m8 - m2) / 6.0,
           "Each extra move in a run of moves adds this much (chains of 3 and 9 moves: %.2f and "
           "%.2f cycles per step). A value just below 1 means a move whose source is itself a "
           "move renamed in the same cycle is executed, not eliminated.", m2, m8);
    report(out, level, "mov_self", "Latency of mov x0, x0", self - add,
           "add + mov x0, x0 takes %.2f cycles per step (add alone: %.2f).", self, add);
    report(out, level, "add_imm0", "Latency of add x, x, #0", add0 - add,
           "add x1, x0, #1 ; add x0, x1, #0 takes %.2f cycles per step (add alone: %.2f). Zero "
           "means add #0 is treated as a move.", add0, add);

    double vadd = per_step(s_fadd_only, 0, level);
    double v_fmov = per_step(s_vadd_fmov, 0, level);
    double v_vmov = per_step(s_vadd_vmov, 0, level);
    double v2 = per_step(s_vadd_vmovs, 2, level);
    double v8 = per_step(s_vadd_vmovs, 8, level);
    report(out, level, "vmov_after_op", "Latency of mov v.16b, v.16b fed by a vector add",
           v_vmov - vadd,
           "Chain of add v.2d + mov v takes %.2f cycles per step against %.2f for the add alone.",
           v_vmov, vadd);
    report(out, level, "vmov_after_mov", "Latency of mov v.16b, v.16b fed by another mov",
           (v8 - v2) / 6.0,
           "Each extra vector move in a run of moves adds this much (chains of 3 and 9: %.2f and "
           "%.2f cycles per step).", v2, v8);
    report(out, level, "fmov_after_op", "Latency of fmov d, d fed by a vector add", v_fmov - vadd,
           "Chain of add v.2d + fmov d, d takes %.2f cycles per step against %.2f for the add "
           "alone. fmov d, d must clear the upper 64 bits, so it is not a plain rename.",
           v_fmov, vadd);

    double mul = per_step(s_mul_only, 0, level);
    static const struct {
        const char *id, *title;
    } idioms[] = {
        {"zero_eor_self", "eor x0, x0, x0 inside a multiply chain"},
        {"zero_sub_self", "sub x0, x0, x0 inside a multiply chain"},
        {"zero_and_zr", "and x0, x0, xzr inside a multiply chain"},
        {"zero_movz", "mov x0, #0 inside a multiply chain"},
    };
    for (int i = 0; i < 4; i++) {
        double v = per_step(s_mul_idiom, i, level);
        report(out, level, idioms[i].id, idioms[i].title, v,
               "Cycles per step of mul x0, x0, x20 followed by the idiom. The multiply alone "
               "takes %.2f; a value below that means the idiom broke the dependency on x0, a "
               "value above it means the idiom was executed as an ordinary operation (%.2f).",
               mul, v);
    }
    double fmul = per_step(s_fmul_only, 0, level);
    double veor = per_step(s_fmul_idiom, 0, level);
    double movi = per_step(s_fmul_idiom, 1, level);
    report(out, level, "zero_veor_self", "eor v0, v0, v0 inside an fmul chain", veor,
           "fmul alone takes %.2f cycles per step; below that means the dependency was broken "
           "(%.2f).", fmul, veor);
    report(out, level, "zero_movi", "movi v0.2d, #0 inside an fmul chain", movi,
           "fmul alone takes %.2f cycles per step; movi has no input, so this is the reference "
           "for a broken dependency (%.2f).", fmul, movi);
}
