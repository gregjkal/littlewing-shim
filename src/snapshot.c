#include "snapshot.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

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
    snapshot_walk_fn walk;
    uint32_t tag; /* fnv1a32 of "file:name" */
} inventory[MAX_BLOCKS];
static int ninventory;

/* The module state's header: magic, version, the layout's hash and the
   number of blocks, 32 bits each. Then each block: its tag and the size of
   its variable, 32 bits each, the length of its body, 64 bits, and the
   body. All in the host's byte order. */
#define STATE_MAGIC 0x4C57736Eu /* 'LWsn' */
#define STATE_VERSION 1u
#define STATE_HEADER 16
#define BLOCK_HEADER 16
#define NO_OWNED UINT64_MAX /* an owned pointer's length when it is NULL */

/* Bytes a block has walked so far, the module's or what it owns, and where
   they are in the buffer; loading, also where the stash keeps what they
   held before (SIZE_MAX for bytes loading made anew). */
typedef struct {
    const uint8_t *live;
    size_t len, at, stash;
} region;

struct snapshot_walker {
    bool loading;
    uint8_t *buf; /* saving: grown as written, zeroed; loading: read */
    size_t len, cap, at;
    int block;        /* the inventory index being walked, -1 for none */
    size_t body, end; /* the block's body, from body to end (end: loading) */
    region *regions;
    size_t nregions, region_cap;
    uint8_t *stash; /* loading: the bytes the module held before */
    size_t stash_len, stash_cap;
    /* Saving to verify: the state compared with, block by block. */
    const uint8_t *ref;
    size_t ref_len, ref_at;
    char *what;
    size_t what_cap;
    bool differs;
};

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
    uint8_t *state; /* the module state */
    size_t state_len;
};

void snapshot_register(const char *file, const char *name, void *p, size_t len,
                       snapshot_walk_fn walk) {
    if (ninventory == MAX_BLOCKS)
        fatal("snapshot: more than %d blocks of module state", MAX_BLOCKS);
    const char *slash = strrchr(file, '/');
    file = slash ? slash + 1 : file;
    char tag[256];
    snprintf(tag, sizeof tag, "%s:%s", file, name);
    inventory[ninventory].file = file;
    inventory[ninventory].name = name;
    inventory[ninventory].p = p;
    inventory[ninventory].len = len;
    inventory[ninventory].walk = walk;
    inventory[ninventory].tag = fnv1a32(tag, strlen(tag));
    ninventory++;
}

int snapshot_blocks(size_t *bytes) {
    *bytes = 0;
    for (int i = 0; i < ninventory; i++)
        *bytes += inventory[i].len;
    return ninventory;
}

void snapshot_block(int i, const char **file, const char **name, void **p, size_t *len) {
    *file = inventory[i].file;
    *name = inventory[i].name;
    *p = inventory[i].p;
    *len = inventory[i].len;
}

/* The layout's hash: every block's tag and size, in order. */
static uint32_t layout_hash(void) {
    uint32_t words[2 * MAX_BLOCKS];
    for (int i = 0; i < ninventory; i++) {
        words[2 * i] = inventory[i].tag;
        words[2 * i + 1] = (uint32_t)inventory[i].len;
    }
    return fnv1a32(words, (size_t)ninventory * 2 * sizeof words[0]);
}

