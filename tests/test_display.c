#include "test.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <unistd.h>

#include "display.h"
#include "guest_mem.h"
#include "memmgr.h"
#include "qd.h"
#include "util.h"

TEST(display_writes_the_screen_as_png) {
    gm_init();
    mm_init();
    qd_init(800, 600, 8);
    const char *t = getenv("TMPDIR");
    char path[1024];
    snprintf(path, sizeof path, "%s/loony-screen-XXXXXX", t && *t ? t : "/tmp");
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
    CHECK(display_write_png(path));
    size_t len;
    uint8_t *f = read_file(path, &len);
    unlink(path);
    CHECK(f != NULL);
    CHECK_EQ(rd_be32(f + 16), 800);
    CHECK_EQ(rd_be32(f + 20), 600);
    free(f);
}

TEST(display_present_works_with_the_dummy_driver) {
    gm_init();
    mm_init();
    qd_init(64, 48, 16);
    unsigned before = display_frames();
    display_present();          /* always presents */
    display_present_if_dirty(); /* qd_init left the screen dirty: presents */
    display_present_if_dirty(); /* nothing new: doesn't */
    CHECK_EQ(display_frames(), before + 2);
}

/* The pump presents when the display is due: before the window opens,
   three quarters of a refresh after the last present, and always without
   vsync (the fixed clock). */
TEST(display_is_due_most_of_a_refresh_after_a_present) {
    gm_init();
    mm_init();
    qd_init(64, 48, 16);
    CHECK(display_due()); /* no window yet */
    display_present();
    CHECK(!display_due());
    SDL_Delay(30); /* a refresh at 34 Hz or more, the rate when SDL reports none */
    CHECK(display_due());
    display_present();
    display_set_vsync(false);
    CHECK(display_due());
}

static int keys, quits, last_scancode, clicks, mouse_x, mouse_y;
static char pasted[64];
static bool last_down;
static void on_key(int sc, bool down, bool repeat) {
    (void)repeat;
    keys++;
    last_scancode = sc;
    last_down = down;
}
static void on_focus(bool active) { (void)active; }
static void on_quit(void) { quits++; }
static void on_mouse(int x, int y, bool down) {
    clicks += down;
    mouse_x = x;
    mouse_y = y;
}
static void on_paste(const char *t) { snprintf(pasted, sizeof pasted, "%s", t); }
static int marks, mark_number;
static int on_mark(void) {
    marks++;
    return mark_number;
}

static void push_key(SDL_Scancode sc, bool down, SDL_Keymod mod) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = sc;
    e.key.down = down;
    e.key.mod = mod;
    SDL_PushEvent(&e);
}

TEST(display_poll_forwards_keys_but_keeps_cmd_q_and_cmd_f) {
    gm_init();
    mm_init();
    qd_init(64, 48, 16);
    display_present(); /* opens the (dummy) window */
    static const display_input in = {on_key, on_focus, on_quit, on_mouse, on_paste, on_mark};
    display_set_input(&in);
    keys = quits = 0;
    push_key(SDL_SCANCODE_Z, true, SDL_KMOD_NONE);
    push_key(SDL_SCANCODE_Z, false, SDL_KMOD_NONE);
    display_poll();
    CHECK_EQ(keys, 2);
    CHECK_EQ(last_scancode, SDL_SCANCODE_Z);
    CHECK(!last_down);
    push_key(SDL_SCANCODE_Q, true, SDL_KMOD_LGUI);
    push_key(SDL_SCANCODE_F, true, SDL_KMOD_LGUI);
    push_key(SDL_SCANCODE_F, false, SDL_KMOD_LGUI);
    display_poll();
    CHECK_EQ(keys, 2);
    CHECK_EQ(quits, 1);
    SDL_Event q;
    memset(&q, 0, sizeof q);
    q.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&q);
    display_poll();
    CHECK_EQ(quits, 2);
}

static const char *window_title(void) {
    int n = 0;
    SDL_Window **w = SDL_GetWindows(&n);
    const char *title = w && n > 0 ? SDL_GetWindowTitle(w[0]) : "";
    SDL_free(w);
    return title;
}

