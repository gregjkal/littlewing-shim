#include "test.h"

#include <stdlib.h>
#include <unistd.h>

#include "game.h"
#include "license.h"
#include "plist.h"
#include "util.h"

/* license_save_default as the app runs it, without the tests'
   LOONY_UNREGISTERED, into a fresh folder (dir). */
static char dir[1024];

static bool save(const char *game_id) {
    unsetenv("LOONY_UNREGISTERED");
    bool ok = license_save_default(game_by_id(game_id), dir);
    setenv("LOONY_UNREGISTERED", "1", 1);
    return ok;
}

/* The string under key in dir's prefs.plist, malloc'd, or NULL. */
static char *pref(const char *key) {
    char path[1200], err[256];
    snprintf(path, sizeof path, "%s/prefs.plist", dir);
    plist_entry *e;
    uint32_t n;
    if (plist_read(path, &e, &n, err, sizeof err) != PLIST_OK)
        return NULL;
    char *v = NULL;
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(e[i].key, key) == 0 && !e[i].is_number)
            v = strdup(e[i].str);
    plist_free(e, n);
    return v;
}

static bool pref_is(const char *key, const char *want) {
    char *v = pref(key);
    bool same = v && strcmp(v, want) == 0;
    free(v);
    return same;
}

static bool write_prefs(const plist_entry *e, uint32_t n) {
    char path[1200], err[256];
    snprintf(path, sizeof path, "%s/prefs.plist", dir);
    return plist_write(path, e, n, err, sizeof err);
}

/* A game without preferences gets them, holding the license alone. */
TEST(license_is_saved_in_new_preferences) {
    test_tmp_dir(dir, sizeof dir);
    bool ok = save("loony-labyrinth");
    bool email = pref_is("user email", LICENSE_EMAIL), key = pref_is("user id", LICENSE_KEY);
    test_remove_tree(dir);
    CHECK(ok);
    CHECK(email);
    CHECK(key);
}

/* Preferences without a license (or with an empty one, as the games save
   before registering) get it, and keep the rest. */
TEST(license_is_added_to_preferences_and_keeps_the_rest) {
    test_tmp_dir(dir, sizeof dir);
    plist_entry e[] = {{"user email", false, 0, ""}, {"highscore name 1", false, 0, "LUNA"},
                       {"keycode plunger", true, 60, NULL}};
    bool wrote = write_prefs(e, 3);
    bool ok = save("monster-fair");
    bool email = pref_is("user email", LICENSE_EMAIL), key = pref_is("user id", LICENSE_KEY);
    bool name = pref_is("highscore name 1", "LUNA");
    test_remove_tree(dir);
    CHECK(wrote);
    CHECK(ok);
    CHECK(email);
    CHECK(key);
    CHECK(name);
}

/* The player's own license stays. */
TEST(license_leaves_one_already_saved) {
    test_tmp_dir(dir, sizeof dir);
    plist_entry e[] = {{"user email", false, 0, "me@example.org"},
                       {"user id", false, 0, "999999999999999999999999"}};
    bool wrote = write_prefs(e, 2);
    bool ok = save("crystal-caliburn");
    bool email = pref_is("user email", "me@example.org");
    bool key = pref_is("user id", "999999999999999999999999");
    test_remove_tree(dir);
    CHECK(wrote);
    CHECK(ok);
    CHECK(email);
    CHECK(key);
}

/* Angel Egg, Golden Logres and Tristan keep a file: the e-mail address at 0
   and the key code further in, the rest zero. */
TEST(license_is_saved_in_a_games_own_file) {
    static const struct {
        const char *id, *file;
        size_t size, key_at;
    } want[] = {
        {"angel-egg", "Angel Egg IE Registration", 320, 0x100},
        {"golden-logres", "Golden Logres 3.0 Registration", 320, 0x100},
        {"tristan", "TRISTAN IE Registration", 256, 0x80},
    };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        test_tmp_dir(dir, sizeof dir);
        bool ok = save(want[i].id);
        char path[1200], prefs[1200];
        snprintf(path, sizeof path, "%s/%s", dir, want[i].file);
        snprintf(prefs, sizeof prefs, "%s/prefs.plist", dir);
        size_t len = 0;
        uint8_t *b = read_file(path, &len);
        bool no_prefs = access(prefs, F_OK) != 0;
        test_remove_tree(dir);
        CHECK(ok);
        CHECK(b != NULL);
        CHECK_EQ(len, want[i].size);
        CHECK_STR((const char *)b, LICENSE_EMAIL);
        CHECK_STR((const char *)b + want[i].key_at, LICENSE_KEY);
        size_t nonzero = 0;
        for (size_t j = 0; j < len; j++)
            nonzero += b[j] != 0;
        free(b);
        CHECK_EQ(nonzero, strlen(LICENSE_EMAIL) + strlen(LICENSE_KEY));
        CHECK(no_prefs);
    }
}

/* A license file already there, whatever it holds, stays. */
TEST(license_leaves_a_games_file_already_saved) {
    test_tmp_dir(dir, sizeof dir);
    char path[1200];
    snprintf(path, sizeof path, "%s/TRISTAN IE Registration", dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs("mine", f);
        fclose(f);
    }
    bool ok = save("tristan");
    size_t len = 0;
    uint8_t *b = read_file(path, &len);
    test_remove_tree(dir);
    CHECK(f != NULL);
    CHECK(ok);
    CHECK(b != NULL);
    CHECK_STR((const char *)b, "mine");
    free(b);
}

/* LOONY_UNREGISTERED=1 saves nothing. */
TEST(license_unregistered_saves_nothing) {
    test_tmp_dir(dir, sizeof dir);
    setenv("LOONY_UNREGISTERED", "1", 1);
    bool prefs_ok = license_save_default(game_by_id("loony-labyrinth"), dir);
    bool file_ok = license_save_default(game_by_id("angel-egg"), dir);
    char path[1200];
    snprintf(path, sizeof path, "%s/prefs.plist", dir);
    bool no_prefs = access(path, F_OK) != 0;
    snprintf(path, sizeof path, "%s/Angel Egg IE Registration", dir);
    bool no_file = access(path, F_OK) != 0;
    test_remove_tree(dir);
    CHECK(prefs_ok);
    CHECK(file_ok);
    CHECK(no_prefs);
    CHECK(no_file);
}
