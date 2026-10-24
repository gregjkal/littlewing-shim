#include "test.h"

#include <stdlib.h>

#include "cf.h"
#include "harness.h"
#include "memmgr.h"
#include "qd.h"
#include "region.h"
#include "rsrc.h"

static const char *const names[] = {
    "SetRect", "OffsetRect", "GetCTable", "GetMainDevice", "SetDepth", "NewGWorld",
    "UpdateGWorld", "DisposeGWorld", "GetGWorldPixMap", "LockPixels", "UnlockPixels",
    "GetPixBaseAddr", "SetGWorld", "GetGWorld", "SetPortWindowPort", "GetWindowPort",
    "GetPortBounds", "GetWindowPortBounds", "GetPortBitMapForCopyBits", "GetQDGlobalsScreenBits",
    "ShowWindow", "HideWindow", "InvalWindowRect", "QDFlushPortBuffer", "BeginFullScreen",
    "EndFullScreen", "ClipRect", "RGBForeColor", "PaintRect", "CopyBits", "DrawPicture",
    "GetEntryColor", "DisposePalette", "CreateNewWindow", "ChangeWindowAttributes",
    "SetWindowTitleWithCFString", "SetClip", "GetClip", "GetPortVisibleRegion", "NewRgn",
    "RectRgn", "UnionRgn", "GetRegionBounds", "GetDeviceList", "GetNextDevice", "GetMaxDevice",
    "TestDeviceAttribute", "Index2Color", "Color2Index", "Random",
};

