#include "test.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

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
    t = 70; /* resting on the strip, then out of the window with no movement */
    o->pointer(&l, 500, 600 + STRIP_HEIGHT / 2, true);
    t = 80;
    o->pointer(&l, 0, 0, false);
    CHECK(o->shown() == 1);
    t = 80 + STRIP_HIDE_AFTER + STRIP_FADE / 2; /* it fades from then */
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

/* A 15-minute recording with 3 marks and 2 clips, the second clip starting
   on the third mark. */
static const uint32_t marks[] = {3600, 18000, 36000};
static const clip_span clips[] = {{6000, 9000, 0, 0}, {36000, 39000, 0, 0}};
static watch_state timeline(void) {
    watch_state w;
    memset(&w, 0, sizeof w);
    w.length = 54000;
    w.speed = 1;
    w.marks = marks;
    w.nmarks = 3;
    w.clips = clips;
    w.nclips = 2;
    return w;
}

/* On the bar: a mark or a clip's start within STRIP_SNAP points, marks
   first, else the time under the pointer; off it, nothing. */
TEST(strip_spot_at_finds_marks_clips_and_times) {
    strip_reset(fake_now);
    display_layout l = window_layout();
    watch_state w = timeline();
    display_rect bar = strip_bar(&l, &w);
    CHECK(bar.w > 300);
    float y = 600 + STRIP_HEIGHT / 2;
#define AT(tick) (bar.x + bar.w * (float)(tick) / 54000)
    strip_spot s = strip_spot_at(&l, &w, AT(18000) + STRIP_SNAP - 1, y);
    CHECK_EQ(s.on, STRIP_ON_MARK);
    CHECK_EQ(s.index, 1);
    CHECK_EQ(s.tick, 18000);
    s = strip_spot_at(&l, &w, AT(6000) - 2, 600 + 2); /* the strip's height counts */
    CHECK_EQ(s.on, STRIP_ON_CLIP);
    CHECK_EQ(s.index, 0);
    CHECK_EQ(s.tick, 6000);
    s = strip_spot_at(&l, &w, AT(36000), y); /* a mark and a clip's start: the mark */
    CHECK_EQ(s.on, STRIP_ON_MARK);
    CHECK_EQ(s.index, 2);
    s = strip_spot_at(&l, &w, AT(27000), y);
    CHECK_EQ(s.on, STRIP_ON_TIME);
    CHECK(s.tick > 26800 && s.tick < 27200);
    s = strip_spot_at(&l, &w, bar.x - 2, y); /* just before the bar: the start */
    CHECK_EQ(s.on, STRIP_ON_TIME);
    CHECK_EQ(s.tick, 0);
    s = strip_spot_at(&l, &w, bar.x + bar.w + 2, y);
    CHECK_EQ(s.tick, 54000);
    CHECK_EQ(strip_spot_at(&l, &w, AT(27000), 300).on, STRIP_ON_NOTHING); /* the game */
    CHECK_EQ(strip_spot_at(&l, &w, 10, y).on, STRIP_ON_NOTHING);         /* the buttons */
#undef AT
}

TEST(strip_spot_tip_says_the_time_a_mark_or_a_clip) {
    watch_state w = timeline();
    char tip[64];
    strip_spot s = {STRIP_ON_TIME, -1, 34830};
    strip_spot_tip(&s, &w, tip, sizeof tip);
    CHECK_STR(tip, "9:40");
    s = (strip_spot){STRIP_ON_MARK, 1, 18000};
    strip_spot_tip(&s, &w, tip, sizeof tip);
    CHECK_STR(tip, "Mark 2: 5:00");
    s = (strip_spot){STRIP_ON_CLIP, 0, 6000};
    strip_spot_tip(&s, &w, tip, sizeof tip);
    CHECK_STR(tip, "Clip 1: 1:40-2:30");
    s = (strip_spot){STRIP_ON_NOTHING, -1, 0};
    strip_spot_tip(&s, &w, tip, sizeof tip);
    CHECK_STR(tip, "");
    s = (strip_spot){STRIP_ON_CLIP, 1, 36000}; /* the list was read again, shorter */
    w.nclips = 1;
    strip_spot_tip(&s, &w, tip, sizeof tip);
    CHECK_STR(tip, "");
}

/* [ goes to the last mark more than WATCH_MARK_GRACE_TICKS before now, so
   that pressed again just after landing on one it goes on to the one
   before; ] to the first after now. */
TEST(watch_mark_from_finds_the_marks_before_and_after) {
    watch_state w = timeline();
    w.now = 18000;
    CHECK_EQ(watch_mark_from(&w, 1), 2);
    CHECK_EQ(watch_mark_from(&w, -1), 0);
    w.now = 18000 + WATCH_MARK_GRACE_TICKS + 1;
    CHECK_EQ(watch_mark_from(&w, -1), 1);
    w.now = 36000;
    CHECK_EQ(watch_mark_from(&w, 1), -1);
    w.now = 3600;
    CHECK_EQ(watch_mark_from(&w, -1), -1);
    w.nmarks = 0;
    CHECK_EQ(watch_mark_from(&w, 1), -1);
}

