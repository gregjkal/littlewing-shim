#include "test.h"

#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <sys/time.h>
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
#include "plist.h"
#include "replay.h"
#include "sound.h"
#include "util.h"
#include "watch.h"

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
    "LOONY_FIXED_CLOCK", "LOONY_REPLAY_LICENSE", "HOME",
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
    snprintf(want, sizeof want, "clip: 0:00-0:03 (marks 1 to 2) to %s/run-0m00-0m03.mp4", dir);
    CHECK_CONTAINS(out, want);
    snprintf(want, sizeof want, "clip: 0:05.65-0:07.15 (mark 3) to %s/run-0m05.65-0m07.15.mp4", dir);
    CHECK_CONTAINS(out, want);

    CHECK(record(NULL)); /* none */
    replay_finish();
    CHECK_EQ(test_run_child(child_clip_marks, NULL, out, sizeof out), 1);
    CHECK_CONTAINS(out, "has no marks (Cmd-S marks one while recording)");
}

static void touch(const char *folder, const char *name) {
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", folder, name);
    FILE *f = fopen(path, "w");
    if (f)
        fclose(f);
}

static bool exists_in(const char *folder, const char *name) {
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", folder, name);
    return access(path, F_OK) == 0;
}

/* A recording in folder made by the test (not one of January's), other
   than except: its name, or "". */
static void new_recording(const char *folder, const char *except, char *name, size_t cap) {
    DIR *d = opendir(folder);
    struct dirent *e;
    name[0] = '\0';
    while (d && (e = readdir(d)))
        if (strncmp(e->d_name, "loony-labyrinth-2026-01", 23) != 0 &&
            strncmp(e->d_name, "loony-labyrinth-", 16) == 0 && strcmp(e->d_name, except) != 0)
            snprintf(name, cap, "%s", e->d_name);
    if (d)
        closedir(d);
}

/* The app records each game by itself, into a file of its own: at launch,
   the game's recordings beyond the last REPLAY_KEEP_GAMES go, but not a
   marked one or another game's. Nothing is written until a game starts;
   then its file holds what came before it too, and a mark renames it. */
TEST(replay_records_every_game_keeping_the_last_three_and_the_marked) {
    BEGIN_TEST();
    char rec_dir[1300];
    snprintf(rec_dir, sizeof rec_dir, "%s/recordings", dir);
    CHECK(make_dirs(rec_dir));
    static const char *const old[] = {
        "loony-labyrinth-2026-01-01-120000.lwreplay",
        "loony-labyrinth-2026-01-02-120000-marked.lwreplay",
        "loony-labyrinth-2026-01-03-120000.lwreplay",
        "loony-labyrinth-2026-01-04-120000.lwreplay",
        "loony-labyrinth-2026-01-05-120000.lwreplay",
        "crystal-caliburn-2026-01-01-120000.lwreplay",
        "notes.txt",
    };
    for (size_t i = 0; i < sizeof old / sizeof old[0]; i++)
        touch(rec_dir, old[i]);
    unsetenv("LOONY_RECORD");
    unsetenv("LOONY_FIXED_CLOCK");
    unsetenv("LOONY_REPLAY");
    replay_reset();
    replay_record_into(rec_dir, "loony-labyrinth", NULL, 0);
    CHECK(!exists_in(rec_dir, old[0]));
    CHECK(exists_in(rec_dir, old[1])); /* marked */
    for (size_t i = 2; i < sizeof old / sizeof old[0]; i++)
        CHECK(exists_in(rec_dir, old[i])); /* the last three, another game's, not a recording */

    char err[256], name[256], first[256];
    CHECK(replay_start("loony-labyrinth", exe, sizeof exe, NULL, err, sizeof err));
    CHECK(replay_recording());
    replay_step();
    CHECK_EQ(replay_mark(), 0); /* no game: nothing to mark */
    new_recording(rec_dir, "", name, sizeof name);
    CHECK_STR(name, "");

    replay_step();
    replay_log(REPLAY_KEY, (uint32_t[3]){40, 1, 0}); /* before the games: in both files */
    replay_game(true);
    replay_play(true);
    new_recording(rec_dir, "", name, sizeof name);
    CHECK(strlen(name) == strlen("loony-labyrinth-2026-10-15-101500.lwreplay"));
    replay_step();
    CHECK_EQ(replay_mark(), 1);
    char marked[300];
    snprintf(marked, sizeof marked, "%.*s-marked.lwreplay", (int)(strlen(name) - 9), name);
    CHECK(!exists_in(rec_dir, name));
    CHECK(exists_in(rec_dir, marked));
    CHECK_EQ(replay_mark(), 2); /* renamed once */
    replay_step();
    replay_play(false);
    replay_game(false);
    snprintf(first, sizeof first, "%s", marked);

    replay_step();
    replay_log(REPLAY_KEY, (uint32_t[3]){41, 1, 0}); /* between the games: in the second */
    replay_step();
    replay_game(true);
    replay_play(true);
    replay_step();
    replay_finish(); /* quitting ends the game */
    new_recording(rec_dir, first, name, sizeof name);
    CHECK(strstr(name, ".lwreplay") && !strstr(name, "-marked")); /* -2 in the same second */

    /* Each replays from launch with its own marks: the first has both. */
    snprintf(file, sizeof file, "%s/%s", rec_dir, first);
    CHECK(play_back());
    const uint32_t *ticks;
    CHECK_EQ(replay_marks(&ticks), 2);
    for (int i = 0; i < 2; i++) {
        replay_step();
        uint32_t v[3];
        CHECK(replay_take(REPLAY_KEY, v) == (i == 1));
        CHECK(replay_take_play() == (i == 1)); /* the game starts */
        CHECK(replay_take_play() == (i == 1)); /* play goes on */
    }
    snprintf(file, sizeof file, "%s/%s", rec_dir, name);
    CHECK(play_back());
    CHECK_EQ(replay_marks(&ticks), 0);
    for (int i = 0; i < 5; i++) {
        replay_step();
        uint32_t v[3];
        CHECK(replay_take(REPLAY_KEY, v) == (i == 1 || i == 4));
        CHECK(!replay_take_mark()); /* the first game's are its own */
        CHECK(!replay_take_play()); /* and so is its game */
    }
    replay_step();
    CHECK(replay_take_play() && replay_take_play());
}

