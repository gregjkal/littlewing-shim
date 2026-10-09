#include "test.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "blit.h"
#include "hd.h"
#include "png.h"
#include "util.h"

static qd_palette pal8;

/* A zeroed w x h buffer. */
static qd_pixels buffer(int w, int h, int depth) {
    qd_std_palette(8, &pal8);
    uint32_t rb = (uint32_t)((w * depth + 31) / 32 * 4);
    qd_pixels p = {calloc(rb * (size_t)h, 1), rb, {0, 0, (int16_t)h, (int16_t)w}, depth,
                   depth <= 8 ? &pal8 : NULL};
    return p;
}

static const uint8_t *frame_px(const uint8_t *rgba, int w, int x, int y) {
    return rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
}

/* Whether the HD copy of px shows each 1x pixel as an NxN block of its color. */
static bool shows_blocks(const qd_pixels *px) {
    int w, h, n = hd_scale();
    const uint8_t *rgba = hd_frame(px, &w, &h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            qd_rgb c = qd_color_of(qd_get_pixel(px, x / n, y / n), px->depth, px->pal);
            const uint8_t *p = frame_px(rgba, w, x, y);
            if (p[0] != c.r >> 8 || p[1] != c.g >> 8 || p[2] != c.b >> 8 || p[3] != 0xFF)
                return false;
        }
    return true;
}

/* Writes a w x h PNG for picture data into dir, every pixel a different color. */
static void write_art(const char *dir, const char *data, int w, int h, uint8_t seed) {
    uint8_t *rgba = malloc((size_t)w * (size_t)h * 4);
    for (int i = 0; i < w * h; i++) {
        rgba[i * 4] = (uint8_t)(i * 7 + seed);
        rgba[i * 4 + 1] = (uint8_t)(i * 13);
        rgba[i * 4 + 2] = (uint8_t)(i / 3 + seed);
        rgba[i * 4 + 3] = 0xFF;
    }
    char path[1200];
    snprintf(path, sizeof path, "%s/%08x.png", dir, fnv1a32(data, strlen(data)));
    png_write_rgba(path, rgba, w, h);
    free(rgba);
}

/* The art pixel the HD copy should show at HD (x, y), for art drawn at 1x (0, 0). */
static bool shows_art(const uint8_t *rgba, int fw, int x, int y, int aw, int ax, int ay,
                      uint8_t seed) {
    const uint8_t *p = frame_px(rgba, fw, x, y);
    int i = ay * aw + ax;
    return p[0] == (uint8_t)(i * 7 + seed) && p[1] == (uint8_t)(i * 13) &&
           p[2] == (uint8_t)(i / 3 + seed);
}

TEST(hd_repeats_copybits_on_the_hd_copies) {
    hd_configure(2, NULL, NULL);
    qd_pixels src = buffer(3, 2, 8), dst = buffer(4, 4, 16);
    for (int i = 0; i < 6; i++)
        qd_set_pixel(&src, i % 3, i / 3, (uint32_t)(i * 40 + 5));
    char err[64];
    qd_rect sr = {0, 0, 2, 3}, dr = {1, 1, 3, 4};
    CHECK(qd_blit(&src, sr, &dst, dr, dst.bounds, QD_SRC_COPY, (qd_rgb){0}, (qd_rgb){0}, err,
                  sizeof err));
    hd_copy(&src, sr, &dst, dr, dst.bounds);
    int w, h;
    const uint8_t *rgba = hd_frame(&dst, &w, &h);
    CHECK_EQ(w, 8);
    CHECK_EQ(h, 8);
    /* Copied pixels keep the source's full color (16 bits would round it). */
    qd_rgb c = pal8.c[5 + 40 * 4];
    const uint8_t *p = frame_px(rgba, w, 2 * 2 + 1, 2 * 2 + 1); /* src (1, 1) -> dst (2, 2) */
    CHECK_EQ(p[0], c.r >> 8);
    CHECK_EQ(p[1], c.g >> 8);
    CHECK_EQ(p[2], c.b >> 8);
    p = frame_px(rgba, w, 0, 0); /* untouched */
    CHECK_EQ(p[0] | p[1] | p[2], 0);
    hd_configure(0, NULL, NULL);
    free(src.base);
    free(dst.base);
}

