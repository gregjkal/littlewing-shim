#include "test.h"

#include <stdlib.h>

#include "harness.h"
#include "memmgr.h"
#include "rsrc.h"

static const char *const names[] = {
    "GetResource", "GetNamedResource", "LoadResource", "ReleaseResource",
    "ResError", "CurResFile", "GetIndString",
};

static uint8_t *fork_buf;
static size_t fork_len;

/* Fresh machine and heap, and the fork reopened so no handle is loaded yet.
   Returns false if the game isn't present. */
static bool setup(void) {
    if (!test_game_present())
        return false;
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    if (!fork_buf) {
        char path[1100];
        snprintf(path, sizeof path, "%s/..namedfork/rsrc", test_game_exe_path());
        fork_buf = read_file(path, &fork_len);
    }
    char err[256];
    if (!fork_buf || !rsrc_open(fork_buf, fork_len, err, sizeof err))
        fatal("can't open the resource fork");
    rsrc_register();
    return true;
}

static int16_t res_error(void) { return (int16_t)call_import("ResError", 0); }

TEST(rsrccall_get_resource_loads_a_handle) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    uint32_t h = call_import("GetResource", 2, FOURCC('V', 'i', 's', 'u'), 128u);
    CHECK(mm_is_handle(h));
    CHECK_EQ(res_error(), 0);
    CHECK_EQ(mm_handle_size(h), 36);
    CHECK_EQ(fnv1a32(gm_ptr(gm_r32(h), 36), 36), 0xC4405513u);
    CHECK(mm_handle_state(h) & MM_STATE_RESOURCE);
    CHECK_EQ(call_import("GetResource", 2, FOURCC('V', 'i', 's', 'u'), 128u), h);
    call_import("LoadResource", 1, h);
    CHECK_EQ(res_error(), 0);
}

TEST(rsrccall_purgeable_attribute_sets_handle_state) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    uint32_t h = call_import("GetResource", 2, FOURCC('A', 'L', 'R', 'T'), 901u);
    CHECK_EQ(mm_handle_state(h), MM_STATE_RESOURCE | MM_STATE_PURGEABLE);
}

TEST(rsrccall_negative_ids_are_sign_extended) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    uint32_t h = call_import("GetResource", 2, FOURCC('S', 'I', 'Z', 'E'), 0xFFFFFFFFu);
    CHECK(mm_is_handle(h));
    CHECK_EQ(mm_handle_size(h), 10);
}

/* Review Focus 3: a resource that doesn't exist. */
TEST(rsrccall_missing_resource_returns_null_and_res_error) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    CHECK_EQ(call_import("GetResource", 2, FOURCC('V', 'i', 's', 'u'), 999u), 0);
    CHECK_EQ(res_error(), RSRC_NOT_FOUND_ERR);
    CHECK_EQ(call_import("GetResource", 2, FOURCC('Z', 'Z', 'Z', 'Z'), 128u), 0);
    CHECK_EQ(res_error(), RSRC_NOT_FOUND_ERR);
    call_import("ReleaseResource", 1, 0u);
    CHECK_EQ(res_error(), RSRC_NOT_FOUND_ERR);
}

TEST(rsrccall_release_then_get_gives_a_fresh_copy) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    uint32_t h = call_import("GetResource", 2, FOURCC('V', 'i', 's', 'u'), 128u);
    gm_w8(gm_r32(h), 0xEE);
    call_import("ReleaseResource", 1, h);
    CHECK_EQ(res_error(), 0);
    CHECK(!mm_is_handle(h));
    uint32_t h2 = call_import("GetResource", 2, FOURCC('V', 'i', 's', 'u'), 128u);
    CHECK(mm_is_handle(h2));
    CHECK_EQ(gm_r8(gm_r32(h2)), 0x6C);
}

TEST(rsrccall_get_named_resource) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    uint32_t name = scratch(256);
    gm_write_pstr(name, "Buttom Left");
    uint32_t h = call_import("GetNamedResource", 2, FOURCC('F', 'l', 'i', 'p'), name);
    CHECK(mm_is_handle(h));
    CHECK_EQ(call_import("GetResource", 2, FOURCC('F', 'l', 'i', 'p'), 1000u), h);
    gm_write_pstr(name, "Nope");
    CHECK_EQ(call_import("GetNamedResource", 2, FOURCC('F', 'l', 'i', 'p'), name), 0);
    CHECK_EQ(res_error(), RSRC_NOT_FOUND_ERR);
}

TEST(rsrccall_cur_res_file) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    CHECK_EQ(call_import("CurResFile", 0), RSRC_APP_REFNUM);
}

TEST(rsrccall_get_ind_string) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    uint32_t s = scratch(256);
    char out[256];
    call_import("GetIndString", 3, s, 128u, 2u);
    gm_read_pstr(s, out);
    CHECK_STR(out, "Untitled");
    call_import("GetIndString", 3, s, 128u, 25u);
    CHECK(gm_r8(s) > 0);
    call_import("GetIndString", 3, s, 128u, 26u);
    CHECK_EQ(gm_r8(s), 0);
    call_import("GetIndString", 3, s, 128u, 0u);
    CHECK_EQ(gm_r8(s), 0);
    call_import("GetIndString", 3, s, 9100u, 1u);
    CHECK_EQ(gm_r8(s), 0);
    call_import("GetIndString", 3, s, 4242u, 1u);
    CHECK_EQ(gm_r8(s), 0);
    CHECK_EQ(res_error(), RSRC_NOT_FOUND_ERR);
}