static time_t local_time(int day, int h, int mi, int sec) {
    struct tm tm = {.tm_year = 2026 - 1900, .tm_mon = 0, .tm_mday = day, .tm_hour = h,
                    .tm_min = mi, .tm_sec = sec, .tm_isdst = -1};
    return mktime(&tm);
}

/* A recording of its own name that ended (was last written) at end. */
static void recording_ending(const char *folder, const char *name, time_t end) {
    char path[1400];
    touch(folder, name);
    snprintf(path, sizeof path, "%s/%s", folder, name);
    struct timeval tv[2] = {{end, 0}, {end, 0}};
    utimes(path, tv);
}

/* An older game is kept while a high score it set is in the table (it was
   recording when the score's time came), and goes once the score is gone. */
TEST(replay_keeps_the_games_whose_high_scores_are_in_the_table) {
    BEGIN_TEST();
    char rec_dir[1300];
    snprintf(rec_dir, sizeof rec_dir, "%s/recordings", dir);
    CHECK(make_dirs(rec_dir));
    static const char *const old[] = {
        "loony-labyrinth-2026-01-01-120000.lwreplay", /* its score is in the table */
        "loony-labyrinth-2026-01-02-120000.lwreplay", /* its score was evicted */
        "loony-labyrinth-2026-01-03-120000.lwreplay", /* a score a second after its end */
        "loony-labyrinth-2026-01-04-120000.lwreplay", /* a score long after its end */
        "loony-labyrinth-2026-01-05-120000.lwreplay",
        "loony-labyrinth-2026-01-06-120000.lwreplay",
        "loony-labyrinth-2026-01-07-120000.lwreplay",
    };
    for (int i = 0; i < 7; i++)
        recording_ending(rec_dir, old[i], local_time(i + 1, 12, 10, 0));
    time_t scores[] = {local_time(1, 12, 5, 0), local_time(3, 12, 10, 1), local_time(4, 13, 0, 0)};
    unsetenv("LOONY_RECORD");
    unsetenv("LOONY_FIXED_CLOCK");
    unsetenv("LOONY_REPLAY");
    replay_reset();
    replay_record_into(rec_dir, "loony-labyrinth", scores, 3);
    CHECK(exists_in(rec_dir, old[0]));
    CHECK(!exists_in(rec_dir, old[1]));
    CHECK(exists_in(rec_dir, old[2]));
    CHECK(!exists_in(rec_dir, old[3]));
    CHECK(exists_in(rec_dir, old[4])); /* the last three */
    CHECK(exists_in(rec_dir, old[5]));
    CHECK(exists_in(rec_dir, old[6]));

    /* The first game's score leaves the table: next time, so does the game. */
    replay_reset();
    replay_record_into(rec_dir, "loony-labyrinth", &scores[1], 1);
    CHECK(!exists_in(rec_dir, old[0]));
    CHECK(exists_in(rec_dir, old[2]));
}