/* Cmd-S marks a clip once per press, never reaches the game, and says so in
   the window's title. */
TEST(display_poll_marks_a_clip_on_cmd_s) {
    gm_init();
    mm_init();
    qd_init(64, 48, 16);
    display_present();
    static const display_input in = {on_key, on_focus, on_quit, on_mouse, on_paste, on_mark};
    display_set_input(&in);
    display_set_title("Loony");
    keys = marks = 0;
    mark_number = 2;
    push_key(SDL_SCANCODE_S, true, SDL_KMOD_LGUI);
    push_key(SDL_SCANCODE_S, false, SDL_KMOD_LGUI);
    display_poll();
    CHECK_EQ(keys, 0);
    CHECK_EQ(marks, 1);
    CHECK_STR(window_title(), "Loony: clip 2 marked");
    display_set_title("Loony"); /* the message stays for its time */
    CHECK_STR(window_title(), "Loony: clip 2 marked");
    mark_number = 0;
    push_key(SDL_SCANCODE_S, true, SDL_KMOD_RGUI);
    display_poll();
    CHECK_EQ(marks, 2);
    CHECK_STR(window_title(), "Loony: no game is being recorded, nothing marked");
    push_key(SDL_SCANCODE_S, true, SDL_KMOD_NONE); /* S alone is the game's */
    display_poll();
    CHECK_EQ(marks, 2);
    CHECK_EQ(keys, 1);
}

static void push_click(float x, float y, bool down, Uint8 button) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.button = button;
    e.button.down = down;
    e.button.x = x;
    e.button.y = y;
    SDL_PushEvent(&e);
}

TEST(display_poll_maps_clicks_to_the_screen_and_pastes) {
    gm_init();
    mm_init();
    qd_init(64, 48, 16);
    display_present();
    static const display_input in = {on_key, on_focus, on_quit, on_mouse, on_paste, on_mark};
    display_set_input(&in);
    /* Whichever test opened the window chose its size; its center is the
       screen's center either way. */
    int n = 0;
    SDL_Window **wins = SDL_GetWindows(&n);
    CHECK(n == 1);
    int ww, wh;
    SDL_GetWindowSize(wins[0], &ww, &wh);
    SDL_free(wins);
    keys = clicks = 0;
    push_click((float)ww / 2, (float)wh / 2, true, SDL_BUTTON_LEFT);
    push_click((float)ww / 2, (float)wh / 2, false, SDL_BUTTON_LEFT);
    push_click(5, 5, true, SDL_BUTTON_RIGHT); /* ignored */
    display_poll();
    CHECK_EQ(clicks, 1);
    CHECK_EQ(mouse_x, 32);
    CHECK_EQ(mouse_y, 24);
    pasted[0] = '\0';
    CHECK(SDL_SetClipboardText("me@example.com"));
    push_key(SDL_SCANCODE_V, true, SDL_KMOD_LGUI);
    push_key(SDL_SCANCODE_V, false, SDL_KMOD_LGUI);
    display_poll();
    CHECK_STR(pasted, "me@example.com");
    CHECK_EQ(keys, 0); /* the game never sees Cmd-V */
    display_set_cursor(false);
    display_set_cursor(true);
}

static int opens, opens_in_poll, key_events, keys_at_open;
static char opened[256];
static uint32_t opened_from;
static bool polling; /* inside the key callback, as a menu's action runs inside an event */
static void on_open(const char *path, uint32_t from) {
    opens++;
    opens_in_poll += polling;
    keys_at_open = key_events;
    snprintf(opened, sizeof opened, "%s", path);
    opened_from = from;
}
static void on_key_opening(int sc, bool down, bool repeat) {
    (void)sc;
    (void)repeat;
    key_events++;
    if (!down)
        return;
    polling = true;
    display_request_open("/r/game.lwreplay", 3600);
    polling = false;
}

/* A file asked to be opened while events are handled (a menu item's
   action), or dropped, opens once the events are handled: once, with its
   start. */
