#include "picker.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_scancode.h>
#include <ctype.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "blit.h"
#include "cgimage.h"
#include "display.h"
#include "font.h"
#include "pict.h"
#include "png.h"
#include "rsrc.h"
#include "settings.h"
#include "theme.h"
#include "util.h"

#define ART_SRC_W 512
#define ART_SRC_H 384
#define SLOT_W 384 /* two cards: the room each takes, the art square in it */
#define SLOT_H PICKER_ART_H
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
/* Four cards: smaller still, two rows of two. */
#define ART4_W 224
#define ART4_H 168
#define CARD4_GAP 64
#define CARD4_TOP0 108
#define CARD4_ROW 216 /* from one row's card top to the next */
#define NAME4_TOP0 284
/* Five cards: the same size, two rows of three columns, the second row
   centered under the first. */
#define CARD5_GAP 56
/* Seven or eight cards: smaller again, a row of four over a row of three
   or four; a name wider than its card goes on two lines. */
#define ART7_W 176
#define ART7_H 132
#define CARD7_GAP 24
#define CARD7_ROW 200
#define NAME7_TOP0 252
/* Nine cards: three rows of three, the columns 272 pixels apart so that
   every name fits on one line with 24 pixels to its neighbour's (the
   longest two, LOONY LABYRINTH and CRYSTAL CALIBURN, side by side), and
   the heading and the hint moved toward the screen's edges to make room
   for three rows. */
#define ART9_W 160
#define ART9_H 120
#define CARD9_GAP 112
#define CARD9_TOP0 76
#define CARD9_ROW 164
#define HEADING9_TOP 32
#define HINT9_TOP 566
#define NAME9_BELOW 10 /* from a card's art bottom to its name */
#define NAME_LEADING 4 /* between a two-line name's lines */
#define ICON_SIZE 128 /* icns 128's largest image, it32 */

static const qd_rgb BLACK = {0, 0, 0}, GOLD = {0xFFFF, 0xCCCC, 0x3333}, DARK = {0x3333, 0x3333, 0x3333},
                    PLAIN = {0x2222, 0x2222, 0x2222}, WHITE = {0xFFFF, 0xFFFF, 0xFFFF},
                    GRAY = {0x8888, 0x8888, 0x8888};

/* Cards in a row: all of them, except four, which take two rows of two,
   five and six, two rows of three, seven and eight, rows of four, and
   nine, three rows of three. */
static int columns(int n) { return n == 4 ? 2 : n == 5 || n == 6 || n == 9 ? 3 : n >= 7 ? 4 : n; }

/* Whether n cards take rows of four, where a name may need two lines. */
static bool four_across(int n) { return n == 7 || n == 8; }

/* The room card i takes, 4:3: two side by side, three smaller ones, or
   four to eight in two rows and nine in three, each row centered. */
static qd_rect slot(int n, int i) {
    if (n >= 4) {
        int cols = columns(n), row = i / cols;
        int w = n == 9 ? ART9_W : n >= 7 ? ART7_W : ART4_W, h = n == 9 ? ART9_H : n >= 7 ? ART7_H : ART4_H;
        int gap = n == 4 ? CARD4_GAP : n == 9 ? CARD9_GAP : n >= 7 ? CARD7_GAP : CARD5_GAP;
        int in_row = n - row * cols < cols ? n - row * cols : cols;
        int left = (PICKER_W - in_row * w - (in_row - 1) * gap) / 2 + i % cols * (w + gap);
        int top = n == 9 ? CARD9_TOP0 + row * CARD9_ROW
                         : CARD4_TOP0 + row * (n >= 7 ? CARD7_ROW : CARD4_ROW);
        return (qd_rect){(int16_t)top, (int16_t)left, (int16_t)(top + h), (int16_t)(left + w)};
    }
    if (n == 3) {
        int left = CARD3_LEFT0 + i * (ART3_W + CARD3_GAP);
        return (qd_rect){CARD3_TOP, (int16_t)left, CARD3_TOP + ART3_H, (int16_t)(left + ART3_W)};
    }
    int left = CARD_LEFT0 + i * (SLOT_W + CARD_GAP);
    return (qd_rect){CARD_TOP, (int16_t)left, CARD_TOP + SLOT_H, (int16_t)(left + SLOT_W)};
}

