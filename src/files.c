#include "files.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "guest_mem.h"
#include "trap.h"
#include "util.h"

#define MAX_DIRS 64
#define MAX_FILES 16
#define MAX_REFS 64
#define FSREF_SIZE 80
#define FSREF_MAGIC 0x4C576673u /* 'LWfs' */
#define FIRST_REFNUM 20
#define REL_CAP 1024  /* a path relative to the game folder */
#define PATH_CAP 2100 /* a host path */
#define FILES_PARAM_ERR (-50)

/* ParamBlockRec (IOParam) offsets. */
#define PB_RESULT     16
#define PB_REFNUM     24
#define PB_BUFFER     32
#define PB_REQ_COUNT  36
#define PB_ACT_COUNT  40
#define PB_POS_MODE   44
#define PB_POS_OFFSET 46

/* Positioning modes. */
#define FS_AT_MARK    0
#define FS_FROM_START 1
#define FS_FROM_LEOF  2
#define FS_FROM_MARK  3

typedef struct {
    FILE *f;
    long eof;
    bool writable; /* opened with a permission that allows writing */
    bool in_data;  /* f is the copy in the data folder */
    char rel[REL_CAP];
} open_file;

static struct {
    char game_dir[1024];
    char data_dir[1024]; /* "" = none */
    char dirs[MAX_DIRS][REL_CAP]; /* relative host path of each directory ID - 2 ("" = game folder) */
    int ndirs;
    open_file files[MAX_FILES];
    char refs[MAX_REFS][REL_CAP]; /* what each FSRef names, relative to the game folder */
    int nrefs;
    bool warned_no_data;
} F;

