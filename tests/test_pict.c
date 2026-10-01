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

TEST(pict_reports_quicktime_pictures) {
    SKIP_UNLESS_GAME();
    rsrc_entry *e = pict(128);
    CHECK(e != NULL);
    canvas c;
    canvas_init(&c, 104, 128);
    char err[256] = "";
    bool ok = draw(e, &c, err);
    free(c.buf);
    CHECK(!ok);
    CHECK_CONTAINS(err, "picture opcode 0x8201 is not supported");
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
