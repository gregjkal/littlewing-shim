#include "memmgr.h"

#include <string.h>

#include "guest_mem.h"
#include "util.h"

/* Block header, 16 bytes, big-endian:
     +0  magic
     +4  capacity (payload bytes, a multiple of 16)
     +8  logical size (what the caller asked for)
     +12 owner: for a handle's data block, the handle (master pointer address);
         for a master pointer block, the handle state byte; otherwise 0 */
#define MAGIC_FREE   0x46524545u /* 'FREE' */
#define MAGIC_PTR    0x5054524Bu /* 'PTRK' */
#define MAGIC_DATA   0x44415441u /* 'DATA': a handle's data block */
#define MAGIC_MASTER 0x4D415354u /* 'MAST': a master pointer block */

#define HEAP_END (GUEST_HEAP_BASE + GUEST_HEAP_SIZE)
#define MIN_SPLIT 32u /* header + 16 bytes */

static int16_t last_error;

static uint32_t hdr(uint32_t p) { return p - MM_HEADER_SIZE; }
static uint32_t magic(uint32_t p) { return gm_r32(hdr(p)); }
static uint32_t capacity(uint32_t p) { return gm_r32(hdr(p) + 4); }
static uint32_t logical(uint32_t p) { return gm_r32(hdr(p) + 8); }
static uint32_t owner(uint32_t p) { return gm_r32(hdr(p) + 12); }

static void set_header(uint32_t p, uint32_t m, uint32_t cap, uint32_t size, uint32_t own) {
    uint32_t h = hdr(p);
    gm_w32(h, m);
    gm_w32(h + 4, cap);
    gm_w32(h + 8, size);
    gm_w32(h + 12, own);
}

static uint32_t next_block(uint32_t p) {
    return p + capacity(p) + MM_HEADER_SIZE;
}

/* True if p is the payload address of a block whose header has magic m. */
static bool is_block(uint32_t p, uint32_t m) {
    if (p < GUEST_HEAP_BASE + MM_HEADER_SIZE || p >= HEAP_END || (p & 15u) != 0)
        return false;
    return magic(p) == m;
}

static void fill_free(uint32_t p, uint32_t cap) {
    uint8_t *b = gm_ptr(p, cap);
    for (uint32_t i = 0; i + 4 <= cap; i += 4)
        wr_be32(b + i, 0xDEADBEEFu);
}

/* Merges the free block at p with any free blocks that follow it. */
static void coalesce(uint32_t p) {
    for (;;) {
        uint32_t n = next_block(p);
        if (n >= HEAP_END || magic(n) != MAGIC_FREE)
            return;
        gm_w32(hdr(p) + 4, capacity(p) + MM_HEADER_SIZE + capacity(n));
    }
}

void mm_init(void) {
    uint32_t first = GUEST_HEAP_BASE + MM_HEADER_SIZE;
    set_header(first, MAGIC_FREE, GUEST_HEAP_SIZE - MM_HEADER_SIZE, 0, 0);
    last_error = MM_NO_ERR;
}

static uint32_t alloc_block(uint32_t size, uint32_t m, uint32_t own, bool clear) {
    if (size > GUEST_HEAP_SIZE)
        return 0;
    uint32_t need = (size + 15u) & ~15u;
    if (need == 0)
        need = 16;
    for (uint32_t p = GUEST_HEAP_BASE + MM_HEADER_SIZE; p < HEAP_END; p = next_block(p)) {
        if (magic(p) != MAGIC_FREE)
            continue;
        coalesce(p);
        uint32_t cap = capacity(p);
        if (cap < need)
            continue;
        if (cap - need >= MIN_SPLIT) {
            uint32_t rest = p + need + MM_HEADER_SIZE;
            set_header(rest, MAGIC_FREE, cap - need - MM_HEADER_SIZE, 0, 0);
            cap = need;
        }
        set_header(p, m, cap, size, own);
        if (clear)
            memset(gm_ptr(p, cap), 0, cap);
        return p;
    }
    return 0;
}

static void free_block(uint32_t p) {
    uint32_t cap = capacity(p);
    set_header(p, MAGIC_FREE, cap, 0, 0);
    fill_free(p, cap);
    coalesce(p);
}

uint32_t mm_new_ptr(uint32_t size, bool clear) {
    return alloc_block(size, MAGIC_PTR, 0, clear);
}

bool mm_is_ptr(uint32_t p) { return is_block(p, MAGIC_PTR); }

int16_t mm_dispose_ptr(uint32_t p) {
    if (!mm_is_ptr(p))
        return MM_MEM_WZ_ERR;
    free_block(p);
    return MM_NO_ERR;
}

uint32_t mm_ptr_size(uint32_t p) { return logical(p); }

int16_t mm_set_ptr_size(uint32_t p, uint32_t size) {
    if (!mm_is_ptr(p))
        return MM_MEM_WZ_ERR;
    uint32_t need = (size + 15u) & ~15u;
    if (need == 0)
        need = 16;
    uint32_t cap = capacity(p);
    if (need > cap) {
        uint32_t n = next_block(p);
        if (n >= HEAP_END || magic(n) != MAGIC_FREE)
            return MM_MEM_FULL_ERR;
        coalesce(n);
        uint32_t avail = cap + MM_HEADER_SIZE + capacity(n);
        if (avail < need)
            return MM_MEM_FULL_ERR;
        cap = avail;
    }
    if (cap - need >= MIN_SPLIT) {
        uint32_t rest = p + need + MM_HEADER_SIZE;
        uint32_t rest_cap = cap - need - MM_HEADER_SIZE;
        set_header(rest, MAGIC_FREE, rest_cap, 0, 0);
        fill_free(rest, rest_cap);
        coalesce(rest);
        cap = need;
    }
    set_header(p, MAGIC_PTR, cap, size, 0);
    return MM_NO_ERR;
}

uint32_t mm_new_handle(uint32_t size, bool clear) {
    uint32_t h = alloc_block(4, MAGIC_MASTER, 0, true);
    if (!h)
        return 0;
    uint32_t d = alloc_block(size, MAGIC_DATA, h, clear);
    if (!d) {
        free_block(h);
        return 0;
    }
    gm_w32(h, d);
    return h;
}

bool mm_is_handle(uint32_t h) {
    if (!is_block(h, MAGIC_MASTER))
        return false;
    uint32_t d = gm_r32(h);
    return is_block(d, MAGIC_DATA) && owner(d) == h;
}

int16_t mm_dispose_handle(uint32_t h) {
    if (!mm_is_handle(h))
        return MM_MEM_WZ_ERR;
    free_block(gm_r32(h));
    free_block(h);
    return MM_NO_ERR;
}

uint32_t mm_handle_size(uint32_t h) { return logical(gm_r32(h)); }

uint32_t mm_recover_handle(uint32_t p) {
    if (!is_block(p, MAGIC_DATA))
        return 0;
    uint32_t h = owner(p);
    return mm_is_handle(h) && gm_r32(h) == p ? h : 0;
}

uint8_t mm_handle_state(uint32_t h) { return (uint8_t)owner(h); }

void mm_set_handle_state(uint32_t h, uint8_t state) { gm_w32(hdr(h) + 12, state); }

uint32_t mm_free_bytes(void) {
    uint32_t total = 0;
    for (uint32_t p = GUEST_HEAP_BASE + MM_HEADER_SIZE; p < HEAP_END; p = next_block(p)) {
        if (magic(p) == MAGIC_FREE) {
            coalesce(p);
            total += capacity(p);
        }
    }
    return total;
}

int16_t mm_error(void) { return last_error; }
void mm_set_error(int16_t err) { last_error = err; }

