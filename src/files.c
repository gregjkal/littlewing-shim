#include "files.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "guest_mem.h"
#include "trap.h"
#include "util.h"

#define MAX_DIRS 64
#define MAX_FILES 16
#define FIRST_REFNUM 20

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

static struct {
    char game_dir[1024];
    char dirs[MAX_DIRS][512]; /* relative host path of each directory ID - 2 ("" = game folder) */
    int ndirs;
    struct {
        FILE *f;
        long eof;
    } files[MAX_FILES];
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

void files_init(const char *game_dir) {
    for (int i = 0; i < MAX_FILES; i++)
        if (F.files[i].f)
            fclose(F.files[i].f);
    memset(&F, 0, sizeof F);
    snprintf(F.game_dir, sizeof F.game_dir, "%s", game_dir);
    F.ndirs = 1; /* ID 2: the game folder */
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

static void host_path(const char *rel, char *out, size_t cap) {
    snprintf(out, cap, "%s%s%s", F.game_dir, *rel ? "/" : "", rel);
}

static bool is_dir(const char *rel) {
    char p[1600];
    host_path(rel, p, sizeof p);
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Joins a relative directory and a UTF-8 component. */
static void join(const char *dir, const char *name, char *out, size_t cap) {
    snprintf(out, cap, "%s%s%s", dir, *dir ? "/" : "", name);
}

/* FSMakeFSSpec(vRefNum, dirID, fileName, FSSpec *spec) -> OSErr */
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
    /* Walk ':'-separated components; a leading ':' means relative. */
    char rel[512], comp[256], utf[512];
    snprintf(rel, sizeof rel, "%s", base);
    const char *p = mac[0] == ':' ? mac + 1 : mac;
    if (strchr(mac, ':') && mac[0] != ':')
        trap_crash("FSMakeFSSpec: full path names (\"%s\") are not supported", mac);
    for (;;) {
        const char *colon = strchr(p, ':');
        size_t n = colon ? (size_t)(colon - p) : strlen(p);
        if (n == 0 || n > 63)
            trap_crash("FSMakeFSSpec: bad path \"%s\"", mac);
        memcpy(comp, p, n);
        comp[n] = '\0';
        if (!colon)
            break;
        files_mac_to_utf8(comp, utf, sizeof utf);
        char next[512];
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
    char file_rel[1024], hp[1600];
    join(rel, utf, file_rel, sizeof file_rel);
    host_path(file_rel, hp, sizeof hp);
    struct stat st;
    trap_return(stat(hp, &st) == 0 ? 0 : (uint32_t)FILES_FNF_ERR);
}

static void spec_host_path(uint32_t spec, char *out, size_t cap) {
    uint32_t dir = gm_r32(spec + 2);
    const char *base = dir_path(dir);
    if (!base)
        trap_crash("FSSpec has an unknown directory ID %u", dir);
    char mac[256], utf[512], rel[1024];
    gm_read_pstr(spec + 6, mac);
    files_mac_to_utf8(mac, utf, sizeof utf);
    join(base, utf, rel, sizeof rel);
    host_path(rel, out, cap);
}

/* FSpOpenDF(const FSSpec *spec, SInt8 permission, short *refNum) -> OSErr */
static void h_fsp_open_df(void) {
    uint32_t spec = trap_arg(0), out = trap_arg(2);
    int perm = (int8_t)trap_arg(1);
    if (perm != 0 && perm != 1)
        trap_crash("FSpOpenDF: opening files for writing (permission %d) is not supported yet", perm);
    char path[1600];
    spec_host_path(spec, path, sizeof path);
    int slot = 0;
    while (slot < MAX_FILES && F.files[slot].f)
        slot++;
    if (slot == MAX_FILES)
        trap_crash("FSpOpenDF: more than %d open files", MAX_FILES);
    FILE *f = fopen(path, "rb");
    if (!f) {
        trap_return((uint32_t)FILES_FNF_ERR);
        return;
    }
    fseek(f, 0, SEEK_END);
    F.files[slot].f = f;
    F.files[slot].eof = ftell(f);
    fseek(f, 0, SEEK_SET);
    gm_w16(out, (uint16_t)(FIRST_REFNUM + slot));
    trap_return(0);
}

static FILE *file_of(int16_t ref, long *eof) {
    int slot = ref - FIRST_REFNUM;
    if (slot < 0 || slot >= MAX_FILES || !F.files[slot].f)
        return NULL;
    *eof = F.files[slot].eof;
    return F.files[slot].f;
}

/* Moves the mark; returns an OSErr. */
static int16_t set_pos(FILE *f, long eof, int mode, int32_t off) {
    long base;
    switch (mode & 3) {
    case FS_AT_MARK: return 0;
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
    long eof;
    FILE *f = file_of((int16_t)gm_r16(pb + PB_REFNUM), &eof);
    int16_t err = 0;
    uint32_t got = 0;
    if (!f) {
        err = FILES_RF_NUM_ERR;
    } else {
        int mode = (int16_t)gm_r16(pb + PB_POS_MODE);
        if (mode & 0x80)
            trap_crash("PBReadSync: newline mode is not supported");
        err = set_pos(f, eof, mode, (int32_t)gm_r32(pb + PB_POS_OFFSET));
        int32_t want = (int32_t)gm_r32(pb + PB_REQ_COUNT);
        if (!err && want > 0) {
            uint8_t *buf = gm_ptr(gm_r32(pb + PB_BUFFER), (uint32_t)want);
            got = (uint32_t)fread(buf, 1, (size_t)want, f);
            if (got < (uint32_t)want)
                err = FILES_EOF_ERR;
        }
        gm_w32(pb + PB_POS_OFFSET, (uint32_t)ftell(f));
    }
    gm_w32(pb + PB_ACT_COUNT, got);
    gm_w16(pb + PB_RESULT, (uint16_t)err);
    trap_return((uint32_t)(int32_t)err);
}

static void h_get_eof(void) {
    long eof;
    FILE *f = file_of((int16_t)trap_arg(0), &eof);
    if (!f) {
        trap_return((uint32_t)FILES_RF_NUM_ERR);
        return;
    }
    gm_w32(trap_arg(1), (uint32_t)eof);
    trap_return(0);
}

static void h_set_fpos(void) {
    long eof;
    FILE *f = file_of((int16_t)trap_arg(0), &eof);
    if (!f) {
        trap_return((uint32_t)FILES_RF_NUM_ERR);
        return;
    }
    trap_return((uint32_t)(int32_t)set_pos(f, eof, (int16_t)trap_arg(1), (int32_t)trap_arg(2)));
}

static void h_get_fpos(void) {
    long eof;
    FILE *f = file_of((int16_t)trap_arg(0), &eof);
    if (!f) {
        trap_return((uint32_t)FILES_RF_NUM_ERR);
        return;
    }
    gm_w32(trap_arg(1), (uint32_t)ftell(f));
    trap_return(0);
}

static void h_fs_close(void) {
    int16_t ref = (int16_t)trap_arg(0);
    long eof;
    FILE *f = file_of(ref, &eof);
    if (!f) {
        trap_return((uint32_t)FILES_RF_NUM_ERR);
        return;
    }
    fclose(f);
    F.files[ref - FIRST_REFNUM].f = NULL;
    trap_return(0);
}

void files_register(void) {
    trap_register("FSMakeFSSpec", h_fs_make_fsspec);
    trap_register("FSpOpenDF", h_fsp_open_df);
    trap_register("PBReadSync", h_pb_read_sync);
    trap_register("GetEOF", h_get_eof);
    trap_register("SetFPos", h_set_fpos);
    trap_register("GetFPos", h_get_fpos);
    trap_register("FSClose", h_fs_close);
}
