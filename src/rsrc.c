#include "rsrc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "files.h"
#include "guest_mem.h"
#include "snapshot.h"
#include "memmgr.h"
#include "trap.h"
#include "util.h"

#define FIRST_REFNUM 200  /* the files the game opens; the File Manager's start at 20 */
#define REL_CAP 1024
#define FORK_HEADER 256   /* the header, then reserved bytes, then the data */
#define MAP_HEADER 28     /* where a map's type list starts */
#define FS_RD_PERM 1

typedef struct {
    int16_t refnum;
    const uint8_t *fork; /* what it was parsed from (or last written) */
    size_t fork_len;
    uint8_t *owned;      /* fork, when the shim read or wrote it (every file but the application's) */
    rsrc_entry *entries;
    uint32_t n, cap;
    uint32_t ntypes;     /* when it was parsed */
    bool writable;
    bool changed;        /* something to write */
    char rel[REL_CAP];   /* where it is, for the File Manager (files.h) */
} res_file;

/* The chain, in the order the files were opened: the application's first. */
static res_file files[RSRC_MAX_FILES];
static uint32_t nfiles;

static struct {
    int32_t cur;   /* the current file, an index into files; -1 for the System file */
    int16_t error; /* ResError */
} R;

/* A snapshot's walk (snapshot.h): ResError, the current file, and the
   guest handle each loaded resource is in, with its attributes. The files
   are parsed by the host and stay where they are; the count of files and
   of each one's entries is walked to check that. */
static void snap_walk(snapshot_walker *w) {
    snapshot_bytes(w, &R, sizeof R);
    SNAPSHOT_PADDING(w, R, error);
    uint32_t nf = nfiles;
    snapshot_bytes(w, &nf, sizeof nf);
    if (nf != nfiles)
        fatal("snapshot: %u resource files are open, and the snapshot has %u", nfiles, nf);
    /* Before rsrc_open, the current file is 0 of none, an empty slot. */
    snapshot_check(w, R.cur >= -1 && R.cur < (int32_t)(nfiles ? nfiles : 1),
                   "current resource file %d", R.cur);
    for (uint32_t i = 0; i < nfiles; i++) {
        res_file *f = &files[i];
        uint32_t n = f->n;
        snapshot_bytes(w, &n, sizeof n);
        if (n != f->n)
            fatal("snapshot: resource file %u has %u entries, and the snapshot %u", i, f->n, n);
        for (uint32_t k = 0; k < f->n; k++) {
            snapshot_bytes(w, &f->entries[k].handle, sizeof f->entries[k].handle);
            snapshot_bytes(w, &f->entries[k].attrs, sizeof f->entries[k].attrs);
        }
    }
}
SNAPSHOT_STATE_WALK(R, snap_walk)

