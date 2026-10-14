#include "test.h"

#include <CommonCrypto/CommonDigest.h>
#include <SDL3/SDL_scancode.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "asm.h"
#include "clip.h"
#include "events.h"
#include "harness.h"
#include "memmgr.h"
#include "misc.h"
#include "replay.h"
#include "sound.h"
#include "util.h"

static const char *const names[] = {
    "TickCount", "Microseconds", "Delay", "GetDateTime", "NewSndCallBackUPP", "SndNewChannel",
    "SndDoCommand", "SndChannelStatus", "ICLaunchURL",
};

static const uint8_t exe[] = "a game's program";
static char dir[1024], file[1200], data[1200];

#define CB    (GUEST_IMAGE_BASE + 0x200)
#define TV_CB (GUEST_IMAGE_BASE + 0x8100)
#define GOT   (GUEST_IMAGE_BASE + 0x8200)

/* A fresh machine; a sound callback that stores cmd->param2 at GOT and
   counts its calls at GOT + 4. */
static void setup(void) {
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    misc_init();
    misc_register();
    sound_init();
    sound_register();
    events_init();
    uint32_t code[] = {
        ppc_lwz(5, 4, 4),  ppc_lis(6, GOT >> 16), ppc_ori(6, 6, GOT & 0xFFFF), ppc_stw(5, 0, 6),
        ppc_lwz(7, 4, 6),  ppc_addi(7, 7, 1),     ppc_stw(7, 4, 6),            PPC_BLR,
    };
    put_words(CB, code, 8);
    gm_w32(TV_CB, CB);
    gm_w32(TV_CB + 4, 0);
    gm_w32(GOT, 0);
    gm_w32(GOT + 4, 0);
}

/* The variables a recording or a replay sets or clears, as they were before
   the test. */
static const char *const env_names[] = {
    "LOONY_RECORD", "LOONY_REPLAY", "LOONY_GAME", "LOONY_DATA_DIR", "LOONY_AUTO_ALERTS",
    "LOONY_FIXED_CLOCK",
};
static char *env_saved[sizeof env_names / sizeof env_names[0]];

static int begin_test(void) {
    for (size_t i = 0; i < sizeof env_names / sizeof env_names[0]; i++) {
        const char *v = getenv(env_names[i]);
        env_saved[i] = v ? strdup(v) : NULL;
    }
    test_tmp_dir(dir, sizeof dir);
    snprintf(file, sizeof file, "%s/run.lwreplay", dir);
    snprintf(data, sizeof data, "%s/data", dir);
    return 0;
}

static void end_test(int *unused) {
    (void)unused;
    replay_reset();
    events_set_poll(NULL);
    events_set_modal(NULL);
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

/* First in every test: however the test ends, the recorder is reset, the
   environment put back and the folder removed, for the tests that run after
   it in the same process (-j 1). */
#define BEGIN_TEST() __attribute__((cleanup(end_test))) int test_guard_ = begin_test()

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
    BEGIN_TEST();
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
}

/* Marks (Cmd-S while recording) are found before the replay reaches them,
   at the ticks they were made, and a replay takes each where it was made. A
   replay marks nothing itself. */
TEST(replay_marks_are_found_before_they_are_played) {
    BEGIN_TEST();
    CHECK_EQ(replay_mark(), 0); /* nothing recorded */
    CHECK(record(NULL));
    replay_clock(1000000); /* 1 s: tick 60 */
    CHECK_EQ(replay_mark(), 1);
    replay_clock(2500000); /* tick 150 */
    replay_step();
    CHECK_EQ(replay_mark(), 2);
    replay_step();
    replay_finish();

    CHECK(play_back());
    const uint32_t *ticks;
    CHECK_EQ(replay_marks(&ticks), 2);
    CHECK_EQ(ticks[0], 60);
    CHECK_EQ(ticks[1], 150);
    CHECK_EQ(replay_mark(), 0);
    CHECK(!replay_take_mark());
    CHECK_EQ(replay_clock(0), 1000000);
    CHECK(replay_take_mark());
    CHECK(!replay_take_mark());
    CHECK_EQ(replay_clock(0), 2500000);
    replay_step();
    CHECK(replay_take_mark());
}

