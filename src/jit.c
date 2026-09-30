#include "jit.h"

#include "enc.h"

#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define ARENA_BYTES (8u * 1024u * 1024u)

static uint32_t *g_arena;
static size_t g_pos;
static int g_overflow;
static size_t g_loop_top;
static uint8_t *g_scratch;

int ua_jit_init(void)
{
    if (g_arena)
        return 0;
    void *p = mmap(NULL, ARENA_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    g_arena = p;
    /* Leave the arena executable; ua_jit_begin() flips this thread to write. */
    pthread_jit_write_protect_np(1);

    void *s = mmap(NULL, UA_SCRATCH_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (s == MAP_FAILED) {
        munmap(p, ARENA_BYTES);
        g_arena = NULL;
        return -1;
    }
    g_scratch = s;
    memset(g_scratch, 0, UA_SCRATCH_BYTES);
    return 0;
}

size_t ua_jit_capacity(void) { return ARENA_BYTES / 4; }

void ua_jit_begin(void)
{
    g_pos = 0;
    g_overflow = 0;
    pthread_jit_write_protect_np(0);
}

void ua_jit_put(uint32_t w)
{
    if (g_pos >= ARENA_BYTES / 4) {
        g_overflow = 1;
        return;
    }
    g_arena[g_pos++] = w;
}

void ua_jit_put_n(const uint32_t *w, size_t n)
{
    for (size_t i = 0; i < n; i++)
        ua_jit_put(w[i]);
}

size_t ua_jit_pos(void) { return g_pos; }

void ua_jit_patch(size_t pos, uint32_t w)
{
    if (pos < g_pos)
        g_arena[pos] = w;
}

uint32_t ua_jit_peek(size_t pos) { return pos < g_pos ? g_arena[pos] : 0; }

void ua_jit_align(size_t words)
{
    while (words && g_pos % words && !g_overflow)
        ua_jit_put(a64_nop());
}

const void *ua_jit_end(void)
{
    pthread_jit_write_protect_np(1);
    if (g_overflow)
        return NULL;
    sys_icache_invalidate(g_arena, g_pos * 4);
    return g_arena;
}

void ua_jit_loop_open(const uint32_t *init, size_t n_init)
{
    ua_jit_begin();
    ua_jit_put_n(init, n_init);
    ua_jit_align(16);
    g_loop_top = ua_jit_pos();
}

const void *ua_jit_loop_close(const uint32_t *fini, size_t n_fini)
{
    /* sub + cbnz rather than subs + b.ne: the loop counter must not touch
     * NZCV, or it would cut every latency chain that runs through the flags
     * (and hand the core a fresh, branch-correlated flag value each time). */
    ua_jit_put(a64_sub_imm(UA_REG_CNT, UA_REG_CNT, 1));
    int64_t back = (int64_t)g_loop_top - (int64_t)ua_jit_pos();
    /* cbnz reaches +-1 MiB: a signed 19-bit word offset. */
    int too_far = back < -(1 << 18);
    ua_jit_put(a64_cbnz(UA_REG_CNT, (int32_t)back));
    ua_jit_put_n(fini, n_fini);
    ua_jit_put(a64_ret());
    const void *entry = ua_jit_end();
    return too_far ? NULL : entry;
}

const void *ua_jit_loop(const uint32_t *init, size_t n_init,
                        const uint32_t *body, size_t n_body, unsigned reps,
                        const uint32_t *fini, size_t n_fini,
                        uint64_t *insns_per_iter)
{
    ua_jit_loop_open(init, n_init);
    for (unsigned r = 0; r < reps; r++)
        ua_jit_put_n(body, n_body);
    if (insns_per_iter)
        *insns_per_iter = (uint64_t)n_body * reps + 2;
    return ua_jit_loop_close(fini, n_fini);
}

uint8_t *ua_scratch_base(void) { return g_scratch; }
uint8_t *ua_scratch_mid(void) { return g_scratch + UA_SCRATCH_BYTES / 2; }

void ua_regs_default(ua_regs *r)
{
    static const double one = 1.0;
    memset(r, 0, sizeof *r);
    for (int i = 0; i < 31; i++)
        r->x[i] = 0x100u + 2u * (unsigned)i + 1u;
    r->x[UA_REG_MEM] = (uint64_t)(uintptr_t)ua_scratch_mid();
    for (int i = 0; i < 32; i++) {
        memcpy(&r->v[i][0], &one, 8);
        memcpy(&r->v[i][8], &one, 8);
    }
}