/* Where card i's art goes: a square as tall as its slot, centered in it. */
static qd_rect card(int n, int i) {
    qd_rect r = slot(n, i);
    int h = rect_h(r), left = r.left + (rect_w(r) - h) / 2;
    return (qd_rect){r.top, (int16_t)left, r.bottom, (int16_t)(left + h)};
}

static int name_top(int n, int i) {
    if (n == 9)
        return card(n, i).bottom + NAME9_BELOW;
    if (n >= 7)
        return NAME7_TOP0 + i / columns(n) * CARD7_ROW;
    return n >= 4 ? NAME4_TOP0 + i / columns(n) * CARD4_ROW : n == 3 ? NAME3_TOP : NAME_TOP;
}

/* The widest a name may be before it takes two lines: a column, one slot
   and the gap beside it, or with four across, where a name that wide
   would touch its neighbour's, the slot alone. */
static int name_room(int n) {
    qd_rect a = slot(n, 0), b = slot(n, 1);
    return four_across(n) ? rect_w(a) : b.left - a.left;
}

/* Where name (scale 2) breaks into two lines under a column room pixels
   wide: the last space that leaves the first line fitting, or 0 if it
   fits on one. */
static size_t name_break(const char *name, int room) {
    size_t n = strlen(name), at = 0;
    if ((int)n * FONT_W * 2 <= room)
        return 0;
    for (size_t k = 1; k < n; k++)
        if (name[k] == ' ' && ((int)k * FONT_W * 2 <= room || !at))
            at = k;
    return at;
}

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
    draw_text(&s, PICKER_W / 2, n == 9 ? HEADING9_TOP : HEADING_TOP, "LITTLEWING PINBALL", 3, GOLD);
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
        size_t cut = name_break(name, name_room(n));
        if (cut) {
            name[cut] = '\0';
            draw_text(&s, left + w / 2, name_top(n, i) + 2 * FONT_H + NAME_LEADING, name + cut + 1, 2,
                      i == selected ? WHITE : GRAY);
        }
        draw_text(&s, left + w / 2, name_top(n, i), name, 2, i == selected ? WHITE : GRAY);
    }
    draw_text(&s, PICKER_W / 2, n == 9 ? HINT9_TOP : HINT_TOP, "RETURN TO PLAY - CMD-Q TO QUIT", 2, GRAY);
}

int picker_hit(int n, int x, int y) {
    for (int i = 0; i < n && i < PICKER_MAX; i++) {
        qd_rect c = slot(n, i);
        int bottom = name_top(n, i) + 2 * FONT_H + (four_across(n) ? 2 * FONT_H + NAME_LEADING : 0);
        if (x >= c.left - BORDER && x < c.right + BORDER && y >= c.top - BORDER && y < bottom)
            return i;
    }
    return -1;
}

