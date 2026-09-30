#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The guest's 32-bit address space. See the memory map in the spec. */
#define GUEST_LOWMEM_BASE  0x00000000u
#define GUEST_LOWMEM_SIZE  0x00010000u
#define GUEST_IMAGE_BASE   0x00100000u
#define GUEST_IMAGE_LIMIT  0x01000000u
#define GUEST_HEAP_BASE    0x01000000u
#define GUEST_HEAP_SIZE    0x04000000u
#define GUEST_STACK_BASE   0x06000000u
#define GUEST_STACK_SIZE   0x00100000u
#define GUEST_STACK_TOP    (GUEST_STACK_BASE + GUEST_STACK_SIZE)
/* Unmapped. Jumping here stops the CPU; see cpu_run(). */
#define GUEST_TRAP_BASE    0x07000000u
#define GUEST_TRAP_LIMIT   0x07FF0000u
#define GUEST_TRAP_ADDR(i) (GUEST_TRAP_BASE + 4u * (uint32_t)(i))
#define GUEST_RETURN_MAGIC 0x07FFFFF0u
/* Opaque host-object IDs (Plan 2+). Never dereferenced. */
#define GUEST_TAG_BASE     0x08000000u

#define GM_PROT_R 1
#define GM_PROT_W 2
#define GM_PROT_X 4

typedef struct {
    uint32_t base, size;
    int prot;
} gm_region;

/* Allocates fresh zeroed guest memory, replacing any previous allocation.
   Call before cpu_init(), which maps this memory into the CPU. */
void gm_init(void);
void gm_shutdown(void);

/* Host address of guest address 0. Only backed regions may be touched. */
uint8_t *gm_host_base(void);

/* The regions the CPU maps. Everything else is unmapped. */
int gm_regions(const gm_region **out);

/* True if [addr, addr+len) lies inside a single backed region. */
bool gm_is_backed(uint32_t addr, uint32_t len);

/* Host pointer for a guest range. Fatal if the range is not backed. */
uint8_t *gm_ptr(uint32_t addr, uint32_t len);

uint8_t gm_r8(uint32_t addr);
uint16_t gm_r16(uint32_t addr);
uint32_t gm_r32(uint32_t addr);
void gm_w8(uint32_t addr, uint8_t v);
void gm_w16(uint32_t addr, uint16_t v);
void gm_w32(uint32_t addr, uint32_t v);