/* An older game with clips saved from watching it (watch.h) is kept, like a
   marked one; its list of clips is never deleted. */
TEST(replay_keeps_the_games_with_clips_from_watching) {
    BEGIN_TEST();
    char rec_dir[1300];
    snprintf(rec_dir, sizeof rec_dir, "%s/recordings", dir);
    CHECK(make_dirs(rec_dir));
    static const char *const old[] = {
        "loony-labyrinth-2026-01-01-120000.lwreplay",
        "loony-labyrinth-2026-01-01-120000.clips",
        "loony-labyrinth-2026-01-02-120000.lwreplay",
        "loony-labyrinth-2026-01-03-120000-marked.lwreplay",
        "loony-labyrinth-2026-01-03-120000-marked.clips",
        "loony-labyrinth-2026-01-04-120000.clips", /* its game is gone */
        "loony-labyrinth-2026-01-05-120000.lwreplay",
        "loony-labyrinth-2026-01-06-120000.lwreplay",
    };
    for (size_t i = 0; i < sizeof old / sizeof old[0]; i++)
        touch(rec_dir, old[i]);
    unsetenv("LOONY_RECORD");
    unsetenv("LOONY_FIXED_CLOCK");
    unsetenv("LOONY_REPLAY");
    replay_reset();
    replay_record_into(rec_dir, "loony-labyrinth", NULL, 0);
    for (size_t i = 0; i < sizeof old / sizeof old[0]; i++)
        if (i != 2)
            CHECK(exists_in(rec_dir, old[i]));
    CHECK(!exists_in(rec_dir, old[2]));
}

/* The kept recording of the game that set a high score: the newest whose
   span (the time in its name to its last write) holds the score's time. */
TEST(replay_finds_the_recording_of_a_high_score) {
    BEGIN_TEST();
    char rec_dir[1300], path[1500], want[1500];
    snprintf(rec_dir, sizeof rec_dir, "%s/recordings", dir);
    CHECK(make_dirs(rec_dir));
    recording_ending(rec_dir, "loony-labyrinth-2026-01-01-120000.lwreplay", local_time(1, 12, 30, 0));
    recording_ending(rec_dir, "loony-labyrinth-2026-01-02-120000-marked.lwreplay",
                     local_time(2, 12, 30, 0));
    recording_ending(rec_dir, "crystal-caliburn-2026-01-03-120000.lwreplay", local_time(3, 12, 30, 0));
    CHECK(replay_find_recording(rec_dir, "loony-labyrinth", local_time(2, 12, 20, 0), path, sizeof path));
    snprintf(want, sizeof want, "%s/loony-labyrinth-2026-01-02-120000-marked.lwreplay", rec_dir);
    CHECK_STR(path, want);
    CHECK(replay_find_recording(rec_dir, "loony-labyrinth", local_time(1, 12, 30, 2), path, sizeof path));
    snprintf(want, sizeof want, "%s/loony-labyrinth-2026-01-01-120000.lwreplay", rec_dir);
    CHECK_STR(path, want);
    CHECK(!replay_find_recording(rec_dir, "loony-labyrinth", local_time(1, 13, 0, 0), path, sizeof path));
    CHECK(!replay_find_recording(rec_dir, "loony-labyrinth", local_time(3, 12, 20, 0), path, sizeof path));
    snprintf(want, sizeof want, "%s/missing", dir);
    CHECK(!replay_find_recording(want, "loony-labyrinth", local_time(2, 12, 20, 0), path, sizeof path));
}

/* Preferences in folder holding a score and this license, as the game
   saves them. */
static bool write_prefs(const char *folder, const char *email, const char *id) {
    char path[1400], err[256];
    snprintf(path, sizeof path, "%s/prefs.plist", folder);
    plist_entry e[] = {
        {"highscore 1", true, 43074596, NULL},
        {"highscore name 1", false, 0, "GREG"},
        {"user email", false, 0, (char *)email},
        {"user id", false, 0, (char *)id},
    };
    return make_dirs(folder) && plist_write(path, e, 4, err, sizeof err);
}

/* A recording made from preferences with this license, holding one key. */
static bool record_licensed(const char *email, const char *id) {
    if (!write_prefs(data, email, id) || !record(data))
        return false;
    replay_step();
    replay_log(REPLAY_KEY, (uint32_t[3]){SDL_SCANCODE_Z, 1, 0});
    replay_finish();
    return true;
}

