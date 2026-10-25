#include "test.h"

#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "memmgr.h"
#include "qd.h"
#include "region.h"
#include "rsrc.h"
#include "util.h"

/* QuickDraw and the Window Manager as a System 7 program (Tristan) uses
   them: its own QuickDraw globals, ports in its own memory, and windows
   from WIND resources that get update events. */

static const char *const names[] = {
    "InitGraf", "InitWindows", "SetPort", "GetPort", "Random", "GetMainDevice", "SetGDevice",
    "RestoreDeviceClut", "SetEntries", "NewPixMap", "OpenCPort", "CloseCPort", "SetPortPix",
    "PortSize", "RGBForeColor", "PaintRect", "CopyBits", "GetNewCWindow", "CloseWindow",
    "BeginUpdate", "EndUpdate", "PaintBehind", "ShowWindow", "HideWindow", "RectRgn",
    "GetCTable", "SetDepth",
};

/* WIND 128: bounds (12, 28, 455, 608), plainDBox, visible, refCon 7,
   title "Machine". WIND 129: the same, hidden. */
static const uint8_t wind_shown[] = {0, 12, 0, 28, 1, 0xC7, 2, 0x60, 0, 3, 1, 0, 0, 0, 0, 0, 0, 7,
                                     7, 'M', 'a', 'c', 'h', 'i', 'n', 'e'};
static const uint8_t wind_hidden[] = {0, 12, 0, 28, 1, 0xC7, 2, 0x60, 0, 3, 0, 0, 0, 0, 0, 0, 0, 7,
                                      7, 'M', 'a', 'c', 'h', 'i', 'n', 'e'};
static uint8_t *fork;

static void setup(void) {
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    rsrc_entry e[2] = {{.type = FOURCC('W', 'I', 'N', 'D'), .id = 128, .len = sizeof wind_shown},
                       {.type = FOURCC('W', 'I', 'N', 'D'), .id = 129, .len = sizeof wind_hidden}};
    const uint8_t *d[2] = {wind_shown, wind_hidden};
    size_t len;
    free(fork);
    fork = rsrc_build(e, d, 2, &len);
    char err[256];
    if (!rsrc_open(fork, len, err, sizeof err))
        fatal("rsrc_open: %s", err);
    qd_init(640, 480, 8);
    qd_register();
    region_register();
    rsrc_register();
}

static uint32_t rect(int top, int left, int bottom, int right) {
    uint32_t r = scratch(8);
    qd_write_rect(r, (qd_rect){(int16_t)top, (int16_t)left, (int16_t)bottom, (int16_t)right});
    return r;
}

static uint32_t rgb(uint16_t r, uint16_t g, uint16_t b) {
    uint32_t c = scratch(6);
    qd_write_rgb(c, (qd_rgb){r, g, b});
    return c;
}

static uint32_t current_port(void) {
    uint32_t p = scratch(4);
    call_import("GetPort", 1, p);
    return gm_r32(p);
}

/* The program's QDGlobals, and InitGraf given its thePort, the last field. */
static uint32_t init_graf(void) {
    uint32_t qd = scratch(QDGLOBALS_SIZE);
    memset(gm_ptr(qd, QDGLOBALS_SIZE), 0xAA, QDGLOBALS_SIZE);
    call_import("InitGraf", 1, qd + QDG_THE_PORT);
    call_import("InitWindows", 0);
    return qd;
}

TEST(classic_init_graf_fills_the_programs_globals) {
    setup();
    uint32_t qd = init_graf();
    CHECK_EQ(gm_r32(qd + QDG_THE_PORT), current_port());
    CHECK_EQ(gm_r32(qd + QDG_RAND_SEED), 1);
    uint32_t screen = gm_r32(gm_r32(gm_r32(qd_main_device()) + GD_PMAP));
    CHECK_EQ(gm_r32(qd + QDG_SCREEN_BITS), gm_r32(screen + PM_BASE_ADDR));
    CHECK_EQ(gm_r16(qd + QDG_SCREEN_BITS + 4), 640);
    qd_rect b = qd_read_rect(qd + QDG_SCREEN_BITS + 6);
    CHECK(b.top == 0 && b.left == 0 && b.bottom == 480 && b.right == 640);
    static const uint8_t gray[8] = {0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55};
    CHECK(memcmp(gm_ptr(qd + QDG_PATTERNS + 16, 8), gray, 8) == 0);
    CHECK_EQ(gm_r8(qd + QDG_PATTERNS + 24), 0xFF); /* black */
    CHECK_EQ(gm_r8(qd + QDG_PATTERNS + 32), 0);    /* white */
    CHECK_EQ(gm_r8(qd), 0); /* the privates are cleared */

    uint32_t port = scratch(CGRAFPORT_SIZE);
    call_import("OpenCPort", 1, port);
    CHECK_EQ(gm_r32(qd + QDG_THE_PORT), port);
    call_import("CloseCPort", 1, port);
    CHECK(gm_r32(qd + QDG_THE_PORT) != port); /* back to the screen's */
    CHECK_EQ(gm_r32(qd + QDG_THE_PORT), current_port());

    /* The screen changing changes screenBits. */
    call_import("SetDepth", 4, qd_main_device(), 16u, 1u, 1u);
    CHECK_EQ(gm_r16(qd + QDG_SCREEN_BITS + 4), 1280);
}

