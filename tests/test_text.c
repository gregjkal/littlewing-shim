#include "test.h"

#include "harness.h"
#include "memmgr.h"
#include "pen.h"
#include "qd.h"
#include "region.h"
#include "text.h"

static const char *const names[] = {
    "NewGWorld",      "SetGWorld",       "GetGWorldPixMap", "GetPixBaseAddr",  "RGBForeColor",
    "RGBBackColor",   "ClipRect",        "MoveTo",          "GetPen",          "TextFont",
    "TextSize",       "TextFace",        "TextMode",        "GetPortTextFont", "GetPortTextSize",
    "GetPortTextFace", "GetPortTextMode", "GetFNum",        "GetAppFont",      "GetSysFont",
    "GetFontInfo",    "FMSwapFont",      "DrawString",      "DrawText",        "DrawChar",
    "StringWidth",    "TextWidth",       "CharWidth",       "EqualString",     "IdenticalString",
    "IdenticalText",  "CompareText",     "StringToNum",
};

#define RED 0xFF0000u
#define WHITE 0xFFFFFFu
#define GREEN 0x00FF00u
#define W 64
#define H 32

static uint32_t base, row_bytes, gw;

static uint32_t rect(int top, int left, int bottom, int right) {
    uint32_t r = scratch(8);
    qd_write_rect(r, (qd_rect){(int16_t)top, (int16_t)left, (int16_t)bottom, (int16_t)right});
    return r;
}

static uint32_t pstr(const char *s) {
    uint32_t p = scratch(256);
    gm_write_pstr(p, s);
    return p;
}

/* A 32-bit 64x32 GWorld, all green, the current port, drawing red on white. */
static void setup(void) {
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    qd_init(800, 600, 8);
    qd_register();
    region_register();
    pen_register();
    text_register();
    uint32_t out = scratch(4);
    if (call_import("NewGWorld", 6, out, 32u, rect(0, 0, H, W), 0u, 0u, 0u) != 0)
        fatal("NewGWorld failed");
    gw = gm_r32(out);
    call_import("SetGWorld", 2, gw, 0u);
    uint32_t pm = call_import("GetGWorldPixMap", 1, gw);
    base = call_import("GetPixBaseAddr", 1, pm);
    row_bytes = gm_r16(gm_r32(pm) + PM_ROW_BYTES) & 0x3FFF;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            gm_w32(base + (uint32_t)y * row_bytes + 4u * (uint32_t)x, GREEN);
    uint32_t c = scratch(6);
    qd_write_rgb(c, (qd_rgb){0xFFFF, 0, 0});
    call_import("RGBForeColor", 1, c);
}

static uint32_t px(int x, int y) { return gm_r32(base + (uint32_t)y * row_bytes + 4u * (uint32_t)x) & 0xFFFFFF; }

/* The box holding every pixel of color c, and how many there are. */
static int box(uint32_t c, qd_rect *b) {
    int n = 0;
    *b = (qd_rect){H, W, 0, 0};
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (px(x, y) == c) {
                n++;
                b->top = (int16_t)(y < b->top ? y : b->top);
                b->left = (int16_t)(x < b->left ? x : b->left);
                b->bottom = (int16_t)(y + 1 > b->bottom ? y + 1 : b->bottom);
                b->right = (int16_t)(x + 1 > b->right ? x + 1 : b->right);
            }
    return n;
}

static int pen_h(void) {
    uint32_t pt = scratch(4);
    call_import("GetPen", 1, pt);
    return (int16_t)gm_r16(pt + 2);
}

TEST(text_draws_on_the_baseline_and_moves_the_pen_its_width) {
    setup();
    call_import("TextFont", 1, 3u); /* Geneva */
    call_import("TextSize", 1, 12u);
    uint32_t info = scratch(8);
    call_import("GetFontInfo", 1, info);
    int ascent = (int16_t)gm_r16(info), descent = (int16_t)gm_r16(info + 2);
    CHECK(ascent >= 9 && ascent <= 14);
    CHECK(descent >= 2 && descent <= 5);
    CHECK((int16_t)gm_r16(info + 4) >= 8); /* widMax */
    int width = (int16_t)call_import("StringWidth", 1, pstr("Egg"));
    CHECK(width >= 15 && width <= 30);
    CHECK_EQ((int16_t)call_import("TextWidth", 3, pstr("xEggx"), 2u, 3u), width);
    call_import("MoveTo", 2, 4u, 20u);
    call_import("DrawString", 1, pstr("Egg"));
    CHECK_EQ(pen_h(), 4 + width);
    qd_rect b;
    CHECK(box(RED, &b) > 20);
    CHECK(b.left >= 4 && b.right <= 4 + width + 1);
    CHECK(b.top >= 20 - ascent && b.bottom <= 20 + descent);
    CHECK(b.bottom > 20); /* the g descends */
    CHECK_EQ(box(WHITE, &b), 0); /* srcOr: no background */
}

