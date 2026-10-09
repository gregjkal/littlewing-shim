#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "blit.h"

/* HD mode (a prototype): every pixel buffer the game draws into gets a
   hidden RGBA copy at N times its size, and the screen's copy is what the
   window shows. The game still sees and draws only its own 1x pixels.

   - CopyBits is repeated on the HD copies.
   - DrawPicture of a picture with replacement art draws that art into the
     HD copy. A picture is named by the FNV-1a hash of its data, and its art
     is <art dir>/<hash>.png (8 hex digits), at any size.
   - Anything else that changed a 1x pixel (the game's own drawing code,
     PaintRect, dialogs) is found by comparing the 1x pixels against what the
     HD copy last showed. A changed pixel that is back to what art was drawn
     over gets that art again (a lamp going out); a patch of changed pixels
     that holds exactly a small picture with art (a sprite: a lit lamp, a
     flipper) gets that picture's art; any other pixel is drawn as an NxN
     block of its color.
   - Each HD copy notes which of its rows changed, and how wide, so the
     window uploads only those parts of the screen's copy (hd_take_changes).
     An unscaled CopyBits of pixels already there (the game copies its
     whole table each frame) changes nothing.

   Environment: LOONY_HD=<art dir> turns it on, LOONY_HD_SCALE=<2..8> sets N
   (default 4), and LOONY_HD_DUMP=<dir> writes each distinct picture the game
   draws to <dir>/<hash>.png at its own size, as a starting point for art. */

void hd_init(void);

/* Sets the mode directly (tests): scale 0 turns HD off. Either directory may
   be NULL. Drops every HD copy and all loaded art. */
void hd_configure(int scale, const char *art_dir, const char *dump_dir);

/* N, or 0 when HD is off. */
int hd_scale(void);

/* Forgets the HD copy of the buffer at base, which is being freed. */
void hd_forget(const uint8_t *base);

/* After qd_blit(src, sr, dst, dr, clip) succeeded: repeats it in HD. */
void hd_copy(const qd_pixels *src, qd_rect sr, const qd_pixels *dst, qd_rect dr, qd_rect clip);

/* After pict_draw(data, len, dst, target, clip) succeeded: dumps the picture
   and draws its replacement art. */
void hd_picture(const uint8_t *data, size_t len, qd_rect dst, const qd_pixels *target,
                qd_rect clip);

/* The HD copy of screen, brought up to date: RGBA rows, *w x *h pixels. */
const uint8_t *hd_frame(const qd_pixels *screen, int *w, int *h);

/* The parts of screen's HD copy that changed since the last call (all of it
   the first time), and forgets them: rects in the copy's own pixels, one per
   run of 1x rows with changes, each as wide as the changes in its rows, top
   to bottom. Returns how many, at most max; when there are more runs, the
   last rect covers all of the rest. Call after hd_frame, which can change it. */
int hd_take_changes(const qd_pixels *screen, qd_rect *out, int max);
