#include "test.h"

#include <stdlib.h>
#include <unistd.h>

#include "util.h"

static void run_loony(void *dir) {
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
    int status = test_run_child(run_loony_headless, (void *)test_game_dir(), out, sizeof out);
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
   golden hashes (FNV-1a32 of the PNG file, which is uncompressed). The
   goldens were recorded from runs whose frames were checked by eye: the
   title, and a one-player game started from the menu. */

static char script_path[1024];

static void run_loony_scripted(void *dir) {
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    setenv("LOONY_SCRIPT", script_path, 1);
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
    int status = test_run_child(run_loony_scripted, (void *)test_game_dir(), out, outlen);
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
