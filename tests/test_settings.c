#include "test.h"

#include <stdlib.h>
#include <unistd.h>

#include "files.h"
#include "game.h"
#include "settings.h"

/* Settings live under HOME unless LOONY_DATA_DIR is set; these point HOME at
   a fresh folder for one test and put both back. */
static char saved_home[1024], saved_data[1024], home[1024];
static bool had_home, had_data;

static void set_env(void) {
    const char *h = getenv("HOME"), *d = getenv("LOONY_DATA_DIR");
    had_home = h != NULL;
    had_data = d != NULL;
    snprintf(saved_home, sizeof saved_home, "%s", h ? h : "");
    snprintf(saved_data, sizeof saved_data, "%s", d ? d : "");
    test_tmp_dir(home, sizeof home);
    setenv("HOME", home, 1);
    unsetenv("LOONY_DATA_DIR");
}

static void restore_env(void) {
    if (had_home)
        setenv("HOME", saved_home, 1);
    else
        unsetenv("HOME");
    if (had_data)
        setenv("LOONY_DATA_DIR", saved_data, 1);
    else
        unsetenv("LOONY_DATA_DIR");
    test_remove_tree(home);
}

/* Setting the volume mustn't forget the picker's last game, or the reverse. */
static void keep_each_others_keys(void) {
    char s[64] = "";
    int64_t n = 0;
    CHECK(!settings_get_str(SETTINGS_LAST_GAME, s, sizeof s)); /* no file yet */
    CHECK(!settings_get_int(SETTINGS_VOLUME, &n));
    settings_set_str(SETTINGS_LAST_GAME, "crystal-caliburn");
    settings_set_int(SETTINGS_VOLUME, 55);
    settings_set_int(SETTINGS_VOLUME, 60); /* replaces it */
    CHECK(settings_get_str(SETTINGS_LAST_GAME, s, sizeof s));
    CHECK_STR(s, "crystal-caliburn");
    CHECK(settings_get_int(SETTINGS_VOLUME, &n));
    CHECK_EQ(n, 60);
    CHECK(!settings_get_int(SETTINGS_LAST_GAME, &n)); /* a string, not an integer */
    char root[1100], path[1300];
    CHECK(files_data_root(root, sizeof root));
    CHECK(strncmp(root, home, strlen(home)) == 0);
    snprintf(path, sizeof path, "%s/" GAME_PICKER_FILE, root);
    CHECK(access(path, F_OK) == 0);
}

TEST(settings_keep_each_others_keys) {
    set_env();
    keep_each_others_keys();
    restore_env();
}

TEST(settings_without_a_save_root_do_nothing) {
    set_env();
    setenv("LOONY_DATA_DIR", home, 1);
    settings_set_int(SETTINGS_VOLUME, 30);
    int64_t n = 0;
    bool got = settings_get_int(SETTINGS_VOLUME, &n);
    restore_env();
    CHECK(!got);
}
