#include "test.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "files.h"
#include "harness.h"

static const char *const names[] = {
    "FSMakeFSSpec", "FSpOpenDF", "PBReadSync", "GetEOF", "SetFPos", "GetFPos", "FSClose",
    "FSpCreate", "FSWrite", "SetEOF", "PBFlushFileSync",
};

static char dir[1024], data[1100];

static void write_file(const char *rel, const char *text) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    FILE *f = fopen(p, "wb");
    fputs(text, f);
    fclose(f);
}

/* A fresh game folder, "game", with "LL Data/effect.bin" and "Café" (Mac
   Roman "Caf\x8e"), both read-only, and a data folder "data/sub" that
   doesn't exist yet. */
static void setup(void) {
    char root[1024];
    test_tmp_dir(root, sizeof root);
    snprintf(dir, sizeof dir, "%s/game", root);
    snprintf(data, sizeof data, "%s/data/sub", root);
    char sub[1100];
    snprintf(sub, sizeof sub, "%s/LL Data", dir);
    if (!make_dirs(sub))
        fatal("can't create %s", sub);
    write_file("LL Data/effect.bin", "hello world");
    write_file("Caf\xc3\xa9", "x");
    char p[1200];
    snprintf(p, sizeof p, "%s/LL Data/effect.bin", dir);
    chmod(p, 0444);
    harness_init(names, sizeof names / sizeof names[0]);
    files_init(dir, data);
    files_register();
}

static void teardown(void) {
    char root[1024];
    snprintf(root, sizeof root, "%s", dir);
    *strrchr(root, '/') = '\0';
    test_remove_tree(root);
}

/* The contents of a host file ("" if it doesn't exist). */
static const char *contents(const char *base, const char *rel) {
    static char buf[256];
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", base, rel);
    buf[0] = '\0';
    FILE *f = fopen(p, "rb");
    if (f) {
        buf[fread(buf, 1, sizeof buf - 1, f)] = '\0';
        fclose(f);
    }
    return buf;
}

static int16_t make_spec(const char *mac, uint32_t spec) {
    uint32_t name = scratch(256);
    gm_write_pstr(name, mac);
    return (int16_t)call_import("FSMakeFSSpec", 4, 0u, 0u, name, spec);
}

TEST(files_mac_roman_names_become_utf8) {
    char out[64];
    files_mac_to_utf8("Caf\x8e", out, sizeof out);
    CHECK_STR(out, "Caf\xc3\xa9");
    files_mac_to_utf8("a/b", out, sizeof out);
    CHECK_STR(out, "a:b");
    files_mac_to_utf8("\xaa", out, sizeof out); /* trademark sign, U+2122 */
    CHECK_STR(out, "\xe2\x84\xa2");
}

TEST(files_make_fsspec_resolves_relative_paths) {
    setup();
    uint32_t spec = scratch(FSSPEC_SIZE);
    CHECK_EQ(make_spec(":LL Data:effect.bin", spec), 0);
    CHECK_EQ((int16_t)gm_r16(spec), FILES_VREFNUM);
    CHECK(gm_r32(spec + 2) != FILES_ROOT_DIRID);
    char name[256];
    gm_read_pstr(spec + 6, name);
    CHECK_STR(name, "effect.bin");
    CHECK_EQ(make_spec("Caf\x8e", spec), 0);
    CHECK_EQ(gm_r32(spec + 2), FILES_ROOT_DIRID);
    CHECK_EQ(make_spec(":LL Data:nope", spec), FILES_FNF_ERR);
    gm_read_pstr(spec + 6, name);
    CHECK_STR(name, "nope"); /* the spec is still filled in */
    CHECK_EQ(make_spec(":Missing:x", spec), FILES_DIR_NF_ERR);
    teardown();
}

TEST(files_read_a_file) {
    setup();
    uint32_t spec = scratch(FSSPEC_SIZE), ref = scratch(2), eof = scratch(4);
    CHECK_EQ(make_spec(":LL Data:effect.bin", spec), 0);
    CHECK_EQ(call_import("FSpOpenDF", 3, spec, 1u, ref), 0);
    uint16_t r = gm_r16(ref);
    CHECK_EQ(call_import("GetEOF", 2, (uint32_t)r, eof), 0);
    CHECK_EQ(gm_r32(eof), 11);
    uint32_t pb = scratch(80), buf = scratch(16);
    gm_w16(pb + 24, r);
    gm_w32(pb + 32, buf);
    gm_w32(pb + 36, 5);
    gm_w16(pb + 44, 1); /* fsFromStart */
    gm_w32(pb + 46, 6);
    CHECK_EQ(call_import("PBReadSync", 1, pb), 0);
    CHECK_EQ(gm_r32(pb + 40), 5);
    CHECK_EQ(gm_r32(pb + 46), 11);
    char s[16];
    memcpy(s, gm_ptr(buf, 5), 5);
    s[5] = '\0';
    CHECK_STR(s, "world");
    gm_w16(pb + 44, 0); /* fsAtMark: nothing left */
    CHECK_EQ((int16_t)call_import("PBReadSync", 1, pb), FILES_EOF_ERR);
    CHECK_EQ(gm_r32(pb + 40), 0);
    CHECK_EQ((int16_t)gm_r16(pb + 16), FILES_EOF_ERR);
    CHECK_EQ(call_import("SetFPos", 3, (uint32_t)r, 1u, 2u), 0);
    uint32_t pos = scratch(4);
    CHECK_EQ(call_import("GetFPos", 2, (uint32_t)r, pos), 0);
    CHECK_EQ(gm_r32(pos), 2);
    CHECK_EQ((int16_t)call_import("SetFPos", 3, (uint32_t)r, 1u, 99u), FILES_EOF_ERR);
    CHECK_EQ(call_import("FSClose", 1, (uint32_t)r), 0);
    CHECK_EQ((int16_t)call_import("FSClose", 1, (uint32_t)r), FILES_RF_NUM_ERR);
    CHECK_EQ((int16_t)call_import("GetEOF", 2, (uint32_t)r, eof), FILES_RF_NUM_ERR);
    teardown();
}

