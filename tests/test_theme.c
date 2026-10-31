#include "test.h"

#include <SDL3/SDL.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "game.h"
#include "theme.h"
#include "util.h"

#define SEC THEME_RATE

/* frames of silence, with a tone (each sample level) over [on, off). */
static int16_t *tone(uint32_t frames, uint32_t on, uint32_t off, int16_t level) {
    int16_t *s = calloc(frames, 4);
    for (uint32_t i = on; i < off && i < frames; i++)
        s[2 * i] = s[2 * i + 1] = (i & 1) ? level : (int16_t)-level;
    return s;
}

TEST(theme_clip_is_the_sound_up_to_a_gap_of_silence) {
    int16_t *s = tone(10 * SEC, 2 * SEC, 5 * SEC, 1000), *clip;
    uint32_t n = theme_clip(s, 10 * SEC, 0, &clip);
    CHECK_EQ(n, 3 * SEC);
    CHECK(clip != NULL);
    CHECK_EQ(memcmp(clip, s + 2 * 2 * SEC, (size_t)n * 4), 0); /* not faded: the music ended */
    free(clip);
    free(s);
}

TEST(theme_clip_keeps_a_pause_shorter_than_the_gap) {
    int16_t *s = tone(10 * SEC, 1 * SEC, 3 * SEC, 1000);
    for (uint32_t i = 3 * SEC + SEC / 2; i < 4 * SEC; i++)
        s[2 * i] = s[2 * i + 1] = 1000;
    int16_t *clip;
    CHECK_EQ(theme_clip(s, 10 * SEC, 0, &clip), 3 * SEC);
    free(clip);
    free(s);
}

TEST(theme_clip_listens_from_its_start) {
    int16_t *s = tone(10 * SEC, 0, 1 * SEC, 1000), *clip;
    for (uint32_t i = 6 * SEC; i < 8 * SEC; i++)
        s[2 * i] = s[2 * i + 1] = 1000;
    CHECK_EQ(theme_clip(s, 10 * SEC, 2 * SEC, &clip), 2 * SEC);
    free(clip);
    free(s);
}

TEST(theme_clip_of_silence_is_empty) {
    int16_t *s = tone(5 * SEC, 0, 5 * SEC, THEME_QUIET), *clip = (int16_t *)1;
    CHECK_EQ(theme_clip(s, 5 * SEC, 0, &clip), 0);
    CHECK(clip == NULL);
    free(s);
}

/* Music that goes on stops at THEME_MAX_SECONDS, fading out. */
TEST(theme_clip_of_long_music_fades_out_at_its_longest) {
    uint32_t frames = (THEME_MAX_SECONDS + 5) * SEC, fade = THEME_FADE_MS * SEC / 1000;
    int16_t *s = tone(frames, SEC, frames, 10000), *clip;
    uint32_t n = theme_clip(s, frames, 0, &clip);
    CHECK_EQ(n, THEME_MAX_SECONDS * SEC);
    CHECK_EQ(abs(clip[2 * (n - fade - 1)]), 10000);
    CHECK(abs(clip[2 * (n - fade / 2)]) < 5100);
    CHECK(abs(clip[2 * (n - 1)]) < 10);
    free(clip);
    free(s);
}

/* At full scale the fade's product is past 32 bits: it still fades. */
TEST(theme_clip_fades_full_scale_music) {
    uint32_t frames = (THEME_MAX_SECONDS + 1) * SEC, fade = THEME_FADE_MS * SEC / 1000;
    int16_t *s = tone(frames, 0, frames, 32767), *clip;
    for (uint32_t i = 0; i < frames; i += 2)
        s[2 * i] = s[2 * i + 1] = -32768;
    uint32_t n = theme_clip(s, frames, 0, &clip);
    CHECK_EQ(n, THEME_MAX_SECONDS * SEC);
    uint32_t first = n - fade; /* the fade's first frame, at full scale */
    CHECK_EQ(clip[2 * first], s[2 * first]);
    for (uint32_t i = first; i < n; i++) /* never louder, never the other sign */
        CHECK(abs(clip[2 * i]) <= abs(s[2 * i]) && (int32_t)clip[2 * i] * s[2 * i] >= 0);
    free(clip);
    free(s);
}

/* ---- rendering, as the picker does ---- */

static const char *theme_id;
static char theme_home[1024];

static void root_path(char *out, size_t cap, const char *rel) {
    snprintf(out, cap, "%s/Library/Application Support/littlewing-shim/%s", theme_home, rel);
}

/* Renders theme_id's clip as the picker does, in a save root under
   theme_home, and waits (two minutes at most) for it. */
