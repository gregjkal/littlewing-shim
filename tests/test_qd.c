#include "test.h"

#include <stdlib.h>

#include "harness.h"
#include "memmgr.h"
#include "qd.h"
#include "rsrc.h"

static const char *const names[] = {
    "SetRect", "OffsetRect", "GetCTable", "GetMainDevice", "SetDepth", "NewGWorld",
    "UpdateGWorld", "DisposeGWorld", "GetGWorldPixMap", "LockPixels", "UnlockPixels",
    "GetPixBaseAddr", "SetGWorld", "GetGWorld", "SetPortWindowPort", "GetWindowPort",
    "GetPortBounds", "GetWindowPortBounds", "GetPortBitMapForCopyBits", "GetQDGlobalsScreenBits",
    "ShowWindow", "HideWindow", "InvalWindowRect", "QDFlushPortBuffer", "BeginFullScreen",
    "EndFullScreen", "ClipRect", "RGBForeColor", "PaintRect", "CopyBits", "DrawPicture",
};

static void setup(void) {
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    qd_init(800, 600, 8);
    qd_register();
}

static uint32_t rect(int top, int left, int bottom, int right) {
    uint32_t r = scratch(8);
    qd_write_rect(r, (qd_rect){(int16_t)top, (int16_t)left, (int16_t)bottom, (int16_t)right});
    return r;
}

static uint32_t rgb(uint16_t r, uint16_t g, uint16_t b) {
    uint32_t c = scratch(6);
    gm_w16(c, r);
    gm_w16(c + 2, g);
    gm_w16(c + 4, b);
    return c;
}

static uint32_t new_gworld(int depth, int w, int h) {
    uint32_t out = scratch(4);
    if (call_import("NewGWorld", 6, out, (uint32_t)depth, rect(0, 0, h, w), 0u, 0u, 0u) != 0)
        fatal("NewGWorld failed");
    return gm_r32(out);
}

static uint32_t base_of(uint32_t gw) {
    return call_import("GetPixBaseAddr", 1, call_import("GetGWorldPixMap", 1, gw));
}

TEST(qd_set_rect_and_offset_rect) {
    setup();
    uint32_t r = scratch(8);
    call_import("SetRect", 5, r, 10u, 20u, 30u, 40u); /* left, top, right, bottom */
    qd_rect q = qd_read_rect(r);
    CHECK_EQ(q.top, 20);
    CHECK_EQ(q.left, 10);
    CHECK_EQ(q.bottom, 40);
    CHECK_EQ(q.right, 30);
    call_import("OffsetRect", 3, r, (uint32_t)-5, 7u);
    q = qd_read_rect(r);
    CHECK_EQ(q.top, 27);
    CHECK_EQ(q.left, 5);
}

TEST(qd_new_gworld_builds_a_pixmap) {
    setup();
    uint32_t gw = new_gworld(8, 20, 10);
    uint32_t pm_h = call_import("GetGWorldPixMap", 1, gw);
    CHECK(mm_is_handle(pm_h));
    uint32_t pm = gm_r32(pm_h);
    CHECK_EQ(gm_r16(pm + PM_ROW_BYTES), 0x8000 | 20);
    CHECK_EQ(gm_r16(pm + PM_PIXEL_SIZE), 8);
    qd_rect b = qd_read_rect(pm + PM_BOUNDS);
    CHECK_EQ(b.bottom, 10);
    CHECK_EQ(b.right, 20);
    uint32_t ctab = gm_r32(pm + PM_TABLE);
    CHECK(mm_is_handle(ctab));
    CHECK_EQ(gm_r16(gm_r32(ctab) + 6), 255); /* ctSize: 256 entries */
    CHECK_EQ(base_of(gw), gm_r32(pm + PM_BASE_ADDR));
    CHECK(mm_is_ptr(base_of(gw)));
    CHECK_EQ(call_import("LockPixels", 1, pm_h), 1);
    CHECK_EQ(call_import("GetPortBitMapForCopyBits", 1, gw), pm);
    uint32_t r = scratch(8);
    CHECK_EQ(call_import("GetPortBounds", 2, gw, r), r);
    CHECK_EQ(qd_read_rect(r).right, 20);
}

TEST(qd_new_gworld_depth_0_uses_the_screen_depth) {
    setup();
    uint32_t gw = new_gworld(0, 4, 4);
    CHECK_EQ(gm_r16(gm_r32(call_import("GetGWorldPixMap", 1, gw)) + PM_PIXEL_SIZE), 8);
}

