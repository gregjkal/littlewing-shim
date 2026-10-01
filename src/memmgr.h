#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Memory Manager: Ptrs and Handles in the guest heap (GUEST_HEAP_BASE, 64 MB).
   Every block has a 16-byte big-endian header in guest memory. A handle is the
   address of a master pointer block, whose 4-byte payload points at the data
   block. Handles never move and are never purged. */

#define MM_NO_ERR         0
#define MM_MEM_FULL_ERR   (-108)
#define MM_NIL_HANDLE_ERR (-109)
#define MM_MEM_WZ_ERR     (-111)

/* Handle state bits, as returned by HGetState. */
#define MM_STATE_LOCKED    0x80
#define MM_STATE_PURGEABLE 0x40
#define MM_STATE_RESOURCE  0x20

#define MM_HEADER_SIZE 16u

/* Resets the heap to one free block. Requires gm_init(). */
void mm_init(void);

/* Returns a 16-byte-aligned block of size bytes, or 0 if the heap is full.
   clear zeroes the block; otherwise its contents are unspecified. */
uint32_t mm_new_ptr(uint32_t size, bool clear);
/* MM_MEM_WZ_ERR if p is not an allocated pointer block. */
int16_t mm_dispose_ptr(uint32_t p);
bool mm_is_ptr(uint32_t p);
/* Requires mm_is_ptr(p). */
uint32_t mm_ptr_size(uint32_t p);
/* Resizes in place. MM_MEM_FULL_ERR if the block can't grow in place,
   MM_MEM_WZ_ERR if p is not a pointer block. */
int16_t mm_set_ptr_size(uint32_t p, uint32_t size);

/* Returns a handle to size bytes, or 0 if the heap is full. */
uint32_t mm_new_handle(uint32_t size, bool clear);
/* MM_MEM_WZ_ERR if h is not a handle. */
int16_t mm_dispose_handle(uint32_t h);
bool mm_is_handle(uint32_t h);
/* Requires mm_is_handle(h). */
uint32_t mm_handle_size(uint32_t h);
/* The handle whose data block starts at p, or 0 if there is none. */
uint32_t mm_recover_handle(uint32_t p);
/* Requires mm_is_handle(h). */
uint8_t mm_handle_state(uint32_t h);
void mm_set_handle_state(uint32_t h, uint8_t state);

/* Total payload bytes in free blocks. */
uint32_t mm_free_bytes(void);

/* MemError: the result of the last guest Memory Manager call. */
int16_t mm_error(void);
void mm_set_error(int16_t err);

/* Registers the Memory Manager imports with trap.c. */
void mm_register(void);
