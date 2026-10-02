#include "test.h"

#include <stdlib.h>

#include "pict.h"
#include "rsrc.h"
#include "util.h"

static uint8_t *fork_buf;
static size_t fork_len;

static rsrc_entry *pict(int16_t id) {
    if (!fork_buf) {
        char path[1100];
        snprintf(path, sizeof path, "%s/..namedfork/rsrc", test_game_exe_path());
        fork_buf = read_file(path, &fork_len);
    }
    char err[256];
    if (!fork_buf || !rsrc_open(fork_buf, fork_len, err, sizeof err))
        return NULL;
    return rsrc_find(FOURCC('P', 'I', 'C', 'T'), id);
}

typedef struct {
    uint8_t *buf;
    qd_palette pal;
    qd_pixels px;
} canvas;

static void canvas_init(canvas *c, int w, int h) {
    qd_std_palette(8, &c->pal);
    uint32_t rb = (uint32_t)((w + 3) & ~3);
    c->buf = calloc(rb * (uint32_t)h, 1);
    c->px = (qd_pixels){c->buf, rb, {0, 0, (int16_t)h, (int16_t)w}, 8, &c->pal};
}

static bool draw(rsrc_entry *e, canvas *c, char *err) {
    return pict_draw(rsrc_data(e), e->len, c->px.bounds, &c->px, c->px.bounds, (qd_rgb){0, 0, 0},
                     (qd_rgb){0xFFFF, 0xFFFF, 0xFFFF}, err, 256);
}

/* The expected hashes come from an independent Python decoder (PackBits,
   the picture's color table, nearest standard-palette color). */
TEST(pict_decodes_the_title_picture) {
    SKIP_UNLESS_GAME();
    rsrc_entry *e = pict(800);
    CHECK(e != NULL);
    qd_rect frame;
    CHECK(pict_frame(rsrc_data(e), e->len, &frame));
    CHECK_EQ(rect_w(frame), 512);
    CHECK_EQ(rect_h(frame), 384);
    canvas c;
    canvas_init(&c, 512, 384);
    char err[256] = "";
    bool ok = draw(e, &c, err);
    uint32_t h = fnv1a32(c.buf, 512 * 384);
    free(c.buf);
    CHECK(ok);
    CHECK_EQ(h, 0x4657C203u);
}

TEST(pict_decodes_4bit_and_version_1_pictures) {
    SKIP_UNLESS_GAME();
    char err[256] = "";
    canvas c;
    rsrc_entry *e = pict(804); /* 309x80, 4 bits */
    CHECK(e != NULL);
    canvas_init(&c, 309, 80);
    bool ok = draw(e, &c, err);
    uint32_t h = fnv1a32(c.buf, c.px.row_bytes * 80);
    free(c.buf);
    CHECK(ok);
    CHECK_EQ(h, 0x958A1FF5u);
    e = pict(30000); /* version 1, 230x11 */
    CHECK(e != NULL);
    canvas_init(&c, 230, 11);
    ok = draw(e, &c, err);
    h = fnv1a32(c.buf, c.px.row_bytes * 11);
    free(c.buf);
    CHECK(ok);
    CHECK_EQ(h, 0xA2CE526Eu);
}

TEST(pict_scales_to_the_destination_rect) {
    SKIP_UNLESS_GAME();
    rsrc_entry *e = pict(800);
    CHECK(e != NULL);
    canvas c;
    canvas_init(&c, 256, 192);
    char err[256] = "";
    bool ok = draw(e, &c, err);
    /* Every destination pixel comes from the picture, so no white gaps remain
       where the canvas started out as 0 (white) apart from white pixels in
       the image itself: check a pixel known to be dark sand in the title. */
    uint8_t mid = c.buf[96 * c.px.row_bytes + 128];
    free(c.buf);
    CHECK(ok);
    CHECK(mid != 0);
}

/* The dialogs' icons: a QuickTime matte to skip, then a 32-bit
   DirectBitsRect packed as component planes. */
TEST(pict_draws_the_dialog_icons) {
    SKIP_UNLESS_GAME();
    for (int16_t id = 128; id <= 129; id++) {
        rsrc_entry *e = pict(id);
        CHECK(e != NULL);
        canvas c;
        canvas_init(&c, 104, 128);
        char err[256] = "";
        bool ok = draw(e, &c, err);
        int distinct = 0;
        bool seen[256] = {false};
        for (int i = 0; i < 104 * 128; i++)
            if (!seen[c.buf[i]]) {
                seen[c.buf[i]] = true;
                distinct++;
            }
        free(c.buf);
        CHECK_STR(err, "");
        CHECK(ok);
        CHECK(distinct > 8); /* a picture, not a flat fill */
    }
}

