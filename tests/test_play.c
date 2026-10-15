#include "test.h"

#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>

#include "asm.h"
#include "harness.h"
#include "play.h"
#include "replay.h"

/* A program laid out where play.c looks in Loony Labyrinth 3.0.1: the two
   entries it hooks (each mflr r0, then blr) and the words that name the
   flags; and a TOC whose flags the test sets as the game would. */
#define CODE (GUEST_IMAGE_BASE + 0x200000)
#define TOC (GUEST_IMAGE_BASE + 0x300000)
#define TV (GUEST_IMAGE_BASE + 0x400000)
#define START_NEW_GAME 0x1df90
#define RESET_GAME 0x15400
#define IN_LOOP 0x4e36
#define DEMO 0x4dd1
#define SUSPENDED 0x4e35
#define GAME_OVER 0x4e34

static const char *const names[] = {"Unused"};
static const uint8_t exe[] = "a game's program";
static char dir[1024], rec_dir[1100];

static void setup(void) {
    harness_init(names, 1);
    static const uint32_t words[][2] = {
        {0x1df90, 0x7c0802a6}, {0x1df94, PPC_BLR}, {0x1df9c, 0x3be24e36}, {0x15400, 0x7c0802a6},
        {0x15404, PPC_BLR},    {0x15408, 0x3be24e35}, {0x155c0, 0x38624dd1}, {0x15538, 0x38624e34},
    };
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++)
        gm_w32(CODE + words[i][0], words[i][1]);
}

/* The game runs the function at offset in its code. */
static void run(uint32_t offset) {
    gm_w32(TV, CODE + offset);
    gm_w32(TV + 4, TOC);
    guest_call(TV, 0, NULL);
}

static void set(uint32_t flag, bool on) { gm_w8(TOC + flag, on); }

/* The variables recording reads, as they were before the test. */
static const char *const env_names[] = {"LOONY_RECORD", "LOONY_REPLAY", "LOONY_FIXED_CLOCK"};
static char *env_saved[sizeof env_names / sizeof env_names[0]];

static int begin_test(void) {
    for (size_t i = 0; i < sizeof env_names / sizeof env_names[0]; i++) {
        const char *v = getenv(env_names[i]);
        env_saved[i] = v ? strdup(v) : NULL;
        unsetenv(env_names[i]);
    }
    test_tmp_dir(dir, sizeof dir);
    snprintf(rec_dir, sizeof rec_dir, "%s/recordings", dir);
    replay_reset();
    play_reset();
    setup();
    return 0;
}

static void end_test(int *unused) {
    (void)unused;
    replay_reset();
    play_reset();
    for (size_t i = 0; i < sizeof env_names / sizeof env_names[0]; i++) {
        if (env_saved[i])
            setenv(env_names[i], env_saved[i], 1);
        else
            unsetenv(env_names[i]);
        free(env_saved[i]);
        env_saved[i] = NULL;
    }
    test_remove_tree(dir);
}

#define BEGIN_TEST() __attribute__((cleanup(end_test))) int test_guard_ = begin_test()

/* Recording each game into rec_dir, as the app does. */
static bool record_each_game(void) {
    char err[256];
    replay_record_into(rec_dir, "loony-labyrinth", NULL, 0);
    return replay_start("loony-labyrinth", exe, sizeof exe, NULL, err, sizeof err);
}

/* The recordings in rec_dir, by name, oldest first (names hold the time). */
static int recordings(char names_out[][256], int cap) {
    int n = 0;
    DIR *d = opendir(rec_dir);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.' && n < cap)
            snprintf(names_out[n++], 256, "%s", e->d_name);
    if (d)
        closedir(d);
    for (int i = 1; i < n; i++) /* few: insertion sort */
        for (int j = i; j > 0 && strcmp(names_out[j - 1], names_out[j]) > 0; j--) {
            char t[256];
            memcpy(t, names_out[j], 256);
            memcpy(names_out[j], names_out[j - 1], 256);
            memcpy(names_out[j - 1], t, 256);
        }
    return n;
}

/* Opens rec_dir's recording name for playing, and gives its play spans. */
static size_t spans_of(const char *name, const replay_span **spans) {
    char path[1400], err[256];
    snprintf(path, sizeof path, "%s/%s", rec_dir, name);
    replay_reset();
    setenv("LOONY_REPLAY", path, 1);
    if (!replay_open(err, sizeof err))
        return (size_t)-1;
    return replay_play_spans(spans);
}