static _Noreturn void walk_fatal(const snapshot_walker *w, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static _Noreturn void walk_fatal(const snapshot_walker *w, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (w->block >= 0)
        fatal("snapshot: %s %s, %s: %s", inventory[w->block].file, inventory[w->block].name,
              w->loading ? "loading" : "saving", msg);
    fatal("snapshot: %s", msg);
}

/* p, an array of *cap items of size bytes, with room for need. */
static void *grow(void *p, size_t *cap, size_t need, size_t size) {
    if (need <= *cap)
        return p;
    size_t n = *cap ? *cap : 16;
    while (n < need)
        n *= 2;
    p = realloc(p, n * size);
    if (!p)
        fatal("out of memory");
    *cap = n;
    return p;
}

/* Saving: the next n bytes of the buffer, zeroed; loading: the next n
   bytes of the block. */
static uint8_t *advance(snapshot_walker *w, size_t n) {
    if (w->loading) {
        if (n > w->end - w->at)
            walk_fatal(w, "wants %zu bytes, and its block has %zu left", n, w->end - w->at);
    } else {
        size_t cap = w->cap;
        w->buf = grow(w->buf, &w->cap, w->at + n, 1);
        memset(w->buf + cap, 0, w->cap - cap);
        if (w->at + n > w->len)
            w->len = w->at + n;
    }
    uint8_t *p = w->buf + w->at;
    w->at += n;
    return p;
}

static void put32(snapshot_walker *w, uint32_t v) { memcpy(advance(w, 4), &v, 4); }

static uint32_t get32(snapshot_walker *w) {
    uint32_t v;
    memcpy(&v, advance(w, 4), 4);
    return v;
}

static void put64(snapshot_walker *w, uint64_t v) { memcpy(advance(w, 8), &v, 8); }

static uint64_t get64(snapshot_walker *w) {
    uint64_t v;
    memcpy(&v, advance(w, 8), 8);
    return v;
}

static void add_region(snapshot_walker *w, const void *live, size_t len, size_t at, size_t stash) {
    w->regions = grow(w->regions, &w->region_cap, w->nregions + 1, sizeof *w->regions);
    w->regions[w->nregions++] = (region){live, len, at, stash};
}

/* The region holding [field, field + len), the newest first. */
static const region *region_of(const snapshot_walker *w, const void *field, size_t len) {
    const uint8_t *f = field;
    for (size_t i = w->nregions; i-- > 0;) {
        const region *r = &w->regions[i];
        if (f >= r->live && f + len <= r->live + r->len)
            return r;
    }
    walk_fatal(w, "a field at %p (%zu bytes) is outside what it has walked", field, len);
}

bool snapshot_loading(const snapshot_walker *w) { return w->loading; }

void snapshot_check(snapshot_walker *w, bool ok, const char *fmt, ...) {
    if (!w->loading || ok)
        return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    walk_fatal(w, "%s", msg);
}

void snapshot_bytes(snapshot_walker *w, void *p, size_t len) {
    size_t at = w->at;
    uint8_t *b = advance(w, len);
    size_t stash = SIZE_MAX;
    if (w->loading) {
        stash = w->stash_len;
        w->stash = grow(w->stash, &w->stash_cap, w->stash_len + len, 1);
        memcpy(w->stash + w->stash_len, p, len);
        w->stash_len += len;
        memcpy(p, b, len);
    } else {
        memcpy(b, p, len);
    }
    add_region(w, p, len, at, stash);
}

/* Saving, zeroes field's bytes where the buffer holds them. Their region. */
static const region *blank(snapshot_walker *w, void *field, size_t len) {
    const region *r = region_of(w, field, len);
    if (!w->loading)
        memset(w->buf + r->at + ((const uint8_t *)field - r->live), 0, len);
    return r;
}

/* An owned pointer: its length (NO_OWNED for NULL), then what it points
   to, a region if more is walked inside it. Loading with len SIZE_MAX
   takes the length saved (a string's). */
static void owned(snapshot_walker *w, void *field, size_t len, bool region) {
    void *p;
    memcpy(&p, field, sizeof p);
    blank(w, field, sizeof p);
    if (!w->loading) {
        put64(w, p ? (uint64_t)len : NO_OWNED);
        if (p) {
            size_t at = w->at;
            memcpy(advance(w, len), p, len);
            if (region)
                add_region(w, p, len, at, SIZE_MAX);
        }
        return;
    }
    uint64_t saved = get64(w);
    p = NULL;
    if (saved != NO_OWNED) {
        if (len != SIZE_MAX && saved != len)
            walk_fatal(w, "owns %zu bytes at %p, and the snapshot %llu", len, field,
                       (unsigned long long)saved);
        if (saved > w->end - w->at)
            walk_fatal(w, "owns %llu bytes, and its block has %zu left", (unsigned long long)saved,
                       w->end - w->at);
        len = (size_t)saved;
        p = malloc(len ? len : 1);
        if (!p)
            fatal("out of memory");
        size_t at = w->at;
        memcpy(p, advance(w, len), len);
        if (region)
            add_region(w, p, len, at, SIZE_MAX);
    }
    memcpy(field, &p, sizeof p);
}

void snapshot_owned(snapshot_walker *w, void *field, size_t len) { owned(w, field, len, true); }

void snapshot_string(snapshot_walker *w, char **field) {
    owned(w, field, w->loading ? SIZE_MAX : *field ? strlen(*field) + 1 : 0, false);
}

void snapshot_host(snapshot_walker *w, void *field, size_t len) {
    const region *r = blank(w, field, len);
    if (!w->loading)
        return;
    if (r->stash == SIZE_MAX)
        walk_fatal(w, "keeps the host's bytes at %p in what loading made anew", field);
    memcpy(field, w->stash + r->stash + ((const uint8_t *)field - r->live), len);
}

void snapshot_rebuilt(snapshot_walker *w, void *field, size_t len) {
    blank(w, field, len);
    if (w->loading)
        memset(field, 0, len);
}

/* Adds an item to the list in what. */
static void add_what(char *what, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void add_what(char *what, size_t cap, const char *fmt, ...) {
    size_t n = strlen(what);
    if (n && n + 3 < cap) {
        memcpy(what + n, "; ", 3);
        n += 2;
    }
    va_list ap;
    va_start(ap, fmt);
    if (n + 1 < cap)
        vsnprintf(what + n, cap - n, fmt, ap);
    va_end(ap);
}

/* Verifying, at the end of block i's save: compares it with the
   reference's next block and says where it first differs. */
static void compare_block(snapshot_walker *w, int i) {
    const char *file = inventory[i].file, *name = inventory[i].name;
    if (w->ref_len - w->ref_at < BLOCK_HEADER) {
        add_what(w->what, w->what_cap, "%s %s: not in the snapshot", file, name);
        w->differs = true;
        w->ref_at = w->ref_len;
        return;
    }
    uint32_t tag, size;
    uint64_t len;
    memcpy(&tag, w->ref + w->ref_at, 4);
    memcpy(&size, w->ref + w->ref_at + 4, 4);
    memcpy(&len, w->ref + w->ref_at + 8, 8);
    const uint8_t *ref = w->ref + w->ref_at + BLOCK_HEADER;
    if (len > w->ref_len - w->ref_at - BLOCK_HEADER)
        len = w->ref_len - w->ref_at - BLOCK_HEADER;
    w->ref_at += BLOCK_HEADER + (size_t)len;
    if (tag != inventory[i].tag || size != inventory[i].len) {
        add_what(w->what, w->what_cap, "%s %s: the snapshot has another block there", file, name);
        w->differs = true;
        return;
    }
    size_t now = w->at - w->body, n = now < len ? now : (size_t)len, k = 0;
    while (k < n && ref[k] == w->buf[w->body + k])
        k++;
    if (k == n && now == len)
        return;
    w->differs = true;
    if (k == n) {
        add_what(w->what, w->what_cap, "%s %s: %zu bytes, not %llu", file, name, now,
                 (unsigned long long)len);
        return;
    }
    size_t at = w->body + k;
    for (size_t r = 0; r < w->nregions; r++) {
        const region *g = &w->regions[r];
        if (at < g->at || at >= g->at + g->len)
            continue;
        const uint8_t *p = g->live + (at - g->at), *var = inventory[i].p;
        if (p >= var && p < var + inventory[i].len)
            add_what(w->what, w->what_cap, "%s %s at byte %zu of %zu", file, name,
                     (size_t)(p - var), inventory[i].len);
        else
            add_what(w->what, w->what_cap, "%s %s, in what it owns, at byte %zu of %zu", file,
                     name, at - g->at, g->len);
        return;
    }
    add_what(w->what, w->what_cap, "%s %s at byte %zu of its block, in a length", file, name, k);
}

/* Walks every block into w (saving) or out of it (loading). */
static void walk_state(snapshot_walker *w) {
    w->block = -1;
    if (w->loading) {
        w->end = w->len < STATE_HEADER ? w->len : STATE_HEADER;
        if (w->len < STATE_HEADER)
            walk_fatal(w, "the module state is %zu bytes, too short for its header", w->len);
        uint32_t magic = get32(w), version = get32(w), layout = get32(w), n = get32(w);
        if (magic != STATE_MAGIC || version != STATE_VERSION)
            walk_fatal(w, "the module state isn't version %u's", STATE_VERSION);
        if (layout != layout_hash() || n != (uint32_t)ninventory)
            walk_fatal(w, "the module state's layout (%08x, %u blocks) isn't this build's (%08x, %d)",
                       layout, n, layout_hash(), ninventory);
    } else {
        put32(w, STATE_MAGIC);
        put32(w, STATE_VERSION);
        put32(w, layout_hash());
        put32(w, (uint32_t)ninventory);
        w->ref_at = STATE_HEADER;
        if (w->ref && (w->ref_len < STATE_HEADER || memcmp(w->ref, w->buf, STATE_HEADER) != 0)) {
            add_what(w->what, w->what_cap, "the module state's header");
            w->differs = true;
            return;
        }
    }
    for (int i = 0; i < ninventory; i++) {
        w->block = i;
        w->nregions = 0;
        w->stash_len = 0;
        size_t header = w->at;
        if (w->loading) {
            w->end = w->len;
            uint32_t tag = get32(w), size = get32(w);
            uint64_t len = get64(w);
            if (tag != inventory[i].tag)
                walk_fatal(w, "the snapshot's block %d has tag %08x, not this one's %08x", i, tag,
                           inventory[i].tag);
            if (size != inventory[i].len)
                walk_fatal(w, "the snapshot's is %u bytes, this build's %zu", size, inventory[i].len);
            if (len > w->len - w->at)
                walk_fatal(w, "its body is %llu bytes, past the end of the module state",
                           (unsigned long long)len);
            w->end = w->at + (size_t)len;
        } else {
            advance(w, BLOCK_HEADER);
        }
        w->body = w->at;
        if (inventory[i].walk)
            inventory[i].walk(w);
        else
            snapshot_bytes(w, inventory[i].p, inventory[i].len);
        if (w->loading) {
            if (w->at != w->end)
                walk_fatal(w, "took %zu bytes of its %zu", w->at - w->body, w->end - w->body);
            continue;
        }
        uint32_t tag = inventory[i].tag, size = (uint32_t)inventory[i].len;
        uint64_t len = w->at - w->body;
        memcpy(w->buf + header, &tag, 4);
        memcpy(w->buf + header + 4, &size, 4);
        memcpy(w->buf + header + 8, &len, 8);
        if (w->ref)
            compare_block(w, i);
    }
    w->block = -1;
    if (w->loading && w->at != w->len)
        walk_fatal(w, "%zu bytes past the module state's last block", w->len - w->at);
}

static void walker_free(snapshot_walker *w) {
    free(w->regions);
    free(w->stash);
}

uint8_t *snapshot_state_save(size_t *len) {
    snapshot_walker w = {0};
    walk_state(&w);
    walker_free(&w);
    *len = w.len;
    return w.buf;
}

void snapshot_state_load(const uint8_t *state, size_t len) {
    snapshot_walker w = {.loading = true, .buf = (uint8_t *)state, .len = len};
    walk_state(&w);
    walker_free(&w);
}

bool snapshot_state_verify(const uint8_t *state, size_t len, char *what, size_t cap) {
    if (cap)
        what[0] = '\0';
    snapshot_walker w = {.ref = state, .ref_len = len, .what = what, .what_cap = cap};
    walk_state(&w);
    if (!w.differs && w.ref_at != len) {
        add_what(what, cap, "the module state: %zu bytes past its last block", len - w.ref_at);
        w.differs = true;
    }
    walker_free(&w);
    free(w.buf);
    return !w.differs;
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
    s->state = snapshot_state_save(&s->state_len);
    return s;
}

void snapshot_free(snapshot *s) {
    if (!s)
        return;
    if (s->mem)
        mach_vm_deallocate(mach_task_self(), s->mem, gm_host_size());
    cpu_context_free(s->cpu);
    for (int d = 0; d < DEPTH; d++)
        cpu_context_free(s->level[d].saved);
    free(s->state);
    free(s);
}

bool snapshot_fits(const snapshot *s) {
    ev_frame f;
    uint32_t index, resume;
    return loop_frame(&f, &index, &resume) && same_frame(&f, &s->frame) &&
           index == s->level[0].index && resume == s->level[0].resume &&
           trap_call_target(1) == s->level[0].called && trap_call_target(2) == s->level[1].called;
}

/* The host pages of region r that differ between the guest mapping and
   s's copy, and the guest address where the first does. A page neither
   side holds (mincore: in memory, or compressed or swapped out) is zeros
   on both, and isn't read, since reading it would make it resident. */
static size_t differing_pages(const snapshot *s, const gm_region *r, uint32_t *first) {
    size_t page = vm_page_size;
    uintptr_t live = (uintptr_t)gm_host_base(), copy = (uintptr_t)s->mem;
    size_t begin = r->base / page * page, end = ((size_t)r->base + r->size + page - 1) / page * page;
    size_t npages = (end - begin) / page, differ = 0;
    char *in_live = malloc(npages), *in_copy = malloc(npages);
    if (!in_live || !in_copy)
        fatal("out of memory");
    if (mincore((void *)(live + begin), end - begin, in_live) != 0 ||
        mincore((void *)(copy + begin), end - begin, in_copy) != 0)
        memset(in_live, MINCORE_INCORE, npages); /* compare every page */
    for (size_t k = 0; k < npages; k++) {
        if (!((in_live[k] | in_copy[k]) & (MINCORE_INCORE | MINCORE_PAGED_OUT)))
            continue;
        size_t a = begin + k * page, b = a + page;
        if (a < r->base)
            a = r->base;
        if (b > (size_t)r->base + r->size)
            b = (size_t)r->base + r->size;
        if (memcmp((const void *)(live + a), (const void *)(copy + a), b - a) != 0 && !differ++)
            *first = (uint32_t)a;
    }
    free(in_live);
    free(in_copy);
    return differ;
}

bool snapshot_verify(const snapshot *s, char *what, size_t cap) {
    if (cap)
        what[0] = '\0';
    bool same = true;
    if (!snapshot_fits(s)) {
        add_what(what, cap, "the frame");
        same = false;
    }
    uint64_t now = replay_now_us();
    if (now != s->clock_us) {
        add_what(what, cap, "the replay's clock, at %llu us, not %llu", (unsigned long long)now,
                 (unsigned long long)s->clock_us);
        same = false;
    }
    const gm_region *regions;
    int nregions = gm_regions(&regions);
    for (int i = 0; i < nregions; i++) {
        uint32_t first;
        size_t pages = differing_pages(s, &regions[i], &first);
        if (pages) {
            add_what(what, cap, "guest memory at 0x%08x (%zu page%s)", first, pages,
                     pages == 1 ? "" : "s");
            same = false;
        }
    }
    char reg[96];
    cpu_context *cpu = cpu_save();
    if (!cpu_context_same(cpu, s->cpu, reg, sizeof reg)) {
        add_what(what, cap, "the CPU: %s", reg);
        same = false;
    }
    cpu_context_free(cpu);
    for (int d = 1; d <= DEPTH; d++)
        if (!cpu_context_same(trap_saved_context(d), s->level[d - 1].saved, reg, sizeof reg)) {
            add_what(what, cap, "the context depth %d saved: %s", d, reg);
            same = false;
        }
    char blocks[1024];
    if (!snapshot_state_verify(s->state, s->state_len, blocks, sizeof blocks)) {
        add_what(what, cap, "%s", blocks);
        same = false;
    }
    return same;
}

/* Puts back what isn't the CPU's: the guest mapping and the module blocks,
   then drops every translation and cached page. */
static void put_back_memory(const snapshot *s) {
    kern_return_t kr = vm_copy(mach_task_self(), (vm_address_t)s->mem, gm_host_size(),
                               (vm_address_t)(uintptr_t)gm_host_base());
    if (kr != KERN_SUCCESS)
        fatal("snapshot: can't copy the guest mapping back: %s", mach_error_string(kr));
    snapshot_state_load(s->state, s->state_len);
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

/* ---- on disk (#46) ---- */

#define CODEC_MAGIC 0x4C57736Bu /* 'LWsk' */
#define CODEC_VERSION 1u

typedef struct {
    uint8_t *p;
    size_t len, cap;
} out_buf;

static void out_add(out_buf *o, const void *p, size_t n) {
    o->p = grow(o->p, &o->cap, o->len + n, 1);
    memcpy(o->p + o->len, p, n);
    o->len += n;
}

static void out_le(out_buf *o, uint64_t v, int bytes) {
    uint8_t b[8];
    for (int i = 0; i < bytes; i++)
        b[i] = (uint8_t)(v >> (8 * i));
    out_add(o, b, (size_t)bytes);
}

static void out_regs(out_buf *o, const cpu_context *ctx) {
    uint64_t regs[CPU_REGS];
    cpu_context_regs(ctx, regs);
    for (int i = 0; i < CPU_REGS; i++)
        out_le(o, regs[i], 8);
}

/* Where page k of the guest mapping is, in s's copy. */
static uint8_t *page_of(const snapshot *s, size_t k) {
    return (uint8_t *)(uintptr_t)s->mem + k * vm_page_size;
}

uint8_t *snapshot_encode(const snapshot *s, const snapshot *base, const snapshot_range *same,
                         size_t nsame, size_t *len) {
    out_buf o = {0};
    out_le(&o, CODEC_MAGIC, 4);
    out_le(&o, CODEC_VERSION, 4);
    out_le(&o, vm_page_size, 4);
    out_le(&o, gm_host_size(), 8);
    out_le(&o, s->clock_us, 8);
    out_regs(&o, s->cpu);
    for (int d = 0; d < DEPTH; d++) {
        out_regs(&o, s->level[d].saved);
        out_le(&o, s->level[d].called, 4);
        out_le(&o, s->level[d].index, 4);
        out_le(&o, s->level[d].resume, 4);
    }
    const ev_frame *f = &s->frame;
    int32_t frame[8] = {f->timer, f->slot, (int32_t)f->chain[0], (int32_t)f->chain[1],
                        f->nchain, f->link, f->handler, f->loop_depth};
    for (int i = 0; i < 8; i++)
        out_le(&o, (uint32_t)frame[i], 4);
    out_le(&o, s->state_len, 8);
    out_add(&o, s->state, s->state_len);

    size_t page = vm_page_size, npages = gm_host_size() / page;
    if (gm_host_size() % page)
        fatal("snapshot: the guest mapping (%zu bytes) isn't whole pages", gm_host_size());
    char *in_s = malloc(npages), *in_base = malloc(npages);
    uint8_t *buf = malloc(page);
    if (!in_s || !in_base || !buf)
        fatal("out of memory");
    if (mincore((void *)(uintptr_t)s->mem, npages * page, in_s) != 0 ||
        mincore((void *)(uintptr_t)base->mem, npages * page, in_base) != 0)
        memset(in_s, MINCORE_INCORE, npages); /* every page, then */
    size_t count_at = o.len;
    uint32_t count = 0;
    out_le(&o, 0, 4);
    for (size_t k = 0; k < npages; k++) {
        if (!((in_s[k] | in_base[k]) & (MINCORE_INCORE | MINCORE_PAGED_OUT)))
            continue; /* zeros on both sides */
        const uint8_t *b = page_of(base, k);
        memcpy(buf, page_of(s, k), page);
        uint64_t lo = (uint64_t)k * page, hi = lo + page;
        for (size_t r = 0; r < nsame; r++) {
            uint64_t a = same[r].addr, e = a + same[r].len;
            if (e <= lo || a >= hi)
                continue;
            if (a < lo)
                a = lo;
            if (e > hi)
                e = hi;
            memcpy(buf + (a - lo), b + (a - lo), (size_t)(e - a));
        }
        if (memcmp(buf, b, page) == 0)
            continue;
        for (size_t i = 0; i < page; i++)
            buf[i] ^= b[i];
        out_le(&o, k, 4);
        out_add(&o, buf, page);
        count++;
    }
    for (int i = 0; i < 4; i++)
        o.p[count_at + (size_t)i] = (uint8_t)(count >> (8 * i));
    free(in_s);
    free(in_base);
    free(buf);
    *len = o.len;
    return o.p;
}

bool snapshot_changed_holds(const snapshot *s, const snapshot *base, const char *text) {
    size_t n = strlen(text), page = vm_page_size, size = gm_host_size(), npages = size / page;
    if (!n || n > 255)
        return false;
    char forms[3][256];
    for (size_t i = 0; i <= n; i++) {
        forms[0][i] = text[i];
        forms[1][i] = (char)toupper((unsigned char)text[i]);
        forms[2][i] = (char)tolower((unsigned char)text[i]);
    }
    char *in_s = malloc(npages), *in_base = malloc(npages);
    if (!in_s || !in_base)
        fatal("out of memory");
    if (mincore((void *)(uintptr_t)s->mem, npages * page, in_s) != 0 ||
        mincore((void *)(uintptr_t)base->mem, npages * page, in_base) != 0)
        memset(in_s, MINCORE_INCORE, npages);
    const uint8_t *m = (const uint8_t *)(uintptr_t)s->mem, *b = (const uint8_t *)(uintptr_t)base->mem;
    bool found = false;
    for (size_t k = 0; k < npages && !found; k++) {
        if (!((in_s[k] | in_base[k]) & (MINCORE_INCORE | MINCORE_PAGED_OUT)) ||
            memcmp(m + k * page, b + k * page, page) == 0)
            continue;
        /* An occurrence starting in this page, and a byte of it that changed. */
        size_t from = k * page, to = from + page + n - 1 < size ? from + page + n - 1 : size;
        for (int f = 0; f < 3 && !found; f++) {
            const uint8_t *at = m + from, *end = m + to;
            while (!found && at < end && (at = memmem(at, (size_t)(end - at), forms[f], n))) {
                if ((size_t)(at - m) >= from + page)
                    break;
                found = memcmp(at, b + (at - m), n) != 0;
                at++;
            }
        }
    }
    free(in_s);
    free(in_base);
    return found;
}

/* A place in an encoded snapshot. */
typedef struct {
    const uint8_t *p;
    size_t len, at;
    bool bad;
} in_buf;

static uint64_t in_le(in_buf *in, int bytes) {
    if (in->bad || (size_t)bytes > in->len - in->at) {
        in->bad = true;
        return 0;
    }
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++)
        v |= (uint64_t)in->p[in->at + (size_t)i] << (8 * i);
    in->at += (size_t)bytes;
    return v;
}

static const uint8_t *in_bytes(in_buf *in, size_t n) {
    if (in->bad || n > in->len - in->at) {
        in->bad = true;
        return NULL;
    }
    const uint8_t *p = in->p + in->at;
    in->at += n;
    return p;
}

static cpu_context *in_regs(in_buf *in) {
    uint64_t regs[CPU_REGS];
    for (int i = 0; i < CPU_REGS; i++)
        regs[i] = in_le(in, 8);
    return in->bad ? NULL : cpu_context_with_regs(regs);
}

snapshot *snapshot_decode(const uint8_t *data, size_t len, char *err, size_t errlen) {
    in_buf in = {data, len, 0, false};
    if (in_le(&in, 4) != CODEC_MAGIC || in_le(&in, 4) != CODEC_VERSION) {
        snprintf(err, errlen, "it isn't a snapshot this version reads");
        return NULL;
    }
    uint64_t page = in_le(&in, 4), size = in_le(&in, 8);
    if (!in.bad && (page != vm_page_size || size != gm_host_size())) {
        snprintf(err, errlen, "its guest mapping is %llu bytes in pages of %llu, not %zu in %zu",
                 (unsigned long long)size, (unsigned long long)page, gm_host_size(),
                 (size_t)vm_page_size);
        return NULL;
    }
    snapshot *s = calloc(1, sizeof *s);
    if (!s)
        fatal("out of memory");
    s->clock_us = in_le(&in, 8);
    s->cpu = in_regs(&in);
    for (int d = 0; d < DEPTH; d++) {
        s->level[d].saved = in_regs(&in);
        s->level[d].called = (uint32_t)in_le(&in, 4);
        s->level[d].index = (uint32_t)in_le(&in, 4);
        s->level[d].resume = (uint32_t)in_le(&in, 4);
    }
    int32_t frame[8];
    for (int i = 0; i < 8; i++)
        frame[i] = (int32_t)(uint32_t)in_le(&in, 4);
    s->frame = (ev_frame){.timer = frame[0], .slot = frame[1], .chain = {(uint32_t)frame[2],
                          (uint32_t)frame[3]}, .nchain = frame[4], .link = frame[5],
                          .handler = frame[6], .loop_depth = frame[7]};
    uint64_t state_len = in_le(&in, 8);
    const uint8_t *state = in_bytes(&in, (size_t)state_len);
    if (state) {
        if (!(s->state = malloc(state_len ? (size_t)state_len : 1)))
            fatal("out of memory");
        memcpy(s->state, state, (size_t)state_len);
        s->state_len = (size_t)state_len;
    }
    uint64_t count = in_le(&in, 4);
    const ev_frame *f = &s->frame;
    bool frame_ok = f->timer >= 0 ? f->timer < EV_MAX_TIMERS
                                  : f->timer == -1 && f->slot >= 0 && f->slot < EV_MAX_EVENTS &&
                                        f->nchain >= 1 && f->nchain <= 2 && f->link >= 0 &&
                                        f->link < f->nchain && f->handler >= 0 &&
                                        f->handler < EV_MAX_HANDLERS;
    if (in.bad || !frame_ok || f->loop_depth < 1) {
        snprintf(err, errlen, "it is damaged");
        snapshot_free(s);
        return NULL;
    }
    /* The guest mapping as it is (the baseline's), then the pages that
       differ from it. */
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &s->mem, gm_host_size(), VM_FLAGS_ANYWHERE);
    if (kr == KERN_SUCCESS)
        kr = vm_copy(mach_task_self(), (vm_address_t)(uintptr_t)gm_host_base(), gm_host_size(),
                     (vm_address_t)s->mem);
    if (kr != KERN_SUCCESS)
        fatal("snapshot: can't copy the guest mapping: %s", mach_error_string(kr));
    size_t npages = gm_host_size() / vm_page_size;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t k = in_le(&in, 4);
        const uint8_t *x = in_bytes(&in, vm_page_size);
        if (!x || k >= npages) {
            snprintf(err, errlen, "its page %llu is damaged", (unsigned long long)i);
            snapshot_free(s);
            return NULL;
        }
        uint8_t *p = page_of(s, (size_t)k);
        for (size_t b = 0; b < vm_page_size; b++)
            p[b] ^= x[b];
    }
    if (in.at != in.len) {
        snprintf(err, errlen, "it has %zu bytes past its last page", in.len - in.at);
        snapshot_free(s);
        return NULL;
    }
    return s;
}

