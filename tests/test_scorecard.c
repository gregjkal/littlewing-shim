#include "test.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cgimage.h"
#include "png.h"
#include "scorecard.h"

/* The shapes' margins and baselines, as scorecard.h gives them. */
typedef struct {
    scorecard_shape shape;
    int w, h, left, right, name_x;
    int size, big;  /* the two type sizes */
    int base[5];    /* the best's name, its score, the others */
} shape_info;
static const shape_info TALL = {SCORECARD_TALL, 1080, 1350, 72, 1008, 134, 42, 150, {806, 950, 1077, 1140, 1203}};
static const shape_info WIDE = {SCORECARD_WIDE, 1200, 675, 506, 1136, 552, 30, 92, {242, 332, 423, 468, 513}};

static const game_high_score FOUR[4] = {
    {430745960, "GREG", 0},
    {212380450, "GREG", 0},
    {96114700, "AMY", 0},
    {41902330, "GREG", 0},
};

static time_t november_10(void) {
    struct tm tm = {.tm_year = 126, .tm_mon = 10, .tm_mday = 10, .tm_hour = 12, .tm_isdst = -1};
    return mktime(&tm);
}

static scorecard card(const game_high_score *scores, int n) {
    return (scorecard){.title = "Loony Labyrinth", .scores = scores, .n = n, .when = november_10()};
}

static uint8_t *img;
static int img_w, img_h;

static bool draw(const scorecard *c, const shape_info *s) {
    free(img);
    img = scorecard_render(c, s->shape, &img_w, &img_h);
    return img != NULL;
}

/* Whether column x has a pixel with a channel above level in rows y0..y1. */
static bool ink_in_column(int x, int y0, int y1, int level) {
    for (int y = y0; y <= y1; y++) {
        const uint8_t *p = img + ((size_t)y * (size_t)img_w + (size_t)x) * 4;
        if (p[0] > level || p[1] > level || p[2] > level)
            return true;
    }
    return false;
}

/* The first and last columns with ink in x0..x1 (inclusive), or -1. */
static int first_ink(int x0, int x1, int y0, int y1, int level) {
    for (int x = x0; x <= x1; x++)
        if (ink_in_column(x, y0, y1, level))
            return x;
    return -1;
}
static int last_ink(int x0, int x1, int y0, int y1, int level) {
    for (int x = x1; x >= x0; x--)
        if (ink_in_column(x, y0, y1, level))
            return x;
    return -1;
}

/* The rows a line of capitals and digits of size takes above its baseline. */
static int line_top(int base, int size) { return base - size * 2 / 3; }

TEST(scorecard_has_the_two_shapes) {
    scorecard c = card(FOUR, 4);
    CHECK(draw(&c, &TALL));
    CHECK_EQ(img_w, 1080);
    CHECK_EQ(img_h, 1350);
    for (size_t i = 3; i < (size_t)img_w * (size_t)img_h * 4; i += 4 * 997)
        CHECK_EQ(img[i], 0xFF); /* opaque */
    CHECK(draw(&c, &WIDE));
    CHECK_EQ(img_w, 1200);
    CHECK_EQ(img_h, 675);
}

TEST(scorecard_without_scores_is_no_picture) {
    scorecard c = card(FOUR, 0);
    int w, h;
    CHECK(scorecard_render(&c, SCORECARD_TALL, &w, &h) == NULL);
}

static const uint8_t *at(int x, int y) { return img + ((size_t)y * (size_t)img_w + (size_t)x) * 4; }

/* A picture of the table at scale: red where the playfield is, green in
   the rows above it and the rail's under it. Written to path. */
static bool write_art(const char *path, int scale) {
    int w = SCORECARD_ART_W * scale, h = 527 * scale;
    uint8_t *px = malloc((size_t)w * (size_t)h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            bool table = y >= SCORECARD_ART_TOP * scale && y < (SCORECARD_ART_TOP + SCORECARD_ART_ROWS) * scale;
            uint8_t *p = px + ((size_t)y * (size_t)w + (size_t)x) * 4;
            p[0] = table ? 0xFF : 0;
            p[1] = table ? 0 : 0xFF;
            p[2] = 0;
            p[3] = 0xFF;
        }
    bool ok = png_write_rgba(path, px, w, h);
    free(px);
    return ok;
}

