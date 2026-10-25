#include "test.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <copyfile.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>


#include "cgimage.h"
#include "clip.h"
#include "gamestart.h"
#include "mp4.h"
#include "plist.h"
#include "recordings.h"
#include "replay.h"
#include "util.h"
#include "watch.h"

/* LOONY_DATA_DIR for the runs that follow: each scripted run gets a fresh
   one unless a test set this, so preferences saved by one run never change
   another's frames. Otherwise the runner's own temporary folder is used. */
static char run_data[1024];

/* The golden frames predate dialogs, so runs answer alerts at once unless a
   test asks for real ones. */
static bool real_alerts;

static void run_loony(void *dir) {
    if (run_data[0])
        setenv("LOONY_DATA_DIR", run_data, 1);
    if (!real_alerts)
        setenv("LOONY_AUTO_ALERTS", "1", 1);
    execl(LOONY_BIN, "loony", (const char *)dir, (char *)NULL);
    fprintf(stderr, "exec %s failed\n", LOONY_BIN);
    _exit(127);
}

static char shot[1024];

static void run_loony_headless(void *dir) {
    setenv("LOONY_EXIT_AFTER", "240", 1);
    setenv("LOONY_SCREENSHOT", shot, 1);
    run_loony(dir);
}

TEST(run_plays_the_opening_headless) {
    SKIP_UNLESS_GAME();
    const char *t = getenv("TMPDIR");
    snprintf(shot, sizeof shot, "%s/loony-run-XXXXXX", t && *t ? t : "/tmp");
    int fd = mkstemp(shot);
    CHECK(fd >= 0);
    close(fd);
    char out[32768];
    test_tmp_dir(run_data, sizeof run_data);
    int status = test_run_child(run_loony_headless, (void *)test_game_dir(), out, sizeof out);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    size_t len = 0;
    uint8_t *png = read_file(shot, &len);
    unlink(shot);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "132 imports");
    CHECK_CONTAINS(out, "loony: Alert 901 (answering item 1): Play Demo");
    CHECK_CONTAINS(out, "loony: Alert 900 (answering item 1): OK");
    CHECK_CONTAINS(out, "loony: exiting after 240 ticks (LOONY_EXIT_AFTER)");
    CHECK(!strstr(out, "unknown selector"));
    CHECK(!strstr(out, "not supported"));
    CHECK(png != NULL);
    CHECK(len > 33);
    CHECK_EQ(rd_be32(png + 16), 800);
    CHECK_EQ(rd_be32(png + 20), 600);
    free(png);
}

TEST(run_crystal_caliburn_plays_its_opening_headless) {
    SKIP_UNLESS_CC();
    const char *t = getenv("TMPDIR");
    snprintf(shot, sizeof shot, "%s/loony-run-XXXXXX", t && *t ? t : "/tmp");
    int fd = mkstemp(shot);
    CHECK(fd >= 0);
    close(fd);
    char out[32768];
    test_tmp_dir(run_data, sizeof run_data);
    int status = test_run_child(run_loony_headless, (void *)test_cc_dir(), out, sizeof out);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    size_t len = 0;
    uint8_t *png = read_file(shot, &len);
    unlink(shot);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "playing Crystal Caliburn from");
    CHECK_CONTAINS(out, "CRYSTAL CALIBURN 3.0.1: 132 imports");
    CHECK_CONTAINS(out, "loony: Alert 901 (answering item 1): Play Demo");
    CHECK_CONTAINS(out, "LittleWing CRYSTAL CALIBURN Pinball");
    CHECK_CONTAINS(out, "loony: Alert 900 (answering item 1): OK");
    CHECK_CONTAINS(out, "loony: exiting after 240 ticks (LOONY_EXIT_AFTER)");
    CHECK(!strstr(out, "unknown selector"));
    CHECK(!strstr(out, "not supported"));
    CHECK(png != NULL);
    CHECK(len > 33);
    CHECK_EQ(rd_be32(png + 16), 800);
    CHECK_EQ(rd_be32(png + 20), 600);
    free(png);
}

/* Jinni Zeala 1.2.2, an older build of the classic engine: its own alerts,
   and the 123 calls it makes all answered. */
TEST(run_jinni_zeala_plays_its_opening_headless) {
    SKIP_UNLESS_JZ();
    const char *t = getenv("TMPDIR");
    snprintf(shot, sizeof shot, "%s/loony-run-XXXXXX", t && *t ? t : "/tmp");
    int fd = mkstemp(shot);
    CHECK(fd >= 0);
    close(fd);
    char out[32768];
    test_tmp_dir(run_data, sizeof run_data);
    int status = test_run_child(run_loony_headless, (void *)test_jz_dir(), out, sizeof out);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    size_t len = 0;
    uint8_t *png = read_file(shot, &len);
    unlink(shot);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "playing Jinni Zeala from");
    CHECK_CONTAINS(out, "JINNI ZEALA 1.2.2: 123 imports");
    CHECK_CONTAINS(out, "loony: Alert 128 (answering item 1): Play Demo");
    CHECK_CONTAINS(out, "LittleWing JINNI ZEALA Pinball");
    CHECK_CONTAINS(out, "loony: Alert 132 (answering item 1): OK");
    CHECK_CONTAINS(out, "loony: exiting after 240 ticks (LOONY_EXIT_AFTER)");
    CHECK(!strstr(out, "unknown selector"));
    CHECK(!strstr(out, "not supported"));
    CHECK(png != NULL);
    CHECK(len > 33);
    CHECK_EQ(rd_be32(png + 16), 800);
    CHECK_EQ(rd_be32(png + 20), 600);
    free(png);
}

/* Tristan 2.5.1, a classic pre-Carbon program: its two demo alerts
   answered, the 135 calls it makes, and its 640x480 screen. */
TEST(run_tristan_plays_its_opening_headless) {
    SKIP_UNLESS_TR();
    const char *t = getenv("TMPDIR");
    snprintf(shot, sizeof shot, "%s/loony-run-XXXXXX", t && *t ? t : "/tmp");
    int fd = mkstemp(shot);
    CHECK(fd >= 0);
    close(fd);
    char out[32768];
    test_tmp_dir(run_data, sizeof run_data);
    int status = test_run_child(run_loony_headless, (void *)test_tr_dir(), out, sizeof out);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    size_t len = 0;
    uint8_t *png = read_file(shot, &len);
    unlink(shot);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "playing Tristan from");
    CHECK_CONTAINS(out, "TRISTAN IE 2.5.1: 135 imports");
    CHECK_CONTAINS(out, "loony: Alert 901 (answering item 1): Start Demo");
    CHECK_CONTAINS(out, "LittleWing PINBALL TRISTAN");
    CHECK_CONTAINS(out, "loony: Alert 900 (answering item 1): OK");
    CHECK_CONTAINS(out, "loony: exiting after 240 ticks (LOONY_EXIT_AFTER)");
    CHECK(!strstr(out, "unknown selector"));
    CHECK(!strstr(out, "not supported"));
    CHECK(png != NULL);
    CHECK(len > 33);
    CHECK_EQ(rd_be32(png + 16), 640);
    CHECK_EQ(rd_be32(png + 20), 480);
    free(png);
}

/* Fixed-clock runs are deterministic, so their frames can be compared with
   golden hashes (FNV-1a32 of the PNG file, which is uncompressed). The user
   approved the golden frames on 2026-10-02: the LittleWing logo, the title,
   the menu, and a one-player game started from it. */

static char script_path[1024], wav_path[1024];

static void run_loony_scripted(void *dir) {
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    setenv("LOONY_SCRIPT", script_path, 1);
    if (wav_path[0])
        setenv("LOONY_WAV", wav_path, 1);
    run_loony(dir);
}

static void tmp_name(char *buf, size_t cap, const char *what) {
    const char *t = getenv("TMPDIR");
    snprintf(buf, cap, "%s/loony-%s-XXXXXX", t && *t ? t : "/tmp", what);
    int fd = mkstemp(buf);
    if (fd >= 0)
        close(fd);
}

static int by_tick(const void *a, const void *b) {
    unsigned long x = strtoul(*(char *const *)a, NULL, 10), y = strtoul(*(char *const *)b, NULL, 10);
    return x < y ? -1 : x > y;
}

/* Runs the game in dir with a script (lines in any order, sorted by tick here),
   returning the exit status; shots[i] gets the hash of the screenshot taken
   at ticks[i]. */
static int run_script_in(const char *dir, const char *actions, const uint32_t *ticks, uint32_t *shots,
                         int nshots, char *out, size_t outlen) {
    char pngs[4][1024], lines[32][1100];
    char *order[32];
    int n = 0;
    for (int i = 0; i < nshots; i++) {
        tmp_name(pngs[i], sizeof pngs[i], "shot");
        snprintf(lines[n++], sizeof lines[0], "%u screenshot %s", ticks[i], pngs[i]);
    }
    for (const char *p = actions; *p && n < 32;) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        snprintf(lines[n++], sizeof lines[0], "%.*s", (int)len, p);
        p += len + (eol != NULL);
    }
    for (int i = 0; i < n; i++)
        order[i] = lines[i];
    qsort(order, (size_t)n, sizeof order[0], by_tick); /* qsort isn't stable; ticks here are distinct */
    tmp_name(script_path, sizeof script_path, "script");
    FILE *f = fopen(script_path, "w");
    for (int i = 0; i < n; i++)
        fprintf(f, "%s\n", order[i]);
    fclose(f);
    bool own_data = !run_data[0];
    if (own_data)
        test_tmp_dir(run_data, sizeof run_data);
    int status = test_run_child(run_loony_scripted, (void *)dir, out, outlen);
    if (own_data) {
        test_remove_tree(run_data);
        run_data[0] = '\0';
    }
    for (int i = 0; i < nshots; i++) {
        size_t len = 0;
        uint8_t *png = read_file(pngs[i], &len);
        shots[i] = png ? fnv1a32(png, len) : 0;
        free(png);
        unlink(pngs[i]);
    }
    unlink(script_path);
    return status;
}

/* The same, with Loony Labyrinth. */
static int run_script(const char *actions, const uint32_t *ticks, uint32_t *shots, int nshots,
                      char *out, size_t outlen) {
    return run_script_in(test_game_dir(), actions, ticks, shots, nshots, out, outlen);
}

TEST(run_opening_frames_match_their_goldens) {
    SKIP_UNLESS_GAME();
    uint32_t ticks[2] = {30, 240}, shots[2];
    char out[32768];
    int status = run_script("300 quit\n", ticks, shots, 2, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: sending the quit Apple Event");
    CHECK_EQ(shots[0], 0xABFE3C2Bu); /* the LittleWing logo */
    CHECK_EQ(shots[1], 0x4C3A7003u); /* the title */
}

TEST(run_a_game_starts_from_the_menu) {
    SKIP_UNLESS_GAME();
    /* Esc ends the self-playing demo, Esc again opens the menu, Return picks
       "1 player"; then pull and release the plunger. */
    const char *actions = "1720 down esc\n1724 up esc\n1800 down esc\n1804 up esc\n"
                          "1900 down return\n1906 up return\n2100 down return\n"
                          "2190 up return\n2600 quit\n";
    uint32_t ticks[2] = {1880, 2500}, shots[2];
    char out[32768];
    int status = run_script(actions, ticks, shots, 2, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_EQ(shots[0], 0xADE78151u); /* the menu */
    CHECK_EQ(shots[1], 0xA162CB3Du); /* ball 1 in play, "DEMO VERSION TIME LEFT" */
}

/* A recording and its replay (replay.h): the paths each run uses. */
static char replay_file[1024], replay_shot[1024];

static void run_loony_recording(void *dir) {
    setenv("LOONY_SCRIPT", script_path, 1);
    setenv("LOONY_RECORD", replay_file, 1);
    setenv("LOONY_SCREENSHOT", replay_shot, 1);
    run_loony(dir);
}

static void run_loony_replaying(void *unused) {
    (void)unused;
    unsetenv("LOONY_AUTO_ALERTS"); /* the replay sets it as the recording had it */
    setenv("LOONY_REPLAY", replay_file, 1);
    setenv("LOONY_DATA_DIR", run_data, 1);
    setenv("LOONY_SCREENSHOT", replay_shot, 1);
    execl(LOONY_BIN, "loony", (char *)NULL);
    _exit(127);
}

/* Records a game started from the menu and played a little, on the real
   clock with sound (SDL's dummy audio device plays it in real time), then
   replays it headless: the replay reaches the end of the recording with
   every frame check matching, and leaves the same last frame and the same
   preferences. */
SLOW_TEST(run_a_recorded_game_replays_exactly) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024], prefs[2][1100], shots[2][1024];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    snprintf(replay_file, sizeof replay_file, "%s/game.lwreplay", rec_dir);
    tmp_name(script_path, sizeof script_path, "script");
    FILE *f = fopen(script_path, "w");
    fputs("1720 down esc\n1724 up esc\n1800 down esc\n1804 up esc\n1900 down return\n"
          "1906 up return\n2100 down return\n2190 up return\n2300 down z\n2310 up z\n"
          "2400 down slash\n2410 up slash\n2600 quit\n", f);
    fclose(f);
    char out[2][32768];
    for (int i = 0; i < 2; i++) {
        tmp_name(shots[i], sizeof shots[i], "shot");
        snprintf(replay_shot, sizeof replay_shot, "%s", shots[i]);
        snprintf(run_data, sizeof run_data, "%s", i ? play_dir : rec_dir);
        int status = test_run_child(i ? run_loony_replaying : run_loony_recording,
                                    (void *)test_game_dir(), out[i], sizeof out[i]);
        CHECK_EQ(status, 0);
        snprintf(prefs[i], sizeof prefs[i], "%s/prefs.plist", run_data);
    }
    run_data[0] = '\0';
    unlink(script_path);
    size_t len[4];
    uint8_t *files[4] = {read_file(shots[0], &len[0]), read_file(shots[1], &len[1]),
                         read_file(prefs[0], &len[2]), read_file(prefs[1], &len[3])};
    bool same_shot = files[0] && files[1] && len[0] == len[1] && memcmp(files[0], files[1], len[0]) == 0;
    bool same_prefs = files[2] && files[3] && len[2] == len[3] && memcmp(files[2], files[3], len[2]) == 0;
    for (int i = 0; i < 4; i++)
        free(files[i]);
    unlink(shots[0]);
    unlink(shots[1]);
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_CONTAINS(out[0], "loony: replay: recording to");
    CHECK(!strstr(out[0], "no audio output"));
    CHECK_CONTAINS(out[1], "loony: replay: reached the end of the recording");
    CHECK_CONTAINS(out[1], "no desync");
    const char *end = strstr(out[1], "reached the end of the recording");
    unsigned frames = 0;
    CHECK(end && sscanf(end, "reached the end of the recording at step %*u (tick %*u), %u frames",
                        &frames) == 1);
    CHECK(frames > 0);
    CHECK(same_shot);
    CHECK(same_prefs);
}

static char clip_path[1100]; /* LOONY_CLIP_OUT, unless empty */
static const char *clip_spec, *clip_before, *clip_after; /* LOONY_CLIP, _BEFORE, _AFTER */

static void run_loony_clipping(void *unused) {
    (void)unused;
    unsetenv("LOONY_AUTO_ALERTS");
    setenv("LOONY_REPLAY", replay_file, 1);
    setenv("LOONY_DATA_DIR", run_data, 1);
    setenv("LOONY_CLIP", clip_spec, 1);
    if (clip_path[0])
        setenv("LOONY_CLIP_OUT", clip_path, 1);
    if (clip_before)
        setenv("LOONY_CLIP_BEFORE", clip_before, 1);
    if (clip_after)
        setenv("LOONY_CLIP_AFTER", clip_after, 1);
    setenv("LOONY_HD", LOONY_SRC_DIR "/hd-art/loony-labyrinth", 1);
    execl(LOONY_BIN, "loony", (char *)NULL);
    _exit(127);
}

/* Records the opening with this script (quitting at tick 330, 5.5 s, in
   the tests) into rec_dir/game.lwreplay: the recording's status. */
static int record_for_clips(const char *rec_dir, const char *script, char *out, size_t cap) {
    snprintf(replay_file, sizeof replay_file, "%s/game.lwreplay", rec_dir);
    tmp_name(script_path, sizeof script_path, "script");
    tmp_name(replay_shot, sizeof replay_shot, "shot");
    FILE *f = fopen(script_path, "w");
    fputs(script, f);
    fclose(f);
    snprintf(run_data, sizeof run_data, "%s", rec_dir);
    int status = test_run_child(run_loony_recording, (void *)test_game_dir(), out, cap);
    run_data[0] = '\0';
    unlink(script_path);
    unlink(replay_shot);
    return status;
}

/* Clips span of that recording to <play_dir>/<name> (with no name, where
   the clips go by default), replaying in a data folder there of its own:
   the clip's status. */
static int clip_recording(const char *span, const char *play_dir, const char *name, char *out, size_t cap) {
    clip_spec = span;
    if (name)
        snprintf(clip_path, sizeof clip_path, "%s/%s", play_dir, name);
    else
        clip_path[0] = '\0';
    snprintf(run_data, sizeof run_data, "%s/data-%s", play_dir, name ? name : "default");
    int status = test_run_child(run_loony_clipping, NULL, out, cap);
    run_data[0] = '\0';
    return status;
}

/* Two games, the first thrown away with Esc, as the unregistered game does,
   after the self-playing demo. */
static const char two_games[] =
    "1720 down esc\n1724 up esc\n1800 down esc\n1804 up esc\n1900 down return\n1906 up return\n"
    "2100 down return\n2190 up return\n2400 down esc\n2404 up esc\n2600 down esc\n2604 up esc\n"
    "2700 down return\n2706 up return\n2900 down return\n2990 up return\n3300 quit\n";

static int count_of(const char *s, const char *what) {
    int n = 0;
    for (const char *p = s; (p = strstr(p, what)); p += strlen(what))
        n++;
    return n;
}

/* The shim tells where a game starts and ends from the game's own state
   (play.h): the demo isn't a game, and Esc throws the first one away. */
TEST(run_games_are_told_apart_from_the_demo_and_each_other) {
    SKIP_UNLESS_GAME();
    char out[32768];
    int status = run_script(two_games, NULL, NULL, 0, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_EQ(count_of(out, "loony: play: a game starts"), 2);
    CHECK_EQ(count_of(out, "loony: play: the game ends (thrown away)"), 1);
}

static void run_loony_recording_games(void *dir) {
    setenv("LOONY_SCRIPT", script_path, 1);
    unsetenv("LOONY_RECORD");
    run_loony(dir);
}

/* Unless told otherwise, each game is recorded into a file of its own
   (replay.h) that replays from launch exactly to the game's end; a clip of
   the second game starts where it does, past the first. */
SLOW_TEST(run_each_game_is_recorded_into_a_file_of_its_own) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024], recs[1100], names[2][256], out[4][32768];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    tmp_name(script_path, sizeof script_path, "script");
    FILE *f = fopen(script_path, "w");
    fputs(two_games, f);
    fclose(f);
    snprintf(run_data, sizeof run_data, "%s", rec_dir);
    int rec_status = test_run_child(run_loony_recording_games, (void *)test_game_dir(), out[0], sizeof out[0]);
    run_data[0] = '\0';
    unlink(script_path);
    snprintf(recs, sizeof recs, "%s/recordings", rec_dir);
    int n = 0;
    DIR *d = opendir(recs);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (strncmp(e->d_name, "loony-labyrinth-", 16) == 0 && n < 2)
            snprintf(names[n++], sizeof names[0], "%s", e->d_name);
    if (d)
        closedir(d);
    if (n == 2 && strcmp(names[0], names[1]) > 0) { /* the first game first */
        char t[256];
        snprintf(t, sizeof t, "%s", names[0]);
        snprintf(names[0], sizeof names[0], "%s", names[1]);
        snprintf(names[1], sizeof names[1], "%s", t);
    }
    int status[3] = {-1, -1, -1};
    for (int i = 0; i < n; i++) {
        snprintf(replay_file, sizeof replay_file, "%s/%s", recs, names[i]);
        tmp_name(replay_shot, sizeof replay_shot, "shot");
        snprintf(run_data, sizeof run_data, "%s/data-%d", play_dir, i);
        status[i] = test_run_child(run_loony_replaying, NULL, out[1 + i], sizeof out[1 + i]);
        unlink(replay_shot);
    }
    run_data[0] = '\0';
    if (n == 2)
        status[2] = clip_recording("0:00-0:02", play_dir, "start.mp4", out[3], sizeof out[3]);
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK_CONTAINS(out[0], "loony: replay: recording each game into");
    CHECK_EQ(count_of(out[0], "loony: replay: recording the game to"), 2);
    CHECK_EQ(n, 2);
    for (int i = 0; i < 2; i++) {
        CHECK_EQ(status[i], 0);
        CHECK_CONTAINS(out[1 + i], "loony: replay: reached the end of the recording");
        CHECK_CONTAINS(out[1 + i], "no desync");
        CHECK_EQ(count_of(out[1 + i], "loony: replay: the game starts"), 1);
    }
    CHECK_CONTAINS(out[1], "loony: replay: the game ends"); /* thrown away */
    CHECK(!strstr(out[2], "loony: replay: the game ends")); /* the quit ends it */
    CHECK_EQ(status[2], 0);
    const char *start = strstr(out[3], "loony: replay: the game starts");
    const char *reached = strstr(out[3], "loony: clip: reached 0:00");
    CHECK(start && reached && start < reached);
    CHECK_CONTAINS(out[3], "loony: clip: wrote 0:00-0:02");
    CHECK_CONTAINS(out[3], "120 frames");
}

/* Copies the file at from to to: false if it can't. */
static bool copy_file_to(const char *from, const char *to) {
    size_t len;
    uint8_t *data = read_file(from, &len);
    FILE *f = data ? fopen(to, "wb") : NULL;
    bool ok = f && fwrite(data, 1, len, f) == len;
    if (f && fclose(f) != 0)
        ok = false;
    free(data);
    return ok;
}

/* Records two_games' script, cut to script, into rec_dir's recordings
   folder, one file a game: the recording's status, its names (the first
   game first) in names and their count in *n. */
static int record_games(const char *rec_dir, const char *script, char names[2][256], int *n, char *out,
                        size_t cap) {
    tmp_name(script_path, sizeof script_path, "script");
    FILE *f = fopen(script_path, "w");
    fputs(script, f);
    fclose(f);
    snprintf(run_data, sizeof run_data, "%s", rec_dir);
    int status = test_run_child(run_loony_recording_games, (void *)test_game_dir(), out, cap);
    run_data[0] = '\0';
    unlink(script_path);
    char recs[1100];
    snprintf(recs, sizeof recs, "%s/recordings", rec_dir);
    *n = 0;
    DIR *d = opendir(recs);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (strncmp(e->d_name, "loony-labyrinth-", 16) == 0 && *n < 2)
            snprintf(names[(*n)++], 256, "%s", e->d_name);
    if (d)
        closedir(d);
    if (*n == 2 && strcmp(names[0], names[1]) > 0) {
        char t[256];
        snprintf(t, sizeof t, "%s", names[0]);
        snprintf(names[0], 256, "%s", names[1]);
        snprintf(names[1], 256, "%s", t);
    }
    return status;
}

/* Files in dir whose names start with a dot, as a take's unfinished one. */
static int hidden_files(const char *dir) {
    int n = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (e->d_name[0] == '.' && strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, ".."))
            n++;
    if (d)
        closedir(d);
    return n;
}

/* A game recorded into a file of its own gets its snapshot of the game's
   start as soon as it ends, from a take in the background
   (gamestart_take_later), so its first replay starts at the game; the
   second game, which the quit ends, gets none. A share's take while a
   background one runs puts it off until after. */
