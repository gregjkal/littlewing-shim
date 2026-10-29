#include "test.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "game.h"
#include "theme.h"

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

/* ---- the games whose music starts with a game ---- */

static const game_info *theme_game;
static char theme_wav[1024];

/* As theme.c's render runs the game. */
static void run_theme_render(void *dir) {
    const game_info *g = theme_game;
    char script[1100], exit_after[32];
    snprintf(script, sizeof script, "%s.txt", theme_wav);
    FILE *f = fopen(script, "w");
    if (!f || fputs(g->theme_script, f) < 0 || fclose(f) != 0)
        exit(3);
    snprintf(exit_after, sizeof exit_after, "%d", g->theme_from + (THEME_MAX_SECONDS + 2) * 60);
    unsetenv("LOONY_UNREGISTERED"); /* the app's games are registered */
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    setenv("LOONY_AUTO_ALERTS", "1", 1);
    setenv("LOONY_HD", "off", 1);
    setenv("LOONY_SCRIPT", script, 1);
    setenv("LOONY_WAV", theme_wav, 1);
    setenv("LOONY_EXIT_AFTER", exit_after, 1);
    execl(LOONY_BIN, "loony", (const char *)dir, (char *)NULL);
    exit(4);
}

/* The clip a game's row gives it, in seconds (0 for none, -1 if it can't run). */
static double theme_seconds(const char *id, const char *dir) {
    theme_game = game_by_id(id);
    test_tmp_dir(theme_wav, sizeof theme_wav - 16);
    char folder[1024];
    snprintf(folder, sizeof folder, "%s", theme_wav);
    strcat(theme_wav, "/out.wav");
    char out[16384];
    int status = test_run_child(run_theme_render, (void *)dir, out, sizeof out);
    SDL_AudioSpec spec;
    Uint8 *buf = NULL;
    Uint32 len = 0;
    double secs = -1;
    if (status == 0 && SDL_LoadWAV(theme_wav, &spec, &buf, &len)) {
        int16_t *clip;
        uint32_t n = theme_clip((const int16_t *)buf, len / 4,
                                (uint32_t)theme_game->theme_from * THEME_RATE / 60, &clip);
        free(clip);
        SDL_free(buf);
        secs = (double)n / THEME_RATE;
    }
    test_remove_tree(folder);
    return secs;
}

TEST(theme_jinni_zeala_plays_music_once_a_game_starts) {
    SKIP_UNLESS_JZ();
    CHECK(theme_seconds("jinni-zeala", test_jz_dir()) >= 10);
}

TEST(theme_fairy_tower_plays_music_once_a_game_starts) {
    SKIP_UNLESS_FT();
    CHECK(theme_seconds("fairy-tower", test_ft_app()) >= 10);
}
