#include "test.h"

#include <stdlib.h>

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
