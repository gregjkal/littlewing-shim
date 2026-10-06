#include "test.h"

#include <stdlib.h>
#include <unistd.h>

#include "game.h"
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

TEST(game_table_knows_both_games) {
    CHECK_EQ(game_count(), 2);
    CHECK_STR(game_at(0)->id, "loony-labyrinth");
    CHECK_STR(game_at(1)->id, "crystal-caliburn");
    CHECK(game_at(2) == NULL);
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
    game_folder(game_at(1), def, sizeof def);
    setenv("LOONY_APPS_DIR", "/tmp/apps", 1);
    game_folder(game_at(0), out, sizeof out);
    restore_apps_dir();
    CHECK_STR(def, "/Applications/Crystal Caliburn");
    CHECK_STR(out, "/tmp/apps/Loony Labyrinth");
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
    CHECK_EQ(game_move_legacy_data("/nonexistent/loony-shim"), 0);
}
