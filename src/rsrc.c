#include "rsrc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "guest_mem.h"
#include "memmgr.h"
#include "trap.h"
#include "util.h"

static struct {
    const uint8_t *fork;
    rsrc_entry *entries;
    uint32_t n;
    uint32_t ntypes;
    int16_t error; /* ResError */
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

/* ---- guest calls ---- */

/* Returns e's handle, loading it into the guest heap on first use. */
static uint32_t load(rsrc_entry *e) {
    if (!e->handle) {
        uint32_t h = mm_new_handle(e->len, false);
        if (!h) {
            R.error = MM_MEM_FULL_ERR;
            return 0;
        }
        memcpy(gm_ptr(gm_r32(h), e->len), rsrc_data(e), e->len);
        uint8_t state = MM_STATE_RESOURCE;
        if (e->attrs & 0x20) /* resPurgeable */
            state |= MM_STATE_PURGEABLE;
        mm_set_handle_state(h, state);
        e->handle = h;
    }
    R.error = 0;
    return e->handle;
}

static uint32_t get(rsrc_entry *e) {
    if (!e) {
        R.error = RSRC_NOT_FOUND_ERR;
        return 0;
    }
    return load(e);
}

static void h_get_resource(void) {
    trap_return(get(rsrc_find(trap_arg(0), (int16_t)trap_arg(1))));
}

static void h_get_named_resource(void) {
    char name[256];
    gm_read_pstr(trap_arg(1), name);
    trap_return(get(rsrc_find_named(trap_arg(0), name)));
}

static void h_load_resource(void) {
    R.error = rsrc_find_handle(trap_arg(0)) ? 0 : RSRC_NOT_FOUND_ERR;
}

static void h_release_resource(void) {
    rsrc_entry *e = rsrc_find_handle(trap_arg(0));
    if (!e) {
        R.error = RSRC_NOT_FOUND_ERR;
        return;
    }
    mm_dispose_handle(e->handle);
    e->handle = 0;
    R.error = 0;
}

static void h_res_error(void) { trap_return((uint32_t)(int32_t)R.error); }

static void h_cur_res_file(void) { trap_return(RSRC_APP_REFNUM); }

/* GetIndString(Str255 theString, short strListID, short index): index is
   1-based; an absent list or index gives an empty string. */
static void h_get_ind_string(void) {
    uint32_t out = trap_arg(0);
    int16_t id = (int16_t)trap_arg(1), index = (int16_t)trap_arg(2);
    gm_w8(out, 0);
    rsrc_entry *e = rsrc_find(FOURCC('S', 'T', 'R', '#'), id);
    if (!e) {
        R.error = RSRC_NOT_FOUND_ERR;
        return;
    }
    R.error = 0;
    const uint8_t *d = rsrc_data(e);
    if (e->len < 2 || index < 1 || index > rd_be16(d))
        return;
    uint32_t p = 2;
    for (int i = 1; i < index; i++) {
        if (p >= e->len)
            return;
        p += 1u + d[p];
    }
    if (p >= e->len || p + 1u + d[p] > e->len)
        return;
    memcpy(gm_ptr(out, 1u + d[p]), d + p, 1u + d[p]);
}

void rsrc_register(void) {
    trap_register("GetResource", h_get_resource);
    trap_register("GetNamedResource", h_get_named_resource);
    trap_register("LoadResource", h_load_resource);
    trap_register("ReleaseResource", h_release_resource);
    trap_register("ResError", h_res_error);
    trap_register("CurResFile", h_cur_res_file);
    trap_register("GetIndString", h_get_ind_string);
}