/* The decompressed bytes of path (at most cap). */
static size_t gunzip_into(const char *path, char *buf, size_t cap) {
    gzFile g = gzopen(path, "rb");
    int n = g ? gzread(g, buf, (unsigned)cap) : -1;
    if (g)
        gzclose(g);
    return n > 0 ? (size_t)n : 0;
}

static const char *string_in(plist_entry *e, uint32_t n, const char *key) {
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(e[i].key, key) == 0 && !e[i].is_number)
            return e[i].str;
    return "(none)";
}

/* A shared copy holds no license; its replay puts in the viewer's, keeps
   everything else, and plays the records as they were. Sharing it again
   keeps it shared. */
TEST(replay_share_leaves_the_license_out_and_a_replay_puts_the_viewers_in) {
    BEGIN_TEST();
    CHECK(record_licensed("me@example.com", "123456789012"));
    char shared[1300], again[1300], err[256], buf[65536];
    snprintf(shared, sizeof shared, "%s/shared.lwreplay", dir);
    CHECK(replay_share(file, shared, err, sizeof err));
    size_t n = gunzip_into(shared, buf, sizeof buf);
    CHECK(n > 9);
    CHECK_EQ((uint8_t)buf[8], 5); /* format 5, shared */
    CHECK(!memmem(buf, n, "me@example.com", 14));
    CHECK(!memmem(buf, n, "123456789012", 12));
    CHECK(memmem(buf, n, "GREG", 4) != NULL);
    snprintf(again, sizeof again, "%s/again.lwreplay", dir);
    CHECK(replay_share(shared, again, err, sizeof err));

    char viewer[1300], license[1400], played[1400];
    snprintf(viewer, sizeof viewer, "%s/viewer", dir);
    CHECK(write_prefs(viewer, "you@example.org", "999"));
    snprintf(license, sizeof license, "%s/prefs.plist", viewer);
    setenv("LOONY_REPLAY_LICENSE", license, 1);
    snprintf(file, sizeof file, "%s", again);
    CHECK(play_back());
    snprintf(played, sizeof played, "%s/play/prefs.plist", dir);
    plist_entry *e;
    uint32_t ne;
    CHECK_EQ(plist_read(played, &e, &ne, err, sizeof err), PLIST_OK);
    char email[64], id[64], name[64];
    snprintf(email, sizeof email, "%s", string_in(e, ne, "user email"));
    snprintf(id, sizeof id, "%s", string_in(e, ne, "user id"));
    snprintf(name, sizeof name, "%s", string_in(e, ne, "highscore name 1"));
    plist_free(e, ne);
    CHECK_STR(email, "you@example.org");
    CHECK_STR(id, "999");
    CHECK_STR(name, "GREG");
    uint32_t v[3];
    replay_step();
    CHECK(replay_take(REPLAY_KEY, v));
    CHECK_EQ(v[0], SDL_SCANCODE_Z);
}

/* A high score set during a game finds that game's own file (Loony
   Labyrinth saves the score before its game ends), and the file shares and
   replays like a whole-session recording: what came before the game, then
   the game. */
