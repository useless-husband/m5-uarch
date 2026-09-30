#include "insn.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Instances per throughput loop iteration: enough that the loop's own
 * subs/b.ne pair is well under one percent of the work. */
#define TP_MIN_INSTANCES 320
/* Chain steps per latency loop iteration.  Odd on purpose: a chain whose
 * condition alternates from step to step then also alternates at every code
 * address from one iteration to the next, so no instruction sees a constant
 * condition (relevant for conditional selects, see DESIGN.md). */
#define LAT_STEPS 31

/* Nodes in a memory cycle.  Prime, so that every one of the LAT_STEPS load
 * sites walks the whole cycle instead of a short sub-sequence that a value
 * predictor could learn. */
#define CYCLE_NODES 509
#define CYCLE_NODES_BYTE 127 /* also fits a sign-extending byte load */
#define CYCLE_SPACING 64

int ua_insns_verify(char *err, size_t errsz)
{
    size_t words = (size_t)(ua_code_end - ua_code);
    if (words != ua_code_words) {
        snprintf(err, errsz, "assembled table has %zu words, generator expected %u", words,
                 ua_code_words);
        return -1;
    }
    for (size_t i = 0; i < ua_n_code_marks; i++) {
        uint32_t want = UA_CODE_MARK | (uint32_t)(i & 0xffff);
        if (ua_code_marks[i] >= words || ua_code[ua_code_marks[i]] != want) {
            snprintf(err, errsz, "blob marker %zu missing at word %u", i, ua_code_marks[i]);
            return -1;
        }
    }
    return 0;
}

const ua_insn *ua_insn_find(const char *name)
{
    for (size_t i = 0; i < ua_n_insns; i++)
        if (strcmp(ua_insns[i].name, name) == 0)
            return &ua_insns[i];
    return NULL;
}

/* A single random cycle over 0..n-1 (Sattolo's algorithm), fixed seed. */
static void random_cycle(uint16_t *next, unsigned n)
{
    static uint16_t perm[CYCLE_NODES];
    uint64_t x = 0x9e3779b97f4a7c15ull;
    for (unsigned i = 0; i < n; i++)
        perm[i] = (uint16_t)i;
    for (unsigned i = n - 1; i > 0; i--) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        unsigned j = (unsigned)(x % i); /* j < i: guarantees one cycle */
        uint16_t t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }
    /* perm is a cyclic order: node perm[i] is followed by perm[i+1]. */
    for (unsigned i = 0; i < n; i++)
        next[perm[i]] = perm[(i + 1) % n];
}

static void store_le(uint8_t *p, uint64_t v, unsigned size)
{
    memcpy(p, &v, size); /* little-endian host */
}

void ua_init_apply(ua_regs *regs, const ua_init *init, size_t n)
{
    static uint16_t next[CYCLE_NODES];
    uint8_t *mid = ua_scratch_mid();
    for (size_t i = 0; i < n; i++) {
        const ua_init *it = &init[i];
        uint64_t v;
        switch (it->kind) {
        case UA_INIT_GPR:
            if (it->reg < 31)
                regs->x[it->reg] = it->val;
            break;
        case UA_INIT_GPR_PTR:
            if (it->reg < 31)
                regs->x[it->reg] = (uint64_t)(uintptr_t)mid + it->val;
            break;
        case UA_INIT_VEC:
            if (it->reg < 32) {
                memcpy(&regs->v[it->reg][0], &it->val, 8);
                memcpy(&regs->v[it->reg][8], &it->val2, 8);
            }
            break;
        case UA_INIT_VEC_ALL:
            for (int r = 0; r < 32; r++) {
                memcpy(&regs->v[r][0], &it->val, 8);
                memcpy(&regs->v[r][8], &it->val2, 8);
            }
            break;
        case UA_INIT_MEM:
            if (it->off >= -(int32_t)(UA_SCRATCH_BYTES / 2) &&
                it->off <= (int32_t)(UA_SCRATCH_BYTES / 2) - 8)
                memcpy(mid + it->off, &it->val, 8);
            break;
        case UA_INIT_MEM_PTR:
            v = (uint64_t)(uintptr_t)mid + it->val;
            if (it->off >= -(int32_t)(UA_SCRATCH_BYTES / 2) &&
                it->off <= (int32_t)(UA_SCRATCH_BYTES / 2) - 8)
                memcpy(mid + it->off, &v, 8);
            break;
        case UA_INIT_NZCV:
            regs->nzcv = it->val;
            break;
        case UA_INIT_CYCLE_PTR:
            /* Nodes at mid + k*64; the cell at node + off holds the address
             * of the next node. */
            if (it->reg >= 31 || it->off < 0 || it->off > 192)
                break;
            random_cycle(next, CYCLE_NODES);
            for (unsigned k = 0; k < CYCLE_NODES; k++) {
                v = (uint64_t)(uintptr_t)mid + (uint64_t)next[k] * CYCLE_SPACING;
                memcpy(mid + k * CYCLE_SPACING + it->off, &v, 8);
            }
            regs->x[it->reg] = (uint64_t)(uintptr_t)mid;
            break;
        case UA_INIT_CYCLE_IDX: {
            /* The cell at mid + off + index*scale holds the next index.
             * Byte-wide cells can only hold 0..255, so they sit one byte
             * apart; wider cells sit one cache line apart. */
            unsigned scale = (unsigned)it->val, size = (unsigned)it->val2;
            unsigned nodes = size == 1 ? CYCLE_NODES_BYTE : CYCLE_NODES;
            unsigned spacing = size == 1 ? scale : CYCLE_SPACING;
            if (it->reg >= 31 || !scale || spacing % scale || it->off < 0 || it->off > 1024)
                break;
            random_cycle(next, nodes);
            for (unsigned k = 0; k < nodes; k++)
                store_le(mid + it->off + k * spacing, (uint64_t)next[k] * (spacing / scale), size);
            regs->x[it->reg] = 0;
            break;
        }
        default:
            break;
        }
    }
}

