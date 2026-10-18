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
#include "events.h"
#include "guest_mem.h"
#include "hd.h"
#include "misc.h"
#include "replay.h"
#include "trap.h"
#include "util.h"

static struct {
    bool on;
    double cow_at; /* seconds of the game's clock, or -1 */
    bool cow_done;
    const char *delta_dir; /* LOONY_MEASURE_DELTA, or NULL */
    uint64_t baseline_at_us; /* LOONY_MEASURE_DELTA_AT, in us */
    bool baseline_done;
} MS;

static double process_start; /* wall_seconds() before main */

__attribute__((constructor)) static void note_process_start(void) { process_start = wall_seconds(); }

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

/* The delta spike (measure.h): writes the resident pages to the folder's
   name, and returns how many. With check_rest, also counts the pages
   mincore leaves out that hold something, which reads them. */
static size_t write_pages(const char *name, bool check_rest) {
    size_t npages, page;
    char *vec = resident_pages(&npages, &page);
    if (!vec)
        fatal("measure: can't tell which guest pages are resident");
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", MS.delta_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f)
        fatal("measure: can't write %s", path);
    const uint8_t *base = gm_host_base();
    size_t n = 0, missed = 0;
    for (size_t i = 0; i < npages; i++) {
        uint64_t off = (uint64_t)i * page;
        if (vec[i] & MINCORE_INCORE) {
            uint8_t le[8];
            for (int b = 0; b < 8; b++)
                le[b] = (uint8_t)(off >> (8 * b));
            if (fwrite(le, 1, 8, f) != 8 || fwrite(base + off, 1, page, f) != page)
                fatal("measure: can't write %s", path);
            n++;
        } else if (check_rest) {
            for (size_t k = 0; k < page && off + k < gm_host_size(); k++)
                if (base[off + k]) {
                    missed++;
                    break;
                }
        }
    }
    if (fclose(f) != 0)
        fatal("measure: can't write %s", path);
    free(vec);
    if (check_rest)
        log_msg("measure: delta: %zu pages mincore leaves out hold something", missed);
    return n;
}

/* The delta spike's trap entry hook: the baseline, then the game's start. */
static uint32_t delta_entry(uint32_t index) {
    ev_frame f;
    uint32_t loop_index, loop_resume;
    if (trap_depth() != 2 || !trap_level(1, &loop_index, &loop_resume) ||
        strcmp(trap_import_name(loop_index), "RunApplicationEventLoop") != 0 ||
        !events_loop_frame(&f))
        return index;
    const replay_span *spans;
    bool start = MS.baseline_done && f.timer < 0 && replay_play_spans(&spans) &&
                 replay_now_us() >= spans[0].on_us;
    if (MS.baseline_done ? !start : f.timer < 0 || replay_now_us() < MS.baseline_at_us)
        return index;
    double t0 = wall_seconds();
    const char *name = start ? "start.pages" : "baseline.pages";
    size_t n = write_pages(name, start);
    log_msg("measure: delta: %s at %.3f s of the replay's clock, trap entry %llu, %.3f s after "
            "the process started: %zu pages of %d bytes (written in %.1f ms)",
            name, (double)replay_now_us() / 1e6, (unsigned long long)trap_entries(),
            t0 - process_start, n, getpagesize(), (wall_seconds() - t0) * 1000);
    if (!start) {
        MS.baseline_done = true;
        return index;
    }
    exit(0);
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
    const char *delta = getenv("LOONY_MEASURE_DELTA");
    MS.delta_dir = delta && *delta ? delta : NULL;
    MS.baseline_done = false;
    const char *at = getenv("LOONY_MEASURE_DELTA_AT");
    MS.baseline_at_us = at && *at ? (uint64_t)(strtod(at, NULL) * 1e6) : 0;
    if (MS.delta_dir) {
        if (replay_playing())
            trap_on_entry(delta_entry);
        else
            log_msg("measure: LOONY_MEASURE_DELTA is for a replay; nothing measured");
    }
}

void measure_present(void) {
    if (MS.cow_at < 0 || MS.cow_done || misc_last_seconds() < MS.cow_at)
        return;
    MS.cow_done = true;
    cow_spike();
}
