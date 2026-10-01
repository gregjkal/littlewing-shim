#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "blit.h"

/* Draws a PICT (version 1 or 2) into target, mapping the picture's frame onto
   dst and clipping to clip. Supported opcodes: version, header, DefHilite,
   clip (rectangular), BitsRect, PackBitsRect (1, 2, 4 and 8 bits, srcCopy),
   short and long comments, and end. Anything else writes err and returns false.
   Reference: Inside Macintosh: Imaging With QuickDraw, appendix A. */
bool pict_draw(const uint8_t *data, size_t len, qd_rect dst, const qd_pixels *target, qd_rect clip,
               qd_rgb fg, qd_rgb bg, char *err, size_t errlen);

/* The picture frame from the header. False if data is too short. */
bool pict_frame(const uint8_t *data, size_t len, qd_rect *frame);
