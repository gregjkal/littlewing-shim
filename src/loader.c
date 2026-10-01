#include "loader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_mem.h"

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
    return true;
}

bool image_load(const uint8_t *buf, size_t len, loaded_image *img, char *err, size_t errlen) {
    memset(img, 0, sizeof *img);
    if (load(buf, len, img, err, errlen))
        return true;
    image_free(img);
    return false;
}

void image_free(loaded_image *img) {
    pef_free(&img->pef);
    free(img->import_addr);
    memset(img, 0, sizeof *img);
}

int32_t image_find_import(const loaded_image *img, const char *name) {
    for (uint32_t i = 0; i < img->pef.nimports; i++)
        if (strcmp(img->pef.imports[i].name, name) == 0)
            return (int32_t)i;
    return -1;
}
