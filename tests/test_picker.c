#include "test.h"

#include <SDL3/SDL_scancode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "game.h"
#include "picker.h"
#include "png.h"
#include "rsrc.h"
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
    CHECK_EQ(px(454 - 2, 200)[1], 0xFF);         /* gold border: the second is selected */
    CHECK_EQ(px(454 - 2, 200)[2], 0xCC);
    CHECK_EQ(px(58 - 2, 200)[1], 0x33);          /* gray border on the first */
    CHECK_EQ(px(10 + 2, 200)[1], 0);             /* black beside the square card */
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
    CHECK_STR(got, "4d3203a5");
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
    CHECK_EQ(px(46, 180)[1], 0xFF);               /* the first card's art, reduced */
    CHECK_EQ(px(46 + 179, 180 + 179)[1], 0xFF);
    CHECK_EQ(px(46 + 180, 200)[1], 0x33);         /* its gray border */
    CHECK_EQ(px(280 + 100, 180 + 90)[1], 0x22);   /* the second: a plain card */
    CHECK_EQ(px(544 + 100, 180 + 90)[3], 0xFF);   /* the third card's art */
    CHECK_EQ(px(574 - 2, 200)[1], 0xFF);          /* gold border: the third is selected */
    CHECK_EQ(px(574 - 2, 200)[2], 0xCC);
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

/* Four games: 168x168 cards in two rows of two, at x = 172 and 460, from
   y = 108 and 324. */
