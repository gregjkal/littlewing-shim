#include "measure.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "cpu.h"
#include "guest_mem.h"
#include "hd.h"
#include "misc.h"
#include "trap.h"
#include "util.h"

static struct {
    bool on;
    double cow_at; /* seconds of the game's clock, or -1 */
    bool cow_done;
} MS;

static double mb(size_t bytes) { return (double)bytes / (1024 * 1024); }

/* Which pages of the guest mapping are resident: one byte a page, malloc'd,
   with *npages pages of *page bytes. NULL, logged, if mincore fails. */
static char *resident_pages(size_t *npages, size_t *page) {
    *page = (size_t)getpagesize();
    *npages = (gm_host_size() + *page - 1) / *page;
    char *vec = malloc(*npages ? *npages : 1);
    if (!vec)
        fatal("out of memory");
    if (mincore(gm_host_base(), gm_host_size(), vec) != 0) {
        log_msg("measure: mincore failed");
        free(vec);
        return NULL;
    }
    return vec;
}

static size_t count_resident(const char *vec, size_t from, size_t to) {
    size_t n = 0;
    for (size_t i = from; i < to; i++)
        n += vec[i] & MINCORE_INCORE;
    return n;
}

static void log_sizes(void) {
    size_t npages, page;
    char *vec = resident_pages(&npages, &page);
    if (vec) {
        const gm_region *regions;
        int n = gm_regions(&regions);
        char text[512] = "";
        for (int i = 0; i < n; i++) {
            size_t from = regions[i].base / page, /* the pages the region touches */
                to = (regions[i].base + regions[i].size + page - 1) / page;
            if (to > npages)
                to = npages;
            size_t len = strlen(text);
            snprintf(text + len, sizeof text - len, "%s0x%08x: %.1f of %.1f MB", i ? ", " : "",
                     regions[i].base, mb(count_resident(vec, from, to) * page), mb(regions[i].size));
        }
        log_msg("measure: guest memory: %.1f MB resident of %.1f MB mapped (%s)",
                mb(count_resident(vec, 0, npages) * page), mb(gm_host_size()), text);
        free(vec);
    }
    log_msg("measure: a saved CPU context takes %zu bytes", cpu_context_size());
    int copies;
    size_t bytes = hd_copies_bytes(&copies);
    if (hd_scale())
        log_msg("measure: HD copies: %d, %.1f MB at %dx", copies, mb(bytes), hd_scale());
    const uint64_t *counts;
    size_t depths = trap_depth_counts(&counts);
    char text[256] = "";
    uint64_t total = 0;
    for (size_t d = 1; d < depths; d++) {
        size_t len = strlen(text);
        snprintf(text + len, sizeof text - len, "%s%zu%s: %llu", d > 1 ? ", " : "", d,
                 d + 1 == depths ? " and deeper" : "", (unsigned long long)counts[d]);
        total += counts[d];
    }
    log_msg("measure: trap entries by depth: %s (%llu in all)", text, (unsigned long long)total);
}

/* Writes each resident page's first byte back as it is: the live side's
   first write to a page after a copy-on-write copy, which the kernel
   answers by giving it a private page again. Nothing the game sees changes. */
static void touch_resident(const char *vec, size_t npages, size_t page) {
    uint8_t *base = gm_host_base();
    for (size_t i = 0; i < npages; i++)
        if (vec[i] & MINCORE_INCORE) {
            volatile uint8_t *p = base + i * page;
            *p = *p;
        }
}

/* The copy-on-write spike (measure.h). */
static void cow_spike(void) {
    size_t npages, page;
    char *vec = resident_pages(&npages, &page);
    if (!vec)
        return;
    uint8_t *src = gm_host_base();
    size_t len = gm_host_size(), resident = count_resident(vec, 0, npages);
    for (int variant = 0; variant < 2; variant++) {
        const char *name = variant == 0 ? "mach_vm_remap" : "vm_copy";
        mach_vm_address_t dst = 0;
        kern_return_t kr;
        double t0 = wall_seconds();
        if (variant == 0) {
            vm_prot_t cur, max;
            kr = mach_vm_remap(mach_task_self(), &dst, len, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                               (mach_vm_address_t)(uintptr_t)src, TRUE, &cur, &max, VM_INHERIT_NONE);
        } else {
            kr = mach_vm_allocate(mach_task_self(), &dst, len, VM_FLAGS_ANYWHERE);
            if (kr == KERN_SUCCESS)
                kr = vm_copy(mach_task_self(), (vm_address_t)(uintptr_t)src, len, (vm_address_t)dst);
        }
        double t1 = wall_seconds();
        if (kr != KERN_SUCCESS) {
            log_msg("measure: copy-on-write with %s failed: %s", name, mach_error_string(kr));
            if (dst)
                mach_vm_deallocate(mach_task_self(), dst, len);
            continue;
        }
        /* The copy holds what the guest held. */
        size_t differ = 0;
        for (size_t i = 0; i < npages; i++)
            if ((vec[i] & MINCORE_INCORE) &&
                memcmp(src + i * page, (const uint8_t *)(uintptr_t)dst + i * page, page) != 0)
                differ++;
        double t2 = wall_seconds();
        touch_resident(vec, npages, page); /* the faults */
        double t3 = wall_seconds();
        touch_resident(vec, npages, page); /* again, private now: the loop alone */
        double t4 = wall_seconds();
        log_msg("measure: copy-on-write with %s: %.1f MB resident of %.1f MB mapped copied in "
                "%.3f ms (%zu of %zu pages differ, checked in %.1f ms); the first write to every "
                "resident page took %.2f ms (%.2f us a page), the loop alone %.2f ms",
                name, mb(resident * page), mb(len), (t1 - t0) * 1000, differ, resident,
                (t2 - t1) * 1000, (t3 - t2) * 1000, resident ? (t3 - t2) * 1e6 / (double)resident : 0,
                (t4 - t3) * 1000);
        mach_vm_deallocate(mach_task_self(), dst, len);
    }
    free(vec);
}

void measure_init(void) {
    const char *on = getenv("LOONY_MEASURE"), *cow = getenv("LOONY_MEASURE_COW");
    MS.on = on && strcmp(on, "1") == 0;
    MS.cow_at = -1;
    MS.cow_done = false;
    if (cow && *cow) {
        char *e;
        MS.cow_at = strtod(cow, &e);
        if (*e || MS.cow_at < 0) {
            log_msg("measure: LOONY_MEASURE_COW=%s isn't a number of seconds; no spike", cow);
            MS.cow_at = -1;
        }
    }
    if (MS.on)
        atexit(log_sizes);
}

void measure_present(void) {
    if (MS.cow_at < 0 || MS.cow_done || misc_last_seconds() < MS.cow_at)
        return;
    MS.cow_done = true;
    cow_spike();
}