TEST(scorecard_shows_the_playfield_without_its_rails) {
    char dir[1024], path[1100];
    test_tmp_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/table.png", dir);
    for (int scale = 1; scale <= 4; scale += 3) {
        CHECK(write_art(path, scale));
        scorecard c = card(FOUR, 4);
        c.table = path;
        CHECK(draw(&c, &TALL));
        for (int y = 0; y < 600; y += 13)
            for (int x = 0; x < 1080; x += 7) {
                CHECK(at(x, y)[0] > 0xF0); /* a band as wide as the picture */
                CHECK(at(x, y)[1] < 0x10);
            }
        CHECK(at(0, 599)[0] > 0xF0);
        CHECK_EQ(at(540, 600)[0], 0); /* black under the band */
        CHECK(draw(&c, &WIDE));
        for (int y = 0; y < 675; y += 13)
            for (int x = 0; x < 443; x += 7) {
                CHECK(at(x, y)[0] > 0xF0); /* the whole playfield at the left */
                CHECK(at(x, y)[1] < 0x10);
            }
        CHECK_EQ(at(443, 300)[0], 0);
    }
    unlink(path);
}

TEST(scorecard_without_the_tables_picture_leaves_its_place_black) {
    scorecard c = card(FOUR, 4);
    CHECK(draw(&c, &TALL));
    CHECK_EQ(first_ink(0, 1079, 0, 599, 0), -1);
    c.table = "/nonexistent-folder/table.png";
    CHECK(draw(&c, &TALL));
    CHECK_EQ(first_ink(0, 1079, 0, 599, 0), -1);
}

/* The art the app carries is the picture the crop is measured on. */
TEST(scorecard_art_is_the_whole_table) {
    cgimage_pixels px;
    char err[256];
    CHECK(cgimage_decode_png(LOONY_SRC_DIR "/" SCORECARD_ART_DIR "/loony-labyrinth.png", &px, err, sizeof err));
    free(px.xrgb);
    CHECK_EQ(px.width % SCORECARD_ART_W, 0);
    CHECK_EQ(px.height, px.width / SCORECARD_ART_W * 527);
    scorecard c = card(FOUR, 4);
    c.table = LOONY_SRC_DIR "/" SCORECARD_ART_DIR "/loony-labyrinth.png";
    CHECK(draw(&c, &TALL));
    CHECK(first_ink(0, 0, 0, 599, 64) >= 0); /* art to both edges of the band */
    CHECK(first_ink(1079, 1079, 0, 599, 64) >= 0);
}

/* Every rank starts on the left margin, every name on one column, and
   every score ends on the right margin, the large one too. */
static bool columns_line_up(const shape_info *s) {
    scorecard c = card(FOUR, 4);
    if (!draw(&c, s))
        return false;
    int between = (s->left + s->name_x) / 2 + s->size / 4; /* past a rank, before a name */
    for (int row = 0; row < 4; row++) {
        int base = s->base[row ? row + 1 : 0], top = line_top(base, s->size);
        int rank = first_ink(0, between, top, base, 128);
        int name = first_ink(between, s->w - 1, top, base, 128);
        if (rank < s->left - 1 || rank > s->left + 1 || name < s->name_x - 1 || name > s->name_x + 1) {
            fprintf(stderr, "row %d: rank at %d, name at %d\n", row + 1, rank, name);
            return false;
        }
        int score_base = row ? base : s->base[1], size = row ? s->size : s->big;
        /* solid ink only: the large score's glow goes past its edge */
        int end = last_ink(s->name_x, s->w - 1, line_top(score_base, size), score_base, 200);
        if (end < s->right - 3 || end > s->right - 1) {
            fprintf(stderr, "row %d: score ends at %d, margin %d\n", row + 1, end, s->right);
            return false;
        }
    }
    return true;
}

TEST(scorecard_columns_line_up_tall) { CHECK(columns_line_up(&TALL)); }
TEST(scorecard_columns_line_up_wide) { CHECK(columns_line_up(&WIDE)); }

