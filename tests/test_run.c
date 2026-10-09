#include "test.h"

#include <stdlib.h>
#include <unistd.h>

#include "cgimage.h"
#include "plist.h"
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
    CHECK_CONTAINS(out, "207 imports, main at 0x41ca8, 13 initializers");
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

/* MONSTER FAIR's license tests take the address and key code from
   LOONY_TEST_MF_EMAIL and LOONY_TEST_MF_KEY; the public ones in
   docs/test_key.txt are a key the game accepts at first and refuses in play
   (the address ends in an IP address). Sets *email and *key, or skips. */
#define MF_TEST_KEY_OR_SKIP(email, key)                                            \
    do {                                                                           \
        email = getenv("LOONY_TEST_MF_EMAIL");                                     \
        key = getenv("LOONY_TEST_MF_KEY");                                         \
        if (!email || !*email || !key || !*key) {                                  \
            test_skip("LOONY_TEST_MF_EMAIL and LOONY_TEST_MF_KEY aren't set");     \
            return;                                                                \
        }                                                                          \
    } while (0)

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

/* With LOONY_MF_SKIP_LICENSE_RECHECK=1 the test key survives the second
   check: the game plays on until the script quits, keeps the license, and
   the next launch skips the Welcome window. */
TEST(run_monster_fair_keeps_a_license_with_the_recheck_skipped) {
    SKIP_UNLESS_MF();
    const char *email, *key;
    MF_TEST_KEY_OR_SKIP(email, key);
    test_tmp_dir(run_data, sizeof run_data);
    setenv("LOONY_MF_SKIP_LICENSE_RECHECK", "1", 1);
    char out[32768], again[32768], *xml;
    int status = mf_register_and_play_past_the_recheck(email, key, out, sizeof out, &xml);
    setenv("LOONY_EXIT_AFTER", "300", 1);
    int status2 = run_script_in(test_mf_app(), "", NULL, NULL, 0, again, sizeof again);
    unsetenv("LOONY_EXIT_AFTER");
    unsetenv("LOONY_MF_SKIP_LICENSE_RECHECK");
    test_remove_tree(run_data);
    run_data[0] = '\0';
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: LOONY_MF_SKIP_LICENSE_RECHECK: MONSTER FAIR's second license check is off");
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
   ~/Library/Application Support/loony-shim/<id>, and Loony Labyrinth's
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
    snprintf(root, sizeof root, "%s/Library/Application Support/loony-shim", save_home);
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
    snprintf(root, sizeof root, "%s/Library/Application Support/loony-shim", save_home);
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

/* Launched as the app, the log goes to ~/Library/Logs/loony-shim instead
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
    snprintf(log, sizeof log, "%s/Library/Logs/loony-shim/loony.log", app_home);
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
    snprintf(log, sizeof log, "%s/Library/Logs/loony-shim/loony.previous.log", app_home);
    CHECK(access(log, F_OK) == 0);
    test_remove_tree(app_home);
}

static char bundle_bin[1300];

static void run_bundle_scripted(void *dir) {
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
    char icns[1024];
    snprintf(icns, sizeof icns, "%s/LittleWing.app/Contents/Resources/AppIcon.icns", out_dir);
    CHECK(access(icns, R_OK) == 0);
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
    test_remove_tree(out_dir);
    CHECK_EQ(status, 0);
    CHECK(shot != NULL);
    CHECK_EQ(fnv1a32(shot, len), 0xADE78151u); /* the menu */
    free(shot);
}

/* The regression run: three minutes of scripted play on the fixed clock,
   from the opening through a game (plunger, flippers, nudges), with frames
   at each minute and the whole recording hashed. Any change to the
   emulation, the physics the game computes, drawing or sound shows up here.
   Recorded on 2026-10-02 after the user approved Plan 6's build. */
#define REGRESSION_TICKS 10800

/* How a game starts: the keys, then the tick of the first plunger. */
typedef struct {
    const char *keys;
    int play_from;
} regression_start;

/* The classic games: Esc ends the demo, Esc opens the menu, Return twice. */
static const regression_start classic_start = {
    "1720 down esc\n1724 up esc\n1800 down esc\n1804 up esc\n"
    "1900 down return\n1906 up return\n2000 down return\n2006 up return\n",
    2100};

/* MONSTER FAIR: before its table plays by itself, one Esc opens the menu;
   Return picks NEW GAME, then 1 PLAYER. */
static const regression_start mf_start = {
    "2200 down esc\n2204 up esc\n2320 down return\n2326 up return\n2440 down return\n2446 up return\n", 2600};

static void write_regression_script(const char *path, const regression_start *start, char shots[3][1024]) {
    FILE *f = fopen(path, "w");
    fputs(start->keys, f);
    int shot = 0;
    for (int t = start->play_from; t < REGRESSION_TICKS - 200;) {
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

TEST(run_three_minutes_of_play_match_the_recording) {
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
TEST(run_three_minutes_of_crystal_caliburn_match_the_recording) {
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

/* Three minutes of MONSTER FAIR, started from its menu, with the same
   plunger, flippers and nudges. Recorded after the user's Plan 9 playtest. */
TEST(run_three_minutes_of_monster_fair_match_the_recording) {
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