TEST(text_modes_faces_and_clipping) {
    setup();
    call_import("TextFont", 1, 3u);
    call_import("TextSize", 1, 12u);
    CHECK_EQ(call_import("GetPortTextMode", 1, gw), 1); /* srcOr */
    int plain = (int16_t)call_import("StringWidth", 1, pstr("Egg"));
    call_import("TextFace", 1, 1u); /* bold */
    CHECK_EQ(call_import("GetPortTextFace", 1, gw), 1);
    CHECK_EQ((int16_t)call_import("StringWidth", 1, pstr("Egg")), plain + 3);
    call_import("TextFace", 1, 0u);
    CHECK_EQ((int16_t)call_import("CharWidth", 1, (uint32_t)'g'),
             (int16_t)call_import("StringWidth", 1, pstr("g")));

    call_import("TextMode", 1, 0u); /* srcCopy: the line's box in the background color */
    call_import("MoveTo", 2, 2u, 16u);
    call_import("DrawText", 3, pstr("xEg"), 1u, 2u);
    qd_rect b;
    CHECK(box(WHITE, &b) > 0);
    CHECK(box(RED, &b) > 0);
    CHECK(b.left >= 2);

    setup();
    call_import("TextFont", 1, 3u);
    call_import("ClipRect", 1, rect(0, 0, H, 10));
    call_import("MoveTo", 2, 12u, 20u);
    call_import("DrawString", 1, pstr("Egg"));
    CHECK_EQ(box(RED, &b), 0);
    call_import("MoveTo", 2, 2u, 20u);
    call_import("DrawChar", 1, (uint32_t)'M');
    CHECK(box(RED, &b) > 0);
    CHECK(b.right <= 10);
}

TEST(text_fonts_by_number_and_name) {
    setup();
    uint32_t num = scratch(2);
    call_import("GetFNum", 2, pstr("geneva"), num);
    CHECK_EQ(gm_r16(num), 3);
    call_import("GetFNum", 2, pstr("Chicago"), num);
    CHECK_EQ(gm_r16(num), 0);
    call_import("GetFNum", 2, pstr("Monaco"), num);
    CHECK_EQ(gm_r16(num), 4);
    call_import("GetFNum", 2, pstr("No Such Font"), num);
    CHECK_EQ(gm_r16(num), 0);
    CHECK_EQ(call_import("GetAppFont", 0), 1);
    CHECK_EQ(call_import("GetSysFont", 0), 0);
    call_import("TextFont", 1, 4u);
    call_import("TextSize", 1, 9u);
    CHECK_EQ(call_import("GetPortTextFont", 1, gw), 4);
    CHECK_EQ(call_import("GetPortTextSize", 1, gw), 9);
    /* Monaco is monospaced. */
    CHECK_EQ((int16_t)call_import("StringWidth", 1, pstr("iiii")),
             (int16_t)call_import("StringWidth", 1, pstr("WWWW")));
    CHECK_EQ((int16_t)call_import("StringWidth", 1, pstr("")), 0);

    uint32_t in = scratch(16);
    gm_w16(in, 3);
    gm_w16(in + 2, 9);
    gm_w8(in + 4, 1); /* bold */
    uint32_t out = call_import("FMSwapFont", 1, in);
    CHECK(out != 0);
    CHECK_EQ(gm_r16(out), 0); /* errNum */
    CHECK_EQ(gm_r8(out + 6), 1); /* boldPixels */
    CHECK(gm_r8(out + 13) >= 7);  /* ascent */
    CHECK_EQ(gm_r8(out + 17), 1); /* curStyle */
    CHECK_EQ(gm_r16(out + 18), 1);
}

TEST(text_utilities_compare_strings_and_read_numbers) {
    setup();
    CHECK_EQ(call_import("EqualString", 4, pstr("Angel"), pstr("ANGEL"), 0u, 0u), 1);
    CHECK_EQ(call_import("EqualString", 4, pstr("Angel"), pstr("ANGEL"), 1u, 0u), 0);
    CHECK_EQ(call_import("EqualString", 4, pstr("Angel"), pstr("Angels"), 0u, 0u), 0);
    CHECK_EQ(call_import("IdenticalString", 3, pstr("egg"), pstr("egg"), 0u), 0);
    CHECK_EQ(call_import("IdenticalString", 3, pstr("egg"), pstr("Egg"), 0u), 1);
    uint32_t a = pstr("apple"), b = pstr("Banana");
    CHECK_EQ(call_import("IdenticalText", 5, a + 1, a + 1, 5u, 5u, 0u), 0);
    CHECK_EQ((int32_t)call_import("CompareText", 5, a + 1, b + 1, 5u, 6u, 0u), -1);
    CHECK_EQ((int32_t)call_import("CompareText", 5, b + 1, a + 1, 6u, 5u, 0u), 1);
    CHECK_EQ((int32_t)call_import("CompareText", 5, a + 1, a + 1, 5u, 5u, 0u), 0);
    uint32_t n = scratch(4);
    call_import("StringToNum", 2, pstr("-1234"), n);
    CHECK_EQ((int32_t)gm_r32(n), -1234);
    call_import("StringToNum", 2, pstr("42"), n);
    CHECK_EQ(gm_r32(n), 42);
}
