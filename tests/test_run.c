#include "test.h"

#include <stdlib.h>
#include <unistd.h>

#include "util.h"

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

/* Runs the game with a script (lines in any order, sorted by tick here),
   returning the exit status; shots[i] gets the hash of the screenshot taken
   at ticks[i]. */
static int run_script(const char *actions, const uint32_t *ticks, uint32_t *shots, int nshots,
                      char *out, size_t outlen) {
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
    int status = test_run_child(run_loony_scripted, (void *)test_game_dir(), out, outlen);
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
   frames on (pending: shown in the Plan 6 handoff). */
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

/* Registers with a real key code, given at run time (never stored):
   LOONY_TEST_EMAIL and LOONY_TEST_KEY. The license is kept in the
   preferences, so the next launch skips the shareware alerts. */
TEST(run_a_key_code_registers_and_survives_a_relaunch) {
    SKIP_UNLESS_GAME();
    const char *email = getenv("LOONY_TEST_EMAIL"), *key = getenv("LOONY_TEST_KEY");
    if (!email || !*email || !key || !*key) {
        test_skip("LOONY_TEST_EMAIL and LOONY_TEST_KEY aren't set");
        return;
    }
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
TEST(run_reports_missing_game_folder) {
    char out[4096];
    int status = test_run_child(run_loony, (void *)"/nonexistent/loony", out, sizeof out);
    CHECK_EQ(status, 1);
    CHECK_CONTAINS(out, "can't read /nonexistent/loony/LOONY LABYRINTH 3.0.1");
}