static bool fail(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static bool fail(char *err, size_t errlen, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return false;
}

static void free_file(res_file *f) {
    for (uint32_t i = 0; i < f->n; i++)
        free(f->entries[i].name);
    free(f->entries);
    free(f->owned);
    memset(f, 0, sizeof *f);
}

void rsrc_close(void) {
    for (uint32_t i = 0; i < nfiles; i++)
        free_file(&files[i]);
    nfiles = 0;
    memset(&R, 0, sizeof R);
}

/* Parses fork (len bytes) into f, whose index in the chain is file. */
static bool parse(res_file *f, uint8_t file, const uint8_t *fork, size_t len, char *err, size_t errlen) {
    if (len < 16)
        return fail(err, errlen, "resource fork is too short (%zu bytes)", len);
    uint64_t data_off = rd_be32(fork), map_off = rd_be32(fork + 4);
    uint64_t data_len = rd_be32(fork + 8), map_len = rd_be32(fork + 12);
    if (data_off + data_len > len || map_off + map_len > len || map_len < 30)
        return fail(err, errlen, "resource fork header is out of range");
    const uint8_t *m = fork + map_off;
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
    f->cap = total ? total : 1;
    f->entries = calloc(f->cap, sizeof *f->entries);
    if (!f->entries)
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
            rsrc_entry *e = &f->entries[f->n++];
            e->type = type;
            e->file = file;
            e->id = (int16_t)rd_be16(re);
            uint16_t name_off = rd_be16(re + 2);
            e->attrs = re[4];
            uint64_t off = rd_be32(re + 4) & 0xFFFFFFu;
            if (off + 4 > data_len)
                return fail(err, errlen, "resource %d has a data offset out of range", e->id);
            e->len = rd_be32(fork + data_off + off);
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
    f->ntypes = ntypes;
    f->fork = fork;
    f->fork_len = len;
    return true;
}

static void open_app(void) {
    rsrc_close();
    nfiles = 1;
    files[0].refnum = RSRC_APP_REFNUM;
    R.cur = 0;
}

bool rsrc_open(const uint8_t *fork, size_t len, char *err, size_t errlen) {
    open_app();
    if (parse(&files[0], 0, fork, len, err, errlen))
        return true;
    rsrc_close();
    return false;
}

void rsrc_open_empty(void) { open_app(); }

uint32_t rsrc_type_count(void) { return nfiles ? files[0].ntypes : 0; }
uint32_t rsrc_total(void) { return nfiles ? files[0].n : 0; }
rsrc_entry *rsrc_at(uint32_t i) { return nfiles && i < files[0].n ? &files[0].entries[i] : NULL; }

uint32_t rsrc_count(uint32_t type) {
    uint32_t c = 0;
    for (uint32_t f = 0; f < nfiles; f++)
        for (uint32_t i = 0; i < files[f].n; i++)
            c += files[f].entries[i].type == type;
    return c;
}

static rsrc_entry *find_in(res_file *f, uint32_t type, int16_t id) {
    for (uint32_t i = 0; i < f->n; i++)
        if (f->entries[i].type == type && f->entries[i].id == id)
            return &f->entries[i];
    return NULL;
}

rsrc_entry *rsrc_find(uint32_t type, int16_t id) {
    for (int32_t f = R.cur; f >= 0; f--) {
        rsrc_entry *e = find_in(&files[f], type, id);
        if (e)
            return e;
    }
    return NULL;
}

rsrc_entry *rsrc_find_named(uint32_t type, const char *name) {
    for (int32_t f = R.cur; f >= 0; f--)
        for (uint32_t i = 0; i < files[f].n; i++) {
            rsrc_entry *e = &files[f].entries[i];
            if (e->type == type && e->name && strcasecmp(e->name, name) == 0)
                return e;
        }
    return NULL;
}

rsrc_entry *rsrc_find_handle(uint32_t h) {
    if (!h)
        return NULL;
    for (uint32_t f = 0; f < nfiles; f++)
        for (uint32_t i = 0; i < files[f].n; i++)
            if (files[f].entries[i].handle == h)
                return &files[f].entries[i];
    return NULL;
}

const uint8_t *rsrc_data(const rsrc_entry *e) {
    if (e->added)
        return gm_ptr(gm_r32(e->handle), mm_handle_size(e->handle));
    return files[e->file].fork + e->data_off;
}

/* ---- writing ---- */

uint8_t *rsrc_build(const rsrc_entry *entries, const uint8_t *const *data, uint32_t n, size_t *len) {
    /* The types, in the order they first appear, and the names' bytes. */
    uint32_t *types = calloc(n ? n : 1, sizeof *types), ntypes = 0;
    size_t data_len = 0, names_len = 0;
    if (!types)
        fatal("out of memory");
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t = 0;
        while (t < ntypes && types[t] != entries[i].type)
            t++;
        if (t == ntypes)
            types[ntypes++] = entries[i].type;
        data_len += 4 + (size_t)entries[i].len;
        if (entries[i].name)
            names_len += 1 + strnlen(entries[i].name, 255);
    }
    size_t type_list = MAP_HEADER, refs = type_list + 2 + 8 * (size_t)ntypes;
    size_t name_list = refs + 12 * (size_t)n, map_len = name_list + names_len;
    size_t map_off = FORK_HEADER + data_len;
    *len = map_off + map_len;
    uint8_t *out = calloc(1, *len);
    if (!out)
        fatal("out of memory");
    wr_be32(out, FORK_HEADER);
    wr_be32(out + 4, (uint32_t)map_off);
    wr_be32(out + 8, (uint32_t)data_len);
    wr_be32(out + 12, (uint32_t)map_len);
    uint8_t *m = out + map_off;
    memcpy(m, out, 16); /* the map starts with a copy of the header */
    wr_be16(m + 24, (uint16_t)type_list);
    wr_be16(m + 26, (uint16_t)name_list);
    wr_be16(m + type_list, (uint16_t)(ntypes - 1)); /* 0xFFFF for none */
    /* The data in entry order; the references grouped by type. */
    size_t ref = refs, name = 0;
    for (uint32_t t = 0; t < ntypes; t++) {
        uint8_t *te = m + type_list + 2 + 8 * t;
        wr_be32(te, types[t]);
        wr_be16(te + 6, (uint16_t)(ref - type_list));
        uint32_t count = 0;
        size_t at = 0;
        for (uint32_t i = 0; i < n; i++) {
            const rsrc_entry *e = &entries[i];
            if (e->type == types[t]) {
                uint8_t *re = m + ref;
                wr_be16(re, (uint16_t)e->id);
                wr_be16(re + 2, 0xFFFF);
                if (e->name) {
                    size_t nlen = strnlen(e->name, 255);
                    wr_be16(re + 2, (uint16_t)name);
                    m[name_list + name] = (uint8_t)nlen;
                    memcpy(m + name_list + name + 1, e->name, nlen);
                    name += 1 + nlen;
                }
                wr_be32(re + 4, (uint32_t)at);
                re[4] = e->attrs & (uint8_t)~RSRC_ATTR_CHANGED;
                ref += 12;
                count++;
            }
            at += 4 + (size_t)e->len;
        }
        wr_be16(te + 4, (uint16_t)(count - 1));
    }
    size_t at = FORK_HEADER;
    for (uint32_t i = 0; i < n; i++) {
        wr_be32(out + at, entries[i].len);
        if (entries[i].len)
            memcpy(out + at + 4, data[i], entries[i].len);
        at += 4 + (size_t)entries[i].len;
    }
    free(types);
    return out;
}