SLOW_TEST(run_a_recorded_game_gets_its_snapshot_when_it_ends) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024], names[2][256], path[2][1400], recs[1100];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    static char out[2][65536];
    int n;
    /* two_games, but the second game goes on long enough for the take of
       the first, in a sanitizer build at utility priority (17 s), to end */
    char script[sizeof two_games + 16];
    snprintf(script, sizeof script, "%.*s7200 quit\n", (int)(strstr(two_games, "3300 quit") - two_games),
             two_games);
    int rec_status = record_games(rec_dir, script, names, &n, out[0], sizeof out[0]);
    snprintf(recs, sizeof recs, "%s/recordings", rec_dir);
    bool has[2] = {false, false};
    for (int i = 0; i < n; i++) {
        snprintf(path[i], sizeof path[i], "%s/%s", recs, names[i]);
        has[i] = gamestart_in_file(path[i]);
    }
    int hidden = hidden_files(recs), status = -1;
    /* A share while the second game's take runs in the background: the
       share's goes first, and the background one starts again after it. */
    bool queued = false, started = false, shared_kept = false, later_kept = false;
    if (n == 2) {
        char copy[1100], err[256];
        snprintf(copy, sizeof copy, "%s/shared.lwreplay", play_dir);
        if (copy_file_to(path[1], copy)) {
            queued = gamestart_take_later(LOONY_BIN, path[1], GAMESTART_MIN_PRELUDE_US);
            started = gamestart_take_start(LOONY_BIN, copy, err, sizeof err);
            for (int i = 0; started && i < 6000 && !gamestart_take_poll(&shared_kept); i++)
                usleep(100000);
            for (int i = 0; i < 6000 && !(later_kept = gamestart_in_file(path[1])); i++) {
                gamestart_tend();
                usleep(100000);
            }
        }
    }
    if (n == 2) {
        snprintf(replay_file, sizeof replay_file, "%s", path[0]);
        tmp_name(replay_shot, sizeof replay_shot, "shot");
        snprintf(run_data, sizeof run_data, "%s/data", play_dir);
        status = test_run_child(run_loony_replaying, NULL, out[1], sizeof out[1]);
        run_data[0] = '\0';
        unlink(replay_shot);
    }
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK_EQ(n, 2);
    CHECK_CONTAINS(out[0], "in the background (process ");
    CHECK_CONTAINS(out[0], "has its snapshot of the game's start");
    CHECK_EQ(count_of(out[0], "gamestart: taking a snapshot of the game's start into"), 1);
    CHECK(has[0]);
    CHECK(!has[1]);
    CHECK_EQ(hidden, 0);
    CHECK(queued);
    CHECK(started);
    CHECK(shared_kept);
    CHECK(later_kept);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out[1], "gamestart: going to the game's start at ");
    CHECK_CONTAINS(out[1], "loony: replay: reached the end of the recording");
    CHECK(!strstr(out[1], "replay: desync"));
}

/* Quitting while a take runs in the background stops it: the recording
   is left as it was (it replays to its end), with no hidden file beside
   it, and no take is left running. */
SLOW_TEST(run_a_quit_stops_a_take_in_the_background) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024], names[2][256], path[1400], recs[1100];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    static char out[2][65536];
    int n;
    /* two_games' first game, thrown away with Esc at 2400, and a quit a
       moment after: the take has barely started */
    int rec_status = record_games(rec_dir,
                                  "1720 down esc\n1724 up esc\n1800 down esc\n1804 up esc\n1900 down return\n"
                                  "1906 up return\n2100 down return\n2190 up return\n2400 down esc\n"
                                  "2404 up esc\n2412 quit\n",
                                  names, &n, out[0], sizeof out[0]);
    snprintf(recs, sizeof recs, "%s/recordings", rec_dir);
    int hidden = hidden_files(recs), status = -1;
    const char *pid_at = strstr(out[0], "in the background (process ");
    int pid = pid_at ? atoi(pid_at + strlen("in the background (process ")) : 0;
    bool gone = pid > 0 && kill(pid, 0) != 0 && errno == ESRCH;
    if (n == 1) {
        snprintf(path, sizeof path, "%s/%s", recs, names[0]);
        snprintf(replay_file, sizeof replay_file, "%s", path);
        tmp_name(replay_shot, sizeof replay_shot, "shot");
        snprintf(run_data, sizeof run_data, "%s/data", play_dir);
        status = test_run_child(run_loony_replaying, NULL, out[1], sizeof out[1]);
        run_data[0] = '\0';
        unlink(replay_shot);
    }
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK_EQ(n, 1);
    CHECK(pid > 0);
    CHECK_CONTAINS(out[0], "gamestart: stopping the take of ");
    CHECK(gone);
    CHECK_EQ(hidden, 0);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out[1], "loony: replay: reached the end of the recording");
    CHECK(!strstr(out[1], "replay: desync"));
}

/* A game whose play starts within GAMESTART_MIN_PRELUDE_US of launch, or
   that has no play, gets no take (a replay wouldn't keep a snapshot). */
TEST(run_a_short_prelude_starts_no_take) {
    CHECK(!gamestart_take_later(LOONY_BIN, "/nonexistent/game.lwreplay", GAMESTART_MIN_PRELUDE_US - 1));
    CHECK(!gamestart_take_later(LOONY_BIN, "/nonexistent/game.lwreplay", UINT64_MAX));
}

/* A stand-in for this program in a take: a shell script in dir, named
   name, running body. */
static void fake_take(char *out, size_t cap, const char *dir, const char *name, const char *body) {
    snprintf(out, cap, "%s/%s", dir, name);
    FILE *f = fopen(out, "w");
    if (f) {
        fprintf(f, "#!/bin/sh\n%s\n", body);
        fclose(f);
    }
    chmod(out, 0755);
}

/* The number of lines in path, and the number on the last, in *last. */
static int lines_of(const char *path, int *last) {
    FILE *f = fopen(path, "r");
    int n = 0, v;
    *last = 0;
    while (f && fscanf(f, "%d", &v) == 1) {
        n++;
        *last = v;
    }
    if (f)
        fclose(f);
    return n;
}

/* A take in the background writing its file, which waits out SIGTERM
   (here: ignores it), doesn't hold up a share: the share's start returns
   at once, its poll says it hasn't ended until the background one is
   killed TAKE_STOP_WAIT (3 s) on, and then the share's runs, and the
   background one starts again after it. A second share meanwhile is
   refused. */
TEST(run_a_share_doesnt_wait_for_a_take_in_the_background) {
    char dir[1024], bg[1100], share[1100], rec[1100], copy[1100], started[1100], err[256];
    test_tmp_dir(dir, sizeof dir);
    snprintf(started, sizeof started, "%s/started", dir);
    char body[1300];
    snprintf(body, sizeof body, "trap '' TERM\necho $$ >> '%s'\nexec sleep 30", started);
    fake_take(bg, sizeof bg, dir, "background", body);
    fake_take(share, sizeof share, dir, "share", "exit 0");
    snprintf(rec, sizeof rec, "%s/game.lwreplay", dir);
    snprintf(copy, sizeof copy, "%s/shared.lwreplay", dir);
    fclose(fopen(rec, "w"));
    bool queued = gamestart_take_later(bg, rec, GAMESTART_MIN_PRELUDE_US);
    for (int i = 0; i < 500 && access(started, F_OK) != 0; i++)
        usleep(10000);
    double t0 = wall_seconds();
    bool started_share = gamestart_take_start(share, copy, err, sizeof err);
    double start_took = wall_seconds() - t0;
    bool second = gamestart_take_start(share, copy, err, sizeof err);
    bool ended = false, kept = true;
    for (int i = 0; started_share && i < 200 && !(ended = gamestart_take_poll(&kept)); i++)
        usleep(50000);
    double poll_took = wall_seconds() - t0;
    gamestart_tend(); /* the background one again */
    int last, before = lines_of(started, &last);
    for (int i = 0; i < 500 && lines_of(started, &last) == before; i++)
        usleep(10000);
    int runs = lines_of(started, &last);
    t0 = wall_seconds();
    gamestart_take_end();
    double end_took = wall_seconds() - t0;
    bool gone = last > 0 && kill(last, 0) != 0 && errno == ESRCH;
    test_remove_tree(dir);
    CHECK(queued);
    CHECK(started_share);
    CHECK(start_took < 0.5);
    CHECK(!second);
    CHECK_CONTAINS(err, "being taken already");
    CHECK(ended);
    CHECK(!kept);
    CHECK(poll_took > 2.5);
    CHECK_EQ(runs, 2);
    CHECK(end_took > 2.5); /* at exit, the app waits out a write */
    CHECK(gone);
}

/* A share cancelled while it waits for a take in the background to stop
   doesn't wait either, and a share again after takes the background
   one's place once, not twice: once the background one has run again,
   none is left to start. Cancelling a share's take that has started
   kills it at once, and so does deleting a recording whose take runs. */
TEST(run_a_cancelled_share_doesnt_wait_for_a_take_in_the_background) {
    char dir[1024], bg[1100], share[1100], rec[1100], copy[1100], started[1100], err[256];
    test_tmp_dir(dir, sizeof dir);
    snprintf(started, sizeof started, "%s/started", dir);
    char body[1300];
    snprintf(body, sizeof body, "trap '' TERM\necho $$ >> '%s'\nexec sleep 30", started);
    fake_take(bg, sizeof bg, dir, "background", body);
    fake_take(share, sizeof share, dir, "share", "exit 0");
    snprintf(rec, sizeof rec, "%s/game.lwreplay", dir);
    snprintf(copy, sizeof copy, "%s/shared.lwreplay", dir);
    fclose(fopen(rec, "w"));
    bool queued = gamestart_take_later(bg, rec, GAMESTART_MIN_PRELUDE_US);
    for (int i = 0; i < 500 && access(started, F_OK) != 0; i++)
        usleep(10000);
    bool first = gamestart_take_start(share, copy, err, sizeof err);
    double t0 = wall_seconds();
    gamestart_take_stop();
    double stop_took = wall_seconds() - t0;
    bool again = gamestart_take_start(share, copy, err, sizeof err), kept = true, ended = false;
    for (int i = 0; again && i < 200 && !(ended = gamestart_take_poll(&kept)); i++)
        usleep(50000);
    int last;
    for (int i = 0; i < 500 && lines_of(started, &last) < 2; i++) {
        gamestart_tend();
        usleep(10000);
    }
    /* as if it had ended, and collected (gone, not a zombie): the queue
       should hold nothing more */
    bool restarted = lines_of(started, &last) == 2 && last > 0;
    if (restarted)
        kill(last, SIGKILL);
    for (int i = 0; restarted && i < 500 && !(kill(last, 0) != 0 && errno == ESRCH); i++) {
        gamestart_tend();
        usleep(10000);
    }
    for (int i = 0; i < 50; i++) {
        gamestart_tend();
        usleep(10000);
    }
    int runs = lines_of(started, &last);
    /* the share's own take, cancelled once it has started */
    char writing[1100], wrote[1100];
    snprintf(wrote, sizeof wrote, "%s/writing.pid", dir);
    snprintf(body, sizeof body, "trap '' TERM\necho $$ >> '%s'\nexec sleep 30", wrote);
    fake_take(writing, sizeof writing, dir, "writing", body);
    bool third = gamestart_take_start(writing, copy, err, sizeof err);
    int writer = 0;
    for (int i = 0; third && i < 500 && lines_of(wrote, &writer) == 0; i++)
        usleep(10000);
    t0 = wall_seconds();
    gamestart_take_stop();
    double cancel_took = wall_seconds() - t0;
    bool cancelled = writer > 0 && kill(writer, 0) != 0 && errno == ESRCH;
    /* a deleted recording's take */
    bool fourth = gamestart_take_later(bg, rec, GAMESTART_MIN_PRELUDE_US);
    for (int i = 0; i < 500 && lines_of(started, &last) < 3; i++)
        usleep(10000);
    int runs3 = lines_of(started, &last);
    t0 = wall_seconds();
    gamestart_take_forget(rec);
    double forget_took = wall_seconds() - t0;
    bool gone = last > 0 && kill(last, 0) != 0 && errno == ESRCH;
    gamestart_take_end();
    test_remove_tree(dir);
    CHECK(queued);
    CHECK(first);
    CHECK(stop_took < 0.5);
    CHECK(again);
    CHECK(ended);
    CHECK(!kept);
    CHECK_EQ(runs, 2);
    CHECK(restarted);
    CHECK(third);
    CHECK(cancel_took < 0.5);
    CHECK(cancelled);
    CHECK(fourth);
    CHECK_EQ(runs3, 3);
    CHECK(forget_took < 0.5); /* a deleted recording's take is killed, not waited for */
    CHECK(gone);
}

/* A clip of a few seconds of a recording (clip.h): one frame a tick for
   exactly the span, in HD at 2x scaled to 1280x960, with the opening's
   music. */
TEST(run_a_clip_of_a_recording_is_written) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    char out[2][32768];
    int rec_status = record_for_clips(rec_dir, "330 quit\n", out[0], sizeof out[0]);
    int clip_status = clip_recording("0:01-0:04", play_dir, "clip.mp4", out[1], sizeof out[1]);
    mp4_info info;
    char err[256] = "";
    bool read = mp4_read_info(clip_path, &info, err, sizeof err);
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK_EQ(clip_status, 0);
    CHECK_CONTAINS(out[1], "loony: clip: reached 0:01");
    CHECK_CONTAINS(out[1], "1280x960 (scaled from 1600x1200) at 60 fps");
    CHECK_CONTAINS(out[1], "loony: clip: wrote 0:01-0:04 to");
    CHECK_CONTAINS(out[1], "180 frames");
    CHECK(!strstr(out[1], "desync"));
    if (!read)
        fprintf(stderr, "%s\n", err);
    CHECK(read);
    CHECK_EQ(info.width, 1280);
    CHECK_EQ(info.height, 960);
    CHECK_EQ(info.video_frames, 180);
    CHECK(info.video_seconds > 2.999 && info.video_seconds < 3.001);
    CHECK(info.audio_seconds > 2.97 && info.audio_seconds < 3.03);
    CHECK(info.peak > 0.1); /* the music */
}

/* A clip that runs past the end of the recording keeps what there is, with
   its sound (the writer can want sound from past the last frame: waiting
   for frames there once stalled), and one that starts past the end writes
   nothing and says so in its status (CLIP_NOTHING_STATUS). A failure
   before the clip's start keeps its own status. */
TEST(run_a_clip_past_the_end_of_a_recording_keeps_what_there_is) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    char out[4][32768];
    int rec_status = record_for_clips(rec_dir, "330 quit\n", out[0], sizeof out[0]);
    int past_status = clip_recording("0:03-0:20", play_dir, "past.mp4", out[1], sizeof out[1]);
    mp4_info info;
    char err[256] = "";
    bool read = mp4_read_info(clip_path, &info, err, sizeof err);
    int none_status = clip_recording("0:30-0:35", play_dir, "none.mp4", out[2], sizeof out[2]);
    bool none_written = access(clip_path, F_OK) != 0;
    /* A failure before the clip's start keeps its own status. */
    setenv("LOONY_SCRIPT", "/nonexistent/script", 1);
    int bad_status = clip_recording("0:01-0:02", play_dir, "bad.mp4", out[3], sizeof out[3]);
    unsetenv("LOONY_SCRIPT");
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK_EQ(past_status, 0);
    CHECK_CONTAINS(out[1], "loony: clip: the replay stopped at 0:");
    CHECK_CONTAINS(out[1], "before the clip's end at 0:20");
    CHECK_CONTAINS(out[1], "loony: clip: wrote 0:03-0:");
    if (!read)
        fprintf(stderr, "%s\n", err);
    CHECK(read);
    CHECK(info.video_frames > 100 && info.video_frames < 600); /* from 0:03 to the end, 5.5 s or so */
    CHECK(fabs(info.video_seconds - info.video_frames / 60.0) < 0.001);
    CHECK(fabs(info.audio_seconds - info.video_seconds) < 0.05);
    CHECK(info.peak > 0.1);
    CHECK_EQ(none_status, CLIP_NOTHING_STATUS);
    CHECK_CONTAINS(out[2], "where the clip starts: nothing written");
    CHECK(none_written);
    CHECK_EQ(bad_status, 1);
    CHECK_CONTAINS(out[3], "can't load the script");
}

/* The same when the player quits mid-game: the game quits on its own where
   the recording ends, so the replay finishes there instead of stepping past
   it, and the clip still keeps what there is. */
TEST(run_a_clip_past_the_end_of_a_game_quit_mid_play_keeps_what_there_is) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    char out[3][32768];
    int rec_status = record_for_clips(rec_dir,
                                      "1720 down esc\n1724 up esc\n1800 down esc\n1804 up esc\n"
                                      "1900 down return\n1906 up return\n2100 down return\n"
                                      "2190 up return\n2400 quit\n",
                                      out[0], sizeof out[0]);
    int past_status = clip_recording("0:01-0:20", play_dir, "past.mp4", out[1], sizeof out[1]);
    mp4_info info;
    char err[256] = "";
    bool read = mp4_read_info(clip_path, &info, err, sizeof err);
    int none_status = clip_recording("0:30-0:35", play_dir, "none.mp4", out[2], sizeof out[2]);
    bool none_written = access(clip_path, F_OK) != 0;
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK_CONTAINS(out[0], "loony: ExitToShell");
    CHECK_EQ(past_status, 0);
    CHECK_CONTAINS(out[1], "loony: replay: reached the end of the recording");
    CHECK(!strstr(out[1], "exiting where the recording stopped")); /* it finished there */
    CHECK_CONTAINS(out[1], "loony: clip: wrote 0:01-0:");
    if (!read)
        fprintf(stderr, "%s\n", err);
    CHECK(read);
    CHECK(info.video_frames > 60);
    CHECK_EQ(none_status, CLIP_NOTHING_STATUS);
    CHECK_CONTAINS(out[2], "where the clip starts: nothing written");
    CHECK(none_written);
}

/* The files in dir ending in suffix: those starting with a dot (a clip
   not yet finished) if hidden, else the others. */
static int files_in(const char *dir, const char *suffix, bool hidden) {
    DIR *d = opendir(dir);
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t len = strlen(e->d_name), k = strlen(suffix);
        if (len > k && strcmp(e->d_name + len - k, suffix) == 0 && (e->d_name[0] == '.') == hidden)
            n++;
    }
    if (d)
        closedir(d);
    return n;
}

/* A clip cut short with the recording still going on (here
   LOONY_EXIT_AFTER; for the user, a desync) is thrown away, not left under
   its name, so the next export renders it instead of skipping it. */
TEST(run_a_clip_cut_short_is_thrown_away_and_rendered_again) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024], list[1100], file[1100], span[64], err[256] = "";
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    char out[4][32768];
    int rec_status = record_for_clips(rec_dir, "330 quit\n", out[0], sizeof out[0]);
    snprintf(list, sizeof list, "%s/game.clips", rec_dir);
    FILE *f = fopen(list, "w");
    fputs("0:01-0:04\n0:30-0:35\n", f); /* the second past the recording's end */
    fclose(f);
    clip_span_name(60, 240, span, sizeof span);
    snprintf(file, sizeof file, "%s/game-%s.mp4", rec_dir, span);
    setenv("LOONY_EXIT_AFTER", "150", 1);
    int short_status = clip_recording("clips", play_dir, NULL, out[1], sizeof out[1]);
    unsetenv("LOONY_EXIT_AFTER");
    bool short_written = access(file, F_OK) == 0;
    int short_unfinished = files_in(rec_dir, ".mp4", true);
    int again_status = clip_recording("clips", play_dir, NULL, out[2], sizeof out[2]);
    mp4_info info;
    bool read = mp4_read_info(file, &info, err, sizeof err);
    int unfinished = files_in(rec_dir, ".mp4", true);
    /* Once more: the first is skipped, and the export still isn't one that
       wrote nothing. */
    int skip_status = clip_recording("clips", play_dir, NULL, out[3], sizeof out[3]);
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK_EQ(short_status, 1);
    CHECK_CONTAINS(out[1], "loony: exiting after 150 ticks (LOONY_EXIT_AFTER)");
    CHECK_CONTAINS(out[1], "isn't written: the recording goes on past where the replay stopped");
    CHECK(!short_written);
    CHECK_EQ(short_unfinished, 0);
    CHECK_EQ(again_status, 0);
    CHECK(!strstr(out[2], "already"));
    if (!read)
        fprintf(stderr, "%s: %s\n", file, err);
    CHECK(read);
    CHECK_EQ(info.video_frames, 180);
    CHECK_EQ(unfinished, 0);
    CHECK_CONTAINS(out[2], "where the clip starts: nothing written");
    CHECK_EQ(skip_status, 0);
    CHECK_CONTAINS(out[3], "is in ");
    CHECK_CONTAINS(out[3], "already: skipped");
    CHECK_CONTAINS(out[3], "where the clip starts: nothing written");
}

/* The tick of mark n (from 1) where a run logged "<who>: mark <n> at tick",
   or 0. */
static unsigned mark_tick(const char *out, const char *who, int n) {
    char what[64];
    snprintf(what, sizeof what, "%s: mark %d at tick ", who, n);
    const char *at = strstr(out, what);
    unsigned tick = 0;
    return at && sscanf(at + strlen(what), "%u", &tick) == 1 ? tick : 0;
}

/* Marks made while recording (Cmd-S; here the script's) become clips: the
   export finds each at the tick it was made, before the replay reaches it,
   and writes one clip per mark, named after the replay and its span, from
   a second before the mark to half a second after it. */
TEST(run_marks_in_a_recording_become_clips) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024], files[2][1100], err[256] = "";
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    char out[2][32768];
    int rec_status = record_for_clips(rec_dir, "100 mark\n280 mark\n330 quit\n", out[0], sizeof out[0]);
    clip_before = "1";
    clip_after = "0.5";
    int clip_status = clip_recording("marks", play_dir, NULL, out[1], sizeof out[1]);
    clip_before = clip_after = NULL;
    unsigned ticks[2] = {mark_tick(out[0], "replay", 1), mark_tick(out[0], "replay", 2)};
    mp4_info info[2];
    bool read[2] = {false, false};
    for (int i = 0; i < 2; i++) {
        char span[64];
        clip_span_name(ticks[i] - 60, ticks[i] + 30, span, sizeof span);
        snprintf(files[i], sizeof files[i], "%s/game-%s.mp4", rec_dir, span);
        if (!(read[i] = mp4_read_info(files[i], &info[i], err, sizeof err)))
            fprintf(stderr, "%s: %s\n", files[i], err);
    }
    int clips = files_in(rec_dir, ".mp4", false), unfinished = files_in(rec_dir, ".mp4", true);
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK_EQ(clip_status, 0);
    CHECK(ticks[0] >= 100 && ticks[0] < 110);
    CHECK(ticks[1] >= 280 && ticks[1] < 290);
    for (int i = 0; i < 2; i++) {
        CHECK_EQ(mark_tick(out[1], "clip", i + 1), ticks[i]);   /* found ahead */
        CHECK_EQ(mark_tick(out[1], "replay", i + 1), ticks[i]); /* and reached */
        CHECK_CONTAINS(out[1], files[i]);
        CHECK(read[i]);
        CHECK_EQ(info[i].video_frames, 90);
        CHECK(fabs(info[i].audio_seconds - 1.5) < 0.03);
    }
    CHECK_EQ(clips, 2);
    CHECK_EQ(unfinished, 0);
    CHECK(!strstr(out[1], "desync"));
}

static void run_loony_clipping_with_progress(void *unused) {
    setenv("LOONY_CLIP_PROGRESS", "1", 1);
    run_loony_clipping(unused);
}

/* Appends what fd has to buf (NUL-terminated, cut at cap - 1): false at
   its end. */
