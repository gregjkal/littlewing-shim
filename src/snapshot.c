#include "snapshot.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "events.h"
#include "guest_mem.h"
#include "replay.h"
#include "trap.h"
#include "util.h"

/* The inventory (SNAPSHOT_STATE), filled before main. */
#define MAX_BLOCKS 32
static struct {
    const char *file, *name;
    void *p;
    size_t len;
    snapshot_save_fn save;
    snapshot_load_fn load;
    snapshot_drop_fn drop;
} inventory[MAX_BLOCKS];
static int ninventory;

/* A snapshot is always at depth 2. */
#define DEPTH 2

struct snapshot {
    mach_vm_address_t mem; /* the guest mapping, copied */
    cpu_context *cpu;      /* stopped at the trap it was taken at */
    struct {
        cpu_context *saved;     /* the context its guest_call saved */
        uint32_t called;        /* the guest function it called */
        uint32_t index, resume; /* the trap it is in, and where that returns to */
    } level[DEPTH];             /* each guest_call level's, at [depth - 1] */
    ev_frame frame;             /* the run loop's, above depth 2 */
    uint64_t clock_us;
    void *blocks[MAX_BLOCKS];
};

void snapshot_register(const char *file, const char *name, void *p, size_t len,
                       snapshot_save_fn save, snapshot_load_fn load, snapshot_drop_fn drop) {
    if (ninventory == MAX_BLOCKS)
        fatal("snapshot: more than %d blocks of module state", MAX_BLOCKS);
    const char *slash = strrchr(file, '/');
    inventory[ninventory].file = slash ? slash + 1 : file;
    inventory[ninventory].name = name;
    inventory[ninventory].p = p;
    inventory[ninventory].len = len;
    inventory[ninventory].save = save;
    inventory[ninventory].load = load;
    inventory[ninventory].drop = drop;
    ninventory++;
}

int snapshot_blocks(size_t *bytes) {
    *bytes = 0;
    for (int i = 0; i < ninventory; i++)
        *bytes += inventory[i].len;
    return ninventory;
}

uint64_t snapshot_clock_us(const snapshot *s) { return s->clock_us; }

/* Whether the run loop's frame f is the same as g: the same timer, or the
   same dispatch at the same handler, with the same loop depth hidden. */
static bool same_frame(const ev_frame *f, const ev_frame *g) {
    if (f->timer != g->timer || f->loop_depth != g->loop_depth)
        return false;
    if (f->timer >= 0)
        return true;
    if (f->slot != g->slot || f->nchain != g->nchain || f->link != g->link ||
        f->handler != g->handler)
        return false;
    for (int i = 0; i < f->nchain; i++)
        if (f->chain[i] != g->chain[i])
            return false;
    return true;
}

/* At depth 2, the run loop's frame above, when depth 1 is in the run loop. */
static bool loop_frame(ev_frame *f, uint32_t *index, uint32_t *resume) {
    return trap_depth() == DEPTH && trap_level(1, index, resume) &&
           strcmp(trap_import_name(*index), "RunApplicationEventLoop") == 0 && events_loop_frame(f);
}

snapshot *snapshot_take(uint32_t index) {
    ev_frame f;
    uint32_t loop_index, loop_resume;
    if (!loop_frame(&f, &loop_index, &loop_resume))
        return NULL;
    snapshot *s = calloc(1, sizeof *s);
    if (!s)
        fatal("out of memory");
    size_t len = gm_host_size();
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &s->mem, len, VM_FLAGS_ANYWHERE);
    if (kr == KERN_SUCCESS)
        kr = vm_copy(mach_task_self(), (vm_address_t)(uintptr_t)gm_host_base(), len,
                     (vm_address_t)s->mem);
    if (kr != KERN_SUCCESS)
        fatal("snapshot: can't copy the guest mapping: %s", mach_error_string(kr));
    s->cpu = cpu_save();
    for (int d = 1; d <= DEPTH; d++) {
        s->level[d - 1].saved = cpu_context_clone(trap_saved_context(d));
        s->level[d - 1].called = trap_call_target(d);
    }
    s->level[0].index = loop_index;
    s->level[0].resume = loop_resume;
    s->level[1].index = index; /* the entry being taken */
    s->level[1].resume = cpu_lr();
    s->frame = f;
    s->clock_us = replay_now_us();
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
    return s;
}

