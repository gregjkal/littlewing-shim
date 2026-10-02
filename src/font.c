#include "font.h"

#include <SDL3/SDL.h>
#include <string.h>

#include "util.h"

#define FIRST 0x20
#define LAST 0x7E
#define COUNT (LAST - FIRST + 1)

static uint8_t glyphs[COUNT][FONT_H];
static bool ready;

void font_init(void) {
    if (ready)
        return;
    SDL_Surface *s = SDL_CreateSurface(COUNT * FONT_W, FONT_H, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer *r = s ? SDL_CreateSoftwareRenderer(s) : NULL;
    if (!r)
        fatal("can't rasterize the dialog font: %s", SDL_GetError());
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    char all[COUNT + 1];
    for (int i = 0; i < COUNT; i++)
        all[i] = (char)(FIRST + i);
    all[COUNT] = '\0';
    SDL_RenderDebugText(r, 0, 0, all);
    SDL_RenderPresent(r);
    SDL_LockSurface(s);
    for (int c = 0; c < COUNT; c++)
        for (int y = 0; y < FONT_H; y++) {
            uint8_t bits = 0;
            for (int x = 0; x < FONT_W; x++) {
                const uint8_t *p = (const uint8_t *)s->pixels + y * s->pitch + (c * FONT_W + x) * 4;
                if (p[0] > 127)
                    bits |= (uint8_t)(0x80 >> x);
            }
            glyphs[c][y] = bits;
        }
    SDL_UnlockSurface(s);
    SDL_DestroyRenderer(r);
    SDL_DestroySurface(s);
    ready = true;
}

const uint8_t *font_glyph(uint8_t c) {
    if (c < FIRST || c > LAST)
        c = '?';
    return glyphs[c - FIRST];
}

/* ASCII spellings of Mac Roman 0x80-0xFF. */
static const char *const high[128] = {
    "A", "A", "C", "E", "N", "O", "U", "a", "a", "a", "a", "a", "a", "c", "e", "e",     /* 80 */
    "e", "e", "i", "i", "i", "i", "n", "o", "o", "o", "o", "o", "u", "u", "u", "u",     /* 90 */
    "+", "o", "c", "L", "S", "*", "P", "ss", "(R)", "(c)", "TM", "'", "\"", "!=", "AE", "O", /* A0 */
    "?", "+-", "<=", ">=", "Y", "u", "d", "?", "?", "p", "?", "a", "o", "O", "ae", "o", /* B0 */
    "?", "!", "?", "?", "f", "~", "?", "<<", ">>", "...", " ", "A", "A", "O", "OE", "oe", /* C0 */
    "-", "-", "\"", "\"", "'", "'", "/", "?", "y", "Y", "/", "E", "<", ">", "fi", "fl", /* D0 */
    "+", ".", ",", "\"", "%", "A", "E", "A", "E", "E", "I", "I", "I", "I", "O", "O",    /* E0 */
    "?", "O", "U", "U", "U", "i", "^", "~", "-", "?", ".", "o", ",", "\"", ",", "?",    /* F0 */
};

void font_ascii(const uint8_t *mac, size_t n, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        uint8_t c = mac[i];
        if (c >= 0x80) {
            for (const char *s = high[c - 0x80]; *s && o + 1 < cap; s++)
                out[o++] = *s;
        } else {
            out[o++] = c == '\r' || (c >= FIRST && c <= LAST) ? (char)c : '?';
        }
    }
    out[o] = '\0';
}

int font_wrap(const char *text, int width, int max_lines, int *starts, int *lens) {
    int per = width / FONT_W > 0 ? width / FONT_W : 1;
    int n = (int)strlen(text), nlines = 0, i = 0;
    for (;;) {
        int start = i, end = i, last_space = -1; /* the line is text[start, end) */
        while (end < n && text[end] != '\r' && end - start < per) {
            if (text[end] == ' ')
                last_space = end;
            end++;
        }
        int next = end;
        if (end < n && text[end] == '\r') {
            next = end + 1;
        } else if (end < n) { /* full, and more follows: break between words */
            if (text[end] != ' ' && last_space > start)
                end = next = last_space;
            while (next < n && text[next] == ' ')
                next++;
        }
        if (nlines < max_lines) {
            starts[nlines] = start;
            lens[nlines] = end - start;
        }
        nlines++;
        if (next >= n)
            return nlines;
        i = next;
    }
}

void font_draw(const qd_pixels *dst, int x, int y, const char *text, int n, qd_rgb color,
               qd_rect clip) {
    qd_rect lim = rect_sect(clip, dst->bounds);
    for (int k = 0; k < n; k++, x += FONT_W) {
        const uint8_t *g = font_glyph((uint8_t)text[k]);
        for (int row = 0; row < FONT_H; row++)
            for (int col = 0; col < FONT_W; col++) {
                if (!(g[row] & (0x80 >> col)))
                    continue;
                int px = x + col, py = y + row;
                if (px < lim.left || px >= lim.right || py < lim.top || py >= lim.bottom)
                    continue;
                qd_rect one = {(int16_t)py, (int16_t)px, (int16_t)(py + 1), (int16_t)(px + 1)};
                qd_fill(dst, one, lim, color);
            }
    }
}