static bool read_into(int fd, char *buf, size_t cap, size_t *len) {
    char tmp[4096];
    ssize_t n = read(fd, tmp, sizeof tmp);
    if (n <= 0)
        return false;
    size_t take = (size_t)n < cap - 1 - *len ? (size_t)n : cap - 1 - *len;
    memcpy(buf + *len, tmp, take);
    *len += take;
    buf[*len] = '\0';
    return true;
}

/* clip_recording with LOONY_CLIP_PROGRESS=1: its stdout (the progress) in
   progress and its stderr in out. With stop_at, the export gets SIGTERM
   once a progress line starts with it, and *hidden is then the number of
   unfinished clips in hidden_dir. The status, or -1 if a signal killed it
   (so 143 is the export's own exit after SIGTERM). */
static int clip_with_progress(const char *span, const char *play_dir, const char *stop_at,
                              const char *hidden_dir, int *hidden, char *progress, size_t pcap,
                              char *out, size_t cap) {
    clip_spec = span;
    clip_path[0] = '\0';
    snprintf(run_data, sizeof run_data, "%s/data-progress", play_dir);
    int o[2], e[2];
    if (pipe(o) != 0 || pipe(e) != 0)
        return -2;
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(o[1], STDOUT_FILENO);
        dup2(e[1], STDERR_FILENO);
        close(o[0]);
        close(o[1]);
        close(e[0]);
        close(e[1]);
        run_loony_clipping_with_progress(NULL);
    }
    close(o[1]);
    close(e[1]);
    progress[0] = out[0] = '\0';
    size_t plen = 0, elen = 0;
    bool stopped = false;
    struct pollfd fds[2] = {{.fd = o[0], .events = POLLIN}, {.fd = e[0], .events = POLLIN}};
    while (fds[0].fd >= 0 || fds[1].fd >= 0) {
        if (poll(fds, 2, -1) < 0)
            break;
        for (int i = 0; i < 2; i++) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP)))
                continue;
            bool more = i ? read_into(fds[i].fd, out, cap, &elen) : read_into(fds[i].fd, progress, pcap, &plen);
            if (!more) {
                close(fds[i].fd);
                fds[i].fd = -1;
            }
        }
        if (stop_at && !stopped) {
            char line[128];
            snprintf(line, sizeof line, "\n%s", stop_at);
            if (strncmp(progress, stop_at, strlen(stop_at)) == 0 || strstr(progress, line)) {
                *hidden = files_in(hidden_dir, ".mp4", true);
                kill(pid, SIGTERM);
                stopped = true;
            }
        }
    }
    int status = 0;
    waitpid(pid, &status, 0);
    run_data[0] = '\0';
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Whether each line of progress is one LOONY_CLIP_PROGRESS writes. */
static bool progress_lines_are_whole(const char *progress) {
    for (const char *p = progress; *p;) {
        const char *nl = strchr(p, '\n');
        if (!nl)
            return false;
        char line[1200], word[16];
        unsigned a, b, c, d;
        size_t len = (size_t)(nl - p) < sizeof line - 1 ? (size_t)(nl - p) : sizeof line - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        bool ok = (sscanf(line, "seek %u %u%15s", &a, &b, word) == 2) ||
                  (sscanf(line, "clip %u %u %u %u%15s", &a, &b, &c, &d, word) == 4 && a >= 1 && a <= b &&
                   c <= d) ||
                  strncmp(line, "skipped /", 9) == 0 || strncmp(line, "wrote /", 7) == 0 ||
                  strncmp(line, "error ", 6) == 0;
        if (!ok) {
            fprintf(stderr, "not a progress line: \"%s\"\n", line);
            return false;
        }
        p = nl + 1;
    }
    return true;
}

/* LOONY_CLIP_PROGRESS=1 tells the program running an export what it is
   doing, on stdout: why it couldn't start, the seek, each clip's frames and
   file, and, exporting again, the clips it skips. */
TEST(run_an_export_reports_its_progress) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024], list[1100], files[2][1100], want[6][2400];
    static char progress[3][65536], out[4][32768];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    int rec_status = record_for_clips(rec_dir, "330 quit\n", out[0], sizeof out[0]);
    int no_list_status = clip_with_progress("clips", play_dir, NULL, NULL, NULL, progress[0],
                                            sizeof progress[0], out[1], sizeof out[1]);
    snprintf(list, sizeof list, "%s/game.clips", rec_dir);
    FILE *f = fopen(list, "w");
    fputs("0:01-0:02\n0:03-0:04\n", f);
    fclose(f);
    snprintf(files[0], sizeof files[0], "%s/game-0m01-0m02.mp4", rec_dir);
    snprintf(files[1], sizeof files[1], "%s/game-0m03-0m04.mp4", rec_dir);
    int status = clip_with_progress("clips", play_dir, NULL, NULL, NULL, progress[1], sizeof progress[1],
                                    out[2], sizeof out[2]);
    int again_status = clip_with_progress("clips", play_dir, NULL, NULL, NULL, progress[2],
                                          sizeof progress[2], out[3], sizeof out[3]);
    int clips = files_in(rec_dir, ".mp4", false);
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    snprintf(want[0], sizeof want[0], "error can't read the clips of %s/game.lwreplay from %s", rec_dir, list);
    snprintf(want[1], sizeof want[1], "clip 1 2 0 60\n");
    snprintf(want[2], sizeof want[2], "clip 1 2 60 60\nwrote %s\n", files[0]);
    snprintf(want[3], sizeof want[3], "clip 2 2 0 60\n");
    snprintf(want[4], sizeof want[4], "clip 2 2 60 60\nwrote %s\n", files[1]);
    snprintf(want[5], sizeof want[5], "skipped %s\nskipped %s\n", files[0], files[1]);
    CHECK_EQ(rec_status, 0);
    CHECK_EQ(no_list_status, 1);
    CHECK_CONTAINS(progress[0], want[0]);
    CHECK(progress_lines_are_whole(progress[0]));
    CHECK_EQ(status, 0);
    CHECK(progress_lines_are_whole(progress[1]));
    CHECK(strncmp(progress[1], "seek ", 5) == 0);
    for (int i = 1; i <= 4; i++)
        CHECK_CONTAINS(progress[1], want[i]);
    CHECK(strstr(progress[1], want[2]) < strstr(progress[1], want[3]));
    CHECK_EQ(count_of(progress[1], "\nclip 1 2 "), 61); /* 0, then each frame's new percent */
    CHECK(!strstr(progress[1], "skipped"));
    CHECK(!strstr(out[2], "clip 1 2")); /* the log is as it was */
    CHECK_EQ(again_status, 0);
    CHECK_STR(progress[2], want[5]);
    CHECK_EQ(clips, 2);
}

/* An export stopped by SIGTERM (or SIGINT, SIGHUP) takes the clip it was
   writing with it, hidden file and all, and exits 128 plus the signal, so
   the next export renders that clip in full. */
TEST(run_an_export_stopped_by_a_signal_leaves_nothing_behind) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024], list[1100], file[1100], want[1300], err[256] = "";
    static char progress[2][65536], out[3][32768];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    int rec_status = record_for_clips(rec_dir, "330 quit\n", out[0], sizeof out[0]);
    snprintf(list, sizeof list, "%s/game.clips", rec_dir);
    FILE *f = fopen(list, "w");
    fputs("0:01-0:04\n", f);
    fclose(f);
    snprintf(file, sizeof file, "%s/game-0m01-0m04.mp4", rec_dir);
    int hidden_at_stop = -1;
    int stopped_status = clip_with_progress("clips", play_dir, "clip 1 1 90 ", rec_dir, &hidden_at_stop,
                                            progress[0], sizeof progress[0], out[1], sizeof out[1]);
    int hidden = files_in(rec_dir, ".mp4", true), written = files_in(rec_dir, ".mp4", false);
    int again_status = clip_with_progress("clips", play_dir, NULL, NULL, NULL, progress[1], sizeof progress[1],
                                          out[2], sizeof out[2]);
    mp4_info info;
    bool read = mp4_read_info(file, &info, err, sizeof err);
    int hidden_after = files_in(rec_dir, ".mp4", true);
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    snprintf(want, sizeof want, "clip 1 1 180 180\nwrote %s\n", file);
    CHECK_EQ(rec_status, 0);
    CHECK_EQ(stopped_status, 143);
    CHECK_EQ(hidden_at_stop, 1);
    CHECK(!strstr(progress[0], "wrote"));
    CHECK_EQ(hidden, 0);
    CHECK_EQ(written, 0);
    CHECK_EQ(again_status, 0);
    CHECK(!strstr(progress[1], "skipped"));
    CHECK_CONTAINS(progress[1], want);
    if (!read)
        fprintf(stderr, "%s: %s\n", file, err);
    CHECK(read);
    CHECK_EQ(info.video_frames, 180);
    CHECK_EQ(hidden_after, 0);
}

static const char *watch_speed; /* LOONY_WATCH_SPEED for run_loony_watching, unless NULL */

static void run_loony_watching(void *unused) {
    (void)unused;
    unsetenv("LOONY_AUTO_ALERTS");
    setenv("LOONY_REPLAY", replay_file, 1);
    setenv("LOONY_DATA_DIR", run_data, 1);
    setenv("LOONY_WATCH", "1", 1);
    setenv("LOONY_SCRIPT", script_path, 1);
    if (watch_speed)
        setenv("LOONY_WATCH_SPEED", watch_speed, 1);
    execl(LOONY_BIN, "loony", (char *)NULL);
    _exit(127);
}

/* Watches the recording (watch.h) with keys, a script, in a data folder of
   its own in play_dir: its status, and in *seconds how long it took. */
static int watch_recording(const char *keys, const char *play_dir, char *out, size_t cap,
                           double *seconds) {
    tmp_name(script_path, sizeof script_path, "script");
    FILE *f = fopen(script_path, "w");
    fputs(keys, f);
    fclose(f);
    snprintf(run_data, sizeof run_data, "%s/data-watch", play_dir);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int status = test_run_child(run_loony_watching, NULL, out, cap);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    *seconds = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    run_data[0] = '\0';
    unlink(script_path);
    return status;
}

/* Watching a recording at 1x keeps to the wall clock: never ahead of it,
   and never so far behind that it starts again from where it is. I and O
   save clips next to the replay, which LOONY_CLIP=clips then renders, each
   as long as it was marked. */
TEST(run_a_watched_replay_keeps_time_and_its_clips_render) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024], list[1100], err[256] = "";
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    char out[3][32768];
    double seconds;
    int rec_status = record_for_clips(rec_dir, "330 quit\n", out[0], sizeof out[0]);
    int watch_status = watch_recording("60 down i\n64 up i\n180 down o\n184 up o\n"
                                       "240 down i\n244 up i\n300 down o\n304 up o\n",
                                       play_dir, out[1], sizeof out[1], &seconds);
    snprintf(list, sizeof list, "%s/game.clips", rec_dir);
    size_t len;
    char *text = (char *)read_file(list, &len);
    clip_span *spans = NULL;
    size_t n = 0;
    bool parsed = text && clip_parse_list(text, &spans, &n, err, sizeof err);
    free(text);
    int clip_status = clip_recording("clips", play_dir, NULL, out[2], sizeof out[2]);
    mp4_info info[2];
    bool read[2] = {false, false};
    for (size_t i = 0; i < n && i < 2; i++) {
        char name[1100], span[64];
        clip_span_name(spans[i].start, spans[i].end, span, sizeof span);
        snprintf(name, sizeof name, "%s/game-%s.mp4", rec_dir, span);
        if (!(read[i] = mp4_read_info(name, &info[i], err, sizeof err)))
            fprintf(stderr, "%s: %s\n", name, err);
    }
    /* Again, with every clip written: nothing to render. */
    char again[4096];
    int again_status = clip_recording("clips", play_dir, NULL, again, sizeof again);
    int unfinished = files_in(rec_dir, ".mp4", true);
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK_EQ(watch_status, 0);
    CHECK(seconds > 5.0); /* the recording is 5.5 s, from launch to the quit */
    /* The stretch starts at 0:00, or later if a busy machine left the replay
       too far behind and it went on from there. */
    const char *last = strstr(out[1], "watch: the last stretch, ");
    double took = 0, ms = 1e9;
    CHECK(last && sscanf(last, "watch: the last stretch, %*s at 1x, took %lf s on the wall clock (%lf ms)",
                         &took, &ms) == 2);
    CHECK(took > 0.5);
    CHECK(ms > -1.0 && ms < WATCH_MAX_LAG * 1000);
    CHECK_CONTAINS(out[1], "watch: clip 1, 0:01");
    CHECK_CONTAINS(out[1], "watch: clip 2, 0:04");
    CHECK_CONTAINS(out[1], "loony: replay: reached the end of the recording");
    CHECK(!strstr(out[1], "replay: desync"));
    CHECK(parsed);
    CHECK_EQ(n, 2);
    uint32_t want[2][2] = {{60, 180}, {240, 300}};
    for (size_t i = 0; i < n && i < 2; i++) { /* a key can come a tick late */
        CHECK(spans[i].start >= want[i][0] && spans[i].start <= want[i][0] + 2);
        CHECK(spans[i].end >= want[i][1] && spans[i].end <= want[i][1] + 2);
    }
    CHECK_EQ(clip_status, 0);
    CHECK_CONTAINS(out[2], "loony: clip: 2 listed in");
    for (size_t i = 0; i < n && i < 2; i++) {
        CHECK(read[i]);
        CHECK_EQ(info[i].video_frames, spans[i].end - spans[i].start);
    }
    CHECK_EQ(unfinished, 0);
    CHECK_EQ(again_status, 0);
    CHECK_CONTAINS(again, "loony: clip: all 2 clips are written already");
    free(spans);
}

static const recording *listed; /* run_loony_watching_listed's recording */

/* Watches the listed recording from its first mark, as the Replays menu's
   "Play from" item does (main.c's open_replay_at), in run_data. */
static void run_loony_watching_listed(void *unused) {
    (void)unused;
    unsetenv("LOONY_AUTO_ALERTS");
    recordings_watch_env(listed->path, listed->marks[0]);
    setenv("LOONY_DATA_DIR", run_data, 1); /* in place of a temporary folder */
    execl(LOONY_BIN, "loony", (char *)NULL);
    _exit(127);
}

/* A game marked earlier is in the list of recordings, its marks in play
   time, and watching it from a mark goes there first and plays on from
   there to the end. */
TEST(run_a_listed_recording_plays_from_its_mark) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    char out[2][32768];
    int rec_status = record_for_clips(rec_dir, "100 mark\n280 mark\n330 quit\n", out[0], sizeof out[0]);
    const recording *list = NULL;
    size_t n = recordings_list(rec_dir, &list);
    int watch_status = -1;
    if (n == 1 && list[0].nmarks == 2) {
        listed = &list[0];
        snprintf(run_data, sizeof run_data, "%s/data-watch", play_dir);
        watch_status = test_run_child(run_loony_watching_listed, NULL, out[1], sizeof out[1]);
        run_data[0] = '\0';
    }
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK_EQ(n, 1);
    if (n != 1)
        return;
    CHECK_EQ(list[0].nmarks, 2);
    CHECK(list[0].length >= 330 && list[0].length < 1200); /* launch to the quit, and its end */
    /* The opening isn't a game, so it is all play: the marks are where they
       were made. */
    unsigned made = mark_tick(out[0], "replay", 1);
    CHECK(made >= 100 && made < 110);
    if (list[0].nmarks == 2) {
        CHECK_EQ(list[0].marks[0], made);
        CHECK_EQ(list[0].marks[1], mark_tick(out[0], "replay", 2));
    }
    CHECK_EQ(watch_status, 0);
    char want[64], t[32];
    snprintf(want, sizeof want, "watch: reached %s\n", clip_format_time(made, t, sizeof t));
    CHECK_CONTAINS(out[1], want);
    CHECK_CONTAINS(out[1], "loony: replay: reached the end of the recording");
    CHECK(!strstr(out[1], "replay: desync"));
    snprintf(want, sizeof want, "watch: the last stretch, %s-", t); /* watched from there */
    CHECK_CONTAINS(out[1], want);
    recordings_forget();
}

/* L goes ahead without pacing; J, and then Left, go back by putting back a
   snapshot in place (seek.h), without a restart, keeping the speed, and
   play on from there to the end without a desync. */
TEST(run_a_watched_replay_goes_ahead_and_back) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    char out[3][32768];
    double ahead_seconds, back_seconds;
    int rec_status = record_for_clips(rec_dir, "330 quit\n", out[0], sizeof out[0]);
    int ahead_status = watch_recording("60 down l\n64 up l\n", play_dir, out[1], sizeof out[1],
                                       &ahead_seconds);
    test_remove_tree(play_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    int back_status = watch_recording("30 down lshift\n30 down period\n34 up period\n34 up lshift\n"
                                      "150 down j\n154 up j\n240 down left\n244 up left\n", /* > for 2x */
                                      play_dir, out[2], sizeof out[2], &back_seconds);
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK_EQ(ahead_status, 0);
    CHECK_CONTAINS(out[1], "watch: going to 0:11");
    CHECK_CONTAINS(out[1], "loony: replay: reached the end of the recording");
    CHECK(ahead_seconds < 5.0); /* not paced past 0:01 */
    CHECK_EQ(back_status, 0);
    CHECK_CONTAINS(out[2], "watch: 2x");
    const char *back = strstr(out[2], "watch: back to 0:00\n");
    CHECK(back);
    if (back) {
        CHECK_CONTAINS(back, "in place, in ");
        CHECK_CONTAINS(back, "watch: reached 0:00");
        CHECK_CONTAINS(back, "loony: replay: reached the end of the recording");
        const char *left = strstr(back + 1, "watch: back to 0:00\n"); /* Left from 0:04 */
        CHECK(left);
        CHECK(left && strstr(left, "in place, in "));
        CHECK(left && !strstr(left + 1, "watch: back to")); /* nor do the script's keys come again */
    }
    CHECK(!strstr(out[2], "restarting"));
    CHECK(!strstr(out[2], "replay: desync"));
}

/* Paused at 1 s (headless, the script's keys come at once while paused):
   period goes a frame ahead twice, comma a frame back through a seek, 5 to
   the middle of the recording, 0 back to its start (as near as its first
   snapshot, 0.4 s in, since play starts at launch here), and Space plays on
   to the end without a desync. */
TEST(run_a_watched_replay_steps_frames_and_goes_to_tenths) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    char out[2][32768], err[256] = "";
    double seconds;
    int rec_status = record_for_clips(rec_dir, "330 quit\n", out[0], sizeof out[0]);
    replay_summary s = {0};
    bool summed = replay_summarize(replay_file, &s, err, sizeof err);
    setenv("LOONY_WATCH_FROM", "60", 1);
    setenv("LOONY_WATCH_PAUSED", "1", 1);
    /* past the recording's end, so each comes while paused and none as the
       clock passes it */
    int watch_status = watch_recording("1000 down period\n1001 up period\n1002 down period\n1003 up period\n"
                                       "1004 down comma\n1005 up comma\n1006 down 5\n1007 up 5\n"
                                       "1008 down 0\n1009 up 0\n1010 down space\n1011 up space\n",
                                       play_dir, out[1], sizeof out[1], &seconds);
    unsetenv("LOONY_WATCH_FROM");
    unsetenv("LOONY_WATCH_PAUSED");
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK(summed);
    CHECK_EQ(watch_status, 0);
    char half[64], t[32];
    snprintf(half, sizeof half, "watch: going to %s\n", clip_format_time(s.play_ticks / 2, t, sizeof t));
    replay_summary_free(&s);
    const char *at = strstr(out[1], "watch: reached 0:01\n");
    CHECK(at);
    const char *steps[] = {"watch: going to 0:01.01\n", "watch: going to 0:01.03\n",
                           "watch: back to 0:01.01\n", half, "watch: back to 0:00\n",
                           "watch: going on at "};
    for (size_t i = 0; at && i < sizeof steps / sizeof steps[0]; i++) {
        at = strstr(at, steps[i]);
        CHECK(at);
    }
    CHECK_CONTAINS(out[1], "loony: replay: reached the end of the recording");
    CHECK(!strstr(out[1], "restarting"));
    CHECK(!strstr(out[1], "replay: desync"));
}

/* In a game, where the game reads the clock once a tick, period lands on
   the frame it goes to, not the one after: each goes a tick past the last.
   Comma at the end goes a frame back from it, and headless, a viewer paused
   with no keys left in its script goes on rather than wait forever. */
TEST(run_a_watched_game_steps_one_frame_at_a_time) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    char out[2][32768];
    int rec_status = record_for_clips(rec_dir,
                                      "1720 down esc\n1724 up esc\n1800 down esc\n1804 up esc\n"
                                      "1900 down return\n1906 up return\n2000 down return\n"
                                      "2006 up return\n2100 down return\n2180 up return\n"
                                      "2300 down z\n2310 up z\n2400 down slash\n2410 up slash\n"
                                      "2900 quit\n",
                                      out[0], sizeof out[0]);
    replay_summary s = {0};
    char err[256] = "";
    bool summed = replay_summarize(replay_file, &s, err, sizeof err);
    setenv("LOONY_WATCH_FROM", "300", 1);
    setenv("LOONY_WATCH_PAUSED", "1", 1);
    watch_speed = "0";
    double seconds;
    int watch_status = watch_recording("100000 down period\n100001 up period\n"
                                       "100002 down period\n100003 up period\n"
                                       "100004 down period\n100005 up period\n"
                                       "100006 down space\n100007 up space\n"
                                       "100008 down comma\n100009 up comma\n",
                                       play_dir, out[1], sizeof out[1], &seconds);
    watch_speed = NULL;
    unsetenv("LOONY_WATCH_FROM");
    unsetenv("LOONY_WATCH_PAUSED");
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK(summed);
    CHECK_EQ(watch_status, 0);
    char before_end[64], t[32];
    snprintf(before_end, sizeof before_end, "watch: back to %s\n",
             clip_format_time(s.play_ticks - 1, t, sizeof t));
    replay_summary_free(&s);
    const char *at = strstr(out[1], "watch: reached 0:05\n");
    CHECK(at);
    for (uint32_t tick = 301; at && tick <= 303; tick++) {
        char want[64];
        snprintf(want, sizeof want, "watch: going to %s\n", clip_format_time(tick, t, sizeof t));
        at = strstr(at, want);
        CHECK(at);
    }
    /* comma at the end: a frame back, and paused there with the script
       spent, it goes on again to the end */
    at = at ? strstr(at, before_end) : NULL;
    CHECK(at);
    CHECK(strstr(at, "watch: going on at "));
    CHECK(strstr(at, "replay: reached the end of the recording"));
    CHECK(!strstr(out[1], "replay: desync"));
}

/* J while going: the unpaced stretch so far is logged as such, not as a
   paced one, before the seek. */
TEST(run_a_watched_replay_logs_the_stretch_when_it_goes_back_while_going) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    char out[2][32768];
    double seconds;
    int rec_status = record_for_clips(rec_dir, "330 quit\n", out[0], sizeof out[0]);
    int status = watch_recording("60 down l\n64 up l\n100 down j\n104 up j\n", play_dir, out[1],
                                 sizeof out[1], &seconds);
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out[1], "watch: going to 0:11");
    CHECK_CONTAINS(out[1], "watch: the last stretch, unpaced: ");
    const char *back = strstr(out[1], "watch: back to 0:01\n");
    const char *paced = strstr(out[1], "at 1x, took"); /* the run after the seek */
    CHECK(back);
    CHECK(!paced || paced > back); /* the going wasn't logged as a paced stretch */
    CHECK_CONTAINS(out[1], "loony: replay: reached the end of the recording");
    CHECK(!strstr(out[1], "replay: desync"));
}

/* Watches the recording going to a time past its end, in HD, with the seek
   engine's measurements on (measure.h) and the copy-on-write spike at 2 s. */
