/*
 * exp_width.c - pipeline width and execution-unit counts.
 *
 * Width: a long run of NOPs needs no execution unit and no physical
 * register, so its sustained rate is the narrowest of fetch, decode, rename
 * and retire.
 *
 * Units: a pure stream of one kind of instruction does not always reach the
 * number of units that can execute it, because uops are assigned to
 * schedulers when they are dispatched and the assignment is not perfectly
 * balanced.  Diluting the stream with NOPs (r instructions of the kind under
 * test in every group of W) gives the balancer slack; the highest rate seen
 * over all r is the estimate for the number of units.
 */
#include "enc.h"
#include "exp.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t (*gen_fn)(unsigned j);

/* j counts instances of the instruction under test. */
static uint32_t g_add_imm(unsigned j) { return a64_add_imm(j % 8, 19 + j % 8, 1); }
static uint32_t g_add_reg(unsigned j)
{
    return a64_add(j % 8, 19 + j % 8, 19 + (3 * j + 1) % 8);
}
static uint32_t g_add_same(unsigned j) { return a64_add(j % 8, 19, 19); }
static uint32_t g_cmp_imm(unsigned j) { return a64_cmp_imm(19 + j % 8, 1); }
static uint32_t g_mul(unsigned j) { return a64_mul(j % 8, 19 + j % 8, 19 + (3 * j + 1) % 8); }
static uint32_t g_load(unsigned j) { return a64_ldr(j % 8, UA_REG_MEM, 8 * (j % 16)); }
static uint32_t g_store(unsigned j) { return a64_str(19 + j % 8, UA_REG_MEM, 8 * (j % 32)); }
static uint32_t g_fadd(unsigned j) { return a64_fadd_d(j % 8, 12 + j % 4, 12 + (j + 1) % 4); }
static uint32_t g_fmul(unsigned j) { return a64_fmul_d(j % 8, 12 + j % 4, 12 + (j + 1) % 4); }
static uint32_t g_vadd(unsigned j) { return a64_vadd_2d(j % 8, 12 + j % 4, 12 + (j + 1) % 4); }
static uint32_t g_cbz_nt(unsigned j) { return a64_cbz(19 + j % 8, 1); }
static uint32_t g_b_taken(unsigned j)
{
    (void)j;
    return a64_b(1);
}
static uint32_t g_mov(unsigned j) { return a64_mov(j % 8, 19 + j % 8); }

typedef struct {
    gen_fn gen;
    unsigned r, w, groups;
} mix_ctx;

static void emit_mix(void *p)
{
    const mix_ctx *c = p;
    unsigned j = 0;
    for (unsigned g = 0; g < c->groups; g++)
        for (unsigned i = 0; i < c->w; i++)
            ua_jit_put(i < c->r ? c->gen(j++) : a64_nop());
}

/* Sustained instructions per cycle for r test instructions in every group
 * of w, by differencing two loop lengths. */
static double mix_ipc(gen_fn gen, unsigned r, unsigned w, int level)
{
    ua_regs regs;
    mix_ctx c = {gen, r, w, 0};
    double cyc[2];
    for (int k = 0; k < 2; k++) {
        c.groups = (k + 1) * (480 / w);
        ua_regs_default(&regs);
        cyc[k] = ua_exp_cycles(emit_mix, &c, &regs, level);
    }
    if (isnan(cyc[0]) || isnan(cyc[1]) || cyc[1] <= cyc[0])
        return NAN;
    return (double)((480 / w) * w) / (cyc[1] - cyc[0]);
}