TEST(qd_set_and_get_gworld) {
    setup();
    uint32_t gw = new_gworld(16, 4, 4);
    call_import("SetGWorld", 2, gw, 0u);
    uint32_t p = scratch(4), d = scratch(4);
    call_import("GetGWorld", 2, p, d);
    CHECK_EQ(gm_r32(p), gw);
    CHECK_EQ(gm_r32(d), call_import("GetMainDevice", 0));
    CHECK_EQ(qd_current_port(), gw);
}

TEST(qd_paint_rect_uses_the_fore_color_and_clip) {
    setup();
    uint32_t gw = new_gworld(16, 4, 2);
    call_import("SetGWorld", 2, gw, 0u);
    call_import("RGBForeColor", 1, rgb(0xFFFF, 0, 0));
    call_import("ClipRect", 1, rect(0, 0, 2, 3));
    call_import("PaintRect", 1, rect(0, 1, 1, 4));
    uint32_t b = base_of(gw);
    CHECK_EQ(gm_r16(b), 0);
    CHECK_EQ(gm_r16(b + 2), 0x7C00);
    CHECK_EQ(gm_r16(b + 4), 0x7C00);
    CHECK_EQ(gm_r16(b + 6), 0); /* clipped */
    CHECK_EQ(gm_r16(b + 8), 0); /* row 1 untouched */
}

TEST(qd_copy_bits_converts_8_to_16_bits) {
    setup();
    uint32_t src = new_gworld(8, 4, 1), dst = new_gworld(16, 4, 1);
    gm_w8(base_of(src) + 1, 255); /* black */
    call_import("SetGWorld", 2, dst, 0u);
    uint32_t sb = call_import("GetPortBitMapForCopyBits", 1, src);
    uint32_t db = call_import("GetPortBitMapForCopyBits", 1, dst);
    call_import("CopyBits", 6, sb, db, rect(0, 0, 1, 4), rect(0, 0, 1, 4), 0u, 0u);
    uint32_t b = base_of(dst);
    CHECK_EQ(gm_r16(b), 0x7FFF); /* white */
    CHECK_EQ(gm_r16(b + 2), 0);  /* black */
}

static void child_copy_bits_mask(void *unused) {
    (void)unused;
    setup();
    uint32_t gw = new_gworld(8, 4, 4);
    uint32_t bits = call_import("GetPortBitMapForCopyBits", 1, gw);
    call_import("CopyBits", 6, bits, bits, rect(0, 0, 1, 1), rect(0, 0, 1, 1), 0u, 0x1234u);
}