/* LOONY_CLIP=marks, in a child (clip_open turns clips on for good): the
   clips of the replay's marks, and their files. */
static void child_clip_marks(void *unused) {
    (void)unused;
    char err[256], play_data[1300];
    snprintf(play_data, sizeof play_data, "%s/play", dir);
    replay_reset();
    unsetenv("LOONY_RECORD");
    setenv("LOONY_REPLAY", file, 1);
    setenv("LOONY_DATA_DIR", play_data, 1);
    setenv("LOONY_CLIP", "marks", 1);
    setenv("LOONY_CLIP_BEFORE", "1", 1);
    setenv("LOONY_CLIP_AFTER", "0.5", 1);
    if (!replay_open(err, sizeof err) || !clip_open(err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        _exit(1);
    }
    _exit(0);
}

TEST(replay_marks_become_clips_named_after_the_replay) {
    BEGIN_TEST();
    CHECK(record(NULL));
    replay_clock(1000000); /* tick 60 */
    replay_mark();
    replay_clock(2500000); /* tick 150: its clip meets the first's */
    replay_mark();
    replay_clock(6666667); /* tick 399 */
    replay_mark();
    replay_finish();
    char out[8192], want[2048];
    CHECK_EQ(test_run_child(child_clip_marks, NULL, out, sizeof out), 0);
    CHECK_CONTAINS(out, "clip: mark 1 at tick 60 (0:01)");
    CHECK_CONTAINS(out, "clip: mark 2 at tick 150 (0:02.50)");
    CHECK_CONTAINS(out, "clip: mark 3 at tick 399 (0:06.65)");
    snprintf(want, sizeof want, "clip: 0:00-0:03 (marks 1 to 2) to %s/run-clip-1.mp4", dir);
    CHECK_CONTAINS(out, want);
    snprintf(want, sizeof want, "clip: 0:05.65-0:07.15 (mark 3) to %s/run-clip-2.mp4", dir);
    CHECK_CONTAINS(out, want);

    CHECK(record(NULL)); /* none */
    replay_finish();
    CHECK_EQ(test_run_child(child_clip_marks, NULL, out, sizeof out), 1);
    CHECK_CONTAINS(out, "has no marks (Cmd-S marks one while recording)");
}

TEST(replay_refuses_another_game_or_program) {
    BEGIN_TEST();
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
}

/* The clock moves in whole ms while recording, and a run of TickCount polls
   on one tick sleeps until the next; a replay returns the same values. */
TEST(replay_clock_values_come_back) {
    BEGIN_TEST();
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
}

/* The date is the recording's start plus the game's clock (Loony Labyrinth
   stamps a high score with it), so a replay a second later sees the same. */
TEST(replay_dates_come_back) {
    BEGIN_TEST();
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
    BEGIN_TEST();
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
    BEGIN_TEST();
    setup();
    CHECK(record(NULL));
    uint32_t chan = new_channel();
    queue_sound(chan);
    sound_pump();
    bool busy_before = busy(chan);
    sleep_s(0.08);
    misc_seconds(); /* the mixer renders up to the clock the game last read */
    sound_pump();
    uint32_t got = gm_r32(GOT), calls = gm_r32(GOT + 4);
    bool busy_after = busy(chan);
    replay_finish();
    CHECK(busy_before);
    CHECK_EQ(got, 0xCAFE);
    CHECK_EQ(calls, 1);
    CHECK(!busy_after);

    /* The replay's mixer gets a callback (0xBEEF), due at once, then 3 s of
       sound, so it stays busy. Only the file's answers count. */
    setup();
    CHECK(play_back());
    chan = new_channel();
    uint32_t h = scratch(22 + 30);
    gm_w32(h + 4, 30);
    gm_w32(h + 8, 10u << 16); /* 30 samples at 10 Hz */
    memset(gm_ptr(h + 22, 30), 0x80, 30);
    call_import("SndDoCommand", 3, chan, cmd(SND_CALLBACK_CMD, 0, 0xBEEFu), 0u);
    call_import("SndDoCommand", 3, chan, cmd(SND_BUFFER_CMD, 0, h), 0u);
    sound_pump();
    CHECK_EQ(gm_r32(GOT), 0);
    CHECK(busy(chan));
    misc_seconds();
    sound_pump();
    CHECK_EQ(gm_r32(GOT), 0xCAFE);
    CHECK_EQ(gm_r32(GOT + 4), 1); /* the mixer's own callback never came */
    CHECK(!busy(chan));
}

static void child_desync(void *unused) {
    (void)unused;
    CHECK(record(NULL));
    for (int i = 0; i < REPLAY_FRAME_EVERY; i++) {
        replay_step();
        if (replay_frame_due())
            replay_frame(0x1234);
    }
    replay_finish();
    CHECK(play_back());
    for (int i = 0; i < REPLAY_FRAME_EVERY; i++) {
        replay_step();
        if (replay_frame_due())
            replay_frame(0x1235);
    }
}

TEST(replay_reports_a_desync_at_the_first_frame_that_differs) {
    BEGIN_TEST();
    char out[4096];
    int status = test_run_child(child_desync, NULL, out, sizeof out);
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
    BEGIN_TEST();
    char out[4096];
    int status = test_run_child(child_input_not_used, NULL, out, sizeof out);
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
    BEGIN_TEST();
    char out[4096];
    int status = test_run_child(child_past_the_end, NULL, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "replay: exiting where the recording stopped");
    CHECK_CONTAINS(out, "replay: reached the end of the recording at step 3");
    CHECK(!strstr(out, "still running"));
}

/* A replay never writes over preferences already in its save folder: they
   could be the player's own, with their license and high scores. */
TEST(replay_refuses_a_folder_with_preferences) {
    BEGIN_TEST();
    CHECK(record(NULL));
    replay_finish();
    char play_dir[1300], prefs[1400], err[256];
    snprintf(play_dir, sizeof play_dir, "%s/play", dir);
    snprintf(prefs, sizeof prefs, "%s/prefs.plist", play_dir);
    CHECK(make_dirs(play_dir));
    FILE *f = fopen(prefs, "w");
    fputs("mine", f);
    fclose(f);
    CHECK(!play("loony-labyrinth", exe, sizeof exe, err, sizeof err));
    CHECK_CONTAINS(err, "already has preferences: replay into a new folder");
    size_t len = 0;
    char *text = (char *)read_file(prefs, &len);
    bool kept = text && len == 4 && memcmp(text, "mine", 4) == 0;
    free(text);
    CHECK(kept);
    CHECK(!replay_playing());
}

static void child_bad_channel(void *unused) {
    (void)unused;
    setup();
    CHECK(record(NULL));
    replay_step();
    replay_log(REPLAY_SOUND_CALLBACK, (uint32_t[3]){100000000u, 0, 0});
    replay_finish();
    setup();
    CHECK(play_back());
    replay_step();
    sound_pump();
    fprintf(stderr, "still running\n");
}

/* A damaged or crafted file can't make a callback on a channel that isn't. */
TEST(replay_a_sound_callback_on_no_channel_is_a_desync) {
    BEGIN_TEST();
    char out[4096];
    int status = test_run_child(child_bad_channel, NULL, out, sizeof out);
    CHECK_EQ(status, REPLAY_DESYNC_STATUS);
    CHECK_CONTAINS(out, "replay: desync at step 1 (tick 0): a sound callback for channel "
                        "100000000, of 16");
    CHECK(!strstr(out, "still running"));
}

/* A recording that stops without finishing (a crash): 30 presents, so one
   frame check and a flush, then 5 steps never flushed. */
static void child_crashing_recording(void *unused) {
    (void)unused;
    CHECK(record(NULL));
    for (int i = 0; i < REPLAY_FRAME_EVERY; i++) {
        replay_step();
        replay_log(REPLAY_KEY, (uint32_t[3]){SDL_SCANCODE_Z, 1, 0});
        if (replay_frame_due())
            replay_frame(0x1234);
    }
    for (int i = 0; i < 5; i++)
        replay_step();
    _exit(0);
}

static void child_replay_crashed(void *unused) {
    (void)unused;
    CHECK(play_back());
    uint32_t v[3];
    for (int i = 0; i < REPLAY_FRAME_EVERY; i++) {
        replay_step();
        CHECK(replay_take(REPLAY_KEY, v));
        if (replay_frame_due())
            replay_frame(0x1234);
    }
    fprintf(stderr, "checked to the flush\n");
    for (int i = 0; i < 5; i++)
        replay_step();
    fprintf(stderr, "still running\n");
}

/* The recording reaches the file at every frame check, so a recording that
   crashed replays up to its last one. */
TEST(replay_a_recording_that_crashed_plays_to_its_last_frame_check) {
    BEGIN_TEST();
    char out[4096];
    CHECK_EQ(test_run_child(child_crashing_recording, NULL, out, sizeof out), 0);
    int status = test_run_child(child_replay_crashed, NULL, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "replay: the file ends without an end record (after step 30)");
    CHECK_CONTAINS(out, "checked to the flush");
    CHECK_CONTAINS(out, "replay: exiting where the recording stopped");
    CHECK_CONTAINS(out, "replay: reached where the recording was cut short at step 30");
    CHECK(!strstr(out, "still running"));
}

static void child_record_then_crash(void *unused) {
    (void)unused;
    CHECK(record(NULL));
    _exit(0);
}

/* The header reaches the file at once: a recording that crashes before its
   first frame check still replays (to nothing). */
TEST(replay_a_recording_that_crashed_at_once_has_its_header) {
    BEGIN_TEST();
    char out[4096];
    CHECK_EQ(test_run_child(child_record_then_crash, NULL, out, sizeof out), 0);
    CHECK(play_back());
}

static char posted[REPLAY_MAX_TEXT + 200];
static void sink_key(uint32_t vkey, uint8_t chr, uint32_t modifiers) {
    (void)vkey, (void)chr, (void)modifiers;
}
static void sink_mouse(int x, int y, bool down) { (void)x, (void)y, (void)down; }
static void sink_text(const char *utf8) { snprintf(posted, sizeof posted, "%s", utf8); }

/* Text too long for a record is cut before a character, and the game gets
   the cut text while recording too, as it will on the replay. */
TEST(replay_long_text_is_cut_the_same_both_ways) {
    BEGIN_TEST();
    setup();
    static const ev_modal_sink sink = {sink_key, sink_mouse, sink_text};
    events_set_modal(&sink);
    char text[REPLAY_MAX_TEXT + 100];
    memset(text, 'a', REPLAY_MAX_TEXT - 2);
    strcpy(text + REPLAY_MAX_TEXT - 2, "\xC3\xA9 and more"); /* an e-acute across the cut */
    CHECK(record(NULL));
    replay_step();
    events_host_text(text);
    replay_finish();
    CHECK_EQ(strlen(posted), REPLAY_MAX_TEXT - 2);
    CHECK(memcmp(posted, text, REPLAY_MAX_TEXT - 2) == 0);

    CHECK(play_back());
    replay_step();
    char got[REPLAY_MAX_TEXT];
    CHECK(replay_take_text(got, sizeof got));
    CHECK_STR(got, posted);
}

/* A replay file of these bytes, compressed. */
static void write_replay(const void *bytes, size_t len) {
    gzFile f = gzopen(file, "wb");
    gzwrite(f, bytes, (unsigned)len);
    gzclose(f);
}

/* A format 2 header for "x" with this start and time zone (varints). */
static size_t header_with(uint8_t *h, const char *start, const char *gmtoff) {
    memcpy(h, "LWREPLAY\x02\x01x", 11);
    memset(h + 11, 0, 32); /* the SHA-256 */
    size_t n = 43;
    h[n++] = 0; /* no flags */
    memcpy(h + n, start, strlen(start));
    n += strlen(start);
    memcpy(h + n, gmtoff, strlen(gmtoff));
    n += strlen(gmtoff);
    h[n++] = 0; /* no preferences */
    return n;
}

TEST(replay_refuses_a_damaged_header) {
    BEGIN_TEST();
    char err[256];
    unsetenv("LOONY_RECORD");
    setenv("LOONY_REPLAY", file, 1);
    write_replay("LWREPLAY", 8); /* no version */
    CHECK(!replay_open(err, sizeof err));
    CHECK_STR(err, "the replay's header is damaged");
    write_replay("LWREPLAY\x09", 9);
    CHECK(!replay_open(err, sizeof err));
    CHECK_STR(err, "replay format 9 (this program reads 1 to 3)");
    uint8_t h[64];
    static const char *const damaged[][2] = {
        {"\x80\x80\x80\x80\x80\x20", "\x01"}, /* a start of 2^40 s: 36812 AD */
        {"\x01", "\x90\xBF\x05"},             /* 90000 s east of UTC */
        {"\x01", "\x90\x9C\x80\x80\x10"},     /* 2^32 + 3600 */
    };
    for (size_t i = 0; i < sizeof damaged / sizeof damaged[0]; i++) {
        write_replay(h, header_with(h, damaged[i][0], damaged[i][1]));
        CHECK(!replay_open(err, sizeof err));
        CHECK_STR(err, "the replay's header is damaged");
    }
    write_replay(h, header_with(h, "\x01", "\x90\x1C")); /* 1 s, an hour east */
    CHECK(replay_open(err, sizeof err));
}

/* Format 1, from before dates were recorded, has no start or time zone: it
   plays, and its dates are the host's. */
TEST(replay_plays_the_first_format_with_the_hosts_dates) {
    BEGIN_TEST();
    uint8_t h[128];
    memcpy(h, "LWREPLAY\x01\x0floony-labyrinth", 25); /* format 1, the game's id */
    CC_SHA256(exe, sizeof exe, h + 25);
    size_t n = 25 + CC_SHA256_DIGEST_LENGTH;
    h[n++] = 0;           /* no flags */
    h[n++] = 0;           /* no preferences */
    h[n++] = 5;           /* at step 5, */
    h[n++] = REPLAY_END;  /* the end */
    write_replay(h, n);
    char err[256] = "";
    bool ok = play("loony-labyrinth", exe, sizeof exe, err, sizeof err);
    if (!ok)
        fprintf(stderr, "%s\n", err);
    CHECK(ok);
    CHECK(replay_playing());
    int64_t start;
    int32_t gmtoff;
    bool dst;
    CHECK(!replay_calendar(&start, &gmtoff, &dst));
}

static int urls_opened;
static bool count_open(const char *url) {
    (void)url;
    urls_opened++;
    return true;
}

/* A replay of someone clicking a link (Alert 901's Buy Now) opens nothing. */
TEST(replay_opens_no_web_pages) {
    BEGIN_TEST();
    misc_set_url_opener(count_open);
    setup();
    CHECK(record(NULL));
    replay_finish();
    CHECK(play_back());
    const char *u = "http://www.LittleWingPinball.com/";
    uint32_t d = scratch(64), sp = scratch(4), ep = scratch(4);
    memcpy(gm_ptr(d, 64), u, strlen(u));
    gm_w32(sp, 0);
    gm_w32(ep, (uint32_t)strlen(u));
    urls_opened = 0;
    uint32_t r = call_import("ICLaunchURL", 6, MISC_IC_INSTANCE, scratch(2), d,
                             (uint32_t)strlen(u), sp, ep);
    CHECK_EQ(r, 0);
    CHECK_EQ(urls_opened, 0);
}
