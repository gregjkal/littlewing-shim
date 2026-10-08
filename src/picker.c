#include "picker.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_scancode.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blit.h"
#include "cgimage.h"
#include "display.h"
#include "files.h"
#include "font.h"
#include "pict.h"
#include "plist.h"
#include "png.h"
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
/* Three cards: smaller art, in a row centered on the screen. */
#define ART3_W 240
#define ART3_H 180
#define CARD3_GAP 24
#define CARD3_LEFT0 ((PICKER_W - 3 * ART3_W - 2 * CARD3_GAP) / 2)
#define CARD3_TOP 180
#define NAME3_TOP 392

static const qd_rgb BLACK = {0, 0, 0}, GOLD = {0xFFFF, 0xCCCC, 0x3333}, DARK = {0x3333, 0x3333, 0x3333},
                    PLAIN = {0x2222, 0x2222, 0x2222}, WHITE = {0xFFFF, 0xFFFF, 0xFFFF},
                    GRAY = {0x8888, 0x8888, 0x8888};

/* Where card i's art goes: two cards side by side, or three smaller ones. */
static qd_rect card(int n, int i) {
    if (n == 3) {
        int left = CARD3_LEFT0 + i * (ART3_W + CARD3_GAP);
        return (qd_rect){CARD3_TOP, (int16_t)left, CARD3_TOP + ART3_H, (int16_t)(left + ART3_W)};
    }
    int left = CARD_LEFT0 + i * (PICKER_ART_W + CARD_GAP);
    return (qd_rect){CARD_TOP, (int16_t)left, CARD_TOP + PICKER_ART_H, (int16_t)(left + PICKER_ART_W)};
}

static int name_top(int n) { return n == 3 ? NAME3_TOP : NAME_TOP; }

static qd_pixels pixels(uint8_t *base, int w, int h) {
    qd_pixels p = {base, (uint32_t)w * 4, {0, 0, (int16_t)h, (int16_t)w}, 32, NULL};
    return p;
}

static qd_rect rect(int left, int top, int w, int h) {
    qd_rect r = {(int16_t)top, (int16_t)left, (int16_t)(top + h), (int16_t)(left + w)};
    return r;
}

/* src (sw x sh xRGB) reduced to dw x dh (no larger) by averaging the source
   pixels under each output pixel; dst rows are dst_stride bytes apart. */
static void reduce(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh, size_t dst_stride) {
    for (int y = 0; y < dh; y++) {
        int y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
        for (int x = 0; x < dw; x++) {
            int x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
            unsigned sum[3] = {0, 0, 0}, count = 0;
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++, count++)
                    for (int c = 0; c < 3; c++)
                        sum[c] += src[((size_t)sy * (size_t)sw + (size_t)sx) * 4 + 1 + (size_t)c];
            uint8_t *d = dst + (size_t)y * dst_stride + (size_t)x * 4;
            d[0] = 0;
            for (int c = 0; c < 3; c++)
                d[1 + c] = (uint8_t)(sum[c] / count);
        }
    }
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
        qd_rect c = card(n, i);
        int left = c.left, w = rect_w(c), h = rect_h(c);
        qd_fill(&s, rect(left - BORDER, c.top - BORDER, w + 2 * BORDER, h + 2 * BORDER), s.bounds,
                i == selected ? GOLD : DARK);
        uint8_t *at = screen + ((size_t)c.top * PICKER_W + (size_t)left) * 4;
        if (!e[i].art)
            qd_fill(&s, c, s.bounds, PLAIN);
        else if (w == PICKER_ART_W)
            for (int y = 0; y < PICKER_ART_H; y++)
                memcpy(at + (size_t)y * PICKER_W * 4, e[i].art + (size_t)y * PICKER_ART_W * 4, PICKER_ART_W * 4);
        else
            reduce(e[i].art, PICKER_ART_W, PICKER_ART_H, at, w, h, PICKER_W * 4);
        char name[64];
        size_t k = 0;
        for (const char *p = e[i].game->title; *p && k + 1 < sizeof name; p++)
            name[k++] = (char)toupper((unsigned char)*p);
        name[k] = '\0';
        draw_text(&s, left + w / 2, name_top(n), name, 2, i == selected ? WHITE : GRAY);
    }
    draw_text(&s, PICKER_W / 2, HINT_TOP, "RETURN TO PLAY - CMD-Q TO QUIT", 2, GRAY);
}

int picker_hit(int n, int x, int y) {
    for (int i = 0; i < n && i < PICKER_MAX; i++) {
        qd_rect c = card(n, i);
        if (x >= c.left - BORDER && x < c.right + BORDER && y >= c.top - BORDER && y < name_top(n) + 2 * FONT_H)
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
            reduce(full, ART_SRC_W, ART_SRC_H, art, PICKER_ART_W, PICKER_ART_H, PICKER_ART_W * 4);
        else
            snprintf(err, errlen, "%s: PICT 800: %s", exe_path, rerr);
    }
    free(full);
    rsrc_close();
    free(fork);
    return ok;
}

bool picker_load_icon(const char *png_path, uint8_t *art, char *err, size_t errlen) {
    cgimage_pixels icon;
    if (!cgimage_decode_png_over(png_path, 0x22, 0x22, 0x22, &icon, err, errlen))
        return false;
    bool ok = icon.width <= PICKER_ART_W && icon.height <= PICKER_ART_H;
    if (!ok) {
        snprintf(err, errlen, "%s is %dx%d, larger than a card", png_path, icon.width, icon.height);
    } else {
        qd_pixels a = pixels(art, PICKER_ART_W, PICKER_ART_H);
        qd_fill(&a, a.bounds, a.bounds, PLAIN);
        int left = (PICKER_ART_W - icon.width) / 2, top = (PICKER_ART_H - icon.height) / 2;
        for (int y = 0; y < icon.height; y++)
            memcpy(art + ((size_t)(top + y) * PICKER_ART_W + (size_t)left) * 4,
                   icon.xrgb + (size_t)y * (size_t)icon.width * 4, (size_t)icon.width * 4);
    }
    free(icon.xrgb);
    return ok;
}

