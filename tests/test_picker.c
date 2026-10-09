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