/* Random's seed is qd.randSeed, which the program may set itself. */
TEST(classic_random_uses_the_programs_seed) {
    setup();
    uint32_t qd = init_graf();
    CHECK_EQ((int16_t)call_import("Random", 0), 16807);
    CHECK_EQ(gm_r32(qd + QDG_RAND_SEED), 16807);
    gm_w32(qd + QDG_RAND_SEED, 1);
    CHECK_EQ((int16_t)call_import("Random", 0), 16807);
}

static qd_rgb screen_color(int index) {
    uint32_t ctab = gm_r32(gm_r32(gm_r32(gm_r32(qd_main_device()) + GD_PMAP)) + PM_TABLE);
    return qd_read_rgb(gm_r32(ctab) + CTAB_HEADER + 8u * (uint32_t)index + 2);
}

TEST(classic_set_entries_and_restore_device_clut) {
    setup();
    uint32_t gd = call_import("GetMainDevice", 0);
    call_import("SetGDevice", 1, gd);
    uint32_t ctab = gm_r32(gm_r32(gm_r32(gd) + GD_PMAP));
    uint32_t seed = gm_r32(gm_r32(gm_r32(ctab + PM_TABLE)));
    qd_rgb white = screen_color(0);
    uint32_t specs = scratch(16);
    gm_w16(specs, 0);
    qd_write_rgb(specs + 2, (qd_rgb){0x1111, 0x2222, 0x3333});
    gm_w16(specs + 8, 0);
    qd_write_rgb(specs + 10, (qd_rgb){0x4444, 0x5555, 0x6666});
    call_import("SetEntries", 3, 4u, 1u, specs); /* count is one less */
    CHECK_EQ(screen_color(4).r, 0x1111);
    CHECK_EQ(screen_color(5).b, 0x6666);
    CHECK(gm_r32(gm_r32(gm_r32(ctab + PM_TABLE))) != seed);
    CHECK(qd_take_dirty());
    gm_w16(specs, 9);
    call_import("SetEntries", 3, (uint32_t)-1, 0u, specs); /* at each spec's value */
    CHECK_EQ(screen_color(9).g, 0x2222);
    call_import("RestoreDeviceClut", 1, 0u);
    qd_palette std;
    qd_std_palette(8, &std);
    CHECK_EQ(screen_color(4).r, std.c[4].r);
    CHECK_EQ(screen_color(9).g, std.c[9].g);
    CHECK_EQ(screen_color(0).r, white.r);
}

/* Tristan's offscreen: a port in its memory (OpenCPort), whose PixMap it
   replaces with one of its own (NewPixMap, SetPortPix) holding its own
   pixels, then CopyBits from the port's &portBits to a window's. */
TEST(classic_a_port_in_the_programs_memory_draws_offscreen) {
    setup();
    init_graf();
    uint32_t saved = current_port();
    uint32_t pm = call_import("NewPixMap", 0);
    CHECK(mm_is_handle(pm));
    CHECK_EQ(gm_r16(gm_r32(pm) + PM_PIXEL_SIZE), 8);
    uint32_t screen_pm = gm_r32(gm_r32(gm_r32(qd_main_device()) + GD_PMAP));
    CHECK(gm_r32(gm_r32(pm) + PM_TABLE) != gm_r32(screen_pm + PM_TABLE)); /* a table of its own */
    uint32_t pixels = mm_new_ptr(16 * 8, true);
    uint32_t port = scratch(CGRAFPORT_SIZE);
    call_import("OpenCPort", 1, port);
    CHECK_EQ(current_port(), port);
    CHECK_EQ(gm_r16(port + PORT_VERSION), 0xC000);
    qd_rect pr = qd_read_rect(port + PORT_RECT);
    CHECK(pr.bottom == 480 && pr.right == 640);
    CHECK(mm_dispose_handle(gm_r32(port + PORT_PIXMAP)) == MM_NO_ERR); /* as Tristan does */
    uint32_t p = gm_r32(pm);
    qd_write_rect(p + PM_BOUNDS, (qd_rect){0, 0, 8, 16});
    gm_w16(p + PM_ROW_BYTES, 0x8000 | 16);
    gm_w32(p + PM_BASE_ADDR, pixels);
    call_import("SetPortPix", 1, pm);
    call_import("PortSize", 2, 16u, 8u);
    pr = qd_read_rect(port + PORT_RECT);
    CHECK(pr.top == 0 && pr.left == 0 && pr.bottom == 8 && pr.right == 16);
    call_import("RectRgn", 2, gm_r32(port + PORT_VIS_RGN), rect(0, 0, 8, 16));
    call_import("RGBForeColor", 1, rgb(0, 0, 0));
    call_import("PaintRect", 1, rect(2, 2, 4, 6));
    CHECK_EQ(gm_r8(pixels + 2 * 16 + 2), 255); /* black in the standard table */
    CHECK_EQ(gm_r8(pixels + 2 * 16 + 1), 0);
    call_import("SetPort", 1, saved);

    uint32_t w = call_import("GetNewCWindow", 3, 128u, 0u, (uint32_t)-1);
    call_import("SetPort", 1, w);
    call_import("CopyBits", 6, port + 2, w + 2, rect(0, 0, 8, 16), rect(0, 0, 8, 16), 0u, 0u);
    uint32_t base = gm_r32(screen_pm + PM_BASE_ADDR);
    CHECK_EQ(gm_r8(base + (12 + 2) * 640 + 28 + 2), 255);
    CHECK_EQ(gm_r8(base + (12 + 2) * 640 + 28 + 1), 0);

    call_import("CloseCPort", 1, port);
    CHECK(!mm_is_handle(pm));
    CHECK(mm_is_ptr(pixels)); /* the pixels stay the program's */
}