/* The speed asked, or the speed reached when it falls short or at max. */
TEST(strip_speed_text_says_the_speed_reached) {
    watch_state w;
    memset(&w, 0, sizeof w);
    char t[16];
    w.speed = 8;
    CHECK(!strip_speed_text(&w, t, sizeof t)); /* not known yet */
    CHECK_STR(t, "8x");
    w.reached = 7.6f; /* close enough */
    CHECK(!strip_speed_text(&w, t, sizeof t));
    CHECK_STR(t, "8x");
    w.reached = 5.2f; /* HD falling behind */
    CHECK(strip_speed_text(&w, t, sizeof t));
    CHECK_STR(t, "5.2x");
    w.speed = 0;
    w.reached = 0;
    CHECK(!strip_speed_text(&w, t, sizeof t));
    CHECK_STR(t, "max");
    w.reached = 17.7f;
    CHECK(!strip_speed_text(&w, t, sizeof t));
    CHECK_STR(t, "18x");
    w.reached = 8.8f;
    CHECK(!strip_speed_text(&w, t, sizeof t));
    CHECK_STR(t, "8.8x");
}

/* The overlay drawn into a 1000 x 632 surface cleared to blue. */
static SDL_Surface *surface;
static SDL_Renderer *renderer;
static void draw_strip(const display_overlay *o, const display_layout *l) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);
    SDL_RenderClear(renderer);
    o->draw(renderer, l);
    SDL_FlushRenderer(renderer);
}
static bool blue_at(int x, int y) {
    Uint8 r, g, b, a;
    SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a);
    return r == 0 && g == 0 && b == 255;
}
static int red_at(int x, int y) {
    Uint8 r, g, b, a;
    SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a);
    return r;
}
static void start_watching(void) {
    char err[256];
    setenv("LOONY_WATCH", "1", 1);
    setenv("LOONY_REPLAY", "game.lwreplay", 1);
    CHECK(watch_open(err, sizeof err));
    strip_reset(fake_now);
    watch_start("Game");
    surface = SDL_CreateSurface(1000, 600 + STRIP_HEIGHT, SDL_PIXELFORMAT_RGBA32);
    renderer = SDL_CreateSoftwareRenderer(surface);
}

/* Pausing and going on show their sign over the middle of the game, which
   fades; the strip's own drawing is left alone. */
TEST(strip_shows_a_sign_for_pause_and_play) {
    t = 1;
    start_watching();
    CHECK(renderer);
    const display_overlay *o = strip_overlay();
    display_layout l = window_layout();
    draw_strip(o, &l);
    CHECK(blue_at(500, 300)); /* nothing over the game yet */
    t = 10;
    draw_strip(o, &l); /* the strip has gone */
    CHECK(!o->changed());
    watch_act(WATCH_PLAY_PAUSE, false);
    CHECK(o->changed());
    draw_strip(o, &l);
    CHECK(!blue_at(500, 300)); /* the sign's disc, between the bars */
    t = 10 + STRIP_SIGN / 2;
    CHECK(o->changed()); /* fading */
    t = 10 + STRIP_SIGN;
    draw_strip(o, &l);
    CHECK(blue_at(500, 300));
    watch_act(WATCH_PLAY_PAUSE, false); /* going on: the play sign */
    draw_strip(o, &l);
    CHECK(!blue_at(500, 300));
    watch_reset();
}

/* What the viewer says goes above the strip, which stays up while it's
   shown, longer than it would for a key. */
TEST(strip_shows_what_the_viewer_says) {
    t = 1;
    start_watching();
    CHECK(renderer);
    const display_overlay *o = strip_overlay();
    display_layout l = window_layout();
    float x = STRIP_HEIGHT / 4 + 2, y = 600 - 15; /* inside the message's box */
    t = 10;
    draw_strip(o, &l);
    CHECK(blue_at((int)x, (int)y));
    CHECK(o->shown() == 0);
    display_flash("clip 1 saved: 0:10-0:20"); /* through the overlay */
    CHECK(o->changed());
    t = 10 + STRIP_FADE;
    CHECK(o->shown() == 1);
    draw_strip(o, &l);
    CHECK(!blue_at((int)x, (int)y));
    t = 10 + DISPLAY_FLASH_MS / 1000.0 - 0.01; /* past STRIP_HIDE_AFTER */
    CHECK(o->shown() == 1);
    t = 10 + DISPLAY_FLASH_MS / 1000.0 + 0.01;
    draw_strip(o, &l); /* gone, as the strip fades */
    CHECK(blue_at((int)x, (int)y));
    t = 10 + DISPLAY_FLASH_MS / 1000.0 + STRIP_FADE;
    CHECK(o->shown() == 0);
    watch_reset();
}

/* A change of speed lights the speed up for a moment. */
TEST(strip_lights_the_speed_when_it_changes) {
    t = 1;
    start_watching();
    CHECK(renderer);
    const display_overlay *o = strip_overlay();
    display_layout l = window_layout();
    float b = STRIP_HEIGHT;
    int x = (int)(b / 4 + 3 * b + b / 4 + 0.75f * b + 1), y = 600 + 2; /* the speed's corner */
    CHECK(o->shown() == 0); /* fading in */
    t = 1 + STRIP_FADE;
    draw_strip(o, &l);
    int dark = red_at(x, y);
    watch_act(WATCH_FASTER, false);
    CHECK(o->changed());
    draw_strip(o, &l);
    CHECK(red_at(x, y) > dark + 30);
    t = 1 + STRIP_FADE + STRIP_SPEED_LIT;
    draw_strip(o, &l);
    CHECK_EQ(red_at(x, y), dark);
    watch_reset();
}
