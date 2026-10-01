#include "rsrc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"

static struct {
    const uint8_t *fork;
    rsrc_entry *entries;
    uint32_t n;
    uint32_t ntypes;
} R;

static bool fail(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static bool fail(char *err, size_t errlen, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return false;
}

void rsrc_close(void) {
    for (uint32_t i = 0; i < R.n; i++)
        free(R.entries[i].name);
    free(R.entries);
    memset(&R, 0, sizeof R);
}

static bool parse(const uint8_t *f, size_t len, char *err, size_t errlen) {
    if (len < 16)
        return fail(err, errlen, "resource fork is too short (%zu bytes)", len);
    uint64_t data_off = rd_be32(f), map_off = rd_be32(f + 4);
    uint64_t data_len = rd_be32(f + 8), map_len = rd_be32(f + 12);
    if (data_off + data_len > len || map_off + map_len > len || map_len < 30)
        return fail(err, errlen, "resource fork header is out of range");
    const uint8_t *m = f + map_off;
    uint32_t tl = rd_be16(m + 24), nl = rd_be16(m + 26);
    if (tl + 2u > map_len || nl > map_len)
        return fail(err, errlen, "resource map offsets are out of range");
    /* Counts are stored minus one; a type count of 0xFFFF means an empty map. */
    uint32_t ntypes = (uint16_t)(rd_be16(m + tl) + 1u);
    if (tl + 2ull + 8ull * ntypes > map_len)
        return fail(err, errlen, "resource type list is truncated");

    uint32_t total = 0;
    for (uint32_t t = 0; t < ntypes; t++)
        total += rd_be16(m + tl + 2 + 8 * t + 4) + 1u;
    R.entries = calloc(total ? total : 1, sizeof *R.entries);
    if (!R.entries)
        return fail(err, errlen, "out of memory");

    for (uint32_t t = 0; t < ntypes; t++) {
        const uint8_t *te = m + tl + 2 + 8 * t;
        uint32_t type = rd_be32(te);
        uint32_t count = rd_be16(te + 4) + 1u;
        uint64_t refs = (uint64_t)tl + rd_be16(te + 6);
        if (refs + 12ull * count > map_len)
            return fail(err, errlen, "reference list for type %u is truncated", t);
        for (uint32_t k = 0; k < count; k++) {
            const uint8_t *re = m + refs + 12 * k;
            rsrc_entry *e = &R.entries[R.n++];
            e->type = type;
            e->id = (int16_t)rd_be16(re);
            uint16_t name_off = rd_be16(re + 2);
            e->attrs = re[4];
            uint64_t off = rd_be32(re + 4) & 0xFFFFFFu;
            if (off + 4 > data_len)
                return fail(err, errlen, "resource %d has a data offset out of range", e->id);
            e->len = rd_be32(f + data_off + off);
            if (off + 4 + e->len > data_len)
                return fail(err, errlen, "resource %d has data out of range", e->id);
            e->data_off = (uint32_t)(data_off + off + 4);
            if (name_off != 0xFFFF) {
                uint64_t np = (uint64_t)nl + name_off;
                if (np + 1 > map_len || np + 1 + m[np] > map_len)
                    return fail(err, errlen, "resource %d has a name out of range", e->id);
                e->name = strndup((const char *)m + np + 1, m[np]);
                if (!e->name)
                    return fail(err, errlen, "out of memory");
            }
        }
    }
    R.ntypes = ntypes;
    R.fork = f;
    return true;
}

bool rsrc_open(const uint8_t *fork, size_t len, char *err, size_t errlen) {
    rsrc_close();
    if (parse(fork, len, err, errlen))
        return true;
    rsrc_close();
    return false;
}

uint32_t rsrc_type_count(void) { return R.ntypes; }
uint32_t rsrc_total(void) { return R.n; }

uint32_t rsrc_count(uint32_t type) {
    uint32_t c = 0;
    for (uint32_t i = 0; i < R.n; i++)
        c += R.entries[i].type == type;
    return c;
}

rsrc_entry *rsrc_find(uint32_t type, int16_t id) {
    for (uint32_t i = 0; i < R.n; i++)
        if (R.entries[i].type == type && R.entries[i].id == id)
            return &R.entries[i];
    return NULL;
}

rsrc_entry *rsrc_find_named(uint32_t type, const char *name) {
    for (uint32_t i = 0; i < R.n; i++)
        if (R.entries[i].type == type && R.entries[i].name &&
            strcasecmp(R.entries[i].name, name) == 0)
            return &R.entries[i];
    return NULL;
}

rsrc_entry *rsrc_find_handle(uint32_t h) {
    if (!h)
        return NULL;
    for (uint32_t i = 0; i < R.n; i++)
        if (R.entries[i].handle == h)
            return &R.entries[i];
    return NULL;
}

const uint8_t *rsrc_data(const rsrc_entry *e) { return R.fork + e->data_off; }