static void run_loony_watching_measured(void *unused) {
    (void)unused;
    unsetenv("LOONY_AUTO_ALERTS");
    setenv("LOONY_REPLAY", replay_file, 1);
    setenv("LOONY_DATA_DIR", run_data, 1);
    setenv("LOONY_WATCH", "1", 1);
    setenv("LOONY_WATCH_FROM", "36000", 1);
    setenv("LOONY_HD", LOONY_SRC_DIR "/hd-art/loony-labyrinth", 1);
    setenv("LOONY_MEASURE", "1", 1);
    setenv("LOONY_MEASURE_COW", "2", 1);
    execl(LOONY_BIN, "loony", (char *)NULL);
    _exit(127);
}

/* Whether the first line of out that starts with prefix contains needle. */
static bool line_contains(const char *out, const char *prefix, const char *needle) {
    const char *line = strstr(out, prefix);
    if (!line)
        return false;
    const char *end = strchr(line, '\n'), *hit = strstr(line, needle);
    return hit && (!end || hit < end);
}

/* A replay going to a time logs how fast it went and what its output work
   cost; LOONY_MEASURE logs the sizes the seek engine is designed around,
   and LOONY_MEASURE_COW the copy-on-write spike's numbers, with the copies
   holding what the guest held. None of it changes the replay. */
TEST(run_a_watched_replay_logs_the_seek_engines_measurements) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    char out[2][32768];
    int rec_status = record_for_clips(rec_dir, "330 quit\n", out[0], sizeof out[0]);
    snprintf(run_data, sizeof run_data, "%s/data-watch", play_dir);
    int status = test_run_child(run_loony_watching_measured, NULL, out[1], sizeof out[1]);
    run_data[0] = '\0';
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out[1], "watch: going to 10:00");
    CHECK_CONTAINS(out[1], "watch: the stretch to the end, unpaced: ");
    CHECK_CONTAINS(out[1], " presents; HD sync ");
    CHECK_CONTAINS(out[1], "loony: replay: reached the end of the recording");
    CHECK_CONTAINS(out[1], "s of the game's clock in ");
    CHECK_CONTAINS(out[1], "measure: guest memory: ");
    CHECK_CONTAINS(out[1], " MB resident of ");
    CHECK_CONTAINS(out[1], "measure: a saved CPU context takes ");
    CHECK_CONTAINS(out[1], "measure: HD copies: ");
    CHECK_CONTAINS(out[1], "measure: trap entries by depth: 1: ");
    CHECK_CONTAINS(out[1], "measure: copy-on-write with mach_vm_remap: ");
    CHECK_CONTAINS(out[1], "measure: copy-on-write with vm_copy: ");
    /* Every resident page of each copy matched the source. */
    CHECK(line_contains(out[1], "measure: copy-on-write with mach_vm_remap: ", " (0 of "));
    CHECK(line_contains(out[1], "measure: copy-on-write with vm_copy: ", " (0 of "));
    CHECK(!strstr(out[1], "replay: desync"));
}

/* Records one game from the menu into rec_dir/game.lwreplay, with the soak
   script's keys until tick 7000 and none after, so the game ends by itself
   (at tick 8750 or so) and the recording ends between games at tick 10200:
   by quitting (quit), where Loony's main calls ExitToShell at depth 1, or
   with LOONY_EXIT_AFTER, inside a trap of the run loop. The recording's
   status. */
static int record_one_game(const char *rec_dir, bool quit, char *out, size_t cap) {
    char script[16384] = "";
    int t = 1700;
    const char *menu[4] = {"esc", "esc", "return", "return"};
    for (int i = 0; i < 4; i++, t += 90) {
        size_t len = strlen(script);
        snprintf(script + len, sizeof script - len, "%d down %s\n%d up %s\n", t, menu[i], t + 4,
                 menu[i]);
    }
    while (t < 7000) {
        size_t len = strlen(script);
        snprintf(script + len, sizeof script - len, "%d down return\n%d up return\n", t, t + 80);
        for (int i = 0, k = (t += 120); i < 12; i++, k += 25) {
            const char *key = i % 2 ? "slash" : "z";
            len = strlen(script);
            snprintf(script + len, sizeof script - len, "%d down %s\n%d up %s\n", k, key, k + 8,
                     key);
        }
        t += 12 * 25;
    }
    if (quit)
        strcat(script, "10200 quit\n");
    else
        setenv("LOONY_EXIT_AFTER", "10200", 1);
    int status = record_for_clips(rec_dir, script, out, cap);
    unsetenv("LOONY_EXIT_AFTER");
    return status;
}

/* Watches the recording as fast as it can, with keys, in a data folder of
   its own in play_dir, verifying its snapshots (seek.h): its status. */
static int watch_fastest(const char *keys, const char *play_dir, char *out, size_t cap) {
    double seconds;
    watch_speed = "0";
    setenv("LOONY_SNAPSHOT_VERIFY", "1", 1);
    int status = watch_recording(keys, play_dir, out, cap, &seconds);
    unsetenv("LOONY_SNAPSHOT_VERIFY");
    watch_speed = NULL;
    return status;
}

/* How many snapshots out's last verify line says were compared again
   after going back; -1 if there is none. */
static int verified_again(const char *out) {
    const char *line = NULL;
    for (const char *p = out; (p = strstr(p, "seek: verify: ")) && strstr(p, " snapshots the same ");
         p++)
        line = p;
    int taken, again;
    if (!line || sscanf(line, "seek: verify: %d snapshots the same put back where they were taken, %d",
                        &taken, &again) != 2)
        return -1;
    return again;
}

/* How many frames the first end of the recording in out says were
   checked; 0 if there is none. */
static unsigned frames_checked(const char *out) {
    const char *end = strstr(out, "reached the end of the recording at step");
    unsigned frames = 0;
    if (end)
        sscanf(end, "reached the end of the recording at step %*u (tick %*u), %u frames", &frames);
    return frames;
}

/* Whether out unwound to main as how says, put the snapshot back from
   there, and then reached the end of the recording with frames checked
   and no desync. */
static bool went_back_from_main(const char *out, const char *how) {
    const char *unwound = strstr(out, how);
    const char *back = unwound ? strstr(unwound, " from main, from ") : NULL;
    const char *end = back ? strstr(back, "loony: replay: reached the end of the recording") : NULL;
    return end && frames_checked(end) > 0 && !strstr(out, "replay: desync");
}

/* A watched game, as fast as it can, going back (seek.h): J three times in
   the game puts back a snapshot taken in its handler in place each time; J
   between games, while the replay runs in the event loop timer's callback,
   and J once the game has quit (ExitToShell, after the end: headless, the
   script's actions left come at once there) unwind to main and go back into
   the game from there. Each time the game plays on, ends by itself, hands
   back to the event loop, and the replay reaches its end with every frame
   check matching, as a straight replay's do. Verifying, each snapshot it
   comes to again after going back is what the emulator is there. The
   recording is on the real clock, so its game ends anywhere from about
   tick 9450 to 9550 and the recording a few to a hundred ticks after the
   quit at 10200: the J between games is at 9900, and the one after the
   end far past it. */
SLOW_TEST(run_a_watched_game_goes_back) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    static char out[4][65536];
    int rec_status = record_one_game(rec_dir, true, out[0], sizeof out[0]);
    const char *keys[3] = {"3000 down j\n3004 up j\n4500 down j\n4504 up j\n6000 down j\n6004 up j\n",
                           "9900 down j\n9904 up j\n", "20000 down j\n20001 up j\n"};
    int status[3];
    for (int i = 0; i < 3; i++) {
        char dir[1100];
        snprintf(dir, sizeof dir, "%s/%d", play_dir, i);
        mkdir(dir, 0700);
        status[i] = watch_fastest(keys[i], dir, out[i + 1], sizeof out[i + 1]);
    }
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    for (int i = 0; i < 3; i++)
        CHECK_EQ(status[i], 0);
    CHECK_EQ(count_of(out[1], "watch: back to "), 3);
    CHECK_EQ(count_of(out[1], "in place, in "), 3);
    CHECK(!strstr(out[1], "unwinding to main"));
    CHECK(frames_checked(out[1]) > 0);
    CHECK(!strstr(out[1], "replay: desync"));
    CHECK(went_back_from_main(out[2], "was taken in another frame, unwinding to main"));
    CHECK(went_back_from_main(out[3], "where the replay has ended: unwinding to main"));
    CHECK_EQ(count_of(out[3], "loony: ExitToShell\n"), 2); /* the end, and the end again */
    for (int i = 1; i <= 3; i++)
        CHECK(verified_again(out[i]) > 0);
}

/* Going back from the end of a recording that ends inside a trap of the
   run loop, between games, where the viewer holds the last frame. */
SLOW_TEST(run_a_watched_game_goes_back_from_the_end_of_the_recording) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    static char out[2][65536];
    int rec_status = record_one_game(rec_dir, false, out[0], sizeof out[0]);
    int status = watch_fastest("10300 down j\n10301 up j\n", play_dir, out[1], sizeof out[1]);
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK_EQ(status, 0);
    CHECK_EQ(count_of(out[1], "loony: replay: exiting where the recording stopped"), 1);
    CHECK(went_back_from_main(out[1], "where the replay has ended: unwinding to main"));
    CHECK(verified_again(out[1]) > 0);
}

static void run_loony_replaying_with_sound(void *wav) {
    setenv("LOONY_WAV", wav, 1);
    run_loony_replaying(NULL);
}

/* Replays the recording headless, not watched, in a data folder of its
   own in play_dir/name: its status; its last frame's and its sound's
   hashes in *shot and *wav. */
static int replay_hashed(const char *play_dir, const char *name, char *out, size_t cap, uint32_t *shot,
                         uint32_t *wav) {
    char wav_file[1100];
    snprintf(run_data, sizeof run_data, "%s/data-%s", play_dir, name);
    snprintf(replay_shot, sizeof replay_shot, "%s/%s.png", play_dir, name);
    snprintf(wav_file, sizeof wav_file, "%s/%s.wav", play_dir, name);
    int status = test_run_child(run_loony_replaying_with_sound, wav_file, out, cap);
    run_data[0] = '\0';
    size_t len = 0;
    uint8_t *png = read_file(replay_shot, &len), *sound = NULL;
    *shot = png ? fnv1a32(png, len) : 0;
    free(png);
    sound = read_file(wav_file, &len);
    *wav = sound ? fnv1a32(sound, len) : 0;
    free(sound);
    return status;
}

/* One of the replays replays_side_by_side runs: of the recording file, in
   a data folder of its own in play_dir named for tag, either replayed with
   sound as replay_hashed does (keys NULL), or watched with keys as
   watch_fastest does; with name set to value as well, unless name is
   NULL. */
typedef struct {
    const char *file, *tag, *keys, *name, *value;
    char data[1100], shot[1100], wav[1100], script[1100];
} side_replay;

static void run_side_replay(void *arg) {
    side_replay *r = arg;
    snprintf(replay_file, sizeof replay_file, "%s", r->file);
    snprintf(run_data, sizeof run_data, "%s", r->data);
    if (r->name)
        setenv(r->name, r->value, 1);
    if (r->keys) {
        snprintf(script_path, sizeof script_path, "%s", r->script);
        watch_speed = "0";
        setenv("LOONY_SNAPSHOT_VERIFY", "1", 1);
        run_loony_watching(NULL);
    } else {
        snprintf(replay_shot, sizeof replay_shot, "%s", r->shot);
        run_loony_replaying_with_sound(r->wav);
    }
}

/* Runs the n replays at once (test_run_children), since a slow test's
   replays are most of its time: each one's log in out[i], its status, and
   for one replayed with sound, its last frame's and its sound's hashes.
   Replays of one file side by side must not take a snapshot of the game's
   start into it (gamestart.h): give each a copy, or let one replay take
   it first. */
static void replays_side_by_side(const char *play_dir, side_replay *r, size_t n, char (*out)[65536],
                                 int *status, uint32_t *shot, uint32_t *wav) {
    void *args[8];
    char *outs[8];
    if (n > 8)
        n = 8;
    for (size_t i = 0; i < n; i++) {
        snprintf(r[i].data, sizeof r[i].data, "%s/data-%s", play_dir, r[i].tag);
        snprintf(r[i].shot, sizeof r[i].shot, "%s/%s.png", play_dir, r[i].tag);
        snprintf(r[i].wav, sizeof r[i].wav, "%s/%s.wav", play_dir, r[i].tag);
        r[i].script[0] = '\0';
        if (r[i].keys) {
            tmp_name(r[i].script, sizeof r[i].script, "script");
            FILE *f = fopen(r[i].script, "w");
            if (f) {
                fputs(r[i].keys, f);
                fclose(f);
            }
        }
        args[i] = &r[i];
        outs[i] = out[i];
    }
    test_run_children(n, run_side_replay, args, outs, sizeof out[0], status);
    for (size_t i = 0; i < n; i++) {
        if (r[i].script[0])
            unlink(r[i].script);
        if (r[i].keys)
            continue;
        size_t len = 0;
        uint8_t *png = read_file(r[i].shot, &len);
        shot[i] = png ? fnv1a32(png, len) : 0;
        free(png);
        uint8_t *sound = read_file(r[i].wav, &len);
        wav[i] = sound ? fnv1a32(sound, len) : 0;
        free(sound);
    }
}

/* A copy of the recording at path, as dir/name: its path in out. */
static bool copy_recording(const char *path, const char *dir, const char *name, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s.lwreplay", dir, name);
    return copyfile(path, out, NULL, COPYFILE_DATA) == 0;
}

/* A replay that takes snapshots, and puts each back the moment it is
   taken (LOONY_SNAPSHOT_VERIFY), plays exactly as one that takes none: the
   same frame checks, the same last frame, the same sound. A capture or a
   restore that changed anything the game sees would show here. */
SLOW_TEST(run_a_replay_with_snapshots_plays_as_one_without) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    static char out[3][65536];
    int rec_status = record_one_game(rec_dir, true, out[0], sizeof out[0]);
    uint32_t shot[2], wav[2];
    int status[2];
    /* At once, each of a copy of its own, so both play from launch. */
    char files[2][1200];
    bool copied = copy_recording(replay_file, rec_dir, "without", files[0], sizeof files[0]) &&
                  copy_recording(replay_file, rec_dir, "with", files[1], sizeof files[1]);
    side_replay r[2] = {{.file = files[0], .tag = "without"},
                        {.file = files[1], .tag = "with", .name = "LOONY_SNAPSHOT_VERIFY", .value = "1"}};
    replays_side_by_side(play_dir, r, 2, out + 1, status, shot, wav);
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK(copied);
    CHECK_EQ(status[0], 0);
    CHECK_EQ(status[1], 0);
    CHECK(!strstr(out[1], "seek: snapshot at"));
    CHECK(count_of(out[2], ", verified (") >= 30); /* every 5 s of 170 */
    CHECK_EQ(verified_again(out[2]), 0);
    for (int i = 1; i <= 2; i++) {
        CHECK_CONTAINS(out[i], "loony: replay: reached the end of the recording");
        CHECK(!strstr(out[i], "replay: desync"));
    }
    CHECK_EQ(frames_checked(out[2]), frames_checked(out[1]));
    CHECK(frames_checked(out[1]) > 250);
    CHECK(shot[0] != 0);
    CHECK_EQ(shot[1], shot[0]);
    CHECK(wav[0] != 0);
    CHECK_EQ(wav[1], wav[0]);
}

/* The size of the PNG at path, and its hash in *hash; 0 if there is none. */
static uint32_t png_width(const char *path, uint32_t *hash) {
    size_t len = 0;
    uint8_t *png = read_file(path, &len);
    uint32_t w = png && len > 24 ? rd_be32(png + 16) : 0;
    *hash = png ? fnv1a32(png, len) : 0;
    free(png);
    return w;
}

/* In HD, the picture after going back is the straight run's at the same
   tick: in the game, where the snapshot goes back in place and HD keeps
   its copies (Spike C), and at the end again after going back from the
   end, where it goes back from main. The recording ends a few to a
   hundred ticks after the quit at 10200, so the J after its end and the
   screenshot of the end are far past it, as in
   run_a_watched_game_goes_back. The straight run takes the snapshot of
   the game's start; the other two, at once, verify it (watch_fastest). */
SLOW_TEST(run_a_watched_game_in_hd_looks_the_same_after_going_back) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    static char out[4][65536];
    int rec_status = record_one_game(rec_dir, true, out[0], sizeof out[0]);
    char shots[4][1100], keys[3][2600];
    for (int i = 0; i < 4; i++)
        snprintf(shots[i], sizeof shots[i], "%s/shot%d.png", play_dir, i);
    /* Straight; back in the game; back from the end. */
    snprintf(keys[0], sizeof keys[0], "4600 screenshot %s\n20100 screenshot %s\n", shots[0], shots[1]);
    snprintf(keys[1], sizeof keys[1], "4500 down j\n4504 up j\n4600 screenshot %s\n", shots[2]);
    snprintf(keys[2], sizeof keys[2], "20000 down j\n20001 up j\n20100 screenshot %s\n", shots[3]);
    setenv("LOONY_HD", LOONY_SRC_DIR "/hd-art/loony-labyrinth", 1);
    int status[3];
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/0", play_dir);
    mkdir(dir, 0700);
    status[0] = watch_fastest(keys[0], dir, out[1], sizeof out[1]);
    side_replay r[2] = {{.file = replay_file, .tag = "in-the-game", .keys = keys[1]},
                        {.file = replay_file, .tag = "from-the-end", .keys = keys[2]}};
    uint32_t unused[2];
    replays_side_by_side(play_dir, r, 2, out + 2, status + 1, unused, unused);
    unsetenv("LOONY_HD");
    uint32_t hash[4], width[4];
    for (int i = 0; i < 4; i++)
        width[i] = png_width(shots[i], &hash[i]);
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(status[i], 0);
        CHECK(!strstr(out[i + 1], "replay: desync"));
    }
    for (int i = 0; i < 4; i++)
        CHECK_EQ(width[i], 3200);
    CHECK_CONTAINS(out[2], "in place, in ");
    CHECK(went_back_from_main(out[3], "where the replay has ended: unwinding to main"));
    CHECK(hash[0] != hash[1]);
    CHECK_EQ(hash[2], hash[0]); /* 4600, after going back in the game */
    CHECK_EQ(hash[3], hash[1]); /* the end, after going back from it */
    CHECK_CONTAINS(out[1], "gamestart: kept a snapshot of the game's start at ");
    for (int i = 2; i <= 3; i++) /* verifying, they play from launch to it */
        CHECK_CONTAINS(out[i], " the replay from launch is the recording's snapshot");
}

/* The file the first clip in rec_dir/game.clips is exported to, or "". */
static void first_clip_file(const char *rec_dir, char *out, size_t cap) {
    char list[1100], err[256];
    snprintf(list, sizeof list, "%s/game.clips", rec_dir);
    size_t len;
    char *text = (char *)read_file(list, &len);
    clip_span *spans = NULL;
    size_t n = 0;
    out[0] = '\0';
    if (text && clip_parse_list(text, &spans, &n, err, sizeof err) && n) {
        char span[64];
        clip_span_name(spans[0].start, spans[0].end, span, sizeof span);
        snprintf(out, cap, "%s/game-%s.mp4", rec_dir, span);
    }
    free(text);
    free(spans);
}

/* E exports the clips marked so far while the replay plays on, and the
   viewer waits for it at the end; Finder is shown the file. E again, with
   it written, says so. */
TEST(run_a_watched_replay_exports_its_clips) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024], file[1200], want[1300];
    static char out[3][65536];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    double seconds;
    int rec_status = record_for_clips(rec_dir, "330 quit\n", out[0], sizeof out[0]);
    int watch_status = watch_recording("30 down i\n34 up i\n90 down o\n94 up o\n100 down e\n104 up e\n",
                                       play_dir, out[1], sizeof out[1], &seconds);
    first_clip_file(rec_dir, file, sizeof file);
    mp4_info info;
    char err[256] = "";
    bool read = file[0] && mp4_read_info(file, &info, err, sizeof err);
    int unfinished = files_in(rec_dir, ".mp4", true);
    test_remove_tree(play_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    int again_status = watch_recording("30 down e\n34 up e\n", play_dir, out[2], sizeof out[2], &seconds);
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK_EQ(watch_status, 0);
    CHECK(file[0]);
    snprintf(want, sizeof want, "watch: exporting the clips of %s", replay_file);
    CHECK_CONTAINS(out[1], want);
    CHECK_CONTAINS(out[1], "watch: exported 1 clip");
    snprintf(want, sizeof want, "watch: revealing %s\n", file);
    CHECK_CONTAINS(out[1], want);
    if (!read)
        fprintf(stderr, "%s: %s\n", file, err);
    CHECK(read);
    CHECK_EQ(unfinished, 0);
    CHECK(!strstr(out[1], "replay: desync"));
    CHECK(!strstr(out[1], "not understood"));
    CHECK_EQ(again_status, 0);
    CHECK_CONTAINS(out[2], "watch: the 1 clip is exported already");
    CHECK_CONTAINS(out[2], want);
}

/* Esc cancels the export, here as it starts (an export of these clips
   takes about a second, too little to time a key inside the rendering;
   run_an_export_stopped_by_a_signal_leaves_nothing_behind stops one
   there): nothing is left. */
TEST(run_a_watched_replay_cancels_its_export) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024];
    static char out[2][65536];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    int rec_status = record_for_clips(rec_dir, "330 quit\n", out[0], sizeof out[0]);
    double seconds;
    int watch_status = watch_recording("30 down i\n34 up i\n150 down o\n154 up o\n160 down e\n161 up e\n"
                                       "162 down esc\n163 up esc\n",
                                       play_dir, out[1], sizeof out[1], &seconds);
    int clips = files_in(rec_dir, ".mp4", false), unfinished = files_in(rec_dir, ".mp4", true);
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK_EQ(watch_status, 0);
    CHECK_CONTAINS(out[1], "watch: cancelling the export");
    CHECK_CONTAINS(out[1], "watch: export cancelled");
    CHECK(!strstr(out[1], "watch: exported"));
    CHECK(!strstr(out[1], "watch: revealing"));
    CHECK_EQ(clips, 0);
    CHECK_EQ(unfinished, 0);
}

/* Going back doesn't stop the export: the viewer goes on following it to
   its end and shows the file. J comes as the export starts, long before it
   ends. */
TEST(run_a_watched_replay_keeps_exporting_when_it_goes_back) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024], file[1200], want[1300];
    static char out[2][65536];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    int rec_status = record_for_clips(rec_dir, "330 quit\n", out[0], sizeof out[0]);
    double seconds;
    int watch_status = watch_recording("30 down i\n34 up i\n150 down o\n154 up o\n160 down e\n161 up e\n"
                                       "168 down j\n169 up j\n",
                                       play_dir, out[1], sizeof out[1], &seconds);
    first_clip_file(rec_dir, file, sizeof file);
    bool written = file[0] && access(file, F_OK) == 0;
    int unfinished = files_in(rec_dir, ".mp4", true);
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK_EQ(watch_status, 0);
    const char *back = strstr(out[1], "watch: back to 0:00\n");
    CHECK(back);
    if (back) {
        CHECK_CONTAINS(back, "in place, in ");
        CHECK_CONTAINS(back, "watch: exported 1 clip");
        snprintf(want, sizeof want, "watch: revealing %s\n", file);
        CHECK_CONTAINS(back, want);
    }
    CHECK(!strstr(out[1], "restarting"));
    CHECK(written);
    CHECK_EQ(unfinished, 0);
    CHECK(!strstr(out[1], "replay: desync"));
}

