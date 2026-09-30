/*
 * uarch - measure instruction latency/throughput and core structure sizes
 * on Apple Silicon without root.
 */
#include "counters.h"
#include "exp.h"
#include "insn.h"
#include "jit.h"
#include "measure.h"
#include "report.h"
#include "sysinfo.h"

#include <math.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EXIT_SKIP 77 /* conventional "test skipped" status */

typedef struct {
    int level_mask;          /* bit l set: measure performance level l */
    const char *filter;      /* substring of the instruction name      */
    const char *experiments; /* comma-separated names, NULL = all      */
    const char *output;      /* JSON file, NULL = none                 */
    int quiet;
} options;

static void usage(FILE *f)
{
    fputs("usage: uarch <command> [options]\n"
          "\n"
          "commands:\n"
          "  info        what the OS reports about this chip, and which counters work\n"
          "  selftest    check the measurement itself (add = 1 cycle, ...); exit 77 if this\n"
          "              machine exposes no cycle counters (virtual machines)\n"
          "  list        list the instruction table\n"
          "  insn        measure instruction latency and throughput\n"
          "  structure   run the core-structure experiments\n"
          "  all         insn + structure\n"
          "\n"
          "options:\n"
          "  -l LEVEL    core type: P, E, a level number, or all (default all)\n"
          "  -f TEXT     only instructions whose name contains TEXT\n"
          "  -e NAMES    only these experiments (comma separated; see 'uarch structure -e help')\n"
          "  -o FILE     also write the results as JSON\n"
          "  -q          no per-line progress on stdout\n",
          f);
}

static double now_seconds(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom)
        mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

static int parse_level(const char *s, int nlevels, int *mask)
{
    if (strcmp(s, "all") == 0) {
        *mask = (1 << nlevels) - 1;
        return 0;
    }
    if (strcmp(s, "P") == 0 || strcmp(s, "p") == 0) {
        *mask = 1;
        return 0;
    }
    if (strcmp(s, "E") == 0 || strcmp(s, "e") == 0) {
        if (nlevels < 2)
            return -1;
        *mask = 1 << (nlevels - 1);
        return 0;
    }
    char *end;
    long v = strtol(s, &end, 10);
    if (*end || v < 0 || v >= nlevels)
        return -1;
    *mask = 1 << v;
    return 0;
}

static int name_selected(const char *list, const char *name)
{
    if (!list)
        return 1;
    size_t n = strlen(name);
    for (const char *p = list; *p;) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == n && strncmp(p, name, n) == 0)
            return 1;
        p += len;
        if (*p == ',')
            p++;
    }
    return 0;
}

static int cmd_info(const ua_sysinfo *si)
{
    printf("chip        %s (%s)\n", si->brand, si->model);
    printf("os          macOS %s (%s)%s\n", si->os_version, si->os_build,
           si->is_vm ? ", virtual machine" : "");
    printf("page size   %llu bytes\n", (unsigned long long)si->page_size);
    printf("counters    %s\n", ua_counters_backend_name(ua_counters_backend()));
    for (int l = 0; l < si->nlevels; l++) {
        const ua_syslevel *s = &si->level[l];
        printf("level %d     %s: \"%s\", %d cores, L1I %llu KiB, L1D %llu KiB, L2 %llu KiB "
               "shared by %d\n",
               l, ua_level_label(l, si->nlevels), s->name, s->cores,
               (unsigned long long)(s->l1i >> 10), (unsigned long long)(s->l1d >> 10),
               (unsigned long long)(s->l2 >> 10), s->cpus_per_l2);
    }
    int pac = ua_probe_pauth();
    printf("pauth       %s\n", pac == 1 ? "keys active: pac*/aut* sign and authenticate"
                           : pac == 0 ? "keys inactive in this process: pac*/aut* pass their "
                                        "operand through"
                                      : "unknown");
    if (ua_counters_backend() == UA_CTR_NONE) {
        printf("\nNo cycle counter is available to an unprivileged process here, so nothing "
               "can be measured.\n");
        return EXIT_SKIP;
    }
    for (int l = 0; l < si->nlevels; l++) {
        int ok = ua_level_request(l);
        printf("reach %s     %s\n", ua_level_label(l, si->nlevels),
               ok ? "yes (confirmed by the per-level counters)"
                  : "no: the scheduler did not move this thread there");
    }
    return 0;
}

static int cmd_list(const options *opt)
{
    const char *group = "";
    for (size_t i = 0; i < ua_n_insns; i++) {
        const ua_insn *in = &ua_insns[i];
        if (opt->filter && !strstr(in->name, opt->filter))
            continue;
        if (strcmp(group, in->group) != 0) {
            group = in->group;
            printf("\n%s\n", group);
        }
        printf("  %-26s %-44s %s\n", in->name, in->text, in->ext);
    }
    return 0;
}

