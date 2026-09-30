/*
 * enc.h - a deliberately small AArch64 encoder.
 *
 * The instruction tables get their encodings from the system assembler at
 * build time.  This file covers only what the harness has to generate at
 * run time with computed operands: loop scaffolding and the structure-size
 * experiments, where the number and kind of filler instructions varies.
 *
 * Every function here is cross-checked against the system assembler by
 * tests/enc_check (randomised operands, fixed seed).
 */
#ifndef UA_ENC_H
#define UA_ENC_H

#include <stdint.h>

enum {
    A64_EQ = 0, A64_NE = 1, A64_CS = 2, A64_CC = 3, A64_MI = 4, A64_PL = 5,
    A64_VS = 6, A64_VC = 7, A64_HI = 8, A64_LS = 9, A64_GE = 10, A64_LT = 11,
    A64_GT = 12, A64_LE = 13,
};

#define A64_ZR 31u

#define A64_R(x) ((uint32_t)(x) & 31u)

/* ---- no operands ---- */
static inline uint32_t a64_nop(void) { return 0xd503201fu; }
static inline uint32_t a64_ret(void) { return 0xd65f03c0u; }

/* ---- moves and immediates ---- */
static inline uint32_t a64_movz(unsigned rd, unsigned imm16, unsigned hw)
{
    return 0xd2800000u | ((hw & 3u) << 21) | ((imm16 & 0xffffu) << 5) | A64_R(rd);
}
static inline uint32_t a64_movk(unsigned rd, unsigned imm16, unsigned hw)
{
    return 0xf2800000u | ((hw & 3u) << 21) | ((imm16 & 0xffffu) << 5) | A64_R(rd);
}
/* mov xd, xm  (orr xd, xzr, xm) */
static inline uint32_t a64_mov(unsigned rd, unsigned rm)
{
    return 0xaa0003e0u | (A64_R(rm) << 16) | A64_R(rd);
}

/* ---- add/sub ---- */
static inline uint32_t a64_add_imm(unsigned rd, unsigned rn, unsigned imm12)
{
    return 0x91000000u | ((imm12 & 0xfffu) << 10) | (A64_R(rn) << 5) | A64_R(rd);
}
static inline uint32_t a64_sub_imm(unsigned rd, unsigned rn, unsigned imm12)
{
    return 0xd1000000u | ((imm12 & 0xfffu) << 10) | (A64_R(rn) << 5) | A64_R(rd);
}
static inline uint32_t a64_subs_imm(unsigned rd, unsigned rn, unsigned imm12)
{
    return 0xf1000000u | ((imm12 & 0xfffu) << 10) | (A64_R(rn) << 5) | A64_R(rd);
}
static inline uint32_t a64_add(unsigned rd, unsigned rn, unsigned rm)
{
    return 0x8b000000u | (A64_R(rm) << 16) | (A64_R(rn) << 5) | A64_R(rd);
}
static inline uint32_t a64_sub(unsigned rd, unsigned rn, unsigned rm)
{
    return 0xcb000000u | (A64_R(rm) << 16) | (A64_R(rn) << 5) | A64_R(rd);
}
static inline uint32_t a64_adds(unsigned rd, unsigned rn, unsigned rm)
{
    return 0xab000000u | (A64_R(rm) << 16) | (A64_R(rn) << 5) | A64_R(rd);
}
static inline uint32_t a64_subs(unsigned rd, unsigned rn, unsigned rm)
{
    return 0xeb000000u | (A64_R(rm) << 16) | (A64_R(rn) << 5) | A64_R(rd);
}
/* cmp xn, xm  (subs xzr, xn, xm) */
static inline uint32_t a64_cmp(unsigned rn, unsigned rm) { return a64_subs(A64_ZR, rn, rm); }
/* cmp xn, #imm */
static inline uint32_t a64_cmp_imm(unsigned rn, unsigned imm12)
{
    return a64_subs_imm(A64_ZR, rn, imm12);
}