TEST(display_poll_opens_a_requested_file_after_its_events) {
    gm_init();
    mm_init();
    qd_init(64, 48, 16);
    display_present();
    static const display_input in = {on_key_opening, on_focus, on_quit, on_mouse, on_paste, on_mark};
    display_set_input(&in);
    display_set_open(on_open);
    opens = opens_in_poll = key_events = 0;
    push_key(SDL_SCANCODE_A, true, SDL_KMOD_NONE);
    push_key(SDL_SCANCODE_A, false, SDL_KMOD_NONE);
    display_poll();
    CHECK_EQ(opens, 1);
    CHECK_EQ(opens_in_poll, 0);
    CHECK_EQ(keys_at_open, 2); /* after the key-up that came next */
    CHECK_STR(opened, "/r/game.lwreplay");
    CHECK_EQ(opened_from, 3600);
    display_poll(); /* nothing more */
    CHECK_EQ(opens, 1);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_DROP_FILE;
    e.drop.data = "/r/dropped.lwreplay";
    SDL_PushEvent(&e);
    display_poll();
    CHECK_EQ(opens, 2);
    CHECK_STR(opened, "/r/dropped.lwreplay");
    CHECK_EQ(opened_from, 0);
    display_set_open(NULL);
}

static int closes;
static char closed_why[256];
static void on_close(const char *why) {
    closes++;
    snprintf(closed_why, sizeof closed_why, "%s", why);
}

TEST(display_poll_closes_a_replay_before_opening_one) {
    gm_init();
    mm_init();
    qd_init(64, 48, 16);
    display_present();
    static const display_input in = {on_key_opening, on_focus, on_quit, on_mouse, on_paste, on_mark};
    display_set_input(&in);
    display_set_open(on_open);
    display_set_close(on_close);
    opens = closes = key_events = 0;
    display_request_open("/r/game.lwreplay", 0);
    display_request_close("gone");
    CHECK_EQ(closes, 0); /* not until display_poll */
    push_key(SDL_SCANCODE_O, true, SDL_KMOD_NONE);
    display_poll();
    CHECK_EQ(closes, 1);
    CHECK_EQ(key_events, 0); /* left for the restart to drop */
    CHECK_EQ(opens, 0); /* the restart would have gone first */
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST); /* as the restart would */
    CHECK_STR(closed_why, "gone");
    display_poll(); /* nothing more */
    CHECK_EQ(closes, 1);
    CHECK_EQ(opens, 0);
    display_set_open(NULL);
    display_set_close(NULL);
}

static void push_drop(const char *path) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_DROP_FILE;
    e.drop.data = path;
    SDL_PushEvent(&e);
}

/* Of several files dropped at once, the first replay opens, whatever comes
   before or after it. */
TEST(display_poll_opens_the_first_replay_of_several_dropped) {
    gm_init();
    mm_init();
    qd_init(64, 48, 16);
    display_present();
    display_set_open(on_open);
    opens = 0;
    push_drop("/r/notes.txt");
    push_drop("/r/first.lwreplay");
    push_drop("/r/second.lwreplay");
    push_drop("/r/after.txt");
    display_poll();
    CHECK_EQ(opens, 1);
    CHECK_STR(opened, "/r/first.lwreplay");
    push_drop("/r/notes.txt");
    display_poll(); /* not a replay: still handed on, to be turned down */
    CHECK_EQ(opens, 2);
    CHECK_STR(opened, "/r/notes.txt");
    display_set_open(NULL);
}

static bool rect_is(display_rect r, float x, float y, float w, float h) {
    return r.x == x && r.y == y && r.w == w && r.h == h;
}

/* The game is letterboxed as SDL's logical presentation did it; a strip
   goes below the game in a window and over it in full screen. */