/* Anchors: facts that must hold on every AArch64 core if the method works. */
static int cmd_selftest(const ua_sysinfo *si)
{
    static const struct {
        const char *name;
        int chain;
        double want;
        const char *why;
    } anchors[] = {
        {"add_x_reg", 0, 1.0, "a 64-bit add takes one cycle"},
        {"eor_x_reg", 0, 1.0, "a 64-bit eor takes one cycle"},
        {"sub_x_reg", 0, 1.0, "a 64-bit sub takes one cycle"},
    };
    if (ua_counters_backend() == UA_CTR_NONE) {
        printf("SKIP: no unprivileged cycle counter on this machine%s.\n",
               si->is_vm ? " (it is a virtual machine)" : "");
        return EXIT_SKIP;
    }
    int failures = 0, measured = 0;
    for (int l = 0; l < si->nlevels; l++) {
        const char *label = ua_level_label(l, si->nlevels);
        if (!ua_level_request(l)) {
            printf("%s-core: not reachable right now, skipped\n", label);
            continue;
        }
        ua_helpers h;
        ua_helpers_measure(l, &h);
        int ok = fabs(h.cmp_csinc - 2.0) < 0.05;
        printf("%s-core  %-4s cmp + csinc round trip = %.3f cycles (want 2: two one-cycle "
               "operations)\n",
               label, ok ? "ok" : "FAIL", h.cmp_csinc);
        failures += !ok;
        for (size_t a = 0; a < sizeof anchors / sizeof anchors[0]; a++) {
            const ua_insn *in = ua_insn_find(anchors[a].name);
            if (!in)
                continue;
            ua_insn_result r;
            ua_insn_run(in, l, &h, &r);
            const ua_lat_result *lr = &r.lat[anchors[a].chain];
            ok = r.supported && lr->raw.status == UA_OK && fabs(lr->lat - anchors[a].want) < 0.03;
            printf("%s-core  %-4s %-12s latency = %.3f cycles, %d clean runs, instructions per "
                   "iteration as generated (%s)\n",
                   label, ok ? "ok" : "FAIL", in->name, lr->lat, lr->raw.clean, anchors[a].why);
            failures += !ok;
            measured++;
        }
    }
    if (!measured) {
        printf("SKIP: no performance level could be measured.\n");
        return EXIT_SKIP;
    }
    ua_run_totals t = ua_measure_totals();
    printf("%llu runs, %llu discarded for migration, %llu discarded for interrupts\n",
           (unsigned long long)t.runs, (unsigned long long)t.migrated,
           (unsigned long long)t.disturbed);
    printf(failures ? "selftest FAILED\n" : "selftest passed\n");
    return failures ? 1 : 0;
}

static void run_insns(ua_report *rep, const options *opt, int level)
{
    const char *label = ua_level_label(level, rep->n_levels);
    ua_helpers *h = &rep->helpers[level];
    ua_helpers_measure(level, h);
    if (!opt->quiet)
        printf("\n# %s-core instructions   (tp = instances per cycle, * = limited by its own "
               "dependency chain; A>B = latency in cycles from operand A to B, rt = round trip "
               "through a helper)\n",
               label);
    const char *group = "";
    double ghz_sum = 0;
    int ghz_n = 0;
    for (size_t i = 0; i < ua_n_insns; i++) {
        const ua_insn *in = &ua_insns[i];
        if (opt->filter && !strstr(in->name, opt->filter))
            continue;
        ua_insn_result *res = &rep->insn[level][i];
        ua_insn_run(in, level, h, res);
        rep->insn_done[level][i] = 1;
        if (res->have_tp && res->tp_raw.ghz > 0) {
            ghz_sum += res->tp_raw.ghz;
            ghz_n++;
        }
        if (!opt->quiet) {
            if (strcmp(group, in->group) != 0) {
                group = in->group;
                printf("## %s\n", group);
            }
            ua_report_print_insn(res, stdout);
            fflush(stdout);
        }
    }
    if (ghz_n)
        rep->ghz[level] = ghz_sum / ghz_n;
}

static void run_experiments(ua_report *rep, const options *opt, int level)
{
    const char *label = ua_level_label(level, rep->n_levels);
    if (!opt->quiet)
        printf("\n# %s-core structure\n", label);
    for (size_t e = 0; e < ua_n_experiments; e++) {
        if (!name_selected(opt->experiments, ua_experiments[e].name))
            continue;
        int before = rep->exps[level].n;
        /* Experiments take long enough for the scheduler to move us. */
        ua_level_request(level);
        ua_experiments[e].run(level, &rep->exps[level]);
        if (!opt->quiet) {
            printf("## %s: %s\n", ua_experiments[e].name, ua_experiments[e].summary);
            for (int i = before; i < rep->exps[level].n; i++)
                ua_report_print_exp(&rep->exps[level].r[i], stdout);
            fflush(stdout);
        }
    }
}