TEST(files_open_missing_file) {
    setup();
    uint32_t spec = scratch(FSSPEC_SIZE), ref = scratch(2);
    make_spec("nope", spec);
    CHECK_EQ((int16_t)call_import("FSpOpenDF", 3, spec, 1u, ref), FILES_FNF_ERR);
    teardown();
}

static uint16_t open_df(const char *mac, int perm) {
    uint32_t spec = scratch(FSSPEC_SIZE), ref = scratch(2);
    make_spec(mac, spec);
    if (call_import("FSpOpenDF", 3, spec, (uint32_t)perm, ref) != 0)
        return 0;
    return gm_r16(ref);
}

static int16_t fs_write(uint16_t ref, const char *text) {
    uint32_t count = scratch(4), buf = scratch((uint32_t)strlen(text) + 1);
    gm_w32(count, (uint32_t)strlen(text));
    memcpy(gm_ptr(buf, (uint32_t)strlen(text) + 1), text, strlen(text));
    int16_t err = (int16_t)call_import("FSWrite", 3, (uint32_t)ref, count, buf);
    if (!err && gm_r32(count) != strlen(text))
        return 99;
    return err;
}

static uint32_t eof_of(uint16_t ref) {
    uint32_t eof = scratch(4);
    call_import("GetEOF", 2, (uint32_t)ref, eof);
    return gm_r32(eof);
}

/* Reads n bytes from offset off (fsFromStart). */
static const char *read_at(uint16_t ref, uint32_t off, uint32_t n) {
    static char out[64];
    uint32_t pb = scratch(80), buf = scratch(64);
    gm_w16(pb + 24, ref);
    gm_w32(pb + 32, buf);
    gm_w32(pb + 36, n);
    gm_w16(pb + 44, 1);
    gm_w32(pb + 46, off);
    call_import("PBReadSync", 1, pb);
    memcpy(out, gm_ptr(buf, 64), 63);
    out[gm_r32(pb + 40)] = '\0';
    return out;
}

TEST(files_create_write_and_read_back) {
    setup();
    uint32_t spec = scratch(FSSPEC_SIZE);
    CHECK_EQ(make_spec(":LL Data:scores", spec), FILES_FNF_ERR);
    CHECK_EQ(call_import("FSpCreate", 4, spec, 0x664C6F4Fu, 0x54455854u, 0u), 0);
    CHECK_STR(contents(data, "LL Data/scores"), ""); /* created, in the data folder */
    char p[1200];
    snprintf(p, sizeof p, "%s/LL Data/scores", data);
    CHECK(access(p, F_OK) == 0);
    snprintf(p, sizeof p, "%s/LL Data/scores", dir);
    CHECK(access(p, F_OK) != 0);
    CHECK_EQ(make_spec(":LL Data:scores", spec), 0);
    CHECK_EQ((int16_t)call_import("FSpCreate", 4, spec, 0u, 0u, 0u), FILES_DUP_FN_ERR);

    uint16_t r = open_df(":LL Data:scores", 3);
    CHECK(r != 0);
    CHECK_EQ(fs_write(r, "abcdef"), 0);
    CHECK_EQ(eof_of(r), 6);
    CHECK_STR(read_at(r, 2, 3), "cde");
    CHECK_EQ(fs_write(r, "XY"), 0); /* at the mark, after the read */
    CHECK_STR(read_at(r, 0, 10), "abcdeXY");
    CHECK_EQ(call_import("SetEOF", 2, (uint32_t)r, 2u), 0);
    CHECK_EQ(eof_of(r), 2);
    uint32_t pos = scratch(4);
    call_import("GetFPos", 2, (uint32_t)r, pos);
    CHECK_EQ(gm_r32(pos), 2);
    CHECK_EQ(call_import("SetEOF", 2, (uint32_t)r, 4u), 0); /* extends with zeros */
    CHECK_EQ(eof_of(r), 4);
    uint32_t pb = scratch(80);
    gm_w16(pb + 24, r);
    CHECK_EQ(call_import("PBFlushFileSync", 1, pb), 0);
    CHECK_EQ(call_import("FSClose", 1, (uint32_t)r), 0);
    snprintf(p, sizeof p, "%s/LL Data/scores", data);
    size_t len;
    uint8_t *got = read_file(p, &len);
    CHECK(got != NULL);
    CHECK_EQ(len, 4);
    CHECK(memcmp(got, "ab\0\0", 4) == 0);
    free(got);
    teardown();
}

