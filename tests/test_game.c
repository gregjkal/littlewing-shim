#include "test.h"

#include <stdlib.h>
#include <unistd.h>

#include "game.h"
#include "plist.h"
#include "util.h"

static void touch(const char *dir, const char *name) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (f)
        fclose(f);
}

/* Sets LOONY_APPS_DIR for one test; restore_apps_dir puts back the old value. */
static char saved_apps[1024];
static bool had_apps;

static void set_apps_dir(const char *dir) {
    const char *a = getenv("LOONY_APPS_DIR");
    had_apps = a != NULL;
    snprintf(saved_apps, sizeof saved_apps, "%s", a ? a : "");
    setenv("LOONY_APPS_DIR", dir, 1);
}

static void restore_apps_dir(void) {
    if (had_apps)
        setenv("LOONY_APPS_DIR", saved_apps, 1);
    else
        unsetenv("LOONY_APPS_DIR");
}

TEST(game_table_knows_six_games) {
    CHECK_EQ(game_count(), 6);
    CHECK_STR(game_at(0)->id, "loony-labyrinth");
    CHECK_STR(game_at(1)->id, "crystal-caliburn");
    CHECK_STR(game_at(2)->id, "monster-fair");
    CHECK_STR(game_at(3)->id, "jinni-zeala");
    CHECK_STR(game_at(4)->id, "mad-daedalus");
    CHECK_STR(game_at(5)->id, "angel-egg");
    CHECK(game_at(6) == NULL);
    CHECK_EQ(game_at(0)->kind, GAME_PEF_FOLDER);
    CHECK_EQ(game_at(1)->kind, GAME_PEF_FOLDER);
    const game_info *jz = game_by_id("jinni-zeala");
    CHECK_EQ(jz->kind, GAME_PEF_FOLDER);
    CHECK_STR(jz->title, "Jinni Zeala");
    CHECK_STR(jz->folder_name, "Jinni Zeala");
    CHECK_STR(jz->exe, "JINNI ZEALA 1.2.2");
    const game_info *mf = game_by_id("monster-fair");
    CHECK_EQ(mf->kind, GAME_MACHO_BUNDLE);
    CHECK_STR(mf->title, "MONSTER FAIR");
    CHECK_STR(mf->folder_name, "MONSTER FAIR.app");
    CHECK_STR(mf->exe, "Contents/MacOS/MONSTER FAIR");
    const game_info *md = game_by_id("mad-daedalus");
    CHECK_EQ(md->kind, GAME_MACHO_BUNDLE);
    CHECK_STR(md->title, "Mad Daedalus");
    CHECK_STR(md->folder_name, "Mad Daedalus.app");
    CHECK_STR(md->exe, "Contents/MacOS/Mad Daedalus");
    CHECK_EQ(md->screen_width, 1024);
    CHECK_EQ(md->screen_height, 768);
    const game_info *ae = game_by_id("angel-egg");
    CHECK_EQ(ae->kind, GAME_MACHO_BUNDLE);
    CHECK_STR(ae->title, "Angel Egg");
    CHECK_STR(ae->folder_name, "Angel Egg.app");
    CHECK_STR(ae->exe, "Contents/MacOS/Angel Egg");
    CHECK_EQ(ae->screen_width, 1024);
    CHECK_EQ(ae->screen_height, 768);
    CHECK_EQ(mf->screen_width, 800);
    CHECK_EQ(mf->screen_height, 600);
    const game_info *cc = game_by_id("crystal-caliburn");
    CHECK(cc != NULL);
    CHECK_STR(cc->title, "Crystal Caliburn");
    CHECK_STR(cc->folder_name, "Crystal Caliburn");
    CHECK_STR(cc->exe, "CRYSTAL CALIBURN 3.0.1");
    CHECK_STR(game_by_id("loony-labyrinth")->exe, "LOONY LABYRINTH 3.0.1");
    CHECK(game_by_id("pacman") == NULL);
}

TEST(game_folders_are_under_the_apps_dir) {
    char def[1024], out[1024];
    set_apps_dir(""); /* empty means unset */
    snprintf(def, sizeof def, "%s", game_apps_dir());
    setenv("LOONY_APPS_DIR", "/tmp/apps", 1);
    game_folder(game_at(0), out, sizeof out);
    restore_apps_dir();
    CHECK_STR(def, "/Applications");
    CHECK_STR(out, "/tmp/apps/Loony Labyrinth");
}