/* Writes f again, whole, with each changed or added resource's bytes taken
   from its handle. Returns an OSErr. */
static int16_t write_file(res_file *f) {
    if (!f->writable)
        return FILES_WR_PERM_ERR;
    const uint8_t **data = calloc(f->n ? f->n : 1, sizeof *data);
    rsrc_entry *e = calloc(f->n ? f->n : 1, sizeof *e);
    if (!data || !e)
        fatal("out of memory");
    for (uint32_t i = 0; i < f->n; i++) {
        e[i] = f->entries[i];
        bool from_handle = e[i].added || ((e[i].attrs & RSRC_ATTR_CHANGED) && e[i].handle);
        if (from_handle) {
            e[i].len = mm_handle_size(e[i].handle);
            data[i] = gm_ptr(gm_r32(e[i].handle), e[i].len);
        } else {
            data[i] = f->fork + e[i].data_off;
        }
    }
    size_t len;
    uint8_t *out = rsrc_build(e, data, f->n, &len);
    int16_t err = files_write_rsrc_fork(f->rel, out, len);
    if (err) {
        free(out);
        free(data);
        free(e);
        return err;
    }
    /* The file as written is what its resources are read from now. */
    size_t at = FORK_HEADER;
    for (uint32_t i = 0; i < f->n; i++) {
        rsrc_entry *x = &f->entries[i];
        x->len = e[i].len;
        x->data_off = (uint32_t)(at + 4);
        x->added = false;
        x->attrs &= (uint8_t)~RSRC_ATTR_CHANGED;
        at += 4 + (size_t)x->len;
    }
    free(f->owned);
    f->owned = out;
    f->fork = out;
    f->fork_len = len;
    f->changed = false;
    free(data);
    free(e);
    return 0;
}