static void setup(void) {
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    qd_init(800, 600, 8);
    qd_register();
    region_register();
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

TEST(qd_copy_bits_dither_copy_is_src_copy) {
    setup();
    uint32_t src = new_gworld(8, 4, 1), dst = new_gworld(16, 4, 1);
    gm_w8(base_of(src) + 1, 255); /* black */
    call_import("SetGWorld", 2, dst, 0u);
    uint32_t sb = call_import("GetPortBitMapForCopyBits", 1, src);
    uint32_t db = call_import("GetPortBitMapForCopyBits", 1, dst);
    call_import("CopyBits", 6, sb, db, rect(0, 0, 1, 4), rect(0, 0, 1, 4), 64u, 0u);
    uint32_t b = base_of(dst);
    CHECK_EQ(gm_r16(b), 0x7FFF);
    CHECK_EQ(gm_r16(b + 2), 0);
}

TEST(qd_gworld_rows_padded_as_system_7_1_did) {
    setup();
    qd_pad_gworld_rows(true);
    uint32_t a = new_gworld(8, 384, 2), b = new_gworld(16, 17, 1);
    qd_pad_gworld_rows(false);
    uint32_t c = new_gworld(8, 384, 2);
    CHECK_EQ(gm_r16(gm_r32(call_import("GetGWorldPixMap", 1, a)) + PM_ROW_BYTES), 0x8000 | 400);
    CHECK_EQ(gm_r16(gm_r32(call_import("GetGWorldPixMap", 1, b)) + PM_ROW_BYTES), 0x8000 | 64);
    CHECK_EQ(gm_r16(gm_r32(call_import("GetGWorldPixMap", 1, c)) + PM_ROW_BYTES), 0x8000 | 384);
    CHECK_EQ(mm_ptr_size(base_of(a)), 400 * 2);
}

TEST(qd_copy_bits_with_a_mask_that_isnt_a_region_crashes) {
    char out[16384];
    CHECK_EQ(test_run_child(child_copy_bits_mask, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "CopyBits: 0x00001234 is not a region");
}

/* An L: rows 0-1 columns 0-1, and row 1 columns 0-3. */
static uint32_t l_shape(void) {
    uint32_t a = call_import("NewRgn", 0), b = call_import("NewRgn", 0);
    call_import("RectRgn", 2, a, rect(0, 0, 2, 2));
    call_import("RectRgn", 2, b, rect(1, 0, 2, 4));
    call_import("UnionRgn", 3, a, b, a);
    return a;
}

TEST(qd_paint_rect_clips_to_a_shaped_clip_region) {
    setup();
    uint32_t gw = new_gworld(16, 4, 2);
    call_import("SetGWorld", 2, gw, 0u);
    call_import("RGBForeColor", 1, rgb(0xFFFF, 0, 0));
    call_import("SetClip", 1, l_shape());
    call_import("PaintRect", 1, rect(0, 0, 2, 4));
    uint32_t b = base_of(gw), row = 8;
    CHECK_EQ(gm_r16(b), 0x7C00);
    CHECK_EQ(gm_r16(b + 2), 0x7C00);
    CHECK_EQ(gm_r16(b + 4), 0); /* outside the L */
    CHECK_EQ(gm_r16(b + 6), 0);
    for (uint32_t x = 0; x < 4; x++)
        CHECK_EQ(gm_r16(b + row + 2 * x), 0x7C00);

    uint32_t back = call_import("NewRgn", 0), r = scratch(8);
    call_import("GetClip", 1, back);
    call_import("GetRegionBounds", 2, back, r);
    CHECK_EQ(qd_read_rect(r).right, 4);
    CHECK_EQ(gm_r16(gm_r32(back)), gm_r16(gm_r32(l_shape()))); /* the same packed size */
}

TEST(qd_copy_bits_draws_through_a_mask_region) {
    setup();
    uint32_t src = new_gworld(8, 4, 2), dst = new_gworld(8, 4, 2);
    for (uint32_t i = 0; i < 4; i++)
        gm_w8(base_of(src) + i, 255), gm_w8(base_of(src) + 4 + i, 255);
    call_import("SetGWorld", 2, dst, 0u);
    uint32_t sb = call_import("GetPortBitMapForCopyBits", 1, src);
    uint32_t db = call_import("GetPortBitMapForCopyBits", 1, dst);
    call_import("CopyBits", 6, sb, db, rect(0, 0, 2, 4), rect(0, 0, 2, 4), 0u, l_shape());
    uint32_t d = base_of(dst);
    CHECK_EQ(gm_r8(d), 255);
    CHECK_EQ(gm_r8(d + 1), 255);
    CHECK_EQ(gm_r8(d + 2), 0); /* masked off */
    CHECK_EQ(gm_r8(d + 3), 0);
    CHECK_EQ(gm_r8(d + 4 + 3), 255);
}

TEST(qd_get_port_visible_region_copies_the_ports) {
    setup();
    uint32_t gw = new_gworld(8, 20, 10);
    uint32_t vis = call_import("NewRgn", 0), r = scratch(8);
    CHECK_EQ(call_import("GetPortVisibleRegion", 2, gw, vis), vis);
    call_import("GetRegionBounds", 2, vis, r);
    CHECK_EQ(qd_read_rect(r).bottom, 10);
    CHECK_EQ(qd_read_rect(r).right, 20);
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
    CHECK(!qd_take_dirty());
    call_import("QDFlushPortBuffer", 2, win, 0u); /* presented at the next pump */
    CHECK(qd_take_dirty());
    call_import("HideWindow", 1, win);
    call_import("ShowWindow", 1, win);
    call_import("InvalWindowRect", 2, win, r);
    CHECK_EQ(call_import("EndFullScreen", 2, gm_r32(restore), 0u), 0);
}

TEST(qd_resize_screen_keeps_the_device) {
    setup();
    uint32_t gd = qd_main_device(), pm_h = gm_r32(gm_r32(gd) + GD_PMAP), port = qd_current_port();
    qd_resize_screen(1024, 768, 16);
    CHECK_EQ(qd_main_device(), gd);
    CHECK_EQ(gm_r32(gm_r32(gd) + GD_PMAP), pm_h);
    CHECK_EQ(qd_current_port(), port);
    CHECK_EQ(gm_r16(gm_r32(gd) + GD_TYPE), 2);
    CHECK_EQ(qd_read_rect(gm_r32(gd) + GD_RECT).right, 1024);
    CHECK_EQ(qd_read_rect(port + PORT_RECT).bottom, 768);
    qd_pixels px;
    qd_palette pal;
    qd_screen(&px, &pal);
    CHECK_EQ(rect_w(px.bounds), 1024);
    CHECK_EQ(rect_h(px.bounds), 768);
    CHECK_EQ(px.depth, 16);
    CHECK_EQ(px.row_bytes, 2048);
    CHECK_EQ(qd_screen_base(), gm_r32(gm_r32(pm_h) + PM_BASE_ADDR));
    CHECK(qd_take_dirty());
}

TEST(qd_direct_drawing_keeps_the_screen_dirty) {
    setup();
    qd_take_dirty();
    CHECK(!qd_take_dirty());
    qd_set_direct_drawing(true);
    CHECK(qd_take_dirty());
    CHECK(qd_take_dirty());
    qd_set_direct_drawing(false);
    CHECK(!qd_take_dirty());
}

TEST(qd_create_new_window_sizes_the_screen) {
    setup();
    cf_init();
    uint32_t out = scratch(4), r = scratch(8);
    CHECK_EQ(call_import("CreateNewWindow", 4, 6u, 0x0200001Fu, rect(100, 50, 868, 1074), out), 0);
    uint32_t win = gm_r32(out);
    call_import("GetWindowPortBounds", 2, win, r);
    CHECK_EQ(qd_read_rect(r).bottom, 768);
    CHECK_EQ(qd_read_rect(r).right, 1024);
    qd_pixels px;
    qd_palette pal;
    qd_screen(&px, &pal);
    CHECK_EQ(rect_w(px.bounds), 1024);
    CHECK_EQ(rect_h(px.bounds), 768);
    CHECK_EQ(px.depth, 8); /* the depth stays */
    /* Only the first window sizes the screen. */
    CHECK_EQ(call_import("CreateNewWindow", 4, 6u, 0u, rect(0, 0, 100, 200), out), 0);
    qd_screen(&px, &pal);
    CHECK_EQ(rect_w(px.bounds), 1024);
    CHECK_EQ(call_import("ChangeWindowAttributes", 3, win, 0u, 17u), 0);
    CHECK_EQ(call_import("SetWindowTitleWithCFString", 2, win, cf_string("Monster Fair")), 0);
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

TEST(qd_get_ctable_gray_tables) {
    setup();
    qd_palette pal;
    qd_read_ctab(call_import("GetCTable", 1, 40u), &pal);
    CHECK_EQ(pal.n, 256);
    CHECK_EQ(pal.c[0].r, 0xFFFF);
    CHECK_EQ(pal.c[255].b, 0);
    CHECK_EQ(pal.c[128].g, 0xFFFF * 127 / 255);
    uint32_t one = call_import("GetCTable", 1, 33u);
    CHECK_EQ(gm_r16(gm_r32(one) + 6), 1); /* ctSize: 2 entries */
    qd_read_ctab(one, &pal);
    CHECK_EQ(pal.c[0].r, 0xFFFF);
    CHECK_EQ(pal.c[1].r, 0);
}

TEST(qd_index2color_and_color2index_use_the_current_port) {
    setup();
    call_import("SetGWorld", 2, new_gworld(8, 4, 1), 0u);
    uint32_t c = scratch(6);
    call_import("Index2Color", 2, 255u, c);
    CHECK_EQ(gm_r16(c) | gm_r16(c + 2) | gm_r16(c + 4), 0);
    CHECK_EQ(call_import("Color2Index", 1, rgb(0xFFFF, 0xFFFF, 0xFFFF)), 0);
    CHECK_EQ(call_import("Color2Index", 1, rgb(0x0100, 0, 0)), 255); /* nearest: black */
}

TEST(qd_random_follows_quickdraws_sequence) {
    setup();
    CHECK_EQ((int16_t)call_import("Random", 0), 16807);
    CHECK_EQ((int16_t)call_import("Random", 0), 15089);
    CHECK_EQ((int16_t)call_import("Random", 0), -21287);
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

static void child_bad_pixmap(void *arg) {
    setup();
    uint32_t gw = new_gworld(16, 8, 2);
    uint32_t pm = gm_r32(call_import("GetGWorldPixMap", 1, gw));
    if (*(int *)arg == 0)
        gm_w16(pm + PM_ROW_BYTES, 0x8000 | 8); /* 8 bytes is too short for 8 16-bit pixels */
    else {
        gm_w16(pm + PM_PIXEL_SIZE, 8);
        gm_w32(pm + PM_TABLE, 0);           /* an indexed pixmap with no color table */
    }
    uint32_t bits = call_import("GetPortBitMapForCopyBits", 1, gw);
    call_import("CopyBits", 6, bits, bits, rect(0, 0, 1, 1), rect(0, 0, 1, 1), 0u, 0u);
}

TEST(qd_pixmap_with_rows_too_short_crashes) {
    char out[16384];
    int which = 0;
    CHECK_EQ(test_run_child(child_bad_pixmap, &which, out, sizeof out), 2);
    CHECK_CONTAINS(out, "CopyBits: pixmap rowBytes 8 is too small for 8 16-bit pixels");
}

TEST(qd_indexed_pixmap_without_a_color_table_crashes) {
    char out[16384];
    int which = 1;
    CHECK_EQ(test_run_child(child_bad_pixmap, &which, out, sizeof out), 2);
    CHECK_CONTAINS(out, "CopyBits: 8-bit pixmap has no color table");
}

TEST(qd_palette_entries_and_dispose) {
    setup();
    uint32_t pal = mm_new_handle(16 + 2 * 16, true);
    uint32_t p = gm_r32(pal);
    gm_w16(p, 2);
    gm_w16(p + 32, 0x1111);
    gm_w16(p + 34, 0x2222);
    gm_w16(p + 36, 0x3333);
    uint32_t rgb = scratch(6);
    call_import("GetEntryColor", 3, pal, 1u, rgb);
    CHECK_EQ(gm_r16(rgb), 0x1111);
    CHECK_EQ(gm_r16(rgb + 2), 0x2222);
    CHECK_EQ(gm_r16(rgb + 4), 0x3333);
    call_import("DisposePalette", 1, 0u); /* NULL is fine */
    call_import("DisposePalette", 1, pal);
    CHECK(!mm_is_handle(pal));
}

static void child_entry_outside(void *unused) {
    (void)unused;
    setup();
    uint32_t pal = mm_new_handle(16 + 16, true);
    gm_w16(gm_r32(pal), 1);
    call_import("GetEntryColor", 3, pal, 1u, scratch(6));
}

TEST(qd_palette_entry_outside_crashes) {
    char out[16384];
    CHECK_EQ(test_run_child(child_entry_outside, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "GetEntryColor: entry 1 is outside the palette (1 entries)");
}

/* The device list is the main screen, a color screen device. */
TEST(qd_the_device_list_is_the_main_screen) {
    setup();
    uint32_t gd = call_import("GetMainDevice", 0);
    CHECK_EQ(call_import("GetDeviceList", 0), gd);
    CHECK_EQ(call_import("GetNextDevice", 1, gd), 0);
    CHECK_EQ(call_import("GetMaxDevice", 1, rect(0, 0, 10, 10)), gd);
    CHECK_EQ(call_import("TestDeviceAttribute", 2, gd, 11u), 1); /* mainScreen */
    CHECK_EQ(call_import("TestDeviceAttribute", 2, gd, 13u), 1); /* screenDevice */
    CHECK_EQ(call_import("TestDeviceAttribute", 2, gd, 0u), 1);  /* gdDevType: color */
    CHECK_EQ(call_import("TestDeviceAttribute", 2, gd, 14u), 0); /* noDriver */
}

/* Golden Logres sets the depth on a NULL device: the main screen's. */
TEST(qd_set_depth_on_no_device_sets_the_main_screens) {
    setup();
    CHECK_EQ(call_import("SetDepth", 4, 0u, 16u, 1u, 1u), 0);
    uint32_t gd = call_import("GetMainDevice", 0);
    CHECK_EQ(gm_r16(gm_r32(gm_r32(gm_r32(gd) + GD_PMAP)) + PM_PIXEL_SIZE), 16);
}
