#include "test.h"

#include <SDL3/SDL_scancode.h>
#include <stdlib.h>

#include "game.h"
#include "picker.h"
#include "png.h"
#include "util.h"

static uint8_t screen[PICKER_W * PICKER_H * 4];

static uint8_t *solid_art(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *a = malloc(PICKER_ART_W * PICKER_ART_H * 4);
    for (int i = 0; i < PICKER_ART_W * PICKER_ART_H; i++) {
        a[i * 4] = 0;
        a[i * 4 + 1] = r;
        a[i * 4 + 2] = g;
        a[i * 4 + 3] = b;
    }
    return a;
}

static const uint8_t *px(int x, int y) { return screen + ((size_t)y * PICKER_W + (size_t)x) * 4; }

TEST(picker_draw_places_the_cards_and_marks_the_selection) {
    picker_entry e[2] = {{game_at(0), solid_art(0xFF, 0, 0)}, {game_at(1), solid_art(0, 0, 0xFF)}};
    picker_draw(e, 2, 1, screen);
    free(e[0].art);
    free(e[1].art);
    CHECK_EQ(px(10 + 100, 140 + 100)[1], 0xFF); /* the first card's art */
    CHECK_EQ(px(406 + 100, 140 + 100)[3], 0xFF); /* the second card's art */
    CHECK_EQ(px(406 - 2, 200)[1], 0xFF);         /* gold border: the second is selected */
    CHECK_EQ(px(406 - 2, 200)[2], 0xCC);
    CHECK_EQ(px(10 - 2, 200)[1], 0x33);          /* gray border on the first */
    CHECK_EQ(px(0, 0)[1], 0);                    /* black background */
}

/* Review Focus 4: a game whose art couldn't be read gets a plain card. */
TEST(picker_draw_without_art) {
    picker_entry e[2] = {{game_at(0), NULL}, {game_at(1), NULL}};
    picker_draw(e, 2, 0, screen);
    CHECK_EQ(px(10 + 100, 140 + 100)[1], 0x22);
    CHECK_EQ(px(406 + 100, 140 + 100)[1], 0x22);
}

/* The whole frame, with solid art, pinned by a hash recorded when this
   task was implemented (picker_draw is deterministic). */
TEST(picker_frame_matches_its_recording) {
    picker_entry e[2] = {{game_at(0), solid_art(0x80, 0x40, 0x20)}, {game_at(1), NULL}};
    picker_draw(e, 2, 0, screen);
    free(e[0].art);
    char got[16];
    snprintf(got, sizeof got, "%08x", fnv1a32(screen, sizeof screen));
    CHECK_STR(got, "df066ba5");
}

TEST(picker_hit_finds_the_card_under_a_click) {
    CHECK_EQ(picker_hit(2, 10 + 5, 140 + 5), 0);
    CHECK_EQ(picker_hit(2, 406 + 383, 140 + 287), 1);
    CHECK_EQ(picker_hit(2, 400, 300), -1); /* the gap */
    CHECK_EQ(picker_hit(2, 100, 60), -1);  /* the heading */
    CHECK_EQ(picker_hit(2, 100, 460), 0);  /* the name under a card counts */
}

TEST(picker_keys_move_and_choose) {
    bool choose = false;
    CHECK_EQ(picker_key(2, 0, SDL_SCANCODE_RIGHT, &choose), 1);
    CHECK(!choose);
    CHECK_EQ(picker_key(2, 1, SDL_SCANCODE_RIGHT, &choose), 1); /* stops at the end */
    CHECK_EQ(picker_key(2, 1, SDL_SCANCODE_LEFT, &choose), 0);
    CHECK_EQ(picker_key(2, 0, SDL_SCANCODE_LEFT, &choose), 0);
    CHECK_EQ(picker_key(2, 1, SDL_SCANCODE_RETURN, &choose), 1);
    CHECK(choose);
    choose = false;
    CHECK_EQ(picker_key(2, 0, SDL_SCANCODE_KP_ENTER, &choose), 0);
    CHECK(choose);
    choose = false;
    CHECK_EQ(picker_key(2, 0, SDL_SCANCODE_Z, &choose), 0);
    CHECK(!choose);
}

TEST(picker_starts_on_the_last_game) {
    picker_entry e[2] = {{game_at(0), NULL}, {game_at(1), NULL}};
    CHECK_EQ(picker_initial(e, 2, "crystal-caliburn"), 1);
    CHECK_EQ(picker_initial(e, 2, "loony-labyrinth"), 0);
    CHECK_EQ(picker_initial(e, 2, ""), 0);
    CHECK_EQ(picker_initial(e, 2, "pacman"), 0);
}

