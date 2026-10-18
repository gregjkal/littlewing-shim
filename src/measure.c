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
} MS;

/* Spike A's inventory (MEASURE_STATE), filled before main. */
#define MAX_BLOCKS 32
static struct {
    const char *file, *name;
    void *p;
    size_t len;
    measure_save_fn save;
    measure_load_fn load;
} inventory[MAX_BLOCKS];
static int ninventory;

/* The emulator captured at a trap entry, for the spikes. */
typedef struct {
    mach_vm_address_t mem; /* the guest mapping, copied */
    cpu_context *cpu;
    int depth;
    struct {
        cpu_context *saved;    /* the context its guest_call saved */
        uint32_t called;       /* the guest function it called */
        uint32_t index, resume; /* the trap it is in, and where that returns to */
    } level[TRAP_MAX_DEPTH];   /* each guest_call level's, at [depth - 1] */
    void *blocks[MAX_BLOCKS];
    double clock;            /* the game's clock at the capture */
    double ms[3];            /* what the mapping, the CPU and the blocks took */
} snapshot;

/* Spike A (measure.h). */
static struct {
    double at, back; /* N and M, seconds of the game's clock; at < 0: off */
    enum { SA_WAITING, SA_CAPTURED, SA_DONE } state;
    snapshot s;
} SA;

/* Spike B (measure.h). */
static struct {
    double at; /* N, seconds of the game's clock; < 0: off */
    enum { SB_WAITING, SB_CAPTURED, SB_UNWOUND, SB_DONE } state;
    snapshot s;
    ev_frame frame; /* the run loop's dispatch above the handler */
} SB;

jmp_buf measure_unwound;

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

void measure_register(const char *file, const char *name, void *p, size_t len,
                      measure_save_fn save, measure_load_fn load) {
    if (ninventory == MAX_BLOCKS)
        fatal("measure: more than %d blocks of module state", MAX_BLOCKS);
    const char *slash = strrchr(file, '/');
    inventory[ninventory].file = slash ? slash + 1 : file;
    inventory[ninventory].name = name;
    inventory[ninventory].p = p;
    inventory[ninventory].len = len;
    inventory[ninventory].save = save;
    inventory[ninventory].load = load;
    ninventory++;
}

/* The guest function the guest_call at depth entered, for the log. */
static const char *call_target(int depth, char buf[static 32]) {
    return trap_format_addr(trap_call_target(depth), buf);
}

/* Captures the emulator into *s at the entry of trap index, from the trap
   entry hook: the guest mapping with vm_copy, the CPU, each guest_call
   level, and every block of module state, crudely or with its save. */
static void capture(snapshot *s, const char *who, uint32_t index) {
    double t0 = wall_seconds();
    size_t len = gm_host_size();
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &s->mem, len, VM_FLAGS_ANYWHERE);
    if (kr == KERN_SUCCESS)
        kr = vm_copy(mach_task_self(), (vm_address_t)(uintptr_t)gm_host_base(), len,
                     (vm_address_t)s->mem);
    if (kr != KERN_SUCCESS)
        fatal("%s: can't copy the guest mapping: %s", who, mach_error_string(kr));
    double t1 = wall_seconds();
    s->cpu = cpu_save();
    s->depth = trap_depth();
    for (int d = 1; d <= s->depth; d++) {
        s->level[d - 1].saved = cpu_context_clone(trap_saved_context(d));
        s->level[d - 1].called = trap_call_target(d);
        if (d < s->depth)
            trap_level(d, &s->level[d - 1].index, &s->level[d - 1].resume);
    }
    s->level[s->depth - 1].index = index; /* the entry being captured */
    s->level[s->depth - 1].resume = cpu_lr();
    double t2 = wall_seconds();
    for (int i = 0; i < ninventory; i++) {
        if (inventory[i].save) {
            s->blocks[i] = inventory[i].save();
        } else {
            s->blocks[i] = malloc(inventory[i].len);
            if (!s->blocks[i])
                fatal("out of memory");
            memcpy(s->blocks[i], inventory[i].p, inventory[i].len);
        }
    }
    double t3 = wall_seconds();
    s->clock = misc_last_seconds();
    s->ms[0] = (t1 - t0) * 1000;
    s->ms[1] = (t2 - t1) * 1000;
    s->ms[2] = (t3 - t2) * 1000;
}

static size_t inventory_bytes(void) {
    size_t bytes = 0;
    for (int i = 0; i < ninventory; i++)
        bytes += inventory[i].len;
    return bytes;
}

/* Puts back what isn't the CPU's: the guest mapping and the module blocks
   (freeing their copies), then drops every translation. Returns what the
   mapping took, in ms. */
