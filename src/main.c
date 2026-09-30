#include "counters.h"
#include "insn.h"
#include "jit.h"
#include "measure.h"
#include "sysinfo.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_insn(const ua_insn_result *r)
{
    const ua_insn *in = r->insn;
    printf("%-22s %-34s", in->name, in->text);
    if (!r->supported) {
        printf(" unsupported (signal %d)\n", r->fault_sig);
        return;
    }
    if (r->have_tp) {
        if (isnan(r->tp_ipc))
            printf(" tp=%s", ua_status_name(r->tp_raw.status));
        else
            printf(" tp=%6.2f/c%s", r->tp_ipc, r->tp_chain_bound ? "*" : " ");
    }
    for (int i = 0; i < r->n_lat; i++) {
        const ua_chain *ch = &in->chains[i];
        if (r->lat[i].raw.status != UA_OK)
            printf("  %s>%s=%s", ch->from, ch->to, ua_status_name(r->lat[i].raw.status));
        else
            printf("  %s>%s=%.2f%s", ch->from, ch->to, r->lat[i].lat,
                   r->lat[i].roundtrip ? "(rt)" : "");
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    ua_sysinfo si;
    ua_sysinfo_get(&si);
    ua_ctr_backend b = ua_counters_init(UA_CTR_NONE);
    printf("%s, macOS %s, counters: %s\n", si.brand, si.os_version, ua_counters_backend_name(b));
    if (ua_jit_init() != 0) {
        fprintf(stderr, "MAP_JIT refused\n");
        return 1;
    }
    ua_measure_init();
    char err[128];
    if (ua_insns_verify(err, sizeof err) != 0) {
        fprintf(stderr, "instruction table: %s\n", err);
        return 1;
    }
    const char *filter = argc > 1 ? argv[1] : "";
    for (int level = 0; level < si.nlevels; level++) {
        ua_helpers h;
        ua_level_request(level);
        ua_helpers_measure(level, &h);
        printf("== level %d (%s): cmp+csinc=%.2f fmov rt=%.2f fcmp+fcsel=%.2f\n", level,
               si.level[level].name, h.cmp_csinc, h.fmov_rt, h.fcmp_fcsel);
        for (size_t i = 0; i < ua_n_insns; i++) {
            ua_insn_result r;
            if (*filter && !strstr(ua_insns[i].name, filter))
                continue;
            ua_insn_run(&ua_insns[i], level, &h, &r);
            print_insn(&r);
        }
    }
    ua_run_totals t = ua_measure_totals();
    printf("runs=%llu clean=%llu migrated=%llu disturbed=%llu\n", (unsigned long long)t.runs,
           (unsigned long long)t.clean, (unsigned long long)t.migrated,
           (unsigned long long)t.disturbed);
    return 0;
}
