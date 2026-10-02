#include "test.h"

#include "font.h"

TEST(font_glyphs_come_from_sdl) {
    font_init();
    static const uint8_t a[FONT_H] = {0x30, 0x78, 0xCC, 0xCC, 0xFC, 0xCC, 0xCC, 0x00};
    CHECK(memcmp(font_glyph('A'), a, FONT_H) == 0);
    static const uint8_t blank[FONT_H] = {0};
    CHECK(memcmp(font_glyph(' '), blank, FONT_H) == 0);
    CHECK(font_glyph(0x8E) == font_glyph('?'));
    font_init(); /* again: harmless */
    CHECK(memcmp(font_glyph('A'), a, FONT_H) == 0);
}

TEST(font_mac_roman_text_is_spelled_in_ascii) {
    char out[64];
    const uint8_t mac[] = "Caf\x8e \xaa \xd2hi\xd3\r\x01";
    font_ascii(mac, sizeof mac - 1, out, sizeof out);
    CHECK_STR(out, "Cafe TM \"hi\"\r?");
    font_ascii(mac, sizeof mac - 1, out, 5); /* truncated, still terminated */
    CHECK_STR(out, "Cafe");
}

static int wrap(const char *text, int chars, char lines[][64]) {
    int starts[8], lens[8];
    int n = font_wrap(text, chars * FONT_W, 8, starts, lens);
    for (int i = 0; i < n && i < 8; i++)
        snprintf(lines[i], 64, "%.*s", lens[i], text + starts[i]);
    return n;
}

TEST(font_wrap_breaks_between_words) {
    char l[8][64];
    CHECK_EQ(wrap("hello world foo", 9, l), 2);
    CHECK_STR(l[0], "hello");
    CHECK_STR(l[1], "world foo");
    CHECK_EQ(wrap("hello world", 5, l), 2); /* a space right at the edge */
    CHECK_STR(l[0], "hello");
    CHECK_STR(l[1], "world");
    CHECK_EQ(wrap("abcdefghij", 4, l), 3); /* a word longer than a line */
    CHECK_STR(l[0], "abcd");
    CHECK_STR(l[2], "ij");
    CHECK_EQ(wrap("a\r\rb", 10, l), 3); /* hard breaks, and an empty line */
    CHECK_STR(l[0], "a");
    CHECK_STR(l[1], "");
    CHECK_STR(l[2], "b");
    CHECK_EQ(wrap("", 10, l), 1);
    CHECK_STR(l[0], "");
    CHECK_EQ(wrap("one two", 0, l), 6); /* under one character wide: one per line, no space */
    int s[2], n[2];
    CHECK_EQ(font_wrap("a b c d", FONT_W, 2, s, n), 4); /* more lines than room */
}

TEST(font_draw_sets_glyph_pixels_inside_the_clip) {
    font_init();
    uint8_t buf[16 * 16];
    memset(buf, 0, sizeof buf);
    qd_palette pal;
    qd_std_palette(8, &pal);
    qd_pixels px = {buf, 16, {0, 0, 16, 16}, 8, &pal};
    qd_rgb white = {0xFFFF, 0xFFFF, 0xFFFF};
    qd_rect clip = {0, 0, 16, 12};
    font_draw(&px, 2, 1, "AA", 2, white, clip);
    uint8_t w = (uint8_t)qd_pixel_for(white, 8, &pal);
    CHECK_EQ(buf[1 * 16 + 2 + 2], w);      /* row 0 of 'A' is ..##.... */
    CHECK_EQ(buf[1 * 16 + 2 + 0], 0);
    CHECK_EQ(buf[5 * 16 + 2 + 0], w);      /* row 4 is ######.. */
    CHECK_EQ(buf[5 * 16 + 10 + 0], w);     /* the second 'A' */
    CHECK_EQ(buf[5 * 16 + 10 + 4], 0);     /* x = 14: clipped (and . anyway) */
    CHECK_EQ(buf[5 * 16 + 10 + 3], 0);     /* x = 13: clipped at right 12 */
    CHECK_EQ(buf[5 * 16 + 11], w);         /* x = 11: inside */
}
