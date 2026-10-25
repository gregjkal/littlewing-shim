#include "test.h"

#include <stdlib.h>
#include <string.h>

#include "files.h"
#include "harness.h"
#include "memmgr.h"
#include "rsrc.h"
#include "util.h"

static const char *const names[] = {
    "FSMakeFSSpec",    "GetResource",      "Get1Resource",  "GetNamedResource", "ReleaseResource",
    "DetachResource",  "ResError",         "CurResFile",    "UseResFile",       "FSpOpenResFile",
    "FSpCreateResFile", "CloseResFile",    "UpdateResFile", "WriteResource",    "ChangedResource",
    "AddResource",     "CountResources",   "GetIndResource", "GetResAttrs",     "SetResAttrs",
    "GetResInfo",      "SetResInfo",       "GetIndString",  "Get1NamedResource", "Count1Resources",
    "Get1IndResource", "GetPicture", "KillPicture", "RemoveResource",
    "OpenResFile", "OpenRFPerm", "CreateResFile",
};

#define TEXT FOURCC('T', 'E', 'X', 'T')
#define SND FOURCC('s', 'n', 'd', ' ')

static char dir[1024], data[1100];
static uint8_t *app_fork;

/* A resource file of n resources, each of type, ids from id, named by
   name (NULL for none), holding text. */
static uint8_t *build(uint32_t type, int16_t id, int n, const char *name, const char *text,
                      size_t *len) {
    rsrc_entry e[4] = {{0}};
    const uint8_t *d[4];
    for (int i = 0; i < n; i++) {
        e[i] = (rsrc_entry){.type = type, .id = (int16_t)(id + i), .name = (char *)name,
                            .len = (uint32_t)strlen(text)};
        d[i] = (const uint8_t *)text;
    }
    return rsrc_build(e, d, (uint32_t)n, len);
}

static void put(const char *path, const void *bytes, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(bytes, 1, len, f) != len || fclose(f) != 0)
        fatal("can't write %s", path);
}

/* rel in the game folder: a data fork, and a resource fork of two TEXT
   resources, 128 and 129, named "two" and holding "from file". */
static void game_file(const char *rel) {
    char p[1200], fork[1300];
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    put(p, "data", 4);
    size_t len;
    uint8_t *b = build(TEXT, 128, 2, "two", "from file", &len);
    snprintf(fork, sizeof fork, "%s/..namedfork/rsrc", p);
    put(fork, b, len);
    free(b);
}

/* A host file's data or resource fork: whether it holds text, and how
   long it is (-1 if the file is missing). */
static uint8_t *host_fork(const char *base, const char *rel, bool rsrc, size_t *len) {
    char p[1300];
    snprintf(p, sizeof p, "%s/%s%s", base, rel, rsrc ? "/..namedfork/rsrc" : "");
    *len = 0;
    return read_file(p, len);
}

static bool host_has(const char *base, const char *rel, bool rsrc, const char *text) {
    size_t len;
    uint8_t *b = host_fork(base, rel, rsrc, &len);
    bool has = b && memmem(b, len, text, strlen(text)) != NULL;
    free(b);
    return has;
}

static long host_len(const char *base, const char *rel, bool rsrc) {
    size_t len;
    uint8_t *b = host_fork(base, rel, rsrc, &len);
    free(b);
    return b ? (long)len : -1;
}

/* A game folder holding "Machine" (game_file), an application file with
   a STR# 128 and a TEXT 128 holding "from app", and a data folder that
   doesn't exist yet. */
static void setup(void) {
    char root[1024];
    test_tmp_dir(root, sizeof root);
    snprintf(dir, sizeof dir, "%s/game", root);
    snprintf(data, sizeof data, "%s/data", root);
    if (!make_dirs(dir))
        fatal("can't create %s", dir);
    game_file("Machine");
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    files_init(dir, data);
    files_register();
    static const uint8_t strs[] = {0, 1, 3, 'a', 'b', 'c'};
    rsrc_entry e[2] = {{.type = FOURCC('S', 'T', 'R', '#'), .id = 128, .len = sizeof strs},
                       {.type = TEXT, .id = 128, .len = 8}};
    const uint8_t *d[2] = {strs, (const uint8_t *)"from app"};
    size_t len;
    free(app_fork);
    app_fork = rsrc_build(e, d, 2, &len);
    char err[256];
    if (!rsrc_open(app_fork, len, err, sizeof err))
        fatal("can't open the application's file: %s", err);
    rsrc_register();
}