/* Makes <apps>/<sub>/Loony Labyrinth with its program; sub may be "". */
static void put_loony(const char *apps, const char *sub) {
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/%s/Loony Labyrinth", apps, sub);
    make_dirs(dir);
    touch(dir, "LOONY LABYRINTH 3.0.1");
}

TEST(game_folder_looks_one_folder_down) {
    char apps[1024], found[1200], want[1200];
    test_tmp_dir(apps, sizeof apps);
    set_apps_dir(apps);
    const game_info *ll = game_by_id("loony-labyrinth");
    put_loony(apps, "Pinball");
    game_folder(ll, found, sizeof found);
    const game_info *g[4];
    int n = game_installed(g, 4);
    restore_apps_dir();
    test_remove_tree(apps);
    snprintf(want, sizeof want, "%s/Pinball/Loony Labyrinth", apps);
    CHECK_STR(found, want);
    CHECK_EQ(n, 1);
    CHECK_STR(g[0]->id, "loony-labyrinth");
}

TEST(game_folder_prefers_the_apps_dir_then_the_first_folder_by_name) {
    char apps[1024], first[1200], top[1200], want[1200];
    test_tmp_dir(apps, sizeof apps);
    set_apps_dir(apps);
    const game_info *ll = game_by_id("loony-labyrinth");
    put_loony(apps, "Zed");
    put_loony(apps, "Games");
    game_folder(ll, first, sizeof first);
    put_loony(apps, "");
    game_folder(ll, top, sizeof top);
    restore_apps_dir();
    test_remove_tree(apps);
    snprintf(want, sizeof want, "%s/Games/Loony Labyrinth", apps);
    CHECK_STR(first, want);
    snprintf(want, sizeof want, "%s/Loony Labyrinth", apps);
    CHECK_STR(top, want);
}

TEST(game_folder_skips_apps_and_hidden_folders) {
    char apps[1024], found[1200], want[1200];
    test_tmp_dir(apps, sizeof apps);
    set_apps_dir(apps);
    put_loony(apps, "Other.app");
    put_loony(apps, ".hidden");
    game_folder(game_by_id("loony-labyrinth"), found, sizeof found);
    const game_info *g[4];
    int n = game_installed(g, 4);
    restore_apps_dir();
    test_remove_tree(apps);
    snprintf(want, sizeof want, "%s/Loony Labyrinth", apps);
    CHECK_STR(found, want); /* where to put it */
    CHECK_EQ(n, 0);
}

TEST(game_in_folder_finds_the_program_that_is_there) {
    char dir[1024];
    test_tmp_dir(dir, sizeof dir);
    CHECK(game_in_folder(dir) == NULL);
    touch(dir, "CRYSTAL CALIBURN 3.0.1");
    const game_info *g = game_in_folder(dir);
    touch(dir, "LOONY LABYRINTH 3.0.1");
    const game_info *both = game_in_folder(dir);
    test_remove_tree(dir);
    CHECK(g != NULL);
    CHECK_STR(g->id, "crystal-caliburn");
    CHECK_STR(both->id, "loony-labyrinth"); /* table order */
}

TEST(game_installed_lists_what_is_in_the_apps_dir) {
    char apps[1024], dir[1200];
    test_tmp_dir(apps, sizeof apps);
    set_apps_dir(apps);
    const game_info *g[4];
    int none = game_installed(g, 4);
    snprintf(dir, sizeof dir, "%s/Crystal Caliburn", apps);
    make_dirs(dir);
    touch(dir, "CRYSTAL CALIBURN 3.0.1");
    int one = game_installed(g, 4);
    const game_info *first = g[0];
    snprintf(dir, sizeof dir, "%s/Loony Labyrinth", apps);
    make_dirs(dir);
    touch(dir, "LOONY LABYRINTH 3.0.1");
    int two = game_installed(g, 4);
    restore_apps_dir();
    test_remove_tree(apps);
    CHECK_EQ(none, 0);
    CHECK_EQ(one, 1);
    CHECK_STR(first->id, "crystal-caliburn");
    CHECK_EQ(two, 2);
    CHECK_STR(g[0]->id, "loony-labyrinth");
    CHECK_STR(g[1]->id, "crystal-caliburn");
}