TEST(hd_draws_pixels_changed_behind_its_back_as_blocks) {
    hd_configure(3, NULL, NULL);
    qd_pixels px = buffer(5, 3, 16);
    CHECK(shows_blocks(&px));
    qd_set_pixel(&px, 4, 2, 0x7C00); /* the game writes red itself */
    qd_set_pixel(&px, 0, 1, 0x03E0);
    CHECK(shows_blocks(&px));
    hd_configure(0, NULL, NULL);
    free(px.base);
}

TEST(hd_copies_within_one_buffer_when_they_overlap) {
    hd_configure(2, NULL, NULL);
    qd_pixels px = buffer(4, 5, 16);
    for (int y = 0; y < 5; y++)
        for (int x = 0; x < 4; x++)
            qd_set_pixel(&px, x, y, (uint32_t)(y * 0x0C63 + x * 0x0401));
    CHECK(shows_blocks(&px));
    char err[64];
    qd_rect a = {0, 0, 4, 4}, b = {1, 0, 5, 4}; /* down one row, then back up */
    CHECK(qd_blit(&px, a, &px, b, px.bounds, QD_SRC_COPY, (qd_rgb){0}, (qd_rgb){0}, err, sizeof err));
    hd_copy(&px, a, &px, b, px.bounds);
    CHECK(shows_blocks(&px));
    CHECK(qd_blit(&px, b, &px, a, px.bounds, QD_SRC_COPY, (qd_rgb){0}, (qd_rgb){0}, err, sizeof err));
    hd_copy(&px, b, &px, a, px.bounds);
    CHECK(shows_blocks(&px));
    hd_configure(0, NULL, NULL);
    free(px.base);
}

TEST(hd_draws_replacement_art_for_a_picture) {
    char dir[1024];
    test_tmp_dir(dir, sizeof dir);
    write_art(dir, "a picture", 6, 4, 1);
    hd_configure(2, dir, NULL);
    qd_pixels px = buffer(3, 2, 8);
    hd_picture((const uint8_t *)"a picture", 9, px.bounds, &px, px.bounds);
    int w, h;
    const uint8_t *rgba = hd_frame(&px, &w, &h);
    bool all = true;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 6; x++)
            all = all && shows_art(rgba, w, x, y, 6, x, y, 1);
    CHECK(all);
    /* A picture without art leaves the pixels to the blocks. */
    hd_picture((const uint8_t *)"another", 7, px.bounds, &px, px.bounds);
    qd_set_pixel(&px, 0, 0, 35);
    rgba = hd_frame(&px, &w, &h);
    CHECK_EQ(frame_px(rgba, w, 1, 1)[0], pal8.c[35].r >> 8);
    CHECK(shows_art(rgba, w, 2, 0, 6, 2, 0, 1));
    hd_configure(0, NULL, NULL);
    free(px.base);
    test_remove_tree(dir);
}

TEST(hd_puts_the_art_back_where_the_game_restores_a_layers_pixels) {
    char dir[1024];
    test_tmp_dir(dir, sizeof dir);
    write_art(dir, "table", 300, 4, 9);
    hd_configure(2, dir, NULL);
    qd_pixels px = buffer(150, 2, 8); /* too big to be a sprite */
    memset(px.base, 10, px.row_bytes * 2);
    hd_picture((const uint8_t *)"table", 5, px.bounds, &px, px.bounds);
    int w, h;
    qd_set_pixel(&px, 7, 1, 200); /* a lamp lights */
    const uint8_t *rgba = hd_frame(&px, &w, &h);
    CHECK_EQ(frame_px(rgba, w, 15, 3)[0], pal8.c[200].r >> 8);
    qd_set_pixel(&px, 7, 1, 10); /* and goes out */
    rgba = hd_frame(&px, &w, &h);
    bool all = true;
    for (int y = 2; y < 4; y++)
        for (int x = 14; x < 16; x++)
            all = all && shows_art(rgba, w, x, y, 300, x, y, 9);
    CHECK(all);
    hd_configure(0, NULL, NULL);
    free(px.base);
    test_remove_tree(dir);
}