/* Three games: 240x180 cards at x = 16, 280 and 544, from y = 180. */
TEST(picker_draws_three_cards) {
    picker_entry e[3] = {{game_at(0), solid_art(0xFF, 0, 0)}, {game_at(1), NULL},
                         {game_at(2), solid_art(0, 0, 0xFF)}};
    picker_draw(e, 3, 2, screen);
    free(e[0].art);
    free(e[2].art);
    CHECK_EQ(px(16, 180)[1], 0xFF);               /* the first card's art, reduced */
    CHECK_EQ(px(16 + 239, 180 + 179)[1], 0xFF);
    CHECK_EQ(px(16 + 240, 200)[1], 0x33);         /* its gray border */
    CHECK_EQ(px(280 + 100, 180 + 90)[1], 0x22);   /* the second: a plain card */
    CHECK_EQ(px(544 + 100, 180 + 90)[3], 0xFF);   /* the third card's art */
    CHECK_EQ(px(544 - 2, 200)[1], 0xFF);          /* gold border: the third is selected */
    CHECK_EQ(px(544 - 2, 200)[2], 0xCC);
    CHECK_EQ(px(268, 200)[1], 0);                 /* black between the cards */
}

TEST(picker_left_and_right_move_across_three) {
    bool choose = false;
    CHECK_EQ(picker_key(3, 0, SDL_SCANCODE_RIGHT, &choose), 1);
    CHECK_EQ(picker_key(3, 1, SDL_SCANCODE_RIGHT, &choose), 2);
    CHECK_EQ(picker_key(3, 2, SDL_SCANCODE_RIGHT, &choose), 2); /* stops at the end, as with two */
    CHECK_EQ(picker_key(3, 2, SDL_SCANCODE_LEFT, &choose), 1);
    CHECK(!choose);
    CHECK_EQ(picker_hit(3, 16 + 5, 180 + 5), 0);
    CHECK_EQ(picker_hit(3, 280 + 239, 180 + 179), 1);
    CHECK_EQ(picker_hit(3, 544 + 100, 400), 2); /* the name under a card counts */
    CHECK_EQ(picker_hit(3, 268, 200), -1);      /* a gap */
    CHECK_EQ(picker_hit(3, 300, 100), -1);      /* the heading */
}

/* Four games: 224x168 cards in two rows of two, at x = 144 and 432, from
   y = 108 and 324. */
TEST(picker_draws_four_cards_in_two_rows) {
    picker_entry e[4] = {{game_at(0), solid_art(0xFF, 0, 0)}, {game_at(1), NULL}, {game_at(2), NULL},
                         {game_at(3), solid_art(0, 0, 0xFF)}};
    picker_draw(e, 4, 3, screen);
    free(e[0].art);
    free(e[3].art);
    CHECK_EQ(px(144, 108)[1], 0xFF);              /* the first card's art, reduced */
    CHECK_EQ(px(144 + 223, 108 + 167)[1], 0xFF);
    CHECK_EQ(px(144 + 224, 200)[1], 0x33);        /* its gray border */
    CHECK_EQ(px(432 + 100, 108 + 80)[1], 0x22);   /* the second: a plain card, beside it */
    CHECK_EQ(px(144 + 100, 324 + 80)[1], 0x22);   /* the third: a plain card, under the first */
    CHECK_EQ(px(432 + 100, 324 + 80)[3], 0xFF);   /* the fourth card's art */
    CHECK_EQ(px(432 - 2, 400)[1], 0xFF);          /* gold border: the fourth is selected */
    CHECK_EQ(px(432 - 2, 400)[2], 0xCC);
    CHECK_EQ(px(400, 200)[1], 0);                 /* black between the columns */
    CHECK_EQ(px(200, 318)[1], 0);                 /* and between the rows */
}