int picker_key(int n, int selected, int scancode, bool *choose) {
    switch (scancode) {
    case SDL_SCANCODE_LEFT: return selected > 0 ? selected - 1 : 0;
    case SDL_SCANCODE_RIGHT: return selected < n - 1 ? selected + 1 : n - 1;
    case SDL_SCANCODE_UP: return selected >= columns(n) ? selected - columns(n) : selected;
    case SDL_SCANCODE_DOWN: /* to the card below, or the last card when the row below is short */
        return selected + columns(n) < n                   ? selected + columns(n)
               : selected / columns(n) < (n - 1) / columns(n) ? n - 1
                                                             : selected;
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

/* Unpacks one 128x128 plane of an it32 image: a byte n below 0x80 copies
   the next n + 1 bytes, any other repeats the next byte n - 0x80 + 3 times.
   *at moves past what was read. False if the data ends first or a run
   overflows the plane. */
static bool unpack_plane(const uint8_t *p, size_t len, size_t *at, uint8_t *plane) {
    size_t k = 0;
    while (k < ICON_SIZE * ICON_SIZE) {
        if (*at >= len)
            return false;
        uint8_t b = p[(*at)++];
        size_t run = b < 0x80 ? (size_t)b + 1 : (size_t)b - 0x80 + 3;
        if (k + run > ICON_SIZE * ICON_SIZE || *at + (b < 0x80 ? run : 1) > len)
            return false;
        if (b < 0x80) {
            memcpy(plane + k, p + *at, run);
            *at += run;
        } else {
            memset(plane + k, p[(*at)++], run);
        }
        k += run;
    }
    return true;
}

bool picker_load_icns(const uint8_t *p, size_t len, uint8_t *art) {
    const uint8_t *rgb = NULL, *mask = NULL;
    size_t rgb_len = 0;
    for (size_t at = 8; at + 8 <= len;) {
        uint32_t type = rd_be32(p + at), n = rd_be32(p + at + 4);
        if (n < 8 || n > len - at)
            return false;
        if (type == FOURCC('i', 't', '3', '2')) {
            rgb = p + at + 8;
            rgb_len = n - 8;
        } else if (type == FOURCC('t', '8', 'm', 'k') && n - 8 == ICON_SIZE * ICON_SIZE) {
            mask = p + at + 8;
        }
        at += n;
    }
    static uint8_t planes[3][ICON_SIZE * ICON_SIZE];
    size_t at = 4; /* it32 starts with four zero bytes */
    if (!rgb || !mask || rgb_len < at)
        return false;
    for (int c = 0; c < 3; c++)
        if (!unpack_plane(rgb, rgb_len, &at, planes[c]))
            return false;
    qd_pixels a = pixels(art, PICKER_ART_W, PICKER_ART_H);
    qd_fill(&a, a.bounds, a.bounds, PLAIN);
    int left = (PICKER_ART_W - ICON_SIZE) / 2, top = (PICKER_ART_H - ICON_SIZE) / 2;
    for (int y = 0; y < ICON_SIZE; y++)
        for (int x = 0; x < ICON_SIZE; x++) {
            size_t i = (size_t)y * ICON_SIZE + (size_t)x;
            uint8_t *d = art + ((size_t)(top + y) * PICKER_ART_W + (size_t)(left + x)) * 4;
            for (int c = 0; c < 3; c++)
                d[1 + c] = (uint8_t)((planes[c][i] * mask[i] + 0x22 * (255 - mask[i]) + 127) / 255);
        }
    return true;
}

/* A picture (w x h xRGB), reduced to fit the card if it's larger and
   centered on the plain card color, into art. */
static void fit(const uint8_t *full, int w, int h, uint8_t *art) {
    int dw = w, dh = h;
    if (dw > PICKER_ART_W || dh > PICKER_ART_H) {
        bool wide = w * PICKER_ART_H > h * PICKER_ART_W;
        dw = wide ? PICKER_ART_W : w * PICKER_ART_H / h;
        dh = wide ? h * PICKER_ART_W / w : PICKER_ART_H;
    }
    qd_pixels a = pixels(art, PICKER_ART_W, PICKER_ART_H);
    qd_fill(&a, a.bounds, a.bounds, PLAIN);
    reduce(full, w, h, art + ((size_t)(PICKER_ART_H - dh) / 2 * PICKER_ART_W + (size_t)(PICKER_ART_W - dw) / 2) * 4,
           dw, dh, PICKER_ART_W * 4);
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
    qd_rect frame;
    rsrc_entry *e = rsrc_find(FOURCC('P', 'I', 'C', 'T'), 800), *icon = rsrc_find(FOURCC('i', 'c', 'n', 's'), 128);
    if (e && (!pict_frame(rsrc_data(e), e->len, &frame) || rect_w(frame) != ART_SRC_W || rect_h(frame) != ART_SRC_H))
        e = NULL; /* Tristan's PICT 800 is its credits */
    rsrc_entry *title = e || icon ? NULL : rsrc_find(FOURCC('P', 'I', 'C', 'T'), 900);
    if (title && (!pict_frame(rsrc_data(title), title->len, &frame) || rect_w(frame) <= 0 || rect_h(frame) <= 0 ||
                  rect_w(frame) > ART_SRC_W || rect_h(frame) > ART_SRC_H))
        title = NULL;
    uint8_t *full = NULL;
    if (icon && !e) {
        ok = picker_load_icns(rsrc_data(icon), icon->len, art);
        if (!ok)
            snprintf(err, errlen, "%s: icns 128 has no readable 128x128 icon (it32) and mask (t8mk)",
                     exe_path);
    } else if (!e && !title) {
        snprintf(err, errlen, "%s has no 512x384 PICT 800, icns 128 or PICT 900", exe_path);
    } else if (!(full = calloc((size_t)ART_SRC_W * ART_SRC_H, 4))) {
        fatal("out of memory");
    } else if (e) {
        qd_pixels t = pixels(full, ART_SRC_W, ART_SRC_H);
        ok = pict_draw(rsrc_data(e), e->len, t.bounds, &t, t.bounds, BLACK, WHITE, rerr, sizeof rerr);
        if (ok)
            fit(full, ART_SRC_W, ART_SRC_H, art);
        else
            snprintf(err, errlen, "%s: PICT 800: %s", exe_path, rerr);
    } else {
        int w = rect_w(frame), h = rect_h(frame);
        qd_pixels t = pixels(full, w, h);
        ok = pict_draw(rsrc_data(title), title->len, t.bounds, &t, t.bounds, BLACK, WHITE, rerr, sizeof rerr);
        if (ok)
            fit(full, w, h, art);
        else
            snprintf(err, errlen, "%s: PICT 900: %s", exe_path, rerr);
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

bool picker_load_bundle_art(const char *app, uint8_t *art, char *err, size_t errlen) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/Contents/Resources/appl.png", app);
    if (access(path, F_OK) == 0)
        return picker_load_icon(path, art, err, errlen);
    snprintf(path, sizeof path, "%s/Contents/Resources/app.icns", app);
    size_t len = 0;
    uint8_t *icns = read_file(path, &len);
    bool ok = icns && picker_load_icns(icns, len, art);
    if (!ok)
        snprintf(err, errlen, "%s has neither appl.png nor an app.icns with a 128x128 icon", app);
    free(icns);
    return ok;
}

bool picker_bundled_art(const char *exe_path, const char *game_id, char *out, size_t cap) {
    const char *macos = exe_path ? strstr(exe_path, ".app/Contents/MacOS/") : NULL;
    if (!macos)
        return false;
    int n = snprintf(out, cap, "%.*s.app/Contents/Resources/picker-art/%s.png", (int)(macos - exe_path),
                     exe_path, game_id);
    return n > 0 && (size_t)n < cap && access(out, F_OK) == 0;
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
    char self[PATH_MAX];
    uint32_t self_size = sizeof self;
    bool have_self = _NSGetExecutablePath(self, &self_size) == 0;
    for (int i = 0; i < n; i++) {
        e[i].game = games[i];
        e[i].art = malloc(PICKER_ART_W * PICKER_ART_H * 4);
        char dir[PATH_MAX], path[PATH_MAX + 64], err[512];
        game_folder(games[i], dir, sizeof dir);
        bool bundle = games[i]->kind == GAME_MACHO_BUNDLE;
        bool loaded = false;
        if (e[i].art && have_self && picker_bundled_art(self, games[i]->id, path, sizeof path) &&
            !(loaded = picker_load_icon(path, e[i].art, err, sizeof err)))
            log_msg("picker: %s", err); /* the game's own art instead */
        if (e[i].art && !loaded) {
            snprintf(path, sizeof path, "%s/%s", dir, games[i]->exe);
            loaded = bundle ? picker_load_bundle_art(dir, e[i].art, err, sizeof err)
                            : picker_load_art(path, e[i].art, err, sizeof err);
        }
        if (!loaded) {
            log_msg("picker: %s", e[i].art ? err : "out of memory");
            free(e[i].art);
            e[i].art = NULL;
        }
    }
    char last[64] = "";
    settings_get_str(SETTINGS_LAST_GAME, last, sizeof last);
    memset(&P, 0, sizeof P);
    P.n = n;
    P.selected = picker_initial(e, n, last);
    P.dirty = true;
    const game_info *shown[PICKER_MAX];
    for (int i = 0; i < n; i++)
        shown[i] = e[i].game;
    theme_start(shown, n, P.selected, have_self ? self : NULL);
    theme_select(P.selected);
    int playing = P.selected;
    static const display_input input = {on_key, NULL, on_quit, on_mouse, NULL, NULL};
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
        if (P.selected != playing && !P.choose)
            theme_select(playing = P.selected);
        theme_tend();
        SDL_Delay(1); /* without vsync (the dummy driver), don't spin */
    }
    theme_stop();
    for (int i = 0; i < n; i++)
        free(e[i].art);
    if (P.quit) {
        log_msg("picker: quit");
        exit(0);
    }
    log_msg("picker: %s", e[P.selected].game->id);
    settings_set_str(SETTINGS_LAST_GAME, e[P.selected].game->id);
    return e[P.selected].game;
}