void rsrc_update_all(void) {
    for (uint32_t i = 1; i < nfiles; i++)
        if (files[i].changed) {
            int16_t err = write_file(&files[i]);
            if (err)
                log_msg("can't save the resource file %s: error %d", files[i].rel, err);
        }
}

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
        if (e->attrs & RSRC_ATTR_PURGEABLE)
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

/* The entry whose handle h is, or NULL with ResError resNotFound. */
static rsrc_entry *entry_of(uint32_t h) {
    rsrc_entry *e = rsrc_find_handle(h);
    R.error = e ? 0 : RSRC_NOT_FOUND_ERR;
    return e;
}

/* The file e is in, about to be changed by call. */
static res_file *changing(const rsrc_entry *e, const char *call) {
    if (e->file == 0)
        trap_crash("%s: changing the application's resources isn't supported", call);
    files[e->file].changed = true;
    return &files[e->file];
}

static int32_t index_of(int16_t refnum) {
    for (uint32_t i = 0; i < nfiles; i++)
        if (files[i].refnum == refnum)
            return (int32_t)i;
    return -1;
}

static void h_get_resource(void) {
    trap_return(get(rsrc_find(trap_arg(0), (int16_t)trap_arg(1))));
}

static void h_get1_resource(void) {
    trap_return(get(R.cur >= 0 ? find_in(&files[R.cur], trap_arg(0), (int16_t)trap_arg(1)) : NULL));
}

static void h_get_named_resource(void) {
    char name[256];
    gm_read_pstr(trap_arg(1), name);
    trap_return(get(rsrc_find_named(trap_arg(0), name)));
}

static void h_get1_named_resource(void) {
    char name[256];
    gm_read_pstr(trap_arg(1), name);
    rsrc_entry *e = NULL;
    if (R.cur >= 0)
        for (uint32_t i = 0; i < files[R.cur].n && !e; i++) {
            rsrc_entry *x = &files[R.cur].entries[i];
            if (x->type == trap_arg(0) && x->name && strcasecmp(x->name, name) == 0)
                e = x;
        }
    trap_return(get(e));
}

static void h_load_resource(void) { entry_of(trap_arg(0)); }

/* A changed resource stays: its bytes are still to be written. */
static void h_release_resource(void) {
    rsrc_entry *e = entry_of(trap_arg(0));
    if (!e)
        return;
    if (e->attrs & RSRC_ATTR_CHANGED) {
        R.error = RSRC_ATTR_ERR;
        return;
    }
    mm_dispose_handle(e->handle);
    e->handle = 0;
}

/* The handle stays the game's, no longer a resource. */
static void h_detach_resource(void) {
    rsrc_entry *e = entry_of(trap_arg(0));
    if (!e)
        return;
    if (e->attrs & RSRC_ATTR_CHANGED) {
        R.error = RSRC_ATTR_ERR;
        return;
    }
    mm_set_handle_state(e->handle, mm_handle_state(e->handle) & (uint8_t)~MM_STATE_RESOURCE);
    e->handle = 0;
}

static void h_res_error(void) { trap_return((uint32_t)(int32_t)R.error); }

static void h_cur_res_file(void) {
    trap_return((uint32_t)(int32_t)(R.cur >= 0 ? files[R.cur].refnum : RSRC_SYSTEM_REFNUM));
}