TEST(display_lay_out_letterboxes_the_game_and_places_the_strip) {
    display_layout l = display_lay_out(1600, 1200, 800, 600, 0, false);
    CHECK(rect_is(l.game, 0, 0, 1600, 1200));
    l = display_lay_out(1000, 600, 800, 600, 0, false); /* bars at the sides */
    CHECK(rect_is(l.game, 100, 0, 800, 600));
    l = display_lay_out(800, 700, 800, 600, 0, false); /* above and below */
    CHECK(rect_is(l.game, 0, 50, 800, 600));
    l = display_lay_out(840, 630, 800, 600, 0, false); /* 4:3, though 630 / 600 isn't exact */
    CHECK(rect_is(l.game, 0, 0, 840, 630));
    l = display_lay_out(1600, 1264, 800, 600, 64, false);
    CHECK(rect_is(l.game, 0, 0, 1600, 1200));
    CHECK(rect_is(l.strip, 0, 1200, 1600, 64));
    l = display_lay_out(1920, 1200, 800, 600, 64, true); /* over the game's bottom edge */
    CHECK(rect_is(l.game, 160, 0, 1600, 1200));
    CHECK(rect_is(l.strip, 0, 1136, 1920, 64));
    l = display_lay_out(100, 30, 800, 600, 64, false); /* no room for the game */
    CHECK_EQ((int)l.game.w, 0);
}

static int strip_presses, strip_releases, strip_moves, strip_leaves;
static float strip_up = 1;
static float press_x, press_y;
static void ov_draw(void *r, const display_layout *l) {
    /* the strip's place, white */
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    SDL_FRect f = {l->strip.x, l->strip.y, l->strip.w, l->strip.h};
    if (strip_up > 0)
        SDL_RenderFillRect(r, &f);
}
static bool ov_changed(void) { return false; }
static float ov_shown(void) { return strip_up; }
static void ov_pointer(const display_layout *l, float x, float y, bool in) {
    (void)l, (void)x, (void)y;
    strip_moves++;
    strip_leaves += !in;
}
static void ov_press(const display_layout *l, float x, float y, bool down) {
    (void)l;
    strip_presses += down;
    strip_releases += !down;
    press_x = x;
    press_y = y;
}
static char ov_said[64];
static void ov_say(const char *msg) { snprintf(ov_said, sizeof ov_said, "%s", msg); }
static const display_overlay test_overlay = {16,       100,        ov_draw,  ov_changed,
                                             ov_shown, ov_pointer, ov_press, ov_say};

/* With an overlay, a message goes to it rather than the window's title,
   window or not. */
TEST(display_flash_goes_to_the_overlay) {
    display_set_overlay(&test_overlay);
    display_flash("clip 1 saved: 0:10-0:20");
    CHECK_STR(ov_said, "clip 1 saved: 0:10-0:20");
    display_set_overlay(NULL);
}

/* With an overlay, the window grows by its strip, a click on the strip is
   its and a click on the game still maps to the screen, and the pointer's
   movements reach it. */
TEST(display_poll_gives_clicks_on_the_strip_to_the_overlay) {
    gm_init();
    mm_init();
    qd_init(64, 48, 16);
    display_set_overlay(&test_overlay);
    display_present(); /* a 128 x 96 game and the 16-point strip */
    static const display_input in = {on_key, on_focus, on_quit, on_mouse, on_paste, on_mark};
    display_set_input(&in);
    int n = 0;
    SDL_Window **wins = SDL_GetWindows(&n);
    CHECK(n == 1);
    int ww, wh;
    SDL_GetWindowSize(wins[0], &ww, &wh);
    SDL_free(wins);
    CHECK_EQ(ww, 128);
    CHECK_EQ(wh, 112);
    clicks = strip_presses = strip_moves = 0;
    push_click(64, 48, true, SDL_BUTTON_LEFT);
    push_click(64, 104, true, SDL_BUTTON_LEFT);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.x = 10;
    e.motion.y = 10;
    SDL_PushEvent(&e);
    display_poll();
    CHECK_EQ(clicks, 1);
    CHECK_EQ(mouse_x, 32);
    CHECK_EQ(mouse_y, 24);
    CHECK_EQ(strip_presses, 1);
    CHECK_EQ((int)press_x, 64);
    CHECK_EQ((int)press_y, 104);
    CHECK(strip_moves >= 1); /* SDL moves the pointer to each click too */
    display_set_overlay(NULL);
}

static void push_motion(float x, float y) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.x = x;
    e.motion.y = y;
    SDL_PushEvent(&e);
}

/* A press on the strip is a drag: its release is the strip's wherever it
   comes, over the game too, the pointer leaving the window meanwhile isn't
   news to it, and the window losing focus first, or the button going down
   again, releases it where the pointer was. A release with no press on the strip before it
   is the game's. */