static void units(ua_exp_list *out, int level, unsigned w, const char *id, const char *title,
                  gen_fn gen, const char *what)
{
    ua_exp_result *r = ua_exp_add(out, id, title, "per cycle", level);
    snprintf(r->xlabel, sizeof r->xlabel, "instructions of this kind per %u", w);
    snprintf(r->ylabel, sizeof r->ylabel, "executed per cycle");
    double best = NAN, pure = NAN;
    for (unsigned k = 1; k <= w; k++) {
        double ipc = mix_ipc(gen, k, w, level);
        if (isnan(ipc))
            continue;
        double rate = ipc * k / w;
        ua_exp_point(r, k, rate);
        if (isnan(best) || rate > best)
            best = rate;
        if (k == w)
            pure = rate;
    }
    if (isnan(best))
        return;
    r->status = UA_EXP_OK;
    r->value = best;
    r->lo = pure;
    r->hi = best;
    double nearest = round(best);
    int integral = fabs(best - nearest) < 0.04 * nearest;
    snprintf(r->confidence, sizeof r->confidence, integral ? "high" : "medium");
    ua_exp_note(r,
                "Highest sustained rate of %s over all dilutions with NOPs; an undiluted stream "
                "reaches %.2f.%s",
                what, pure,
                integral ? "" : " Not close to an integer: the unit count is probably the next "
                                "integer up, not reached because of scheduler balancing.");
}

void ua_exp_width(int level, ua_exp_list *out)
{
    /* Width from NOPs.  (Two lengths again, so the loop branch cancels.) */
    ua_exp_result *r = ua_exp_add(out, "width", "Pipeline width (sustained NOPs per cycle)",
                                  "per cycle", level);
    ua_regs regs;
    double cyc[2];
    static const unsigned lens[2] = {600, 1200};
    for (int k = 0; k < 2; k++) {
        mix_ctx c = {g_add_imm, 0, 1, lens[k]};
        ua_regs_default(&regs);
        cyc[k] = ua_exp_cycles(emit_mix, &c, &regs, level);
    }
    unsigned w = 8;
    if (!isnan(cyc[0]) && !isnan(cyc[1]) && cyc[1] > cyc[0]) {
        double ipc = 600.0 / (cyc[1] - cyc[0]);
        r->status = UA_EXP_OK;
        r->value = ipc;
        r->lo = r->hi = ipc;
        snprintf(r->confidence, sizeof r->confidence,
                 fabs(ipc - round(ipc)) < 0.05 ? "high" : "medium");
        ua_exp_note(r, "NOPs use no execution unit, so this is the narrowest of fetch, decode, "
                       "rename and retire. It bounds every other rate on this page.");
        w = (unsigned)lround(ipc);
        if (w < 2)
            w = 2;
        if (w > 16)
            w = 16;
    }

    units(out, level, w, "units_mov", "Register moves per cycle (mov x, x)", g_mov,
          "independent register moves");
    units(out, level, w, "units_alu", "Integer ALUs (add with immediate)", g_add_imm,
          "independent add-immediate instructions");
    units(out, level, w, "units_alu_2src", "Integer adds with two register sources", g_add_reg,
          "independent register-register adds whose sources rotate through eight registers");
    units(out, level, w, "units_alu_same_src", "Integer adds that all read one register",
          g_add_same, "add xN, x19, x19 (every instance reads the same register twice)");
    units(out, level, w, "units_flags", "Flag-setting ALUs (cmp with immediate)", g_cmp_imm,
          "independent compares");
    units(out, level, w, "units_mul", "Integer multipliers", g_mul, "independent multiplies");
    units(out, level, w, "units_load", "Load units (L1 hits)", g_load,
          "independent loads that hit the L1 cache");
    units(out, level, w, "units_store", "Store units", g_store,
          "independent stores to distinct addresses");
    units(out, level, w, "units_fp_add", "FP adders (fadd)", g_fadd, "independent scalar fadd");
    units(out, level, w, "units_fp_mul", "FP multipliers (fmul)", g_fmul,
          "independent scalar fmul");
    units(out, level, w, "units_simd_int", "SIMD integer units (add v.2d)", g_vadd,
          "independent vector integer adds");
    units(out, level, w, "units_branch_nt", "Not-taken conditional branches per cycle (cbz)",
          g_cbz_nt, "never-taken cbz");
    units(out, level, w, "units_branch_taken", "Taken branches per cycle (b to next)", g_b_taken,
          "unconditional branches to the next instruction");
}
