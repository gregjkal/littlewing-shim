#include "memmgr.h"

#include <string.h>

#include "guest_mem.h"
#include "snapshot.h"
#include "trap.h"
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

#define HEAP_BASE gm_heap_base()
#define HEAP_SIZE gm_heap_size()
#define HEAP_END (HEAP_BASE + HEAP_SIZE)
#define MIN_SPLIT 32u /* header + 16 bytes */

static int16_t last_error;
SNAPSHOT_STATE(last_error)

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

/* Crashes unless the header of the block at p is one the allocator wrote:
   a known magic and a capacity that is a multiple of 16 and ends inside the
   heap. Headers live in guest memory, so a guest writing past the end of a
   block can clobber the next one. */
static void check_block(uint32_t p) {
    uint32_t m = magic(p), cap = capacity(p);
    bool known = m == MAGIC_FREE || m == MAGIC_PTR || m == MAGIC_DATA || m == MAGIC_MASTER;
    if (!known || (cap & 15u) != 0 || cap > HEAP_END - p)
        trap_crash("heap block header at 0x%08x is corrupt (did the game write past a block?)",
                   hdr(p));
}

static uint32_t next_block(uint32_t p) {
    check_block(p);
    return p + capacity(p) + MM_HEADER_SIZE;
}

/* True if p is the payload address of a block whose header has magic m. */
static bool is_block(uint32_t p, uint32_t m) {
    if (p < HEAP_BASE + MM_HEADER_SIZE || p >= HEAP_END || (p & 15u) != 0)
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
        check_block(n);
        gm_w32(hdr(p) + 4, capacity(p) + MM_HEADER_SIZE + capacity(n));
    }
}

void mm_init(void) {
    uint32_t first = HEAP_BASE + MM_HEADER_SIZE;
    set_header(first, MAGIC_FREE, HEAP_SIZE - MM_HEADER_SIZE, 0, 0);
    last_error = MM_NO_ERR;
}

