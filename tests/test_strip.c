#include "test.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

#include "strip.h"
#include "watch.h"

static double t;
static double fake_now(void) { return t; }

/* An 800 x 600 game in a window with the strip, at one pixel a point. */
static display_layout window_layout(void) {
    return display_lay_out(1000, 600 + STRIP_HEIGHT, 800, 600, STRIP_HEIGHT, false);
}

/* Up for a while at the start, on any movement or key, and while the
   pointer is on it; down STRIP_HIDE_AFTER after the last, fading. */
TEST(strip_comes_up_on_movement_and_goes_after_a_while) {
    t = 0;
    strip_reset(fake_now);
    strip_install();
    const display_overlay *o = strip_overlay();
    display_layout l = window_layout();
    CHECK(o->shown() == 0); /* fading in */
    t = STRIP_FADE / 2;
    CHECK(o->shown() > 0.4f && o->shown() < 0.6f);
    t = STRIP_FADE;
    CHECK(o->shown() == 1);
    t = STRIP_HIDE_AFTER - 0.01;
    CHECK(o->shown() == 1);
    t = STRIP_HIDE_AFTER + STRIP_FADE / 2; /* fading out */
    CHECK(o->shown() > 0.4f && o->shown() < 0.6f);
    t = STRIP_HIDE_AFTER + STRIP_FADE;
    CHECK(o->shown() == 0);
    t = 5; /* the pointer moves over the game */
    o->pointer(&l, 100, 100, true);
    CHECK(o->shown() == 0);
    t = 5 + STRIP_FADE;
    CHECK(o->shown() == 1);
    t = 5 + STRIP_HIDE_AFTER - 0.01;
    CHECK(o->shown() == 1);
    t = 5 + STRIP_HIDE_AFTER + STRIP_FADE;
    CHECK(o->shown() == 0);
    t = 10; /* onto the strip: it stays up */
    o->pointer(&l, 500, 600 + STRIP_HEIGHT / 2, true);
    t = 10 + STRIP_FADE;
    CHECK(o->shown() == 1);
    t = 20;
    CHECK(o->shown() == 1);
    o->pointer(&l, 100, 100, true); /* off it */
    t = 20 + STRIP_HIDE_AFTER - 0.01;
    CHECK(o->shown() == 1);
    t = 20 + STRIP_HIDE_AFTER + STRIP_FADE;
    CHECK(o->shown() == 0);
    t = 30; /* a key */
    strip_poke();
    CHECK(o->shown() == 0);
    t = 30 + STRIP_FADE;
    CHECK(o->shown() == 1);
    t = 40; /* left the window: down */
    o->pointer(&l, 500, 600 + STRIP_HEIGHT / 2, true);
    o->pointer(&l, 0, 0, false);
    t = 40 + STRIP_FADE;
    CHECK(o->shown() == 1);
    t = 40 + STRIP_HIDE_AFTER - 0.01;
    CHECK(o->shown() == 1);
    t = 40 + STRIP_HIDE_AFTER + STRIP_FADE;
    CHECK(o->shown() == 0);
    t = 50; /* seen only long after, it went at its time all the same */
    strip_poke();
    t = 50 + STRIP_HIDE_AFTER + STRIP_FADE / 2 + 5;
    CHECK(o->shown() == 0);
    t = 50 + STRIP_HIDE_AFTER + STRIP_FADE / 2; /* the clock can't go back, but the fade was set */
    CHECK(o->shown() > 0.4f && o->shown() < 0.6f);
}

/* The buttons, from the left, on the strip and nowhere else. */
TEST(strip_button_at_finds_the_buttons) {
    strip_reset(fake_now);
    display_layout l = window_layout();
    float y = 600 + STRIP_HEIGHT / 2, b = STRIP_HEIGHT, x0 = b / 4;
    CHECK_EQ(strip_button_at(&l, x0 + b / 2, y), STRIP_PLAY);
    CHECK_EQ(strip_button_at(&l, x0 + 1.5f * b, y), STRIP_BACK);
    CHECK_EQ(strip_button_at(&l, x0 + 2.5f * b, y), STRIP_AHEAD);
    CHECK_EQ(strip_button_at(&l, x0 + 3.5f * b, y), STRIP_SLOWER);
    CHECK_EQ(strip_button_at(&l, x0 + b / 2, 300), STRIP_NONE); /* the game */
    CHECK_EQ(strip_button_at(&l, 990, y), STRIP_NONE);          /* the bar */
    int faster = 0;
    for (float x = 0; x < 1000; x += 1)
        faster += strip_button_at(&l, x, y) == STRIP_FASTER;
    CHECK_EQ(faster, (int)(b * 0.75f));
}

/* A tip says the button's key as typed, from the viewer's key table. */
TEST(strip_tip_says_each_buttons_key) {
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    char tip[64];
    strip_tip(STRIP_PLAY, tip, sizeof tip);
    CHECK_STR(tip, "Play/Pause (Space)");
    strip_tip(STRIP_BACK, tip, sizeof tip);
    CHECK_STR(tip, "Back 10 s (J)");
    strip_tip(STRIP_AHEAD, tip, sizeof tip);
    CHECK_STR(tip, "Ahead 10 s (L)");
    strip_tip(STRIP_SLOWER, tip, sizeof tip);
    CHECK_STR(tip, "Slower (<)");
    strip_tip(STRIP_FASTER, tip, sizeof tip);
    CHECK_STR(tip, "Faster (>)");
    strip_tip(STRIP_NONE, tip, sizeof tip);
    CHECK_STR(tip, "");
}

/* A press on a button does what its key does. */
TEST(strip_buttons_do_what_their_keys_do) {
    char err[256];
    setenv("LOONY_WATCH", "1", 1);
    setenv("LOONY_REPLAY", "game.lwreplay", 1);
    CHECK(watch_open(err, sizeof err));
    strip_reset(fake_now);
    watch_start("Game");
    const display_overlay *o = strip_overlay();
    display_layout l = window_layout();
    float y = 600 + STRIP_HEIGHT / 2, b = STRIP_HEIGHT, x0 = b / 4;
    watch_state s;
    watch_get_state(&s);
    CHECK(!s.paused);
    CHECK_EQ(s.speed, 1);
    o->press(&l, x0 + b / 2, y, true);
    o->press(&l, x0 + b / 2, y, false);
    watch_get_state(&s);
    CHECK(s.paused);
    o->press(&l, x0 + 3.5f * b, y, true); /* slower: at 1x already */
    watch_get_state(&s);
    CHECK_EQ(s.speed, 1);
    for (float x = x0 + 4 * b; x < 1000; x += 1)
        if (strip_button_at(&l, x, y) == STRIP_FASTER) {
            o->press(&l, x, y, true);
            break;
        }
    watch_get_state(&s);
    CHECK_EQ(s.speed, 2);
    o->press(&l, x0 + b / 2, 300, true); /* not a button */
    watch_get_state(&s);
    CHECK(s.paused);
    watch_reset();
}