TEST(hd_recognizes_a_sprite_the_game_copies_by_hand) {
    char dir[1024];
    test_tmp_dir(dir, sizeof dir);
    write_art(dir, "lamp", 8, 8, 3);
    write_art(dir, "table", 300, 16, 50);
    hd_configure(2, dir, NULL);
    qd_pixels lamp = buffer(4, 4, 8), table = buffer(150, 8, 8);
    for (int i = 0; i < 16; i++)
        qd_set_pixel(&lamp, i % 4, i / 4, (uint32_t)(i + 1));
    hd_picture((const uint8_t *)"lamp", 4, lamp.bounds, &lamp, lamp.bounds);
    hd_picture((const uint8_t *)"table", 5, table.bounds, &table, table.bounds);
    for (int i = 0; i < 16; i++) /* the game's own code draws the lamp at (60, 2) */
        qd_set_pixel(&table, 60 + i % 4, 2 + i / 4, qd_get_pixel(&lamp, i % 4, i / 4));
    int w, h;
    const uint8_t *rgba = hd_frame(&table, &w, &h);
    bool all = true;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            all = all && shows_art(rgba, w, 120 + x, 4 + y, 8, x, y, 3);
    CHECK(all);
    CHECK(shows_art(rgba, w, 119, 4, 300, 119, 4, 50)); /* next to it: the table's art */
    hd_configure(0, NULL, NULL);
    free(lamp.base);
    free(table.base);
    test_remove_tree(dir);
}

/* Brings px's HD copy up to date and takes its changes (at most 8, into
   rects, *n of them). True if every HD pixel that differs from before lies
   in one of them and nothing is left to take after; before is then updated
   to the new frame. */
static bool changes_cover(const qd_pixels *px, uint8_t *before, qd_rect *rects, int *n) {
    int w, h;
    const uint8_t *rgba = hd_frame(px, &w, &h);
    *n = hd_take_changes(px, rects, 8);
    bool ok = true;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t at = ((size_t)y * (size_t)w + (size_t)x) * 4;
            if (memcmp(before + at, rgba + at, 4) == 0)
                continue;
            bool in = false;
            for (int i = 0; i < *n && !in; i++)
                in = y >= rects[i].top && y < rects[i].bottom && x >= rects[i].left &&
                     x < rects[i].right;
            ok = ok && in;
        }
    memcpy(before, rgba, (size_t)w * (size_t)h * 4);
    qd_rect more[8];
    hd_frame(px, &w, &h);
    return ok && hd_take_changes(px, more, 8) == 0;
}

/* Whether HD row y is in one of the n rects. */
static bool row_taken(const qd_rect *rects, int n, int y) {
    for (int i = 0; i < n; i++)
        if (y >= rects[i].top && y < rects[i].bottom)
            return true;
    return false;
}

