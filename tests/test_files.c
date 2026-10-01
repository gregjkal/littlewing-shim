#include "test.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "files.h"
#include "harness.h"

static const char *const names[] = {
    "FSMakeFSSpec", "FSpOpenDF", "PBReadSync", "GetEOF", "SetFPos", "GetFPos", "FSClose",
};

static char dir[1024];

static void write_file(const char *rel, const char *text) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    FILE *f = fopen(p, "wb");
    fputs(text, f);
    fclose(f);
}

/* A fresh game folder: "LL Data/effect.bin" and "Café" (Mac Roman "Caf\x8e"). */
static void setup(void) {
    const char *t = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/loony-files-XXXXXX", t && *t ? t : "/tmp");
    if (!mkdtemp(dir))
        fatal("mkdtemp failed");
    char sub[1100];
    snprintf(sub, sizeof sub, "%s/LL Data", dir);
    mkdir(sub, 0755);
    write_file("LL Data/effect.bin", "hello world");
    write_file("Caf\xc3\xa9", "x");
    harness_init(names, sizeof names / sizeof names[0]);
    files_init(dir);
    files_register();
}

static void teardown(void) {
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "can't remove %s\n", dir);
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

static void child_open_for_writing(void *unused) {
    (void)unused;
    setup();
    uint32_t spec = scratch(FSSPEC_SIZE), ref = scratch(2);
    make_spec(":LL Data:effect.bin", spec);
    call_import("FSpOpenDF", 3, spec, 3u, ref);
}

TEST(files_writing_is_not_supported_yet) {
    char out[16384];
    CHECK_EQ(test_run_child(child_open_for_writing, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "FSpOpenDF: opening files for writing (permission 3) is not supported yet");
    teardown();
}

static void child_full_path(void *unused) {
    (void)unused;
    setup();
    make_spec("Macintosh HD:x", scratch(FSSPEC_SIZE));
}

TEST(files_full_paths_crash) {
    char out[16384];
    CHECK_EQ(test_run_child(child_full_path, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "full path names");
    teardown();
}
