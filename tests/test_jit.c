/*
 * test_jit.c - functional tests for generated code.  No counters needed, so
 * this runs everywhere, including virtual machines.
 *
 * It checks that the trampoline passes register state in and out, that code
 * built with the encoder computes what it should, that faults are contained,
 * and that every entry of the instruction table either runs or is refused
 * by the CPU as an illegal instruction: never a crash, never a wild address.
 */
#include "check.h"
#include "enc.h"
#include "exp.h"
#include "insn.h"
#include "jit.h"
#include "measure.h"

#include <signal.h>
#include <string.h>

#define SEED 0x0123456789abcdefull

static void fill_regs(ua_regs *r, unsigned long long *s)
{
    ua_regs_default(r);
    for (int i = 0; i < 27; i++)
        if (i != 18)
            r->x[i] = test_rand(s);
    for (int i = 0; i < 32; i++)
        for (int b = 0; b < 16; b++)
            r->v[i][b] = (uint8_t)test_rand(s);
    r->nzcv = (test_rand(s) & 0xf) << 28;
}

static void test_trampoline(void)
{
    unsigned long long s = SEED;
    ua_regs r;
    fill_regs(&r, &s);
    ua_jit_begin();
    ua_jit_put(a64_ret());
    const void *code = ua_jit_end();
    CHECK(code != NULL);
    CHECK(ua_run_guarded(code, &r, 7) == 0);
    for (int i = 0; i < 28; i++)
        if (i != 18)
            CHECK(r.out_x[i] == r.x[i]);
    CHECK(r.out_nzcv == r.nzcv);
    CHECK(memcmp(r.out_v, r.v, sizeof r.v) == 0);
}

static void test_loop_and_constants(void)
{
    ua_regs r;
    ua_regs_default(&r);
    r.x[0] = 100;
    uint32_t body[1] = {a64_add_imm(0, 0, 3)};
    uint64_t per_iter = 0;
    const void *code = ua_jit_loop(NULL, 0, body, 1, 5, NULL, 0, &per_iter);
    CHECK(code != NULL);
    CHECK(per_iter == 7);
    CHECK(ua_run_guarded(code, &r, 1000) == 0);
    CHECK(r.out_x[0] == 100 + 3 * 5 * 1000);

    /* Zero iterations: nothing runs (regression: the counter used to wrap). */
    r.x[0] = 100;
    CHECK(ua_run_guarded(code, &r, 0) == 0);
    CHECK(r.out_x[0] == 100);

    static const uint64_t consts[] = {0, 1, 0xffff, 0x10000, 0x123456789abcdef0ull,
                                      0xffffffffffffffffull, 0x8000000000000000ull};
    for (size_t i = 0; i < sizeof consts / sizeof consts[0]; i++) {
        uint32_t w[4];
        int n = a64_mov_imm64(w, 5, consts[i]);
        ua_jit_begin();
        ua_jit_put_n(w, (size_t)n);
        ua_jit_put(a64_ret());
        code = ua_jit_end();
        CHECK(ua_run_guarded(code, &r, 1) == 0);
        CHECK(r.out_x[5] == consts[i]);
    }
}

static void test_xorshift_and_branches(void)
{
    ua_regs r;
    ua_regs_default(&r);
    uint64_t x = 0x9e3779b97f4a7c15ull, ones = 0;
    r.x[0] = x;
    r.x[5] = r.x[6] = 0;
    ua_jit_loop_open(NULL, 0);
    ua_jit_put(a64_eor_lsl(0, 0, 0, 13));
    ua_jit_put(a64_eor_lsr(0, 0, 0, 7));
    ua_jit_put(a64_eor_lsl(0, 0, 0, 17));
    ua_jit_put(a64_tbz(0, 40, 3)); /* bit clear: skip to the x6 increment */
    ua_jit_put(a64_add_imm(5, 5, 1));
    ua_jit_put(a64_b(2));
    ua_jit_put(a64_add_imm(6, 6, 1));
    const void *code = ua_jit_loop_close(NULL, 0);
    CHECK(ua_run_guarded(code, &r, 5000) == 0);
    for (int i = 0; i < 5000; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        ones += x >> 40 & 1;
    }
    CHECK(r.out_x[0] == x);
    CHECK(r.out_x[5] == ones);
    CHECK(r.out_x[6] == 5000 - ones);
}

