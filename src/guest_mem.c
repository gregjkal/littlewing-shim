#include "guest_mem.h"

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "util.h"

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static uint8_t *mem;
static size_t mem_size;
static gm_fault_fn fault_handler;

static const gm_region pef_regions[] = {
    {GUEST_LOWMEM_BASE, GUEST_LOWMEM_SIZE, GM_PROT_R | GM_PROT_W},
    {GUEST_IMAGE_BASE, GUEST_IMAGE_LIMIT - GUEST_IMAGE_BASE, GM_PROT_R | GM_PROT_W | GM_PROT_X},
    {GUEST_HEAP_BASE, GUEST_HEAP_SIZE, GM_PROT_R | GM_PROT_W},
    {GUEST_STACK_BASE, GUEST_STACK_SIZE, GM_PROT_R | GM_PROT_W},
};

static const gm_region macho_regions[] = {
    {GUEST_MACHO_IMAGE_BASE, GUEST_MACHO_IMAGE_LIMIT - GUEST_MACHO_IMAGE_BASE,
     GM_PROT_R | GM_PROT_W | GM_PROT_X},
    {GUEST_STACK_BASE, GUEST_STACK_SIZE, GM_PROT_R | GM_PROT_W},
    {GUEST_MACHO_HEAP_BASE, GUEST_MACHO_HEAP_SIZE, GM_PROT_R | GM_PROT_W},
};

static gm_layout layout = GM_LAYOUT_PEF;
static const gm_region *regions = pef_regions;
static int nregions = COUNT(pef_regions);

void gm_init_layout(gm_layout l) {
    gm_shutdown();
    layout = l;
    regions = l == GM_LAYOUT_MACHO ? macho_regions : pef_regions;
    nregions = l == GM_LAYOUT_MACHO ? COUNT(macho_regions) : COUNT(pef_regions);
    /* One reservation from address 0 to the end of the highest region. The
       host backs only the pages the guest touches. */
    uint64_t end = 0;
    for (int i = 0; i < nregions; i++)
        if ((uint64_t)regions[i].base + regions[i].size > end)
            end = (uint64_t)regions[i].base + regions[i].size;
    mem_size = (size_t)end;
    void *p = mmap(NULL, mem_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        fatal("can't allocate %zu bytes of guest memory", mem_size);
    mem = p;
}

void gm_init(void) {
    gm_init_layout(GM_LAYOUT_PEF);
}

void gm_shutdown(void) {
    if (mem)
        munmap(mem, mem_size);
    mem = NULL;
}

gm_layout gm_current_layout(void) {
    return layout;
}

uint32_t gm_heap_base(void) {
    return layout == GM_LAYOUT_MACHO ? GUEST_MACHO_HEAP_BASE : GUEST_HEAP_BASE;
}

uint32_t gm_heap_size(void) {
    return layout == GM_LAYOUT_MACHO ? GUEST_MACHO_HEAP_SIZE : GUEST_HEAP_SIZE;
}

uint8_t *gm_host_base(void) {
    return mem;
}

int gm_regions(const gm_region **out) {
    *out = regions;
    return nregions;
}

bool gm_is_backed(uint32_t addr, uint32_t len) {
    for (int i = 0; i < nregions; i++) {
        uint64_t start = regions[i].base, end = start + regions[i].size;
        if (addr >= start && addr < end && (uint64_t)addr + len <= end)
            return true;
    }
    return false;
}

void gm_set_fault_handler(gm_fault_fn fn) {
    fault_handler = fn;
}

uint8_t *gm_ptr(uint32_t addr, uint32_t len) {
    if (!mem)
        fatal("guest memory used before gm_init()");
    if (!gm_is_backed(addr, len)) {
        char msg[96];
        snprintf(msg, sizeof msg, "access to unmapped guest address 0x%08x (%u bytes)", addr, len);
        if (fault_handler)
            fault_handler(msg);
        fatal("%s", msg);
    }
    return mem + addr;
}

uint8_t gm_r8(uint32_t addr) { return *gm_ptr(addr, 1); }
uint16_t gm_r16(uint32_t addr) { return rd_be16(gm_ptr(addr, 2)); }
uint32_t gm_r32(uint32_t addr) { return rd_be32(gm_ptr(addr, 4)); }
void gm_w8(uint32_t addr, uint8_t v) { *gm_ptr(addr, 1) = v; }
void gm_w16(uint32_t addr, uint16_t v) { wr_be16(gm_ptr(addr, 2), v); }
void gm_w32(uint32_t addr, uint32_t v) { wr_be32(gm_ptr(addr, 4), v); }

void gm_read_pstr(uint32_t addr, char out[256]) {
    uint8_t n = gm_r8(addr);
    memcpy(out, gm_ptr(addr + 1, n), n);
    out[n] = '\0';
}

void gm_write_pstr(uint32_t addr, const char *s) {
    size_t n = strlen(s);
    if (n > 255)
        n = 255;
    uint8_t *p = gm_ptr(addr, (uint32_t)n + 1);
    p[0] = (uint8_t)n;
    memcpy(p + 1, s, n);
}

bool gm_read_cstr(uint32_t addr, char *out, size_t cap) {
    for (size_t i = 0; i + 1 < cap; i++) {
        out[i] = (char)gm_r8(addr + (uint32_t)i);
        if (out[i] == '\0')
            return true;
    }
    if (cap)
        out[cap - 1] = '\0';
    return false;
}

void gm_write_cstr(uint32_t addr, const char *s) {
    size_t n = strlen(s) + 1;
    memcpy(gm_ptr(addr, (uint32_t)n), s, n);
}
