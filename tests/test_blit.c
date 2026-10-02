#include "test.h"

#include "blit.h"
#include "util.h"

static const qd_rgb BLACK = {0, 0, 0}, WHITE = {0xFFFF, 0xFFFF, 0xFFFF};

TEST(blit_standard_8bit_palette) {
    qd_palette p;
    qd_std_palette(8, &p);
    CHECK_EQ(p.n, 256);
    CHECK_EQ(p.c[0].r, 0xFFFF); /* white first */
    CHECK_EQ(p.c[0].b, 0xFFFF);
    CHECK_EQ(p.c[255].r, 0);    /* black last */
    CHECK_EQ(p.c[5].b, 0x0000); /* 0xFFFF, 0xFFFF, 0x0000: yellow */
    CHECK_EQ(p.c[5].r, 0xFFFF);
    CHECK_EQ(p.c[215].r, 0xEEEE); /* the red ramp starts after the cube */
    CHECK_EQ(p.c[215].g, 0);
    CHECK_EQ(p.c[245].r, 0xEEEE); /* the gray ramp */
    CHECK_EQ(p.c[245].g, 0xEEEE);
    qd_std_palette(1, &p);
    CHECK_EQ(p.n, 2);
    CHECK_EQ(p.c[1].r, 0);
}

TEST(blit_pixel_values_for_colors) {
    qd_palette p;
    qd_std_palette(8, &p);
    CHECK_EQ(qd_pixel_for(WHITE, 8, &p), 0);
    CHECK_EQ(qd_pixel_for(BLACK, 8, &p), 255);
    CHECK_EQ(qd_pixel_for((qd_rgb){0xFFFF, 0, 0}, 16, NULL), 0x7C00);
    CHECK_EQ(qd_pixel_for((qd_rgb){0, 0xFFFF, 0}, 16, NULL), 0x03E0);
    CHECK_EQ(qd_pixel_for((qd_rgb){0x1234, 0xABCD, 0xFF00}, 32, NULL), 0x12ABFF);
}

static qd_pixels px(uint8_t *buf, int w, int h, int depth, const qd_palette *pal) {
    return (qd_pixels){buf, (uint32_t)((w * depth + 31) / 32 * 4), {0, 0, (int16_t)h, (int16_t)w},
                       depth, pal};
}

TEST(blit_8bit_to_16bit_converts_through_the_palette) {
    qd_palette p;
    qd_std_palette(8, &p);
    uint8_t s[4 * 2] = {0, 255, 5, 0, 0, 0, 0, 0};
    uint8_t d[8 * 2] = {0};
    qd_pixels src = px(s, 4, 2, 8, &p), dst = px(d, 4, 2, 16, NULL);
    qd_rect r = {0, 0, 1, 3};
    char err[128];
    CHECK(qd_blit(&src, r, &dst, r, dst.bounds, QD_SRC_COPY, BLACK, WHITE, err, sizeof err));
    CHECK_EQ(rd_be16(d), 0x7FFF);     /* white */
    CHECK_EQ(rd_be16(d + 2), 0x0000); /* black */
    CHECK_EQ(rd_be16(d + 4), 0x7FE0); /* yellow */
    CHECK_EQ(rd_be16(d + 6), 0);      /* outside the rect: untouched */
}

TEST(blit_same_palette_copies_indexes) {
    qd_palette p;
    qd_std_palette(8, &p);
    uint8_t s[4] = {17, 42, 99, 200}, d[4] = {0};
    qd_pixels src = px(s, 4, 1, 8, &p), dst = px(d, 4, 1, 8, &p);
    char err[128];
    CHECK(qd_blit(&src, src.bounds, &dst, dst.bounds, dst.bounds, QD_SRC_COPY, BLACK, WHITE, err,
                  sizeof err));
    CHECK(memcmp(s, d, 4) == 0);
}

TEST(blit_scales_with_nearest_neighbor) {
    uint8_t s[4] = {1, 2, 0, 0}; /* 2x1, padded rows */
    uint8_t d[8] = {0};
    qd_palette p;
    qd_std_palette(8, &p);
    qd_pixels src = px(s, 2, 1, 8, &p), dst = px(d, 4, 2, 8, &p);
    char err[128];
    CHECK(qd_blit(&src, (qd_rect){0, 0, 1, 2}, &dst, (qd_rect){0, 0, 2, 4}, dst.bounds, QD_SRC_COPY,
                  BLACK, WHITE, err, sizeof err));
    uint8_t want[8] = {1, 1, 2, 2, 1, 1, 2, 2};
    CHECK(memcmp(d, want, 8) == 0);
}