static void test_memory_and_chase(void)
{
    /* Property: ua_build_chase makes one cycle through every node, and the
     * generated chase visits the same nodes as a walk in C. */
    unsigned long long s = SEED ^ 0x77;
    static uint8_t buf[64 * 1024];
    for (int trial = 0; trial < 20; trial++) {
        size_t nodes = 2 + (size_t)(test_rand(&s) % 400);
        size_t stride = 64u << (test_rand(&s) % 2);
        size_t off_mod = (trial & 1) ? 64 : 0;
        memset(buf, 0xff, sizeof buf);
        uint64_t half = 0;
        uint64_t first = ua_build_chase(buf, nodes, stride, 8, off_mod, test_rand(&s) | 1, &half);
        CHECK(first != UINT64_MAX);
        /* Walk in C: must return to the start after exactly `nodes` steps. */
        uint64_t idx = first;
        size_t steps = 0;
        int hit_half = 0;
        do {
            CHECK(idx * 8 + 8 <= sizeof buf);
            if (idx == half)
                hit_half = 1;
            memcpy(&idx, buf + idx * 8, 8);
            steps++;
        } while (idx != first && steps <= nodes);
        CHECK(steps == nodes);
        CHECK(hit_half);
        /* The same walk in generated code, 3 loads per iteration. */
        ua_regs r;
        ua_regs_default(&r);
        r.x[UA_REG_BIG] = (uint64_t)(uintptr_t)buf;
        r.x[1] = first;
        uint32_t body[1] = {a64_ldr_idx3(1, UA_REG_BIG, 1)};
        const void *code = ua_jit_loop(NULL, 0, body, 1, 3, NULL, 0, NULL);
        CHECK(ua_run_guarded(code, &r, nodes) == 0);
        CHECK(r.out_x[1] == first); /* 3 * nodes steps = three full laps */
        if (g_failures) {
            fprintf(stderr, "seed %#llx trial %d\n", SEED ^ 0x77, trial);
            return;
        }
    }
    CHECK(ua_build_chase(buf, 1, 64, 0, 0, 1, NULL) == UINT64_MAX);
}

static void test_limits_and_faults(void)
{
    /* A body too long for the loop branch is refused, not mis-encoded. */
    static uint32_t big[1] = {0xd503201fu};
    CHECK(ua_jit_loop(NULL, 0, big, 1, 300000, NULL, 0, NULL) == NULL);
    /* Arena overflow is reported. */
    ua_jit_begin();
    for (size_t i = 0; i < ua_jit_capacity() + 8; i++)
        ua_jit_put(a64_nop());
    CHECK(ua_jit_end() == NULL);
    /* An undefined instruction is caught and the next run still works. */
    ua_regs r;
    ua_regs_default(&r);
    ua_jit_begin();
    ua_jit_put(0x00000000u); /* udf #0 */
    ua_jit_put(a64_ret());
    const void *code = ua_jit_end();
    CHECK(ua_run_guarded(code, &r, 1) == SIGILL);
    /* A wild load is caught too. */
    ua_jit_begin();
    ua_jit_put(a64_movz(3, 16, 0));
    ua_jit_put(a64_ldr(4, 3, 0));
    ua_jit_put(a64_ret());
    code = ua_jit_end();
    int sig = ua_run_guarded(code, &r, 1);
    CHECK(sig == SIGSEGV || sig == SIGBUS);
    test_trampoline();
}

/* Every table entry must run or raise SIGILL (extension not implemented by
 * this CPU).  Anything else is a bug in a definition. */
static void test_table(void)
{
    char err[160];
    CHECK(ua_insns_verify(err, sizeof err) == 0);
    CHECK(ua_n_insns > 500);
    uintptr_t mid = (uintptr_t)ua_scratch_mid();
    size_t unsupported = 0, blocks = 0;
    for (size_t i = 0; i < ua_n_insns; i++) {
        const ua_insn *in = &ua_insns[i];
        for (int c = -1; c < in->n_chains; c++) {
            uint32_t off = c < 0 ? in->tp_off : in->chains[c].off;
            size_t n = c < 0 ? in->tp_n : in->chains[c].n;
            const ua_init *init = c < 0 ? in->tp_init : in->chains[c].init;
            size_t n_init = c < 0 ? in->tp_n_init : in->chains[c].n_init;
            if (!n)
                continue;
            ua_regs r;
            memset(ua_scratch_base(), 0, UA_SCRATCH_BYTES);
            ua_regs_default(&r);
            ua_init_apply(&r, init, n_init);
            const void *code = ua_jit_loop(ua_code + in->pre_off, in->pre_n, ua_code + off, n, 3,
                                           ua_code + in->post_off, in->post_n, NULL);
            CHECK(code != NULL);
            int sig = ua_run_guarded(code, &r, 5);
            blocks++;
            g_checks++;
            if (sig == SIGILL) {
                unsupported++;
            } else if (sig != 0) {
                g_failures++;
                fprintf(stderr, "%s (%s): signal %d\n", in->name,
                        c < 0 ? "throughput block" : in->chains[c].text, sig);
            }
            /* A pointer-chasing chain must still hold a node address. */
            if (sig == 0 && c >= 0) {
                for (size_t k = 0; k < n_init; k++) {
                    if (init[k].kind != UA_INIT_CYCLE_PTR)
                        continue;
                    uint64_t p = r.out_x[init[k].reg];
                    g_checks++;
                    if (p < mid || p >= mid + 509 * 64 || (p - mid) % 64) {
                        g_failures++;
                        fprintf(stderr, "%s (%s): chase left the cycle (%#llx)\n", in->name,
                                in->chains[c].text, (unsigned long long)p);
                    }
                    break;
                }
            }
        }
    }
    printf("table: %zu entries, %zu code blocks run, %zu refused as illegal instructions\n",
           ua_n_insns, blocks, unsupported);
    /* add x0, x0, x20 chain: the value must really be carried. */
    const ua_insn *add = ua_insn_find("add_x_reg");
    CHECK(add != NULL && add->n_chains == 2);
    if (add && add->n_chains == 2) {
        ua_regs r;
        ua_regs_default(&r);
        ua_init_apply(&r, add->chains[0].init, add->chains[0].n_init);
        const void *code = ua_jit_loop(NULL, 0, ua_code + add->chains[0].off, add->chains[0].n,
                                       4, NULL, 0, NULL);
        CHECK(ua_run_guarded(code, &r, 10) == 0);
        CHECK(r.out_x[0] == r.x[0] + 40 * r.x[20]);
    }
    CHECK(ua_insn_find("no_such_instruction") == NULL);
}

