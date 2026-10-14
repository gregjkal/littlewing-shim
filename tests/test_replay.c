#include "test.h"

#include <SDL3/SDL_scancode.h>
#include <stdlib.h>
#include <time.h>

#include "asm.h"
#include "events.h"
#include "harness.h"
#include "memmgr.h"
#include "misc.h"
#include "replay.h"
#include "sound.h"
#include "util.h"

static const char *const names[] = {
    "TickCount", "Microseconds", "Delay", "GetDateTime", "NewSndCallBackUPP", "SndNewChannel",
    "SndDoCommand", "SndChannelStatus",
};

static const uint8_t exe[] = "a game's program";
static char dir[1024], file[1200], data[1200];

#define CB    (GUEST_IMAGE_BASE + 0x200)
#define TV_CB (GUEST_IMAGE_BASE + 0x8100)
#define GOT   (GUEST_IMAGE_BASE + 0x8200)

/* A fresh machine; a sound callback that stores cmd->param2 at GOT. */
static void setup(void) {
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    misc_init();
    misc_register();
    sound_init();
    sound_register();
    events_init();
    uint32_t code[] = {
        ppc_lwz(5, 4, 4), ppc_lis(6, GOT >> 16), ppc_ori(6, 6, GOT & 0xFFFF), ppc_stw(5, 0, 6),
        PPC_BLR,
    };
    put_words(CB, code, 5);
    gm_w32(TV_CB, CB);
    gm_w32(TV_CB + 4, 0);
    gm_w32(GOT, 0);
}

static void make_paths(void) {
    test_tmp_dir(dir, sizeof dir);
    snprintf(file, sizeof file, "%s/run.lwreplay", dir);
    snprintf(data, sizeof data, "%s/data", dir);
}

static bool record(const char *data_dir) {
    char err[256];
    replay_reset();
    unsetenv("LOONY_REPLAY");
    unsetenv("LOONY_FIXED_CLOCK");
    setenv("LOONY_RECORD", file, 1);
    return replay_start("loony-labyrinth", exe, sizeof exe, data_dir, err, sizeof err);
}

/* Starts playing the file back, from a new save folder. */
static bool play(const char *game, const uint8_t *program, size_t len, char *err, size_t errlen) {
    replay_reset();
    unsetenv("LOONY_RECORD");
    unsetenv("LOONY_GAME");
    setenv("LOONY_REPLAY", file, 1);
    char play_data[1300];
    snprintf(play_data, sizeof play_data, "%s/play", dir);
    setenv("LOONY_DATA_DIR", play_data, 1);
    return replay_open(err, errlen) && replay_start(game, program, len, play_data, err, errlen);
}

static bool play_back(void) {
    char err[256];
    return play("loony-labyrinth", exe, sizeof exe, err, sizeof err);
}

static void sleep_s(double s) {
    struct timespec ts = {0, (long)(s * 1e9)};
    nanosleep(&ts, NULL);
}

TEST(replay_file_round_trips_every_kind_and_the_starting_state) {
    make_paths();
    CHECK(make_dirs(data));
    char prefs[1300];
    snprintf(prefs, sizeof prefs, "%s/prefs.plist", data);
    FILE *f = fopen(prefs, "w");
    fputs("<plist>high scores</plist>", f);
    fclose(f);
    setenv("LOONY_AUTO_ALERTS", "1", 1);
    CHECK(record(data));
    replay_step();
    replay_log(REPLAY_KEY, (uint32_t[3]){SDL_SCANCODE_Z, 1, 0});
    replay_log_text("me@example.com");
    replay_step();
    replay_step();
    replay_log(REPLAY_SOUND_CALLBACK, (uint32_t[3]){3, 0, 0xDEADBEEFu});
    replay_log(REPLAY_SOUND_BUSY, NULL);
    replay_step();
    replay_log(REPLAY_MOUSE, (uint32_t[3]){460, 270, 1});
    replay_log(REPLAY_FOCUS, (uint32_t[1]){0});
    replay_log(REPLAY_QUIT, NULL);
    replay_finish();

    unsetenv("LOONY_AUTO_ALERTS");
    CHECK(play_back());
    CHECK_STR(getenv("LOONY_GAME"), "loony-labyrinth");
    CHECK_STR(getenv("LOONY_AUTO_ALERTS"), "1");
    char played[1400];
    snprintf(played, sizeof played, "%s/play/prefs.plist", dir);
    size_t len = 0;
    char *text = (char *)read_file(played, &len);
    CHECK(text != NULL);
    CHECK_EQ(len, 26);
    CHECK(memcmp(text, "<plist>high scores</plist>", 26) == 0);
    free(text);

    uint32_t v[3];
    char s[64];
    CHECK(!replay_take(REPLAY_KEY, v)); /* step 0: nothing yet */
    replay_step();
    CHECK(!replay_take(REPLAY_MOUSE, v)); /* the next record is a key */
    CHECK(replay_take(REPLAY_KEY, v));
    CHECK_EQ(v[0], SDL_SCANCODE_Z);
    CHECK_EQ(v[1], 1);
    CHECK(replay_take_text(s, sizeof s));
    CHECK_STR(s, "me@example.com");
    replay_step();
    CHECK(!replay_take(REPLAY_SOUND_CALLBACK, v)); /* that one is a step later */
    replay_step();
    CHECK(replay_take(REPLAY_SOUND_CALLBACK, v));
    CHECK_EQ(v[0], 3);
    CHECK_EQ(v[2], 0xDEADBEEFu);
    CHECK(replay_take(REPLAY_SOUND_BUSY, NULL));
    replay_step();
    CHECK(replay_take(REPLAY_MOUSE, v));
    CHECK_EQ(v[0], 460);
    CHECK_EQ(v[1], 270);
    CHECK(replay_take(REPLAY_FOCUS, v));
    CHECK_EQ(v[0], 0);
    CHECK(replay_take(REPLAY_QUIT, NULL));
    test_remove_tree(dir);
}