static double put_back_memory(snapshot *s, const char *who) {
    double t0 = wall_seconds();
    kern_return_t kr = vm_copy(mach_task_self(), (vm_address_t)s->mem, gm_host_size(),
                               (vm_address_t)(uintptr_t)gm_host_base());
    if (kr != KERN_SUCCESS)
        fatal("%s: can't copy the guest mapping back: %s", who, mach_error_string(kr));
    double t1 = wall_seconds();
    for (int i = 0; i < ninventory; i++) {
        if (inventory[i].load) {
            inventory[i].load(s->blocks[i]); /* and frees it */
        } else {
            memcpy(inventory[i].p, s->blocks[i], inventory[i].len);
            free(s->blocks[i]);
        }
    }
    cpu_flush_translations();
    return (t1 - t0) * 1000;
}

/* Frees what put_back_memory left: the mapping's copy and the contexts. */
static void drop(snapshot *s) {
    mach_vm_deallocate(mach_task_self(), s->mem, gm_host_size());
    cpu_context_free(s->cpu);
    for (int d = 0; d < s->depth; d++)
        cpu_context_free(s->level[d].saved);
}

static void spike_a_capture(uint32_t index) {
    capture(&SA.s, "spike A", index);
    const snapshot *s = &SA.s;
    char a[32], b[32];
    log_msg("spike A: captured at %.3f s of the game's clock, at #%u %s (depth %d, in %s, "
            "returning to %s), in %.3f ms: the mapping in %.3f ms, the CPU and %d saved "
            "contexts in %.3f ms, %d blocks of module state (%zu bytes) in %.3f ms",
            s->clock, index, trap_import_name(index), s->depth, call_target(s->depth, a),
            trap_format_addr(s->level[s->depth - 1].resume, b), s->ms[0] + s->ms[1] + s->ms[2],
            s->ms[0], s->depth, s->ms[1], ninventory, inventory_bytes(), s->ms[2]);
}

/* Puts the capture back and returns the trap it was taken at, for
   guest_call to dispatch in place of the one it stopped at. */
static uint32_t spike_a_restore(uint32_t index) {
    snapshot *s = &SA.s;
    double from = misc_last_seconds(), t0 = wall_seconds();
    if (trap_depth() != s->depth)
        fatal("spike A: at depth %d, not %d as captured", trap_depth(), s->depth);
    double mapping = put_back_memory(s, "spike A");
    cpu_load(s->cpu);
    for (int d = 1; d <= s->depth; d++)
        cpu_context_copy(trap_saved_context(d), s->level[d - 1].saved);
    double t2 = wall_seconds();
    uint32_t back = s->level[s->depth - 1].index;
    char a[32], b[32];
    log_msg("spike A: restored at %.3f s of the game's clock, at #%u %s (in %s), back to "
            "%.3f s, in %.3f ms (the mapping in %.3f ms); dispatching #%u %s, returning to %s",
            from, index, trap_import_name(index), call_target(s->depth, a), s->clock,
            (t2 - t0) * 1000, mapping, back, trap_import_name(back),
            trap_format_addr(cpu_lr(), b));
    drop(s);
    return back;
}

/* Spike A's trap entry hook: depth-2 entries only, where Loony's games run. */
static uint32_t spike_a_entry(uint32_t index) {
    if (trap_depth() != 2)
        return index;
    double now = misc_last_seconds();
    if (SA.state == SA_WAITING && now >= SA.at) {
        spike_a_capture(index);
        SA.state = SA_CAPTURED;
    } else if (SA.state == SA_CAPTURED && now >= SA.back &&
               trap_call_target(2) == SA.s.level[1].called) { /* the same host frames above */
        index = spike_a_restore(index);
        SA.state = SA_DONE;
        trap_on_entry(NULL);
    }
    return index;
}

/* Spike B's trap entry hook: the first depth-2 entry past N inside a
   handler the run loop dispatched to, where Loony's games run. */
static uint32_t spike_b_entry(uint32_t index) {
    ev_frame f;
    if (trap_depth() != 2 || misc_last_seconds() < SB.at || !events_loop_frame(&f))
        return index;
    capture(&SB.s, "spike B", index);
    SB.frame = f;
    SB.state = SB_CAPTURED;
    trap_on_entry(NULL);
    const snapshot *s = &SB.s;
    char a[32], b[32], c[32];
    log_msg("spike B: captured at %.3f s of the game's clock, at #%u %s in %s (depth 2), "
            "returning to %s, in %.3f ms; above it, at depth 1, #%u %s in %s; the run loop "
            "dispatching the event in slot %d to handler %d, target %d of %d, loop depth %d",
            s->clock, index, trap_import_name(index), call_target(2, a),
            trap_format_addr(s->level[1].resume, b), s->ms[0] + s->ms[1] + s->ms[2],
            s->level[0].index, trap_import_name(s->level[0].index), call_target(1, c), f.slot,
            f.handler, f.link + 1, f.nchain, f.loop_depth);
    return index;
}