TEST(picker_draws_four_cards_in_two_rows) {
    picker_entry e[4] = {{game_at(0), solid_art(0xFF, 0, 0)}, {game_at(1), NULL}, {game_at(2), NULL},
                         {game_at(3), solid_art(0, 0, 0xFF)}};
    picker_draw(e, 4, 3, screen);
    free(e[0].art);
    free(e[3].art);
    CHECK_EQ(px(172, 108)[1], 0xFF);              /* the first card's art, reduced */
    CHECK_EQ(px(172 + 167, 108 + 167)[1], 0xFF);
    CHECK_EQ(px(172 + 168, 200)[1], 0x33);        /* its gray border */
    CHECK_EQ(px(432 + 100, 108 + 80)[1], 0x22);   /* the second: a plain card, beside it */
    CHECK_EQ(px(144 + 100, 324 + 80)[1], 0x22);   /* the third: a plain card, under the first */
    CHECK_EQ(px(432 + 100, 324 + 80)[3], 0xFF);   /* the fourth card's art */
    CHECK_EQ(px(460 - 2, 400)[1], 0xFF);          /* gold border: the fourth is selected */
    CHECK_EQ(px(460 - 2, 400)[2], 0xCC);
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

/* Five games: 224x168 cards, three in the first row at x = 8, 288 and 568,
   two centered in the second at x = 148 and 428, from y = 108 and 324. */
TEST(picker_draws_five_cards_in_rows_of_three_and_two) {
    picker_entry e[5] = {{game_at(0), solid_art(0xFF, 0, 0)}, {game_at(1), NULL}, {game_at(2), NULL},
                         {game_at(3), NULL}, {game_at(4), solid_art(0, 0, 0xFF)}};
    picker_draw(e, 5, 4, screen);
    free(e[0].art);
    free(e[4].art);
    CHECK_EQ(px(36, 108)[1], 0xFF);              /* the first card's art, reduced */
    CHECK_EQ(px(36 + 167, 108 + 167)[1], 0xFF);
    CHECK_EQ(px(288 + 100, 108 + 80)[1], 0x22);  /* the second and third: plain cards */
    CHECK_EQ(px(596 + 167, 108 + 80)[1], 0x22);
    CHECK_EQ(px(148 + 100, 324 + 80)[1], 0x22);  /* the fourth, under the first two */
    CHECK_EQ(px(428 + 100, 324 + 80)[3], 0xFF);  /* the fifth card's art */
    CHECK_EQ(px(456 - 2, 400)[1], 0xFF);         /* gold border: the fifth is selected */
    CHECK_EQ(px(456 - 2, 400)[2], 0xCC);
    CHECK_EQ(px(260, 200)[1], 0);                /* black between the columns */
    CHECK_EQ(px(100, 400)[1], 0);                /* and beside the short row */
}

TEST(picker_keys_move_across_and_between_rows_of_five) {
    bool choose = false;
    CHECK_EQ(picker_key(5, 2, SDL_SCANCODE_RIGHT, &choose), 3); /* on to the next row */
    CHECK_EQ(picker_key(5, 4, SDL_SCANCODE_RIGHT, &choose), 4);
    CHECK_EQ(picker_key(5, 0, SDL_SCANCODE_DOWN, &choose), 3);
    CHECK_EQ(picker_key(5, 1, SDL_SCANCODE_DOWN, &choose), 4);
    CHECK_EQ(picker_key(5, 2, SDL_SCANCODE_DOWN, &choose), 4); /* nothing below: the last card */
    CHECK_EQ(picker_key(5, 4, SDL_SCANCODE_DOWN, &choose), 4);
    CHECK_EQ(picker_key(5, 4, SDL_SCANCODE_UP, &choose), 1);
    CHECK_EQ(picker_key(5, 3, SDL_SCANCODE_UP, &choose), 0);
    CHECK(!choose);
    CHECK_EQ(picker_hit(5, 568 + 223, 108 + 167), 2);
    CHECK_EQ(picker_hit(5, 148 + 5, 324 + 5), 3);
    CHECK_EQ(picker_hit(5, 428 + 100, 506), 4); /* the name under a card counts */
    CHECK_EQ(picker_hit(5, 100, 400), -1);      /* beside the short row */
}

/* Nine games by title, for the layouts of up to nine cards. */
static const game_info nine[9] = {
    {.title = "Loony Labyrinth"}, {.title = "Crystal Caliburn"}, {.title = "Jinni Zeala"},
    {.title = "MONSTER FAIR"},    {.title = "Mad Daedalus"},     {.title = "Golden Logres"},
    {.title = "Angel Egg"},       {.title = "FAIRY TOWER"},      {.title = "Tristan"}};

TEST(picker_draws_six_cards_in_two_rows_of_three) {
    picker_entry e[6];
    for (int i = 0; i < 6; i++)
        e[i] = (picker_entry){&nine[i], NULL};
    e[5].art = solid_art(0, 0, 0xFF);
    picker_draw(e, 6, 5, screen);
    free(e[5].art);
    CHECK_EQ(px(8 + 100, 108 + 80)[1], 0x22);   /* the first, where it is with five */
    CHECK_EQ(px(148 + 100, 324 + 80)[1], 0);    /* no longer a short row */
    CHECK_EQ(px(8 + 100, 324 + 80)[1], 0x22);   /* the fourth, under the first */
    CHECK_EQ(px(568 + 100, 324 + 80)[3], 0xFF); /* the sixth card's art */
    CHECK_EQ(px(596 - 2, 400)[1], 0xFF);        /* gold border: the sixth is selected */
    bool choose = false;
    CHECK_EQ(picker_key(6, 2, SDL_SCANCODE_DOWN, &choose), 5);
    CHECK_EQ(picker_key(6, 5, SDL_SCANCODE_UP, &choose), 2);
    CHECK_EQ(picker_hit(6, 568 + 5, 324 + 5), 5);
}

TEST(picker_draws_seven_cards_in_rows_of_four_and_three) {
    picker_entry e[7];
    for (int i = 0; i < 7; i++)
        e[i] = (picker_entry){&nine[i], NULL};
    e[0].art = solid_art(0xFF, 0, 0);
    e[6].art = solid_art(0, 0, 0xFF);
    picker_draw(e, 7, 6, screen);
    free(e[0].art);
    free(e[6].art);
    CHECK_EQ(px(34, 108)[1], 0xFF);               /* the first card's art, reduced */
    CHECK_EQ(px(34 + 131, 108 + 131)[1], 0xFF);
    CHECK_EQ(px(634 + 131, 108 + 131)[1], 0x22);  /* the fourth ends the first row */
    CHECK_EQ(px(112 + 100, 308 + 80)[1], 0x22);   /* the fifth, the second row centered */
    CHECK_EQ(px(512 + 100, 308 + 80)[3], 0xFF);   /* the seventh card's art */
    CHECK_EQ(px(534 - 2, 400)[1], 0xFF);          /* gold border: the seventh is selected */
    CHECK_EQ(px(200, 200)[1], 0);                 /* black between the columns */
    CHECK_EQ(px(50, 400)[1], 0);                  /* and beside the short row */
    /* ANGEL EGG (144 pixels) fits its 176-pixel slot on one line, so
       nothing is drawn on a second; GOLDEN LOGRES (208) takes two, and so
       does MAD DAEDALUS (192), which would reach its neighbours on one. */
    bool second_line[3] = {false, false, false}; /* MAD DAEDALUS, GOLDEN LOGRES, ANGEL EGG */
    for (int c = 0; c < 3; c++)
        for (int y = 472; y < 488; y++)
            for (int x = 112 + c * 200; x < 112 + c * 200 + 176; x++)
                second_line[c] |= px(x, y)[1] != 0;
    CHECK(second_line[0]);
    CHECK(second_line[1]);
    CHECK(!second_line[2]);
}

TEST(picker_keys_move_across_and_between_rows_of_seven) {
    bool choose = false;
    CHECK_EQ(picker_key(7, 3, SDL_SCANCODE_RIGHT, &choose), 4); /* on to the next row */
    CHECK_EQ(picker_key(7, 0, SDL_SCANCODE_DOWN, &choose), 4);
    CHECK_EQ(picker_key(7, 2, SDL_SCANCODE_DOWN, &choose), 6);
    CHECK_EQ(picker_key(7, 3, SDL_SCANCODE_DOWN, &choose), 6); /* nothing below: the last card */
    CHECK_EQ(picker_key(7, 6, SDL_SCANCODE_UP, &choose), 2);
    CHECK(!choose);
    CHECK_EQ(picker_hit(7, 612 + 175, 108 + 131), 3);
    CHECK_EQ(picker_hit(7, 312 + 88, 480), 5); /* a name's second line counts */
    CHECK_EQ(picker_hit(7, 50, 400), -1);       /* beside the short row */
}

TEST(picker_draws_eight_cards_in_two_rows_of_four) {
    picker_entry e[8];
    for (int i = 0; i < 8; i++)
        e[i] = (picker_entry){&nine[i], NULL};
    e[7].art = solid_art(0, 0, 0xFF);
    picker_draw(e, 8, 7, screen);
    free(e[7].art);
    CHECK_EQ(px(12 + 100, 108 + 80)[1], 0x22);   /* the first, where it is with seven */
    CHECK_EQ(px(12 + 100, 308 + 80)[1], 0x22);   /* the fifth, under the first: no short row */
    CHECK_EQ(px(612 + 100, 308 + 80)[3], 0xFF);  /* the eighth card's art */
    CHECK_EQ(px(634 - 2, 400)[1], 0xFF);         /* gold border: the eighth is selected */
    CHECK_EQ(px(200, 400)[1], 0);                /* black between the columns */
    /* FAIRY TOWER (176 pixels) fits its column on one line. */
    bool second_line = false;
    for (int y = 472; y < 488; y++)
        for (int x = 612; x < 612 + 176; x++)
            second_line |= px(x, y)[1] != 0;
    CHECK(!second_line);
    bool choose = false;
    CHECK_EQ(picker_key(8, 3, SDL_SCANCODE_DOWN, &choose), 7);
    CHECK_EQ(picker_key(8, 7, SDL_SCANCODE_UP, &choose), 3);
    CHECK_EQ(picker_key(8, 7, SDL_SCANCODE_DOWN, &choose), 7);
    CHECK_EQ(picker_key(8, 7, SDL_SCANCODE_RIGHT, &choose), 7);
    CHECK_EQ(picker_key(8, 3, SDL_SCANCODE_RIGHT, &choose), 4);
    CHECK(!choose);
    CHECK_EQ(picker_hit(8, 612 + 5, 308 + 5), 7);
    CHECK_EQ(picker_hit(8, 12 + 5, 308 + 5), 4);
}

TEST(picker_draws_nine_cards_in_three_rows_of_three) {
    picker_entry e[9];
    for (int i = 0; i < 9; i++)
        e[i] = (picker_entry){&nine[i], NULL};
    e[0].art = solid_art(0xFF, 0, 0);
    e[8].art = solid_art(0, 0, 0xFF);
    picker_draw(e, 9, 8, screen);
    free(e[0].art);
    free(e[8].art);
    CHECK_EQ(px(68, 76)[1], 0xFF);                /* the first card's art, reduced */
    CHECK_EQ(px(68 + 119, 76 + 119)[1], 0xFF);
    CHECK_EQ(px(320 + 80, 240 + 60)[1], 0x22);    /* the fifth, in the middle */
    CHECK_EQ(px(592 + 80, 404 + 60)[3], 0xFF);    /* the ninth card's art */
    CHECK_EQ(px(612 - 2, 450)[1], 0xFF);          /* gold border: the ninth is selected */
    CHECK_EQ(px(260, 150)[1], 0);                 /* black between the columns */
    CHECK_EQ(px(48 + 80, 76 + 120 + 6)[1], 0);    /* and between a card and its name */
    /* Every name fits its column on one line: CRYSTAL CALIBURN (256 pixels)
       is the widest, and nothing is drawn under a name, where a second
       line would start, before the next row's borders. */
    bool second_line = false;
    for (int row = 0; row < 3; row++)
        for (int y = 206 + row * 164 + 16; y < 206 + row * 164 + 30; y++)
            for (int x = 0; x < 800; x++)
                second_line |= px(x, y)[1] != 0;
    CHECK(!second_line);
    /* LOONY LABYRINTH (240 pixels, ending at 248) and CRYSTAL CALIBURN
       (256, from 272) leave 24 pixels of black between them, as seven and
       eight keep between columns. */
    bool gap_dark = true;
    for (int y = 206; y < 222; y++)
        for (int x = 248; x < 272; x++)
            gap_dark &= px(x, y)[1] == 0;
    CHECK(gap_dark);
    /* Black below the heading (32 to 56) and above the hint (from 566). */
    bool clear = true;
    for (int x = 0; x < 800; x++) {
        for (int y = 57; y < 72; y++)
            clear &= px(x, y)[1] == 0;
        for (int y = 550; y < 566; y++)
            clear &= px(x, y)[1] == 0;
    }
    CHECK(clear);
    bool choose = false;
    CHECK_EQ(picker_key(9, 1, SDL_SCANCODE_DOWN, &choose), 4);
    CHECK_EQ(picker_key(9, 5, SDL_SCANCODE_DOWN, &choose), 8);
    CHECK_EQ(picker_key(9, 8, SDL_SCANCODE_DOWN, &choose), 8);
    CHECK_EQ(picker_key(9, 7, SDL_SCANCODE_UP, &choose), 4);
    CHECK_EQ(picker_key(9, 2, SDL_SCANCODE_RIGHT, &choose), 3);
    CHECK(!choose);
    CHECK_EQ(picker_hit(9, 592 + 5, 404 + 5), 8);
    CHECK_EQ(picker_hit(9, 320 + 80, 370 + 8), 4); /* the name under a card counts */
    CHECK_EQ(picker_hit(9, 320 + 80, 230), -1);    /* between the rows */
    CHECK_EQ(picker_hit(9, 260, 150), -1);         /* between the columns */
}

TEST(picker_draws_ten_cards_in_rows_of_four_four_and_two) {
    static const game_info psychedelic = {.title = "Psychedelic \"O\""};
    picker_entry e[10];
    for (int i = 0; i < 9; i++)
        e[i] = (picker_entry){&nine[i], NULL};
    e[9] = (picker_entry){&psychedelic, NULL};
    e[0].art = solid_art(0xFF, 0, 0);
    e[9].art = solid_art(0, 0, 0xFF);
    picker_draw(e, 10, 9, screen);
    free(e[0].art);
    free(e[9].art);
    CHECK_EQ(px(46, 66)[1], 0xFF);              /* the first card's art, reduced */
    CHECK_EQ(px(46 + 107, 66 + 107)[1], 0xFF);
    CHECK_EQ(px(646 + 54, 228 + 54)[1], 0x22);  /* the eighth ends the second row */
    CHECK_EQ(px(246 + 54, 390 + 54)[1], 0x22);  /* the ninth, the short row centered */
    CHECK_EQ(px(446 + 54, 390 + 54)[3], 0xFF);  /* the tenth card's art */
    CHECK_EQ(px(446 - 2, 440)[1], 0xFF);        /* gold border: the tenth is selected */
    CHECK_EQ(px(100, 440)[1], 0);               /* black beside the short row */
    CHECK_EQ(px(200, 120)[1], 0);               /* and between the columns */
    /* LOONY LABYRINTH (240 pixels) would come within 24 of its neighbour's
       name, so it takes two lines; PSYCHEDELIC "O" breaks after its first
       word (176 pixels), the room a column leaves. */
    bool second_line[2] = {false, false};
    for (int y = 194; y < 210; y++)
        for (int x = 28; x < 28 + 144; x++)
            second_line[0] |= px(x, y)[1] != 0;
    for (int y = 528; y < 544; y++)
        for (int x = 476; x < 524; x++)
            second_line[1] |= px(x, y)[1] != 0;
    CHECK(second_line[0]);
    CHECK(second_line[1]);
    /* Black below the heading (32 to 56) and above the hint (from 566). */
    bool clear = true;
    for (int x = 0; x < 800; x++) {
        for (int y = 57; y < 62; y++)
            clear &= px(x, y)[1] == 0;
        for (int y = 546; y < 566; y++)
            clear &= px(x, y)[1] == 0;
    }
    CHECK(clear);
    bool choose = false;
    CHECK_EQ(picker_key(10, 5, SDL_SCANCODE_DOWN, &choose), 9);
    CHECK_EQ(picker_key(10, 6, SDL_SCANCODE_DOWN, &choose), 9); /* nothing below: the last card */
    CHECK_EQ(picker_key(10, 9, SDL_SCANCODE_UP, &choose), 5);
    CHECK_EQ(picker_key(10, 3, SDL_SCANCODE_RIGHT, &choose), 4);
    CHECK(!choose);
    CHECK_EQ(picker_hit(10, 446 + 5, 390 + 5), 9);
    CHECK_EQ(picker_hit(10, 500, 536), 9); /* a name's second line counts */
    CHECK_EQ(picker_hit(10, 100, 440), -1); /* beside the short row */
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

/* An icns holding a 128x128 it32 (red: each plane one color, as 126
   repeat runs of 130 and a literal run of 4) and a t8mk whose left half
   is opaque. Returns its length; cut_it32 bytes are dropped from the end
   of the it32, and with_mask false leaves out the t8mk. */
static size_t build_icns(uint8_t *out, size_t cut_it32, bool with_mask) {
    static const uint8_t color[3] = {0xFF, 0, 0};
    uint8_t *it32 = out + 8;
    size_t n = 8 + 4; /* the it32 header, then four zero bytes */
    memcpy(it32, "it32", 4);
    memset(it32 + 8, 0, 4);
    for (int c = 0; c < 3; c++) {
        for (int r = 0; r < 126; r++) {
            it32[n++] = 0xFF; /* repeat the next byte 130 times */
            it32[n++] = color[c];
        }
        it32[n++] = 0x03; /* copy the next 4 bytes */
        for (int i = 0; i < 4; i++)
            it32[n++] = color[c];
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

/* picker_load_icns on a copy of exactly len bytes, so the sanitizer
   reports any read past its end. */
static bool load_exact(const uint8_t *icns, size_t len, uint8_t *art) {
    uint8_t *copy = malloc(len);
    memcpy(copy, icns, len);
    bool ok = picker_load_icns(copy, len, art);
    free(copy);
    return ok;
}

/* A damaged icon is refused, never read past its end. */
TEST(picker_refuses_a_damaged_icns) {
    static uint8_t icns[64 * 1024];
    uint8_t *art = malloc(PICKER_ART_W * PICKER_ART_H * 4);
    size_t len = build_icns(icns, 1, true); /* the last literal run is a byte short */
    bool short_literal = load_exact(icns, len, art);
    len = build_icns(icns, 6, true); /* the last repeat run has no byte to repeat */
    bool short_repeat = load_exact(icns, len, art);
    len = build_icns(icns, 0, false);
    bool no_mask = load_exact(icns, len, art);
    len = build_icns(icns, 0, true);
    bool cut_short = load_exact(icns, len - 100, art); /* the mask runs past the end */
    icns[8 + 7] = 4;                                   /* an element shorter than its header */
    icns[8 + 4] = icns[8 + 5] = icns[8 + 6] = 0;
    bool tiny_element = load_exact(icns, len, art);
    len = build_icns(icns, 0, true);
    icns[8 + 12 + 126 * 2] = 0x04; /* the red plane's literal run, 4, becomes 5: past the plane */
    bool overlong = load_exact(icns, len, art);
    free(art);
    CHECK(!short_literal);
    CHECK(!short_repeat);
    CHECK(!no_mask);
    CHECK(!cut_short);
    CHECK(!tiny_element);
    CHECK(!overlong);
}

/* A bundle without appl.png gets its card from app.icns; one with
   neither is refused. */
TEST(picker_draws_a_bundle_card_from_its_app_icns) {
    static uint8_t icns[64 * 1024];
    char dir[1024], res[1100], path[1200], err[512] = "", none_err[512] = "";
    test_tmp_dir(dir, sizeof dir);
    snprintf(res, sizeof res, "%s/Contents", dir);
    mkdir(res, 0700);
    snprintf(res, sizeof res, "%s/Contents/Resources", dir);
    mkdir(res, 0700);
    uint8_t *art = malloc(PICKER_ART_W * PICKER_ART_H * 4);
    bool none = picker_load_bundle_art(dir, art, none_err, sizeof none_err);
    snprintf(path, sizeof path, "%s/app.icns", res);
    size_t len = build_icns(icns, 0, true);
    FILE *f = fopen(path, "wb");
    fwrite(icns, 1, len, f);
    fclose(f);
    bool ok = picker_load_bundle_art(dir, art, err, sizeof err);
    test_remove_tree(dir);
    const uint8_t *in_left = art + ((size_t)144 * PICKER_ART_W + 128 + 10) * 4;
    bool red = in_left[1] == 0xFF && in_left[2] == 0 && in_left[3] == 0;
    free(art);
    CHECK(!none);
    CHECK_CONTAINS(none_err, "neither appl.png nor an app.icns");
    CHECK(ok);
    CHECK_STR(err, "");
    CHECK(red);
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
    CHECK_STR(got, "462c5e6f"); /* the icon shown in the four-game picker */
}

/* Mad Daedalus is a bundle: its card is appl.png, its 128x128 icon. */
TEST(picker_art_for_mad_daedalus_is_its_icon) {
    SKIP_UNLESS_MD();
    char path[1200], err[512] = "";
    snprintf(path, sizeof path, "%s/Contents/Resources/appl.png", test_md_app());
    uint8_t *art = malloc(PICKER_ART_W * PICKER_ART_H * 4);
    bool ok = picker_load_icon(path, art, err, sizeof err);
    const uint8_t *corner = art + ((size_t)5 * PICKER_ART_W + 5) * 4;
    bool around_is_card = corner[1] == 0x22 && corner[2] == 0x22 && corner[3] == 0x22;
    char got[16];
    snprintf(got, sizeof got, "%08x", fnv1a32(art, PICKER_ART_W * PICKER_ART_H * 4));
    free(art);
    CHECK(ok);
    CHECK_STR(err, "");
    CHECK(around_is_card);
    CHECK_STR(got, "7d0970ee"); /* the icon shown in the five-game picker */
}

/* FAIRY TOWER is a bundle like Mad Daedalus: its card is appl.png. */
TEST(picker_art_for_fairy_tower_is_its_icon) {
    SKIP_UNLESS_FT();
    char path[1200], err[512] = "";
    snprintf(path, sizeof path, "%s/Contents/Resources/appl.png", test_ft_app());
    uint8_t *art = malloc(PICKER_ART_W * PICKER_ART_H * 4);
    bool ok = picker_load_icon(path, art, err, sizeof err);
    const uint8_t *corner = art + ((size_t)5 * PICKER_ART_W + 5) * 4;
    bool around_is_card = corner[1] == 0x22 && corner[2] == 0x22 && corner[3] == 0x22;
    char got[16];
    snprintf(got, sizeof got, "%08x", fnv1a32(art, PICKER_ART_W * PICKER_ART_H * 4));
    free(art);
    CHECK(ok);
    CHECK_STR(err, "");
    CHECK(around_is_card);
    CHECK_STR(got, "c8a5485e"); /* the icon shown in the nine-game picker */
}

/* Tristan's PICT 800 is its credits, and it has no icns 128: its card is
   its title, PICT 900 (348x301), reduced to 332x288 and centered. */
TEST(picker_art_for_tristan_is_its_title) {
    SKIP_UNLESS_TR();
    char err[512] = "";
    uint8_t *art = malloc(PICKER_ART_W * PICKER_ART_H * 4);
    bool ok = picker_load_art(test_tr_exe_path(), art, err, sizeof err);
    const uint8_t *side = art + 144 * 4, *in = art + ((size_t)144 * PICKER_ART_W + 144) * 4;
    bool beside_is_card = side[1] == 0x22 && side[2] == 0x22 && side[3] == 0x22; /* above it */
    bool title_is_drawn = !(in[1] == 0x22 && in[2] == 0x22 && in[3] == 0x22);
    char got[16];
    snprintf(got, sizeof got, "%08x", fnv1a32(art, PICKER_ART_W * PICKER_ART_H * 4));
    free(art);
    CHECK(ok);
    CHECK_STR(err, "");
    CHECK(beside_is_card);
    CHECK(title_is_drawn);
    CHECK_STR(got, "7af504b5"); /* the title shown in the nine-game picker */
}

/* An empty version 1 picture whose frame is w by h. */
static void blank_pict(uint8_t pic[13], int w, int h) {
    static const uint8_t head[] = {0, 13, 0, 0, 0, 0};
    memcpy(pic, head, sizeof head);
    wr_be16(pic + 6, (uint16_t)h);
    wr_be16(pic + 8, (uint16_t)w);
    pic[10] = 0x11; /* version 1 */
    pic[11] = 0x01;
    pic[12] = 0xFF; /* end */
}

/* A program at dir/game whose resource fork holds PICT 800 of w800 by
   h800 (none if 0) and PICT 900 of w900 by h900 (none if 0). */
static void art_program(const char *dir, int w800, int h800, int w900, int h900, char *path, size_t cap) {
    uint8_t p800[13], p900[13];
    blank_pict(p800, w800, h800);
    blank_pict(p900, w900, h900);
    rsrc_entry e[2] = {{0}};
    const uint8_t *d[2];
    uint32_t n = 0;
    if (w800) {
        e[n] = (rsrc_entry){.type = FOURCC('P', 'I', 'C', 'T'), .id = 800, .len = sizeof p800};
        d[n++] = p800;
    }
    if (w900) {
        e[n] = (rsrc_entry){.type = FOURCC('P', 'I', 'C', 'T'), .id = 900, .len = sizeof p900};
        d[n++] = p900;
    }
    size_t len;
    uint8_t *fork = rsrc_build(e, d, n, &len);
    char fork_path[1300];
    snprintf(path, cap, "%s/game", dir);
    snprintf(fork_path, sizeof fork_path, "%s/..namedfork/rsrc", path);
    FILE *f = fopen(path, "wb");
    if (!f || fclose(f) != 0 || !(f = fopen(fork_path, "wb")) || fwrite(fork, 1, len, f) != len || fclose(f) != 0)
        fatal("can't write %s", fork_path);
    free(fork);
}

static bool is_card(const uint8_t *art, int x, int y) {
    const uint8_t *p = art + ((size_t)y * PICKER_ART_W + (size_t)x) * 4;
    return p[1] == 0x22 && p[2] == 0x22 && p[3] == 0x22;
}

/* Without the game: a PICT 800 of the wrong size (Tristan's credits) is
   passed over for PICT 900, which is centered on the card at its own size
   when it fits; a 512x384 PICT 800 fills the card's width (288x216,
   centered); a PICT 900 larger than
   512x384, or no picture at all, gives no art. */
TEST(picker_art_falls_back_to_a_centered_pict_900) {
    char dir[1024], path[1200], err[512] = "", big_err[512] = "", none_err[512] = "", err800[512] = "";
    test_tmp_dir(dir, sizeof dir);
    uint8_t *art = malloc(PICKER_ART_W * PICKER_ART_H * 4), *art800 = malloc(PICKER_ART_W * PICKER_ART_H * 4);
    art_program(dir, 348, 301, 100, 50, path, sizeof path);
    bool ok = picker_load_art(path, art, err, sizeof err);
    art_program(dir, 512, 384, 100, 50, path, sizeof path);
    bool ok800 = picker_load_art(path, art800, err800, sizeof err800);
    uint8_t *scratch_art = malloc(PICKER_ART_W * PICKER_ART_H * 4);
    art_program(dir, 348, 301, 600, 50, path, sizeof path);
    bool big = picker_load_art(path, scratch_art, big_err, sizeof big_err);
    art_program(dir, 0, 0, 0, 0, path, sizeof path);
    bool none = picker_load_art(path, scratch_art, none_err, sizeof none_err);
    test_remove_tree(dir);
    /* 100x50 centered on the card: columns l to l + 99, rows 119 to 168 */
    int l = (PICKER_ART_W - 100) / 2, cx = PICKER_ART_W / 2;
    bool centered = !is_card(art, l, 119) && !is_card(art, l + 99, 168) && is_card(art, l - 1, 144) &&
                    is_card(art, l + 100, 144) && is_card(art, cx, 118) && is_card(art, cx, 169);
    bool fills = !is_card(art800, 0, 36) && !is_card(art800, PICKER_ART_W - 1, 251) && is_card(art800, 0, 35) &&
                 is_card(art800, 0, 252);
    free(art);
    free(art800);
    free(scratch_art);
    CHECK(ok);
    CHECK_STR(err, "");
    CHECK(centered);
    CHECK(ok800);
    CHECK(fills);
    CHECK(!big);
    CHECK_CONTAINS(big_err, "has no 512x384 PICT 800, icns 128 or PICT 900");
    CHECK(!none);
    CHECK_CONTAINS(none_err, "has no 512x384 PICT 800, icns 128 or PICT 900");
}

/* Golden Logres is a bundle without appl.png: its card is the 128x128
   image in its app.icns. */
TEST(picker_art_for_golden_logres_is_its_icon) {
    SKIP_UNLESS_GL();
    char err[512] = "";
    uint8_t *art = malloc(PICKER_ART_W * PICKER_ART_H * 4);
    bool ok = picker_load_bundle_art(test_gl_app(), art, err, sizeof err);
    const uint8_t *corner = art + ((size_t)5 * PICKER_ART_W + 5) * 4;
    bool around_is_card = corner[1] == 0x22 && corner[2] == 0x22 && corner[3] == 0x22;
    char got[16];
    snprintf(got, sizeof got, "%08x", fnv1a32(art, PICKER_ART_W * PICKER_ART_H * 4));
    free(art);
    CHECK(ok);
    CHECK_STR(err, "");
    CHECK(around_is_card);
    CHECK_STR(got, "3fae888b"); /* the icon shown in the seven-game picker */
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

TEST(picker_finds_the_card_bundled_with_the_app) {
    char dir[1024], res[1200], card[1300], exe[1200], out[1300];
    test_tmp_dir(dir, sizeof dir);
    snprintf(res, sizeof res, "%s/LittleWing.app/Contents/Resources/picker-art", dir);
    CHECK(make_dirs(res));
    snprintf(card, sizeof card, "%s/loony-labyrinth.png", res);
    write_icon(card, 288);
    snprintf(exe, sizeof exe, "%s/LittleWing.app/Contents/MacOS/loony", dir);
    bool found = picker_bundled_art(exe, "loony-labyrinth", out, sizeof out);
    bool same = found && strcmp(out, card) == 0;
    bool other_game = picker_bundled_art(exe, "crystal-caliburn", out, sizeof out); /* no card for it */
    char tiny[8];
    bool too_long = picker_bundled_art(exe, "loony-labyrinth", tiny, sizeof tiny);
    bool no_path = picker_bundled_art(NULL, "loony-labyrinth", out, sizeof out);
    snprintf(exe, sizeof exe, "%s/build-release/loony", dir);
    bool outside_app = picker_bundled_art(exe, "loony-labyrinth", out, sizeof out);
    test_remove_tree(dir);
    CHECK(found);
    CHECK(same);
    CHECK(!other_game);
    CHECK(!too_long);
    CHECK(!no_path);
    CHECK(!outside_app);
}

/* Every game has a card in picker-art/, and each one fits a card. */
TEST(picker_art_has_a_card_for_every_game) {
    for (size_t i = 0; i < game_count(); i++) {
        char path[1200], err[512] = "";
        snprintf(path, sizeof path, "%s/picker-art/%s.png", LOONY_SRC_DIR, game_at(i)->id);
        uint8_t *art = malloc(PICKER_ART_W * PICKER_ART_H * 4);
        bool ok = picker_load_icon(path, art, err, sizeof err);
        free(art);
        CHECK_STR(err, "");
        CHECK(ok);
    }
}