TEST(replay_refuses_another_game_or_program) {
    make_paths();
    CHECK(record(NULL));
    replay_finish();
    char err[256];
    CHECK(!play("crystal-caliburn", exe, sizeof exe, err, sizeof err));
    CHECK_CONTAINS(err, "the replay is of loony-labyrinth, not crystal-caliburn");
    static const uint8_t other[] = "another version";
    CHECK(!play("loony-labyrinth", other, sizeof other, err, sizeof err));
    CHECK_CONTAINS(err, "another version of loony-labyrinth's program");
    setenv("LOONY_REPLAY", "/nonexistent/run.lwreplay", 1);
    CHECK(!replay_open(err, sizeof err));
    CHECK_CONTAINS(err, "can't read the replay /nonexistent/run.lwreplay");
    test_remove_tree(dir);
}

/* The clock moves in whole ms while recording, and a run of TickCount polls
   on one tick sleeps until the next; a replay returns the same values. */
TEST(replay_clock_values_come_back) {
    make_paths();
    setup();
    CHECK(record(NULL));
    uint32_t got[80], micro[2];
    uint32_t us = scratch(8);
    for (int i = 0; i < 70; i++)
        got[i] = call_import("TickCount", 0);
    sleep_s(0.003);
    call_import("Microseconds", 1, us);
    micro[0] = gm_r32(us + 4);
    call_import("Delay", 2, 2u, 0u);
    call_import("Microseconds", 1, us);
    micro[1] = gm_r32(us + 4);
    replay_finish();
    CHECK(got[69] > got[0]); /* the 50th poll on one tick waited for the next */
    CHECK_EQ(micro[0] % 1000, 0);
    CHECK(micro[1] - micro[0] >= 16000); /* to the second tick boundary from here */

    setup();
    CHECK(play_back());
    for (int i = 0; i < 70; i++)
        CHECK_EQ(call_import("TickCount", 0), got[i]);
    call_import("Microseconds", 1, us);
    CHECK_EQ(gm_r32(us + 4), micro[0]);
    call_import("Delay", 2, 2u, 0u);
    call_import("Microseconds", 1, us);
    CHECK_EQ(gm_r32(us + 4), micro[1]);
    test_remove_tree(dir);
}

/* The date is the recording's start plus the game's clock (Loony Labyrinth
   stamps a high score with it), so a replay a second later sees the same. */
TEST(replay_dates_come_back) {
    make_paths();
    setup();
    CHECK(record(NULL));
    uint32_t at = scratch(4);
    sleep_s(0.3);
    call_import("GetDateTime", 1, at);
    uint32_t recorded = gm_r32(at);
    replay_finish();
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    int64_t host = (int64_t)now + lt.tm_gmtoff + 2082844800; /* Mac epoch */
    CHECK(recorded >= host - 2 && recorded <= host);

    sleep_s(1.2);
    setup();
    CHECK(play_back());
    call_import("GetDateTime", 1, at);
    CHECK_EQ(gm_r32(at), recorded);
    test_remove_tree(dir);
}

/* The window: Z and a focus loss at the second poll, Z up at the third. */
static int polls;

static void host_poll(void) {
    polls++;
    if (polls == 2) {
        events_host_key(SDL_SCANCODE_Z, true, false);
        events_host_activation(false);
    } else if (polls == 3) {
        events_host_key(SDL_SCANCODE_Z, false, false);
    }
}

/* Host input is logged where the pump polled for it and posted there again;
   on a replay the host's own input is dropped. */
TEST(replay_input_is_posted_where_it_was_polled) {
    make_paths();
    setup();
    events_set_poll(host_poll);
    CHECK(record(NULL));
    int queued[3];
    for (int i = 0; i < 3; i++) {
        events_pump();
        queued[i] = events_queued();
    }
    replay_finish();
    CHECK_EQ(queued[0], 1); /* kEventAppActivated, from events_init */
    CHECK_EQ(queued[1], 3);
    CHECK_EQ(queued[2], 4);

    setup();
    polls = 0;
    events_set_poll(host_poll);
    CHECK(play_back());
    for (int i = 0; i < 3; i++) {
        events_pump(); /* the window's input is dropped; the file's is posted */
        CHECK_EQ(events_queued(), queued[i]);
    }
    test_remove_tree(dir);
}

