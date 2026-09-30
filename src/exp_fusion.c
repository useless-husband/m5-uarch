/*
 * exp_fusion.c - instruction pairs that execute as one operation.
 *
 * Without micro-op counters, fusion has to show up in time.  Two tests:
 *
 * Unit pressure (compare-and-branch and friends).  A block is the pair plus
 * k independent adds.  If the pair is fused, the block needs one execution
 * slot fewer than the same instructions with the pair pulled apart by one of
 * the adds.  With enough adds to keep all integer units busy the difference
 * shows as a higher rate for the adjacent form.  Both forms contain exactly
 * the same instructions, only the order differs.
 *
 * Latency (AES).  aese + aesmc as a dependency chain, adjacent and with a
 * NOP in between, taken from the instruction table.
 */
#include "enc.h"
#include "exp.h"
#include "insn.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *id, *title, *text;
    uint32_t first, second;
} pair_def;

typedef struct {
    const pair_def *p;
    unsigned k;      /* adds per block */
    unsigned groups;
    int adjacent;
} fuse_ctx;

static void emit_blocks(void *ctx)
{
    const fuse_ctx *c = ctx;
    unsigned j = 0;
    for (unsigned g = 0; g < c->groups; g++) {
        ua_jit_put(c->p->first);
        if (!c->adjacent)
            ua_jit_put(a64_add_imm(8 + j % 8, 19 + j % 8, 1)), j++;
        ua_jit_put(c->p->second);
        for (unsigned i = c->adjacent ? 0 : 1; i < c->k; i++, j++)
            ua_jit_put(a64_add_imm(8 + j % 8, 19 + j % 8, 1));
    }
}

/* Blocks per cycle, by differencing two loop lengths. */
static double block_rate(const pair_def *p, unsigned k, int adjacent, int level)
{
    double cyc[2];
    for (int i = 0; i < 2; i++) {
        ua_regs regs;
        fuse_ctx c = {p, k, 48u * (unsigned)(i + 1), adjacent};
        ua_regs_default(&regs);
        cyc[i] = ua_exp_cycles(emit_blocks, &c, &regs, level);
    }
    if (isnan(cyc[0]) || isnan(cyc[1]) || cyc[1] <= cyc[0])
        return NAN;
    return 48.0 / (cyc[1] - cyc[0]);
}

static void pressure(ua_exp_list *out, int level, const pair_def *p)
{
    ua_exp_result *r = ua_exp_add(out, p->id, p->title, "ratio", level);
    snprintf(r->xlabel, sizeof r->xlabel, "independent adds per block");
    snprintf(r->ylabel, sizeof r->ylabel, "rate adjacent / rate separated");
    double best = NAN, best_adj = NAN, best_sep = NAN;
    unsigned best_k = 0;
    for (unsigned k = 1; k <= 9; k++) {
        double adj = NAN, sep = NAN;
        for (int attempt = 0; attempt < 3 && (isnan(adj) || isnan(sep)); attempt++) {
            adj = block_rate(p, k, 1, level);
            sep = block_rate(p, k, 0, level);
        }
        if (isnan(adj) || isnan(sep))
            continue;
        double ratio = adj / sep;
        ua_exp_point(r, k, ratio);
        if (isnan(best) || ratio > best) {
            best = ratio;
            best_k = k;
            best_adj = adj;
            best_sep = sep;
        }
    }
    if (isnan(best)) {
        ua_exp_note(r, "Measurement was not clean.");
        return;
    }
    r->status = UA_EXP_OK;
    r->value = best;
    r->lo = r->hi = best;
    int fused = best > 1.05;
    snprintf(r->confidence, sizeof r->confidence, fused ? "high" : "medium");
    ua_exp_note(r,
                "%s. With %u adds per block, %.2f blocks per cycle when the pair is adjacent and "
                "%.2f when one add separates it. %s",
                p->text, best_k, best_adj, best_sep,
                fused ? "The adjacent form needs fewer execution slots: the pair is fused."
                      : "No difference at any block size: either the pair is not fused or the "
                        "two instructions never compete for the same units; this test cannot "
                        "tell those apart.");
}