/* ---- logical (shifted register) ---- */
static inline uint32_t a64_and(unsigned rd, unsigned rn, unsigned rm)
{
    return 0x8a000000u | (A64_R(rm) << 16) | (A64_R(rn) << 5) | A64_R(rd);
}
static inline uint32_t a64_ands(unsigned rd, unsigned rn, unsigned rm)
{
    return 0xea000000u | (A64_R(rm) << 16) | (A64_R(rn) << 5) | A64_R(rd);
}
static inline uint32_t a64_orr(unsigned rd, unsigned rn, unsigned rm)
{
    return 0xaa000000u | (A64_R(rm) << 16) | (A64_R(rn) << 5) | A64_R(rd);
}
static inline uint32_t a64_eor(unsigned rd, unsigned rn, unsigned rm)
{
    return 0xca000000u | (A64_R(rm) << 16) | (A64_R(rn) << 5) | A64_R(rd);
}
/* eor xd, xn, xm, lsl #sh */
static inline uint32_t a64_eor_lsl(unsigned rd, unsigned rn, unsigned rm, unsigned sh)
{
    return a64_eor(rd, rn, rm) | ((sh & 63u) << 10);
}
/* eor xd, xn, xm, lsr #sh */
static inline uint32_t a64_eor_lsr(unsigned rd, unsigned rn, unsigned rm, unsigned sh)
{
    return a64_eor(rd, rn, rm) | (1u << 22) | ((sh & 63u) << 10);
}

/* ---- multiply / divide ---- */
static inline uint32_t a64_madd(unsigned rd, unsigned rn, unsigned rm, unsigned ra)
{
    return 0x9b000000u | (A64_R(rm) << 16) | (A64_R(ra) << 10) | (A64_R(rn) << 5) | A64_R(rd);
}
static inline uint32_t a64_mul(unsigned rd, unsigned rn, unsigned rm)
{
    return a64_madd(rd, rn, rm, A64_ZR);
}
static inline uint32_t a64_udiv(unsigned rd, unsigned rn, unsigned rm)
{
    return 0x9ac00800u | (A64_R(rm) << 16) | (A64_R(rn) << 5) | A64_R(rd);
}

/* ---- conditional select ---- */
static inline uint32_t a64_csel(unsigned rd, unsigned rn, unsigned rm, unsigned cond)
{
    return 0x9a800000u | (A64_R(rm) << 16) | ((cond & 15u) << 12) | (A64_R(rn) << 5) | A64_R(rd);
}
static inline uint32_t a64_csinc(unsigned rd, unsigned rn, unsigned rm, unsigned cond)
{
    return a64_csel(rd, rn, rm, cond) | (1u << 10);
}

/* ---- loads and stores, unsigned scaled offset (byte offset given) ---- */
static inline uint32_t a64_ldr(unsigned rt, unsigned rn, unsigned off)
{
    return 0xf9400000u | (((off >> 3) & 0xfffu) << 10) | (A64_R(rn) << 5) | A64_R(rt);
}
static inline uint32_t a64_str(unsigned rt, unsigned rn, unsigned off)
{
    return 0xf9000000u | (((off >> 3) & 0xfffu) << 10) | (A64_R(rn) << 5) | A64_R(rt);
}
static inline uint32_t a64_ldrb(unsigned rt, unsigned rn, unsigned off)
{
    return 0x39400000u | ((off & 0xfffu) << 10) | (A64_R(rn) << 5) | A64_R(rt);
}
/* ldr xt, [xn, xm] */
static inline uint32_t a64_ldr_idx(unsigned rt, unsigned rn, unsigned rm)
{
    return 0xf8606800u | (A64_R(rm) << 16) | (A64_R(rn) << 5) | A64_R(rt);
}
/* ldr xt, [xn, xm, lsl #3] */
static inline uint32_t a64_ldr_idx3(unsigned rt, unsigned rn, unsigned rm)
{
    return 0xf8607800u | (A64_R(rm) << 16) | (A64_R(rn) << 5) | A64_R(rt);
}
/* ldr dt, [xn, #off] / str dt, [xn, #off] */
static inline uint32_t a64_ldr_d(unsigned rt, unsigned rn, unsigned off)
{
    return 0xfd400000u | (((off >> 3) & 0xfffu) << 10) | (A64_R(rn) << 5) | A64_R(rt);
}
static inline uint32_t a64_str_d(unsigned rt, unsigned rn, unsigned off)
{
    return 0xfd000000u | (((off >> 3) & 0xfffu) << 10) | (A64_R(rn) << 5) | A64_R(rt);
}