static void run_theme_render(void *unused) {
    (void)unused;
    setenv("HOME", theme_home, 1);
    unsetenv("LOONY_DATA_DIR");
    const game_info *g = game_by_id(theme_id);
    theme_start(&g, 1, 0, LOONY_BIN);
    char clip[1200];
    root_path(clip, sizeof clip, theme_id);
    strcat(clip, "/" THEME_FILE);
    double until = wall_seconds() + 120;
    while (access(clip, F_OK) != 0 && wall_seconds() < until) {
        theme_tend();
        SDL_Delay(20);
    }
    theme_stop();
}

/* The seconds of id's clip, rendered by theme.c (-1 if none was kept). */
static double theme_seconds(const char *id) {
    theme_id = id;
    test_tmp_dir(theme_home, sizeof theme_home);
    char out[65536], clip[1200];
    int status = test_run_child(run_theme_render, NULL, out, sizeof out);
    root_path(clip, sizeof clip, id);
    strcat(clip, "/" THEME_FILE);
    SDL_AudioSpec spec;
    Uint8 *buf = NULL;
    Uint32 len = 0;
    double secs = -1;
    if (status == 0 && SDL_LoadWAV(clip, &spec, &buf, &len)) {
        secs = (double)len / 4 / THEME_RATE;
        SDL_free(buf);
    }
    return secs;
}

/* Whether the save root under theme_home holds a render's folder. */
static bool render_folder_left(void) {
    char root[1200];
    root_path(root, sizeof root, "");
    DIR *d = opendir(root);
    struct dirent *e;
    bool left = false;
    while (d && (e = readdir(d)))
        left |= strncmp(e->d_name, THEME_TEMP_PREFIX, strlen(THEME_TEMP_PREFIX)) == 0;
    if (d)
        closedir(d);
    return left;
}

/* The clip lands in the game's save folder, the render's folder is gone,
   and a folder a crashed render left is removed while one of a running
   copy (here the test, its parent) stays, unless it is hours old. */
TEST(theme_render_keeps_the_clip_in_the_save_folder) {
    SKIP_UNLESS_GAME();
    test_tmp_dir(theme_home, sizeof theme_home);
    char live[1200], old[1200], dead[1200], name[64];
    snprintf(name, sizeof name, THEME_TEMP_PREFIX "%d-live", (int)getpid());
    root_path(live, sizeof live, name);
    snprintf(name, sizeof name, THEME_TEMP_PREFIX "%d-old", (int)getpid());
    root_path(old, sizeof old, name);
    root_path(dead, sizeof dead, THEME_TEMP_PREFIX "999999-dead");
    CHECK(kill(999999, 0) != 0);
    CHECK(make_dirs(live) && make_dirs(old) && make_dirs(dead));
    struct timeval two_hours_ago[2] = {{time(NULL) - 7200, 0}, {time(NULL) - 7200, 0}};
    CHECK(utimes(old, two_hours_ago) == 0);
    theme_id = "loony-labyrinth";
    char out[65536], clip[1200];
    int status = test_run_child(run_theme_render, NULL, out, sizeof out);
    root_path(clip, sizeof clip, "loony-labyrinth/" THEME_FILE);
    struct stat st;
    bool kept = stat(clip, &st) == 0, dead_gone = access(dead, F_OK) != 0;
    bool old_gone = access(old, F_OK) != 0, live_kept = rmdir(live) == 0;
    bool left = render_folder_left();
    test_remove_tree(theme_home);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "picker music: rendering Loony Labyrinth's");
    CHECK(kept);
    CHECK(st.st_size > 44 + 10 * THEME_RATE * 4); /* more than 10 s */
    CHECK(dead_gone);
    CHECK(old_gone);
    CHECK(live_kept);
    CHECK(!left);
}

TEST(theme_jinni_zeala_plays_music_once_a_game_starts) {
    SKIP_UNLESS_JZ();
    double secs = theme_seconds("jinni-zeala");
    test_remove_tree(theme_home);
    CHECK(secs >= 10);
}

TEST(theme_psychedelic_o_plays_music_once_a_game_starts) {
    SKIP_UNLESS_PO();
    double secs = theme_seconds("psychedelic-o");
    test_remove_tree(theme_home);
    CHECK(secs >= 10);
}

TEST(theme_fairy_tower_plays_music_once_a_game_starts) {
    SKIP_UNLESS_FT();
    double secs = theme_seconds("fairy-tower");
    test_remove_tree(theme_home);
    CHECK(secs >= 10);
}

/* A render's folder isn't a save from before there were two games. */
TEST(theme_render_folders_stay_out_of_the_legacy_move) {
    char root[1024], path[1200];
    test_tmp_dir(root, sizeof root);
    snprintf(path, sizeof path, "%s/" THEME_TEMP_PREFIX "123-abcdef", root);
    CHECK(make_dirs(path));
    int moved = game_move_legacy_data(root);
    bool stayed = access(path, F_OK) == 0;
    test_remove_tree(root);
    CHECK_EQ(moved, 0);
    CHECK(stayed);
}