static void aes(ua_exp_list *out, int level)
{
    ua_exp_result *r = ua_exp_add(out, "fusion_aese_aesmc", "aese + aesmc as one operation",
                                  "cycles", level);
    const ua_insn *adj = ua_insn_find("aes_round_enc"), *sep = ua_insn_find("aes_round_enc_split");
    const ua_insn *e = ua_insn_find("aese"), *mc = ua_insn_find("aesmc");
    if (!adj || !sep || !e || !mc) {
        ua_exp_note(r, "Instruction table entries missing.");
        return;
    }
    ua_insn_result ra, rs, re, rm;
    ua_insn_run(adj, level, NULL, &ra);
    ua_insn_run(sep, level, NULL, &rs);
    ua_insn_run(e, level, NULL, &re);
    ua_insn_run(mc, level, NULL, &rm);
    if (!ra.supported || !rs.supported || ra.n_lat < 1 || rs.n_lat < 1 || isnan(ra.lat[0].lat) ||
        isnan(rs.lat[0].lat)) {
        r->status = UA_EXP_INCONCLUSIVE;
        ua_exp_note(r, "AES instructions could not be measured on this CPU.");
        return;
    }
    r->status = UA_EXP_OK;
    r->value = ra.lat[0].lat;
    r->lo = ra.lat[0].lat;
    r->hi = rs.lat[0].lat;
    snprintf(r->confidence, sizeof r->confidence, "high");
    ua_exp_note(r,
                "Latency of one AES round through the state: %.2f cycles for aese immediately "
                "followed by aesmc, %.2f with a NOP between them (aese alone %.2f, aesmc alone "
                "%.2f). %s",
                ra.lat[0].lat, rs.lat[0].lat, re.n_lat ? re.lat[0].lat : NAN,
                rm.n_lat ? rm.lat[0].lat : NAN,
                rs.lat[0].lat - ra.lat[0].lat > 0.7
                    ? "The adjacent pair runs as a single operation."
                    : "No fusion visible.");
}

void ua_exp_fusion(int level, ua_exp_list *out)
{
    /* Branches target the next instruction and are never taken: x19 != x20,
     * and every flag-setting result below is non-zero. */
    const pair_def pairs[] = {
        {"fusion_cmp_bcc", "cmp + b.cond", "cmp x19, x20 ; b.eq", a64_cmp(19, 20),
         a64_bcond(A64_EQ, 1)},
        {"fusion_cmp_imm_bcc", "cmp #imm + b.cond", "cmp x19, #1 ; b.eq", a64_cmp_imm(19, 1),
         a64_bcond(A64_EQ, 1)},
        {"fusion_adds_bcc", "adds + b.cond", "adds x3, x19, x20 ; b.eq", a64_adds(3, 19, 20),
         a64_bcond(A64_EQ, 1)},
        {"fusion_subs_imm_bcc", "subs #imm + b.cond", "subs x3, x19, #1 ; b.eq",
         a64_subs_imm(3, 19, 1), a64_bcond(A64_EQ, 1)},
        {"fusion_ands_bcc", "ands + b.cond", "ands x3, x19, x20 ; b.eq", a64_ands(3, 19, 20),
         a64_bcond(A64_EQ, 1)},
        {"fusion_tst_bcc", "tst + b.cond", "tst x19, x20 ; b.eq", a64_ands(A64_ZR, 19, 20),
         a64_bcond(A64_EQ, 1)},
        {"fusion_add_cbz", "add + cbz", "add x3, x19, #1 ; cbz x3", a64_add_imm(3, 19, 1),
         a64_cbz(3, 1)},
        {"fusion_and_cbz", "and + cbz", "and x3, x19, x20 ; cbz x3", a64_and(3, 19, 20),
         a64_cbz(3, 1)},
        {"fusion_adrp_add", "adrp + add", "adrp x3, . ; add x3, x3, #1", a64_adrp0(3),
         a64_add_imm(3, 3, 1)},
        /* Controls: pairs that cannot fuse.  If the method is sound they
         * come out at 1. */
        {"fusion_control_add_bcc", "add + b.cond on older flags (control)",
         "add x3, x19, #1 ; b.eq", a64_add_imm(3, 19, 1), a64_bcond(A64_EQ, 1)},
        {"fusion_control_cmp_csinc", "cmp + csinc (control)", "cmp x19, x20 ; csinc x3, x21, x22, ne",
         a64_cmp(19, 20), a64_csinc(3, 21, 22, A64_NE)},
    };
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++)
        pressure(out, level, &pairs[i]);
    aes(out, level);
}