static void teardown(void) {
    rsrc_close();
    char root[1024];
    snprintf(root, sizeof root, "%s", dir);
    *strrchr(root, '/') = '\0';
    test_remove_tree(root);
}

static uint32_t spec(const char *name) {
    uint32_t s = scratch(FSSPEC_SIZE), n = scratch(64);
    gm_write_pstr(n, name);
    call_import("FSMakeFSSpec", 4, 0u, 0u, n, s);
    return s;
}

static uint32_t pstr(const char *s) {
    uint32_t p = scratch(256);
    gm_write_pstr(p, s);
    return p;
}

static int16_t res_error(void) { return (int16_t)call_import("ResError", 0); }
static int16_t open_res(const char *name, int perm) {
    return (int16_t)call_import("FSpOpenResFile", 2, spec(name), (uint32_t)perm);
}

/* What the handle h holds, as a string. */
static const char *text_of(uint32_t h) {
    static char buf[256];
    uint32_t n = mm_handle_size(h);
    memcpy(buf, gm_ptr(gm_r32(h), n), n);
    buf[n] = '\0';
    return buf;
}

TEST(rsrcfile_build_round_trips) {
    rsrc_entry e[3] = {{.type = TEXT, .id = 5, .name = "five", .attrs = RSRC_ATTR_PURGEABLE, .len = 3},
                       {.type = SND, .id = -2, .len = 0},
                       {.type = TEXT, .id = 6, .attrs = RSRC_ATTR_CHANGED, .len = 2}};
    const uint8_t *d[3] = {(const uint8_t *)"abc", NULL, (const uint8_t *)"xy"};
    size_t len;
    uint8_t *b = rsrc_build(e, d, 3, &len);
    char err[256];
    CHECK(rsrc_open(b, len, err, sizeof err));
    CHECK_EQ(rsrc_type_count(), 2);
    CHECK_EQ(rsrc_total(), 3);
    rsrc_entry *five = rsrc_find(TEXT, 5), *six = rsrc_find(TEXT, 6), *snd = rsrc_find(SND, -2);
    CHECK(five && six && snd);
    CHECK_EQ(five->len, 3);
    CHECK(memcmp(rsrc_data(five), "abc", 3) == 0);
    CHECK_STR(five->name, "five");
    CHECK_EQ(five->attrs, RSRC_ATTR_PURGEABLE);
    CHECK(memcmp(rsrc_data(six), "xy", 2) == 0);
    CHECK_EQ(six->attrs, 0); /* resChanged isn't written */
    CHECK(six->name == NULL);
    CHECK_EQ(snd->len, 0);
    CHECK(rsrc_find_named(TEXT, "FIVE") == five);
    rsrc_close();
    free(b);

    b = rsrc_build(NULL, NULL, 0, &len);
    CHECK(rsrc_open(b, len, err, sizeof err));
    CHECK_EQ(rsrc_type_count(), 0);
    CHECK_EQ(rsrc_total(), 0);
    rsrc_close();
    free(b);
}

TEST(rsrcfile_an_opened_file_is_searched_first_then_the_application) {
    setup();
    CHECK_EQ((int16_t)call_import("CurResFile", 0), RSRC_APP_REFNUM);
    int16_t ref = open_res("Machine", 1);
    CHECK(ref > 0);
    CHECK_EQ(res_error(), 0);
    CHECK_EQ((int16_t)call_import("CurResFile", 0), ref);
    CHECK_STR(text_of(call_import("GetResource", 2, TEXT, 128u)), "from file");
    CHECK_STR(text_of(call_import("GetResource", 2, TEXT, 129u)), "from file");
    CHECK(call_import("GetResource", 2, FOURCC('S', 'T', 'R', '#'), 128u) != 0); /* the application's */
    CHECK_EQ(call_import("Get1Resource", 2, FOURCC('S', 'T', 'R', '#'), 128u), 0);
    CHECK_EQ(res_error(), RSRC_NOT_FOUND_ERR);
    CHECK(call_import("GetNamedResource", 2, TEXT, pstr("TWO")) != 0);
    CHECK(rsrc_find(TEXT, 129) != NULL);

    call_import("UseResFile", 1, (uint32_t)RSRC_APP_REFNUM);
    CHECK_STR(text_of(call_import("GetResource", 2, TEXT, 128u)), "from app");
    CHECK_EQ(call_import("GetResource", 2, TEXT, 129u), 0);
    call_import("UseResFile", 1, (uint32_t)RSRC_SYSTEM_REFNUM);
    CHECK_EQ((int16_t)call_import("CurResFile", 0), RSRC_SYSTEM_REFNUM);
    CHECK_EQ(call_import("GetResource", 2, TEXT, 128u), 0);
    call_import("UseResFile", 1, 999u);
    CHECK_EQ(res_error(), RSRC_FILE_NOT_FOUND_ERR);
    CHECK_EQ((int16_t)call_import("CurResFile", 0), RSRC_SYSTEM_REFNUM);

    CHECK_EQ(open_res("Machine", 1), ref); /* open already: the same refnum */
    CHECK_EQ((int16_t)call_import("CurResFile", 0), RSRC_SYSTEM_REFNUM);
    teardown();
}