static void h_use_res_file(void) {
    int16_t ref = (int16_t)trap_arg(0);
    int32_t i = ref == RSRC_SYSTEM_REFNUM ? -1 : index_of(ref);
    if (i < 0 && ref != RSRC_SYSTEM_REFNUM) {
        R.error = RSRC_FILE_NOT_FOUND_ERR;
        return;
    }
    R.cur = i;
    R.error = 0;
}

/* FSpOpenResFile(const FSSpec *spec, SignedByte permission) -> refnum, or
   -1 with ResError. A file already open gives its refnum and stays where it
   is in the chain. */
static void h_fsp_open_res_file(void) {
    char rel[REL_CAP];
    files_spec_rel(trap_arg(0), rel, sizeof rel);
    int8_t perm = (int8_t)trap_arg(1);
    for (uint32_t i = 1; i < nfiles; i++)
        if (strcmp(files[i].rel, rel) == 0) {
            R.error = 0;
            trap_return((uint32_t)(int32_t)files[i].refnum);
            return;
        }
    uint8_t *fork;
    size_t len;
    int16_t err = files_read_rsrc_fork(rel, &fork, &len);
    if (!err && !fork)
        err = FILES_EOF_ERR; /* a file with no resource fork */
    if (err) {
        R.error = err;
        trap_return((uint32_t)-1);
        return;
    }
    if (nfiles == RSRC_MAX_FILES)
        trap_crash("FSpOpenResFile: more than %d resource files are open", RSRC_MAX_FILES);
    res_file *f = &files[nfiles];
    char why[256];
    if (!parse(f, (uint8_t)nfiles, fork, len, why, sizeof why)) {
        log_msg("FSpOpenResFile: %s: %s", rel, why);
        f->owned = fork;
        free_file(f);
        R.error = RSRC_MAP_READ_ERR;
        trap_return((uint32_t)-1);
        return;
    }
    f->owned = fork;
    f->writable = perm != FS_RD_PERM;
    snprintf(f->rel, sizeof f->rel, "%s", rel);
    int16_t refnum = FIRST_REFNUM;
    while (index_of(refnum) >= 0)
        refnum++;
    f->refnum = refnum;
    R.cur = (int32_t)nfiles++;
    R.error = 0;
    trap_return((uint32_t)(int32_t)refnum);
}

/* FSpCreateResFile(const FSSpec *spec, OSType creator, OSType fileType,
   ScriptCode): a file with an empty resource map, or dupFNErr if it has one. */
static void h_fsp_create_res_file(void) {
    char rel[REL_CAP];
    files_spec_rel(trap_arg(0), rel, sizeof rel);
    uint8_t *fork;
    size_t len;
    int16_t err = files_read_rsrc_fork(rel, &fork, &len);
    free(fork);
    if (!err && len) {
        R.error = FILES_DUP_FN_ERR;
        return;
    }
    uint8_t *empty = rsrc_build(NULL, NULL, 0, &len);
    R.error = files_write_rsrc_fork(rel, empty, len);
    free(empty);
}

/* Writes a file's changes, gives back its resources' handles and takes it
   out of the chain. The application's file and the System's stay open. */
static void h_close_res_file(void) {
    int16_t ref = (int16_t)trap_arg(0);
    if (ref == RSRC_SYSTEM_REFNUM || ref == RSRC_APP_REFNUM) {
        R.error = 0;
        return;
    }
    int32_t i = index_of(ref);
    if (i < 0) {
        R.error = RSRC_FILE_NOT_FOUND_ERR;
        return;
    }
    res_file *f = &files[i];
    R.error = f->changed ? write_file(f) : 0;
    for (uint32_t k = 0; k < f->n; k++)
        if (f->entries[k].handle)
            mm_dispose_handle(f->entries[k].handle);
    free_file(f);
    memmove(&files[i], &files[i + 1], (nfiles - (uint32_t)i - 1) * sizeof files[0]);
    memset(&files[--nfiles], 0, sizeof files[0]);
    for (uint32_t k = (uint32_t)i; k < nfiles; k++)
        for (uint32_t j = 0; j < files[k].n; j++)
            files[k].entries[j].file = (uint8_t)k;
    if (R.cur >= i)
        R.cur--; /* the file below the one closed, if it was current */
}