/* Whether a is somewhere code may run from, or return to (snapshot_brings_no_code). */
static bool code_address(uint32_t a, uint32_t code, uint32_t code_len) {
    return !a || a == GUEST_RETURN_MAGIC || (a >= code && a - code < code_len) ||
           (a >= GUEST_TRAP_BASE && a < GUEST_TRAP_ADDR(trap_count()) && a % 4 == 0);
}

/* The same for the registers of ctx that hold code addresses, named
   after who, in why. */
static bool code_registers(const cpu_context *ctx, const char *who, uint32_t code, uint32_t code_len,
                           char *why, size_t cap) {
    static const char *const names[] = {"pc", "lr", "ctr"};
    uint64_t regs[CPU_REGS];
    cpu_context_regs(ctx, regs);
    for (int i = 0; i < 3; i++)
        if (!code_address((uint32_t)regs[64 + i], code, code_len)) {
            snprintf(why, cap, "%s's %s is 0x%08x, outside the game's code", who, names[i],
                     (uint32_t)regs[64 + i]);
            return false;
        }
    return true;
}

bool snapshot_brings_no_code(const snapshot *s, char *why, size_t cap) {
    uint32_t code, code_len;
    trap_code(&code, &code_len);
    if (!code_len || code > gm_host_size() || code_len > gm_host_size() - code) {
        snprintf(why, cap, "the game's code isn't where a snapshot can be checked");
        return false;
    }
    const uint8_t *mine = (const uint8_t *)(uintptr_t)s->mem + code, *live = gm_host_base() + code;
    for (uint32_t i = 0; i < code_len; i++)
        if (mine[i] != live[i]) {
            snprintf(why, cap, "it changes the game's code, at 0x%08x", code + i);
            return false;
        }
    if (!code_registers(s->cpu, "the CPU", code, code_len, why, cap))
        return false;
    for (int d = 0; d < DEPTH; d++) {
        char who[32];
        snprintf(who, sizeof who, "depth %d", d + 1);
        if (!code_registers(s->level[d].saved, who, code, code_len, why, cap))
            return false;
        if (!code_address(s->level[d].called, code, code_len) ||
            !code_address(s->level[d].resume, code, code_len)) {
            snprintf(why, cap, "depth %d calls 0x%08x and returns to 0x%08x, outside the game's code",
                     d + 1, s->level[d].called, s->level[d].resume);
            return false;
        }
        if (s->level[d].index >= trap_count()) {
            snprintf(why, cap, "depth %d is in trap %u, of %u", d + 1, s->level[d].index, trap_count());
            return false;
        }
    }
    if (strcmp(trap_import_name(s->level[0].index), "RunApplicationEventLoop") != 0) {
        snprintf(why, cap, "depth 1 is in %s, not the run loop", trap_import_name(s->level[0].index));
        return false;
    }
    uint64_t regs[CPU_REGS], now[CPU_REGS];
    cpu_context *cpu = cpu_save();
    cpu_context_regs(cpu, now);
    cpu_context_free(cpu);
    cpu_context_regs(s->cpu, regs);
    if (regs[69] != now[69]) {
        snprintf(why, cap, "its machine state register is 0x%08x, not 0x%08x", (uint32_t)regs[69],
                 (uint32_t)now[69]);
        return false;
    }
    return true;
}

void snapshot_poke(snapshot *s, uint32_t addr, const void *p, size_t len) {
    if (addr > gm_host_size() || len > gm_host_size() - addr)
        fatal("snapshot: writing %zu bytes at 0x%08x, outside the guest mapping", len, addr);
    memcpy((uint8_t *)(uintptr_t)s->mem + addr, p, len);
}