TEST(rsrcfile_counts_and_indexes_every_open_file) {
    setup();
    CHECK_EQ(call_import("CountResources", 1, TEXT), 1);
    int16_t ref = open_res("Machine", 1);
    CHECK_EQ(call_import("CountResources", 1, TEXT), 3);
    CHECK_STR(text_of(call_import("GetIndResource", 2, TEXT, 1u)), "from file");
    CHECK_STR(text_of(call_import("GetIndResource", 2, TEXT, 3u)), "from app");
    CHECK_EQ(call_import("GetIndResource", 2, TEXT, 4u), 0);
    CHECK_EQ(res_error(), RSRC_NOT_FOUND_ERR);
    CHECK_EQ(call_import("GetIndResource", 2, TEXT, 0u), 0);
    call_import("CloseResFile", 1, (uint32_t)ref);
    CHECK_EQ(call_import("CountResources", 1, TEXT), 1);
    teardown();
}

TEST(rsrcfile_missing_files_and_forks_fail_to_open) {
    setup();
    CHECK_EQ(open_res("Nowhere", 1), -1);
    CHECK_EQ(res_error(), FILES_FNF_ERR);
    char p[1200];
    snprintf(p, sizeof p, "%s/Plain", dir);
    put(p, "x", 1);
    CHECK_EQ(open_res("Plain", 1), -1);
    CHECK_EQ(res_error(), FILES_EOF_ERR);
    char fork[1300];
    snprintf(fork, sizeof fork, "%s/..namedfork/rsrc", p);
    put(fork, "junk", 4);
    CHECK_EQ(open_res("Plain", 1), -1);
    CHECK_EQ(res_error(), RSRC_MAP_READ_ERR);
    CHECK_EQ((int16_t)call_import("CurResFile", 0), RSRC_APP_REFNUM);
    teardown();
}

TEST(rsrcfile_closing_gives_back_handles_and_the_file_below) {
    setup();
    int16_t ref = open_res("Machine", 1);
    uint32_t h = call_import("GetResource", 2, TEXT, 128u);
    CHECK(mm_is_handle(h));
    call_import("CloseResFile", 1, (uint32_t)ref);
    CHECK_EQ(res_error(), 0);
    CHECK(!mm_is_handle(h));
    CHECK_EQ((int16_t)call_import("CurResFile", 0), RSRC_APP_REFNUM);
    CHECK_STR(text_of(call_import("GetResource", 2, TEXT, 128u)), "from app");
    call_import("CloseResFile", 1, (uint32_t)ref);
    CHECK_EQ(res_error(), RSRC_FILE_NOT_FOUND_ERR);
    call_import("CloseResFile", 1, (uint32_t)RSRC_APP_REFNUM); /* stays open */
    CHECK_EQ(res_error(), 0);
    CHECK_STR(text_of(call_import("GetResource", 2, TEXT, 128u)), "from app");
    teardown();
}