TEST(blit_respects_clip_and_destination_bounds) {
    qd_palette p;
    qd_std_palette(8, &p);
    uint8_t s[8] = {9, 9, 9, 9, 9, 9, 9, 9}, d[8] = {0};
    qd_pixels src = px(s, 8, 1, 8, &p), dst = px(d, 8, 1, 8, &p);
    char err[128];
    /* Destination rect hangs off the left edge; the clip cuts the right. */
    CHECK(qd_blit(&src, (qd_rect){0, 0, 1, 8}, &dst, (qd_rect){0, -2, 1, 6}, (qd_rect){0, 0, 1, 5},
                  QD_SRC_COPY, BLACK, WHITE, err, sizeof err));
    uint8_t want[8] = {9, 9, 9, 9, 9, 0, 0, 0};
    CHECK(memcmp(d, want, 8) == 0);
}

TEST(blit_1bit_sources_use_foreground_and_background) {
    qd_palette p1, p8;
    qd_std_palette(1, &p1);
    qd_std_palette(8, &p8);
    uint8_t s[4] = {0xA0, 0, 0, 0}; /* 1 0 1 0 */
    uint8_t d[4] = {7, 7, 7, 7};
    qd_pixels src = px(s, 4, 1, 1, &p1), dst = px(d, 4, 1, 8, &p8);
    char err[128];
    CHECK(qd_blit(&src, src.bounds, &dst, dst.bounds, dst.bounds, QD_SRC_COPY, BLACK, WHITE, err,
                  sizeof err));
    uint8_t want[4] = {255, 0, 255, 0};
    CHECK(memcmp(d, want, 4) == 0);
}

TEST(blit_4bit_pixels_pack_two_per_byte) {
    qd_palette p4, p8;
    qd_std_palette(4, &p4);
    qd_std_palette(8, &p8);
    uint8_t s[4] = {0xF0, 0, 0, 0}; /* black, white */
    uint8_t d[4] = {0};
    qd_pixels src = px(s, 2, 1, 4, &p4), dst = px(d, 2, 1, 8, &p8);
    char err[128];
    CHECK(qd_blit(&src, src.bounds, &dst, dst.bounds, dst.bounds, QD_SRC_COPY, BLACK, WHITE, err,
                  sizeof err));
    CHECK_EQ(d[0], 255);
    CHECK_EQ(d[1], 0);
}

TEST(blit_rejects_unsupported_modes) {
    qd_palette p;
    qd_std_palette(8, &p);
    uint8_t s[8] = {0}, d[8] = {0};
    qd_pixels s8 = px(s, 2, 1, 8, &p), d8 = px(d, 2, 1, 8, &p);
    char err[128] = "";
    CHECK(!qd_blit(&s8, s8.bounds, &d8, d8.bounds, d8.bounds, 36, BLACK, WHITE, err, sizeof err));
    CHECK_CONTAINS(err, "transfer mode 36");
}

TEST(blit_direct_to_indexed_picks_the_nearest_color) {
    qd_palette p;
    qd_std_palette(8, &p);
    uint8_t s[8] = {0x00, 0xFF, 0xFF, 0xFF, 0x00, 0xFE, 0x01, 0x02}; /* white, almost red */
    uint8_t d[2] = {7, 7};
    qd_pixels s32 = px(s, 2, 1, 32, NULL), d8 = px(d, 2, 1, 8, &p);
    char err[128] = "";
    CHECK(qd_blit(&s32, s32.bounds, &d8, d8.bounds, d8.bounds, QD_SRC_COPY, BLACK, WHITE, err,
                  sizeof err));
    CHECK_EQ(d[0], 0);  /* the standard palette's white */
    CHECK_EQ(d[1], 35); /* its pure red (0xFFFF, 0, 0) */
}

TEST(blit_fill_and_rgba_conversion) {
    uint8_t d[4 * 2] = {0};
    qd_pixels dst = px(d, 2, 2, 16, NULL);
    qd_fill(&dst, (qd_rect){0, 1, 2, 2}, dst.bounds, (qd_rgb){0xFFFF, 0, 0});
    uint8_t rgba[16];
    qd_to_rgba(&dst, rgba);
    CHECK_EQ(rgba[0], 0);
    CHECK_EQ(rgba[3], 255);
    CHECK_EQ(rgba[4], 255); /* (1,0) is red */
    CHECK_EQ(rgba[5], 0);
    CHECK_EQ(rgba[12], 255); /* (1,1) is red */
}

TEST(blit_rect_intersection) {
    qd_rect a = {0, 0, 10, 10}, b = {5, 5, 20, 20}, c = {11, 11, 12, 12};
    qd_rect r = rect_sect(a, b);
    CHECK_EQ(r.top, 5);
    CHECK_EQ(r.right, 10);
    CHECK(rect_empty(rect_sect(a, c)));
}
