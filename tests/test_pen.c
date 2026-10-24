#include "test.h"

#include "harness.h"
#include "memmgr.h"
#include "pen.h"
#include "qd.h"
#include "region.h"

static const char *const names[] = {
    "NewGWorld",   "SetGWorld",     "GetGWorldPixMap", "GetPixBaseAddr", "RGBForeColor",
    "ClipRect",    "NewRgn",        "RectRgn",         "UnionRgn",       "GetPort",
    "SetPort",     "MoveTo",        "Move",            "GetPen",         "LineTo",
    "Line",        "PenSize",       "PenMode",         "PenPat",         "PenNormal",
    "HidePen",     "ShowPen",       "GetPenState",     "SetPenState",    "BackPat",
    "FrameRect",   "EraseRect",     "FillRect",        "FrameRoundRect", "PaintRoundRect",
    "EraseRoundRect", "PaintRgn",   "EraseRgn",        "FillRgn",        "RGBBackColor",
    "GetForeColor", "GetBackColor", "GetQDGlobalsBlack", "GetQDGlobalsWhite", "GetQDGlobalsGray",
    "GetQDGlobalsLightGray", "GetQDGlobalsDarkGray", "InsetRect", "SectRect", "UnionRect",
    "EqualRect",   "EmptyRect",     "PtInRect",        "EqualPt",
};

#define RED 0xFF0000u
#define WHITE 0xFFFFFFu
#define GREEN 0x00FF00u

static uint32_t base, row_bytes;

static uint32_t rect(int top, int left, int bottom, int right) {
    uint32_t r = scratch(8);
    qd_write_rect(r, (qd_rect){(int16_t)top, (int16_t)left, (int16_t)bottom, (int16_t)right});
    return r;
}

static uint32_t rgb(uint32_t xrgb) {
    uint32_t c = scratch(6);
    qd_write_rgb(c, (qd_rgb){(uint16_t)((xrgb >> 16 & 0xFF) * 0x101), (uint16_t)((xrgb >> 8 & 0xFF) * 0x101),
                             (uint16_t)((xrgb & 0xFF) * 0x101)});
    return c;
}

static uint32_t pattern(const uint8_t p[8]) {
    uint32_t a = scratch(8);
    memcpy(gm_ptr(a, 8), p, 8);
    return a;
}

/* A 32-bit 16x16 GWorld, all green, made the current port, drawing red on
   white. */
static void setup(void) {
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    qd_init(800, 600, 8);
    qd_register();
    region_register();
    pen_register();
    uint32_t out = scratch(4);
    if (call_import("NewGWorld", 6, out, 32u, rect(0, 0, 16, 16), 0u, 0u, 0u) != 0)
        fatal("NewGWorld failed");
    uint32_t gw = gm_r32(out);
    call_import("SetGWorld", 2, gw, 0u);
    uint32_t pm = call_import("GetGWorldPixMap", 1, gw);
    base = call_import("GetPixBaseAddr", 1, pm);
    row_bytes = gm_r16(gm_r32(pm) + PM_ROW_BYTES) & 0x3FFF;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            gm_w32(base + (uint32_t)y * row_bytes + 4u * (uint32_t)x, GREEN);
    call_import("RGBForeColor", 1, rgb(RED));
}

static uint32_t px(int x, int y) { return gm_r32(base + (uint32_t)y * row_bytes + 4u * (uint32_t)x) & 0xFFFFFF; }

/* How many pixels of the GWorld are c. */
static int count(uint32_t c) {
    int n = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            n += px(x, y) == c;
    return n;
}

TEST(pen_lines_draw_both_ends_and_move_the_pen) {
    setup();
    call_import("MoveTo", 2, 2u, 3u);
    call_import("LineTo", 2, 6u, 3u);
    CHECK_EQ(count(RED), 5);
    CHECK_EQ(px(2, 3), RED);
    CHECK_EQ(px(6, 3), RED);
    CHECK_EQ(px(7, 3), GREEN);
    uint32_t pt = scratch(4);
    call_import("GetPen", 1, pt);
    CHECK_EQ(gm_r16(pt), 3);     /* v */
    CHECK_EQ(gm_r16(pt + 2), 6); /* h */
    call_import("Line", 2, 0u, 4u); /* down to (6, 7) */
    CHECK_EQ(px(6, 7), RED);
    CHECK_EQ(count(RED), 9);
    call_import("MoveTo", 2, 0u, 0u);
    call_import("LineTo", 2, 3u, 3u); /* a diagonal */
    CHECK_EQ(px(0, 0), RED);
    CHECK_EQ(px(1, 1), RED);
    CHECK_EQ(px(3, 3), RED);
    CHECK_EQ(px(1, 0), GREEN);
}