TEST(rsrcfile_a_created_file_saves_what_is_added_in_the_data_folder) {
    setup();
    uint32_t s = spec("Prefs");
    call_import("FSpCreateResFile", 4, s, FOURCC('f', 'A', 'E', 'g'), FOURCC('p', 'r', 'e', 'f'), 0u);
    CHECK_EQ(res_error(), 0);
    call_import("FSpCreateResFile", 4, s, 0u, 0u, 0u);
    CHECK_EQ(res_error(), FILES_DUP_FN_ERR);
    int16_t ref = (int16_t)call_import("FSpOpenResFile", 2, s, 3u);
    CHECK(ref > 0);
    CHECK_EQ(call_import("CountResources", 1, SND), 0);

    uint32_t h = mm_new_handle(5, false);
    memcpy(gm_ptr(gm_r32(h), 5), "saved", 5);
    call_import("AddResource", 4, h, SND, 7u, pstr("seven"));
    CHECK_EQ(res_error(), 0);
    CHECK(mm_handle_state(h) & MM_STATE_RESOURCE);
    CHECK_EQ(call_import("GetResAttrs", 1, h), RSRC_ATTR_CHANGED);
    CHECK_EQ(call_import("GetResource", 2, SND, 7u), h);
    call_import("AddResource", 4, h, SND, 8u, 0u); /* a resource already */
    CHECK_EQ(res_error(), RSRC_ADD_FAILED_ERR);
    call_import("ReleaseResource", 1, h); /* changed: it stays */
    CHECK_EQ(res_error(), RSRC_ATTR_ERR);
    CHECK(mm_is_handle(h));
    call_import("CloseResFile", 1, (uint32_t)ref);
    CHECK_EQ(res_error(), 0);
    CHECK_EQ(host_len(dir, "Prefs", false), -1);
    CHECK_EQ(host_len(data, "Prefs", false), 0);

    ref = (int16_t)call_import("FSpOpenResFile", 2, spec("Prefs"), 1u);
    uint32_t again = call_import("Get1Resource", 2, SND, 7u);
    CHECK_STR(text_of(again), "saved");
    CHECK_EQ(call_import("GetResAttrs", 1, again), 0);
    uint32_t id = scratch(2), type = scratch(4), name = scratch(256);
    call_import("GetResInfo", 4, again, id, type, name);
    CHECK_EQ((int16_t)gm_r16(id), 7);
    CHECK_EQ(gm_r32(type), SND);
    char got[256];
    gm_read_pstr(name, got);
    CHECK_STR(got, "seven");
    teardown();
}

TEST(rsrcfile_a_change_to_a_game_file_goes_to_a_copy) {
    setup();
    int16_t ref = open_res("Machine", 3);
    uint32_t h = call_import("GetResource", 2, TEXT, 129u);
    mm_set_handle_size(h, 7);
    memcpy(gm_ptr(gm_r32(h), 7), "changed", 7);
    call_import("ChangedResource", 1, h);
    call_import("SetResInfo", 3, h, 300u, pstr("renamed"));
    call_import("UpdateResFile", 1, (uint32_t)ref);
    CHECK_EQ(res_error(), 0);
    CHECK(host_has(data, "Machine", false, "data")); /* both forks copied */
    CHECK(host_has(data, "Machine", true, "changed"));
    CHECK(!host_has(dir, "Machine", true, "changed"));
    CHECK_EQ(call_import("GetResAttrs", 1, h), 0);
    call_import("CloseResFile", 1, (uint32_t)ref);

    ref = open_res("Machine", 1);
    CHECK_STR(text_of(call_import("Get1Resource", 2, TEXT, 300u)), "changed");
    CHECK_STR(text_of(call_import("Get1Resource", 2, TEXT, 128u)), "from file");
    CHECK(call_import("GetNamedResource", 2, TEXT, pstr("renamed")) != 0);
    CHECK_EQ(call_import("Get1Resource", 2, TEXT, 129u), 0);
    teardown();
}

TEST(rsrcfile_write_resource_and_read_only_files) {
    setup();
    int16_t ref = open_res("Machine", 1);
    uint32_t h = call_import("GetResource", 2, TEXT, 128u);
    call_import("WriteResource", 1, h); /* unchanged: nothing to write */
    CHECK_EQ(res_error(), 0);
    call_import("ChangedResource", 1, h);
    call_import("WriteResource", 1, h);
    CHECK_EQ(res_error(), FILES_WR_PERM_ERR);
    call_import("CloseResFile", 1, (uint32_t)ref);
    CHECK_EQ(host_len(data, "Machine", false), -1);

    ref = open_res("Machine", 3);
    h = call_import("GetResource", 2, TEXT, 128u);
    memcpy(gm_ptr(gm_r32(h), 4), "FROM", 4);
    call_import("ChangedResource", 1, h);
    call_import("WriteResource", 1, h);
    CHECK_EQ(res_error(), 0);
    CHECK(host_has(data, "Machine", true, "FROM file"));
    CHECK(host_has(dir, "Machine", true, "from file"));
    teardown();
}