int main(int argc, char **argv)
{
    options opt = {0, NULL, NULL, NULL, 0};
    ua_sysinfo si;

    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        printf("uarch %s\n", UA_TOOL_VERSION);
        return 0;
    }

    ua_sysinfo_get(&si);
    opt.level_mask = (1 << si.nlevels) - 1;
    optind = 2;
    int c;
    while ((c = getopt(argc, argv, "l:f:e:o:q")) != -1) {
        switch (c) {
        case 'l':
            if (parse_level(optarg, si.nlevels, &opt.level_mask) != 0) {
                fprintf(stderr, "uarch: unknown level '%s' (this chip has %d)\n", optarg,
                        si.nlevels);
                return 2;
            }
            break;
        case 'f': opt.filter = optarg; break;
        case 'e': opt.experiments = optarg; break;
        case 'o': opt.output = optarg; break;
        case 'q': opt.quiet = 1; break;
        default:
            usage(stderr);
            return 2;
        }
    }

    if (strcmp(cmd, "list") == 0)
        return cmd_list(&opt);

    int want_insn = strcmp(cmd, "insn") == 0 || strcmp(cmd, "all") == 0;
    int want_exp = strcmp(cmd, "structure") == 0 || strcmp(cmd, "all") == 0;
    int known = want_insn || want_exp || strcmp(cmd, "info") == 0 || strcmp(cmd, "selftest") == 0;
    if (!known) {
        fprintf(stderr, "uarch: unknown command '%s'\n\n", cmd);
        usage(stderr);
        return 2;
    }
    if (want_exp && opt.experiments && strcmp(opt.experiments, "help") == 0) {
        for (size_t e = 0; e < ua_n_experiments; e++)
            printf("%-8s %s\n", ua_experiments[e].name, ua_experiments[e].summary);
        return 0;
    }
    if (want_exp && opt.experiments) {
        for (const char *p = opt.experiments; *p;) {
            char name[32];
            size_t len = strcspn(p, ",");
            snprintf(name, sizeof name, "%.*s", (int)len, p);
            int found = 0;
            for (size_t e = 0; e < ua_n_experiments; e++)
                found |= strcmp(ua_experiments[e].name, name) == 0;
            if (!found) {
                fprintf(stderr, "uarch: unknown experiment '%s' (try -e help)\n", name);
                return 2;
            }
            p += len;
            if (*p == ',')
                p++;
        }
    }

    ua_counters_init(UA_CTR_NONE);
    ua_measure_init();
    if (ua_jit_init() != 0) {
        fprintf(stderr, "uarch: the kernel refused a MAP_JIT mapping; cannot generate code\n");
        return 1;
    }
    char err[160];
    if (ua_insns_verify(err, sizeof err) != 0) {
        fprintf(stderr, "uarch: instruction table is inconsistent: %s\n", err);
        return 1;
    }

    if (strcmp(cmd, "info") == 0)
        return cmd_info(&si);
    if (strcmp(cmd, "selftest") == 0)
        return cmd_selftest(&si);

    if (ua_counters_backend() == UA_CTR_NONE) {
        fprintf(stderr, "uarch: this machine exposes no cycle counter to an unprivileged "
                        "process%s; nothing can be measured.\n",
                si.is_vm ? " (it is a virtual machine)" : "");
        return EXIT_SKIP;
    }

    ua_report rep;
    if (ua_report_init(&rep) != 0) {
        fprintf(stderr, "uarch: out of memory\n");
        return 1;
    }
    double t0 = now_seconds();
    if (!opt.quiet)
        printf("%s, macOS %s, counters: %s\n", si.brand, si.os_version, rep.counters);

    for (int level = 0; level < si.nlevels; level++) {
        if (!(opt.level_mask & (1 << level)))
            continue;
        if (!ua_level_request(level)) {
            fprintf(stderr, "uarch: could not get onto the %s-cores (the scheduler kept the "
                            "thread elsewhere); skipping that level\n",
                    ua_level_label(level, si.nlevels));
            continue;
        }
        rep.level_measured[level] = 1;
        if (want_insn)
            run_insns(&rep, &opt, level);
        if (want_exp)
            run_experiments(&rep, &opt, level);
    }
    rep.seconds = now_seconds() - t0;

    ua_run_totals t = ua_measure_totals();
    if (!opt.quiet)
        printf("\n%.1f s, %llu timed runs: %llu clean, %llu discarded for migration between "
               "core types, %llu discarded for interrupts\n",
               rep.seconds, (unsigned long long)t.runs, (unsigned long long)t.clean,
               (unsigned long long)t.migrated, (unsigned long long)t.disturbed);

    int rc = 0;
    if (opt.output) {
        FILE *f = fopen(opt.output, "w");
        if (!f) {
            perror(opt.output);
            rc = 1;
        } else {
            if (ua_report_write_json(&rep, f) != 0) {
                fprintf(stderr, "uarch: failed to write %s\n", opt.output);
                rc = 1;
            }
            if (fclose(f) != 0)
                rc = 1;
        }
    }
    ua_report_free(&rep);
    return rc;
}