static uint32_t cmd(uint16_t c, uint16_t p1, uint32_t p2) {
    uint32_t a = scratch(8);
    gm_w16(a, c);
    gm_w16(a + 2, p1);
    gm_w32(a + 4, p2);
    return a;
}

static uint32_t new_channel(void) {
    uint32_t pp = scratch(4);
    uint32_t upp = call_import("NewSndCallBackUPP", 1, TV_CB);
    if (call_import("SndNewChannel", 4, pp, 5u, 0x84u, upp) != 0)
        fatal("SndNewChannel failed");
    return gm_r32(pp);
}

static bool busy(uint32_t chan) {
    uint32_t st = scratch(24);
    call_import("SndChannelStatus", 3, chan, 24u, st);
    return gm_r8(st + 12) != 0;
}

/* 50 ms of silence, then a callback. */
static void queue_sound(uint32_t chan) {
    uint32_t h = scratch(22 + 30);
    gm_w32(h + 4, 30);
    gm_w32(h + 8, 600u << 16);
    memset(gm_ptr(h + 22, 30), 0x80, 30);
    call_import("SndDoCommand", 3, chan, cmd(SND_BUFFER_CMD, 0, h), 0u);
    call_import("SndDoCommand", 3, chan, cmd(SND_CALLBACK_CMD, 0, 0xCAFEu), 0u);
}

/* A replay delivers sound callbacks, and answers SndChannelStatus, as the
   recording did, whatever its own mixer says. */
TEST(replay_sound_callbacks_and_status_come_from_the_file) {
    make_paths();
    setup();
    CHECK(record(NULL));
    uint32_t chan = new_channel();
    queue_sound(chan);
    sound_pump();
    bool busy_before = busy(chan);
    sleep_s(0.08);
    misc_seconds(); /* the mixer renders up to the clock the game last read */
    sound_pump();
    uint32_t got = gm_r32(GOT);
    bool busy_after = busy(chan);
    replay_finish();
    CHECK(busy_before);
    CHECK_EQ(got, 0xCAFE);
    CHECK(!busy_after);

    setup();
    CHECK(play_back());
    chan = new_channel();
    queue_sound(chan);
    sound_pump();
    CHECK(busy(chan));
    misc_seconds();
    sound_pump();
    CHECK_EQ(gm_r32(GOT), 0xCAFE);
    CHECK(!busy(chan));
    test_remove_tree(dir);
}

static void child_desync(void *unused) {
    (void)unused;
    CHECK(record(NULL));
    for (int i = 0; i < REPLAY_FRAME_EVERY; i++) {
        replay_step();
        replay_frame(0x1234);
    }
    replay_finish();
    CHECK(play_back());
    for (int i = 0; i < REPLAY_FRAME_EVERY; i++) {
        replay_step();
        replay_frame(0x1235);
    }
}

TEST(replay_reports_a_desync_at_the_first_frame_that_differs) {
    make_paths();
    char out[4096];
    int status = test_run_child(child_desync, NULL, out, sizeof out);
    test_remove_tree(dir);
    CHECK_EQ(status, REPLAY_DESYNC_STATUS);
    CHECK_CONTAINS(out, "replay: desync at step 30 (tick 0): present 30: the screen's hash is "
                        "00001235, not 00001234 as recorded");
}

static void child_input_not_used(void *unused) {
    (void)unused;
    CHECK(record(NULL));
    replay_step();
    replay_log(REPLAY_KEY, (uint32_t[3]){SDL_SCANCODE_Z, 1, 0});
    replay_step();
    replay_finish();
    CHECK(play_back());
    replay_step();
    replay_step(); /* the key wasn't taken at step 1 */
}

TEST(replay_reports_a_record_left_unused) {
    make_paths();
    char out[4096];
    int status = test_run_child(child_input_not_used, NULL, out, sizeof out);
    test_remove_tree(dir);
    CHECK_EQ(status, REPLAY_DESYNC_STATUS);
    CHECK_CONTAINS(out, "replay: desync at step 2 (tick 0): the key record of step 1 wasn't used");
}

static void child_past_the_end(void *unused) {
    (void)unused;
    CHECK(record(NULL));
    for (int i = 0; i < 3; i++)
        replay_step();
    replay_finish();
    CHECK(play_back());
    for (int i = 0; i < 4; i++)
        replay_step();
    fprintf(stderr, "still running\n");
}

TEST(replay_exits_where_the_recording_stopped) {
    make_paths();
    char out[4096];
    int status = test_run_child(child_past_the_end, NULL, out, sizeof out);
    test_remove_tree(dir);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "replay: exiting where the recording stopped");
    CHECK_CONTAINS(out, "replay: reached the end of the recording at step 3");
    CHECK(!strstr(out, "still running"));
}