/* A System 7 program opens and makes resource files by name. */
TEST(rsrcfile_open_and_create_by_name) {
    setup();
    CHECK_EQ((int16_t)call_import("OpenResFile", 1, pstr("External Sound")), -1);
    CHECK_EQ(res_error(), FILES_FNF_ERR);
    int16_t ref = (int16_t)call_import("OpenResFile", 1, pstr("Machine"));
    CHECK(ref > RSRC_APP_REFNUM);
    CHECK_STR(text_of(call_import("Get1Resource", 2, TEXT, 129u)), "from file");
    CHECK_EQ((int16_t)call_import("OpenRFPerm", 3, pstr("Machine"), 0u, 1u), ref);
    call_import("CloseResFile", 1, (uint32_t)ref);
    CHECK_EQ((int16_t)call_import("OpenRFPerm", 3, pstr(":Nowhere:x"), 0u, 1u), -1);
    CHECK_EQ(res_error(), FILES_DIR_NF_ERR);

    call_import("CreateResFile", 1, pstr("Saved"));
    CHECK_EQ(res_error(), 0);
    CHECK(host_len(data, "Saved", true) > 0);
    call_import("CreateResFile", 1, pstr("Saved"));
    CHECK_EQ(res_error(), FILES_DUP_FN_ERR);
    ref = (int16_t)call_import("OpenRFPerm", 3, pstr("Saved"), (uint32_t)(int32_t)FILES_VREFNUM, 3u);
    CHECK(ref > RSRC_APP_REFNUM);
    CHECK_EQ(call_import("Count1Resources", 1, TEXT), 0);
    teardown();
}

TEST(rsrcfile_remove_resource_writes_the_file_without_it) {
    setup();
    int16_t ref = open_res("Machine", 3);
    uint32_t h = call_import("GetResource", 2, TEXT, 129u);
    call_import("RemoveResource", 1, h);
    CHECK_EQ(res_error(), 0);
    CHECK(mm_is_handle(h)); /* the game's still */
    CHECK_EQ(call_import("GetResource", 2, TEXT, 129u), 0);
    call_import("RemoveResource", 1, h);
    CHECK_EQ(res_error(), RSRC_NOT_FOUND_ERR);
    call_import("CloseResFile", 1, (uint32_t)ref);
    ref = open_res("Machine", 1);
    CHECK_EQ(call_import("Get1Resource", 2, TEXT, 129u), 0);
    CHECK_STR(text_of(call_import("Get1Resource", 2, TEXT, 128u)), "from file");
    teardown();
}

/* Tristan saves into its own resource fork: once named, the
   application's file is written to a copy in the data folder, which is
   read the next time, and the game folder's is never changed. */
TEST(rsrcfile_the_application_file_is_saved_in_the_data_folder) {
    setup();
    rsrc_app_file("Machine");
    CHECK_STR(text_of(call_import("GetResource", 2, TEXT, 128u)), "from file");
    uint32_t h = call_import("GetResource", 2, TEXT, 129u);
    memcpy(gm_ptr(gm_r32(h), 4), "FROM", 4);
    call_import("ChangedResource", 1, h);
    call_import("RemoveResource", 1, call_import("GetResource", 2, TEXT, 128u));
    call_import("UpdateResFile", 1, (uint32_t)RSRC_APP_REFNUM);
    CHECK_EQ(res_error(), 0);
    CHECK(host_has(data, "Machine", true, "FROM file"));
    CHECK(host_has(dir, "Machine", true, "from file"));
    CHECK(!host_has(dir, "Machine", true, "FROM file"));

    rsrc_open_empty(); /* the next launch */
    rsrc_app_file("Machine");
    CHECK_STR(text_of(call_import("GetResource", 2, TEXT, 129u)), "FROM file");
    CHECK_EQ(call_import("GetResource", 2, TEXT, 128u), 0);
    teardown();
}