static void h_update_res_file(void) {
    int32_t i = index_of((int16_t)trap_arg(0));
    if (i < 0) {
        R.error = RSRC_FILE_NOT_FOUND_ERR;
        return;
    }
    R.error = files[i].changed && i > 0 ? write_file(&files[i]) : 0;
}

/* Writes the whole file the resource is in, if the resource changed. */
static void h_write_resource(void) {
    rsrc_entry *e = entry_of(trap_arg(0));
    if (e && (e->attrs & RSRC_ATTR_CHANGED))
        R.error = write_file(changing(e, "WriteResource"));
}

static void h_changed_resource(void) {
    rsrc_entry *e = entry_of(trap_arg(0));
    if (!e)
        return;
    changing(e, "ChangedResource");
    e->attrs |= RSRC_ATTR_CHANGED;
}

/* AddResource(Handle, ResType, short id, ConstStr255Param name): to the
   current file, as a changed resource. */
static void h_add_resource(void) {
    uint32_t h = trap_arg(0);
    if (!mm_is_handle(h) || rsrc_find_handle(h) || R.cur < 0) {
        R.error = RSRC_ADD_FAILED_ERR;
        return;
    }
    if (R.cur == 0)
        trap_crash("AddResource: changing the application's resources isn't supported");
    res_file *f = &files[R.cur];
    if (f->n == f->cap) {
        rsrc_entry *grown = realloc(f->entries, 2 * f->cap * sizeof *grown);
        if (!grown)
            fatal("out of memory");
        f->entries = grown;
        f->cap *= 2;
    }
    char name[256] = "";
    if (trap_arg(3))
        gm_read_pstr(trap_arg(3), name);
    rsrc_entry *e = &f->entries[f->n++];
    *e = (rsrc_entry){.type = trap_arg(1),
                      .id = (int16_t)trap_arg(2),
                      .attrs = RSRC_ATTR_CHANGED,
                      .name = name[0] ? strdup(name) : NULL,
                      .handle = h,
                      .file = (uint8_t)R.cur,
                      .added = true};
    mm_set_handle_state(h, mm_handle_state(h) | MM_STATE_RESOURCE);
    f->changed = true;
    R.error = 0;
}

static void h_count_resources(void) {
    R.error = 0;
    trap_return(rsrc_count(trap_arg(0)) & 0xFFFF);
}

static void h_count1_resources(void) {
    uint32_t c = 0;
    if (R.cur >= 0)
        for (uint32_t i = 0; i < files[R.cur].n; i++)
            c += files[R.cur].entries[i].type == trap_arg(0);
    R.error = 0;
    trap_return(c & 0xFFFF);
}

/* Get1IndResource(ResType, short index): index from 1, in the current file. */
static void h_get1_ind_resource(void) {
    int32_t index = (int16_t)trap_arg(1);
    if (R.cur >= 0)
        for (uint32_t i = 0; i < files[R.cur].n && index > 0; i++)
            if (files[R.cur].entries[i].type == trap_arg(0) && --index == 0) {
                trap_return(load(&files[R.cur].entries[i]));
                return;
            }
    R.error = RSRC_NOT_FOUND_ERR;
    trap_return(0);
}

/* GetIndResource(ResType, short index): index from 1, over every open
   file from the top of the chain down. */
