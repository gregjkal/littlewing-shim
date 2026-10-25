#pragma once
#include <stdint.h>

/* QuickDraw text, drawn with the Mac's own fonts through CoreText, and the
   Text Utilities' string comparisons. Reference: "Inside Macintosh: Text",
   chapters 3 (QuickDraw Text) and 5 (Text Utilities).

   The port's text state is its CGrafPort's fields (qd.h): txFont, txFace,
   txMode and txSize, which a new port starts at the system font, plain,
   srcOr and size 0 (12 points). Font numbers are the classic ones: 0, the
   system font (Chicago, which macOS lacks, drawn in Lucida Grande Bold),
   1, the application font (Geneva), 2 New York (as Times), 3 Geneva,
   4 Monaco, 16 Palatino, 20 Times, 21 Helvetica, 22 Courier and 23 Symbol;
   GetFNum knows their names, and any other number draws in Geneva.

   Text is Mac Roman, laid out a character at a time at whole-pixel
   advances, without kerning, as QuickDraw did, so TextWidth is what
   DrawText moves the pen. Glyphs are drawn without antialiasing, as a
   one-bit mask, in the port's foreground color in txMode (the source
   modes, srcCopy filling the background of the line's box), clipped like
   every other drawing (qd_draw). Bold smears each glyph one pixel right
   and widens it one pixel, italic slants it, underline draws a line below
   the baseline, condense and extend narrow and widen each character by
   a pixel; outline and shadow draw as plain. How a glyph looks is up to
   the host's copy of the font, so a frame with text can differ between
   macOS versions. */

/* Registers TextFont, TextSize, TextFace, TextMode, the GetPortText
   getters, GetFNum, GetAppFont, GetSysFont, GetFontInfo, FMSwapFont,
   DrawString, DrawText, DrawChar, StringWidth, TextWidth, CharWidth,
   EqualString, IdenticalString, IdenticalText, CompareText and
   StringToNum, and InitFonts and TEInit, which do nothing. Requires
   qd_init() and mm_init(). */
void text_register(void);