TEST(pen_size_hide_and_show) {
    setup();
    call_import("PenSize", 2, 2u, 3u); /* width, height */
    call_import("MoveTo", 2, 10u, 10u);
    call_import("LineTo", 2, 10u, 10u);
    CHECK_EQ(count(RED), 6);
    CHECK_EQ(px(11, 12), RED);
    call_import("HidePen", 0);
    call_import("Move", 2, (uint32_t)-10, (uint32_t)-10);
    call_import("LineTo", 2, 5u, 0u);
    CHECK_EQ(count(RED), 6);
    uint32_t pt = scratch(4);
    call_import("GetPen", 1, pt);
    CHECK_EQ(gm_r16(pt + 2), 5);
    call_import("ShowPen", 0);
    call_import("LineTo", 2, 5u, 0u);
    CHECK_EQ(count(RED), 12);
}

TEST(pen_frame_rect_draws_the_outline_inside) {
    setup();
    call_import("FrameRect", 1, rect(2, 2, 8, 10));
    CHECK_EQ(px(2, 2), RED);
    CHECK_EQ(px(9, 7), RED);
    CHECK_EQ(px(5, 2), RED);
    CHECK_EQ(px(2, 5), RED);
    CHECK_EQ(px(5, 5), GREEN);
    CHECK_EQ(px(10, 5), GREEN);
    CHECK_EQ(count(RED), 2 * 8 + 2 * 4);
    call_import("PenSize", 2, 3u, 3u);
    call_import("FrameRect", 1, rect(10, 10, 15, 15)); /* no room for a hole */
    CHECK_EQ(px(12, 12), RED);
}

TEST(pen_erase_fill_and_patterns) {
    setup();
    call_import("EraseRect", 1, rect(0, 0, 2, 2));
    CHECK_EQ(px(0, 0), WHITE);
    CHECK_EQ(px(1, 1), WHITE);
    static const uint8_t gray[8] = {0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55};
    uint32_t g = scratch(8);
    CHECK_EQ(call_import("GetQDGlobalsGray", 1, g), g);
    CHECK(memcmp(gm_ptr(g, 8), gray, 8) == 0);
    call_import("FillRect", 2, rect(4, 0, 6, 4), g);
    CHECK_EQ(px(0, 4), RED);
    CHECK_EQ(px(1, 4), WHITE);
    CHECK_EQ(px(0, 5), WHITE);
    CHECK_EQ(px(1, 5), RED);
    call_import("RGBBackColor", 1, rgb(0x0000FF));
    call_import("BackPat", 1, g);
    call_import("EraseRect", 1, rect(8, 0, 9, 2));
    CHECK_EQ(px(0, 8), RED);
    CHECK_EQ(px(1, 8), 0x0000FF);
    uint32_t c = scratch(6);
    call_import("GetBackColor", 1, c);
    CHECK_EQ(gm_r16(c + 4), 0xFFFF);
    call_import("GetForeColor", 1, c);
    CHECK_EQ(gm_r16(c), 0xFFFF);
    CHECK_EQ(gm_r16(c + 2), 0);
}

TEST(pen_modes_and_pen_state) {
    setup();
    static const uint8_t gray[8] = {0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55};
    call_import("PenPat", 1, pattern(gray));
    call_import("PenMode", 1, (uint32_t)QD_PAT_OR);
    call_import("PaintRoundRect", 3, rect(0, 0, 1, 2), 0u, 0u);
    CHECK_EQ(px(0, 0), RED);
    CHECK_EQ(px(1, 0), GREEN); /* patOr leaves the 0 bits */
    call_import("PenMode", 1, (uint32_t)QD_PAT_XOR);
    call_import("PaintRoundRect", 3, rect(0, 0, 1, 2), 0u, 0u);
    CHECK_EQ(px(0, 0), 0x00FFFF); /* red inverted */
    CHECK_EQ(px(1, 0), GREEN);

    uint32_t st = scratch(18);
    call_import("MoveTo", 2, 4u, 5u);
    call_import("GetPenState", 1, st);
    CHECK_EQ(gm_r16(st), 5);
    CHECK_EQ(gm_r16(st + 2), 4);
    CHECK_EQ(gm_r16(st + 8), QD_PAT_XOR);
    CHECK(memcmp(gm_ptr(st + 10, 8), gray, 8) == 0);
    call_import("PenNormal", 0);
    call_import("MoveTo", 2, 0u, 0u);
    uint32_t again = scratch(18);
    call_import("GetPenState", 1, again);
    CHECK_EQ(gm_r16(again + 8), QD_PAT_COPY);
    CHECK_EQ(gm_r8(again + 10), 0xFF);
    call_import("SetPenState", 1, st);
    call_import("GetPenState", 1, again);
    CHECK(memcmp(gm_ptr(again, 18), gm_ptr(st, 18), 18) == 0);
}