TEST(pict_rejects_truncated_pictures) {
    SKIP_UNLESS_GAME();
    rsrc_entry *e = pict(801);
    CHECK(e != NULL);
    size_t cuts[] = {0, 9, 12, 60, 600, 20000};
    for (size_t i = 0; i < sizeof cuts / sizeof cuts[0]; i++) {
        uint8_t *part = malloc(cuts[i] ? cuts[i] : 1);
        memcpy(part, rsrc_data(e), cuts[i]);
        canvas c;
        canvas_init(&c, 239, 231);
        char err[256] = "";
        bool ok = pict_draw(part, cuts[i], c.px.bounds, &c.px, c.px.bounds, (qd_rgb){0, 0, 0},
                            (qd_rgb){0xFFFF, 0xFFFF, 0xFFFF}, err, sizeof err);
        free(part);
        free(c.buf);
        CHECK(!ok);
        CHECK(err[0] != '\0');
    }
}

TEST(pict_version_1_bitmap_by_hand) {
    /* 4x2 frame; clip; BitsRect of a 1-bit 4x2 bitmap 1001/0110; end. */
    uint8_t pic[] = {
        0, 0, 0, 0, 0, 0, 0, 2, 0, 4,             /* size, frame 0,0,2,4 */
        0x11, 0x01,                               /* version 1 */
        0x01, 0, 10, 0, 0, 0, 0, 0, 2, 0, 4,      /* clip region */
        0x90, 0, 2, 0, 0, 0, 0, 0, 2, 0, 4,       /* BitsRect: rowBytes 2, bounds */
        0, 0, 0, 0, 0, 2, 0, 4,                   /* srcRect */
        0, 0, 0, 0, 0, 2, 0, 4,                   /* dstRect */
        0, 0,                                     /* srcCopy */
        0x90, 0x00, 0x60, 0x00,                   /* rows */
        0xFF,
    };
    canvas c;
    canvas_init(&c, 4, 2);
    char err[256] = "";
    bool ok = pict_draw(pic, sizeof pic, c.px.bounds, &c.px, c.px.bounds, (qd_rgb){0, 0, 0},
                        (qd_rgb){0xFFFF, 0xFFFF, 0xFFFF}, err, sizeof err);
    uint8_t want[8] = {255, 0, 0, 255, 0, 255, 255, 0};
    bool same = memcmp(c.buf, want, 4) == 0 && memcmp(c.buf + c.px.row_bytes, want + 4, 4) == 0;
    free(c.buf);
    CHECK(ok);
    CHECK(same);
}

TEST(pict_rejects_unknown_opcodes) {
    uint8_t pic[] = {0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0x11, 0x01, 0x22, 0, 0};
    canvas c;
    canvas_init(&c, 1, 1);
    char err[256] = "";
    bool ok = pict_draw(pic, sizeof pic, c.px.bounds, &c.px, c.px.bounds, (qd_rgb){0, 0, 0},
                        (qd_rgb){0xFFFF, 0xFFFF, 0xFFFF}, err, sizeof err);
    free(c.buf);
    CHECK(!ok);
    CHECK_CONTAINS(err, "picture opcode 0x0022 is not supported");
}

TEST(pict_odd_offset_at_the_end_is_truncated) {
    /* A v2 picture whose data ends right after a 1-byte long comment, at an
       odd offset, with no end opcode. Aligning must not step past the end. */
    uint8_t pic[] = {0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0x00, 0x11, 0x02, 0xFF,
                     0x00, 0xA1, 0, 0, 0, 1, 0x42};
    uint8_t *copy = malloc(sizeof pic); /* exact size, so ASan sees an over-read */
    memcpy(copy, pic, sizeof pic);
    canvas c;
    canvas_init(&c, 1, 1);
    char err[256] = "";
    bool ok = pict_draw(copy, sizeof pic, c.px.bounds, &c.px, c.px.bounds, (qd_rgb){0, 0, 0},
                        (qd_rgb){0xFFFF, 0xFFFF, 0xFFFF}, err, sizeof err);
    free(copy);
    free(c.buf);
    CHECK(!ok);
    CHECK_CONTAINS(err, "truncated");
}

TEST(pict_every_truncation_point_fails_cleanly) {
    SKIP_UNLESS_GAME();
    rsrc_entry *e = pict(804);
    CHECK(e != NULL);
    for (size_t cut = 0; cut < e->len; cut += 3) {
        uint8_t *part = malloc(cut ? cut : 1);
        memcpy(part, rsrc_data(e), cut);
        canvas c;
        canvas_init(&c, 309, 80);
        char err[256] = "";
        bool ok = pict_draw(part, cut, c.px.bounds, &c.px, c.px.bounds, (qd_rgb){0, 0, 0},
                            (qd_rgb){0xFFFF, 0xFFFF, 0xFFFF}, err, sizeof err);
        free(part);
        free(c.buf);
        CHECK(!ok);
    }
}

