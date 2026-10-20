#include "test.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <unistd.h>

#include "watch.h"

static const char *const env_names[] = {"LOONY_WATCH",       "LOONY_REPLAY",  "LOONY_WATCH_FROM",
                                        "LOONY_WATCH_SPEED", "LOONY_WATCH_IN", "LOONY_WATCH_PAUSED"};

static void clear_env(void) {
    for (size_t i = 0; i < sizeof env_names / sizeof env_names[0]; i++)
        unsetenv(env_names[i]);
}

/* watch_open with LOONY_WATCH=1, the replay and one more setting (name may
   be NULL): its result, with err. */
static bool open_with(const char *replay, const char *name, const char *value, char *err, size_t cap) {
    clear_env();
    setenv("LOONY_WATCH", "1", 1);
    if (replay)
        setenv("LOONY_REPLAY", replay, 1);
    if (name)
        setenv(name, value, 1);
    err[0] = '\0';
    bool ok = watch_open(err, cap);
    clear_env();
    return ok;
}

TEST(watch_open_checks_its_settings) {
    char err[256];
    clear_env();
    CHECK(watch_open(err, sizeof err));
    CHECK(!watch_on());
    setenv("LOONY_WATCH", "0", 1);
    CHECK(watch_open(err, sizeof err));
    CHECK(!watch_on());
    CHECK(!open_with(NULL, NULL, NULL, err, sizeof err));
    CHECK_CONTAINS(err, "LOONY_WATCH needs a replay (LOONY_REPLAY)");
    CHECK(open_with("game.lwreplay", NULL, NULL, err, sizeof err));
    CHECK(watch_on());
    CHECK(open_with("game.lwreplay", "LOONY_WATCH_SPEED", "0", err, sizeof err));
    CHECK(open_with("game.lwreplay", "LOONY_WATCH_SPEED", "8", err, sizeof err));
    CHECK(!open_with("game.lwreplay", "LOONY_WATCH_SPEED", "3", err, sizeof err));
    CHECK_CONTAINS(err, "LOONY_WATCH_SPEED=3 isn't 1, 2, 4, 8 or 0");
    CHECK(!open_with("game.lwreplay", "LOONY_WATCH_SPEED", "16", err, sizeof err));
    CHECK(open_with("game.lwreplay", "LOONY_WATCH_FROM", "36000", err, sizeof err));
    CHECK(!open_with("game.lwreplay", "LOONY_WATCH_FROM", "9:40", err, sizeof err));
    CHECK_CONTAINS(err, "LOONY_WATCH_FROM=9:40 isn't a whole number");
    CHECK(!open_with("game.lwreplay", "LOONY_WATCH_FROM", "-60", err, sizeof err));
    CHECK(!open_with("game.lwreplay", "LOONY_WATCH_IN", "x", err, sizeof err));
    CHECK_CONTAINS(err, "LOONY_WATCH_IN=x");
    watch_reset();
}

/* Watching game.lwreplay, which isn't there: enough for the keys. */
static void start_watching(void) {
    char err[256];
    clear_env();
    setenv("LOONY_WATCH", "1", 1);
    setenv("LOONY_REPLAY", "game.lwreplay", 1);
    if (!watch_open(err, sizeof err))
        _exit(1);
    watch_start("Game");
}

/* Pressing keys: K pauses as Space does (YouTube's key), and a repeat or a
   release does nothing. Logged by watch_key. */
static void child_press_keys(void *unused) {
    (void)unused;
    start_watching();
    watch_key(SDL_SCANCODE_K, true, false);
    watch_key(SDL_SCANCODE_K, true, true);   /* held: no change */
    watch_key(SDL_SCANCODE_K, false, false); /* released */
    watch_key(SDL_SCANCODE_SPACE, true, false);
    _exit(0);
}

TEST(watch_k_pauses_like_space) {
    char out[4096];
    CHECK_EQ(test_run_child(child_press_keys, NULL, out, sizeof out), 0);
    const char *paused = strstr(out, "watch: paused at 0:00");
    CHECK(paused);
    CHECK(paused && !strstr(paused + 1, "watch: paused"));
    CHECK_CONTAINS(out, "watch: going on at 0:00");
}

/* Right goes ahead 5 s and Left back 5 s, from the time going to; speed
   is on Shift-comma and Shift-period, as on YouTube; Shift-L goes ahead a
   minute, and a key with Cmd isn't the viewer's. */