static uint32_t le32_at(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The opening's music, recorded on the fixed clock: the same samples every
   run, in every build (the mixer is integer-only). The user listened to the
   golden recording and approved it on 2026-10-02. */
TEST(run_the_opening_music_is_recorded) {
    SKIP_UNLESS_GAME();
    tmp_name(wav_path, sizeof wav_path, "wav");
    char out[32768];
    setenv("LOONY_EXIT_AFTER", "300", 1); /* on the fixed clock, unlike a quit's grace period */
    int status = run_script("", NULL, NULL, 0, out, sizeof out);
    unsetenv("LOONY_EXIT_AFTER");
    size_t len = 0;
    uint8_t *wav = read_file(wav_path, &len);
    unlink(wav_path);
    wav_path[0] = '\0';
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "sound:"));
    CHECK(!strstr(out, "runtime error")); /* UBSan in Debug builds */
    CHECK(wav != NULL);
    CHECK(len > 44);
    CHECK_EQ(le32_at(wav + 24), 44100);
    uint32_t frames = le32_at(wav + 40) / 4;
    CHECK_EQ(frames, 300 * 44100 / 60); /* exactly the 5 s the run lasted */
    int peak = 0;
    for (uint32_t i = 0; i < 2 * 44100; i++) { /* the first second */
        int v = (int16_t)(wav[44 + 2 * i] | wav[45 + 2 * i] << 8);
        peak = v < 0 ? (-v > peak ? -v : peak) : (v > peak ? v : peak);
    }
    CHECK(peak > 8000);
    CHECK_EQ(fnv1a32(wav, len), 0x852682F2u);
    free(wav);
}

static char *read_text(const char *path) {
    size_t len = 0;
    char *t = (char *)read_file(path, &len);
    if (!t)
        return NULL;
    t = realloc(t, len + 1);
    t[len] = '\0';
    return t;
}

/* Spec success criterion 5: what the game keeps in its preferences (the
   high-score table, the options, the license) is saved when it quits and
   read at the next launch. The game rejects a high-score table it didn't
   write (it checks "highscore id"), so this test changes an option instead:
   with "switch music" off, the opening is silent. */
TEST(run_preferences_are_saved_at_quit_and_read_at_launch) {
    SKIP_UNLESS_GAME();
    test_tmp_dir(run_data, sizeof run_data);
    char prefs[1100];
    snprintf(prefs, sizeof prefs, "%s/prefs.plist", run_data);
    char out[32768];
    int status = run_script("300 quit\n", NULL, NULL, 0, out, sizeof out);
    char *xml = read_text(prefs);
    CHECK_EQ(status, 0);
    CHECK(xml != NULL);
    CHECK_CONTAINS(xml, "<key>highscore name 1</key>");
    CHECK_CONTAINS(xml, "<string>SNOWMAN</string>");
    const char *music_on = "<key>switch music</key>\n\t<integer>1</integer>";
    char *at = strstr(xml, music_on);
    CHECK(at != NULL);
    at[strlen(music_on) - strlen("1</integer>")] = '0';
    FILE *f = fopen(prefs, "w");
    fputs(xml, f);
    fclose(f);
    free(xml);

    tmp_name(wav_path, sizeof wav_path, "wav");
    setenv("LOONY_EXIT_AFTER", "300", 1);
    status = run_script("", NULL, NULL, 0, out, sizeof out);
    unsetenv("LOONY_EXIT_AFTER");
    size_t len = 0;
    uint8_t *wav = read_file(wav_path, &len);
    unlink(wav_path);
    wav_path[0] = '\0';
    test_remove_tree(run_data);
    run_data[0] = '\0';
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "preferences:"));
    CHECK(wav != NULL);
    CHECK_EQ(len, 44 + 300 * 44100 / 60 * 4);
    bool silent = true;
    for (size_t i = 44; i < len; i++)
        silent = silent && wav[i] == 0;
    CHECK(silent);
    free(wav);
}

/* Without LOONY_AUTO_ALERTS the shareware alerts wait for an answer. Alert
   901 sits at (139, 150); its "Enter Key-Code" button is at (399, 260). The
   registration form, DLOG 911, sits at (180, 130). The user approved these
   frames on 2026-10-02, after registering with their own key code. */
TEST(run_the_shareware_alerts_wait_for_an_answer) {
    SKIP_UNLESS_GAME();
    real_alerts = true;
    uint32_t ticks[1] = {30}, shots[1];
    char out[32768];
    int status = run_script("60 down return\n62 up return\n90 down return\n92 up return\n"
                            "400 quit\n",
                            ticks, shots, 1, out, sizeof out);
    real_alerts = false;
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: Alert 901: answered item 1 (Play Demo)");
    CHECK_CONTAINS(out, "loony: Alert 900: answered item 1 (OK)");
    CHECK(strstr(out, "Alert 900: answered") < strstr(out, "sending the quit Apple Event"));
    CHECK_EQ(shots[0], 0x94D533D8u); /* Alert 901 */
}

TEST(run_a_wrong_key_code_is_refused) {
    SKIP_UNLESS_GAME();
    real_alerts = true;
    uint32_t ticks[2] = {80, 110}, shots[2];
    char out[32768];
    int status = run_script("20 click 460 270\n"
                            "50 type nobody@example.com\n60 down tab\n61 up tab\n"
                            "70 type ABCD-1234-EFGH\n90 down return\n91 up return\n"
                            "120 down return\n121 up return\n"   /* 902's OK */
                            "140 down return\n141 up return\n"   /* 901: Play Demo */
                            "160 down return\n161 up return\n"   /* 900: OK */
                            "400 quit\n",
                            ticks, shots, 2, out, sizeof out);
    real_alerts = false;
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: Alert 901: answered item 4 (Enter Key-Code)");
    CHECK_CONTAINS(out, "loony: GetNewDialog 911");
    CHECK_CONTAINS(out, "loony: Alert 902: answered item 1 (OK)");
    CHECK(!strstr(out, "Alert 903"));
    CHECK(!strstr(out, "nobody@example.com")); /* typed text is never logged */
    CHECK_EQ(shots[0], 0x6E85582Au); /* the filled-in form */
    CHECK_EQ(shots[1], 0x8CF0D4FDu); /* Alert 902 */
}

/* The address and key code the registering tests type. No game would accept
   them: the unlock (src/unlock.c) drops each game's verdict, and each game
   still wants 24 digits. */
#define TEST_EMAIL "tester@example.com"
#define TEST_KEY "123456789012345678901234"

/* Registers with a made-up key code. The license is kept in the
   preferences, so the next launch skips the shareware alerts. */
TEST(run_a_key_code_registers_and_survives_a_relaunch) {
    SKIP_UNLESS_GAME();
    const char *email = TEST_EMAIL, *key = TEST_KEY;
    test_tmp_dir(run_data, sizeof run_data);
    real_alerts = true;
    char actions[1024];
    snprintf(actions, sizeof actions,
             "20 click 460 270\n50 type %s\n60 down tab\n61 up tab\n70 type %s\n"
             "90 down return\n91 up return\n120 down return\n121 up return\n600 quit\n",
             email, key);
    char out[32768];
    int status = run_script(actions, NULL, NULL, 0, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: Alert 903: answered item 1 (OK)");
    CHECK(!strstr(out, "Alert 900"));
    status = run_script("300 quit\n", NULL, NULL, 0, out, sizeof out);
    real_alerts = false;
    test_remove_tree(run_data);
    run_data[0] = '\0';
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "Alert 90"));
    CHECK(!strstr(out, email));
    CHECK(!strstr(out, key));
}

/* A run that ends without the game synchronizing (here LOONY_EXIT_AFTER;
   for the user, a quit during a sequence that outlasts the 3-second grace)
   still saves what the game set, as macOS would. */
TEST(run_preferences_are_saved_even_without_synchronize) {
    SKIP_UNLESS_GAME();
    test_tmp_dir(run_data, sizeof run_data);
    char prefs[1100];
    snprintf(prefs, sizeof prefs, "%s/prefs.plist", run_data);
    setenv("LOONY_EXIT_AFTER", "300", 1);
    char out[32768];
    int status = run_script("", NULL, NULL, 0, out, sizeof out);
    unsetenv("LOONY_EXIT_AFTER");
    char *xml = read_text(prefs);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "sending the quit Apple Event")); /* the game never quit */
    CHECK(xml != NULL);
    CHECK_CONTAINS(xml, "<string>SNOWMAN</string>");
    free(xml);
}

static void run_loony_bad_script(void *dir) {
    setenv("LOONY_SCRIPT", "/nonexistent/loony.script", 1);
    run_loony(dir);
}

TEST(run_reports_an_unreadable_script) {
    SKIP_UNLESS_GAME();
    char out[4096];
    CHECK_EQ(test_run_child(run_loony_bad_script, (void *)test_game_dir(), out, sizeof out), 1);
    CHECK_CONTAINS(out, "can't load the script /nonexistent/loony.script");
}

/* Review Focus 1: wrong or missing game folder. */
static void run_monster_fair_traced(void *unused) {
    (void)unused;
    setenv("LOONY_TRACE", "imports", 1);
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    setenv("LOONY_EXIT_AFTER", "120", 1);
    run_loony((void *)test_mf_app());
}

TEST(run_monster_fair_reaches_main) {
    SKIP_UNLESS_MF();
    test_tmp_dir(run_data, sizeof run_data);
    static char out[1 << 20];
    test_run_child(run_monster_fair_traced, NULL, out, sizeof out);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    CHECK_CONTAINS(out, "loony: playing MONSTER FAIR from ");
    CHECK_CONTAINS(out, "208 imports, main at 0x41ca8, 13 initializers");
    /* main's first calls: the system version, then Carbon's. */
    CHECK_CONTAINS(out, "Gestalt(0x73797376, ");
    CHECK_CONTAINS(out, "from code+0x41d1c");
    CHECK(!strstr(out, "the game threw"));
}

/* With no preferences, MONSTER FAIR opens with its Welcome window (from its
   nib) and waits in its modal loop. */
TEST(run_monster_fair_shows_the_welcome_window) {
    SKIP_UNLESS_MF();
    real_alerts = true;
    setenv("LOONY_EXIT_AFTER", "150", 1);
    uint32_t ticks[1] = {120}, shots[1];
    char out[32768];
    int status = run_script_in(test_mf_app(), "", ticks, shots, 1, out, sizeof out);
    unsetenv("LOONY_EXIT_AFTER");
    real_alerts = false;
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: nib window Welcome: 510x144, 6 controls");
    CHECK_CONTAINS(out, "loony: nib window Welcome: shown");
    CHECK(!strstr(out, "command '"));
    CHECK_EQ(shots[0], 0xCF4FD1C9u); /* the Welcome window, approved by the user on 2026-10-08 */
}

/* "Enter Key-Code" opens the Register window; a key code that isn't one
   gets AuthorizeFailed, whose OK goes back to the Welcome window. The
   address and key are made up. */
TEST(run_monster_fair_wrong_key_code_shows_authorize_failed) {
    SKIP_UNLESS_MF();
    real_alerts = true;
    setenv("LOONY_EXIT_AFTER", "400", 1);
    const char *actions = "60 click 456 265\n130 type someone@example.com\n140 down tab\n142 up tab\n"
                          "150 type 0000-1111-2222-3333\n180 down return\n182 up return\n"
                          "280 down return\n282 up return\n";
    char out[32768];
    int status = run_script_in(test_mf_app(), actions, NULL, NULL, 0, out, sizeof out);
    unsetenv("LOONY_EXIT_AFTER");
    real_alerts = false;
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: nib window Welcome: command 'Ans3' (Enter Key-Code)");
    CHECK_CONTAINS(out, "loony: nib window Register: shown");
    CHECK_CONTAINS(out, "loony: nib window Register: command 'ok  ' (Register)");
    CHECK_CONTAINS(out, "loony: nib window AuthorizeFailed: shown");
    CHECK_CONTAINS(out, "loony: nib window AuthorizeFailed: command 'ok  ' (OK)");
    const char *again = strstr(out, "AuthorizeFailed: command");
    CHECK(again && strstr(again, "loony: nib window Welcome: shown"));
    CHECK(!strstr(out, "0000-1111-2222-3333"));
}

/* Registers in run_data's save folder (which the caller made), then plays:
   the second license check runs at an Esc more than 7200 ticks after the
   first key the game sees, here at tick 9800. Returns the exit status;
   *xml gets the saved prefs.plist (to free), or NULL. */
static int mf_register_and_play_past_the_recheck(const char *email, const char *key, char *out, size_t outlen,
                                                 char **xml) {
    char prefs[1100];
    snprintf(prefs, sizeof prefs, "%s/prefs.plist", run_data);
    real_alerts = true;
    char actions[1024];
    snprintf(actions, sizeof actions,
             "60 click 456 265\n130 type %s\n140 down tab\n142 up tab\n150 type %s\n"
             "180 down return\n182 up return\n280 down return\n282 up return\n"
             "2400 down esc\n2404 up esc\n9800 down esc\n9804 up esc\n10000 quit\n",
             email, key);
    int status = run_script_in(test_mf_app(), actions, NULL, NULL, 0, out, outlen);
    real_alerts = false;
    *xml = read_text(prefs);
    return status;
}

/* With the second license check's verdict dropped (patch.h), the test key
   survives it: the game plays on until the script quits, keeps the license,
   and the next launch skips the Welcome window. */
SLOW_TEST(run_monster_fair_keeps_a_license_with_the_recheck_skipped) {
    SKIP_UNLESS_MF();
    const char *email = TEST_EMAIL, *key = TEST_KEY;
    test_tmp_dir(run_data, sizeof run_data);
    char out[32768], again[32768], *xml;
    int status = mf_register_and_play_past_the_recheck(email, key, out, sizeof out, &xml);
    setenv("LOONY_EXIT_AFTER", "300", 1);
    int status2 = run_script_in(test_mf_app(), "", NULL, NULL, 0, again, sizeof again);
    unsetenv("LOONY_EXIT_AFTER");
    test_remove_tree(run_data);
    run_data[0] = '\0';
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: MONSTER FAIR 1.2.5: its second license check is off");
    CHECK_CONTAINS(out, "loony: nib window ThankYou: shown");
    CHECK_CONTAINS(out, "loony: sending the quit Apple Event"); /* the script's quit, not the game's */
    CHECK_CONTAINS(out, "loony: main returned 0");
    CHECK(!strstr(out, "Alert 136"));
    CHECK(!strstr(out, email));
    CHECK(!strstr(out, key));
    CHECK(xml != NULL);
    CHECK(!strstr(xml, "<key>user email</key>\n\t<string></string>"));
    CHECK(!strstr(xml, "<key>user id</key>\n\t<string></string>"));
    free(xml);
    CHECK_EQ(status2, 0);
    CHECK(!strstr(again, "nib window Welcome"));
}

static void run_monster_fair_headless(void *unused) {
    (void)unused;
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    setenv("LOONY_EXIT_AFTER", "900", 1);
    setenv("LOONY_SCREENSHOT", shot, 1);
    run_loony((void *)test_mf_app());
}

/* Answering its windows automatically (Play Demo, then OK), MONSTER FAIR
   takes the display at 1024x768, 16 bits, and plays its opening: by tick
   900, the title. */
TEST(run_monster_fair_plays_its_opening_headless) {
    SKIP_UNLESS_MF();
    tmp_name(shot, sizeof shot, "run");
    char out[32768];
    test_tmp_dir(run_data, sizeof run_data);
    int status = test_run_child(run_monster_fair_headless, NULL, out, sizeof out);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    size_t len = 0;
    uint8_t *png = read_file(shot, &len);
    unlink(shot);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: nib window Welcome: command 'ok  ' (Play Demo)");
    CHECK_CONTAINS(out, "loony: nib window Demo: command 'ok  ' (OK)");
    CHECK_CONTAINS(out, "loony: display mode 1024x768, 16 bits");
    CHECK_CONTAINS(out, "loony: exiting after 900 ticks (LOONY_EXIT_AFTER)");
    CHECK(!strstr(out, "unknown selector"));
    CHECK(!strstr(out, "not supported"));
    CHECK(png != NULL);
    CHECK(len > 33);
    CHECK_EQ(rd_be32(png + 16), 1024);
    CHECK_EQ(rd_be32(png + 20), 768);
    uint32_t h = fnv1a32(png, len);
    free(png);
    CHECK_EQ(h, 0x3F03D118u); /* the title, "PRESS ESC TO START" */
}

/* Whether the line under the score, at (80, 344)-(166, 359), differs
   between two screenshots: "GAME OVER" while the table plays by itself,
   "Player 1 Ball 1" in a game. */
static bool status_line_differs(const char *a, const char *b) {
    cgimage_pixels pa, pb;
    char err[256];
    if (!cgimage_decode_png(a, &pa, err, sizeof err))
        fatal("%s", err);
    if (!cgimage_decode_png(b, &pb, err, sizeof err))
        fatal("%s", err);
    bool differs = false;
    for (int y = 344; y < 360 && !differs; y++)
        differs = memcmp(pa.xrgb + 4 * (y * pa.width + 80), pb.xrgb + 4 * (y * pb.width + 80), 4 * 87) != 0;
    free(pa.xrgb);
    free(pb.xrgb);
    return differs;
}

/* Esc opens the menu over the self-playing table, Return picks NEW GAME
   (the first item), and the plunger, held and released, serves the ball;
   then the game quits cleanly, giving the display back. */
TEST(run_monster_fair_starts_a_game) {
    SKIP_UNLESS_MF();
    char attract[1024], playing[1024], actions[4096];
    tmp_name(attract, sizeof attract, "shot");
    tmp_name(playing, sizeof playing, "shot");
    snprintf(actions, sizeof actions,
             "2100 screenshot %s\n2200 down esc\n2204 up esc\n2320 down return\n2326 up return\n"
             "2600 down return\n2700 up return\n2900 screenshot %s\n3000 quit\n",
             attract, playing);
    char out[32768];
    int status = run_script_in(test_mf_app(), actions, NULL, NULL, 0, out, sizeof out);
    bool differs = status_line_differs(attract, playing);
    char got[32] = "";
    size_t len;
    for (int i = 0; i < 2; i++) {
        uint8_t *png = read_file(i ? playing : attract, &len);
        size_t used = strlen(got);
        snprintf(got + used, sizeof got - used, "%s%08x", i ? " " : "", png ? fnv1a32(png, len) : 0);
        free(png);
    }
    unlink(attract);
    unlink(playing);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: display mode 1024x768, 16 bits");
    CHECK_CONTAINS(out, "loony: sending the quit Apple Event");
    CHECK_CONTAINS(out, "loony: display mode 800x600, 32 bits");
    CHECK_CONTAINS(out, "loony: main returned 0");
    CHECK(differs);
    /* The table playing by itself, "GAME OVER"; then ball 1 served. */
    CHECK_STR(got, "fa497979 e962c1a4");
}

/* Jinni Zeala has no demo to end: one Esc opens its menu, Up wraps to
   QUIT, and Return ends the game with ExitToShell, as the other classic
   games do (no quit Apple Event). The late quit ends a run whose menu
   missed QUIT, failing the test rather than hanging it. */