void snapshot_free(snapshot *s) {
    if (!s)
        return;
    mach_vm_deallocate(mach_task_self(), s->mem, gm_host_size());
    cpu_context_free(s->cpu);
    for (int d = 0; d < DEPTH; d++)
        cpu_context_free(s->level[d].saved);
    for (int i = 0; i < ninventory; i++) {
        if (inventory[i].drop)
            inventory[i].drop(s->blocks[i]);
        else
            free(s->blocks[i]);
    }
    free(s);
}

bool snapshot_fits(const snapshot *s) {
    ev_frame f;
    uint32_t index, resume;
    return loop_frame(&f, &index, &resume) && same_frame(&f, &s->frame) &&
           index == s->level[0].index && resume == s->level[0].resume &&
           trap_call_target(1) == s->level[0].called && trap_call_target(2) == s->level[1].called;
}

/* Puts back what isn't the CPU's: the guest mapping and the module blocks,
   then drops every translation and cached page. */
static void put_back_memory(const snapshot *s) {
    kern_return_t kr = vm_copy(mach_task_self(), (vm_address_t)s->mem, gm_host_size(),
                               (vm_address_t)(uintptr_t)gm_host_base());
    if (kr != KERN_SUCCESS)
        fatal("snapshot: can't copy the guest mapping back: %s", mach_error_string(kr));
    for (int i = 0; i < ninventory; i++) {
        if (inventory[i].load)
            inventory[i].load(s->blocks[i]);
        else
            memcpy(inventory[i].p, s->blocks[i], inventory[i].len);
    }
    cpu_flush_translations();
}

uint32_t snapshot_restore(const snapshot *s) {
    if (!snapshot_fits(s))
        fatal("snapshot: restoring in place where it doesn't fit");
    put_back_memory(s);
    cpu_load(s->cpu);
    for (int d = 1; d <= DEPTH; d++)
        cpu_context_copy(trap_saved_context(d), s->level[d - 1].saved);
    return s->level[1].index;
}

/* What snapshot_reenter rebuilds depth 2 from, copied out of the snapshot,
   which may be gone by the time the run loop gets there. A longjmp out of
   the rebuilt levels leaves the copies for the next re-entry to free. */
static struct {
    uint32_t called, index, resume;
    cpu_context *outer, *cpu;
} at;

static void drop_at(void) {
    cpu_context_free(at.outer);
    cpu_context_free(at.cpu);
    at.outer = at.cpu = NULL;
}

/* Stands in for the run loop's guest call to the handler or the timer's
   callback: rebuilds depth 2 at the trap the snapshot was taken at. */
static uint32_t resume_depth_2(void) {
    uint32_t r = guest_resume(at.called, at.outer, at.cpu, at.index, at.resume);
    drop_at();
    return r;
}

void snapshot_reenter(const snapshot *s, void (*after)(void)) {
    trap_unwind();
    events_unwind();
    put_back_memory(s);
    drop_at();
    at.called = s->level[1].called;
    at.index = s->level[1].index;
    at.resume = s->level[1].resume;
    at.outer = cpu_context_clone(s->level[1].saved);
    at.cpu = cpu_context_clone(s->cpu);
    ev_frame frame = s->frame;
    if (after)
        after();
    events_resume_loop(&frame, resume_depth_2);
    /* guest_resume copies both contexts before any guest code runs. */
    guest_resume(s->level[0].called, s->level[0].saved, s->level[1].saved, s->level[0].index,
                 s->level[0].resume);
}
