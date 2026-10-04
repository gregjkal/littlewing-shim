#include "picker.h"

#include <SDL3/SDL_scancode.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blit.h"
#include "font.h"
#include "pict.h"
#include "rsrc.h"
#include "util.h"

#define ART_SRC_W 512
#define ART_SRC_H 384
#define CARD_TOP 140
#define CARD_GAP 12
#define CARD_LEFT0 10
#define BORDER 4
#define NAME_TOP 448
#define HEADING_TOP 64
#define HINT_TOP 540

static const qd_rgb BLACK = {0, 0, 0}, GOLD = {0xFFFF, 0xCCCC, 0x3333}, DARK = {0x3333, 0x3333, 0x3333},
                    PLAIN = {0x2222, 0x2222, 0x2222}, WHITE = {0xFFFF, 0xFFFF, 0xFFFF},
                    GRAY = {0x8888, 0x8888, 0x8888};

static int card_left(int i) { return CARD_LEFT0 + i * (PICKER_ART_W + CARD_GAP); }

static qd_pixels pixels(uint8_t *base, int w, int h) {
    qd_pixels p = {base, (uint32_t)w * 4, {0, 0, (int16_t)h, (int16_t)w}, 32, NULL};
    return p;
}

static qd_rect rect(int left, int top, int w, int h) {
    qd_rect r = {(int16_t)top, (int16_t)left, (int16_t)(top + h), (int16_t)(left + w)};
    return r;
}

/* ASCII text at `scale`, each glyph bit a scale x scale square, centered on cx. */
static void draw_text(const qd_pixels *s, int cx, int top, const char *text, int scale, qd_rgb c) {
    int n = (int)strlen(text), x0 = cx - n * FONT_W * scale / 2;
    for (int i = 0; i < n; i++) {
        const uint8_t *g = font_glyph((uint8_t)text[i]);
        for (int row = 0; row < FONT_H; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] & (0x80 >> col))
                    qd_fill(s, rect(x0 + (i * FONT_W + col) * scale, top + row * scale, scale, scale), s->bounds, c);
    }
}

void picker_draw(const picker_entry *e, int n, int selected, uint8_t *screen) {
    font_init();
    qd_pixels s = pixels(screen, PICKER_W, PICKER_H);
    qd_fill(&s, s.bounds, s.bounds, BLACK);
    draw_text(&s, PICKER_W / 2, HEADING_TOP, "LITTLEWING PINBALL", 3, GOLD);
    for (int i = 0; i < n && i < PICKER_MAX; i++) {
        int left = card_left(i);
        qd_fill(&s, rect(left - BORDER, CARD_TOP - BORDER, PICKER_ART_W + 2 * BORDER, PICKER_ART_H + 2 * BORDER),
                s.bounds, i == selected ? GOLD : DARK);
        if (e[i].art)
            for (int y = 0; y < PICKER_ART_H; y++)
                memcpy(screen + ((size_t)(CARD_TOP + y) * PICKER_W + (size_t)left) * 4,
                       e[i].art + (size_t)y * PICKER_ART_W * 4, PICKER_ART_W * 4);
        else
            qd_fill(&s, rect(left, CARD_TOP, PICKER_ART_W, PICKER_ART_H), s.bounds, PLAIN);
        char name[64];
        size_t k = 0;
        for (const char *p = e[i].game->title; *p && k + 1 < sizeof name; p++)
            name[k++] = (char)toupper((unsigned char)*p);
        name[k] = '\0';
        draw_text(&s, left + PICKER_ART_W / 2, NAME_TOP, name, 2, i == selected ? WHITE : GRAY);
    }
    draw_text(&s, PICKER_W / 2, HINT_TOP, "RETURN TO PLAY - CMD-Q TO QUIT", 2, GRAY);
}

int picker_hit(int n, int x, int y) {
    for (int i = 0; i < n && i < PICKER_MAX; i++) {
        int left = card_left(i) - BORDER;
        if (x >= left && x < left + PICKER_ART_W + 2 * BORDER && y >= CARD_TOP - BORDER &&
            y < NAME_TOP + 2 * FONT_H)
            return i;
    }
    return -1;
}

int picker_key(int n, int selected, int scancode, bool *choose) {
    switch (scancode) {
    case SDL_SCANCODE_LEFT: return selected > 0 ? selected - 1 : 0;
    case SDL_SCANCODE_RIGHT: return selected < n - 1 ? selected + 1 : n - 1;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: *choose = true; return selected;
    default: return selected;
    }
}

int picker_initial(const picker_entry *e, int n, const char *last_id) {
    for (int i = 0; i < n; i++)
        if (strcmp(e[i].game->id, last_id) == 0)
            return i;
    return 0;
}

/* src (512x384) reduced to 3/4 by averaging the source pixels under each output pixel. */
static void reduce(const uint8_t *src, uint8_t *dst) {
    for (int y = 0; y < PICKER_ART_H; y++) {
        int y0 = y * ART_SRC_H / PICKER_ART_H, y1 = (y + 1) * ART_SRC_H / PICKER_ART_H;
        for (int x = 0; x < PICKER_ART_W; x++) {
            int x0 = x * ART_SRC_W / PICKER_ART_W, x1 = (x + 1) * ART_SRC_W / PICKER_ART_W;
            unsigned sum[3] = {0, 0, 0}, count = 0;
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++, count++)
                    for (int c = 0; c < 3; c++)
                        sum[c] += src[((size_t)sy * ART_SRC_W + (size_t)sx) * 4 + 1 + (size_t)c];
            uint8_t *d = dst + ((size_t)y * PICKER_ART_W + (size_t)x) * 4;
            d[0] = 0;
            for (int c = 0; c < 3; c++)
                d[1 + c] = (uint8_t)(sum[c] / count);
        }
    }
}

bool picker_load_art(const char *exe_path, uint8_t *art, char *err, size_t errlen) {
    char fork_path[PATH_MAX];
    snprintf(fork_path, sizeof fork_path, "%s/..namedfork/rsrc", exe_path);
    size_t len = 0;
    uint8_t *fork = read_file(fork_path, &len);
    if (!fork) {
        snprintf(err, errlen, "can't read %s", fork_path);
        return false;
    }
    char rerr[256];
    bool ok = false;
    if (!rsrc_open(fork, len, rerr, sizeof rerr)) {
        snprintf(err, errlen, "%s: %s", exe_path, rerr);
        free(fork);
        return false;
    }
    rsrc_entry *e = rsrc_find(FOURCC('P', 'I', 'C', 'T'), 800);
    qd_rect frame;
    uint8_t *full = NULL;
    if (!e || !pict_frame(rsrc_data(e), e->len, &frame) || rect_w(frame) != ART_SRC_W ||
        rect_h(frame) != ART_SRC_H) {
        snprintf(err, errlen, "%s has no 512x384 PICT 800", exe_path);
    } else if (!(full = calloc((size_t)ART_SRC_W * ART_SRC_H, 4))) {
        fatal("out of memory");
    } else {
        qd_pixels t = pixels(full, ART_SRC_W, ART_SRC_H);
        ok = pict_draw(rsrc_data(e), e->len, t.bounds, &t, t.bounds, BLACK, WHITE, rerr, sizeof rerr);
        if (ok)
            reduce(full, art);
        else
            snprintf(err, errlen, "%s: PICT 800: %s", exe_path, rerr);
    }
    free(full);
    rsrc_close();
    free(fork);
    return ok;
}