TEST(replay_shares_the_file_of_the_game_that_set_a_high_score) {
    BEGIN_TEST();
    char rec_dir[1300], name[256], want[1600], path[1600], shared[1300], err[256];
    snprintf(rec_dir, sizeof rec_dir, "%s/recordings", dir);
    CHECK(write_prefs(data, "me@example.com", "123456789012"));
    unsetenv("LOONY_RECORD");
    unsetenv("LOONY_FIXED_CLOCK");
    unsetenv("LOONY_REPLAY");
    replay_reset();
    replay_record_into(rec_dir, "loony-labyrinth", NULL, 0);
    CHECK(replay_start("loony-labyrinth", exe, sizeof exe, data, err, sizeof err));
    replay_step();
    replay_log(REPLAY_KEY, (uint32_t[3]){SDL_SCANCODE_X, 1, 0}); /* before the game */
    replay_game(true);
    replay_play(true);
    replay_step();
    replay_log(REPLAY_KEY, (uint32_t[3]){SDL_SCANCODE_Z, 1, 0});
    time_t when = time(NULL); /* the score is saved */
    replay_clock(5000);
    replay_play(false);
    replay_game(false);
    replay_finish();

    new_recording(rec_dir, "", name, sizeof name);
    snprintf(want, sizeof want, "%s/%s", rec_dir, name);
    CHECK(replay_find_recording(rec_dir, "loony-labyrinth", when, path, sizeof path));
    CHECK_STR(path, want);
    snprintf(shared, sizeof shared, "%s/shared.lwreplay", dir);
    CHECK(replay_share(path, shared, err, sizeof err));

    char viewer[1300], license[1400];
    snprintf(viewer, sizeof viewer, "%s/viewer", dir);
    CHECK(write_prefs(viewer, "you@example.org", "999"));
    snprintf(license, sizeof license, "%s/prefs.plist", viewer);
    setenv("LOONY_REPLAY_LICENSE", license, 1);
    snprintf(file, sizeof file, "%s", shared);
    CHECK(play_back());
    const replay_span *spans;
    CHECK_EQ(replay_play_spans(&spans), 1);
    uint32_t v[3];
    replay_step();
    CHECK(replay_take(REPLAY_KEY, v));
    CHECK_EQ(v[0], SDL_SCANCODE_X);
    CHECK(replay_take_play() && replay_take_play()); /* the game starts, and play */
    replay_step();
    CHECK(replay_take(REPLAY_KEY, v));
    CHECK_EQ(v[0], SDL_SCANCODE_Z);
    CHECK_EQ(replay_clock(0), 5000); /* the game's 5 ms of play */
}

/* A game played unregistered isn't shared: given the viewer's license, its
   replay would start differently. */
TEST(replay_share_refuses_a_game_played_unregistered) {
    BEGIN_TEST();
    char shared[1300], err[256];
    snprintf(shared, sizeof shared, "%s/shared.lwreplay", dir);
    CHECK(record_licensed("", ""));
    CHECK(!replay_share(file, shared, err, sizeof err));
    CHECK_STR(err, "this game was played before Loony Labyrinth was registered, so it can't be shared");
    CHECK(access(shared, F_OK) != 0);
    CHECK(record(NULL)); /* no preferences at all */
    replay_finish();
    CHECK(!replay_share(file, shared, err, sizeof err));
    CHECK_STR(err, "this game was played before Loony Labyrinth was registered, so it can't be shared");
}

/* A shared copy needs a license to replay: by default, the one in the
   viewer's own save folder, found before LOONY_DATA_DIR names the
   replay's. */
TEST(replay_of_a_shared_copy_needs_the_viewers_license) {
    BEGIN_TEST();
    CHECK(record_licensed("me@example.com", "123456789012"));
    char shared[1300], err[512], unlicensed[1300], want[1600];
    snprintf(shared, sizeof shared, "%s/shared.lwreplay", dir);
    CHECK(replay_share(file, shared, err, sizeof err));
    snprintf(file, sizeof file, "%s", shared);
    snprintf(unlicensed, sizeof unlicensed, "%s/unlicensed", dir);
    CHECK(write_prefs(unlicensed, "", ""));
    snprintf(unlicensed, sizeof unlicensed, "%s/unlicensed/prefs.plist", dir);
    setenv("LOONY_REPLAY_LICENSE", unlicensed, 1);
    CHECK(!play("loony-labyrinth", exe, sizeof exe, err, sizeof err));
    snprintf(want, sizeof want,
             "this replay was shared without a license: register Loony Labyrinth in LittleWing to "
             "watch it (%s has none)",
             unlicensed);
    CHECK_STR(err, want);
    unsetenv("LOONY_REPLAY_LICENSE");
    CHECK(!play("loony-labyrinth", exe, sizeof exe, err, sizeof err));
    CHECK_STR(err, "this replay was shared without a license: set LOONY_REPLAY_LICENSE to the "
                   "preferences of a registered Loony Labyrinth");

    /* With no LOONY_DATA_DIR, the viewer's own preferences under HOME. */
    char *home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    setenv("HOME", dir, 1);
    unsetenv("LOONY_DATA_DIR");
    replay_reset();
    setenv("LOONY_REPLAY", file, 1);
    bool opened = replay_open(err, sizeof err);
    char made[1300];
    snprintf(made, sizeof made, "%s", getenv("LOONY_DATA_DIR") ? getenv("LOONY_DATA_DIR") : "");
    if (home)
        setenv("HOME", home, 1);
    free(home);
    if (made[0])
        rmdir(made);
    CHECK(opened);
    snprintf(want, sizeof want, "%s/Library/Application Support/littlewing-shim/loony-labyrinth/prefs.plist",
             dir);
    CHECK_STR(getenv("LOONY_REPLAY_LICENSE"), want);
}