static uint32_t alloc_block(uint32_t size, uint32_t m, uint32_t own, bool clear) {
    if (size > HEAP_SIZE)
        return 0;
    uint32_t need = (size + 15u) & ~15u;
    if (need == 0)
        need = 16;
    for (uint32_t p = HEAP_BASE + MM_HEADER_SIZE; p < HEAP_END; p = next_block(p)) {
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

int16_t mm_set_handle_size(uint32_t h, uint32_t size) {
    if (!mm_is_handle(h))
        return MM_MEM_WZ_ERR;
    uint32_t d = gm_r32(h);
    if (size <= capacity(d)) {
        set_header(d, MAGIC_DATA, capacity(d), size, h);
        return MM_NO_ERR;
    }
    uint32_t n = alloc_block(size, MAGIC_DATA, h, false);
    if (!n)
        return MM_MEM_FULL_ERR;
    uint32_t keep = logical(d);
    if (keep)
        memcpy(gm_ptr(n, keep), gm_ptr(d, keep), keep);
    free_block(d);
    gm_w32(h, n);
    return MM_NO_ERR;
}

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
    for (uint32_t p = HEAP_BASE + MM_HEADER_SIZE; p < HEAP_END; p = next_block(p)) {
        if (magic(p) == MAGIC_FREE) {
            coalesce(p);
            total += capacity(p);
        }
    }
    return total;
}

uint32_t mm_largest_free(void) {
    uint32_t best = 0;
    for (uint32_t p = HEAP_BASE + MM_HEADER_SIZE; p < HEAP_END; p = next_block(p)) {
        if (magic(p) == MAGIC_FREE) {
            coalesce(p);
            if (capacity(p) > best)
                best = capacity(p);
        }
    }
    return best;
}

int16_t mm_error(void) { return last_error; }
void mm_set_error(int16_t err) { last_error = err; }

/* ---- guest calls ---- */

static void ret_err(int16_t err) {
    last_error = err;
    trap_return((uint32_t)(int32_t)err);
}

static void need_ptr(const char *call, uint32_t p) {
    if (!mm_is_ptr(p))
        trap_crash("%s: 0x%08x is not an allocated pointer", call, p);
}

/* Returns false (MemError = nilHandleErr) for a NULL handle; crashes on a
   non-NULL value that isn't a handle. */
static bool need_handle(const char *call, uint32_t h) {
    if (h == 0) {
        last_error = MM_NIL_HANDLE_ERR;
        return false;
    }
    if (!mm_is_handle(h))
        trap_crash("%s: 0x%08x is not a handle", call, h);
    return true;
}

static void h_new_ptr(void) {
    uint32_t p = mm_new_ptr(trap_arg(0), false);
    last_error = p ? MM_NO_ERR : MM_MEM_FULL_ERR;
    trap_return(p);
}

static void h_new_ptr_clear(void) {
    uint32_t p = mm_new_ptr(trap_arg(0), true);
    last_error = p ? MM_NO_ERR : MM_MEM_FULL_ERR;
    trap_return(p);
}

static void h_dispose_ptr(void) {
    uint32_t p = trap_arg(0);
    if (p) {
        need_ptr("DisposePtr", p);
        mm_dispose_ptr(p);
    }
    last_error = MM_NO_ERR;
}

static void h_get_ptr_size(void) {
    uint32_t p = trap_arg(0);
    need_ptr("GetPtrSize", p);
    last_error = MM_NO_ERR;
    trap_return(mm_ptr_size(p));
}

static void h_set_ptr_size(void) {
    uint32_t p = trap_arg(0);
    need_ptr("SetPtrSize", p);
    last_error = mm_set_ptr_size(p, trap_arg(1));
}

static void h_new_handle(void) {
    uint32_t h = mm_new_handle(trap_arg(0), false);
    last_error = h ? MM_NO_ERR : MM_MEM_FULL_ERR;
    trap_return(h);
}

/* ReallocateHandle(Handle, Size): new contents of that size (the old ones
   are lost on a Mac; here they're kept, which no caller can tell apart). */
static void h_reallocate_handle(void) {
    uint32_t h = trap_arg(0);
    if (!need_handle("ReallocateHandle", h))
        return;
    last_error = mm_set_handle_size(h, trap_arg(1));
}

static void h_new_handle_clear(void) {
    uint32_t h = mm_new_handle(trap_arg(0), true);
    last_error = h ? MM_NO_ERR : MM_MEM_FULL_ERR;
    trap_return(h);
}

static void h_dispose_handle(void) {
    uint32_t h = trap_arg(0);
    if (!need_handle("DisposeHandle", h))
        return;
    if (mm_handle_state(h) & MM_STATE_RESOURCE)
        trap_crash("DisposeHandle: 0x%08x is a resource handle (use ReleaseResource)", h);
    mm_dispose_handle(h);
    last_error = MM_NO_ERR;
}

static void h_get_handle_size(void) {
    uint32_t h = trap_arg(0);
    if (!need_handle("GetHandleSize", h)) {
        trap_return(0);
        return;
    }
    last_error = MM_NO_ERR;
    trap_return(mm_handle_size(h));
}

static void h_recover_handle(void) {
    uint32_t p = trap_arg(0);
    uint32_t h = mm_recover_handle(p);
    if (!h)
        trap_crash("RecoverHandle: 0x%08x is not the start of a handle's block", p);
    last_error = MM_NO_ERR;
    trap_return(h);
}

static void change_state(const char *call, uint8_t set, uint8_t clear) {
    uint32_t h = trap_arg(0);
    if (!need_handle(call, h))
        return;
    mm_set_handle_state(h, (uint8_t)((mm_handle_state(h) | set) & ~clear));
    last_error = MM_NO_ERR;
}

static void h_hlock(void) { change_state("HLock", MM_STATE_LOCKED, 0); }
static void h_hunlock(void) { change_state("HUnlock", 0, MM_STATE_LOCKED); }
static void h_hpurge(void) { change_state("HPurge", MM_STATE_PURGEABLE, 0); }
static void h_hnopurge(void) { change_state("HNoPurge", 0, MM_STATE_PURGEABLE); }
static void h_move_hhi(void) { change_state("MoveHHi", 0, 0); }

static void h_hget_state(void) {
    uint32_t h = trap_arg(0);
    if (!need_handle("HGetState", h)) {
        trap_return(0);
        return;
    }
    last_error = MM_NO_ERR;
    trap_return((uint32_t)(int32_t)(int8_t)mm_handle_state(h));
}

static void h_hset_state(void) {
    uint32_t h = trap_arg(0);
    if (!need_handle("HSetState", h))
        return;
    mm_set_handle_state(h, (uint8_t)trap_arg(1));
    last_error = MM_NO_ERR;
}

static void h_mem_error(void) { ret_err(last_error); }

static void h_set_handle_size(void) {
    uint32_t h = trap_arg(0);
    if (!need_handle("SetHandleSize", h))
        return;
    last_error = mm_set_handle_size(h, trap_arg(1));
}

/* EmptyHandle: on a Mac the data is freed and the master pointer is NULL.
   Handles here are never empty, so the data shrinks to nothing instead;
   GetHandleSize says 0 either way. */
static void h_empty_handle(void) {
    uint32_t h = trap_arg(0);
    if (!need_handle("EmptyHandle", h))
        return;
    last_error = mm_set_handle_size(h, 0);
}

static void h_hlock_hi(void) { change_state("HLockHi", MM_STATE_LOCKED, 0); }

/* TempNewHandle(size, &err): temporary memory is the same heap. */
static void h_temp_new_handle(void) {
    uint32_t h = mm_new_handle(trap_arg(0), false);
    last_error = h ? MM_NO_ERR : MM_MEM_FULL_ERR;
    if (trap_arg(1))
        gm_w16(trap_arg(1), (uint16_t)last_error);
    trap_return(h);
}

static void h_max_block(void) {
    last_error = MM_NO_ERR;
    trap_return(mm_largest_free());
}

static void h_free_mem(void) {
    last_error = MM_NO_ERR;
    trap_return(mm_free_bytes());
}

/* Master pointers come with each handle, and the heap never asks the game
   to free memory, so its grow zone procedure is kept but never called;
   GZSaveHnd only means something inside one. */
static void h_more_master_pointers(void) { last_error = MM_NO_ERR; }
static void h_new_grow_zone_upp(void) { trap_return(trap_arg(0)); }
static void h_dispose_grow_zone_upp(void) {}
static void h_set_grow_zone(void) {}
static void h_gz_save_hnd(void) { trap_return(0); }
/* ReserveMem(Size): the heap has room enough, and nothing moves. */
static void h_reserve_mem(void) { last_error = MM_NO_ERR; }

/* A classic program's start: MoreMasters (master pointers come with each
   handle), and the heap's limit, which is the end of the guest heap and
   already all the heap there is, so SetApplLimit and MaxApplZone change
   nothing. */
static void h_more_masters(void) { last_error = MM_NO_ERR; }

/* HandToHand(Handle *): *h becomes a new handle holding a copy of its data. */
static void h_hand_to_hand(void) {
    uint32_t hp = trap_arg(0), h = gm_r32(hp);
    if (!need_handle("HandToHand", h)) {
        trap_return((uint32_t)(int32_t)last_error);
        return;
    }
    uint32_t n = mm_handle_size(h), copy = mm_new_handle(n, false);
    last_error = copy ? MM_NO_ERR : MM_MEM_FULL_ERR;
    if (copy) {
        memcpy(gm_ptr(gm_r32(copy), n ? n : 1), gm_ptr(gm_r32(h), n ? n : 1), n);
        gm_w32(hp, copy);
    }
    trap_return((uint32_t)(int32_t)last_error);
}
static void h_get_appl_limit(void) { trap_return(HEAP_BASE + HEAP_SIZE); }
static void h_set_appl_limit(void) { last_error = MM_NO_ERR; }
static void h_max_appl_zone(void) { last_error = MM_NO_ERR; }

void mm_register(void) {
    trap_register("NewPtr", h_new_ptr);
    trap_register("NewPtrClear", h_new_ptr_clear);
    trap_register("DisposePtr", h_dispose_ptr);
    trap_register("GetPtrSize", h_get_ptr_size);
    trap_register("SetPtrSize", h_set_ptr_size);
    trap_register("NewHandle", h_new_handle);
    trap_register("ReallocateHandle", h_reallocate_handle);
    trap_register("NewHandleClear", h_new_handle_clear);
    trap_register("DisposeHandle", h_dispose_handle);
    trap_register("GetHandleSize", h_get_handle_size);
    trap_register("RecoverHandle", h_recover_handle);
    trap_register("HLock", h_hlock);
    trap_register("HUnlock", h_hunlock);
    trap_register("HPurge", h_hpurge);
    trap_register("HNoPurge", h_hnopurge);
    trap_register("MoveHHi", h_move_hhi);
    trap_register("HGetState", h_hget_state);
    trap_register("HSetState", h_hset_state);
    trap_register("MemError", h_mem_error);
    trap_register("SetHandleSize", h_set_handle_size);
    trap_register("EmptyHandle", h_empty_handle);
    trap_register("HLockHi", h_hlock_hi);
    trap_register("TempNewHandle", h_temp_new_handle);
    trap_register("MaxBlock", h_max_block);
    trap_register("FreeMem", h_free_mem);
    trap_register("MoreMasterPointers", h_more_master_pointers);
    trap_register("NewGrowZoneUPP", h_new_grow_zone_upp);
    trap_register("DisposeGrowZoneUPP", h_dispose_grow_zone_upp);
    trap_register("SetGrowZone", h_set_grow_zone);
    trap_register("GZSaveHnd", h_gz_save_hnd);
    trap_register("ReserveMem", h_reserve_mem);
    trap_register("MoreMasters", h_more_masters);
    trap_register("HandToHand", h_hand_to_hand);
    trap_register("GetApplLimit", h_get_appl_limit);
    trap_register("SetApplLimit", h_set_appl_limit);
    trap_register("MaxApplZone", h_max_appl_zone);
}