TEST(pict_rejects_corrupt_pictures) {
    /* clip region smaller than its header */
    uint8_t small_rgn[] = {0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0x11, 0x01, 0x01, 0, 4, 0, 0, 0, 0};
    /* BitsRect with rowBytes 1 for a 16-pixel-wide 1-bit bitmap */
    uint8_t short_rows[] = {0, 0, 0, 0, 0, 0, 0, 1, 0, 16, 0x11, 0x01, 0x90, 0, 1, 0, 0, 0, 0,
                            0, 1, 0, 16, 0, 0, 0, 0, 0, 1, 0, 16, 0, 0, 0, 0, 0, 1, 0, 16,
                            0, 0, 0xFF, 0xFF};
    const uint8_t *pics[] = {small_rgn, short_rows};
    size_t lens[] = {sizeof small_rgn, sizeof short_rows};
    for (int i = 0; i < 2; i++) {
        canvas c;
        canvas_init(&c, 16, 1);
        char err[256] = "";
        bool ok = pict_draw(pics[i], lens[i], c.px.bounds, &c.px, c.px.bounds, (qd_rgb){0, 0, 0},
                            (qd_rgb){0xFFFF, 0xFFFF, 0xFFFF}, err, sizeof err);
        free(c.buf);
        CHECK(!ok);
        CHECK(err[0] != '\0');
    }
}

TEST(pict_scaling_samples_every_other_pixel) {
    SKIP_UNLESS_GAME();
    rsrc_entry *e = pict(800);
    CHECK(e != NULL);
    canvas full, half;
    canvas_init(&full, 512, 384);
    canvas_init(&half, 256, 192);
    char err[256] = "";
    bool ok = draw(e, &full, err) && draw(e, &half, err);
    bool same = true;
    for (int y = 0; y < 192 && same; y++)
        for (int x = 0; x < 256 && same; x++)
            same = half.buf[y * half.px.row_bytes + x] ==
                   full.buf[2 * y * full.px.row_bytes + 2 * x];
    free(full.buf);
    free(half.buf);
    CHECK(ok);
    CHECK(same);
}

static void child_huge_dst(void *unused) {
    (void)unused;
    uint8_t pic[] = {0, 0, 0, 0, 0, 0, 0, 2, 0, 4, 0x11, 0x01, 0x90, 0, 2, 0, 0, 0, 0, 0, 2, 0, 4,
                     0, 0, 0, 0, 0, 2, 0, 4, 0, 0, 0, 0, 0, 2, 0, 4, 0, 0,
                     0x90, 0x00, 0x60, 0x00, 0xFF};
    canvas c;
    canvas_init(&c, 4, 2);
    char err[256];
    pict_draw(pic, sizeof pic, (qd_rect){-32768, -32768, 32767, 32767}, &c.px, c.px.bounds,
              (qd_rgb){0, 0, 0}, (qd_rgb){0xFFFF, 0xFFFF, 0xFFFF}, err, sizeof err);
    free(c.buf);
}

TEST(pict_huge_destination_rects_are_well_defined) {
    char out[8192];
    CHECK_EQ(test_run_child(child_huge_dst, NULL, out, sizeof out), 0);
    CHECK(!strstr(out, "runtime error"));
}

/* A version 2 picture holding only an UncompressedQuickTime opcode whose
   matte's image description claims id_size and data_size. */
static size_t qt_picture(uint8_t *b, uint32_t id_size, uint32_t data_size) {
    memset(b, 0, 400);
    size_t n = 0;
    wr_be16(b + 6, 10); /* frame 0,0,10,10 */
    wr_be16(b + 8, 10);
    n = 10;
    wr_be16(b + n, 0x0011);
    wr_be16(b + n + 2, 0x02FF);
    n += 4;
    wr_be16(b + n, 0x0C00);
    n += 2 + 24;
    wr_be16(b + n, 0x8201);
    n += 2;
    uint32_t body = 2 + 36 + 4 + 8 + 86 + 16;
    wr_be32(b + n, body);
    n += 4;
    uint8_t *q = b + n;
    wr_be32(q + 38, 86 + 16); /* matte size */
    uint8_t *id = q + 50;
    wr_be32(id, id_size);
    memcpy(id + 4, "rle ", 4);
    wr_be16(id + 32, 4); /* 4 x 4 */
    wr_be16(id + 34, 4);
    wr_be32(id + 44, data_size);
    wr_be16(id + 82, 40);
    n += body;
    wr_be16(b + n, 0x00FF);
    return n + 2;
}

/* Review Focus 1: sizes that would overflow a 32-bit sum. */
TEST(pict_quicktime_matte_sizes_cant_overflow) {
    uint8_t b[400];
    canvas c;
    canvas_init(&c, 10, 10);
    uint32_t sizes[3][2] = {{0xFFFFFFF0u, 0x20}, {100, 0xFFFFFFF0u}, {86, 0xFFFFFFFFu}};
    for (int i = 0; i < 3; i++) {
        size_t len = qt_picture(b, sizes[i][0], sizes[i][1]);
        char err[256] = "";
        CHECK(pict_draw(b, len, c.px.bounds, &c.px, c.px.bounds, (qd_rgb){0, 0, 0},
                        (qd_rgb){0xFFFF, 0xFFFF, 0xFFFF}, err, sizeof err)); /* matte ignored */
    }
    free(c.buf);
}
