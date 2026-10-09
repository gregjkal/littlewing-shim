#include "loader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_mem.h"
#include "macho.h"

static bool fail(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static bool fail(char *err, size_t errlen, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return false;
}

static uint32_t align_up(uint32_t v, uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}

static bool instantiable(uint8_t kind) {
    return kind == PEF_KIND_CODE || kind == PEF_KIND_UNPACKED_DATA ||
           kind == PEF_KIND_PATTERN_DATA || kind == PEF_KIND_CONSTANT;
}

static bool entry_point(const pef_file *pef, uint8_t *const host[], const uint32_t addr[],
                        const uint32_t size[], int32_t section, uint32_t offset, uint32_t *out,
                        const char *what, char *err, size_t errlen) {
    *out = 0;
    if (section < 0)
        return true;
    if (section >= pef->nsections || !host[section] || (uint64_t)offset + 8 > size[section])
        return fail(err, errlen, "%s entry point is not inside a loaded section", what);
    *out = addr[section] + offset;
    return true;
}

static bool load(const uint8_t *buf, size_t len, loaded_image *img, char *err, size_t errlen) {
    if (!pef_parse(buf, len, &img->pef, err, errlen))
        return false;
    const pef_file *pef = &img->pef;

    uint8_t *host[PEF_MAX_SECTIONS] = {0};
    uint32_t addr[PEF_MAX_SECTIONS] = {0};
    uint32_t size[PEF_MAX_SECTIONS] = {0};
    uint32_t cursor = GUEST_IMAGE_BASE;

    for (int i = 0; i < pef->nsections; i++) {
        const pef_section *s = &pef->sections[i];
        if (!instantiable(s->kind) || s->total_len == 0)
            continue;
        if (s->unpacked_len > s->total_len)
            return fail(err, errlen, "section %d is larger unpacked than in memory", i);
        uint32_t base = align_up(cursor, 0x1000);
        if ((uint64_t)base + s->total_len > GUEST_IMAGE_LIMIT)
            return fail(err, errlen, "section %d does not fit in the image area", i);
        uint8_t *dst = gm_ptr(base, s->total_len);
        memset(dst, 0, s->total_len);
        const uint8_t *src = buf + s->container_off;
        if (s->kind == PEF_KIND_PATTERN_DATA) {
            if (!pef_unpack_pattern(src, s->container_len, dst, s->unpacked_len, err, errlen))
                return false;
        } else {
            if (s->container_len < s->unpacked_len)
                return fail(err, errlen, "section %d is shorter in the file than unpacked", i);
            memcpy(dst, src, s->unpacked_len);
        }
        host[i] = dst;
        addr[i] = base;
        size[i] = s->total_len;
        cursor = base + s->total_len;
        if (s->kind == PEF_KIND_CODE && !img->code_base) {
            img->code_base = base;
            img->code_len = s->total_len;
        }
        if ((s->kind == PEF_KIND_UNPACKED_DATA || s->kind == PEF_KIND_PATTERN_DATA) &&
            !img->data_base) {
            img->data_base = base;
            img->data_len = s->total_len;
        }
    }

    if (pef->nimports > (GUEST_TRAP_LIMIT - GUEST_TRAP_BASE) / 4)
        return fail(err, errlen, "too many imports (%u)", pef->nimports);
    img->import_area = align_up(cursor, 16);
    if ((uint64_t)img->import_area + 8ull * pef->nimports > GUEST_IMAGE_LIMIT)
        return fail(err, errlen, "imports do not fit in the image area");
    img->import_addr = calloc(pef->nimports ? pef->nimports : 1, sizeof *img->import_addr);
    if (!img->import_addr)
        return fail(err, errlen, "out of memory");

    for (uint32_t i = 0; i < pef->nimports; i++) {
        const pef_import *im = &pef->imports[i];
        uint32_t slot = img->import_area + 8 * i;
        switch (im->sym_class) {
        case PEF_SYM_TVECTOR:
            gm_w32(slot, GUEST_TRAP_ADDR(i));
            gm_w32(slot + 4, 0);
            img->import_addr[i] = slot;
            break;
        case PEF_SYM_DATA:
            gm_w32(slot, 0);
            gm_w32(slot + 4, 0);
            img->import_addr[i] = slot;
            break;
        case PEF_SYM_CODE:
            img->import_addr[i] = GUEST_TRAP_ADDR(i);
            break;
        default:
            return fail(err, errlen, "import %s has unsupported symbol class %u", im->name,
                        im->sym_class);
        }
    }

    if (!pef_relocate(pef, host, addr, size, img->import_addr, err, errlen))
        return false;
    if (!entry_point(pef, host, addr, size, pef->main_section, pef->main_offset,
                     &img->main_tvector, "main", err, errlen))
        return false;
    if (!entry_point(pef, host, addr, size, pef->init_section, pef->init_offset,
                     &img->init_tvector, "init", err, errlen))
        return false;

    img->nnames = pef->nimports;
    img->names = calloc(pef->nimports ? pef->nimports : 1, sizeof *img->names);
    if (!img->names)
        return fail(err, errlen, "out of memory");
    for (uint32_t i = 0; i < pef->nimports; i++)
        img->names[i] = pef->imports[i].name;
    return true;
}

bool image_load(const uint8_t *buf, size_t len, loaded_image *img, char *err, size_t errlen) {
    memset(img, 0, sizeof *img);
    img->kind = IMAGE_PEF;
    if (load(buf, len, img, err, errlen))
        return true;
    image_free(img);
    return false;
}

/* ---- Mach-O ---- */

/* Imports the runtime needs that the program doesn't import by name: dlsym
   hands out sprintf, and the __dyld section's words point at the helper. */
static const char *const synthetic_imports[] = {"sprintf", "dyld_stub_binding_helper"};

static image_data_fn data_resolver;

void image_set_data_resolver(image_data_fn fn) {
    data_resolver = fn;
}

typedef struct {
    const macho_file *m;
    loaded_image *img;
    int32_t *import_of_sym; /* symbol index -> import index, or -1 */
    uint32_t *resolved;     /* import index -> bound address, 0 until asked */
    bool *lazy;             /* import index -> called through a lazy pointer */
} binder;

static int32_t find_name(const loaded_image *img, const char *name) {
    for (uint32_t i = 0; i < img->nnames; i++)
        if (strcmp(img->names[i], name) == 0)
            return (int32_t)i;
    return -1;
}

/* The address a pointer to import i should hold. */
static bool bind_address(binder *b, uint32_t i, uint32_t *out, char *err, size_t errlen) {
    if (b->resolved[i]) {
        *out = b->resolved[i];
        return true;
    }
    uint32_t a = GUEST_TRAP_ADDR(i);
    if (!b->lazy[i]) {
        uint32_t r = data_resolver ? data_resolver(b->img->names[i]) : 0;
        if (r == 0)
            return fail(err, errlen, "the shim doesn't know the symbol %s", b->img->names[i]);
        if (r != IMAGE_SYMBOL_CODE)
            a = r;
    }
    b->resolved[i] = *out = a;
    return true;
}

static bool place_segments(const macho_file *m, char *err, size_t errlen) {
    for (int i = 0; i < m->nsegments; i++) {
        const macho_segment *s = &m->segments[i];
        if (strcmp(s->name, "__PAGEZERO") == 0 || strcmp(s->name, "__LINKEDIT") == 0 ||
            s->vmsize == 0)
            continue;
        if (!gm_is_backed(s->vmaddr, s->vmsize))
            return fail(err, errlen, "segment %s (0x%x-0x%x) is outside the image area", s->name,
                        s->vmaddr, s->vmaddr + s->vmsize);
        uint8_t *dst = gm_ptr(s->vmaddr, s->vmsize);
        memcpy(dst, m->data + s->fileoff, s->filesize);
        memset(dst + s->filesize, 0, s->vmsize - s->filesize);
    }
    return true;
}

static bool is_pointer_section(const macho_section *x) {
    uint32_t type = x->flags & 0xFF;
    return type == MACHO_S_LAZY_SYMBOL_POINTERS || type == MACHO_S_NON_LAZY_SYMBOL_POINTERS;
}

static bool bind_pointers(binder *b, char *err, size_t errlen) {
    const macho_file *m = b->m;
    /* First mark what's called through a lazy pointer, so a symbol that is
       both called and taken by address is a function. */
    for (int s = 0; s < m->nsections; s++) {
        const macho_section *x = &m->sections[s];
        if (!is_pointer_section(x))
            continue;
        for (uint32_t k = 0; k < x->size / 4; k++) {
            uint32_t ind = m->indirect[x->reserved1 + k];
            if (ind & (MACHO_INDIRECT_SYMBOL_LOCAL | MACHO_INDIRECT_SYMBOL_ABS))
                continue;
            int32_t i = b->import_of_sym[ind];
            if (i < 0)
                return fail(err, errlen, "%s,%s slot %u names %s, which isn't an import",
                            x->segname, x->sectname, k, m->syms[ind].name);
            if ((x->flags & 0xFF) == MACHO_S_LAZY_SYMBOL_POINTERS)
                b->lazy[i] = true;
        }
    }
    for (int s = 0; s < m->nsections; s++) {
        const macho_section *x = &m->sections[s];
        if (!is_pointer_section(x))
            continue;
        for (uint32_t k = 0; k < x->size / 4; k++) {
            uint32_t ind = m->indirect[x->reserved1 + k];
            if (ind & (MACHO_INDIRECT_SYMBOL_LOCAL | MACHO_INDIRECT_SYMBOL_ABS))
                continue;
            uint32_t a;
            if (!bind_address(b, (uint32_t)b->import_of_sym[ind], &a, err, errlen))
                return false;
            gm_w32(x->addr + 4 * k, a);
        }
    }
    return true;
}

static bool apply_external_relocations(binder *b, char *err, size_t errlen) {
    const macho_file *m = b->m;
    for (uint32_t r = 0; r < m->nextrel; r++) {
        const macho_extern_reloc *e = &m->extrel[r];
        int32_t i = b->import_of_sym[e->symbol];
        if (i < 0)
            return fail(err, errlen, "external relocation %u names %s, which isn't an import", r,
                        m->syms[e->symbol].name);
        if (!gm_is_backed(e->address, 4))
            return fail(err, errlen, "external relocation %u at 0x%x is outside the image", r,
                        e->address);
        uint32_t a;
        if (!bind_address(b, (uint32_t)i, &a, err, errlen))
            return false;
        gm_w32(e->address, gm_r32(e->address) + a); /* the stored word is the addend */
    }
    return true;
}

/* The target of the branch-and-link at addr, or 0 if it isn't one. */
static uint32_t bl_target(uint32_t addr) {
    uint32_t w = gm_r32(addr);
    if ((w & 0xFC000003u) != 0x48000001u) /* opcode 18, AA=0, LK=1 */
        return 0;
    int32_t off = (int32_t)(w & 0x03FFFFFCu);
    if (off & 0x02000000)
        off -= 0x04000000;
    return addr + (uint32_t)off;
}

/* The stub that calls the import named name, or 0. */
static uint32_t stub_for(const binder *b, const char *name) {
    const macho_file *m = b->m;
    for (int s = 0; s < m->nsections; s++) {
        const macho_section *x = &m->sections[s];
        if ((x->flags & 0xFF) != MACHO_S_SYMBOL_STUBS)
            continue;
        for (uint32_t k = 0; k < x->size / x->reserved2; k++) {
            uint32_t ind = m->indirect[x->reserved1 + k];
            if (!(ind & (MACHO_INDIRECT_SYMBOL_LOCAL | MACHO_INDIRECT_SYMBOL_ABS)) &&
                strcmp(m->syms[ind].name, name) == 0)
                return x->addr + k * x->reserved2;
        }
    }
    return 0;
}

/* crt1's entry calls _start, which calls main and then exit. main is the
   target of the last bl before the one to the exit stub. */
#define MAX_SCAN 256

static bool find_main(binder *b, char *err, size_t errlen) {
    const macho_section *text = macho_find_section(b->m, "__TEXT", "__text");
    uint32_t exit_stub = stub_for(b, "exit");
    if (!text || !exit_stub)
        return fail(err, errlen, "can't find main (no __text or no exit stub)");
    uint32_t end = text->addr + text->size;
    uint32_t start = 0;
    for (uint32_t a = b->m->entry; a < end && a < b->m->entry + 4 * MAX_SCAN && !start; a += 4)
        start = bl_target(a);
    if (!start || start < text->addr || start >= end)
        return fail(err, errlen, "can't find main (no call to _start after the entry point)");
    uint32_t prev = 0;
    for (uint32_t a = start; a < end && a < start + 4 * MAX_SCAN; a += 4) {
        uint32_t t = bl_target(a);
        if (!t)
            continue;
        if (t == exit_stub) {
            if (!prev || prev < text->addr || prev >= end)
                return fail(err, errlen, "can't find main (no call before exit in _start)");
            b->img->main_addr = prev;
            return true;
        }
        prev = t;
    }
    return fail(err, errlen, "can't find main (_start never calls exit)");
}

static bool list_initializers(binder *b, char *err, size_t errlen) {
    const macho_file *m = b->m;
    for (int s = 0; s < m->nsections; s++) {
        const macho_section *x = &m->sections[s];
        if ((x->flags & 0xFF) != MACHO_S_MOD_INIT_FUNC_POINTERS)
            continue;
        uint32_t n = x->size / 4;
        uint32_t *grown = realloc(b->img->init_addrs, (b->img->ninit + n + 1) * sizeof *grown);
        if (!grown)
            return fail(err, errlen, "out of memory");
        b->img->init_addrs = grown;
        for (uint32_t k = 0; k < n; k++)
            b->img->init_addrs[b->img->ninit++] = gm_r32(x->addr + 4 * k);
    }
    return true;
}

static bool load_macho(const macho_file *m, loaded_image *img, char *err, size_t errlen) {
    if (gm_current_layout() != GM_LAYOUT_MACHO)
        return fail(err, errlen, "a Mach-O program needs the Mach-O memory layout");
    if (!place_segments(m, err, errlen))
        return false;

    binder b = {m, img, NULL, NULL, NULL};
    uint32_t nimports = 0;
    for (uint32_t s = 0; s < m->nsyms; s++)
        nimports += macho_symbol_is_import(&m->syms[s]);
    uint32_t cap = nimports + sizeof synthetic_imports / sizeof synthetic_imports[0];
    if (cap > (GUEST_TRAP_LIMIT - GUEST_TRAP_BASE) / 4)
        return fail(err, errlen, "too many imports (%u)", nimports);
    img->names = calloc(cap, sizeof *img->names);
    b.import_of_sym = calloc(m->nsyms ? m->nsyms : 1, sizeof *b.import_of_sym);
    b.resolved = calloc(cap, sizeof *b.resolved);
    b.lazy = calloc(cap, sizeof *b.lazy);
    bool ok = img->names && b.import_of_sym && b.resolved && b.lazy;
    if (!ok) {
        fail(err, errlen, "out of memory");
        goto done;
    }
    for (uint32_t s = 0; s < m->nsyms; s++) {
        b.import_of_sym[s] = -1;
        if (macho_symbol_is_import(&m->syms[s])) {
            b.import_of_sym[s] = (int32_t)img->nnames;
            img->names[img->nnames++] = m->syms[s].name;
        }
    }
    for (size_t k = 0; k < sizeof synthetic_imports / sizeof synthetic_imports[0]; k++)
        if (find_name(img, synthetic_imports[k]) < 0)
            img->names[img->nnames++] = synthetic_imports[k];

    ok = bind_pointers(&b, err, errlen) && apply_external_relocations(&b, err, errlen);
    if (ok) {
        const macho_section *dyld = macho_find_section(m, "__DATA", "__dyld");
        if (dyld && dyld->size >= 8) {
            uint32_t helper = GUEST_TRAP_ADDR(find_name(img, "dyld_stub_binding_helper"));
            gm_w32(dyld->addr, helper);
            gm_w32(dyld->addr + 4, helper);
        }
        ok = find_main(&b, err, errlen) && list_initializers(&b, err, errlen);
    }
    if (ok) {
        /* Code addresses print as code+<linked address>, which is what a
           disassembly of the file shows. */
        for (int s = 0; s < m->nsegments; s++)
            if (strcmp(m->segments[s].name, "__TEXT") == 0)
                img->code_len = m->segments[s].vmaddr + m->segments[s].vmsize;
        img->code_base = 0;
    }
done:
    free(b.import_of_sym);
    free(b.resolved);
    free(b.lazy);
    return ok;
}

bool image_load_macho(const uint8_t *buf, size_t len, loaded_image *img, char *err,
                      size_t errlen) {
    memset(img, 0, sizeof *img);
    img->kind = IMAGE_MACHO;
    macho_file m;
    if (!macho_parse(buf, len, &m, err, errlen))
        return false;
    bool ok = load_macho(&m, img, err, errlen);
    macho_free(&m);
    if (ok)
        return true;
    image_free(img);
    return false;
}

void image_free(loaded_image *img) {
    pef_free(&img->pef);
    free(img->import_addr);
    free(img->names);
    free(img->init_addrs);
    memset(img, 0, sizeof *img);
}

int32_t image_find_import(const loaded_image *img, const char *name) {
    return find_name(img, name);
}