/* A bundle with its program at Contents/MacOS/MONSTER FAIR. */
static void make_bundle(const char *apps) {
    char macos[1200];
    snprintf(macos, sizeof macos, "%s/MONSTER FAIR.app/Contents/MacOS", apps);
    make_dirs(macos);
    touch(macos, "MONSTER FAIR");
}

TEST(game_installed_finds_a_bundle) {
    char apps[1024], dir[1200];
    test_tmp_dir(apps, sizeof apps);
    set_apps_dir(apps);
    const game_info *g[4];
    /* A bundle without its program isn't installed. */
    snprintf(dir, sizeof dir, "%s/MONSTER FAIR.app/Contents", apps);
    make_dirs(dir);
    int empty = game_installed(g, 4);
    make_bundle(apps);
    snprintf(dir, sizeof dir, "%s/Loony Labyrinth", apps);
    make_dirs(dir);
    touch(dir, "LOONY LABYRINTH 3.0.1");
    int two = game_installed(g, 4);
    char folder[1200];
    game_folder(g[1], folder, sizeof folder);
    restore_apps_dir();
    test_remove_tree(apps);
    CHECK_EQ(empty, 0);
    CHECK_EQ(two, 2);
    CHECK_STR(g[0]->id, "loony-labyrinth");
    CHECK_STR(g[1]->id, "monster-fair");
    snprintf(dir, sizeof dir, "%s/MONSTER FAIR.app", apps);
    CHECK_STR(folder, dir);
}

TEST(game_in_folder_finds_monster_fair_in_its_bundle) {
    char apps[1024], bundle[1200];
    test_tmp_dir(apps, sizeof apps);
    make_bundle(apps);
    snprintf(bundle, sizeof bundle, "%s/MONSTER FAIR.app", apps);
    const game_info *g = game_in_folder(bundle);
    test_remove_tree(apps);
    CHECK(g != NULL);
    CHECK_STR(g->id, "monster-fair");
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static bool file_says(const char *path, const char *text) {
    size_t len = 0;
    char *got = (char *)read_file(path, &len);
    bool same = got && len == strlen(text) && memcmp(got, text, len) == 0;
    free(got);
    return same;
}

/* Review Focus 1: before Plan 8 Loony Labyrinth saved straight into the root. */
TEST(game_legacy_data_moves_into_loony_labyrinth) {
    char root[1024], path[1300];
    test_tmp_dir(root, sizeof root);
    snprintf(path, sizeof path, "%s/prefs.plist", root);
    write_text(path, "license");
    snprintf(path, sizeof path, "%s/" GAME_PICKER_FILE, root);
    write_text(path, "picker");
    snprintf(path, sizeof path, "%s/crystal-caliburn", root);
    make_dirs(path);
    CHECK_EQ(game_move_legacy_data(root), 1);
    snprintf(path, sizeof path, "%s/loony-labyrinth/prefs.plist", root);
    CHECK(file_says(path, "license"));
    snprintf(path, sizeof path, "%s/prefs.plist", root);
    CHECK(access(path, F_OK) != 0);
    snprintf(path, sizeof path, "%s/" GAME_PICKER_FILE, root);
    CHECK(file_says(path, "picker")); /* the picker's own file stays */
    snprintf(path, sizeof path, "%s/crystal-caliburn", root);
    CHECK(access(path, F_OK) == 0);
    CHECK_EQ(game_move_legacy_data(root), 0); /* nothing left to move */
    test_remove_tree(root);
}

TEST(game_legacy_data_never_overwrites) {
    char root[1024], path[1300];
    test_tmp_dir(root, sizeof root);
    snprintf(path, sizeof path, "%s/loony-labyrinth", root);
    make_dirs(path);
    snprintf(path, sizeof path, "%s/loony-labyrinth/prefs.plist", root);
    write_text(path, "newer");
    snprintf(path, sizeof path, "%s/prefs.plist", root);
    write_text(path, "older");
    CHECK_EQ(game_move_legacy_data(root), 0);
    snprintf(path, sizeof path, "%s/loony-labyrinth/prefs.plist", root);
    CHECK(file_says(path, "newer"));
    snprintf(path, sizeof path, "%s/prefs.plist", root);
    CHECK(file_says(path, "older")); /* left in place, and logged */
    test_remove_tree(root);
}

TEST(game_legacy_data_with_no_root_is_a_no_op) {
    CHECK_EQ(game_move_legacy_data("/nonexistent/littlewing-shim"), 0);
}

static time_t local_time(int y, int mo, int d, int h, int mi, int sec) {
    struct tm tm = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h,
                    .tm_min = mi, .tm_sec = sec, .tm_isdst = -1};
    return mktime(&tm);
}