/* The filler blend for the reorder-buffer experiment. */
static void test_mix_plan(void)
{
    unsigned char plan[64];
    const double cap[5] = {100, 200, 0, 50, 50};
    int count[5] = {0, 0, 0, 0, 0};
    CHECK_NEAR(ua_window_mix_plan(cap, plan, 64), 400, 0);
    for (int i = 0; i < 64; i++) {
        CHECK(plan[i] < 5);
        count[plan[i] % 5]++;
    }
    /* Regression: a kind with unknown capacity used to slip into the blend. */
    CHECK(count[2] == 0);
    CHECK(count[0] == 16 && count[1] == 32 && count[3] == 8 && count[4] == 8);
    /* Evenly spread: every window of 8 holds 4 of kind 1. */
    for (int i = 0; i + 8 <= 64; i += 8) {
        int ones = 0;
        for (int j = 0; j < 8; j++)
            ones += plan[i + j] == 1;
        CHECK(ones == 4);
    }
    const double none[5] = {0, 0, 0, 0, 0};
    CHECK_NEAR(ua_window_mix_plan(none, plan, 64), 0, 0);
    CHECK(plan[0] == 0 && plan[1] == 1);
}

/* Result records must not move when the list grows: experiments keep the
 * pointer of an early result while they add later ones (regression: the
 * list used to realloc the records themselves, so a run of, for instance,
 * `structure -e width,branch,tlb` wrote through a freed pointer). */
static void test_exp_list_stable(void)
{
    ua_exp_list l = {NULL, 0, 0};
    ua_exp_result *first = ua_exp_add(&l, "first", "The first result", "cycles", 0);
    ua_exp_result *kept[40];
    for (int i = 0; i < 40; i++) {
        char id[16];
        snprintf(id, sizeof id, "r%d", i);
        kept[i] = ua_exp_add(&l, id, "Another result", "entries", 1);
        ua_exp_point(kept[i], i, 2.0 * i);
    }
    CHECK(l.n == 41);
    CHECK(l.r[0] == first);
    CHECK(strcmp(first->id, "first") == 0);
    CHECK(first->status == UA_EXP_FAILED && isnan(first->value));
    ua_exp_point(first, 1, 1);
    CHECK(first->n == 1);
    for (int i = 0; i < 40; i++) {
        CHECK(l.r[i + 1] == kept[i]);
        CHECK(kept[i]->n == 1 && kept[i]->x[0] == i);
    }
    ua_exp_list_free(&l);
    CHECK(l.r == NULL && l.n == 0 && l.cap == 0);
}

/* Initialisers that would write outside the scratch buffer are ignored. */
static void test_init_bounds(void)
{
    ua_regs r;
    ua_regs_default(&r);
    uint64_t before = r.x[5];
    const ua_init bad[] = {
        {UA_INIT_CYCLE_IDX, 5, 1024, 1, 8},      /* cells would run past the buffer */
        {UA_INIT_CYCLE_PTR, 5, 4096, 0, 0},
        {UA_INIT_MEM, 0, (int32_t)UA_SCRATCH_BYTES, 1, 0},
        {UA_INIT_MEM, 0, -(int32_t)UA_SCRATCH_BYTES, 1, 0},
        {UA_INIT_GPR, 40, 0, 1, 0},               /* no such register */
    };
    ua_init_apply(&r, bad, sizeof bad / sizeof bad[0]);
    CHECK(r.x[5] == before);
    const ua_init good[] = {{UA_INIT_CYCLE_IDX, 5, 0, 8, 8}};
    ua_init_apply(&r, good, 1);
    CHECK(r.x[5] == 0);
}

int main(void)
{
    if (ua_jit_init() != 0) {
        printf("test_jit: SKIP (MAP_JIT refused)\n");
        return 77;
    }
    ua_measure_init();
    test_trampoline();
    test_loop_and_constants();
    test_xorshift_and_branches();
    test_memory_and_chase();
    test_limits_and_faults();
    test_mix_plan();
    test_exp_list_stable();
    test_init_bounds();
    test_table();
    return test_finish("test_jit");
}