/* The demo isn't a game; a game is played while its loop runs and the
   window has focus; Esc suspends it; it ends at game over once its loop
   ends, or when Event_ResetGame throws it away suspended; Event_ResetGame
   on a game not suspended ends nothing. */
TEST(play_tells_games_and_their_play_from_the_games_state) {
    BEGIN_TEST();
    CHECK(play_init("loony-labyrinth", CODE, TOC));
    set(DEMO, true);
    run(START_NEW_GAME);
    set(IN_LOOP, true);
    play_pump(true);
    CHECK(!play_in_game());
    CHECK(!play_playing());
    set(IN_LOOP, false);
    set(DEMO, false);
    play_pump(true);

    run(START_NEW_GAME);
    set(IN_LOOP, true);
    play_pump(true);
    CHECK(play_in_game());
    CHECK(play_playing());
    play_pump(false); /* the window in the background */
    CHECK(play_in_game());
    CHECK(!play_playing());
    play_pump(true);
    CHECK(play_playing());
    set(IN_LOOP, false); /* Esc */
    set(SUSPENDED, true);
    play_pump(true);
    CHECK(play_in_game());
    CHECK(!play_playing());
    set(SUSPENDED, false); /* resumed */
    set(IN_LOOP, true);
    play_pump(true);
    CHECK(play_playing());
    set(GAME_OVER, true); /* the loop runs on while the initials are typed */
    play_pump(true);
    CHECK(play_in_game());
    CHECK(play_playing());
    set(IN_LOOP, false);
    play_pump(true);
    CHECK(!play_in_game());
    CHECK(!play_playing());

    set(GAME_OVER, false);
    run(START_NEW_GAME);
    set(IN_LOOP, true);
    play_pump(true);
    run(RESET_GAME); /* not suspended: a game's own reset */
    play_pump(true);
    CHECK(play_in_game());
    set(IN_LOOP, false);
    set(SUSPENDED, true);
    play_pump(true);
    run(RESET_GAME); /* thrown away from the pause menu */
    play_pump(true);
    CHECK(!play_in_game());
}

/* Each game goes into a file of its own, with its play spans: a pause
   leaves a gap, and a game that starts ends the last. */
TEST(play_records_each_game_with_its_pauses) {
    BEGIN_TEST();
    CHECK(record_each_game());
    CHECK(play_init("loony-labyrinth", CODE, TOC));
    replay_clock(1000000);
    play_pump(true); /* the menus: nothing written */
    char names_got[4][256];
    CHECK_EQ(recordings(names_got, 4), 0);

    run(START_NEW_GAME);
    set(IN_LOOP, true);
    play_pump(true);
    replay_clock(2000000);
    set(IN_LOOP, false);
    set(SUSPENDED, true);
    play_pump(true);
    replay_clock(5000000);
    set(SUSPENDED, false);
    set(IN_LOOP, true);
    play_pump(true);
    replay_clock(6000000);
    sleep(1); /* the next game's file is named for another second */
    run(START_NEW_GAME); /* from the game, without game over */
    play_pump(true);
    replay_clock(7000000);
    replay_finish();

    CHECK_EQ(recordings(names_got, 4), 2);
    const replay_span *spans;
    CHECK_EQ(spans_of(names_got[0], &spans), 2);
    CHECK_EQ(spans[0].on_us, 1000000);
    CHECK_EQ(spans[0].off_us, 2000000);
    CHECK_EQ(spans[1].on_us, 5000000);
    CHECK_EQ(spans[1].off_us, 6000000);
    CHECK_EQ(spans_of(names_got[1], &spans), 1);
    CHECK_EQ(spans[0].on_us, 6000000);
    CHECK(spans[0].off_us == UINT64_MAX);
}

/* Code that isn't 3.0.1's: games can't be told apart, so recording each
   game stops at once rather than buffer the whole run for no file. */
TEST(play_off_for_other_code_stops_recording_each_game) {
    BEGIN_TEST();
    CHECK(!play_init("monster-fair", CODE, TOC));
    CHECK(record_each_game());
    CHECK(replay_recording());
    gm_w32(CODE + 0x15538, 0); /* another build */
    CHECK(!play_init("loony-labyrinth", CODE, TOC));
    CHECK(!replay_recording());
    run(START_NEW_GAME);
    set(IN_LOOP, true);
    play_pump(true);
    CHECK(!play_in_game());
    replay_finish();
    char names_got[1][256];
    CHECK_EQ(recordings(names_got, 1), 0);
}
