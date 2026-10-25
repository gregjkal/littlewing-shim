#pragma once
#include <stdint.h>

/* QuickDraw's pen and patterns, the shapes drawn with them, and the
   rectangle and point arithmetic. Reference: "Inside Macintosh: Imaging
   With QuickDraw", chapter 3.

   The pen is the current port's: its location, size, mode and visibility
   are in the CGrafPort's fields (qd.h), and its pattern and the port's
   background pattern are 8-byte guest handles in pnPixPat and bkPixPat,
   made the first time they are set (0 means black and white). A pattern's
   1 bits are drawn in the port's foreground color and its 0 bits in the
   background color, aligned to the port's pixels. Everything drawn clips
   to the port's portRect and clip region (qd_draw).

   The pen hangs below and to the right of its location. Lines draw every
   point between their ends, both included; a hidden pen (HidePen) moves
   but draws nothing. Rounded rectangles' corners are quarter ellipses of
   the oval's width and height. */

/* Writes the QuickDraw globals' five patterns at addr, in their order
   there: dkGray, ltGray, gray, black, white (8 bytes each). */
void pen_write_qd_patterns(uint32_t addr);

/* Registers MoveTo, Move, GetPen, LineTo, Line, PenSize, PenMode, PenPat,
   PenNormal, HidePen, ShowPen, GetPenState, SetPenState, BackPat, FrameRect,
   EraseRect, FillRect, FrameRoundRect, PaintRoundRect, EraseRoundRect,
   PaintRgn, EraseRgn, FillRgn, RGBBackColor, GetForeColor, GetBackColor,
   the GetQDGlobals patterns (Black, White, Gray, LightGray, DarkGray),
   SetRect's siblings InsetRect, SectRect, UnionRect, EqualRect, EmptyRect
   and PtInRect, and EqualPt. Requires qd_init(). */
void pen_register(void);