/* Review Focus 2: the game folder is never modified. */
TEST(files_the_first_write_copies_a_game_file) {
    setup();
    uint16_t r = open_df(":LL Data:effect.bin", 0); /* fsCurPerm */
    CHECK(r != 0);
    CHECK_STR(read_at(r, 0, 5), "hello");
    CHECK_STR(contents(data, "LL Data/effect.bin"), ""); /* reading copies nothing */
    CHECK_EQ(fs_write(r, " W"), 0);
    CHECK_STR(read_at(r, 0, 20), "hello World");
    CHECK_EQ(eof_of(r), 11);
    CHECK_EQ(call_import("FSClose", 1, (uint32_t)r), 0);
    CHECK_STR(contents(dir, "LL Data/effect.bin"), "hello world");
    CHECK_STR(contents(data, "LL Data/effect.bin"), "hello World");
    r = open_df(":LL Data:effect.bin", 1); /* the copy now hides the original */
    CHECK_STR(read_at(r, 0, 20), "hello World");
    call_import("FSClose", 1, (uint32_t)r);
    teardown();
}

TEST(files_read_only_files_refuse_writes) {
    setup();
    uint16_t r = open_df(":LL Data:effect.bin", 1);
    CHECK(r != 0);
    CHECK_EQ(fs_write(r, "x"), FILES_WR_PERM_ERR);
    CHECK_EQ((int16_t)call_import("SetEOF", 2, (uint32_t)r, 0u), FILES_WR_PERM_ERR);
    CHECK_EQ(eof_of(r), 11);
    CHECK_EQ(call_import("FSClose", 1, (uint32_t)r), 0);
    char p[1200];
    snprintf(p, sizeof p, "%s", data);
    CHECK(access(p, F_OK) != 0); /* nothing was copied or created */
    teardown();
}

/* The children below run on the parent's setup(), so the parent cleans up. */
static void child_no_data_folder(void *unused) {
    (void)unused;
    files_init(dir, NULL);
    uint32_t spec = scratch(FSSPEC_SIZE);
    make_spec("new", spec);
    if ((int16_t)call_import("FSpCreate", 4, spec, 0u, 0u, 0u) != FILES_WR_PERM_ERR)
        exit(3);
    uint16_t r = open_df(":LL Data:effect.bin", 3);
    if (!r || strcmp(read_at(r, 0, 5), "hello") != 0)
        exit(4);
    if (fs_write(r, "x") != FILES_WR_PERM_ERR)
        exit(5);
}

TEST(files_without_a_writable_folder_writes_fail_cleanly) {
    setup();
    char out[16384];
    int status = test_run_child(child_no_data_folder, NULL, out, sizeof out);
    teardown();
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "FSpCreate: there is no writable folder");
    CHECK(!strstr(out, "FSWrite: there is no writable folder")); /* logged once */
}

/* Review Focus 3: names that would leave the game folder on the host. */
TEST(files_dot_names_and_double_colons) {
    setup();
    uint32_t spec = scratch(FSSPEC_SIZE);
    CHECK_EQ((int16_t)make_spec(":..:x", spec), FILES_BD_NAM_ERR);
    CHECK_EQ((int16_t)make_spec("..", spec), FILES_BD_NAM_ERR);
    CHECK_EQ((int16_t)make_spec("::x", spec), FILES_DIR_NF_ERR); /* above the game folder */
    CHECK_EQ(make_spec(":LL Data::Caf\x8e", spec), 0);
    CHECK_EQ(gm_r32(spec + 2), FILES_ROOT_DIRID);
    CHECK_EQ((int16_t)make_spec(":", spec), FILES_BD_NAM_ERR);
    teardown();
}

static void child_bad_permission(void *unused) {
    (void)unused;
    open_df(":LL Data:effect.bin", 9);
}

TEST(files_unknown_permissions_crash) {
    setup();
    char out[16384];
    int status = test_run_child(child_bad_permission, NULL, out, sizeof out);
    teardown();
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "FSpOpenDF: unknown permission 9");
}

static void child_full_path(void *unused) {
    (void)unused;
    make_spec("Macintosh HD:x", scratch(FSSPEC_SIZE));
}

TEST(files_full_paths_crash) {
    setup();
    char out[16384];
    int status = test_run_child(child_full_path, NULL, out, sizeof out);
    teardown();
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "full path names");
}