static struct {
    int n, selected;
    bool choose, quit, dirty;
} P;

static void on_key(int scancode, bool down, bool repeat) {
    (void)repeat;
    if (!down)
        return;
    int before = P.selected;
    P.selected = picker_key(P.n, P.selected, scancode, &P.choose);
    P.dirty |= P.selected != before;
}

static void on_mouse(int x, int y, bool down) {
    int i = picker_hit(P.n, x, y);
    if (!down || i < 0)
        return;
    P.selected = i;
    P.choose = true;
}

static void on_quit(void) { P.quit = true; }

static void last_path(char *out, size_t cap, bool *ok) {
    char root[PATH_MAX];
    *ok = files_data_root(root, sizeof root);
    if (*ok)
        snprintf(out, cap, "%s/" GAME_PICKER_FILE, root);
}

static void load_last(char *id, size_t cap) {
    id[0] = '\0';
    char path[PATH_MAX], err[256];
    bool ok;
    last_path(path, sizeof path, &ok);
    plist_entry *e;
    uint32_t n;
    if (!ok || plist_read(path, &e, &n, err, sizeof err) != PLIST_OK)
        return;
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(e[i].key, "last game") == 0 && !e[i].is_number)
            snprintf(id, cap, "%s", e[i].str);
    plist_free(e, n);
}

static void save_last(const char *id) {
    char path[PATH_MAX], err[256];
    bool ok;
    last_path(path, sizeof path, &ok);
    plist_entry e = {.key = "last game", .str = (char *)id};
    if (ok && !plist_write(path, &e, 1, err, sizeof err))
        log_msg("picker: can't remember the last game: %s", err);
}

/* The next LOONY_PICK item into item ("" if none), passing the rest on. */
static void next_scripted_pick(char *item, size_t cap) {
    item[0] = '\0';
    const char *s = getenv("LOONY_PICK");
    if (!s || !*s)
        return;
    const char *comma = strchr(s, ',');
    snprintf(item, cap, "%.*s", (int)(comma ? (size_t)(comma - s) : strlen(s)), s);
    if (comma)
        setenv("LOONY_PICK", comma + 1, 1);
    else
        unsetenv("LOONY_PICK");
}

const game_info *picker_run(const game_info *const *games, int n) {
    if (n > PICKER_MAX)
        n = PICKER_MAX;
    picker_entry e[PICKER_MAX];
    for (int i = 0; i < n; i++) {
        e[i].game = games[i];
        e[i].art = malloc(PICKER_ART_W * PICKER_ART_H * 4);
        char dir[PATH_MAX], path[PATH_MAX + 64], err[512];
        game_folder(games[i], dir, sizeof dir);
        bool bundle = games[i]->kind == GAME_MACHO_BUNDLE;
        if (bundle)
            snprintf(path, sizeof path, "%s/Contents/Resources/appl.png", dir);
        else
            snprintf(path, sizeof path, "%s/%s", dir, games[i]->exe);
        bool loaded = e[i].art && (bundle ? picker_load_icon(path, e[i].art, err, sizeof err)
                                          : picker_load_art(path, e[i].art, err, sizeof err));
        if (!loaded) {
            log_msg("picker: %s", e[i].art ? err : "out of memory");
            free(e[i].art);
            e[i].art = NULL;
        }
    }
    char last[64];
    load_last(last, sizeof last);
    memset(&P, 0, sizeof P);
    P.n = n;
    P.selected = picker_initial(e, n, last);
    P.dirty = true;
    static const display_input input = {on_key, NULL, on_quit, on_mouse, NULL};
    display_set_input(&input);
    display_set_title("LittleWing");
    static uint8_t screen[PICKER_W * PICKER_H * 4], rgba[PICKER_W * PICKER_H * 4];
    char scripted[64] = "";
    bool first = true;
    while (!P.choose && !P.quit) {
        if (P.dirty) {
            picker_draw(e, n, P.selected, screen);
            qd_pixels s = {screen, PICKER_W * 4, {0, 0, PICKER_H, PICKER_W}, 32, NULL};
            qd_to_rgba(&s, rgba);
            P.dirty = false;
        }
        display_present_rgba(rgba, PICKER_W, PICKER_H); /* waits for the display's refresh */
        if (first) {
            first = false;
            const char *shot = getenv("LOONY_PICKER_SHOT");
            if (shot && *shot && !png_write_rgba(shot, rgba, PICKER_W, PICKER_H))
                log_msg("picker: can't write %s", shot);
            next_scripted_pick(scripted, sizeof scripted);
            if (strcmp(scripted, "quit") == 0)
                P.quit = true;
            for (int i = 0; i < n; i++)
                if (strcmp(scripted, e[i].game->id) == 0) {
                    P.selected = i;
                    P.choose = true;
                }
        }
        display_poll();
        SDL_Delay(1); /* without vsync (the dummy driver), don't spin */
    }
    for (int i = 0; i < n; i++)
        free(e[i].art);
    if (P.quit) {
        log_msg("picker: quit");
        exit(0);
    }
    log_msg("picker: %s", e[P.selected].game->id);
    save_last(e[P.selected].game->id);
    return e[P.selected].game;
}
