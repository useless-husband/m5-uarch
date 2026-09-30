/*
 * report.h - collect results and write them as JSON (schema "m5-uarch/1").
 */
#ifndef UA_REPORT_H
#define UA_REPORT_H

#include "exp.h"
#include "insn.h"
#include "sysinfo.h"

#include <stdio.h>

#define UA_TOOL_VERSION "0.1.0"
#define UA_SCHEMA "m5-uarch/1"

typedef struct {
    ua_sysinfo sys;
    const char *counters;          /* backend name */
    int n_levels;
    int level_measured[UA_SYS_MAX_LEVELS];
    ua_helpers helpers[UA_SYS_MAX_LEVELS];
    double ghz[UA_SYS_MAX_LEVELS]; /* frequency seen while measuring */
    /* insn[level][i] is valid if insn_done[level][i] */
    ua_insn_result *insn[UA_SYS_MAX_LEVELS];
    unsigned char *insn_done[UA_SYS_MAX_LEVELS];
    ua_exp_list exps[UA_SYS_MAX_LEVELS];
    double load_avg[3];
    char started[32];              /* ISO 8601, UTC */
    double seconds;
} ua_report;

int ua_report_init(ua_report *r);
void ua_report_free(ua_report *r);
/* Returns 0 on success. */
int ua_report_write_json(const ua_report *r, FILE *f);

/* One-line text renderings for the terminal. */
void ua_report_print_insn(const ua_insn_result *res, FILE *f);
void ua_report_print_exp(const ua_exp_result *res, FILE *f);

#endif