TEST(run_jinni_zeala_quits_from_its_own_menu) {
    SKIP_UNLESS_JZ();
    char out[32768];
    int status = run_script_in(test_jz_dir(), "1720 down esc\n1724 up esc\n1800 down up\n1804 up up\n"
                                              "1900 down return\n1906 up return\n2400 quit\n",
                               NULL, NULL, 0, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: ExitToShell");
    CHECK(!strstr(out, "quit Apple Event"));
}

/* Tristan's File menu has Quit on Cmd-Q, enabled while no game is in play:
   it ends the game with ExitToShell. The late quit ends a run whose Cmd-Q
   missed, and says so. */
TEST(run_tristan_quits_from_its_own_menu) {
    SKIP_UNLESS_TR();
    char out[32768];
    int status = run_script_in(test_tr_dir(), "700 down lcmd\n702 down q\n708 up q\n710 up lcmd\n2400 quit\n", NULL,
                               NULL, 0, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: ExitToShell");
    CHECK(!strstr(out, "sending the quit"));
}

/* Tristan's Register: "Enter your Key-Code" in Alert 901 (at (392, 238))
   opens DLOG 911, which the game shows with ShowWindow on its DialogRef.
   A key code too short for the game's own length test (the unlock drops the
   verdict, so a full 24 digits would certify), then Certify (at (494, 236)),
   ends at Alert 902, the bad key alert, and nothing is saved in the game's
   folder. The frame is
   the filled-in form, without the OK button DITL 911 keeps below the box. */
TEST(run_tristan_refuses_a_made_up_key_code) {
    SKIP_UNLESS_TR();
    real_alerts = true;
    setenv("LOONY_EXIT_AFTER", "500", 1);
    uint32_t ticks[1] = {240}, shots[1];
    char out[32768];
    int status = run_script_in(test_tr_dir(),
                               "130 click 392 238\n200 type someone@example.com\n210 down tab\n212 up tab\n"
                               "220 type 1234-5678\n250 click 494 236\n",
                               ticks, shots, 1, out, sizeof out);
    unsetenv("LOONY_EXIT_AFTER");
    real_alerts = false;
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: Alert 901: answered item 4 (Enter your Key-Code)");
    CHECK_CONTAINS(out, "loony: GetNewDialog 911");
    CHECK_CONTAINS(out, "loony: Alert 902: OK | Sorry, your license counld not be certified.");
    CHECK_EQ(shots[0], 0x972EAF93u); /* DLOG 911, filled in */
    char p[1200];
    snprintf(p, sizeof p, "%s/TRISTAN IE Registration", test_tr_dir());
    CHECK(access(p, F_OK) != 0);
}

/* Tristan takes no Apple Events, so Cmd-Q in the app (a script's quit)
   reaches it as the Cmd-Q of its File menu. */
TEST(run_tristan_quits_on_cmd_q_from_the_host) {
    SKIP_UNLESS_TR();
    char out[32768];
    int status = run_script_in(test_tr_dir(), "700 quit\n", NULL, NULL, 0, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: sending the quit as Cmd-Q; the game takes no Apple Events");
    CHECK_CONTAINS(out, "loony: ExitToShell");
    CHECK(!strstr(out, "didn't quit"));
}

static void run_mad_daedalus_headless(void *unused) {
    (void)unused;
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    setenv("LOONY_EXIT_AFTER", "600", 1);
    setenv("LOONY_SCREENSHOT", shot, 1);
    run_loony((void *)test_md_app());
}

/* Mad Daedalus 1.1.9 answers its windows (Play Demo, then OK), loads its
   sound through CoreAudio, takes the display at its own size, 1024x768,
   32 bits, and by tick 600 shows its table, waiting for Esc. */
TEST(run_mad_daedalus_plays_its_opening_headless) {
    SKIP_UNLESS_MD();
    tmp_name(shot, sizeof shot, "run");
    char out[32768];
    test_tmp_dir(run_data, sizeof run_data);
    int status = test_run_child(run_mad_daedalus_headless, NULL, out, sizeof out);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    size_t len = 0;
    uint8_t *png = read_file(shot, &len);
    unlink(shot);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "playing Mad Daedalus from");
    CHECK_CONTAINS(out, "Contents/MacOS/Mad Daedalus: 179 imports, main at 0x3a8dc, 6 initializers");
    CHECK_CONTAINS(out, "loony: nib window Welcome: command 'ok  ' (Play Demo)");
    CHECK_CONTAINS(out, "loony: nib window Demo: command 'ok  ' (OK)");
    CHECK_CONTAINS(out, "loony: display mode 1024x768, 32 bits");
    CHECK_CONTAINS(out, "loony: exiting after 600 ticks (LOONY_EXIT_AFTER)");
    CHECK(!strstr(out, "coreaudio:")); /* every sound opened and converted */
    CHECK(!strstr(out, "not supported"));
    CHECK(png != NULL);
    CHECK(len > 33);
    CHECK_EQ(rd_be32(png + 16), 1024);
    CHECK_EQ(rd_be32(png + 20), 768);
    uint32_t h = fnv1a32(png, len);
    free(png);
    CHECK_EQ(h, 0xEFEC0DF5u); /* the table, "GAME OVER" and "PRESS ESC TO START" */
}

/* Mad Daedalus's own QUIT: Esc opens the menu over the table, Up wraps
   to QUIT, Return. Its main returns (it has no ExitToShell), with no quit
   Apple Event. The late quit ends a run whose menu missed QUIT. */
TEST(run_mad_daedalus_quits_from_its_own_menu) {
    SKIP_UNLESS_MD();
    char out[32768];
    int status = run_script_in(test_md_app(), "620 down esc\n624 up esc\n1250 down up\n1254 up up\n"
                                              "1400 down return\n1406 up return\n2400 quit\n",
                               NULL, NULL, 0, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: main returned 0");
    CHECK(!strstr(out, "quit Apple Event"));
}

static void run_fairy_tower_headless(void *unused) {
    (void)unused;
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    setenv("LOONY_EXIT_AFTER", "600", 1);
    setenv("LOONY_SCREENSHOT", shot, 1);
    run_loony((void *)test_ft_app());
}

/* FAIRY TOWER 1.1.7 is built as Mad Daedalus is: it answers the same two
   windows (Play Demo, then OK), plays through CoreAudio into a float
   output stream, takes the display at 1024x768, 32 bits, and by tick 600
   shows its table, waiting for Esc. */
TEST(run_fairy_tower_plays_its_opening_headless) {
    SKIP_UNLESS_FT();
    tmp_name(shot, sizeof shot, "run");
    char out[32768];
    test_tmp_dir(run_data, sizeof run_data);
    int status = test_run_child(run_fairy_tower_headless, NULL, out, sizeof out);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    size_t len = 0;
    uint8_t *png = read_file(shot, &len);
    unlink(shot);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "playing FAIRY TOWER from");
    CHECK_CONTAINS(out, "Contents/MacOS/FAIRY TOWER: 172 imports, main at 0x3a410, 7 initializers");
    CHECK_CONTAINS(out, "loony: nib window Welcome: command 'ok  ' (Play Demo)");
    CHECK_CONTAINS(out, "loony: nib window Demo: command 'ok  ' (OK)");
    CHECK_CONTAINS(out, "loony: display mode 1024x768, 32 bits");
    CHECK_CONTAINS(out, "loony: exiting after 600 ticks (LOONY_EXIT_AFTER)");
    CHECK(!strstr(out, "coreaudio:")); /* every sound opened and converted */
    CHECK(!strstr(out, "not supported"));
    CHECK(png != NULL);
    CHECK(len > 33);
    CHECK_EQ(rd_be32(png + 16), 1024);
    CHECK_EQ(rd_be32(png + 20), 768);
    uint32_t h = fnv1a32(png, len);
    free(png);
    CHECK_EQ(h, 0x1E2C61B1u); /* the table, "GAME OVER" and "PRESS ESC TO START" */
}

/* FAIRY TOWER's own QUIT, as Mad Daedalus's: Esc opens the menu, Up
   wraps to QUIT, Return, and main returns with no quit Apple Event. */
TEST(run_fairy_tower_quits_from_its_own_menu) {
    SKIP_UNLESS_FT();
    char out[32768];
    int status = run_script_in(test_ft_app(), "620 down esc\n624 up esc\n1250 down up\n1254 up up\n"
                                              "1400 down return\n1406 up return\n2400 quit\n",
                               NULL, NULL, 0, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: main returned 0");
    CHECK(!strstr(out, "quit Apple Event"));
}

/* Cmd-Q's path: the shim sends the quit Apple Event and the game ends. */
TEST(run_fairy_tower_quits_on_the_quit_apple_event) {
    SKIP_UNLESS_FT();
    char out[32768];
    int status = run_script_in(test_ft_app(), "900 quit\n", NULL, NULL, 0, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "sending the quit Apple Event");
}

static void run_angel_egg_headless(void *unused) {
    (void)unused;
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    setenv("LOONY_EXIT_AFTER", "600", 1);
    setenv("LOONY_SCREENSHOT", shot, 1);
    run_loony((void *)test_ae_app());
}

/* Angel Egg 3.0.1 answers its two shareware alerts (Play Demo, then OK),
   makes its window the 1024x768 screen, loads its table, and by tick 600
   plays its attract mode. */
TEST(run_angel_egg_plays_its_opening_headless) {
    SKIP_UNLESS_AE();
    tmp_name(shot, sizeof shot, "run");
    char out[32768];
    test_tmp_dir(run_data, sizeof run_data);
    int status = test_run_child(run_angel_egg_headless, NULL, out, sizeof out);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    size_t len = 0;
    uint8_t *png = read_file(shot, &len);
    unlink(shot);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "playing Angel Egg from");
    CHECK_CONTAINS(out, "loony: Alert 901 (answering item 1): Play Demo");
    CHECK_CONTAINS(out, "loony: Alert 900 (answering item 1): OK");
    CHECK_CONTAINS(out, "loony: exiting after 600 ticks (LOONY_EXIT_AFTER)");
    CHECK(!strstr(out, "not supported"));
    CHECK(png != NULL);
    CHECK(len > 33);
    CHECK_EQ(rd_be32(png + 16), 1024);
    CHECK_EQ(rd_be32(png + 20), 768);
    uint32_t h = fnv1a32(png, len);
    free(png);
    CHECK_EQ(h, 0x8440172Fu); /* the table, its backglass reading ANGEL EGG, EGG ANGEL */
}

/* Angel Egg's own Quit, Cmd-Q in its File menu, at its attract mode: its
   void main returns (it has no ExitToShell) whatever is in r3, so the run
   exits with 0, with no quit Apple Event. The late quit ends a run whose
   Cmd-Q went astray. */
TEST(run_angel_egg_quits_from_its_own_menu) {
    SKIP_UNLESS_AE();
    char out[32768];
    int status = run_script_in(test_ae_app(), "1200 down lcmd\n1202 down q\n1206 up q\n1208 up lcmd\n2400 quit\n",
                               NULL, NULL, 0, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: main returned ");
    CHECK(!strstr(out, "quit Apple Event"));
}

/* Cmd-Q (the script's quit) at its attract mode: the quit Apple Event, and
   its main returns. */
TEST(run_angel_egg_quits_on_the_quit_apple_event) {
    SKIP_UNLESS_AE();
    char out[32768];
    int status = run_script_in(test_ae_app(), "1200 quit\n", NULL, NULL, 0, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "sending the quit Apple Event");
    CHECK_CONTAINS(out, "loony: main returned ");
    CHECK(!strstr(out, "didn't quit"));
}

static void run_golden_logres_headless(void *unused) {
    (void)unused;
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    setenv("LOONY_EXIT_AFTER", "600", 1);
    setenv("LOONY_SCREENSHOT", shot, 1);
    run_loony((void *)test_gl_app());
}

/* Golden Logres 3.0.4, a CodeWarrior program, answers its two demo alerts
   (Play Demo, then OK), takes the screen at 1024x768 and by tick 600
   tells its story. */
TEST(run_golden_logres_plays_its_opening_headless) {
    SKIP_UNLESS_GL();
    tmp_name(shot, sizeof shot, "run");
    char out[32768];
    test_tmp_dir(run_data, sizeof run_data);
    int status = test_run_child(run_golden_logres_headless, NULL, out, sizeof out);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    size_t len = 0;
    uint8_t *png = read_file(shot, &len);
    unlink(shot);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "playing Golden Logres from");
    CHECK_CONTAINS(out, "Contents/MacOS/Golden Logres: 420 imports, main at 0x2e5d4, 17 initializers");
    CHECK_CONTAINS(out, "loony: Alert 901 (answering item 1): Play Demo");
    CHECK_CONTAINS(out, "loony: Alert 900 (answering item 1): OK");
    CHECK_CONTAINS(out, "loony: exiting after 600 ticks (LOONY_EXIT_AFTER)");
    CHECK(!strstr(out, "not supported"));
    CHECK(png != NULL);
    CHECK(len > 33);
    CHECK_EQ(rd_be32(png + 16), 1024);
    CHECK_EQ(rd_be32(png + 20), 768);
    char got[16];
    snprintf(got, sizeof got, "%08x", fnv1a32(png, len));
    free(png);
    CHECK_STR(got, "7ed83e89"); /* the story's first picture, fading in */
}

/* Golden Logres's Quit is in the application menu Mac OS X gives it, so
   Cmd-Q reaches it as the quit Apple Event (the script's quit), after
   which its main returns. */
TEST(run_golden_logres_quits_on_the_quit_apple_event) {
    SKIP_UNLESS_GL();
    char out[32768];
    int status = run_script_in(test_gl_app(), "700 down esc\n704 up esc\n820 down esc\n824 up esc\n1000 quit\n",
                               NULL, NULL, 0, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: sending the quit Apple Event");
    CHECK_CONTAINS(out, "loony: main returned 0");
    CHECK(!strstr(out, "didn't quit"));
}

TEST(run_reports_missing_game_folder) {
    char out[4096];
    int status = test_run_child(run_loony, (void *)"/nonexistent/loony", out, sizeof out);
    CHECK_EQ(status, 1);
    CHECK_CONTAINS(out, "can't read /nonexistent/loony/LOONY LABYRINTH 3.0.1");
}

/* Fake application folders: apps_dir holds links to the real game folders
   (or empty programs, for runs that never start a game). */
static char apps_dir[1024];

static bool link_game(const char *name, const char *real_dir) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", apps_dir, name);
    return symlink(real_dir, path) == 0;
}

static void run_loony_no_folder(void *unused) {
    (void)unused;
    setenv("LOONY_APPS_DIR", apps_dir, 1);
    if (run_data[0])
        setenv("LOONY_DATA_DIR", run_data, 1);
    setenv("LOONY_AUTO_ALERTS", "1", 1);
    execl(LOONY_BIN, "loony", (char *)NULL);
    _exit(127);
}

/* Review Focus 4. */
TEST(run_with_no_game_installed_says_where_to_put_them) {
    test_tmp_dir(apps_dir, sizeof apps_dir);
    char out[4096];
    int status = test_run_child(run_loony_no_folder, NULL, out, sizeof out);
    char expect[2600];
    snprintf(expect, sizeof expect,
             "no LittleWing game found: put Loony Labyrinth in %s/Loony Labyrinth or Crystal Caliburn in "
             "%s/Crystal Caliburn",
             apps_dir, apps_dir);
    test_remove_tree(apps_dir);
    CHECK_EQ(status, 1);
    CHECK_CONTAINS(out, expect);
}

/* Review Focus 4: one game installed plays at once, with no picker. */
static void run_loony_no_folder_briefly(void *unused) {
    setenv("LOONY_EXIT_AFTER", "60", 1);
    run_loony_no_folder(unused);
}

TEST(run_with_one_game_installed_skips_the_picker) {
    SKIP_UNLESS_CC();
    test_tmp_dir(apps_dir, sizeof apps_dir);
    CHECK(link_game("Crystal Caliburn", test_cc_dir()));
    test_tmp_dir(run_data, sizeof run_data);
    char out[32768];
    int status = test_run_child(run_loony_no_folder_briefly, NULL, out, sizeof out);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    test_remove_tree(apps_dir);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "playing Crystal Caliburn from");
    CHECK(!strstr(out, "picker"));
}

static void run_loony_game_env(void *id) {
    setenv("LOONY_GAME", (const char *)id, 1);
    setenv("LOONY_EXIT_AFTER", "60", 1);
    run_loony_no_folder(NULL);
}

TEST(run_loony_game_picks_without_a_folder) {
    SKIP_UNLESS_CC();
    test_tmp_dir(apps_dir, sizeof apps_dir);
    CHECK(link_game("Crystal Caliburn", test_cc_dir()));
    test_tmp_dir(run_data, sizeof run_data);
    char out[32768], bad_out[4096];
    int status = test_run_child(run_loony_game_env, (void *)"crystal-caliburn", out, sizeof out);
    int bad = test_run_child(run_loony_game_env, (void *)"pacman", bad_out, sizeof bad_out);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    test_remove_tree(apps_dir);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "playing Crystal Caliburn from");
    CHECK_EQ(bad, 1);
    CHECK_CONTAINS(bad_out, "LOONY_GAME names no known game: pacman");
}

/* Review Focus 1 and 5: with no LOONY_DATA_DIR, a game saves in
   ~/Library/Application Support/littlewing-shim/<id>, and Loony Labyrinth's
   file from before Plan 8 moves into its folder. HOME is temporary. */
static char save_home[1024];

static void run_with_home_saves(void *dir) {
    setenv("HOME", save_home, 1);
    unsetenv("LOONY_DATA_DIR");
    setenv("LOONY_AUTO_ALERTS", "1", 1);
    setenv("LOONY_EXIT_AFTER", "240", 1);
    execl(LOONY_BIN, "loony", (const char *)dir, (char *)NULL);
    _exit(127);
}

TEST(run_each_game_saves_in_its_own_folder) {
    SKIP_UNLESS_CC();
    test_tmp_dir(save_home, sizeof save_home);
    char root[1100], path[1300];
    snprintf(root, sizeof root, "%s/Library/Application Support/littlewing-shim", save_home);
    CHECK(make_dirs(root));
    snprintf(path, sizeof path, "%s/prefs.plist", root);
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    fputs("Loony's license", f);
    fclose(f);
    char out[32768];
    int status = test_run_child(run_with_home_saves, (void *)test_cc_dir(), out, sizeof out);
    CHECK_EQ(status, 0);
    snprintf(path, sizeof path, "%s/crystal-caliburn/prefs.plist", root);
    CHECK(access(path, R_OK) == 0);
    snprintf(path, sizeof path, "%s/loony-labyrinth/prefs.plist", root);
    size_t len = 0;
    char *moved = (char *)read_file(path, &len);
    CHECK(moved != NULL);
    CHECK(len == 15 && memcmp(moved, "Loony's license", 15) == 0);
    free(moved);
    snprintf(path, sizeof path, "%s/prefs.plist", root);
    CHECK(access(path, F_OK) != 0);
    test_remove_tree(save_home);
}

/* The game's own QUIT (Facts measured): Esc ends the demo, Esc opens the
   menu, Up wraps to its quit item, Return. */
static const char menu_quit_script[] = "1720 down esc\n1724 up esc\n1800 down esc\n1804 up esc\n"
                                       "1900 down up\n1904 up up\n2000 down return\n2004 up return\n";

static char pick[256], pick_shot[1024];

static void run_picker(void *unused) {
    setenv("LOONY_PICK", pick, 1);
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    if (pick_shot[0])
        setenv("LOONY_PICKER_SHOT", pick_shot, 1);
    if (script_path[0])
        setenv("LOONY_SCRIPT", script_path, 1);
    run_loony_no_folder(unused);
}

static bool both_games(void) {
    return test_game_present() && test_cc_present() && link_game("Loony Labyrinth", test_game_dir()) &&
           link_game("Crystal Caliburn", test_cc_dir());
}

static int run_picker_with(const char *picks, const char *script, char *out, size_t outlen) {
    snprintf(pick, sizeof pick, "%s", picks);
    script_path[0] = '\0';
    if (script) {
        tmp_name(script_path, sizeof script_path, "script");
        FILE *f = fopen(script_path, "w");
        fputs(script, f);
        fclose(f);
    }
    int status = test_run_child(run_picker, NULL, out, outlen);
    if (script)
        unlink(script_path);
    script_path[0] = '\0';
    return status;
}

/* Review Focus 2 and 3: the game's own QUIT goes back to the picker; the
   log carries on across the restart. */
TEST(run_quitting_from_the_game_menu_returns_to_the_picker) {
    SKIP_UNLESS_GAME();
    SKIP_UNLESS_CC();
    test_tmp_dir(apps_dir, sizeof apps_dir);
    CHECK(both_games());
    test_tmp_dir(run_data, sizeof run_data);
    char out[65536];
    int status = run_picker_with("crystal-caliburn,quit", menu_quit_script, out, sizeof out);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    test_remove_tree(apps_dir);
    CHECK_EQ(status, 0);
    const char *a = strstr(out, "picker: crystal-caliburn");
    const char *b = a ? strstr(a, "playing Crystal Caliburn from") : NULL;
    const char *c = b ? strstr(b, "ExitToShell") : NULL;
    const char *d = c ? strstr(c, "back to the picker") : NULL;
    const char *e = d ? strstr(d, "picker: quit") : NULL;
    CHECK(a && b && c && d && e);
    CHECK(!strstr(out, "quit Apple Event"));
}

/* Review Focus 2: Cmd-Q (here a script quit, which takes the same path)
   during a picked game quits the app; the second pick is never used. */
TEST(run_cmd_q_in_a_picked_game_quits_the_app) {
    SKIP_UNLESS_GAME();
    SKIP_UNLESS_CC();
    test_tmp_dir(apps_dir, sizeof apps_dir);
    CHECK(both_games());
    test_tmp_dir(run_data, sizeof run_data);
    char out[65536];
    int status = run_picker_with("loony-labyrinth,crystal-caliburn", "300 quit\n", out, sizeof out);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    test_remove_tree(apps_dir);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "picker: loony-labyrinth");
    CHECK_CONTAINS(out, "sending the quit Apple Event");
    CHECK(!strstr(out, "back to the picker"));
    CHECK(!strstr(out, "picker: crystal-caliburn"));
}

TEST(run_the_picker_frame_shows_both_games) {
    SKIP_UNLESS_GAME();
    SKIP_UNLESS_CC();
    test_tmp_dir(apps_dir, sizeof apps_dir);
    CHECK(both_games());
    test_tmp_dir(run_data, sizeof run_data);
    tmp_name(pick_shot, sizeof pick_shot, "picker");
    char out[16384];
    int status = run_picker_with("quit", NULL, out, sizeof out);
    size_t len = 0;
    uint8_t *png = read_file(pick_shot, &len);
    unlink(pick_shot);
    pick_shot[0] = '\0';
    test_remove_tree(run_data);
    run_data[0] = '\0';
    test_remove_tree(apps_dir);
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "can't read")); /* both pictures loaded */
    CHECK(png != NULL);
    CHECK_EQ(rd_be32(png + 16), 800);
    CHECK_EQ(rd_be32(png + 20), 600);
    char got[16];
    snprintf(got, sizeof got, "%08x", fnv1a32(png, len));
    free(png);
    CHECK_STR(got, "ed1ea7f1"); /* approved by the user on 2026-10-05 */
}

/* Three games: three cards in a row, MONSTER FAIR's showing its icon. Picked,
   it starts from its bundle. */
TEST(run_the_picker_shows_three_games_and_starts_monster_fair) {
    SKIP_UNLESS_GAME();
    SKIP_UNLESS_CC();
    SKIP_UNLESS_MF();
    test_tmp_dir(apps_dir, sizeof apps_dir);
    CHECK(both_games() && link_game("MONSTER FAIR.app", test_mf_app()));
    test_tmp_dir(run_data, sizeof run_data);
    tmp_name(pick_shot, sizeof pick_shot, "picker");
    char out[32768];
    int status = run_picker_with("monster-fair", "300 quit\n", out, sizeof out);
    cgimage_pixels px = {0};
    char err[256];
    bool decoded = cgimage_decode_png(pick_shot, &px, err, sizeof err);
    size_t len = 0;
    uint8_t *png = read_file(pick_shot, &len);
    char got[16];
    snprintf(got, sizeof got, "%08x", png ? fnv1a32(png, len) : 0);
    free(png);
    unlink(pick_shot);
    pick_shot[0] = '\0';
    test_remove_tree(run_data);
    run_data[0] = '\0';
    test_remove_tree(apps_dir);
    CHECK_EQ(status, 0);
    CHECK(decoded);
    const uint8_t *border = px.xrgb + 4 * (200 * px.width + 16 - 2);  /* the first card, selected */
    const uint8_t *plain = px.xrgb + 4 * (185 * px.width + 544 + 5);  /* MONSTER FAIR's card, */
    const uint8_t *icon = px.xrgb + 4 * (270 * px.width + 544 + 120); /* and its icon */
    bool gold = border[1] == 0xFF && border[2] == 0xCC;
    bool card = plain[1] == 0x22 && plain[2] == 0x22;
    bool drawn = !(icon[1] == 0x22 && icon[2] == 0x22 && icon[3] == 0x22);
    free(px.xrgb);
    CHECK(gold);
    CHECK(card);
    CHECK(drawn);
    CHECK(!strstr(out, "picker: can't")); /* every card's art loaded */
    CHECK_CONTAINS(out, "loony: picker: monster-fair");
    CHECK_CONTAINS(out, "loony: playing MONSTER FAIR from ");
    CHECK_CONTAINS(out, "loony: nib window Welcome: command 'ok  ' (Play Demo)");
    CHECK_CONTAINS(out, "loony: main returned 0");
    CHECK(!strstr(out, "back to the picker")); /* the host's quit (Cmd-Q) quits the app */
    CHECK_STR(got, "a1fb5138"); /* approved by the user on 2026-10-09 */
}

/* Four games: two rows of two cards, Jinni Zeala's showing its icon.
   Picked, it plays; the host's quit (Cmd-Q) quits the app. */
TEST(run_the_picker_shows_four_games_and_starts_jinni_zeala) {
    SKIP_UNLESS_GAME();
    SKIP_UNLESS_CC();
    SKIP_UNLESS_MF();
    SKIP_UNLESS_JZ();
    test_tmp_dir(apps_dir, sizeof apps_dir);
    CHECK(both_games() && link_game("MONSTER FAIR.app", test_mf_app()) && link_game("Jinni Zeala", test_jz_dir()));
    test_tmp_dir(run_data, sizeof run_data);
    tmp_name(pick_shot, sizeof pick_shot, "picker");
    char out[32768];
    int status = run_picker_with("jinni-zeala", "300 quit\n", out, sizeof out);
    size_t len = 0;
    uint8_t *png = read_file(pick_shot, &len);
    char got[16];
    snprintf(got, sizeof got, "%08x", png ? fnv1a32(png, len) : 0);
    free(png);
    unlink(pick_shot);
    pick_shot[0] = '\0';
    test_remove_tree(run_data);
    run_data[0] = '\0';
    test_remove_tree(apps_dir);
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "picker: can't")); /* every card's art loaded */
    CHECK(!strstr(out, "picker: /"));
    CHECK_CONTAINS(out, "loony: picker: jinni-zeala");
    CHECK_CONTAINS(out, "loony: playing Jinni Zeala from ");
    CHECK_CONTAINS(out, "loony: sending the quit Apple Event");
    CHECK_CONTAINS(out, "loony: ExitToShell");
    CHECK(!strstr(out, "back to the picker"));
    CHECK_STR(got, "29afcc5c");
}

/* Five games: a row of three and a row of two, Mad Daedalus's card, the
   last, showing its icon. Picked, it plays; the host's quit (Cmd-Q) quits
   the app. */
TEST(run_the_picker_shows_five_games_and_starts_mad_daedalus) {
    SKIP_UNLESS_GAME();
    SKIP_UNLESS_CC();
    SKIP_UNLESS_MF();
    SKIP_UNLESS_JZ();
    SKIP_UNLESS_MD();
    test_tmp_dir(apps_dir, sizeof apps_dir);
    CHECK(both_games() && link_game("MONSTER FAIR.app", test_mf_app()) && link_game("Jinni Zeala", test_jz_dir()) &&
          link_game("Mad Daedalus.app", test_md_app()));
    test_tmp_dir(run_data, sizeof run_data);
    tmp_name(pick_shot, sizeof pick_shot, "picker");
    char out[32768];
    int status = run_picker_with("mad-daedalus", "900 quit\n", out, sizeof out);
    size_t len = 0;
    uint8_t *png = read_file(pick_shot, &len);
    char got[16];
    snprintf(got, sizeof got, "%08x", png ? fnv1a32(png, len) : 0);
    free(png);
    unlink(pick_shot);
    pick_shot[0] = '\0';
    test_remove_tree(run_data);
    run_data[0] = '\0';
    test_remove_tree(apps_dir);
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "picker: can't")); /* every card's art loaded */
    CHECK(!strstr(out, "picker: /"));
    CHECK_CONTAINS(out, "loony: picker: mad-daedalus");
    CHECK_CONTAINS(out, "loony: playing Mad Daedalus from ");
    CHECK_CONTAINS(out, "loony: sending the quit Apple Event");
    CHECK_CONTAINS(out, "loony: main returned 0");
    CHECK(!strstr(out, "back to the picker"));
    CHECK_STR(got, "4aa50c83");
}