static void h_get_ind_resource(void) {
    uint32_t type = trap_arg(0);
    int32_t index = (int16_t)trap_arg(1);
    for (int32_t f = (int32_t)nfiles - 1; f >= 0 && index > 0; f--)
        for (uint32_t i = 0; i < files[f].n; i++)
            if (files[f].entries[i].type == type && --index == 0) {
                trap_return(load(&files[f].entries[i]));
                return;
            }
    R.error = RSRC_NOT_FOUND_ERR;
    trap_return(0);
}

static void h_get_res_attrs(void) {
    rsrc_entry *e = entry_of(trap_arg(0));
    trap_return(e ? e->attrs : 0);
}

/* resChanged isn't an attribute SetResAttrs sets: ChangedResource does. */
static void h_set_res_attrs(void) {
    rsrc_entry *e = entry_of(trap_arg(0));
    if (!e)
        return;
    changing(e, "SetResAttrs");
    e->attrs = (uint8_t)((trap_arg(1) & ~RSRC_ATTR_CHANGED) | (e->attrs & RSRC_ATTR_CHANGED));
}

/* GetResInfo(Handle, short *id, ResType *type, Str255 name): each out
   parameter may be NULL. */
static void h_get_res_info(void) {
    rsrc_entry *e = entry_of(trap_arg(0));
    if (!e)
        return;
    if (trap_arg(1))
        gm_w16(trap_arg(1), (uint16_t)e->id);
    if (trap_arg(2))
        gm_w32(trap_arg(2), e->type);
    if (trap_arg(3))
        gm_write_pstr(trap_arg(3), e->name ? e->name : "");
}

/* SetResInfo(Handle, short id, ConstStr255Param name): a NULL name keeps
   the one it has. */
static void h_set_res_info(void) {
    rsrc_entry *e = entry_of(trap_arg(0));
    if (!e)
        return;
    changing(e, "SetResInfo");
    e->id = (int16_t)trap_arg(1);
    if (trap_arg(2)) {
        char name[256];
        gm_read_pstr(trap_arg(2), name);
        free(e->name);
        e->name = name[0] ? strdup(name) : NULL;
    }
}

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
    uint32_t len = e->added ? mm_handle_size(e->handle) : e->len;
    if (len < 2 || index < 1 || index > rd_be16(d))
        return;
    uint32_t p = 2;
    for (int i = 1; i < index; i++) {
        if (p >= len)
            return;
        p += 1u + d[p];
    }
    if (p >= len || p + 1u + d[p] > len)
        return;
    memcpy(gm_ptr(out, 1u + d[p]), d + p, 1u + d[p]);
}

void rsrc_register(void) {
    trap_register("GetResource", h_get_resource);
    trap_register("Get1Resource", h_get1_resource);
    trap_register("GetNamedResource", h_get_named_resource);
    trap_register("Get1NamedResource", h_get1_named_resource);
    trap_register("LoadResource", h_load_resource);
    trap_register("ReleaseResource", h_release_resource);
    trap_register("DetachResource", h_detach_resource);
    trap_register("ResError", h_res_error);
    trap_register("CurResFile", h_cur_res_file);
    trap_register("UseResFile", h_use_res_file);
    trap_register("FSpOpenResFile", h_fsp_open_res_file);
    trap_register("FSpCreateResFile", h_fsp_create_res_file);
    trap_register("CloseResFile", h_close_res_file);
    trap_register("UpdateResFile", h_update_res_file);
    trap_register("WriteResource", h_write_resource);
    trap_register("ChangedResource", h_changed_resource);
    trap_register("AddResource", h_add_resource);
    trap_register("CountResources", h_count_resources);
    trap_register("GetIndResource", h_get_ind_resource);
    trap_register("Count1Resources", h_count1_resources);
    trap_register("Get1IndResource", h_get1_ind_resource);
    trap_register("GetResAttrs", h_get_res_attrs);
    trap_register("SetResAttrs", h_set_res_attrs);
    trap_register("GetResInfo", h_get_res_info);
    trap_register("SetResInfo", h_set_res_info);
    trap_register("GetIndString", h_get_ind_string);
}