/* Stands in for the run loop's guest call to the game's handler: rebuilds
   depth 2 from the capture, at the trap it was taken at. */
static uint32_t spike_b_handler(void) {
    const snapshot *s = &SB.s;
    uint32_t r = guest_resume(s->level[1].called, s->level[1].saved, s->cpu, s->level[1].index,
                              s->level[1].resume);
    log_msg("spike B: the handler returned %d to the run loop at %.3f s of the game's clock",
            (int32_t)r, misc_last_seconds());
    return r;
}

/* Back to the capture, from main with no guest call on the host stack:
   restores memory and the module blocks, then rebuilds depth 1 at the
   RunApplicationEventLoop trap it was in, whose handler rebuilds the
   dispatch and calls spike_b_handler for depth 2. Returns when the game's
   main returns again. */
static void spike_b_reenter(void) {
    snapshot *s = &SB.s;
    double t0 = wall_seconds();
    int unwound = trap_depth();
    trap_unwind();
    double mapping = put_back_memory(s, "spike B");
    SB.state = SB_DONE;
    /* Not the clock it left: where the replay's end fires inside a clock
       read, misc holds the host's time, not the recording's. */
    log_msg("spike B: re-entering from main (%d guest calls unwound), back to %.3f s of the "
            "game's clock, memory restored in %.3f ms (the mapping in %.3f ms)",
            unwound, s->clock, (wall_seconds() - t0) * 1000, mapping);
    events_resume_loop(&SB.frame, spike_b_handler);
    guest_resume(s->level[0].called, s->level[0].saved, s->level[1].saved, s->level[0].index,
                 s->level[0].resume);
    log_msg("spike B: main returned again at %.3f s of the game's clock", misc_last_seconds());
    drop(s);
}

void measure_game_ended(void) {
    if (SB.state == SB_UNWOUND || (SB.state == SB_CAPTURED && trap_depth() == 0)) {
        if (SB.state == SB_CAPTURED)
            log_msg("spike B: the game's main returned");
        spike_b_reenter();
    } else if (SB.state == SB_CAPTURED) { /* inside a trap: back to main's setjmp */
        uint32_t index = 0, resume;
        trap_level(trap_depth(), &index, &resume);
        char a[32];
        log_msg("spike B: the game ended inside #%u %s at depth %d (in %s); unwinding to main",
                index, trap_import_name(index), trap_depth(), call_target(trap_depth(), a));
        SB.state = SB_UNWOUND;
        longjmp(measure_unwound, 1);
    }
}

static void spike_b_at_exit(void) {
    if (SB.state == SB_WAITING || SB.state == SB_CAPTURED)
        log_msg("spike B: %s", SB.state == SB_WAITING ? "never captured" : "never re-entered");
}

static void spike_a_at_exit(void) {
    if (SA.state != SA_DONE)
        log_msg("spike A: %s", SA.state == SA_WAITING ? "never captured" : "never restored");
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
    const char *spike = getenv("LOONY_SPIKE_A");
    SA.at = -1;
    SA.state = SA_WAITING;
    if (spike && *spike) {
        char *e;
        SA.at = strtod(spike, &e);
        if (*e == ',')
            SA.back = strtod(e + 1, &e);
        if (*e || SA.at < 0 || SA.back <= SA.at) {
            log_msg("measure: LOONY_SPIKE_A=%s isn't <N>,<M> seconds with M after N; no spike",
                    spike);
            SA.at = -1;
        } else {
            trap_on_entry(spike_a_entry);
            atexit(spike_a_at_exit);
        }
    }
    spike = getenv("LOONY_SPIKE_B");
    SB.at = -1;
    SB.state = SB_WAITING;
    if (spike && *spike) {
        char *e;
        SB.at = strtod(spike, &e);
        if (*e || SB.at < 0) {
            log_msg("measure: LOONY_SPIKE_B=%s isn't a number of seconds; no spike", spike);
            SB.at = -1;
        } else if (SA.at >= 0 || !replay_playing()) {
            log_msg("measure: LOONY_SPIKE_B needs a replay, and no spike A; no spike");
            SB.at = -1;
        } else {
            trap_on_entry(spike_b_entry);
            replay_set_end(measure_game_ended); /* a watched replay's viewer replaces it */
            atexit(spike_b_at_exit);
        }
    }
}

void measure_present(void) {
    if (MS.cow_at < 0 || MS.cow_done || misc_last_seconds() < MS.cow_at)
        return;
    MS.cow_done = true;
    cow_spike();
}