/* Unicode code points for Mac Roman 0x80-0xFF. */
static const uint16_t mac_roman[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3,
    0x00E5, 0x00E7, 0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
    0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3,
    0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA,
    0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
    0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D,
    0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE,
    0x00CF, 0x00CC, 0x00D3, 0x00D4, 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
    0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

void files_mac_to_utf8(const char *mac, char *out, size_t cap) {
    size_t o = 0;
    for (const uint8_t *p = (const uint8_t *)mac; *p && o + 4 < cap; p++) {
        uint32_t c = *p < 0x80 ? *p : mac_roman[*p - 0x80];
        if (c == '/')
            c = ':';
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = '\0';
}

bool files_data_root(char *out, size_t cap) {
    const char *d = getenv("LOONY_DATA_DIR"), *home = getenv("HOME");
    if ((d && *d) || !home || !*home)
        return false;
    snprintf(out, cap, "%s/Library/Application Support/loony-shim", home);
    return true;
}

bool files_data_dir(const char *game_id, char *out, size_t cap) {
    const char *d = getenv("LOONY_DATA_DIR");
    if (d && *d) {
        snprintf(out, cap, "%s", d);
        return true;
    }
    char root[PATH_MAX];
    if (!files_data_root(root, sizeof root))
        return false;
    snprintf(out, cap, "%s/%s", root, game_id);
    return true;
}

/* path made absolute with symbolic links resolved, as far as it exists;
   the missing rest is appended as given. */
static void canonical(const char *path, char *out, size_t cap) {
    char p[PATH_MAX], rest[PATH_MAX] = "";
    snprintf(p, sizeof p, "%s", path);
    for (;;) {
        char real[PATH_MAX];
        if (realpath(p, real)) {
            snprintf(out, cap, "%s%s", real, rest);
            return;
        }
        char *slash = strrchr(p, '/');
        if (!slash || slash == p) {
            snprintf(out, cap, "%s", path);
            return;
        }
        char tail[PATH_MAX];
        snprintf(tail, sizeof tail, "%s%s", slash, rest);
        snprintf(rest, sizeof rest, "%s", tail);
        *slash = '\0';
    }
}

/* True if a is b or inside it. */
static bool within(const char *a, const char *b) {
    size_t n = strlen(b);
    return strncmp(a, b, n) == 0 && (a[n] == '\0' || a[n] == '/' || (n > 0 && b[n - 1] == '/'));
}

bool files_init(const char *game_dir, const char *data_dir) {
    for (int i = 0; i < MAX_FILES; i++)
        if (F.files[i].f)
            fclose(F.files[i].f);
    memset(&F, 0, sizeof F);
    snprintf(F.game_dir, sizeof F.game_dir, "%s", game_dir);
    F.ndirs = 1; /* ID 2: the game folder */
    if (!data_dir)
        return false;
    char g[PATH_MAX], d[PATH_MAX];
    canonical(game_dir, g, sizeof g);
    canonical(data_dir, d, sizeof d);
    if (within(g, d) || within(d, g)) {
        log_msg("the writable folder %s overlaps the game folder %s; nothing will be saved", d, g);
        return false;
    }
    snprintf(F.data_dir, sizeof F.data_dir, "%s", data_dir);
    return true;
}

static const char *dir_path(uint32_t id) {
    if (id < FILES_ROOT_DIRID || id - FILES_ROOT_DIRID >= (uint32_t)F.ndirs)
        return NULL;
    return F.dirs[id - FILES_ROOT_DIRID];
}

static uint32_t dir_id(const char *rel) {
    for (int i = 0; i < F.ndirs; i++)
        if (strcmp(F.dirs[i], rel) == 0)
            return FILES_ROOT_DIRID + (uint32_t)i;
    if (F.ndirs == MAX_DIRS)
        trap_crash("more than %d directories", MAX_DIRS);
    snprintf(F.dirs[F.ndirs], sizeof F.dirs[0], "%s", rel);
    return FILES_ROOT_DIRID + (uint32_t)F.ndirs++;
}

/* rel inside root (the game or data folder). */
static void under(const char *root, const char *rel, char *out, size_t cap) {
    if (snprintf(out, cap, "%s%s%s", root, *rel ? "/" : "", rel) >= (int)cap)
        trap_crash("the path %s/%s is too long", root, rel);
}

static bool exists(const char *path, bool want_dir) {
    struct stat st;
    return stat(path, &st) == 0 && (want_dir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode));
}

/* Where rel is on the host: in the data folder if it's there (returns
   true), otherwise in the game folder, whether or not it exists. */
static bool locate(const char *rel, bool want_dir, char *out, size_t cap) {
    if (F.data_dir[0]) {
        under(F.data_dir, rel, out, cap);
        if (exists(out, want_dir))
            return true;
    }
    under(F.game_dir, rel, out, cap);
    return false;
}

static bool is_dir(const char *rel) {
    char p[PATH_CAP];
    locate(rel, true, p, sizeof p);
    return exists(p, true);
}

/* Joins a relative directory and a UTF-8 component. */
static void join(const char *dir, const char *name, char *out, size_t cap) {
    if (snprintf(out, cap, "%s%s%s", dir, *dir ? "/" : "", name) >= (int)cap)
        trap_crash("the path %s/%s is too long", dir, name);
}

/* FSMakeFSSpec(vRefNum, dirID, fileName, FSSpec *spec) -> OSErr. A leading
   ':' means relative; each further empty component ("::") goes up one
   folder, but never above the game folder. */
static void h_fs_make_fsspec(void) {
    int16_t vref = (int16_t)trap_arg(0);
    uint32_t dir = trap_arg(1), name_p = trap_arg(2), spec = trap_arg(3);
    if (vref != 0 && vref != FILES_VREFNUM)
        trap_crash("FSMakeFSSpec: unknown volume %d", vref);
    if (dir == 0)
        dir = FILES_ROOT_DIRID;
    const char *base = dir_path(dir);
    if (!base)
        trap_crash("FSMakeFSSpec: unknown directory ID %u", dir);
    char mac[256];
    gm_read_pstr(name_p, mac);
    char rel[REL_CAP], comp[256], utf[REL_CAP];
    snprintf(rel, sizeof rel, "%s", base);
    const char *p = mac[0] == ':' ? mac + 1 : mac;
    if (strchr(mac, ':') && mac[0] != ':')
        trap_crash("FSMakeFSSpec: full path names (\"%s\") are not supported", mac);
    for (;;) {
        const char *colon = strchr(p, ':');
        size_t n = colon ? (size_t)(colon - p) : strlen(p);
        if (n > 63) {
            trap_return((uint32_t)FILES_BD_NAM_ERR);
            return;
        }
        if (n == 0 && colon) { /* "::": the parent folder */
            char *slash = strrchr(rel, '/');
            if (!*rel) {
                trap_return((uint32_t)FILES_DIR_NF_ERR);
                return;
            }
            if (slash)
                *slash = '\0';
            else
                rel[0] = '\0';
            p = colon + 1;
            continue;
        }
        memcpy(comp, p, n);
        comp[n] = '\0';
        /* "." and ".." are ordinary Mac names but would move around on the host. */
        if (n == 0 || strcmp(comp, ".") == 0 || strcmp(comp, "..") == 0) {
            trap_return((uint32_t)FILES_BD_NAM_ERR);
            return;
        }
        if (!colon)
            break;
        files_mac_to_utf8(comp, utf, sizeof utf);
        char next[REL_CAP];
        join(rel, utf, next, sizeof next);
        if (!is_dir(next)) {
            trap_return((uint32_t)FILES_DIR_NF_ERR);
            return;
        }
        snprintf(rel, sizeof rel, "%s", next);
        p = colon + 1;
    }
    gm_w16(spec, (uint16_t)FILES_VREFNUM);
    gm_w32(spec + 2, dir_id(rel));
    memset(gm_ptr(spec + 6, 64), 0, 64);
    gm_write_pstr(spec + 6, comp);
    files_mac_to_utf8(comp, utf, sizeof utf);
    char file_rel[REL_CAP], hp[PATH_CAP];
    join(rel, utf, file_rel, sizeof file_rel);
    locate(file_rel, false, hp, sizeof hp);
    trap_return(exists(hp, false) ? 0 : (uint32_t)FILES_FNF_ERR);
}

/* The path of spec's file relative to the game folder. */
static void spec_rel_path(uint32_t spec, char *out, size_t cap) {
    uint32_t dir = gm_r32(spec + 2);
    const char *base = dir_path(dir);
    if (!base)
        trap_crash("FSSpec has an unknown directory ID %u", dir);
    char mac[256], utf[REL_CAP];
    gm_read_pstr(spec + 6, mac);
    if (!mac[0] || strcmp(mac, ".") == 0 || strcmp(mac, "..") == 0 || strchr(mac, ':'))
        trap_crash("FSSpec has a bad name \"%s\"", mac);
    files_mac_to_utf8(mac, utf, sizeof utf);
    join(base, utf, out, cap);
}

/* Logs (once) that there's nowhere to write. */
static int16_t no_data_dir(const char *call) {
    if (!F.warned_no_data)
        log_msg("%s: there is no writable folder; the game can't save files", call);
    F.warned_no_data = true;
    return FILES_WR_PERM_ERR;
}

/* FSpCreate(const FSSpec *spec, OSType creator, OSType fileType, ScriptCode) -> OSErr */
static void h_fsp_create(void) {
    char rel[REL_CAP], path[PATH_CAP];
    spec_rel_path(trap_arg(0), rel, sizeof rel);
    locate(rel, false, path, sizeof path);
    if (exists(path, false)) {
        trap_return((uint32_t)FILES_DUP_FN_ERR);
        return;
    }
    if (!F.data_dir[0]) {
        trap_return((uint32_t)no_data_dir("FSpCreate"));
        return;
    }
    under(F.data_dir, rel, path, sizeof path);
    char parent[PATH_CAP];
    snprintf(parent, sizeof parent, "%s", path);
    *strrchr(parent, '/') = '\0';
    FILE *f = make_dirs(parent) ? fopen(path, "wb") : NULL;
    if (!f || fclose(f) != 0) {
        log_msg("FSpCreate: can't create %s: %s", path, strerror(errno));
        trap_return((uint32_t)FILES_IO_ERR);
        return;
    }
    trap_return(0);
}

/* Opens the data fork of rel (relative to the game folder) and writes its
   refnum to out. Every permission but fsRdPerm (1) allows writing; the file
   is opened for reading either way and copied to the data folder at its
   first write. Returns an OSErr. */
static int16_t open_data_fork(const char *call, const char *rel, int perm, uint32_t out) {
    if (perm < 0 || perm > 4)
        trap_crash("%s: unknown permission %d", call, perm);
    char path[PATH_CAP];
    bool in_data = locate(rel, false, path, sizeof path);
    int slot = 0;
    while (slot < MAX_FILES && F.files[slot].f)
        slot++;
    if (slot == MAX_FILES)
        trap_crash("%s: more than %d open files", call, MAX_FILES);
    bool writable = perm != 1;
    FILE *f = exists(path, false) ? fopen(path, in_data && writable ? "r+b" : "rb") : NULL;
    if (!f)
        return FILES_FNF_ERR;
    fseek(f, 0, SEEK_END);
    F.files[slot].f = f;
    F.files[slot].eof = ftell(f);
    F.files[slot].writable = writable;
    F.files[slot].in_data = in_data;
    snprintf(F.files[slot].rel, sizeof F.files[slot].rel, "%s", rel);
    fseek(f, 0, SEEK_SET);
    gm_w16(out, (uint16_t)(FIRST_REFNUM + slot));
    return 0;
}

/* FSpOpenDF(const FSSpec *spec, SInt8 permission, short *refNum) -> OSErr. */
static void h_fsp_open_df(void) {
    char rel[REL_CAP];
    spec_rel_path(trap_arg(0), rel, sizeof rel);
    trap_return((uint32_t)(int32_t)open_data_fork("FSpOpenDF", rel, (int8_t)trap_arg(1), trap_arg(2)));
}

/* ---- FSRefs: a magic word and an index into a table of paths relative to
   the game folder, so they go through the same overlay as FSSpecs ---- */

/* FSPathMakeRef(const UInt8 *path, FSRef *ref, Boolean *isDirectory) ->
   OSStatus. path is a host path in UTF-8, inside the game folder or the
   data folder (which stands for the same place); anything else is fnfErr. */
static void h_fs_path_make_ref(void) {
    char path[PATH_CAP], abs[PATH_MAX], root[PATH_MAX], rel[REL_CAP] = "";
    if (!gm_read_cstr(trap_arg(0), path, sizeof path))
        trap_crash("FSPathMakeRef: the path is longer than %d bytes", PATH_CAP - 1);
    uint32_t ref = trap_arg(1), is_dir_out = trap_arg(2);
    canonical(path, abs, sizeof abs);
    bool inside = false;
    const char *roots[2] = {F.game_dir, F.data_dir};
    for (int i = 0; i < 2 && !inside; i++) {
        if (!roots[i][0])
            continue;
        canonical(roots[i], root, sizeof root);
        if (within(abs, root)) {
            inside = true;
            const char *tail = abs + strlen(root);
            snprintf(rel, sizeof rel, "%s", *tail == '/' ? tail + 1 : tail);
        }
    }
    if (!inside || strstr(rel, "/../") || strncmp(rel, "../", 3) == 0) {
        log_msg("FSPathMakeRef: %s is outside the game's folders", path);
        trap_return((uint32_t)FILES_FNF_ERR);
        return;
    }
    char host[PATH_CAP];
    locate(rel, false, host, sizeof host);
    bool dir = is_dir(rel);
    if (!dir && !exists(host, false)) {
        trap_return((uint32_t)FILES_FNF_ERR);
        return;
    }
    int i = 0;
    while (i < F.nrefs && strcmp(F.refs[i], rel) != 0)
        i++;
    if (i == F.nrefs) {
        if (F.nrefs == MAX_REFS)
            trap_crash("FSPathMakeRef: more than %d files", MAX_REFS);
        snprintf(F.refs[F.nrefs++], sizeof F.refs[0], "%s", rel);
    }
    memset(gm_ptr(ref, FSREF_SIZE), 0, FSREF_SIZE);
    gm_w32(ref, FSREF_MAGIC);
    gm_w32(ref + 4, (uint32_t)i);
    if (is_dir_out)
        gm_w8(is_dir_out, dir);
    trap_return(0);
}

/* The path an FSRef names, relative to the game folder. */
static const char *ref_rel_path(const char *call, uint32_t ref) {
    uint32_t i = gm_r32(ref + 4);
    if (gm_r32(ref) != FSREF_MAGIC || i >= (uint32_t)F.nrefs)
        trap_crash("%s: 0x%08x is not an FSRef from FSPathMakeRef", call, ref);
    return F.refs[i];
}

/* FSGetDataForkName(HFSUniStr255 *name): the data fork's name is empty. */
static void h_fs_get_data_fork_name(void) {
    gm_w16(trap_arg(0), 0);
    trap_return(0);
}

/* FSOpenFork(const FSRef *ref, UniCharCount forkNameLength, const UniChar
   *forkName, SInt8 permissions, FSIORefNum *forkRefNum) -> OSErr. Only the
   data fork. */
static void h_fs_open_fork(void) {
    const char *rel = ref_rel_path("FSOpenFork", trap_arg(0));
    if (trap_arg(1) != 0)
        trap_crash("FSOpenFork: only the data fork can be opened (%s)", rel);
    trap_return((uint32_t)(int32_t)open_data_fork("FSOpenFork", rel, (int8_t)trap_arg(3), trap_arg(4)));
}

static open_file *file_of(int16_t ref) {
    int slot = ref - FIRST_REFNUM;
    if (slot < 0 || slot >= MAX_FILES || !F.files[slot].f)
        return NULL;
    return &F.files[slot];
}

/* Makes an open file writable: copies a game-folder file into the data
   folder (unless another open file already did) and reopens the copy at the
   same mark. The copy is made under a temporary name and renamed when
   complete, so a failed copy never hides the original. Returns an OSErr. */
static int16_t make_writable(const char *call, open_file *o) {
    if (!o->writable)
        return FILES_WR_PERM_ERR;
    if (o->in_data)
        return 0;
    if (!F.data_dir[0])
        return no_data_dir(call);
    char src[PATH_CAP], dst[PATH_CAP], tmp[PATH_CAP + 8], parent[PATH_CAP];
    under(F.game_dir, o->rel, src, sizeof src);
    under(F.data_dir, o->rel, dst, sizeof dst);
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    snprintf(parent, sizeof parent, "%s", dst);
    *strrchr(parent, '/') = '\0';
    long mark = ftell(o->f);
    bool ok = true;
    if (!exists(dst, false)) {
        FILE *out = make_dirs(parent) ? fopen(tmp, "wb") : NULL;
        ok = out != NULL;
        fseek(o->f, 0, SEEK_SET);
        char buf[65536];
        size_t n;
        while (ok && (n = fread(buf, 1, sizeof buf, o->f)) > 0)
            ok = fwrite(buf, 1, n, out) == n;
        ok = ok && !ferror(o->f);
        if (out && fclose(out) != 0)
            ok = false;
        ok = ok && rename(tmp, dst) == 0;
        if (!ok)
            remove(tmp);
    }
    FILE *f = ok ? fopen(dst, "r+b") : NULL;
    if (!f) {
        log_msg("%s: can't copy %s to %s: %s", call, src, dst, strerror(errno));
        fseek(o->f, mark, SEEK_SET);
        return FILES_IO_ERR;
    }
    fclose(o->f);
    o->f = f;
    o->in_data = true;
    fseek(f, mark, SEEK_SET);
    return 0;
}

/* Moves the mark; returns an OSErr. */
static int16_t set_pos(FILE *f, long eof, int mode, int32_t off) {
    long base;
    switch (mode & 3) {
    case FS_AT_MARK: fseek(f, 0, SEEK_CUR); return 0; /* C needs a seek between writes and reads */
    case FS_FROM_START: base = 0; break;
    case FS_FROM_LEOF: base = eof; break;
    default: base = ftell(f); break;
    }
    long pos = base + off;
    if (pos < 0)
        return FILES_POS_ERR;
    if (pos > eof) {
        fseek(f, eof, SEEK_SET);
        return FILES_EOF_ERR;
    }
    fseek(f, pos, SEEK_SET);
    return 0;
}

/* PBReadSync(ParmBlkPtr) -> OSErr */
static void h_pb_read_sync(void) {
    uint32_t pb = trap_arg(0);
    open_file *o = file_of((int16_t)gm_r16(pb + PB_REFNUM));
    int16_t err = 0;
    uint32_t got = 0;
    if (!o) {
        err = FILES_RF_NUM_ERR;
    } else {
        int mode = (int16_t)gm_r16(pb + PB_POS_MODE);
        if (mode & 0x80)
            trap_crash("PBReadSync: newline mode is not supported");
        err = set_pos(o->f, o->eof, mode, (int32_t)gm_r32(pb + PB_POS_OFFSET));
        int32_t want = (int32_t)gm_r32(pb + PB_REQ_COUNT);
        if (!err && want > 0) {
            uint8_t *buf = gm_ptr(gm_r32(pb + PB_BUFFER), (uint32_t)want);
            got = (uint32_t)fread(buf, 1, (size_t)want, o->f);
            if (got < (uint32_t)want)
                err = FILES_EOF_ERR;
        }
        gm_w32(pb + PB_POS_OFFSET, (uint32_t)ftell(o->f));
    }
    gm_w32(pb + PB_ACT_COUNT, got);
    gm_w16(pb + PB_RESULT, (uint16_t)err);
    trap_return((uint32_t)(int32_t)err);
}

/* FSWrite(short refNum, long *count, const void *buffer) -> OSErr: writes at
   the mark, extending the file as needed. *count becomes the bytes written. */
static void h_fs_write(void) {
    open_file *o = file_of((int16_t)trap_arg(0));
    uint32_t count_p = trap_arg(1);
    if (!o) {
        trap_return((uint32_t)FILES_RF_NUM_ERR);
        return;
    }
    int32_t want = (int32_t)gm_r32(count_p);
    int16_t err = want < 0 ? FILES_PARAM_ERR : make_writable("FSWrite", o);
    size_t put = 0;
    if (!err && want > 0) {
        fseek(o->f, 0, SEEK_CUR);
        put = fwrite(gm_ptr(trap_arg(2), (uint32_t)want), 1, (size_t)want, o->f);
        if (put < (size_t)want)
            err = FILES_IO_ERR;
        long mark = ftell(o->f);
        if (mark > o->eof)
            o->eof = mark;
    }
    gm_w32(count_p, (uint32_t)put);
    trap_return((uint32_t)(int32_t)err);
}

static void h_get_eof(void) {
    open_file *o = file_of((int16_t)trap_arg(0));
    if (!o) {
        trap_return((uint32_t)FILES_RF_NUM_ERR);
        return;
    }
    gm_w32(trap_arg(1), (uint32_t)o->eof);
    trap_return(0);
}

/* SetEOF(short refNum, long logEOF) -> OSErr: truncates or extends with
   zeros. A mark past the new end moves to it. */
static void h_set_eof(void) {
    open_file *o = file_of((int16_t)trap_arg(0));
    int32_t eof = (int32_t)trap_arg(1);
    if (!o) {
        trap_return((uint32_t)FILES_RF_NUM_ERR);
        return;
    }
    if (eof < 0) {
        trap_return((uint32_t)FILES_PARAM_ERR);
        return;
    }
    int16_t err = make_writable("SetEOF", o);
    if (!err) {
        long mark = ftell(o->f);
        fflush(o->f);
        if (ftruncate(fileno(o->f), eof) != 0) {
            err = FILES_IO_ERR;
        } else {
            o->eof = eof;
            fseek(o->f, mark > eof ? eof : mark, SEEK_SET);
        }
    }
    trap_return((uint32_t)(int32_t)err);
}

static void h_set_fpos(void) {
    open_file *o = file_of((int16_t)trap_arg(0));
    if (!o) {
        trap_return((uint32_t)FILES_RF_NUM_ERR);
        return;
    }
    trap_return((uint32_t)(int32_t)set_pos(o->f, o->eof, (int16_t)trap_arg(1), (int32_t)trap_arg(2)));
}

static void h_get_fpos(void) {
    open_file *o = file_of((int16_t)trap_arg(0));
    if (!o) {
        trap_return((uint32_t)FILES_RF_NUM_ERR);
        return;
    }
    gm_w32(trap_arg(1), (uint32_t)ftell(o->f));
    trap_return(0);
}

/* PBFlushFileSync(ParmBlkPtr) -> OSErr: pushes buffered writes to the host. */
static void h_pb_flush_file_sync(void) {
    uint32_t pb = trap_arg(0);
    open_file *o = file_of((int16_t)gm_r16(pb + PB_REFNUM));
    int16_t err = !o ? FILES_RF_NUM_ERR : fflush(o->f) != 0 ? FILES_IO_ERR : 0;
    gm_w16(pb + PB_RESULT, (uint16_t)err);
    trap_return((uint32_t)(int32_t)err);
}

/* FSGetForkSize(FSIORefNum, SInt64 *forkSize) -> OSErr */
static void h_fs_get_fork_size(void) {
    open_file *o = file_of((int16_t)trap_arg(0));
    if (!o) {
        trap_return((uint32_t)FILES_RF_NUM_ERR);
        return;
    }
    gm_w32(trap_arg(1), 0);
    gm_w32(trap_arg(1) + 4, (uint32_t)o->eof);
    trap_return(0);
}

/* FSGetForkPosition(FSIORefNum, SInt64 *position) -> OSErr */
static void h_fs_get_fork_position(void) {
    open_file *o = file_of((int16_t)trap_arg(0));
    if (!o) {
        trap_return((uint32_t)FILES_RF_NUM_ERR);
        return;
    }
    gm_w32(trap_arg(1), 0);
    gm_w32(trap_arg(1) + 4, (uint32_t)ftell(o->f));
    trap_return(0);
}

/* FSSetForkPosition(FSIORefNum, UInt16 positionMode, SInt64 positionOffset)
   -> OSErr. The 64-bit offset takes the next two registers, high word first
   (Darwin's ABI doesn't align it to an even register). */
static void h_fs_set_fork_position(void) {
    open_file *o = file_of((int16_t)trap_arg(0));
    if (!o) {
        trap_return((uint32_t)FILES_RF_NUM_ERR);
        return;
    }
    int64_t off = (int64_t)(((uint64_t)trap_arg(2) << 32) | trap_arg(3));
    if (off < INT32_MIN || off > INT32_MAX) {
        trap_return((uint32_t)FILES_POS_ERR);
        return;
    }
    trap_return((uint32_t)(int32_t)set_pos(o->f, o->eof, (int16_t)trap_arg(1), (int32_t)off));
}

static void h_fs_close(void) {
    int16_t ref = (int16_t)trap_arg(0);
    open_file *o = file_of(ref);
    if (!o) {
        trap_return((uint32_t)FILES_RF_NUM_ERR);
        return;
    }
    int failed = fclose(o->f);
    memset(o, 0, sizeof *o);
    trap_return(failed ? (uint32_t)FILES_IO_ERR : 0);
}

void files_register(void) {
    trap_register("FSMakeFSSpec", h_fs_make_fsspec);
    trap_register("FSpCreate", h_fsp_create);
    trap_register("FSpOpenDF", h_fsp_open_df);
    trap_register("PBReadSync", h_pb_read_sync);
    trap_register("FSWrite", h_fs_write);
    trap_register("GetEOF", h_get_eof);
    trap_register("SetEOF", h_set_eof);
    trap_register("PBFlushFileSync", h_pb_flush_file_sync);
    trap_register("SetFPos", h_set_fpos);
    trap_register("GetFPos", h_get_fpos);
    trap_register("FSClose", h_fs_close);
    trap_register("FSPathMakeRef", h_fs_path_make_ref);
    trap_register("FSGetDataForkName", h_fs_get_data_fork_name);
    trap_register("FSOpenFork", h_fs_open_fork);
    trap_register("FSGetForkSize", h_fs_get_fork_size);
    trap_register("FSGetForkPosition", h_fs_get_fork_position);
    trap_register("FSSetForkPosition", h_fs_set_fork_position);
    trap_register("FSCloseFork", h_fs_close);
}