/* The times of Loony Labyrinth's high scores, as its preferences hold them;
   the score the game starts with has none. */
TEST(game_high_score_times_come_from_the_preferences) {
    char dir[1024], prefs[1100], err[256];
    test_tmp_dir(dir, sizeof dir);
    snprintf(prefs, sizeof prefs, "%s/prefs.plist", dir);
    plist_entry e[] = {
        {"highscore 1", true, 43074596, NULL},
        {"highscore timestamp 1", false, 0, "2026-10-14 15:20:00"},
        {"highscore timestamp 2", false, 0, ""},
        {"highscore timestamp 3", false, 0, "2026-10-07 10:47:40"},
        {"highscore name 1", false, 0, "GREG"},
    };
    CHECK(plist_write(prefs, e, 5, err, sizeof err));
    time_t t[8];
    size_t n = game_high_score_times(prefs, t, 8);
    CHECK_EQ(n, 2);
    time_t a = local_time(2026, 10, 14, 15, 20, 0), b = local_time(2026, 10, 7, 10, 47, 40);
    CHECK((t[0] == a && t[1] == b) || (t[0] == b && t[1] == a)); /* in no set order */
    CHECK_EQ(game_high_score_times(prefs, t, 1), 1);
    snprintf(prefs, sizeof prefs, "%s/missing.plist", dir);
    CHECK_EQ(game_high_score_times(prefs, t, 8), 0);
    test_remove_tree(dir);
}

/* A table for game_high_scores: entry 3 has no score, entry 4 is one the
   game starts with (no stamp), and entry 2 has no name. */
static const char *table_str(const char *key) {
    static const char *const s[][2] = {
        {"highscore name 1", "GREG"},
        {"highscore timestamp 1", "2026-10-14 15:20:00"},
        {"highscore timestamp 2", "2026-10-07 10:47:40"},
        {"highscore name 4", "MIO"},
        {"highscore timestamp 4", ""},
    };
    for (size_t i = 0; i < sizeof s / sizeof s[0]; i++)
        if (strcmp(key, s[i][0]) == 0)
            return s[i][1];
    return NULL;
}

static bool table_int(const char *key, int64_t *out) {
    if (strcmp(key, "highscore 1") == 0)
        *out = 43074596;
    else if (strcmp(key, "highscore 2") == 0)
        *out = 23757430;
    else if (strcmp(key, "highscore 4") == 0)
        *out = 4000;
    else
        return false;
    return true;
}

/* The High Scores menu's table: the entries with a score, best first, with
   their names and stamps. */
TEST(game_high_scores_read_the_table) {
    game_high_score hs[GAME_HIGH_SCORES];
    CHECK_EQ(game_high_scores(table_str, table_int, hs), 3);
    CHECK_EQ(hs[0].score, 430745960); /* the game saves scores over 10 */
    CHECK_STR(hs[0].name, "GREG");
    CHECK_EQ(hs[0].when, local_time(2026, 10, 14, 15, 20, 0));
    CHECK_EQ(hs[1].score, 237574300);
    CHECK_STR(hs[1].name, "");
    CHECK_EQ(hs[1].when, local_time(2026, 10, 7, 10, 47, 40));
    CHECK_EQ(hs[2].score, 40000);
    CHECK_STR(hs[2].name, "MIO");
    CHECK_EQ(hs[2].when, 0);
    time_t t;
    CHECK(!game_parse_score_time("", &t));
    CHECK(!game_parse_score_time(NULL, &t));
    CHECK(!game_parse_score_time("2026-10-14 15:20:00 extra", &t));
}