/* Seven games: a row of four over a row of three, Golden Logres's card its
   app.icns icon. Picked, it starts from its bundle. */
TEST(run_the_picker_shows_seven_games_and_starts_golden_logres) {
    SKIP_UNLESS_GAME();
    SKIP_UNLESS_CC();
    SKIP_UNLESS_MF();
    SKIP_UNLESS_JZ();
    SKIP_UNLESS_MD();
    SKIP_UNLESS_AE();
    SKIP_UNLESS_GL();
    test_tmp_dir(apps_dir, sizeof apps_dir);
    CHECK(both_games() && link_game("MONSTER FAIR.app", test_mf_app()) && link_game("Jinni Zeala", test_jz_dir()) &&
          link_game("Mad Daedalus.app", test_md_app()) && link_game("Angel Egg.app", test_ae_app()) &&
          link_game("Golden Logres.app", test_gl_app()));
    test_tmp_dir(run_data, sizeof run_data);
    tmp_name(pick_shot, sizeof pick_shot, "picker");
    char out[32768];
    /* Esc twice ends the story and the title: it reads no events until then. */
    int status = run_picker_with("golden-logres", "700 down esc\n704 up esc\n820 down esc\n824 up esc\n1000 quit\n",
                                 out, sizeof out);
    size_t len = 0;
    uint8_t *png = read_file(pick_shot, &len);
    char got[16];
    snprintf(got, sizeof got, "%08x", png ? fnv1a32(png, len) : 0);
    free(png);
    unlink(pick_shot);
    pick_shot[0] = '\0';
    test_remove_tree(run_data);
    run_data[0] = '\0';
    test_remove_tree(apps_dir);
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "picker: can't")); /* every card's art loaded */
    CHECK(!strstr(out, "picker: /"));
    CHECK_CONTAINS(out, "loony: picker: golden-logres");
    CHECK_CONTAINS(out, "loony: playing Golden Logres from ");
    CHECK_CONTAINS(out, "loony: sending the quit Apple Event");
    CHECK_CONTAINS(out, "loony: main returned 0");
    CHECK(!strstr(out, "back to the picker"));
    CHECK_STR(got, "3d2dec68");
}

/* Nine games, every one: three rows of three, Tristan's card, the last,
   its title, PICT 900. Picked, it starts from its folder, and Cmd-Q ends
   it before a game. */
TEST(run_the_picker_shows_nine_games_and_starts_tristan) {
    SKIP_UNLESS_GAME();
    SKIP_UNLESS_CC();
    SKIP_UNLESS_MF();
    SKIP_UNLESS_JZ();
    SKIP_UNLESS_MD();
    SKIP_UNLESS_AE();
    SKIP_UNLESS_GL();
    SKIP_UNLESS_FT();
    SKIP_UNLESS_TR();
    test_tmp_dir(apps_dir, sizeof apps_dir);
    CHECK(both_games() && link_game("MONSTER FAIR.app", test_mf_app()) && link_game("Jinni Zeala", test_jz_dir()) &&
          link_game("Mad Daedalus.app", test_md_app()) && link_game("Angel Egg.app", test_ae_app()) &&
          link_game("Golden Logres.app", test_gl_app()) && link_game("FAIRY TOWER.app", test_ft_app()) &&
          link_game("TRISTAN IE", test_tr_dir()));
    test_tmp_dir(run_data, sizeof run_data);
    tmp_name(pick_shot, sizeof pick_shot, "picker");
    char out[32768];
    int status = run_picker_with("tristan", "700 quit\n", out, sizeof out);
    size_t len = 0;
    uint8_t *png = read_file(pick_shot, &len);
    char got[16];
    snprintf(got, sizeof got, "%08x", png ? fnv1a32(png, len) : 0);
    free(png);
    unlink(pick_shot);
    pick_shot[0] = '\0';
    test_remove_tree(run_data);
    run_data[0] = '\0';
    test_remove_tree(apps_dir);
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "picker: can't")); /* every card's art loaded */
    CHECK(!strstr(out, "picker: /"));
    CHECK_CONTAINS(out, "loony: picker: tristan");
    CHECK_CONTAINS(out, "loony: playing Tristan from ");
    CHECK_CONTAINS(out, "loony: sending the quit as Cmd-Q");
    CHECK_CONTAINS(out, "loony: ExitToShell");
    CHECK(!strstr(out, "back to the picker"));
    CHECK_STR(got, "fff77955"); /* the frame with Loony Labyrinth selected */
}

/* Eight games: two rows of four, FAIRY TOWER's card, the last, its
   appl.png icon. Picked, it plays; the host's quit (Cmd-Q) quits the app. */
TEST(run_the_picker_shows_eight_games_and_starts_fairy_tower) {
    SKIP_UNLESS_GAME();
    SKIP_UNLESS_CC();
    SKIP_UNLESS_MF();
    SKIP_UNLESS_JZ();
    SKIP_UNLESS_MD();
    SKIP_UNLESS_AE();
    SKIP_UNLESS_GL();
    SKIP_UNLESS_FT();
    test_tmp_dir(apps_dir, sizeof apps_dir);
    CHECK(both_games() && link_game("MONSTER FAIR.app", test_mf_app()) && link_game("Jinni Zeala", test_jz_dir()) &&
          link_game("Mad Daedalus.app", test_md_app()) && link_game("Angel Egg.app", test_ae_app()) &&
          link_game("Golden Logres.app", test_gl_app()) && link_game("FAIRY TOWER.app", test_ft_app()));
    test_tmp_dir(run_data, sizeof run_data);
    tmp_name(pick_shot, sizeof pick_shot, "picker");
    char out[32768];
    int status = run_picker_with("fairy-tower", "900 quit\n", out, sizeof out);
    size_t len = 0;
    uint8_t *png = read_file(pick_shot, &len);
    char got[16];
    snprintf(got, sizeof got, "%08x", png ? fnv1a32(png, len) : 0);
    free(png);
    unlink(pick_shot);
    pick_shot[0] = '\0';
    test_remove_tree(run_data);
    run_data[0] = '\0';
    test_remove_tree(apps_dir);
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "picker: can't")); /* every card's art loaded */
    CHECK(!strstr(out, "picker: /"));
    CHECK_CONTAINS(out, "loony: picker: fairy-tower");
    CHECK_CONTAINS(out, "loony: playing FAIRY TOWER from ");
    CHECK_CONTAINS(out, "loony: sending the quit Apple Event");
    CHECK_CONTAINS(out, "loony: main returned 0");
    CHECK(!strstr(out, "back to the picker"));
    CHECK_STR(got, "c54d53f6");
}

/* Review Focus 1: the picker remembers the last game in the save root,
   and each game's preferences land in its own folder. */
static void run_picker_home(void *unused) {
    setenv("HOME", save_home, 1);
    unsetenv("LOONY_DATA_DIR");
    run_data[0] = '\0';
    run_picker(unused);
}

TEST(run_the_picker_remembers_and_each_game_saves_apart) {
    SKIP_UNLESS_GAME();
    SKIP_UNLESS_CC();
    test_tmp_dir(apps_dir, sizeof apps_dir);
    CHECK(both_games());
    test_tmp_dir(save_home, sizeof save_home);
    snprintf(pick, sizeof pick, "crystal-caliburn,quit");
    tmp_name(script_path, sizeof script_path, "script");
    FILE *f = fopen(script_path, "w");
    fputs(menu_quit_script, f);
    fclose(f);
    char out[65536];
    int status = test_run_child(run_picker_home, NULL, out, sizeof out);
    unlink(script_path);
    script_path[0] = '\0';
    char root[1100], path[1300];
    snprintf(root, sizeof root, "%s/Library/Application Support/littlewing-shim", save_home);
    snprintf(path, sizeof path, "%s/picker.plist", root);
    plist_entry *e = NULL;
    uint32_t n = 0;
    char err[256];
    plist_status ps = plist_read(path, &e, &n, err, sizeof err);
    bool remembered = false;
    for (uint32_t i = 0; ps == PLIST_OK && i < n; i++)
        remembered |= strcmp(e[i].key, "last game") == 0 && !e[i].is_number &&
                      strcmp(e[i].str, "crystal-caliburn") == 0;
    if (ps == PLIST_OK)
        plist_free(e, n);
    snprintf(path, sizeof path, "%s/crystal-caliburn/prefs.plist", root);
    bool cc_saved = access(path, R_OK) == 0;
    snprintf(path, sizeof path, "%s/prefs.plist", root);
    bool none_at_root = access(path, F_OK) != 0;
    test_remove_tree(save_home);
    test_remove_tree(apps_dir);
    CHECK_EQ(status, 0);
    CHECK(remembered);
    CHECK(cc_saved);
    CHECK(none_at_root);
}

/* Launched as the app, the log goes to ~/Library/Logs/littlewing-shim instead
   of the (absent) terminal. HOME is a temporary folder here. */
static char app_home[1024], app_bin[1200];

static void run_as_app(void *dir) {
    setenv("HOME", app_home, 1);
    execl(app_bin, app_bin, (const char *)dir, (char *)NULL);
    _exit(127);
}

TEST(run_as_the_app_logs_to_library_logs) {
    test_tmp_dir(app_home, sizeof app_home);
    char macos[1100], cmd[2600];
    snprintf(macos, sizeof macos, "%s/Loony.app/Contents/MacOS", app_home);
    CHECK(make_dirs(macos));
    snprintf(app_bin, sizeof app_bin, "%s/loony", macos);
    snprintf(cmd, sizeof cmd, "cp '%s' '%s'", LOONY_BIN, app_bin);
    CHECK(system(cmd) == 0);
    char log[1200];
    snprintf(log, sizeof log, "%s/Library/Logs/littlewing-shim/loony.log", app_home);
    char out[4096];
    for (int run = 0; run < 2; run++) { /* the second run keeps the first log as loony.previous.log */
        int status = test_run_child(run_as_app, (void *)"/nonexistent/loony", out, sizeof out);
        CHECK_EQ(status, 1);
        CHECK_STR(out, ""); /* nothing on stderr */
    }
    size_t len = 0;
    char *text = (char *)read_file(log, &len);
    CHECK(text != NULL);
    text = realloc(text, len + 1);
    text[len] = '\0';
    CHECK_CONTAINS(text, "can't read /nonexistent/loony/LOONY LABYRINTH 3.0.1");
    CHECK_CONTAINS(text, "needs the original game in /nonexistent/loony");
    free(text);
    snprintf(log, sizeof log, "%s/Library/Logs/littlewing-shim/loony.previous.log", app_home);
    CHECK(access(log, F_OK) == 0);
    test_remove_tree(app_home);
}

static char bundle_bin[1300];
static bool bundle_hd; /* else HD is turned off, for the approved 800x600 frame */

static void run_bundle_scripted(void *dir) {
    if (!bundle_hd)
        setenv("LOONY_HD", "off", 1);
    setenv("HOME", run_data, 1); /* as an app it logs under HOME */
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    setenv("LOONY_SCRIPT", script_path, 1);
    setenv("LOONY_AUTO_ALERTS", "1", 1);
    setenv("LOONY_DATA_DIR", run_data, 1);
    execl(bundle_bin, bundle_bin, (const char *)dir, (char *)NULL);
    _exit(127);
}

/* tools/make_app.sh: the bundle carries its own libraries and its icon, is
   signed with the hardened runtime and allow-jit, and still emulates the game exactly
   (the approved menu frame). A sanitizer build (Debug) can't be bundled:
   it needs the compiler's runtime, and the script says so. */
TEST(run_the_app_bundle_is_self_contained_and_plays) {
    SKIP_UNLESS_GAME();
    char out_dir[1024], cmd[3000], text[8192];
    test_tmp_dir(out_dir, sizeof out_dir);
    snprintf(cmd, sizeof cmd, "'%s/tools/make_app.sh' '%s' '%s' 2>&1", LOONY_SRC_DIR, LOONY_BIN, out_dir);
    FILE *mk = popen(cmd, "r");
    CHECK(mk != NULL);
    size_t got = fread(text, 1, sizeof text - 1, mk);
    text[got] = '\0';
    int made = pclose(mk);
    snprintf(cmd, sizeof cmd, "otool -L '%s' | grep -q libclang_rt", LOONY_BIN);
    if (system(cmd) == 0) {
        test_remove_tree(out_dir);
        CHECK(made != 0);
        CHECK_CONTAINS(text, "libclang_rt");
        CHECK_CONTAINS(text, "isn't self-contained");
        return;
    }
    CHECK(made == 0);
    snprintf(bundle_bin, sizeof bundle_bin, "%s/LittleWing.app/Contents/MacOS/loony", out_dir);
    snprintf(cmd, sizeof cmd, "otool -L '%s' && codesign -d --entitlements - '%s/LittleWing.app' 2>&1",
             bundle_bin, out_dir);
    FILE *p = popen(cmd, "r");
    CHECK(p != NULL);
    size_t n = fread(text, 1, sizeof text - 1, p);
    text[n] = '\0';
    pclose(p);
    CHECK(!strstr(text, "/opt/homebrew/"));
    CHECK_CONTAINS(text, "@rpath/libunicorn");
    CHECK_CONTAINS(text, "com.apple.security.cs.allow-jit");
    char icon[1024];
    snprintf(icon, sizeof icon, "%s/LittleWing.app/Contents/Resources/AppIcon.icns", out_dir);
    CHECK(access(icon, R_OK) == 0);
    snprintf(icon, sizeof icon, "%s/LittleWing.app/Contents/Resources/Assets.car", out_dir);
    CHECK(access(icon, R_OK) == 0);
    snprintf(cmd, sizeof cmd, "plutil -extract CFBundleName raw '%s/LittleWing.app/Contents/Info.plist'", out_dir);
    p = popen(cmd, "r");
    CHECK(p != NULL);
    n = fread(text, 1, sizeof text - 1, p);
    text[n] = '\0';
    pclose(p);
    CHECK_STR(text, "LittleWing\n");

    char png[1024];
    tmp_name(png, sizeof png, "shot");
    tmp_name(script_path, sizeof script_path, "script");
    FILE *f = fopen(script_path, "w");
    fprintf(f, "1720 down esc\n1724 up esc\n1800 down esc\n1804 up esc\n1880 screenshot %s\n1900 quit\n", png);
    fclose(f);
    test_tmp_dir(run_data, sizeof run_data);
    char out[32768];
    int status = test_run_child(run_bundle_scripted, (void *)test_game_dir(), out, sizeof out);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    unlink(script_path);
    size_t len = 0;
    uint8_t *shot = read_file(png, &len);
    unlink(png);

    /* The app carries the HD art and plays in HD with it, unasked. */
    char art[1300];
    snprintf(art, sizeof art, "%s/LittleWing.app/Contents/Resources/hd-art/loony-labyrinth/454708e3.png",
             out_dir);
    bool has_art = access(art, R_OK) == 0;
    f = fopen(script_path, "w");
    fprintf(f, "300 screenshot %s\n320 quit\n", png);
    fclose(f);
    test_tmp_dir(run_data, sizeof run_data);
    bundle_hd = true;
    int hd_status = test_run_child(run_bundle_scripted, (void *)test_game_dir(), out, sizeof out);
    bundle_hd = false;
    test_remove_tree(run_data);
    run_data[0] = '\0';
    unlink(script_path);
    size_t hd_len = 0;
    uint8_t *hd_shot = read_file(png, &hd_len);
    unlink(png);
    test_remove_tree(out_dir);
    CHECK_EQ(status, 0);
    CHECK(shot != NULL);
    CHECK_EQ(fnv1a32(shot, len), 0xADE78151u); /* the menu */
    free(shot);
    CHECK(has_art);
    CHECK_EQ(hd_status, 0);
    CHECK(hd_shot != NULL && hd_len > 24);
    CHECK_EQ(rd_be32(hd_shot + 16), 3200);
    CHECK_EQ(rd_be32(hd_shot + 20), 2400);
    free(hd_shot);
}

/* The regression run: three minutes of scripted play on the fixed clock,
   from the opening through a game (plunger, flippers, nudges), with frames
   at each minute and the whole recording hashed. Any change to the
   emulation, the physics the game computes, drawing or sound shows up here.
   Recorded on 2026-10-02 after the user approved Plan 6's build. */
#define REGRESSION_TICKS 10800

/* How a game starts: the keys, then the tick of the first plunger, then
   the ticks from which Cmd-N starts another game (0 for none). */
typedef struct {
    const char *keys;
    int play_from;
    int again[2];
} regression_start;

/* The classic games: Esc ends the demo, Esc opens the menu, Return twice. */
static const regression_start classic_start = {
    "1720 down esc\n1724 up esc\n1800 down esc\n1804 up esc\n"
    "1900 down return\n1906 up return\n2000 down return\n2006 up return\n",
    2100, {0}};

/* MONSTER FAIR: before its table plays by itself, one Esc opens the menu;
   Return picks NEW GAME, then 1 PLAYER. */
static const regression_start mf_start = {
    "2200 down esc\n2204 up esc\n2320 down return\n2326 up return\n2440 down return\n2446 up return\n", 2600, {0}};

/* Jinni Zeala: no demo to end, so one Esc opens the menu; Return picks
   NEW GAME, then 1 PLAYER. */
static const regression_start jz_start = {
    "1720 down esc\n1724 up esc\n1900 down return\n1906 up return\n2000 down return\n2006 up return\n", 2100, {0}};

/* Angel Egg: after its title, Cmd-N is Operation > New Game. */
static const regression_start ae_start = {"900 down lcmd\n902 down n\n906 up n\n908 up lcmd\n", 1200, {0}};

/* Mad Daedalus: one Esc opens the menu over the table; Return picks NEW
   GAME, then 1 PLAYER. */
static const regression_start md_start = {
    "620 down esc\n624 up esc\n1250 down return\n1256 up return\n1500 down return\n1506 up return\n", 2000, {0}};

/* FAIRY TOWER, as Mad Daedalus: one Esc opens the menu over the table;
   Return picks NEW GAME, then 1 PLAYER. */
static const regression_start ft_start = {
    "620 down esc\n624 up esc\n1250 down return\n1256 up return\n1500 down return\n1506 up return\n", 2000, {0}};

/* Golden Logres: Esc ends the story, Esc the title, Cmd-N starts a game.
   The demo ends a game after 60 seconds of ball in play, so Cmd-N starts
   another after the first one's GAME OVER. */
static const regression_start gl_start = {
    "700 down esc\n704 up esc\n820 down esc\n824 up esc\n"
    "950 down lcmd\n952 down n\n958 up n\n960 up lcmd\n",
    1250,
    {8000, 0}};

/* Tristan: Cmd-N is File > New Game. Its demo ends a game after 60
   seconds of ball in play, so Cmd-N starts another after the first one's
   TIME UP. */
static const regression_start tr_start = {"700 down lcmd\n702 down n\n708 up n\n710 up lcmd\n", 1000, {5000, 0}};

static void write_regression_script(const char *path, const regression_start *start, char shots[3][1024]) {
    FILE *f = fopen(path, "w");
    fputs(start->keys, f);
    int shot = 0, again = 0;
    for (int t = start->play_from; t < REGRESSION_TICKS - 200;) {
        if (again < 2 && start->again[again] && t >= start->again[again]) {
            fprintf(f, "%d down lcmd\n%d down n\n%d up n\n%d up lcmd\n", t, t + 2, t + 8, t + 10);
            t += 150;
            again++;
        }
        fprintf(f, "%d down return\n%d up return\n", t, t + 80); /* the plunger */
        t += 120;
        for (int i = 0; i < 12; i++, t += 25)
            fprintf(f, "%d down %s\n%d up %s\n", t, i % 2 ? "slash" : "z", t + 8, i % 2 ? "slash" : "z");
        if ((t / 1000) % 3 == 0) {
            fprintf(f, "%d down space\n%d up space\n", t, t + 5);
            t += 20;
        }
        while (shot < 3 && t >= 3600 * (shot + 1) - 300) { /* just before each minute ends */
            fprintf(f, "%d screenshot %s\n", t, shots[shot]);
            shot++;
            t += 2;
        }
    }
    fclose(f);
}

/* Plays the regression script on the fixed clock in the game folder dir.
   h gets the three frames' hashes, *wav_hash and *wav_len the recording's.
   Returns the exit status; out gets stderr. */
static int play_regression(const char *dir, const regression_start *start, uint32_t h[3], uint32_t *wav_hash,
                           size_t *wav_len, char *out, size_t outlen) {
    char shots[3][1024];
    for (int i = 0; i < 3; i++)
        tmp_name(shots[i], sizeof shots[i], "shot");
    tmp_name(script_path, sizeof script_path, "script");
    write_regression_script(script_path, start, shots);
    tmp_name(wav_path, sizeof wav_path, "wav");
    test_tmp_dir(run_data, sizeof run_data);
    char ticks[16];
    snprintf(ticks, sizeof ticks, "%d", REGRESSION_TICKS);
    setenv("LOONY_EXIT_AFTER", ticks, 1);
    int status = test_run_child(run_loony_scripted, (void *)dir, out, outlen);
    unsetenv("LOONY_EXIT_AFTER");
    test_remove_tree(run_data);
    run_data[0] = '\0';
    unlink(script_path);
    for (int i = 0; i < 3; i++) {
        size_t len = 0;
        uint8_t *png = read_file(shots[i], &len);
        h[i] = png ? fnv1a32(png, len) : 0;
        free(png);
        unlink(shots[i]);
    }
    uint8_t *wav = read_file(wav_path, wav_len);
    *wav_hash = wav ? fnv1a32(wav, *wav_len) : 0;
    if (!wav)
        *wav_len = 0;
    free(wav);
    unlink(wav_path);
    wav_path[0] = '\0';
    return status;
}

SLOW_TEST(run_three_minutes_of_play_match_the_recording) {
    SKIP_UNLESS_GAME();
    uint32_t h[3], wh;
    size_t len;
    char out[32768];
    int status = play_regression(test_game_dir(), &classic_start, h, &wh, &len, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "runtime error"));
    CHECK_EQ(len, 44 + (size_t)REGRESSION_TICKS * 44100 / 60 * 4);
    CHECK_EQ(h[0], 0xAAD1E97Fu); /* minute 1: ball 1 in play */
    CHECK_EQ(h[1], 0x015482C8u); /* minute 2: ball 3, 13 seconds of demo time left */
    CHECK_EQ(h[2], 0x66E6FBF1u); /* minute 3: the time ran out; a new game, ball 1 */
    CHECK_EQ(wh, 0x3663C0FEu);
}

/* The same three minutes in Crystal Caliburn (the same keys). Recorded
   after the user's Plan 8 playtest. */
SLOW_TEST(run_three_minutes_of_crystal_caliburn_match_the_recording) {
    SKIP_UNLESS_CC();
    uint32_t h[3], wh;
    size_t len;
    char out[32768];
    int status = play_regression(test_cc_dir(), &classic_start, h, &wh, &len, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "runtime error"));
    CHECK_EQ(len, 44 + (size_t)REGRESSION_TICKS * 44100 / 60 * 4);
    char got[64];
    snprintf(got, sizeof got, "%08x %08x %08x %08x", h[0], h[1], h[2], wh);
    /* Frames at minutes 1, 2 and 3, then the recording. Minute 1: ball 1 in
       play, 26,780 points. Minute 2: ball 3, 13 seconds of demo time left.
       Minute 3: the time ran out; the attract display shows the copyright. */
    CHECK_STR(got, "18e601a7 bdd31ce9 88136063 f331fd82");
}