TEST(rsrcfile_a_missing_application_file_keeps_the_resources) {
    setup();
    rsrc_app_file("Nothing");
    CHECK_STR(text_of(call_import("GetResource", 2, TEXT, 128u)), "from app");
    teardown();
}

TEST(rsrcfile_detach_and_attributes) {
    setup();
    open_res("Machine", 3);
    uint32_t h = call_import("GetResource", 2, TEXT, 128u);
    call_import("SetResAttrs", 2, h, (uint32_t)(RSRC_ATTR_PURGEABLE | RSRC_ATTR_CHANGED));
    CHECK_EQ(call_import("GetResAttrs", 1, h), RSRC_ATTR_PURGEABLE); /* resChanged is ChangedResource's */
    call_import("DetachResource", 1, h);
    CHECK_EQ(res_error(), 0);
    CHECK(mm_is_handle(h));
    CHECK(!(mm_handle_state(h) & MM_STATE_RESOURCE));
    CHECK_EQ(call_import("GetResAttrs", 1, h), 0);
    CHECK_EQ(res_error(), RSRC_NOT_FOUND_ERR);
    uint32_t fresh = call_import("GetResource", 2, TEXT, 128u);
    CHECK(fresh && fresh != h);
    call_import("ChangedResource", 1, fresh);
    call_import("DetachResource", 1, fresh);
    CHECK_EQ(res_error(), RSRC_ATTR_ERR);
    CHECK_EQ(call_import("GetResource", 2, TEXT, 128u), fresh);
    teardown();
}

TEST(rsrcfile_changes_are_saved_at_exit) {
    setup();
    int16_t ref = open_res("Machine", 3);
    uint32_t h = mm_new_handle(3, false);
    memcpy(gm_ptr(gm_r32(h), 3), "new", 3);
    call_import("AddResource", 4, h, SND, 1u, 0u);
    CHECK_EQ(host_len(data, "Machine", true), -1);
    rsrc_update_all();
    CHECK(host_has(data, "Machine", true, "new"));
    call_import("UpdateResFile", 1, (uint32_t)ref);
    CHECK_EQ(res_error(), 0);
    teardown();
}

TEST(rsrcfile_the_current_file_alone) {
    setup();
    open_res("Machine", 1);
    CHECK(call_import("Get1NamedResource", 2, TEXT, pstr("Two")) != 0);
    CHECK_EQ(call_import("Count1Resources", 1, TEXT), 2);
    CHECK_STR(text_of(call_import("Get1IndResource", 2, TEXT, 2u)), "from file");
    CHECK_EQ(call_import("Get1IndResource", 2, TEXT, 3u), 0);
    CHECK_EQ(res_error(), RSRC_NOT_FOUND_ERR);
    call_import("UseResFile", 1, (uint32_t)RSRC_APP_REFNUM);
    CHECK_EQ(call_import("Get1NamedResource", 2, TEXT, pstr("two")), 0);
    CHECK_EQ(res_error(), RSRC_NOT_FOUND_ERR);
    CHECK_EQ(call_import("Count1Resources", 1, TEXT), 1);
    CHECK_STR(text_of(call_import("Get1IndResource", 2, TEXT, 1u)), "from app");
    teardown();
}

TEST(rsrcfile_pictures_come_from_pict_resources) {
    setup();
    rsrc_close();
    rsrc_entry e[1] = {{.type = FOURCC('P', 'I', 'C', 'T'), .id = 128, .len = 4}};
    const uint8_t *d[1] = {(const uint8_t *)"pict"};
    size_t len;
    free(app_fork);
    app_fork = rsrc_build(e, d, 1, &len);
    char err[256];
    CHECK(rsrc_open(app_fork, len, err, sizeof err));
    uint32_t h = call_import("GetPicture", 1, 128u);
    CHECK_STR(text_of(h), "pict");
    CHECK_EQ(call_import("GetPicture", 1, 128u), h);
    CHECK_EQ(call_import("GetPicture", 1, 129u), 0);
    CHECK_EQ(res_error(), RSRC_NOT_FOUND_ERR);
    call_import("KillPicture", 1, h);
    CHECK(!mm_is_handle(h));
    uint32_t again = call_import("GetPicture", 1, 128u);
    CHECK(mm_is_handle(again));
    uint32_t mine = mm_new_handle(4, false);
    call_import("KillPicture", 1, mine);
    CHECK(!mm_is_handle(mine));
    teardown();
}
