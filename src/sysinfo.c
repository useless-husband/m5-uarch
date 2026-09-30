#include "sysinfo.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

static void get_str(const char *name, char *out, size_t outsz)
{
    size_t len = outsz;
    out[0] = 0;
    if (sysctlbyname(name, out, &len, NULL, 0) != 0)
        out[0] = 0;
    out[outsz - 1] = 0;
}

static uint64_t get_u64(const char *name)
{
    uint64_t v = 0;
    size_t len = sizeof v;
    if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
        return 0;
    if (len == 4) {
        uint32_t v32;
        memcpy(&v32, &v, 4);
        return v32;
    }
    return v;
}

void ua_sysinfo_get(ua_sysinfo *si)
{
    char key[64];
    memset(si, 0, sizeof *si);
    get_str("machdep.cpu.brand_string", si->brand, sizeof si->brand);
    get_str("hw.model", si->model, sizeof si->model);
    get_str("kern.osproductversion", si->os_version, sizeof si->os_version);
    get_str("kern.osversion", si->os_build, sizeof si->os_build);
    si->page_size = get_u64("hw.pagesize");
    si->cacheline = get_u64("hw.cachelinesize");
    si->memsize = get_u64("hw.memsize");
    si->is_vm = (int)get_u64("kern.hv_vmm_present");
    int n = (int)get_u64("hw.nperflevels");
    if (n < 1)
        n = 1;
    if (n > UA_SYS_MAX_LEVELS)
        n = UA_SYS_MAX_LEVELS;
    si->nlevels = n;
    for (int i = 0; i < n; i++) {
        ua_syslevel *l = &si->level[i];
        snprintf(key, sizeof key, "hw.perflevel%d.name", i);
        get_str(key, l->name, sizeof l->name);
        snprintf(key, sizeof key, "hw.perflevel%d.physicalcpu", i);
        l->cores = (int)get_u64(key);
        snprintf(key, sizeof key, "hw.perflevel%d.l1icachesize", i);
        l->l1i = get_u64(key);
        snprintf(key, sizeof key, "hw.perflevel%d.l1dcachesize", i);
        l->l1d = get_u64(key);
        snprintf(key, sizeof key, "hw.perflevel%d.l2cachesize", i);
        l->l2 = get_u64(key);
        snprintf(key, sizeof key, "hw.perflevel%d.cpusperl2", i);
        l->cpus_per_l2 = (int)get_u64(key);
    }
}

void ua_slug(const char *in, char *out, unsigned long outsz)
{
    unsigned long o = 0;
    int dash = 0;
    if (!outsz)
        return;
    for (; *in && o + 1 < outsz; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c)) {
            if (dash && o > 0 && o + 2 < outsz)
                out[o++] = '-';
            dash = 0;
            out[o++] = (char)tolower(c);
        } else {
            dash = 1;
        }
    }
    out[o] = 0;
}

const char *ua_level_label(int level, int nlevels)
{
    if (level <= 0)
        return "P";
    if (level == nlevels - 1)
        return "E";
    return "M";
}
