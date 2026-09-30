/*
 * tramp.S - the only hand-written machine code that surrounds a measured loop.
 *
 *   uint64_t ua_tramp(const void *code, ua_regs *regs, uint64_t iters);
 *
 * Loads every general-purpose and vector register and NZCV from `regs`,
 * calls `code`, then stores the register state back into `regs->out_*` so
 * that functional tests can check what the generated code computed.
 *
 * Register contract for generated code:
 *   x0-x17, x19-x26   free, initialised from regs->x[]
 *   x27               scratch-memory base (regs->x[27])
 *   x28               loop counter = iters
 *   x18               platform register, never touched
 *   x29               points at `regs`, must be preserved
 *   x30               return address, must be preserved
 *   v0-v31, NZCV      free, initialised from regs
 *
 * Offsets must match struct ua_regs in jit.h (checked by static asserts).
 */
#define REGS_X      0
#define REGS_NZCV   248
#define REGS_V      256
#define REGS_OUT_X  768
#define REGS_OUT_V  1024

    .text
    .globl _ua_tramp
    .p2align 4
_ua_tramp:
    stp     x29, x30, [sp, #-160]!
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]
    stp     d8,  d9,  [sp, #96]
    stp     d10, d11, [sp, #112]
    stp     d12, d13, [sp, #128]
    stp     d14, d15, [sp, #144]

    mov     x29, x1
    mov     x30, x0
    mov     x28, x2

    ldp     q0,  q1,  [x29, #REGS_V + 0]
    ldp     q2,  q3,  [x29, #REGS_V + 32]
    ldp     q4,  q5,  [x29, #REGS_V + 64]
    ldp     q6,  q7,  [x29, #REGS_V + 96]
    ldp     q8,  q9,  [x29, #REGS_V + 128]
    ldp     q10, q11, [x29, #REGS_V + 160]
    ldp     q12, q13, [x29, #REGS_V + 192]
    ldp     q14, q15, [x29, #REGS_V + 224]
    ldp     q16, q17, [x29, #REGS_V + 256]
    ldp     q18, q19, [x29, #REGS_V + 288]
    ldp     q20, q21, [x29, #REGS_V + 320]
    ldp     q22, q23, [x29, #REGS_V + 352]
    ldp     q24, q25, [x29, #REGS_V + 384]
    ldp     q26, q27, [x29, #REGS_V + 416]
    ldp     q28, q29, [x29, #REGS_V + 448]
    ldp     q30, q31, [x29, #REGS_V + 480]

    ldr     x9, [x29, #REGS_NZCV]
    msr     nzcv, x9

    ldp     x0,  x1,  [x29, #REGS_X + 0]
    ldp     x2,  x3,  [x29, #REGS_X + 16]
    ldp     x4,  x5,  [x29, #REGS_X + 32]
    ldp     x6,  x7,  [x29, #REGS_X + 48]
    ldp     x8,  x9,  [x29, #REGS_X + 64]
    ldp     x10, x11, [x29, #REGS_X + 80]
    ldp     x12, x13, [x29, #REGS_X + 96]
    ldp     x14, x15, [x29, #REGS_X + 112]
    ldp     x16, x17, [x29, #REGS_X + 128]
    ldp     x19, x20, [x29, #REGS_X + 152]
    ldp     x21, x22, [x29, #REGS_X + 168]
    ldp     x23, x24, [x29, #REGS_X + 184]
    ldp     x25, x26, [x29, #REGS_X + 200]
    ldr     x27,      [x29, #REGS_X + 216]

    blr     x30

    /* x28 (the loop counter) is dead now; use it as the output base. */
    add     x28, x29, #REGS_OUT_X
    stp     x0,  x1,  [x28, #0]
    stp     x2,  x3,  [x28, #16]
    stp     x4,  x5,  [x28, #32]
    stp     x6,  x7,  [x28, #48]
    stp     x8,  x9,  [x28, #64]
    stp     x10, x11, [x28, #80]
    stp     x12, x13, [x28, #96]
    stp     x14, x15, [x28, #112]
    stp     x16, x17, [x28, #128]
    stp     x19, x20, [x28, #152]
    stp     x21, x22, [x28, #168]
    stp     x23, x24, [x28, #184]
    stp     x25, x26, [x28, #200]
    str     x27,      [x28, #216]
    mrs     x9, nzcv
    str     x9, [x28, #248]

    add     x28, x29, #REGS_OUT_V
    stp     q0,  q1,  [x28, #0]
    stp     q2,  q3,  [x28, #32]
    stp     q4,  q5,  [x28, #64]
    stp     q6,  q7,  [x28, #96]
    stp     q8,  q9,  [x28, #128]
    stp     q10, q11, [x28, #160]
    stp     q12, q13, [x28, #192]
    stp     q14, q15, [x28, #224]
    stp     q16, q17, [x28, #256]
    stp     q18, q19, [x28, #288]
    stp     q20, q21, [x28, #320]
    stp     q22, q23, [x28, #352]
    stp     q24, q25, [x28, #384]
    stp     q26, q27, [x28, #416]
    stp     q28, q29, [x28, #448]
    stp     q30, q31, [x28, #480]

    ldp     d14, d15, [sp, #144]
    ldp     d12, d13, [sp, #128]
    ldp     d10, d11, [sp, #112]
    ldp     d8,  d9,  [sp, #96]
    ldp     x27, x28, [sp, #80]
    ldp     x25, x26, [sp, #64]
    ldp     x23, x24, [sp, #48]
    ldp     x21, x22, [sp, #32]
    ldp     x19, x20, [sp, #16]
    ldp     x29, x30, [sp], #160
    ret