TEST(hd_reports_every_hd_pixel_that_changed) {
    char dir[1024];
    test_tmp_dir(dir, sizeof dir);
    write_art(dir, "lamp", 8, 8, 3);
    write_art(dir, "table", 300, 80, 50);
    hd_configure(2, dir, NULL);
    qd_pixels screen = buffer(150, 40, 8), src = buffer(20, 10, 8), lamp = buffer(4, 4, 8);
    memset(screen.base, 10, screen.row_bytes * 40);
    for (int i = 0; i < 200; i++)
        qd_set_pixel(&src, i % 20, i / 20, (uint32_t)(i + 20));
    for (int i = 0; i < 16; i++)
        qd_set_pixel(&lamp, i % 4, i / 4, (uint32_t)(i + 1));
    qd_rect rects[8];
    int n, w, h;
    const uint8_t *rgba = hd_frame(&screen, &w, &h);
    uint8_t *before = malloc((size_t)w * (size_t)h * 4);
    memcpy(before, rgba, (size_t)w * (size_t)h * 4);
    CHECK_EQ(hd_take_changes(&screen, rects, 8), 1); /* a new copy: all of it */
    CHECK(memcmp(&rects[0], &(qd_rect){0, 0, 80, 300}, sizeof rects[0]) == 0);
    CHECK(changes_cover(&screen, before, rects, &n));
    CHECK_EQ(n, 0);

    hd_picture((const uint8_t *)"lamp", 4, lamp.bounds, &lamp, lamp.bounds);
    hd_picture((const uint8_t *)"table", 5, screen.bounds, &screen, screen.bounds);
    CHECK(changes_cover(&screen, before, rects, &n));
    CHECK_EQ(n, 1);

    /* A copy near the top and a pixel near the bottom: two runs of rows. */
    char err[64];
    qd_rect sr = {0, 0, 4, 20}, dr = {2, 10, 6, 30};
    CHECK(qd_blit(&src, sr, &screen, dr, screen.bounds, QD_SRC_COPY, (qd_rgb){0}, (qd_rgb){0}, err,
                  sizeof err));
    hd_copy(&src, sr, &screen, dr, screen.bounds);
    qd_set_pixel(&screen, 100, 37, 200);
    CHECK(changes_cover(&screen, before, rects, &n));
    CHECK_EQ(n, 2);
    CHECK(!row_taken(rects, n, 40));
    CHECK_EQ(rects[0].left, 20); /* only as wide as the copy */
    CHECK_EQ(rects[0].right, 60);
    hd_copy(&src, sr, &screen, dr, screen.bounds); /* the same pixels again: no change */
    CHECK(changes_cover(&screen, before, rects, &n));
    CHECK_EQ(n, 0);

    /* The lamp drawn by hand (a sprite), the pixel put back (the table's art
       again), and a copy that scales. */
    for (int i = 0; i < 16; i++)
        qd_set_pixel(&screen, 60 + i % 4, 20 + i / 4, qd_get_pixel(&lamp, i % 4, i / 4));
    qd_set_pixel(&screen, 100, 37, 10);
    CHECK(changes_cover(&screen, before, rects, &n));
    CHECK_EQ(n, 2);
    CHECK(shows_art(before, w, 121, 41, 8, 1, 1, 3));
    qd_rect big = {28, 40, 36, 80};
    CHECK(qd_blit(&src, sr, &screen, big, screen.bounds, QD_SRC_COPY, (qd_rgb){0}, (qd_rgb){0}, err,
                  sizeof err));
    hd_copy(&src, sr, &screen, big, screen.bounds);
    CHECK(changes_cover(&screen, before, rects, &n));
    CHECK_EQ(n, 1);

    /* More runs than room: the last rect holds the rest. */
    qd_set_pixel(&screen, 0, 0, 1);
    qd_set_pixel(&screen, 5, 20, 2);
    qd_set_pixel(&screen, 149, 39, 3);
    hd_frame(&screen, &w, &h);
    CHECK_EQ(hd_take_changes(&screen, rects, 2), 2);
    CHECK(memcmp(&rects[1], &(qd_rect){40, 10, 80, 300}, sizeof rects[1]) == 0);
    hd_configure(0, NULL, NULL);
    free(before);
    free(screen.base);
    free(src.base);
    free(lamp.base);
    test_remove_tree(dir);
}

/* ---- the game in HD ---- */

static char run_shot[1200], run_data[1024], run_art[1024], run_dump[1024];
static bool run_verify;

/* Plays the opening and attract mode on a fixed clock, in HD when run_art is set. */
static void run_game(void *dir) {
    setenv("LOONY_DATA_DIR", run_data, 1);
    setenv("LOONY_AUTO_ALERTS", "1", 1);
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    setenv("LOONY_EXIT_AFTER", "900", 1);
    setenv("LOONY_SCREENSHOT", run_shot, 1);
    if (run_art[0])
        setenv("LOONY_HD", run_art, 1);
    if (run_dump[0])
        setenv("LOONY_HD_DUMP", run_dump, 1);
    if (run_verify)
        setenv("LOONY_HD_VERIFY", "1", 1);
    execl(LOONY_BIN, "loony", (const char *)dir, (char *)NULL);
    _exit(127);
}