TEST(pen_rounded_rectangles_leave_their_corners) {
    setup();
    call_import("PaintRoundRect", 3, rect(0, 0, 16, 16), 8u, 8u);
    CHECK_EQ(px(0, 0), GREEN);
    CHECK_EQ(px(15, 15), GREEN);
    CHECK_EQ(px(8, 0), RED);
    CHECK_EQ(px(0, 8), RED);
    CHECK_EQ(px(8, 8), RED);
    call_import("EraseRoundRect", 3, rect(0, 0, 16, 16), 8u, 8u);
    CHECK_EQ(px(8, 8), WHITE);
    CHECK_EQ(px(0, 0), GREEN);
    call_import("FrameRoundRect", 3, rect(0, 0, 16, 16), 8u, 8u);
    CHECK_EQ(px(8, 0), RED);
    CHECK_EQ(px(15, 8), RED);
    CHECK_EQ(px(8, 8), WHITE);
    CHECK_EQ(px(0, 0), GREEN);
}

TEST(pen_regions_draw_clipped_to_the_port) {
    setup();
    uint32_t r1 = call_import("NewRgn", 0), r2 = call_import("NewRgn", 0);
    call_import("RectRgn", 2, r1, rect(0, 0, 2, 2));
    call_import("RectRgn", 2, r2, rect(4, 4, 6, 16));
    call_import("UnionRgn", 3, r1, r2, r1);
    call_import("ClipRect", 1, rect(0, 0, 16, 8));
    call_import("PaintRgn", 1, r1);
    CHECK_EQ(count(RED), 4 + 2 * 4);
    CHECK_EQ(px(7, 5), RED);
    CHECK_EQ(px(8, 5), GREEN);
    call_import("EraseRgn", 1, r1);
    CHECK_EQ(count(WHITE), 12);
    call_import("FillRgn", 2, r1, call_import("GetQDGlobalsBlack", 1, scratch(8)));
    CHECK_EQ(count(RED), 12);
}

TEST(pen_ports) {
    setup();
    uint32_t out = scratch(4);
    call_import("GetPort", 1, out);
    uint32_t gw = gm_r32(out);
    CHECK(gw != 0);
    call_import("SetPort", 1, gw);
    call_import("GetPort", 1, out);
    CHECK_EQ(gm_r32(out), gw);
}

TEST(pen_rect_and_point_arithmetic) {
    setup();
    uint32_t r = rect(10, 10, 20, 30), d = scratch(8);
    call_import("InsetRect", 3, r, 2u, 3u);
    CHECK(memcmp(gm_ptr(r, 8), gm_ptr(rect(13, 12, 17, 28), 8), 8) == 0);
    call_import("InsetRect", 3, r, 10u, 0u);
    CHECK(memcmp(gm_ptr(r, 8), gm_ptr(rect(0, 0, 0, 0), 8), 8) == 0);
    CHECK_EQ(call_import("SectRect", 3, rect(0, 0, 10, 10), rect(5, 5, 20, 20), d), 1);
    CHECK_EQ(call_import("EqualRect", 2, d, rect(5, 5, 10, 10)), 1);
    CHECK_EQ(call_import("SectRect", 3, rect(0, 0, 10, 10), rect(10, 10, 20, 20), d), 0);
    CHECK_EQ(call_import("EqualRect", 2, d, rect(0, 0, 0, 0)), 1);
    CHECK_EQ(call_import("EmptyRect", 1, d), 1);
    call_import("UnionRect", 3, rect(0, 5, 10, 10), rect(5, 0, 20, 8), d);
    CHECK_EQ(call_import("EqualRect", 2, d, rect(0, 0, 20, 10)), 1);
    CHECK_EQ(call_import("EmptyRect", 1, d), 0);
    CHECK_EQ(call_import("PtInRect", 2, (3u << 16) | 4u, rect(0, 0, 10, 10)), 1); /* v 3, h 4 */
    CHECK_EQ(call_import("PtInRect", 2, (10u << 16) | 4u, rect(0, 0, 10, 10)), 0);
    CHECK_EQ(call_import("EqualPt", 2, 0x00030004u, 0x00030004u), 1);
    CHECK_EQ(call_import("EqualPt", 2, 0x00030004u, 0x00040003u), 0);
}