/* What the app checks before it restarts to watch a file: a replay it can
   read, and for a shared copy, a license in the viewer's own preferences. */
TEST(replay_check_says_why_a_file_cant_be_watched) {
    BEGIN_TEST();
    char err[512], shared[1300], other[1300], own[1300];
    CHECK(record_licensed("me@example.com", "123456789012"));
    CHECK(replay_check(file, err, sizeof err));
    snprintf(shared, sizeof shared, "%s/shared.lwreplay", dir);
    CHECK(replay_share(file, shared, err, sizeof err));
    setenv("HOME", dir, 1);
    CHECK(!replay_check(shared, err, sizeof err));
    CHECK_STR(err, "it was shared without a license: register Loony Labyrinth in LittleWing to watch it");
    snprintf(own, sizeof own, "%s/Library/Application Support/littlewing-shim/loony-labyrinth", dir);
    CHECK(write_prefs(own, "you@example.org", "999"));
    CHECK(replay_check(shared, err, sizeof err));
    snprintf(other, sizeof other, "%s/not-a-replay.lwreplay", dir);
    gzFile g = gzopen(other, "wb");
    gzwrite(g, "PNG", 3);
    gzclose(g);
    CHECK(!replay_check(other, err, sizeof err));
    CHECK_STR(err, "not a replay file");
    snprintf(other, sizeof other, "%s/missing.lwreplay", dir);
    CHECK(!replay_check(other, err, sizeof err));
    CHECK_STR(err, "it can't be read");
}

/* LOONY_RECORD=0 turns it off; LOONY_RECORD names the file instead; a
   fixed clock or a replay never records. */
