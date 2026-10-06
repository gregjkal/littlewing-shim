#include "test.h"

#include <SDL3/SDL_scancode.h>
#include <stdlib.h>

#include "game.h"
#include "picker.h"
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
