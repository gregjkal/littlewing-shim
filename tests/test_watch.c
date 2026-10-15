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

/* Pressing keys: K pauses as Space does (YouTube's key), a repeat or a
   release does nothing, and Right speeds up. Logged by watch_key. */
static void child_press_keys(void *unused) {
    (void)unused;
    char err[256];
    clear_env();
    setenv("LOONY_WATCH", "1", 1);
    setenv("LOONY_REPLAY", "game.lwreplay", 1);
    if (!watch_open(err, sizeof err))
        _exit(1);
    watch_start("Game", NULL);
    watch_key(SDL_SCANCODE_K, true, false);
    watch_key(SDL_SCANCODE_K, true, true);   /* held: no change */
    watch_key(SDL_SCANCODE_K, false, false); /* released */
    watch_key(SDL_SCANCODE_SPACE, true, false);
    watch_key(SDL_SCANCODE_RIGHT, true, false);
    _exit(0);
}

TEST(watch_k_pauses_like_space) {
    char out[4096];
    CHECK_EQ(test_run_child(child_press_keys, NULL, out, sizeof out), 0);
    const char *paused = strstr(out, "watch: paused at 0:00");
    CHECK(paused);
    CHECK(paused && !strstr(paused + 1, "watch: paused"));
    CHECK_CONTAINS(out, "watch: going on at 0:00");
    CHECK_CONTAINS(out, "watch: 2x");
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