TEST(picker_keys_move_across_and_between_rows_of_four) {
    bool choose = false;
    CHECK_EQ(picker_key(4, 0, SDL_SCANCODE_RIGHT, &choose), 1);
    CHECK_EQ(picker_key(4, 1, SDL_SCANCODE_RIGHT, &choose), 2); /* on to the next row */
    CHECK_EQ(picker_key(4, 3, SDL_SCANCODE_RIGHT, &choose), 3);
    CHECK_EQ(picker_key(4, 0, SDL_SCANCODE_DOWN, &choose), 2);
    CHECK_EQ(picker_key(4, 3, SDL_SCANCODE_UP, &choose), 1);
    CHECK_EQ(picker_key(4, 1, SDL_SCANCODE_UP, &choose), 1);   /* already on the top row */
    CHECK_EQ(picker_key(4, 2, SDL_SCANCODE_DOWN, &choose), 2); /* already on the bottom row */
    CHECK_EQ(picker_key(3, 1, SDL_SCANCODE_DOWN, &choose), 1); /* one row: Up and Down do nothing */
    CHECK_EQ(picker_key(2, 0, SDL_SCANCODE_UP, &choose), 0);
    CHECK(!choose);
    CHECK_EQ(picker_hit(4, 144 + 5, 108 + 5), 0);
    CHECK_EQ(picker_hit(4, 432 + 223, 108 + 167), 1);
    CHECK_EQ(picker_hit(4, 144 + 100, 290), 0);  /* the name under a card counts */
    CHECK_EQ(picker_hit(4, 432 + 100, 506), 3);
    CHECK_EQ(picker_hit(4, 144 + 100, 330), 2);
    CHECK_EQ(picker_hit(4, 400, 200), -1);       /* a gap */
    CHECK_EQ(picker_hit(4, 300, 70), -1);        /* the heading */
}

/* A 128x128 icon, its left half opaque red and its right half clear. */
static void write_icon(const char *path, int size) {
    uint8_t *rgba = calloc((size_t)size * (size_t)size, 4);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size / 2; x++) {
            rgba[(y * size + x) * 4] = 0xFF;
            rgba[(y * size + x) * 4 + 3] = 0xFF;
        }
    if (!png_write_rgba(path, rgba, size, size))
        fatal("can't write %s", path);
    free(rgba);
}

TEST(picker_draws_an_icon_card_for_a_bundle_game) {
    char dir[1024], path[1100], big[1100], err[512];
    test_tmp_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/appl.png", dir);
    snprintf(big, sizeof big, "%s/big.png", dir);
    write_icon(path, 128);
    write_icon(big, 400);
    uint8_t *art = malloc(PICKER_ART_W * PICKER_ART_H * 4);
    bool ok = picker_load_icon(path, art, err, sizeof err);
    bool too_big = picker_load_icon(big, art + 0, err, sizeof err);
    test_remove_tree(dir);
    CHECK(ok);
    CHECK(!too_big);
    CHECK_CONTAINS(err, "larger than a card");
    const uint8_t *in_left = art + ((size_t)144 * PICKER_ART_W + 128 + 10) * 4;
    const uint8_t *in_right = art + ((size_t)144 * PICKER_ART_W + 128 + 100) * 4;
    const uint8_t *outside = art + ((size_t)5 * PICKER_ART_W + 5) * 4;
    bool red = in_left[1] == 0xFF && in_left[2] == 0 && in_left[3] == 0;
    bool clear_is_card = in_right[1] == 0x22 && in_right[3] == 0x22; /* over the card, not white */
    bool around_is_card = outside[1] == 0x22;
    free(art);
    CHECK(red);
    CHECK(clear_is_card);
    CHECK(around_is_card);
}

/* An icns holding a 128x128 it32 (red: each plane one color, as runs of
   130 and a run of 4) and a t8mk whose left half is opaque. Returns its
   length; cut_it32 bytes are dropped from the end of the it32, and
   with_mask leaves out the t8mk. */
static size_t build_icns(uint8_t *out, size_t cut_it32, bool with_mask) {
    static const uint8_t color[3] = {0xFF, 0, 0};
    uint8_t *it32 = out + 8;
    size_t n = 8 + 4; /* the it32 header, then four zero bytes */
    memcpy(it32, "it32", 4);
    memset(it32 + 8, 0, 4);
    for (int c = 0; c < 3; c++)
        for (int left = 128 * 128; left > 0;) {
            int run = left >= 130 ? 130 : left;
            it32[n++] = (uint8_t)(0x80 + run - 3);
            it32[n++] = color[c];
            left -= run;
        }
    n -= cut_it32;
    for (int b = 0; b < 4; b++)
        it32[4 + b] = (uint8_t)(n >> (24 - 8 * b));
    size_t len = 8 + n;
    if (with_mask) {
        uint8_t *m = out + len;
        memcpy(m, "t8mk", 4);
        size_t mn = 8 + 128 * 128;
        for (int b = 0; b < 4; b++)
            m[4 + b] = (uint8_t)(mn >> (24 - 8 * b));
        for (int i = 0; i < 128 * 128; i++)
            m[8 + i] = i % 128 < 64 ? 0xFF : 0;
        len += mn;
    }
    memcpy(out, "icns", 4);
    for (int b = 0; b < 4; b++)
        out[4 + b] = (uint8_t)(len >> (24 - 8 * b));
    return len;
}

