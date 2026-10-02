/*
 * insn.h - the instruction table and its runner.
 *
 * The table itself (ua_insns, ua_code) is generated at build time by
 * tools/gen_insns.py from the insns/ definitions.  The encodings in ua_code come
 * straight from the system assembler; C only ever copies them.
 */
#ifndef UA_INSN_H
#define UA_INSN_H

#include "measure.h"

#include <stddef.h>
#include <stdint.h>

/* How one register or memory cell is initialised before a run. */
enum {
    UA_INIT_GPR = 0,   /* x[reg] = val                                        */
    UA_INIT_GPR_PTR,   /* x[reg] = scratch_mid + val                          */
    UA_INIT_VEC,       /* v[reg] = {val, val2}                                */
    UA_INIT_VEC_ALL,   /* every vector register = {val, val2}                 */
    UA_INIT_MEM,       /* 64-bit cell at scratch_mid + off = val              */
    UA_INIT_MEM_PTR,   /* 64-bit cell at scratch_mid + off = scratch_mid + val */
    UA_INIT_NZCV,      /* flags = val                                         */
    UA_INIT_CYCLE_PTR, /* random cycle of nodes; the cell at node + off points
                          to the next node; x[reg] = first node               */
    UA_INIT_CYCLE_IDX, /* random cycle of indices; the cell (val2 bytes wide)
                          at scratch_mid + off + index * val holds the next
                          index; x[reg] = first index                         */
};

typedef struct {
    uint8_t kind;
    uint8_t reg;
    int32_t off;
    uint64_t val;
    uint64_t val2;
} ua_init;

/* Helper instruction appended to a latency chain when the output and the
 * input live in different register files (see docs/DESIGN.md). */
enum {
    UA_HELP_NONE = 0,
    UA_HELP_CMP,      /* cmp x, x        integer -> flags                      */
    UA_HELP_CSINC,    /* csinc x, x, x,c flags   -> integer                    */
    UA_HELP_FMOV_XD,  /* fmov x, d       FP      -> integer                    */
    UA_HELP_FMOV_DX,  /* fmov d, x       integer -> FP                         */
    UA_HELP_FCMP,     /* fcmp d, d       FP      -> flags                      */
    UA_HELP_FCSEL,    /* fcsel d, d, d,c flags   -> FP                         */
    UA_HELP__COUNT,
};

typedef struct {
    const char *from;   /* operand the chain enters through, e.g. "R0:x0"     */
    const char *to;     /* operand it leaves through, e.g. "W0:x0", "nzcv"    */
    const char *text;   /* one step of the chain, as assembled                */
    uint8_t helper;     /* UA_HELP_*                                          */
    uint8_t tied;       /* 1 if a read-write operand was tied to `from`       */
    uint8_t steps;      /* chain traversals per block (2 for "||" entries)    */
    uint8_t extra;      /* one-cycle instructions added by the template       */
    uint32_t off;       /* word offset of the step in ua_code                 */
    uint16_t n;         /* words per step                                     */
    const ua_init *init;
    uint16_t n_init;
} ua_chain;

enum {
    UA_INSN_TP_SERIAL = 1u << 0, /* throughput block is a dependency chain
                                    (reads and writes NZCV or fixed registers) */
    UA_INSN_FLIP = 1u << 1,      /* run with inverted NZCV between measurements */
};

typedef struct {
    const char *name;   /* unique key, e.g. "add_x_reg"                       */
    const char *group;  /* e.g. "Integer / arithmetic"                        */
    const char *ext;    /* architecture extension, "" for the base ISA        */
    const char *text;   /* representative assembly, e.g. "add x0, x19, x20"   */
    const char *note;   /* free-form remark or ""                             */
    uint32_t flags;
    /* throughput block: tp_inst independent instances, tp_n words */
    uint32_t tp_off;
    uint16_t tp_n;
    uint16_t tp_inst;
    uint16_t tp_chains; /* parallel dependency chains in the block, 0 = none  */
    const ua_init *tp_init;
    uint16_t tp_n_init;
    /* optional code run once before / after the loop (e.g. smstart/smstop) */
    uint32_t pre_off, post_off;
    uint16_t pre_n, post_n;
    const ua_chain *chains;
    uint16_t n_chains;
} ua_insn;

extern const ua_insn ua_insns[];
extern const size_t ua_n_insns;
extern const uint32_t ua_code[];      /* assembled by the system assembler */
extern const uint32_t ua_code_end[];
extern const uint32_t ua_code_words;  /* length the generator expects      */
extern const uint32_t ua_code_marks[]; /* word offsets of the blob markers  */
extern const size_t ua_n_code_marks;

#define UA_CODE_MARK 0x0bad0000u

/* Check that the assembled blob has the layout the generator assumed.
 * Returns 0, or -1 with a message in `err`. */
int ua_insns_verify(char *err, size_t errsz);

/* Results --------------------------------------------------------------- */

typedef struct {
    ua_meas raw;      /* cycles are per chain step (instruction + helper)   */
    double lat;       /* raw minus the helper's share, or NaN if unknown    */
    int roundtrip;    /* 1 if lat still includes a helper of unknown share  */
    double per_iter_extra; /* cycles per loop iteration not explained by the
                              chain itself (loop-edge effects)               */
} ua_lat_result;

#define UA_MAX_CHAINS 8

typedef struct {
    const ua_insn *insn;
    int level;
    int supported;       /* 0: the instruction faulted on this CPU          */
    int fault_sig;
    int have_tp;
    ua_meas tp_raw;      /* cycles per iteration of the throughput loop      */
    double tp_cpi;       /* cycles per instruction at steady state           */
    double tp_ipc;       /* instructions per cycle                           */
    int tp_chain_bound;  /* throughput limited by the block's own chains     */
    int tp_whole_loop;   /* the two loop lengths kept disagreeing: rate of
                            the longer loop alone, loop branch included    */
    int n_lat;
    ua_lat_result lat[UA_MAX_CHAINS];
} ua_insn_result;

/* Latencies of the chain helpers on `level`, measured once per level. */
typedef struct {
    int valid;
    double cmp_csinc;   /* integer -> flags -> integer round trip           */
    double fmov_rt;     /* integer -> FP -> integer round trip              */
    double fcmp_fcsel;  /* FP -> flags -> FP round trip                     */
    double share[UA_HELP__COUNT]; /* cycles attributed to each helper, NaN if
                                     the split is not determined             */
} ua_helpers;

void ua_helpers_measure(int level, ua_helpers *h);

/* Measure one table entry on one performance level. */
void ua_insn_run(const ua_insn *in, int level, const ua_helpers *h, ua_insn_result *out);

/* Apply a list of initialisers to `regs` and the scratch buffer. */
void ua_init_apply(ua_regs *regs, const ua_init *init, size_t n);

const ua_insn *ua_insn_find(const char *name);

/* Does pacia change a pointer in this process?  1: yes (keys active),
 * 0: no (the instruction passes its operand through, as it does in a plain
 * arm64 process on macOS), -1: could not tell. */
int ua_probe_pauth(void);

#endif
