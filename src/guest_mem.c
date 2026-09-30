#include "guest_mem.h"

#include <sys/mman.h>

#include "util.h"

#define GUEST_MEM_SIZE GUEST_STACK_TOP

static uint8_t *mem;

static const gm_region regions[] = {
    {GUEST_LOWMEM_BASE, GUEST_LOWMEM_SIZE, GM_PROT_R | GM_PROT_W},
    {GUEST_IMAGE_BASE, GUEST_IMAGE_LIMIT - GUEST_IMAGE_BASE, GM_PROT_R | GM_PROT_W | GM_PROT_X},
    {GUEST_HEAP_BASE, GUEST_HEAP_SIZE, GM_PROT_R | GM_PROT_W},
    {GUEST_STACK_BASE, GUEST_STACK_SIZE, GM_PROT_R | GM_PROT_W},
};

void gm_init(void) {
    gm_shutdown();
    void *p = mmap(NULL, GUEST_MEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        fatal("can't allocate %u bytes of guest memory", GUEST_MEM_SIZE);
    mem = p;
}

void gm_shutdown(void) {
    if (mem)
        munmap(mem, GUEST_MEM_SIZE);
    mem = NULL;
}

uint8_t *gm_host_base(void) {
    return mem;
}

int gm_regions(const gm_region **out) {
    *out = regions;
    return (int)(sizeof regions / sizeof regions[0]);
}

bool gm_is_backed(uint32_t addr, uint32_t len) {
    for (size_t i = 0; i < sizeof regions / sizeof regions[0]; i++) {
        uint64_t start = regions[i].base, end = start + regions[i].size;
        if (addr >= start && (uint64_t)addr + len <= end)
            return true;
    }
    return false;
}

uint8_t *gm_ptr(uint32_t addr, uint32_t len) {
    if (!mem)
        fatal("guest memory used before gm_init()");
    if (!gm_is_backed(addr, len))
        fatal("access to unmapped guest address 0x%08x (%u bytes)", addr, len);
    return mem + addr;
}

uint8_t gm_r8(uint32_t addr) { return *gm_ptr(addr, 1); }
uint16_t gm_r16(uint32_t addr) { return rd_be16(gm_ptr(addr, 2)); }
uint32_t gm_r32(uint32_t addr) { return rd_be32(gm_ptr(addr, 4)); }
void gm_w8(uint32_t addr, uint8_t v) { *gm_ptr(addr, 1) = v; }
void gm_w16(uint32_t addr, uint16_t v) { wr_be16(gm_ptr(addr, 2), v); }
void gm_w32(uint32_t addr, uint32_t v) { wr_be32(gm_ptr(addr, 4), v); }