TEST(replay_records_every_game_unless_told_otherwise) {
    BEGIN_TEST();
    char rec_dir[1300], err[256];
    snprintf(rec_dir, sizeof rec_dir, "%s/recordings", dir);
    unsetenv("LOONY_REPLAY");
    unsetenv("LOONY_FIXED_CLOCK");
    setenv("LOONY_RECORD", "0", 1);
    replay_reset();
    replay_record_into(rec_dir, "loony-labyrinth", NULL, 0);
    CHECK(replay_start("loony-labyrinth", exe, sizeof exe, NULL, err, sizeof err));
    CHECK(!replay_recording());
    CHECK(access(rec_dir, F_OK) != 0);

    unsetenv("LOONY_RECORD");
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    replay_reset();
    replay_record_into(rec_dir, "loony-labyrinth", NULL, 0);
    CHECK(replay_start("loony-labyrinth", exe, sizeof exe, NULL, err, sizeof err));
    CHECK(!replay_recording());

    unsetenv("LOONY_FIXED_CLOCK");
    setenv("LOONY_RECORD", file, 1);
    replay_reset();
    replay_record_into(rec_dir, "loony-labyrinth", NULL, 0);
    CHECK(replay_start("loony-labyrinth", exe, sizeof exe, NULL, err, sizeof err));
    CHECK(replay_recording());
    CHECK_EQ(replay_mark(), 1);
    replay_finish();
    CHECK(access(file, F_OK) == 0); /* a name given is kept as it is */
    CHECK(access(rec_dir, F_OK) != 0);
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

/* A replay that finishes where the recording did (the game quits there on
   its own, as it did when recorded) has reached the end, so a clip running
   past it keeps what there is; one that finishes a step short hasn't. */
TEST(replay_finishing_where_the_recording_did_is_its_end) {
    BEGIN_TEST();
    CHECK(record(NULL));
    replay_step();
    replay_step();
    replay_finish();
    CHECK(play_back());
    replay_step();
    replay_finish();
    CHECK(!replay_reached_end());
    CHECK(play_back());
    replay_step();
    replay_step();
    replay_finish();
    CHECK(replay_reached_end());
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

/* Recording, Microseconds and Delay(0) polled in one ms (Loony Labyrinth's
   menus spin on them) sleep to the next ms every MS_SPIN_POLLS polls, so a
   spin is a few steps a ms, and a replay passes the same polls and values. */
TEST(replay_microseconds_spins_wait_for_the_next_ms) {
    BEGIN_TEST();
    setup();
    CHECK(record(NULL));
    uint32_t us = scratch(8), got[15];
    for (int i = 0; i < 15; i++) {
        call_import("Delay", 2, 0u, 0u);
        call_import("Microseconds", 1, us);
        got[i] = gm_r32(us + 4);
    }
    replay_finish();
    CHECK(got[14] - got[0] >= 9000); /* 30 polls, a ms every 3 */
    CHECK(got[14] - got[0] < 1000000);

    setup();
    CHECK(play_back());
    for (int i = 0; i < 15; i++) {
        call_import("Delay", 2, 0u, 0u);
        call_import("Microseconds", 1, us);
        CHECK_EQ(gm_r32(us + 4), got[i]);
    }
}

/* A replay knows where the game was played before it plays it, and maps its
   clock to play time and back, leaving out what isn't play: before the
   game, and a pause. Without play records, all of it is play. */
TEST(replay_play_time_leaves_out_what_isnt_play) {
    BEGIN_TEST();
    CHECK(record(NULL));
    replay_clock(1000000); /* a second before the game */
    replay_game(true);
    replay_play(true);
    replay_clock(3000000); /* two seconds of play */
    replay_play(false);
    replay_clock(63000000); /* a minute's pause */
    replay_play(true);
    replay_clock(64000000);
    replay_finish(); /* in play */
    CHECK(play_back());
    const replay_span *spans;
    CHECK_EQ(replay_play_spans(&spans), 2);
    CHECK_EQ(spans[0].on_us, 1000000);
    CHECK_EQ(spans[0].off_us, 3000000);
    CHECK_EQ(spans[1].on_us, 63000000);
    CHECK(spans[1].off_us == UINT64_MAX);
    CHECK_EQ(replay_play_us(500000), 0);
    CHECK_EQ(replay_play_us(2000000), 1000000);
    CHECK_EQ(replay_play_us(30000000), 2000000);
    CHECK_EQ(replay_play_us(63500000), 2500000);
    CHECK_EQ(replay_clock_us(0), 1000000);
    CHECK_EQ(replay_clock_us(1500000), 2500000);
    CHECK_EQ(replay_clock_us(2000000), 63000000); /* where the pause ends */
    CHECK_EQ(replay_clock_us(2500000), 63500000);
    CHECK_EQ(replay_play_ticks(63 * 60 + 30), 2 * 60 + 30);
    CHECK_EQ(replay_clock_ticks(2 * 60 + 30), 63 * 60 + 30);
    uint64_t next;
    CHECK(!replay_in_play(500000, &next));
    CHECK_EQ(next, 1000000);
    CHECK(replay_in_play(2000000, &next));
    CHECK(!replay_in_play(30000000, &next));
    CHECK_EQ(next, 63000000);
    CHECK(replay_in_play(70000000, &next));

    CHECK(record(NULL));
    replay_clock(1000000);
    replay_finish();
    CHECK(play_back());
    CHECK_EQ(replay_play_spans(&spans), 0);
    CHECK_EQ(replay_play_us(500000), 500000);
    CHECK_EQ(replay_clock_us(500000), 500000);
    CHECK(replay_in_play(0, &next));
}

/* LOONY_CLIP=<arg> of the replay, in a child: clip_open's err is the output. */
static void child_clip_span(void *span) {
    char err[256], play_data[1300], out[1300];
    snprintf(play_data, sizeof play_data, "%s/play", dir);
    snprintf(out, sizeof out, "%s/clip.mp4", dir);
    replay_reset();
    unsetenv("LOONY_RECORD");
    setenv("LOONY_REPLAY", file, 1);
    setenv("LOONY_DATA_DIR", play_data, 1);
    setenv("LOONY_CLIP", span, 1);
    setenv("LOONY_CLIP_OUT", out, 1);
    if (!replay_open(err, sizeof err) || !clip_open(err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        _exit(1);
    }
    _exit(0);
}

/* A clip whose play is too short to fill a tick on the game's clock is
   refused, not made of nothing. */
TEST(replay_a_clip_with_no_whole_tick_of_play_is_refused) {
    BEGIN_TEST();
    CHECK(record(NULL));
    replay_clock(1000000);
    replay_game(true);
    replay_play(true);
    replay_clock(1005000); /* 5 ms of play, inside tick 60 */
    replay_play(false);
    replay_clock(2000000);
    replay_play(true); /* then the play time's first tick ends 11.7 ms on, inside tick 120 */
    replay_clock(3000000);
    replay_finish();
    char out[8192];
    CHECK_EQ(test_run_child(child_clip_span, "0-1", out, sizeof out), 1);
    CHECK_CONTAINS(out, "the clip 0:00-0:00.01 holds less than a tick of play");
    CHECK_EQ(test_run_child(child_clip_span, "0-2", out, sizeof out), 0);
}

/* Games that start in the same second are kept in the order they started:
   "-2" after the second's first, "-10" after "-2". */
TEST(replay_keeps_the_last_games_of_a_second_in_order) {
    BEGIN_TEST();
    char rec_dir[1300];
    snprintf(rec_dir, sizeof rec_dir, "%s/recordings", dir);
    CHECK(make_dirs(rec_dir));
    static const char *const old[] = {
        "loony-labyrinth-2026-01-01-100000.lwreplay",
        "loony-labyrinth-2026-01-01-100000-2.lwreplay",
        "loony-labyrinth-2026-01-01-100000-10.lwreplay",
        "loony-labyrinth-2026-01-01-100001.lwreplay",
        "loony-labyrinth-2026-01-01-100002.lwreplay",
    };
    for (int i = 0; i < 5; i++)
        touch(rec_dir, old[i]);
    unsetenv("LOONY_RECORD");
    unsetenv("LOONY_FIXED_CLOCK");
    unsetenv("LOONY_REPLAY");
    replay_reset();
    replay_record_into(rec_dir, "loony-labyrinth", NULL, 0);
    CHECK(!exists_in(rec_dir, old[0]));
    CHECK(!exists_in(rec_dir, old[1]));
    for (int i = 2; i < 5; i++)
        CHECK(exists_in(rec_dir, old[i]));
}

/* Watching at 1x, headless, to the recording's end. */
static void child_watch(void *unused) {
    (void)unused;
    char err[256], play_data[1300];
    snprintf(play_data, sizeof play_data, "%s/play", dir);
    replay_reset();
    unsetenv("LOONY_RECORD");
    setenv("LOONY_REPLAY", file, 1);
    setenv("LOONY_WATCH", "1", 1);
    setenv("SDL_VIDEO_DRIVER", "dummy", 1);
    setenv("SDL_AUDIO_DRIVER", "dummy", 1);
    if (!replay_open(err, sizeof err) || !watch_open(err, sizeof err) ||
        !replay_start("loony-labyrinth", exe, sizeof exe, play_data, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        _exit(1);
    }
    watch_start("Game");
    for (;;) { /* exits at the recording's end */
        replay_clock(0);
        while (replay_take_play())
            ;
    }
}

/* Watching goes to the game's start unpaced, plays at 1x, and leaves out a
   minute's pause: 0.6 s of play takes about 0.6 s. The clock moves a ms at
   a time in play, as a game's does. */
TEST(replay_watching_leaves_out_what_isnt_play) {
    BEGIN_TEST();
    CHECK(record(NULL));
    replay_clock(1000000); /* a second before the game */
    replay_game(true);
    replay_play(true);
    for (uint64_t us = 1001000; us <= 1300000; us += 1000)
        replay_clock(us);
    replay_play(false);
    for (uint64_t us = 2300000; us <= 61300000; us += 1000000) /* a minute's pause */
        replay_clock(us);
    replay_play(true);
    for (uint64_t us = 61301000; us <= 61600000; us += 1000)
        replay_clock(us);
    replay_finish();
    char out[16384];
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK_EQ(test_run_child(child_watch, NULL, out, sizeof out), 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double took = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    CHECK(took >= 0.55 && took < 10);
    CHECK_CONTAINS(out, "watch: reached 0:00");
    /* Where a stretch starts moves if the test stalls past WATCH_MAX_LAG;
       where it ends doesn't. */
    CHECK_CONTAINS(out, "watch: leaving out what isn't play from 0:00.2"); /* its last ms isn't paced */
    CHECK_CONTAINS(out, "-0:00.60 at 1x");
    CHECK_CONTAINS(out, "reached the end of the recording at step 661"); /* each clock */
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

/* A format 5 header for "x" with this start and time zone (varints). */
static size_t header_with(uint8_t *h, const char *start, const char *gmtoff) {
    memcpy(h, "LWREPLAY\x05\x01x", 11);
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
    write_replay("LWREPLAY\x06", 9);
    CHECK(!replay_open(err, sizeof err));
    CHECK_STR(err, "the replay was recorded by a newer version of LittleWing");
    uint8_t h[64];
    /* The prototype's formats, which recorded a whole session. */
    for (uint8_t old = 3; old <= 4; old++) {
        size_t n = header_with(h, "\x01", "\x02");
        h[8] = old;
        write_replay(h, n);
        CHECK(!replay_open(err, sizeof err));
        CHECK_STR(err, "the replay was recorded by an early prototype of LittleWing, "
                       "and this version can't play it");
    }
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