/* The same three minutes in Jinni Zeala, started from its menu. */
SLOW_TEST(run_three_minutes_of_jinni_zeala_match_the_recording) {
    SKIP_UNLESS_JZ();
    uint32_t h[3], wh;
    size_t len;
    char out[32768];
    int status = play_regression(test_jz_dir(), &jz_start, h, &wh, &len, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "runtime error"));
    CHECK_EQ(len, 44 + (size_t)REGRESSION_TICKS * 44100 / 60 * 4);
    char got[64];
    snprintf(got, sizeof got, "%08x %08x %08x %08x", h[0], h[1], h[2], wh);
    /* Frames at minutes 1, 2 and 3, then the recording. Minute 1: ball 1,
       25,500 points, 80 seconds of tryout time left. Minute 2: ball 3,
       201,900 points, 36 seconds left. Minute 3: the time ran out, GAME
       OVER and the best scores. */
    CHECK_STR(got, "6a123811 e569cd02 18e95058 72ae1f12");
}

/* The same three minutes in Tristan, started with Cmd-N. */
SLOW_TEST(run_three_minutes_of_tristan_match_the_recording) {
    SKIP_UNLESS_TR();
    uint32_t h[3], wh;
    size_t len;
    char out[32768];
    int status = play_regression(test_tr_dir(), &tr_start, h, &wh, &len, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "runtime error"));
    CHECK_EQ(len, 44 + (size_t)REGRESSION_TICKS * 44100 / 60 * 4);
    char got[64];
    snprintf(got, sizeof got, "%08x %08x %08x %08x", h[0], h[1], h[2], wh);
    /* Frames at minutes 1, 2 and 3, then the recording. Minute 1: the
       first game, ball 3, 12 seconds of demo time left. Minute 2: the
       second game, ball 3, 28 seconds left. Minute 3: the time ran out,
       TIME UP, BY LITTLEWING. */
    CHECK_STR(got, "536e1797 3dae4ac8 aadf91c8 54a37ce6");
}

/* Three minutes of MONSTER FAIR, started from its menu, with the same
   plunger, flippers and nudges. Recorded after the user's Plan 9 playtest. */
SLOW_TEST(run_three_minutes_of_monster_fair_match_the_recording) {
    SKIP_UNLESS_MF();
    uint32_t h[3], wh;
    size_t len;
    char out[32768];
    int status = play_regression(test_mf_app(), &mf_start, h, &wh, &len, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "runtime error"));
    CHECK_EQ(len, 44 + (size_t)REGRESSION_TICKS * 44100 / 60 * 4);
    char got[64];
    snprintf(got, sizeof got, "%08x %08x %08x %08x", h[0], h[1], h[2], wh);
    /* Frames at minutes 1, 2 and 3, then the recording. Minute 1: ball 1,
       82 seconds of tryout time left. Minute 2: ball 2, 161,500 points, 33
       seconds left. Minute 3: TIME UP, GAME OVER. */
    CHECK_STR(got, "f0797114 9453e91a f7f53dc9 ef2e653a");
}

/* Three minutes of Mad Daedalus, started from its menu, with the same
   plunger, flippers and nudges, its sound mixed through CoreAudio. */
SLOW_TEST(run_three_minutes_of_mad_daedalus_match_the_recording) {
    SKIP_UNLESS_MD();
    uint32_t h[3], wh;
    size_t len;
    char out[32768];
    int status = play_regression(test_md_app(), &md_start, h, &wh, &len, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "runtime error"));
    CHECK(!strstr(out, "coreaudio:"));
    /* Its clock moves in its own short sleeps, so the run ends at the first
       pump past the last tick: up to a tick more sound. */
    size_t want = 44 + (size_t)REGRESSION_TICKS * 44100 / 60 * 4;
    CHECK(len >= want && len < want + 44100 / 60 * 4);
    char got[64];
    snprintf(got, sizeof got, "%08x %08x %08x %08x", h[0], h[1], h[2], wh);
    /* Frames at minutes 1, 2 and 3, then the recording. Minute 1: ball 2,
       11,100 points, 68 seconds of tryout time left. Minute 2: the three
       balls are gone, GAME OVER at 155,300. Minute 3: the table plays its
       own demo, ball 1. */
    CHECK_STR(got, "ede7d3d1 af73d7df 3751c795 17b22851");
}

/* Three minutes of FAIRY TOWER, started from its menu as Mad Daedalus
   is, with the same plunger, flippers and nudges. */
SLOW_TEST(run_three_minutes_of_fairy_tower_match_the_recording) {
    SKIP_UNLESS_FT();
    uint32_t h[3], wh;
    size_t len;
    char out[32768];
    int status = play_regression(test_ft_app(), &ft_start, h, &wh, &len, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "runtime error"));
    CHECK(!strstr(out, "coreaudio:"));
    /* Its clock moves in its own short sleeps, so the run ends at the first
       pump past the last tick: up to a tick more sound. */
    size_t want = 44 + (size_t)REGRESSION_TICKS * 44100 / 60 * 4;
    CHECK(len >= want && len < want + 44100 / 60 * 4);
    char got[64];
    snprintf(got, sizeof got, "%08x %08x %08x %08x", h[0], h[1], h[2], wh);
    /* Frames at minutes 1, 2 and 3, then the recording. Minute 1: ball 2,
       58,000 points, 68 seconds of tryout time left. Minute 2: ball 3,
       228,000, 30 seconds left. Minute 3: the tryout's time ran out, GAME
       OVER at 249,200 and PRESS ESC TO START. */
    CHECK_STR(got, "46ed4eb5 4d0508d4 026fcd08 ba004ec5");
}

/* Three minutes of Angel Egg, started with Cmd-N after its title, with
   the same plunger, flippers and nudges. */
SLOW_TEST(run_three_minutes_of_angel_egg_match_the_recording) {
    SKIP_UNLESS_AE();
    uint32_t h[3], wh;
    size_t len;
    char out[32768];
    int status = play_regression(test_ae_app(), &ae_start, h, &wh, &len, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "runtime error"));
    /* The run ends at the first pump past the last tick: up to a tick
       more sound. */
    size_t want = 44 + (size_t)REGRESSION_TICKS * 44100 / 60 * 4;
    CHECK(len >= want && len < want + 44100 / 60 * 4);
    char got[64];
    snprintf(got, sizeof got, "%08x %08x %08x %08x", h[0], h[1], h[2], wh);
    /* Frames at minutes 1, 2 and 3, then the recording. Minute 1: ball 1,
       66 seconds of demo time left. Minute 2: ball 3, LOCK IS ENABLED.
       Minute 3: the demo time ran out; the attract display shows the high
       scores, and the sound stopped with it. */
    CHECK_STR(got, "13540246 05c8953e 3e47a0bd 91f706c2");
}

/* Three minutes of Golden Logres, started with Cmd-N, with the same
   plunger, flippers and nudges. */
SLOW_TEST(run_three_minutes_of_golden_logres_match_the_recording) {
    SKIP_UNLESS_GL();
    uint32_t h[3], wh;
    size_t len;
    char out[32768];
    int status = play_regression(test_gl_app(), &gl_start, h, &wh, &len, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "runtime error"));
    CHECK_EQ(len, 44 + (size_t)REGRESSION_TICKS * 44100 / 60 * 4);
    char got[64];
    snprintf(got, sizeof got, "%08x %08x %08x %08x", h[0], h[1], h[2], wh);
    /* Frames at minutes 1, 2 and 3, then the recording. Minute 1: ball 1,
       its Camelot bonus, 37 seconds of demo time left. Minute 2: ball 3,
       TIME UP. Minute 3: a second game, ball 1, 27 seconds left. */
    CHECK_STR(got, "a163e2bf efa67f8e 2759c976 c266ff4e");
}

/* Edits the snapshot section of the recording at path in place: edit gets
   the section, its length and the room after it, and returns its new
   length. False if the recording has no section, or can't be written. */
static bool edit_section(const char *path, size_t (*edit)(uint8_t *section, size_t len, size_t room)) {
    static uint8_t buf[8 << 20];
    gzFile g = gzopen(path, "rb");
    int n = g ? gzread(g, buf, sizeof buf) : -1;
    if (g)
        gzclose(g);
    if (n < 16 || memcmp(buf + n - 8, "LWSTART6", 8) != 0)
        return false;
    uint64_t len = 0;
    for (int b = 0; b < 8; b++)
        len |= (uint64_t)buf[n - 16 + b] << (8 * b);
    size_t at = (size_t)n - 16 - (size_t)len;
    size_t new_len = edit(buf + at, (size_t)len, sizeof buf - at - 16);
    for (int b = 0; b < 8; b++)
        buf[at + new_len + (size_t)b] = (uint8_t)((uint64_t)new_len >> (8 * b));
    memcpy(buf + at + new_len + 8, "LWSTART6", 8);
    size_t total = at + new_len + 16;
    g = gzopen(path, "wb6");
    bool ok = g && gzwrite(g, buf, (unsigned)total) == (int)total;
    if (g && gzclose(g) != Z_OK)
        ok = false;
    return ok;
}

static uint64_t le_at(const uint8_t *p, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void put_le(uint8_t *p, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* Where the snapshot's encoding starts in a section (gamestart.c): after
   the header, its 116 bytes ending with the count of resources left out,
   those resources (12 bytes each) and the encoding's length. In the
   encoding (snapshot.c): the program counter after 28 bytes of header and
   the 64 general and floating-point registers, and the module state's
   length after the CPU, both levels and the frame. */
#define SECTION_BODY(s) (116 + 12 * (size_t)le_at((s) + 112, 4) + 8)
#define BODY_PC (28 + 64 * 8)
#define BODY_STATE_LEN (28 + 3 * 71 * 8 + 2 * 12 + 32)

/* The baseline's hash, as another copy of the game's would be. */
static size_t damage_baseline(uint8_t *section, size_t len, size_t room) {
    (void)room;
    section[8 + 8 + 32 + 16] ^= 0xFF;
    return len;
}

/* The snapshot's layout, as another version's would be. */
static size_t other_layout(uint8_t *section, size_t len, size_t room) {
    (void)room;
    section[8] ^= 1;
    return len;
}

/* The replay's place among the recording's records, in its block of the
   module state (the last 8 bytes, replay.c's walk), back at the first:
   a snapshot that fits every check and goes out of step at once, as one
   would whose meaning changed with no bump (gamestart.h). The module
   state's header is 16 bytes; each block, its tag, its size (32 bits
   each) and its length (64) before what it holds. */
static size_t rewound(uint8_t *section, size_t len, size_t room) {
    (void)room;
    uint8_t *state = section + SECTION_BODY(section) + BODY_STATE_LEN + 8;
    uint64_t state_len = le_at(state - 8, 8);
    uint32_t tag = fnv1a32("replay.c:R", strlen("replay.c:R"));
    for (uint64_t at = 16; at + 16 <= state_len;) {
        uint64_t n = le_at(state + at + 8, 8);
        if (le_at(state + at, 4) == tag && n >= 8) {
            put_le(state + at + 16 + n - 8, 0, 8);
            return len;
        }
        at += 16 + n;
    }
    return len; /* not found: the replay won't go out of step, and the test says so */
}

/* The program counter, in the game's data. */
static size_t pc_in_data(uint8_t *section, size_t len, size_t room) {
    (void)room;
    put_le(section + SECTION_BODY(section) + BODY_PC, 0x00500000u, 4);
    return len;
}

/* One more page, the first of the game's code (the PEF's first section,
   at the start of the image), with a byte changed. */
static size_t code_page(uint8_t *section, size_t len, size_t room) {
    size_t page = (size_t)getpagesize(), body = SECTION_BODY(section);
    if (room < 4 + page)
        return len;
    uint64_t state_len = le_at(section + body + BODY_STATE_LEN, 8);
    uint8_t *count = section + body + BODY_STATE_LEN + 8 + state_len;
    put_le(count, le_at(count, 4) + 1, 4);
    put_le(section + len, 0x00100000u / page, 4);
    memset(section + len + 4, 0, page);
    section[len + 4 + 0x100] = 1;
    put_le(section + body - 8, le_at(section + body - 8, 8) + 4 + page, 8);
    return len + 4 + page;
}

/* The preferences a replay in play_dir/data-name left. */
static char *prefs_of(const char *play_dir, const char *name) {
    char path[1200];
    snprintf(path, sizeof path, "%s/data-%s/prefs.plist", play_dir, name);
    return read_text(path);
}

/* Writes Loony Labyrinth's preferences, registered to email with key,
   into dir (made). */
static bool write_license(const char *dir, const char *email, const char *key) {
    char path[1200];
    snprintf(path, sizeof path, "%s/prefs.plist", dir);
    if (!make_dirs(dir))
        return false;
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>"
               "<key>user email</key><string>%s</string><key>user id</key><string>%s</string>"
               "</dict></plist>\n",
            email, key);
    return fclose(f) == 0;
}

/* A game recorded after the opening (gamestart.h): its first replay plays
   from launch and keeps a snapshot of the game's start in the recording;
   the second goes to the game's start from the baseline and plays on with
   every frame check matching, to the same last frame and the same
   preferences. Verifying, a replay from launch is the snapshot where it
   was taken. A snapshot whose baseline doesn't match, whose program
   counter is in the game's data, or which changes the game's code, is
   said not to fit, and replaced, and so is one from a build whose
   snapshots are laid out otherwise. One that fits but goes out of step is
   dropped, and the replay starts again from launch and keeps it again.
   LOONY_GAMESTART=0 neither uses one nor takes one. */
SLOW_TEST(run_a_recorded_game_starts_at_the_game_from_its_snapshot) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    static char out[10][65536];
    /* Registered, so the recording holds preferences, as the app's do: a
       restart from launch must replay into a folder without them. */
    bool licensed = write_license(rec_dir, TEST_EMAIL, TEST_KEY);
    int rec_status = record_one_game(rec_dir, true, out[0], sizeof out[0]);
    uint32_t shot[9], wav[9];
    int status[9];
    status[0] = replay_hashed(play_dir, "first", out[1], sizeof out[1], &shot[0], &wav[0]);
    /* The rest at once, each of a copy of the recording with the snapshot
       the first kept, edited for the ones it shouldn't fit. */
    side_replay r[8] = {{.tag = "second"},
                        {.tag = "verify", .name = "LOONY_SNAPSHOT_VERIFY", .value = "1"},
                        {.tag = "baseline"},
                        {.tag = "pc"},
                        {.tag = "code"},
                        {.tag = "off", .name = "LOONY_GAMESTART", .value = "0"},
                        {.tag = "layout"},
                        {.tag = "rewound"}};
    char files[8][1200];
    bool copied = true;
    for (int i = 0; i < 8; i++) {
        copied = copy_recording(replay_file, rec_dir, r[i].tag, files[i], sizeof files[i]) && copied;
        r[i].file = files[i];
    }
    bool edited[5] = {edit_section(files[2], damage_baseline), edit_section(files[3], pc_in_data),
                      edit_section(files[4], code_page), edit_section(files[6], other_layout),
                      edit_section(files[7], rewound)};
    replays_side_by_side(play_dir, r, 8, out + 2, status + 1, shot + 1, wav + 1);
    char *prefs[2] = {prefs_of(play_dir, "first"), prefs_of(play_dir, "second")};
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK(licensed);
    CHECK_EQ(rec_status, 0);
    CHECK(copied);
    for (int i = 0; i < 9; i++) {
        CHECK_EQ(status[i], 0);
        CHECK_CONTAINS(out[i + 1], "loony: replay: reached the end of the recording");
        if (i != 8)
            CHECK(!strstr(out[i + 1], "replay: desync"));
        CHECK_EQ(frames_checked(out[i + 1]), frames_checked(out[1]));
        CHECK_EQ(shot[i], shot[0]);
    }
    CHECK(shot[0] != 0);
    CHECK_CONTAINS(out[1], "gamestart: kept a snapshot of the game's start at ");
    CHECK(!strstr(out[1], "gamestart: going to"));
    CHECK_CONTAINS(out[2], "gamestart: going to the game's start at ");
    CHECK_CONTAINS(out[2], " from main, from 8.0");
    CHECK(!strstr(out[2], "gamestart: kept"));
    CHECK(prefs[0] && prefs[1]);
    CHECK_STR(prefs[1], prefs[0]);
    free(prefs[0]);
    free(prefs[1]);
    CHECK_CONTAINS(out[3], "gamestart: verify: decoded the recording's snapshot at the baseline");
    CHECK_CONTAINS(out[3], " the replay from launch is the recording's snapshot");
    CHECK_EQ(wav[2], wav[0]); /* from launch, it sounds the same */
    for (int i = 0; i < 5; i++)
        CHECK(edited[i]);
    CHECK_CONTAINS(out[4], "gamestart: the recording's snapshot can't be used: the replay's baseline");
    CHECK_CONTAINS(out[5], "gamestart: the recording's snapshot can't be used: the CPU's pc is "
                           "0x00500000, outside the game's code; replaying from launch");
    CHECK_CONTAINS(out[6], "gamestart: the recording's snapshot can't be used: it changes the game's "
                           "code, at 0x00100100; replaying from launch");
    for (int i = 4; i <= 6; i++)
        CHECK_CONTAINS(out[i], "gamestart: kept a snapshot of the game's start at ");
    CHECK(!strstr(out[7], "gamestart:"));
    CHECK_EQ(wav[6], wav[0]);
    CHECK_CONTAINS(out[8], "gamestart: the recording's snapshot can't be used: it was taken by a "
                           "version of LittleWing whose snapshots differ");
    CHECK_CONTAINS(out[8], "gamestart: kept a snapshot of the game's start at ");
    /* Rewound: it goes to the game's start, goes out of step, drops the
       snapshot and starts again from launch, which keeps it again. */
    CHECK_CONTAINS(out[9], "gamestart: going to the game's start at ");
    CHECK_CONTAINS(out[9], "replay: desync at step ");
    CHECK_CONTAINS(out[9], "so the snapshot is dropped, for a replay from launch to take it again");
    CHECK(!strstr(out[9], "already has preferences"));
    CHECK_CONTAINS(out[9], "replay: starting again from launch");
    CHECK_CONTAINS(out[9], "gamestart: kept a snapshot of the game's start at ");
    CHECK_EQ(wav[8], wav[0]);
}

/* A take (LOONY_GAMESTART=take, gamestart_take_start), which draws
   nothing, keeps the same snapshot of the game's start, byte for byte, as
   a replay that draws every frame. */
SLOW_TEST(run_a_take_keeps_the_snapshot_a_drawing_replay_keeps) {
    SKIP_UNLESS_GAME();
    char rec_dir[1024], play_dir[1024], taken[1200], err[256] = "";
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    static char out[2][65536];
    int rec_status = record_one_game(rec_dir, true, out[0], sizeof out[0]);
    snprintf(taken, sizeof taken, "%s/taken.lwreplay", play_dir);
    bool copied = copy_file_to(replay_file, taken);
    uint32_t shot, wav;
    int status = replay_hashed(play_dir, "drawing", out[1], sizeof out[1], &shot, &wav);
    bool started = copied && gamestart_take_start(LOONY_BIN, taken, err, sizeof err), kept = false;
    for (int i = 0; started && i < 6000 && !gamestart_take_poll(&kept); i++)
        usleep(100000);
    uint8_t *drawn = NULL, *took = NULL, id[2][32];
    size_t drawn_len = 0, took_len = 0;
    bool read[2] = {replay_file_snapshot(replay_file, &drawn, &drawn_len, id[0], err, sizeof err),
                    replay_file_snapshot(taken, &took, &took_len, id[1], err, sizeof err)};
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    CHECK_EQ(rec_status, 0);
    CHECK(copied);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out[1], "gamestart: kept a snapshot of the game's start at ");
    CHECK(started);
    CHECK(kept);
    CHECK(read[0] && read[1]);
    CHECK(drawn && took);
    CHECK_EQ(took_len, drawn_len);
    CHECK(drawn && took && took_len == drawn_len && memcmp(drawn, took, took_len) == 0);
    free(drawn);
    free(took);
}


/* A home folder of its own, as another Mac's, with Loony Labyrinth
   registered there: its path in home. */
static bool other_home(char *home, size_t cap, const char *email, const char *key) {
    test_tmp_dir(home, cap);
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/Library/Application Support/littlewing-shim/loony-labyrinth", home);
    return write_license(dir, email, key);
}

/* A shared copy of a recorded game (replay_share) takes a snapshot of the
   game's start the way the High Scores menu has it taken, by a second copy
   of LittleWing replaying the shared copy (gamestart_take_start), under the
   sharer's home folder. Replayed under another home folder, as on the
   recipient's Mac, it goes to the game's start from that snapshot and
   ends on the same frame as a replay of it from launch. */
SLOW_TEST(run_a_shared_game_starts_at_the_game_on_another_install) {
    SKIP_UNLESS_GAME();
    const char *email = TEST_EMAIL, *key = TEST_KEY;
    char rec_dir[1024], play_dir[1024], sharer[1024], viewer[1024], shared[1200], err[1024];
    test_tmp_dir(rec_dir, sizeof rec_dir);
    test_tmp_dir(play_dir, sizeof play_dir);
    bool homes = write_license(rec_dir, email, key) && other_home(sharer, sizeof sharer, email, key) &&
                 other_home(viewer, sizeof viewer, email, key);
    static char out[3][65536];
    int rec_status = record_one_game(rec_dir, true, out[0], sizeof out[0]);
    snprintf(shared, sizeof shared, "%s/shared.lwreplay", play_dir);
    bool share_ok = replay_share(replay_file, shared, err, sizeof err);
    bool had = gamestart_in_file(shared);
    setenv("HOME", sharer, 1);
    bool started = gamestart_take_start(LOONY_BIN, shared, err, sizeof err), kept = false;
    for (int i = 0; started && i < 6000 && !gamestart_take_poll(&kept); i++)
        usleep(100000);
    /* The replays below have a data folder of their own (LOONY_DATA_DIR), so
       the viewer's license is named, as it would be found under HOME. */
    char license[1300];
    snprintf(license, sizeof license, "%s/Library/Application Support/littlewing-shim/loony-labyrinth/prefs.plist",
             viewer);
    setenv("HOME", viewer, 1);
    setenv("LOONY_REPLAY_LICENSE", license, 1);
    snprintf(replay_file, sizeof replay_file, "%s", shared);
    uint32_t shot[2], wav[2];
    int status[2];
    /* At once: the second neither uses the snapshot nor takes one. */
    side_replay r[2] = {{.file = shared, .tag = "viewer"},
                        {.file = shared, .tag = "launch", .name = "LOONY_GAMESTART", .value = "0"}};
    replays_side_by_side(play_dir, r, 2, out + 1, status, shot, wav);
    test_remove_tree(rec_dir);
    test_remove_tree(play_dir);
    test_remove_tree(sharer);
    test_remove_tree(viewer);
    CHECK(homes);
    CHECK_EQ(rec_status, 0);
    CHECK(share_ok);
    CHECK(!had);
    CHECK(started);
    CHECK(kept);
    for (int i = 0; i < 2; i++) {
        CHECK_EQ(status[i], 0);
        CHECK_CONTAINS(out[i + 1], "loony: replay: reached the end of the recording");
        CHECK(!strstr(out[i + 1], "replay: desync"));
    }
    CHECK_CONTAINS(out[1], "gamestart: going to the game's start at ");
    CHECK(!strstr(out[1], "gamestart: kept"));
    CHECK(shot[0] != 0);
    CHECK_EQ(shot[0], shot[1]);
    CHECK_EQ(frames_checked(out[1]), frames_checked(out[2]));
    CHECK(!strstr(out[1], email));
    CHECK(!strstr(out[1], key));
}