TEST(qd_copy_bits_with_a_mask_region_crashes) {
    char out[16384];
    CHECK_EQ(test_run_child(child_copy_bits_mask, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "CopyBits: mask regions are not supported");
}

TEST(qd_set_depth_recreates_the_screen) {
    setup();
    uint32_t gd = call_import("GetMainDevice", 0);
    CHECK_EQ(call_import("SetDepth", 4, gd, 16u, 0u, 1u), 0);
    uint32_t pm = gm_r32(gm_r32(gm_r32(gd) + GD_PMAP));
    CHECK_EQ(gm_r16(pm + PM_PIXEL_SIZE), 16);
    CHECK_EQ(gm_r16(gm_r32(gd) + GD_TYPE), 2);
    uint32_t bits = scratch(14);
    call_import("GetQDGlobalsScreenBits", 1, bits);
    CHECK_EQ(gm_r16(bits + 4), 1600);
    qd_rect b = qd_read_rect(bits + 6);
    CHECK_EQ(b.bottom, 600);
    CHECK_EQ(b.right, 800);
}

static int presents;
static void count_present(void) { presents++; }

TEST(qd_begin_full_screen_makes_a_window_on_the_screen) {
    setup();
    call_import("SetDepth", 4, call_import("GetMainDevice", 0), 16u, 0u, 1u);
    uint32_t restore = scratch(4), win_p = scratch(4);
    qd_take_dirty();
    CHECK_EQ(call_import("BeginFullScreen", 7, restore, 0u, 0u, 0u, win_p, rgb(0, 0, 0xFFFF), 2u), 0);
    uint32_t win = gm_r32(win_p);
    CHECK(win != 0);
    CHECK_EQ(call_import("GetWindowPort", 1, win), win);
    uint32_t r = scratch(8);
    call_import("GetWindowPortBounds", 2, win, r);
    CHECK_EQ(qd_read_rect(r).right, 800);
    qd_pixels px;
    qd_palette pal;
    qd_screen(&px, &pal);
    CHECK_EQ(rd_be16(px.base), 0x001F); /* erased to blue */
    CHECK(qd_take_dirty());
    call_import("SetPortWindowPort", 1, win);
    call_import("RGBForeColor", 1, rgb(0xFFFF, 0, 0));
    call_import("PaintRect", 1, rect(0, 0, 1, 1));
    CHECK_EQ(rd_be16(px.base), 0x7C00);
    CHECK(qd_take_dirty());
    presents = 0;
    qd_set_present(count_present);
    call_import("QDFlushPortBuffer", 2, win, 0u);
    CHECK_EQ(presents, 1);
    call_import("HideWindow", 1, win);
    call_import("ShowWindow", 1, win);
    call_import("InvalWindowRect", 2, win, r);
    CHECK_EQ(call_import("EndFullScreen", 2, gm_r32(restore), 0u), 0);
}

TEST(qd_get_ctable_standard_tables) {
    setup();
    uint32_t h = call_import("GetCTable", 1, 8u);
    CHECK(mm_is_handle(h));
    qd_palette pal;
    qd_read_ctab(h, &pal);
    CHECK_EQ(pal.c[0].r, 0xFFFF);
    CHECK_EQ(pal.c[255].r, 0);
    CHECK_EQ(call_import("GetCTable", 1, 99u), 0);
}

TEST(qd_update_gworld_with_the_same_size_is_a_no_op) {
    setup();
    uint32_t gw = new_gworld(8, 16, 8), p = scratch(4);
    gm_w32(p, gw);
    CHECK_EQ(call_import("UpdateGWorld", 6, p, 8u, rect(0, 0, 8, 16), 0u, 0u, 0u), 0);
    CHECK_EQ(call_import("UpdateGWorld", 6, p, 0u, rect(10, 10, 18, 26), 0u, 0u, 0u), 0);
}

static void child_update_gworld_resize(void *unused) {
    (void)unused;
    setup();
    uint32_t gw = new_gworld(8, 16, 8), p = scratch(4);
    gm_w32(p, gw);
    call_import("UpdateGWorld", 6, p, 8u, rect(0, 0, 9, 16), 0u, 0u, 0u);
}

TEST(qd_update_gworld_resize_crashes) {
    char out[16384];
    CHECK_EQ(test_run_child(child_update_gworld_resize, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "UpdateGWorld: changing a 16x8 8-bit GWorld to 16x9 8-bit");
}

TEST(qd_dispose_gworld_frees_its_memory) {
    setup();
    uint32_t before = mm_free_bytes();
    uint32_t gw = new_gworld(16, 64, 64);
    CHECK(mm_free_bytes() < before);
    call_import("DisposeGWorld", 1, gw);
    CHECK_EQ(mm_free_bytes(), before);
}

static void child_bad_port(void *unused) {
    (void)unused;
    setup();
    call_import("SetGWorld", 2, 0x00101234u, 0u);
}

TEST(qd_unknown_port_crashes) {
    char out[16384];
    CHECK_EQ(test_run_child(child_bad_port, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "SetGWorld: 0x00101234 is not a port");
}

TEST(qd_draw_picture_into_a_gworld) {
    SKIP_UNLESS_GAME();
    setup();
    char path[1100];
    size_t len;
    snprintf(path, sizeof path, "%s/..namedfork/rsrc", test_game_exe_path());
    uint8_t *fork = read_file(path, &len);
    CHECK(fork != NULL);
    char err[256];
    CHECK(rsrc_open(fork, len, err, sizeof err));
    rsrc_entry *e = rsrc_find(FOURCC('P', 'I', 'C', 'T'), 800);
    uint32_t pic = mm_new_handle(e->len, false);
    memcpy(gm_ptr(gm_r32(pic), e->len), rsrc_data(e), e->len);
    uint32_t gw = new_gworld(8, 512, 384);
    call_import("SetGWorld", 2, gw, 0u);
    call_import("DrawPicture", 2, pic, rect(0, 0, 384, 512));
    uint32_t h = fnv1a32(gm_ptr(base_of(gw), 512 * 384), 512 * 384);
    rsrc_close();
    free(fork);
    CHECK_EQ(h, 0x4657C203u); /* same as pict_decodes_the_title_picture */
}