TEST(scorecard_draws_only_the_scores_there_are) {
    scorecard c = card(FOUR, 2);
    CHECK(draw(&c, &TALL));
    CHECK(first_ink(TALL.left, TALL.right, line_top(TALL.base[2], 42), TALL.base[2], 128) >= 0);
    CHECK_EQ(first_ink(0, 1079, line_top(TALL.base[3], 42), TALL.base[4] + 20, 0), -1);
}

TEST(scorecard_sets_a_long_score_smaller_to_fit) {
    game_high_score big[1] = {{9999999999990, "GREG", 0}};
    scorecard c = card(big, 1);
    for (int i = 0; i < 2; i++) {
        const shape_info *s = i ? &WIDE : &TALL;
        CHECK(draw(&c, s));
        int top = line_top(s->base[1], s->big), base = s->base[1];
        int start = first_ink(s->left - 40, s->w - 1, top, base, 200);
        int end = last_ink(s->left - 40, s->w - 1, top, base, 200);
        CHECK(start >= s->left - 1);
        CHECK(end >= s->right - 3 && end <= s->right - 1);
        CHECK(start < s->left + 40); /* and as large as fits */
    }
}

TEST(scorecard_cuts_a_name_short_of_its_score) {
    game_high_score rows[2] = {{430745960, "GREG", 0}, {212380450, "", 0}};
    memset(rows[1].name, 'W', sizeof rows[1].name - 1);
    scorecard c = card(rows, 2);
    CHECK(draw(&c, &TALL));
    int base = TALL.base[2], top = line_top(base, 42);
    /* from the score's end leftward: the score, then a gap a character wide or more */
    int x = last_ink(0, 1079, top, base, 128), gap = 0, widest = 0;
    for (; x > TALL.name_x; x--) {
        gap = ink_in_column(x, top, base, 128) ? 0 : gap + 1;
        if (gap > widest)
            widest = gap;
    }
    CHECK(widest >= 42);
    /* and the name is still there, most of the way across */
    CHECK(first_ink(TALL.name_x - 1, TALL.name_x + 1, top, base, 128) >= 0);
    CHECK(first_ink(500, 520, top, base, 128) >= 0);
}

TEST(scorecard_labels_are_set_on_the_margins) {
    scorecard c = card(FOUR, 4);
    CHECK(draw(&c, &TALL));
    /* the title, orange, from the left margin: five pixels a font pixel */
    CHECK_EQ(first_ink(0, 1079, 652, 691, 128), 72);
    const uint8_t *p = at(72, 652 + 2);
    CHECK(p[0] > 0xF0 && p[1] > 0x50 && p[1] < 0x64 && p[2] < 0x28);
    /* "HIGH SCORES" and the date end on the right margin */
    CHECK_EQ(last_ink(0, 1079, 652, 691, 128), 1007);
    CHECK_EQ(last_ink(0, 1079, 1286, 1301, 128), 1007);
    /* the link starts on the left one */
    CHECK_EQ(first_ink(0, 1079, 1286, 1301, 128), 72);
    CHECK(draw(&c, &WIDE));
    CHECK_EQ(first_ink(443, 1199, 52, 83, 128), 506);
    CHECK_EQ(last_ink(443, 1199, 96, 111, 128), 1135); /* the date, on the line under the title */
    CHECK_EQ(first_ink(443, 1199, 615, 630, 128), 506);
}

TEST(scorecard_writes_a_compressed_png) {
    char dir[1024], path[1100];
    test_tmp_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/scores.png", dir);
    scorecard c = card(FOUR, 4);
    CHECK(scorecard_write(&c, SCORECARD_TALL, path));
    cgimage_pixels px;
    char err[256];
    CHECK(cgimage_decode_png(path, &px, err, sizeof err));
    CHECK_EQ(px.width, 1080);
    CHECK_EQ(px.height, 1350);
    free(px.xrgb);
    struct stat st;
    CHECK_EQ(stat(path, &st), 0);
    CHECK(st.st_size < 1080 * 1350); /* a quarter of its pixels' bytes, at most */
    unlink(path);
    CHECK(!scorecard_write(&c, SCORECARD_TALL, "/nonexistent-folder/scores.png"));
}