/* ---- branches; offsets are in instructions relative to the branch ---- */
static inline uint32_t a64_b(int32_t off)
{
    return 0x14000000u | ((uint32_t)off & 0x03ffffffu);
}
static inline uint32_t a64_bl(int32_t off)
{
    return 0x94000000u | ((uint32_t)off & 0x03ffffffu);
}
static inline uint32_t a64_bcond(unsigned cond, int32_t off)
{
    return 0x54000000u | (((uint32_t)off & 0x7ffffu) << 5) | (cond & 15u);
}
static inline uint32_t a64_cbz(unsigned rt, int32_t off)
{
    return 0xb4000000u | (((uint32_t)off & 0x7ffffu) << 5) | A64_R(rt);
}
static inline uint32_t a64_cbnz(unsigned rt, int32_t off)
{
    return 0xb5000000u | (((uint32_t)off & 0x7ffffu) << 5) | A64_R(rt);
}
static inline uint32_t a64_tbz(unsigned rt, unsigned bit, int32_t off)
{
    return 0x36000000u | ((bit & 32u) << 26) | ((bit & 31u) << 19) |
           (((uint32_t)off & 0x3fffu) << 5) | A64_R(rt);
}
static inline uint32_t a64_tbnz(unsigned rt, unsigned bit, int32_t off)
{
    return a64_tbz(rt, bit, off) | (1u << 24);
}
static inline uint32_t a64_br(unsigned rn) { return 0xd61f0000u | (A64_R(rn) << 5); }
static inline uint32_t a64_blr(unsigned rn) { return 0xd63f0000u | (A64_R(rn) << 5); }
/* adr xd, .+off*4 */
static inline uint32_t a64_adr(unsigned rd, int32_t off)
{
    uint32_t bytes = (uint32_t)off * 4u;
    return 0x10000000u | ((bytes & 3u) << 29) | (((bytes >> 2) & 0x7ffffu) << 5) | A64_R(rd);
}

/* ---- scalar floating point (double) ---- */
static inline uint32_t a64_fadd_d(unsigned rd, unsigned rn, unsigned rm)
{
    return 0x1e602800u | (A64_R(rm) << 16) | (A64_R(rn) << 5) | A64_R(rd);
}
static inline uint32_t a64_fmul_d(unsigned rd, unsigned rn, unsigned rm)
{
    return 0x1e600800u | (A64_R(rm) << 16) | (A64_R(rn) << 5) | A64_R(rd);
}
static inline uint32_t a64_fdiv_d(unsigned rd, unsigned rn, unsigned rm)
{
    return 0x1e601800u | (A64_R(rm) << 16) | (A64_R(rn) << 5) | A64_R(rd);
}
static inline uint32_t a64_fsqrt_d(unsigned rd, unsigned rn)
{
    return 0x1e61c000u | (A64_R(rn) << 5) | A64_R(rd);
}
static inline uint32_t a64_fmov_dd(unsigned rd, unsigned rn)
{
    return 0x1e604000u | (A64_R(rn) << 5) | A64_R(rd);
}
/* fmov dd, xn */
static inline uint32_t a64_fmov_dx(unsigned rd, unsigned rn)
{
    return 0x9e670000u | (A64_R(rn) << 5) | A64_R(rd);
}
/* fmov xd, dn */
static inline uint32_t a64_fmov_xd(unsigned rd, unsigned rn)
{
    return 0x9e660000u | (A64_R(rn) << 5) | A64_R(rd);
}
/* fcmp dn, dm */
static inline uint32_t a64_fcmp_d(unsigned rn, unsigned rm)
{
    return 0x1e602000u | (A64_R(rm) << 16) | (A64_R(rn) << 5);
}

/* ---- Advanced SIMD ---- */
/* add vd.2d, vn.2d, vm.2d */
static inline uint32_t a64_vadd_2d(unsigned rd, unsigned rn, unsigned rm)
{
    return 0x4ee08400u | (A64_R(rm) << 16) | (A64_R(rn) << 5) | A64_R(rd);
}
/* eor vd.16b, vn.16b, vm.16b */
static inline uint32_t a64_veor(unsigned rd, unsigned rn, unsigned rm)
{
    return 0x6e201c00u | (A64_R(rm) << 16) | (A64_R(rn) << 5) | A64_R(rd);
}
/* mov vd.16b, vn.16b  (orr vd.16b, vn.16b, vn.16b) */
static inline uint32_t a64_vmov(unsigned rd, unsigned rn)
{
    return 0x4ea01c00u | (A64_R(rn) << 16) | (A64_R(rn) << 5) | A64_R(rd);
}
/* movi vd.2d, #0 */
static inline uint32_t a64_movi_zero(unsigned rd) { return 0x6f00e400u | A64_R(rd); }

/* Emit `mov xd, #imm64` as movz + up to three movk.  Returns the number of
 * words written (1..4). */
static inline int a64_mov_imm64(uint32_t *out, unsigned rd, uint64_t imm)
{
    int n = 0;
    out[n++] = a64_movz(rd, (unsigned)(imm & 0xffff), 0);
    for (unsigned hw = 1; hw < 4; hw++) {
        unsigned part = (unsigned)((imm >> (16 * hw)) & 0xffff);
        if (part)
            out[n++] = a64_movk(rd, part, hw);
    }
    return n;
}

#endif