TEST(picker_draws_an_icon_card_from_an_icns) {
    static uint8_t icns[64 * 1024];
    size_t len = build_icns(icns, 0, true);
    uint8_t *art = malloc(PICKER_ART_W * PICKER_ART_H * 4);
    bool ok = picker_load_icns(icns, len, art);
    const uint8_t *in_left = art + ((size_t)144 * PICKER_ART_W + 128 + 10) * 4;
    const uint8_t *in_right = art + ((size_t)144 * PICKER_ART_W + 128 + 100) * 4;
    const uint8_t *outside = art + ((size_t)5 * PICKER_ART_W + 5) * 4;
    bool red = in_left[1] == 0xFF && in_left[2] == 0 && in_left[3] == 0;
    bool clear_is_card = in_right[1] == 0x22 && in_right[2] == 0x22 && in_right[3] == 0x22;
    bool around_is_card = outside[1] == 0x22 && outside[3] == 0x22;
    free(art);
    CHECK(ok);
    CHECK(red);
    CHECK(clear_is_card);
    CHECK(around_is_card);
}

/* A damaged icon is refused, never read past its end. */
TEST(picker_refuses_a_damaged_icns) {
    static uint8_t icns[64 * 1024];
    uint8_t *art = malloc(PICKER_ART_W * PICKER_ART_H * 4);
    size_t len = build_icns(icns, 1, true); /* the blue plane's last color byte is missing */
    bool truncated = picker_load_icns(icns, len, art);
    len = build_icns(icns, 0, false);
    bool no_mask = picker_load_icns(icns, len, art);
    len = build_icns(icns, 0, true);
    bool cut_short = picker_load_icns(icns, len - 100, art); /* the mask runs past the end */
    icns[8 + 7] = 4;                                         /* an element shorter than its header */
    icns[8 + 4] = icns[8 + 5] = icns[8 + 6] = 0;
    bool tiny_element = picker_load_icns(icns, len, art);
    len = build_icns(icns, 0, true);
    icns[8 + 12 + 126 * 2] = 0xFF; /* the red plane's last run, 4, becomes 130: past the plane */
    bool overlong = picker_load_icns(icns, len, art);
    free(art);
    CHECK(!truncated);
    CHECK(!no_mask);
    CHECK(!cut_short);
    CHECK(!tiny_element);
    CHECK(!overlong);
}

/* Jinni Zeala has no PICT 800: its card is its icns 128 icon, centered. */
TEST(picker_art_for_jinni_zeala_is_its_icon) {
    SKIP_UNLESS_JZ();
    uint8_t *art = malloc(PICKER_ART_W * PICKER_ART_H * 4);
    char err[256] = "";
    bool ok = picker_load_art(test_jz_exe_path(), art, err, sizeof err);
    const uint8_t *corner = art + ((size_t)5 * PICKER_ART_W + 5) * 4;
    const uint8_t *icon_corner = art + ((size_t)80 * PICKER_ART_W + 128) * 4; /* clear in the icon */
    bool around_is_card = corner[1] == 0x22 && corner[2] == 0x22 && corner[3] == 0x22;
    bool clear_is_card = icon_corner[1] == 0x22 && icon_corner[2] == 0x22 && icon_corner[3] == 0x22;
    char got[16];
    snprintf(got, sizeof got, "%08x", fnv1a32(art, PICKER_ART_W * PICKER_ART_H * 4));
    free(art);
    CHECK(ok);
    CHECK_STR(err, "");
    CHECK(around_is_card);
    CHECK(clear_is_card);
    CHECK_STR(got, "df198a6f"); /* the icon shown in the four-game picker */
}

TEST(picker_art_comes_from_the_games_title_picture) {
    SKIP_UNLESS_GAME();
    uint8_t *art = malloc(PICKER_ART_W * PICKER_ART_H * 4);
    char err[256];
    bool ok = picker_load_art(test_game_exe_path(), art, err, sizeof err);
    bool bad = picker_load_art("/nonexistent/LOONY LABYRINTH 3.0.1", art + 0, err, sizeof err);
    free(art);
    CHECK(ok);
    CHECK(!bad);
    CHECK_CONTAINS(err, "/nonexistent/LOONY LABYRINTH 3.0.1");
}
