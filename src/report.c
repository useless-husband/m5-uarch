#include "report.h"

#include "counters.h"
#include "json.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int ua_report_init(ua_report *r)
{
    memset(r, 0, sizeof *r);
    ua_sysinfo_get(&r->sys);
    r->n_levels = r->sys.nlevels;
    r->counters = ua_counters_backend_name(ua_counters_backend());
    for (int l = 0; l < r->n_levels; l++) {
        r->insn[l] = calloc(ua_n_insns ? ua_n_insns : 1, sizeof *r->insn[l]);
        r->insn_done[l] = calloc(ua_n_insns ? ua_n_insns : 1, 1);
        if (!r->insn[l] || !r->insn_done[l])
            return -1;
    }
    r->pauth_active = ua_probe_pauth();
    getloadavg(r->load_avg, 3);
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(r->started, sizeof r->started, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return 0;
}

void ua_report_free(ua_report *r)
{
    for (int l = 0; l < UA_SYS_MAX_LEVELS; l++) {
        free(r->insn[l]);
        free(r->insn_done[l]);
        ua_exp_list_free(&r->exps[l]);
    }
}

static const char *helper_name(int h)
{
    switch (h) {
    case UA_HELP_CMP:     return "cmp";
    case UA_HELP_CSINC:   return "csinc";
    case UA_HELP_FMOV_XD: return "fmov x,d";
    case UA_HELP_FMOV_DX: return "fmov d,x";
    case UA_HELP_FCMP:    return "fcmp";
    case UA_HELP_FCSEL:   return "fcsel";
    default:              return "";
    }
}

static void write_insn_level(ua_json *j, const ua_insn_result *res)
{
    const ua_insn *in = res->insn;
    ua_json_begin_object(j);
    if (!res->supported) {
        ua_json_kbool(j, "supported", 0);
        ua_json_kint(j, "signal", res->fault_sig);
        ua_json_end_object(j);
        return;
    }
    if (res->have_tp) {
        ua_json_key(j, "tp");
        ua_json_begin_object(j);
        ua_json_kstring(j, "status", ua_status_name(res->tp_raw.status));
        ua_json_knumber(j, "per_cycle", res->tp_ipc, 3);
        ua_json_knumber(j, "cycles", res->tp_cpi, 4);
        ua_json_knumber(j, "spread", res->tp_raw.spread, 4);
        if (res->tp_chain_bound)
            ua_json_kbool(j, "chain_bound", 1);
        ua_json_end_object(j);
    }
    ua_json_key(j, "lat");
    ua_json_begin_array(j);
    for (int i = 0; i < res->n_lat; i++) {
        const ua_chain *ch = &in->chains[i];
        const ua_lat_result *lr = &res->lat[i];
        ua_json_begin_object(j);
        ua_json_kstring(j, "from", ch->from);
        ua_json_kstring(j, "to", ch->to);
        ua_json_kstring(j, "status", ua_status_name(lr->raw.status));
        ua_json_knumber(j, "cycles", lr->lat, 3);
        ua_json_knumber(j, "spread", lr->raw.spread, 4);
        if (ch->helper)
            ua_json_kstring(j, "via", helper_name(ch->helper));
        if (lr->roundtrip)
            ua_json_kbool(j, "roundtrip", 1);
        if (ch->tied)
            ua_json_kbool(j, "tied", 1);
        ua_json_kstring(j, "chain", ch->text);
        ua_json_end_object(j);
    }
    ua_json_end_array(j);
    ua_json_end_object(j);
}

static void write_exp(ua_json *j, const ua_exp_result *e)
{
    static const char *const st[] = {"ok", "inconclusive", "failed"};
    ua_json_begin_object(j);
    ua_json_kstring(j, "id", e->id);
    ua_json_kstring(j, "title", e->title);
    ua_json_kstring(j, "unit", e->unit);
    ua_json_kstring(j, "status", st[e->status]);
    ua_json_knumber(j, "value", e->value, 3);
    ua_json_knumber(j, "lo", e->lo, 3);
    ua_json_knumber(j, "hi", e->hi, 3);
    ua_json_kstring(j, "confidence", e->confidence);
    ua_json_kstring(j, "note", e->note);
    if (e->n) {
        ua_json_kstring(j, "xlabel", e->xlabel);
        ua_json_kstring(j, "ylabel", e->ylabel);
        ua_json_key(j, "curve");
        ua_json_begin_array(j);
        for (int i = 0; i < e->n; i++) {
            ua_json_begin_array(j);
            ua_json_number(j, e->x[i], 3);
            ua_json_number(j, e->y[i], 4);
            ua_json_end_array(j);
        }
        ua_json_end_array(j);
    }
    ua_json_end_object(j);
}

int ua_report_write_json(const ua_report *r, FILE *f)
{
    ua_json js, *j = &js;
    ua_run_totals t = ua_measure_totals();
    ua_json_init(j, f);
    ua_json_begin_object(j);
    ua_json_kstring(j, "schema", UA_SCHEMA);
    ua_json_kstring(j, "tool_version", UA_TOOL_VERSION);
    ua_json_kstring(j, "started", r->started);
    ua_json_knumber(j, "seconds", r->seconds, 1);

    ua_json_key(j, "machine");
    ua_json_begin_object(j);
    ua_json_kstring(j, "brand", r->sys.brand);
    ua_json_kstring(j, "model", r->sys.model);
    ua_json_kstring(j, "os_version", r->sys.os_version);
    ua_json_kstring(j, "os_build", r->sys.os_build);
    ua_json_kuint(j, "page_size", r->sys.page_size);
    ua_json_kuint(j, "memory_bytes", r->sys.memsize);
    ua_json_kbool(j, "virtual_machine", r->sys.is_vm);
    if (r->pauth_active >= 0)
        ua_json_kbool(j, "pauth_keys_active", r->pauth_active);
    ua_json_key(j, "levels");
    ua_json_begin_array(j);
    for (int l = 0; l < r->n_levels; l++) {
        const ua_syslevel *sl = &r->sys.level[l];
        ua_json_begin_object(j);
        ua_json_kstring(j, "label", ua_level_label(l, r->n_levels));
        ua_json_kstring(j, "name", sl->name);
        ua_json_kint(j, "cores", sl->cores);
        ua_json_kuint(j, "l1i_bytes", sl->l1i);
        ua_json_kuint(j, "l1d_bytes", sl->l1d);
        ua_json_kuint(j, "l2_bytes", sl->l2);
        ua_json_kint(j, "cores_per_l2", sl->cpus_per_l2);
        ua_json_kbool(j, "measured", r->level_measured[l]);
        if (r->level_measured[l])
            ua_json_knumber(j, "ghz_observed", r->ghz[l], 2);
        ua_json_end_object(j);
    }
    ua_json_end_array(j);
    ua_json_end_object(j);

    ua_json_key(j, "run");
    ua_json_begin_object(j);
    ua_json_kstring(j, "counters", r->counters);
    ua_json_kuint(j, "runs", t.runs);
    ua_json_kuint(j, "clean", t.clean);
    ua_json_kuint(j, "discarded_migrated", t.migrated);
    ua_json_kuint(j, "discarded_disturbed", t.disturbed);
    ua_json_key(j, "load_average");
    ua_json_begin_array(j);
    for (int i = 0; i < 3; i++)
        ua_json_number(j, r->load_avg[i], 2);
    ua_json_end_array(j);
    ua_json_end_object(j);

    ua_json_key(j, "helpers");
    ua_json_begin_object(j);
    for (int l = 0; l < r->n_levels; l++) {
        if (!r->level_measured[l])
            continue;
        ua_json_key(j, ua_level_label(l, r->n_levels));
        ua_json_begin_object(j);
        ua_json_knumber(j, "cmp_csinc_roundtrip", r->helpers[l].cmp_csinc, 3);
        ua_json_knumber(j, "fmov_roundtrip", r->helpers[l].fmov_rt, 3);
        ua_json_knumber(j, "fcmp_fcsel_roundtrip", r->helpers[l].fcmp_fcsel, 3);
        ua_json_end_object(j);
    }
    ua_json_end_object(j);

    ua_json_key(j, "instructions");
    ua_json_begin_array(j);
    for (size_t i = 0; i < ua_n_insns; i++) {
        int any = 0;
        for (int l = 0; l < r->n_levels; l++)
            any |= r->insn_done[l][i];
        if (!any)
            continue;
        const ua_insn *in = &ua_insns[i];
        ua_json_begin_object(j);
        ua_json_kstring(j, "name", in->name);
        ua_json_kstring(j, "group", in->group);
        ua_json_kstring(j, "ext", in->ext);
        ua_json_kstring(j, "asm", in->text);
        if (in->note[0])
            ua_json_kstring(j, "note", in->note);
        for (int l = 0; l < r->n_levels; l++) {
            if (!r->insn_done[l][i])
                continue;
            ua_json_key(j, ua_level_label(l, r->n_levels));
            write_insn_level(j, &r->insn[l][i]);
        }
        ua_json_end_object(j);
    }
    ua_json_end_array(j);

    ua_json_key(j, "structure");
    ua_json_begin_object(j);
    for (int l = 0; l < r->n_levels; l++) {
        if (!r->exps[l].n)
            continue;
        ua_json_key(j, ua_level_label(l, r->n_levels));
        ua_json_begin_array(j);
        for (int i = 0; i < r->exps[l].n; i++)
            write_exp(j, &r->exps[l].r[i]);
        ua_json_end_array(j);
    }
    ua_json_end_object(j);

    ua_json_end_object(j);
    return ua_json_finish(j);
}

void ua_report_print_insn(const ua_insn_result *r, FILE *f)
{
    const ua_insn *in = r->insn;
    fprintf(f, "%-24s %-40.40s", in->name, in->text);
    if (!r->supported) {
        fprintf(f, " not executable here (signal %d)\n", r->fault_sig);
        return;
    }
    if (r->have_tp) {
        if (isnan(r->tp_ipc))
            fprintf(f, " tp %-9s", ua_status_name(r->tp_raw.status));
        else
            fprintf(f, " tp %5.2f/c%s", r->tp_ipc, r->tp_chain_bound ? "*" : " ");
    } else {
        fprintf(f, "             ");
    }
    for (int i = 0; i < r->n_lat; i++) {
        const ua_chain *ch = &in->chains[i];
        char from[8], to[8];
        snprintf(from, sizeof from, "%.*s", (int)strcspn(ch->from, ":"), ch->from);
        snprintf(to, sizeof to, "%.*s", (int)strcspn(ch->to, ":"), ch->to);
        if (r->lat[i].raw.status != UA_OK)
            fprintf(f, "  %s>%s %s", from, to, ua_status_name(r->lat[i].raw.status));
        else
            fprintf(f, "  %s>%s %.2f%s", from, to, r->lat[i].lat,
                    r->lat[i].roundtrip ? "rt" : "");
    }
    fputc('\n', f);
}

void ua_report_print_exp(const ua_exp_result *e, FILE *f)
{
    if (e->status == UA_EXP_FAILED) {
        fprintf(f, "  %-20s failed: %s\n", e->id, e->note);
        return;
    }
    if (e->status == UA_EXP_INCONCLUSIVE) {
        fprintf(f, "  %-20s inconclusive: %s\n", e->id, e->note);
        return;
    }
    fprintf(f, "  %-20s %9.2f %-10s", e->id, e->value, e->unit);
    if (!isnan(e->lo) && !isnan(e->hi) && e->lo != e->hi)
        fprintf(f, " [%.2f .. %.2f]", e->lo, e->hi);
    fprintf(f, "  (%s)  %s\n", e->confidence, e->title);
}
