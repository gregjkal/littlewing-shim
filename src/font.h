#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "blit.h"

/* The dialogs' bitmap font: SDL's built-in 8x8 debug font (printable ASCII),
   rasterized once with SDL's software renderer, so no font data lives in
   the repo. Mac Roman text is shown through an ASCII spelling: accented
   letters lose their accents, "\xaa" (the trademark sign) becomes "TM",
   curly quotes become straight ones, and anything else without one becomes
   '?'. */

#define FONT_W 8      /* advance per character */
#define FONT_H 8      /* glyph height */
#define FONT_LINE 12  /* distance between lines */

/* Rasterizes the glyphs. Safe to call more than once. Needs no SDL_Init. */
void font_init(void);

/* The 8 rows of character c (0x20-0x7E; others give '?'), most significant
   bit leftmost. */
const uint8_t *font_glyph(uint8_t c);

/* Writes the ASCII spelling of Mac Roman text (n bytes) to out, NUL-terminated.
   Carriage returns stay as '\r'. */
void font_ascii(const uint8_t *mac, size_t n, char *out, size_t cap);

/* Breaks ASCII text into lines at most width pixels wide: at '\r', and
   between words where a line would get too long (a word longer than a line
   is split). Writes up to max_lines (start, length) pairs into starts/lens
   and returns the number of lines, which may exceed max_lines. */
int font_wrap(const char *text, int width, int max_lines, int *starts, int *lens);

/* Draws n characters of ASCII text with its top-left at (x, y): set glyph
   bits become color, the rest is left alone. Only pixels inside clip and
   dst's bounds are written. */
void font_draw(const qd_pixels *dst, int x, int y, const char *text, int n, qd_rgb color,
               qd_rect clip);
