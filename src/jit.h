/*
 * jit.h - MAP_JIT code arena, register-state block and loop builder.
 */
#ifndef UA_JIT_H
#define UA_JIT_H

#include <stddef.h>
#include <stdint.h>

/* Register state handed to the trampoline.  Layout is mirrored in tramp.S. */
typedef struct {
    uint64_t x[31];        /* x0..x30 (x18, x28..x30 are ignored on entry)   */
    uint64_t nzcv;         /* loaded into NZCV                                */
    uint8_t v[32][16];     /* v0..v31                                         */
    uint64_t out_x[31];    /* register values after the call                  */
    uint64_t out_nzcv;
    uint8_t out_v[32][16];
} ua_regs;

_Static_assert(offsetof(ua_regs, nzcv) == 248, "tramp.S REGS_NZCV");
_Static_assert(offsetof(ua_regs, v) == 256, "tramp.S REGS_V");
_Static_assert(offsetof(ua_regs, out_x) == 768, "tramp.S REGS_OUT_X");
_Static_assert(offsetof(ua_regs, out_v) == 1024, "tramp.S REGS_OUT_V");

/* Registers with a fixed meaning inside generated code. */
#define UA_REG_MEM 27 /* scratch memory base */
#define UA_REG_CNT 28 /* loop counter        */

uint64_t ua_tramp(const void *code, ua_regs *regs, uint64_t iters);

/* Arena ------------------------------------------------------------------ */

/* Map the arena (once).  Returns 0 on success, -1 if MAP_JIT is refused. */
int ua_jit_init(void);
/* Capacity in instruction words. */
size_t ua_jit_capacity(void);

/* Start writing a new function at the beginning of the arena, or at a
 * chosen word offset.  A different offset gives the code addresses that no
 * earlier test has used, which matters when a predictor's history is part
 * of what is being measured. */
void ua_jit_begin(void);
void ua_jit_begin_at(size_t word_offset);
/* Append words.  Writes past capacity set an overflow flag instead. */
void ua_jit_put(uint32_t w);
void ua_jit_put_n(const uint32_t *w, size_t n);
/* Current position, in words from the start of the function. */
size_t ua_jit_pos(void);
void ua_jit_patch(size_t pos, uint32_t w);
uint32_t ua_jit_peek(size_t pos);
/* Pad with NOPs until the position is a multiple of `words`. */
void ua_jit_align(size_t words);
/* Finish: flush the instruction cache and make the code executable.
 * Returns the entry point, or NULL if the arena overflowed. */
const void *ua_jit_end(void);

/* Loop builder ------------------------------------------------------------
 *
 *     cbz  x28, done          (zero iterations: do nothing)
 *     [init]
 *     .p2align 6
 *  top:
 *     body   x reps
 *     sub  x28, x28, #1
 *     cbnz x28, top
 *     [fini]
 *  done:
 *     ret
 *
 * Returns the entry point or NULL (arena overflow / loop too long for the
 * cbnz branch).  `*insns_per_iter`, if not NULL, receives the number
 * of instruction words executed per iteration assuming straight-line body
 * code (n_body * reps + 2).
 */
const void *ua_jit_loop(const uint32_t *init, size_t n_init,
                        const uint32_t *body, size_t n_body, unsigned reps,
                        const uint32_t *fini, size_t n_fini,
                        uint64_t *insns_per_iter);

/* Same, for a body already written in the arena between ua_jit_loop_open()
 * and ua_jit_loop_close(). */
void ua_jit_loop_open(const uint32_t *init, size_t n_init);
void ua_jit_loop_open_at(size_t word_offset, const uint32_t *init, size_t n_init);
const void *ua_jit_loop_close(const uint32_t *fini, size_t n_fini);

/* Scratch memory ------------------------------------------------------------
 * A small page-aligned buffer that stays in the L1 cache.  x27 points to its
 * middle so that negative offsets are valid too. */
#define UA_SCRATCH_BYTES (64u * 1024u)
uint8_t *ua_scratch_base(void);   /* lowest address  */
uint8_t *ua_scratch_mid(void);    /* value for x27   */

/* Fill `r` with the default register state: small distinct integers in the
 * GPRs, 1.0 (double) in both lanes of every vector register, x27 pointing
 * at the scratch buffer. */
void ua_regs_default(ua_regs *r);

#endif