TEST(display_poll_gives_a_drag_on_the_strip_its_release) {
    gm_init();
    mm_init();
    qd_init(64, 48, 16);
    display_set_overlay(&test_overlay);
    display_present(); /* a 128 x 96 game and the 16-point strip */
    static const display_input in = {on_key, on_focus, on_quit, on_mouse, on_paste, on_mark};
    display_set_input(&in);
    display_poll(); /* what opening the window queued */
    clicks = strip_presses = strip_releases = strip_leaves = 0;
    mouse_x = -1;
    push_click(64, 104, true, SDL_BUTTON_LEFT);
    SDL_Event leave;
    memset(&leave, 0, sizeof leave);
    leave.type = SDL_EVENT_WINDOW_MOUSE_LEAVE;
    SDL_PushEvent(&leave);
    push_motion(30, 20);
    push_click(30, 20, false, SDL_BUTTON_LEFT); /* over the game */
    display_poll();
    CHECK_EQ(strip_presses, 1);
    CHECK_EQ(strip_releases, 1);
    CHECK_EQ((int)press_y, 20);
    CHECK_EQ(mouse_x, -1);
    CHECK_EQ(strip_leaves, 0);
    SDL_PushEvent(&leave); /* not dragging: the strip hears of it */
    display_poll();
    CHECK_EQ(strip_leaves, 1);
    push_click(30, 20, false, SDL_BUTTON_LEFT); /* no press before it: the game's */
    display_poll();
    CHECK_EQ(strip_releases, 1);
    CHECK_EQ(mouse_x, 15);
    push_click(64, 104, true, SDL_BUTTON_LEFT);
    push_motion(100, 10);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    SDL_PushEvent(&e);
    display_poll();
    CHECK_EQ(strip_presses, 2);
    CHECK_EQ(strip_releases, 2);
    CHECK_EQ((int)press_x, 100);
    CHECK_EQ((int)press_y, 10);
    push_click(70, 104, true, SDL_BUTTON_LEFT); /* not moved since: released where it was */
    SDL_PushEvent(&e);
    display_poll();
    CHECK_EQ(strip_releases, 3);
    CHECK_EQ((int)press_x, 70);
    push_click(64, 104, true, SDL_BUTTON_LEFT);
    push_click(20, 104, true, SDL_BUTTON_LEFT); /* the release was lost: one first */
    display_poll();
    CHECK_EQ(strip_presses, 5);
    CHECK_EQ(strip_releases, 4);
    CHECK_EQ((int)press_x, 20);
    CHECK_EQ(clicks, 0);
    display_set_overlay(NULL);
}

/* The overlay draws only in the window: with the strip up or down, the
   game's pixels in the window and the screen's hash (what replays check,
   and what screenshots and clips are made from) are the same, and the
   strip's place isn't. */
TEST(display_overlay_leaves_the_game_and_its_hash_alone) {
    gm_init();
    mm_init();
    qd_init(64, 48, 16);
    qd_pixels px;
    qd_palette pal;
    qd_screen(&px, &pal);
    size_t len = (size_t)px.row_bytes * (size_t)rect_h(px.bounds);
    for (size_t i = 0; i < len; i++)
        px.base[i] = (uint8_t)(i * 7);
    display_set_overlay(&test_overlay);
    strip_up = 1;
    display_present();
    uint32_t hash_up = fnv1a32(px.base, len);
    int w1, h1, w2, h2;
    uint8_t *up = display_read_window(&w1, &h1);
    CHECK(up != NULL);
    strip_up = 0;
    display_present();
    uint8_t *down = display_read_window(&w2, &h2);
    CHECK(down != NULL);
    CHECK_EQ(w1, w2);
    CHECK_EQ(h1, h2);
    CHECK_EQ(h1, 112);
    size_t game = (size_t)w1 * 96 * 4; /* the rows above the strip */
    CHECK(memcmp(up, down, game) == 0);
    CHECK(memcmp(up + game, down + game, (size_t)w1 * 16 * 4) != 0);
    CHECK_EQ(fnv1a32(px.base, len), hash_up);
    free(up);
    free(down);
    display_set_overlay(NULL);
}