TEST(classic_get_new_cwindow_reads_its_wind) {
    setup();
    init_graf();
    uint32_t w = call_import("GetNewCWindow", 3, 128u, 0u, (uint32_t)-1);
    CHECK(mm_is_ptr(w));
    CHECK(mm_ptr_size(w) >= 156); /* a whole CWindowRecord */
    CHECK_EQ(gm_r16(w + 108), 8);  /* windowKind: userKind */
    CHECK_EQ(gm_r8(w + 110), 1);   /* visible */
    CHECK_EQ(gm_r32(w + 152), 7);  /* refCon */
    qd_rect pr = qd_read_rect(w + PORT_RECT);
    CHECK(pr.top == 0 && pr.left == 0 && pr.bottom == 443 && pr.right == 580);
    qd_rect b = qd_read_rect(gm_r32(gm_r32(w + PORT_PIXMAP)) + PM_BOUNDS);
    CHECK(b.top == -12 && b.left == -28); /* its (0, 0) is at (28, 12) on the screen */
    call_import("CloseWindow", 1, w);
}

/* A WIND the program changed in memory is the one used. */
TEST(classic_get_new_cwindow_reads_a_loaded_wind) {
    setup();
    rsrc_entry *e = rsrc_find(FOURCC('W', 'I', 'N', 'D'), 128);
    e->handle = mm_new_handle(e->len, false);
    memcpy(gm_ptr(gm_r32(e->handle), e->len), rsrc_data(e), e->len);
    qd_write_rect(gm_r32(e->handle), (qd_rect){0, 0, 443, 580});
    uint32_t w = call_import("GetNewCWindow", 3, 128u, 0u, (uint32_t)-1);
    qd_rect b = qd_read_rect(gm_r32(gm_r32(w + PORT_PIXMAP)) + PM_BOUNDS);
    CHECK(b.top == 0 && b.left == 0);
}

/* A shown window gets an update event until BeginUpdate; PaintBehind and
   showing it again give it another. */
TEST(classic_windows_get_update_events) {
    setup();
    init_graf();
    CHECK_EQ(qd_pending_update(), 0);
    uint32_t hidden = call_import("GetNewCWindow", 3, 129u, 0u, (uint32_t)-1);
    CHECK_EQ(qd_pending_update(), 0);
    uint32_t w = call_import("GetNewCWindow", 3, 128u, 0u, (uint32_t)-1);
    CHECK_EQ(qd_pending_update(), w);
    CHECK_EQ(qd_pending_update(), w); /* until it is taken */
    call_import("BeginUpdate", 1, w);
    call_import("EndUpdate", 1, w);
    CHECK_EQ(qd_pending_update(), 0);
    call_import("ShowWindow", 1, hidden);
    CHECK_EQ(qd_pending_update(), hidden);
    call_import("BeginUpdate", 1, hidden);
    call_import("PaintBehind", 2, 0u, 0u);
    CHECK_EQ(qd_pending_update(), w); /* the front one first */
    call_import("BeginUpdate", 1, w);
    CHECK_EQ(qd_pending_update(), hidden);
    call_import("HideWindow", 1, hidden);
    CHECK_EQ(qd_pending_update(), 0);
    call_import("CloseWindow", 1, w);
    CHECK_EQ(qd_pending_update(), 0);
}
