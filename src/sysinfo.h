/*
 * sysinfo.h - what the operating system says about the chip.
 * Used to label results and as a cross-check for measured cache sizes.
 */
#ifndef UA_SYSINFO_H
#define UA_SYSINFO_H

#include <stdint.h>

#define UA_SYS_MAX_LEVELS 4

typedef struct {
    char name[32];       /* hw.perflevelN.name, e.g. "Performance"            */
    int cores;           /* hw.perflevelN.physicalcpu                         */
    uint64_t l1i, l1d, l2; /* bytes, 0 if not reported                        */
    int cpus_per_l2;
} ua_syslevel;

typedef struct {
    char brand[64];      /* machdep.cpu.brand_string, e.g. "Apple M5"         */
    char model[32];      /* hw.model                                          */
    char os_version[32]; /* kern.osproductversion                             */
    char os_build[32];   /* kern.osversion                                    */
    int nlevels;
    ua_syslevel level[UA_SYS_MAX_LEVELS];
    uint64_t page_size;
    uint64_t cacheline;  /* hw.cachelinesize                                  */
    uint64_t memsize;
    int is_vm;           /* kern.hv_vmm_present                               */
} ua_sysinfo;

void ua_sysinfo_get(ua_sysinfo *si);

/* "Apple M5" -> "apple-m5": lower case, runs of non-alphanumerics become '-'. */
void ua_slug(const char *in, char *out, unsigned long outsz);

/* Short label for a performance level: "P" for level 0, "E" for the last
 * level of a multi-level chip, "M" (middle) otherwise. */
const char *ua_level_label(int level, int nlevels);

#endif