/* Reset the scratch buffer so that one entry's stores cannot influence the
 * next entry's loads. */
static void scratch_reset(void) { memset(ua_scratch_base(), 0, UA_SCRATCH_BYTES); }

static ua_meas run_block(const ua_insn *in, uint32_t off, size_t n, unsigned reps,
                         const ua_init *init, size_t n_init, int level)
{
    ua_regs regs;
    uint64_t per_iter = 0;
    scratch_reset();
    ua_regs_default(&regs);
    ua_init_apply(&regs, init, n_init);
    const void *code = ua_jit_loop(ua_code + in->pre_off, in->pre_n, ua_code + off, n, reps,
                                   ua_code + in->post_off, in->post_n, &per_iter);
    ua_mopts o = ua_mopts_default(level);
    o.expect_ins = per_iter;
    o.flip_nzcv = (in->flags & UA_INSN_FLIP) != 0;
    return ua_measure(code, &regs, &o);
}

void ua_helpers_measure(int level, ua_helpers *h)
{
    static const struct {
        const char *name;
        size_t field;
    } rt[] = {
        {"rt_cmp_csinc", offsetof(ua_helpers, cmp_csinc)},
        {"rt_fmov_x_d", offsetof(ua_helpers, fmov_rt)},
        {"rt_fcmp_fcsel", offsetof(ua_helpers, fcmp_fcsel)},
    };
    memset(h, 0, sizeof *h);
    for (int i = 0; i < UA_HELP__COUNT; i++)
        h->share[i] = NAN;
    h->share[UA_HELP_NONE] = 0;
    h->valid = 1;
    for (size_t i = 0; i < sizeof rt / sizeof rt[0]; i++) {
        double *dst = (double *)((char *)h + rt[i].field);
        const ua_insn *in = ua_insn_find(rt[i].name);
        *dst = NAN;
        if (!in || in->n_chains < 1) {
            h->valid = 0;
            continue;
        }
        const ua_chain *ch = &in->chains[0];
        ua_meas m1 = run_block(in, ch->off, ch->n, LAT_STEPS, ch->init, ch->n_init, level);
        ua_meas m3 = run_block(in, ch->off, ch->n, 3 * LAT_STEPS, ch->init, ch->n_init, level);
        if (m1.status == UA_OK && m3.status == UA_OK)
            *dst = (m3.cyc - m1.cyc) / (2.0 * LAT_STEPS * (double)ch->steps);
        else
            h->valid = 0;
    }
    /*
     * A round trip of two non-eliminable operations that takes two cycles
     * can only be 1 + 1.  That argument pins the integer<->flags helpers.
     * The integer<->FP and FP<->flags round trips take longer and cannot be
     * split from timing alone, so their share stays unknown and latencies
     * measured through them are reported as round trips.
     */
    if (fabs(h->cmp_csinc - 2.0) < 0.05) {
        h->share[UA_HELP_CMP] = 1.0;
        h->share[UA_HELP_CSINC] = 1.0;
    }
    if (fabs(h->fcmp_fcsel - 2.0) < 0.05) {
        h->share[UA_HELP_FCMP] = 1.0;
        h->share[UA_HELP_FCSEL] = 1.0;
    }
    if (fabs(h->fmov_rt - 2.0) < 0.05) {
        h->share[UA_HELP_FMOV_XD] = 1.0;
        h->share[UA_HELP_FMOV_DX] = 1.0;
    }
}