static void child_press_moved_keys(void *unused) {
    (void)unused;
    start_watching();
    watch_key(SDL_SCANCODE_RIGHT, true, false);
    watch_key(SDL_SCANCODE_LEFT, true, false);
    watch_key(SDL_SCANCODE_LGUI, true, false);
    watch_key(SDL_SCANCODE_L, true, false); /* Cmd-L: not the viewer's */
    watch_key(SDL_SCANCODE_LGUI, false, false);
    watch_key(SDL_SCANCODE_RSHIFT, true, false);
    watch_key(SDL_SCANCODE_PERIOD, true, false);
    watch_key(SDL_SCANCODE_PERIOD, true, true); /* held: no change */
    watch_key(SDL_SCANCODE_L, true, false);
    watch_key(SDL_SCANCODE_RSHIFT, false, false);
    watch_key(SDL_SCANCODE_PERIOD, true, false); /* without Shift, playing: nothing */
    watch_key(SDL_SCANCODE_LSHIFT, true, false);
    watch_key(SDL_SCANCODE_COMMA, true, false);
    _exit(0);
}

/* Comma and period step a frame only while paused, as on YouTube. */
static void child_step_frames(void *unused) {
    (void)unused;
    start_watching();
    watch_key(SDL_SCANCODE_PERIOD, true, false); /* playing: nothing */
    watch_key(SDL_SCANCODE_COMMA, true, false);
    watch_key(SDL_SCANCODE_SPACE, true, false);
    watch_key(SDL_SCANCODE_PERIOD, true, false);
    watch_key(SDL_SCANCODE_PERIOD, true, true); /* held: another */
    _exit(0);
}

TEST(watch_comma_and_period_step_a_frame_while_paused) {
    char out[4096];
    CHECK_EQ(test_run_child(child_step_frames, NULL, out, sizeof out), 0);
    const char *paused = strstr(out, "watch: paused at 0:00");
    CHECK(paused);
    CHECK(!strstr(out, "watch: going to") || strstr(out, "watch: going to") > paused);
    const char *one = paused ? strstr(paused, "watch: going to 0:00.01\n") : NULL;
    CHECK(one);
    CHECK(one && strstr(one, "watch: going to 0:00.03\n"));
}

TEST(watch_arrows_step_5_s_and_speed_is_on_angle_brackets) {
    char out[4096];
    CHECK_EQ(test_run_child(child_press_moved_keys, NULL, out, sizeof out), 0);
    const char *ahead = strstr(out, "watch: going to 0:05");
    CHECK(ahead);
    const char *left = strstr(ahead, "watch: going to 0:00"); /* back from 0:05, not yet reached */
    CHECK(left);
    const char *faster = strstr(left, "watch: 2x");
    CHECK(faster);
    CHECK(!strstr(faster + 1, "watch: 2x"));
    CHECK(!strstr(ahead, "going to 0:15")); /* Cmd-L */
    const char *minute = strstr(faster, "watch: going to 1:00");
    CHECK(minute);
    CHECK(strstr(minute, "watch: 1x"));
    CHECK(!strstr(out, "back to"));
}

/* The Replay menu is built from watch_bindings: each action has one row
   with a label, but the tenths (0 to 9), which have a key each and no item,
   and no key does two things. */
TEST(watch_bindings_list_each_action_once) {
    for (int a = WATCH_PLAY_PAUSE; a <= WATCH_CANCEL_EXPORT; a++) {
        int labelled = 0, keys = 0;
        for (size_t i = 0; i < watch_nbindings; i++)
            if ((int)watch_bindings[i].action == a) {
                labelled += watch_bindings[i].label != NULL;
                keys += watch_bindings[i].scancode != 0;
            }
        bool tenth = a >= WATCH_TENTH_0 && a <= WATCH_TENTH_9;
        CHECK_EQ(labelled, tenth ? 0 : 1);
        if (tenth)
            CHECK_EQ(keys, 1);
    }
    for (size_t i = 0; i < watch_nbindings; i++)
        for (size_t j = i + 1; j < watch_nbindings; j++) {
            const watch_binding *a = &watch_bindings[i], *b = &watch_bindings[j];
            CHECK(!a->scancode || a->scancode != b->scancode || a->mods != b->mods);
        }
}

/* Each frame at 1x and 2x, one a frame of wall clock above, a few a second
   while going; none while the replay is behind, but one at least every
   quarter second, so a game slower than the speed asked still shows. */
TEST(watch_draws_skip_frames_behind_but_not_for_long) {
    CHECK(watch_draws(0, 0, 1, false));
    CHECK(watch_draws(0, 0, 2, false));
    CHECK(!watch_draws(0.01, 0, 8, false));
    CHECK(watch_draws(0.02, 0, 8, false));
    CHECK(!watch_draws(0.1, 0, 1, true));
    CHECK(watch_draws(0.25, 0, 1, true));
    CHECK(!watch_draws(0.02, 0.05, 8, false)); /* behind */
    CHECK(!watch_draws(0.2, 0.05, 1, false));
    CHECK(watch_draws(0.25, 0.05, 8, false)); /* behind for long */
    CHECK(watch_draws(0.02, 0.05, 0, false)); /* as fast as it can: never behind */
    CHECK(watch_draws(0.25, 5, 1, true));     /* going: never behind */
}