/* Runs the game and returns its screenshot as RGBA32, or NULL. */
static SDL_Surface *run_and_shoot(char *out, size_t outlen) {
    test_tmp_dir(run_data, sizeof run_data);
    snprintf(run_shot, sizeof run_shot, "%s/shot.png", run_data);
    int status = test_run_child(run_game, (void *)test_game_dir(), out, outlen);
    SDL_Surface *s = status == 0 ? SDL_LoadPNG(run_shot) : NULL;
    SDL_Surface *c = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32) : NULL;
    SDL_DestroySurface(s);
    test_remove_tree(run_data);
    return c;
}

/* Without art, every HD frame must be the 1x frame with each pixel as a 4x4
   block: this checks the copying and change tracking against the real game.
   The colors may differ by up to one 5-bit step: 8-bit art copied to the
   16-bit screen is rounded there, but keeps its full color in HD. */
TEST(hd_without_art_shows_the_game_exactly_as_blocks) {
    SKIP_UNLESS_GAME();
    char out[32768];
    run_art[0] = '\0';
    run_dump[0] = '\0';
    SDL_Surface *lo = run_and_shoot(out, sizeof out);
    test_tmp_dir(run_art, sizeof run_art); /* empty: no art */
    test_tmp_dir(run_dump, sizeof run_dump);
    SDL_Surface *hi = run_and_shoot(out, sizeof out);
    char table[1200];
    snprintf(table, sizeof table, "%s/454708e3.png", run_dump);
    struct stat st;
    bool dumped = stat(table, &st) == 0;
    test_remove_tree(run_art);
    test_remove_tree(run_dump);
    run_art[0] = run_dump[0] = '\0';
    CHECK(lo != NULL);
    CHECK(hi != NULL);
    CHECK_CONTAINS(out, "hd: on at 4x");
    CHECK(dumped); /* the table picture */
    CHECK_EQ(lo->w, 800);
    CHECK_EQ(hi->w, 3200);
    CHECK_EQ(hi->h, 2400);
    int bad = 0;
    for (int y = 0; y < hi->h; y++)
        for (int x = 0; x < hi->w; x++) {
            const uint8_t *p = (const uint8_t *)hi->pixels + (size_t)y * hi->pitch + (size_t)x * 4;
            const uint8_t *q =
                (const uint8_t *)lo->pixels + (size_t)(y / 4) * lo->pitch + (size_t)(x / 4) * 4;
            bad += abs(p[0] - q[0]) > 7 || abs(p[1] - q[1]) > 7 || abs(p[2] - q[2]) > 7 ||
                   p[3] != q[3];
        }
    SDL_DestroySurface(lo);
    SDL_DestroySurface(hi);
    CHECK_EQ(bad, 0);
}

/* With the real art, uploading only what hd_take_changes reports, on top of
   the frame before, must make each whole frame (LOONY_HD_VERIFY checks that
   at every present), and must upload less than whole frames. */
TEST(hd_uploads_only_what_changed_and_it_makes_each_frame) {
    SKIP_UNLESS_GAME();
    char out[32768];
    snprintf(run_art, sizeof run_art, "%s/hd-art/loony-labyrinth", LOONY_SRC_DIR);
    run_dump[0] = '\0';
    run_verify = true;
    SDL_Surface *hi = run_and_shoot(out, sizeof out);
    run_art[0] = '\0';
    run_verify = false;
    CHECK(hi != NULL);
    SDL_DestroySurface(hi);
    const char *v = strstr(out, "LOONY_HD_VERIFY: the uploads made the whole frame after ");
    unsigned ok = 0, all = 0, presents = 0;
    unsigned long long bytes = 0;
    CHECK(v && sscanf(v, "LOONY_HD_VERIFY: the uploads made the whole frame after %u of %u", &ok,
                      &all) == 2);
    CHECK(all > 100);
    CHECK_EQ(ok, all);
    const char *st = strstr(out, "display: ");
    CHECK(st && sscanf(st, "display: %u presents, %llu bytes", &presents, &bytes) == 2);
    CHECK_EQ(presents, all);
    CHECK(bytes < 3200ull * 2400 * 4 / 20); /* the first frames are whole; most are small */
}