void ua_insn_run(const ua_insn *in, int level, const ua_helpers *h, ua_insn_result *out)
{
    memset(out, 0, sizeof *out);
    out->insn = in;
    out->level = level;
    out->supported = 1;

    if (in->tp_n) {
        /* Two loop lengths: the difference is the cost of `reps` more copies
         * of the block, free of the loop branch and its fetch bubble. */
        unsigned reps = (TP_MIN_INSTANCES + in->tp_inst - 1) / in->tp_inst;
        ua_meas m1 = run_block(in, in->tp_off, in->tp_n, reps, in->tp_init, in->tp_n_init, level);
        if (m1.status == UA_FAULT) {
            out->tp_raw = m1;
            out->supported = 0;
            out->fault_sig = m1.fault_sig;
            return;
        }
        ua_meas m2 =
            run_block(in, in->tp_off, in->tp_n, 2 * reps, in->tp_init, in->tp_n_init, level);
        out->tp_raw = m2;
        out->have_tp = 1;
        out->tp_cpi = out->tp_ipc = NAN;
        if (m1.status == UA_OK && m2.status == UA_OK && m2.cyc > m1.cyc) {
            out->tp_cpi = (m2.cyc - m1.cyc) / ((double)in->tp_inst * reps);
            out->tp_ipc = 1.0 / out->tp_cpi;
            out->tp_raw.spread = (m1.spread * m1.cyc + m2.spread * m2.cyc) / (m2.cyc - m1.cyc);
        } else if (m2.status == UA_OK) {
            out->tp_raw.status = m1.status != UA_OK ? m1.status : UA_NOISY;
        }
    }

    double rw_lat = NAN;
    for (int c = 0; c < in->n_chains && c < UA_MAX_CHAINS; c++) {
        const ua_chain *ch = &in->chains[c];
        ua_lat_result *lr = &out->lat[out->n_lat++];
        /* Two chain lengths again: whatever happens once per loop iteration
         * (the branch, a bypass that does not span the loop edge) cancels. */
        ua_meas m1 = run_block(in, ch->off, ch->n, LAT_STEPS, ch->init, ch->n_init, level);
        lr->raw = m1;
        lr->lat = NAN;
        if (m1.status == UA_FAULT) {
            out->supported = 0;
            out->fault_sig = m1.fault_sig;
            return;
        }
        ua_meas m3 = run_block(in, ch->off, ch->n, 3 * LAT_STEPS, ch->init, ch->n_init, level);
        lr->raw = m3;
        if (m1.status != UA_OK || m3.status != UA_OK) {
            if (m3.status == UA_OK)
                lr->raw.status = m1.status;
            continue;
        }
        double step = (m3.cyc - m1.cyc) / (2.0 * LAT_STEPS * (double)ch->steps);
        lr->raw.spread = (m1.spread * m1.cyc + m3.spread * m3.cyc) / (m3.cyc - m1.cyc);
        lr->per_iter_extra = m1.cyc - step * LAT_STEPS * (double)ch->steps;
        double share = h ? h->share[ch->helper] : (ch->helper ? NAN : 0.0);
        step -= ch->extra;
        if (isnan(share)) {
            lr->lat = step;
            lr->roundtrip = 1;
        } else {
            lr->lat = step - share;
        }
        /* The chain through a read-write operand itself. */
        if (isnan(rw_lat) && strncmp(ch->from, "RW", 2) == 0 && strcmp(ch->from, ch->to) == 0)
            rw_lat = lr->lat;
    }

    /* Is the throughput number really a latency?  With c parallel chains in
     * the block, an instruction of latency L cannot go faster than L/c cycles
     * per instance, however many ports it has. */
    if (out->have_tp && !isnan(out->tp_cpi)) {
        if (in->flags & UA_INSN_TP_SERIAL) {
            out->tp_chain_bound = 1;
        } else if (in->tp_chains && !isnan(rw_lat)) {
            double floor = rw_lat / in->tp_chains;
            if (out->tp_cpi < floor * 1.06)
                out->tp_chain_bound = 1;
        }
    }
}
