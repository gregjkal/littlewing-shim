#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "game.h"

/* The picture the High Scores menu's Share High Scores makes for posting:
   Loony Labyrinth's table beside the scores. The table is a picture the
   app carries (share-art/<game id>.png): the playfield as the game shows
   it in HD while it waits for a player, lamps lit, 15 seconds after
   launch. So it is the same whatever the screen shows.

   Two shapes. The tall one (1080x1350, 4:5) has a band of the table across
   its top 600 rows: the playfield inside its rails, as wide as the
   picture, its lower middle showing. The wide one (1200x675, 16:9) has the
   whole playfield down its left edge, 443 wide. The rest is black, and
   holds, top to bottom:

   - the game's title in capitals in the picker's bitmap font (font.h),
     orange, with "HIGH SCORES" in gold: on the title's line at the right
     edge in the tall shape, under it in the wide one, where the date is on
     that line's right;
   - the best score: its rank and name in green, then the score itself on a
     line of its own, large, glowing, its right edge on the right margin;
   - the other scores, a line each: rank in gray, name, and score, white;
   - SCORECARD_LINK in gold and, in the tall shape, the date at the right.

   Ranks, names and scores are in Share Tech Mono (fonts/, built into the
   program, SIL Open Font License), leaning 8 degrees as the game's display
   does. Each column is set by its ink, not by its characters' boxes: every
   rank's leftmost pixel is on the left margin, every name's on one column,
   and every score's rightmost pixel on the right margin, whatever its size.
   A score too wide for the picture is set smaller until it fits, and a
   name that would reach its score loses its last characters.

   Baselines, for the tests: tall, the lines at 806, 950 (the large score),
   1077, 1140 and 1203, between x 72 and 1008; wide, at 242, 332, 423, 468
   and 513, between x 506 and 1136. */

#define SCORECARD_LINK "GITHUB.COM/GREGJKAL/LITTLEWING-SHIM"

/* The table's picture: 336x527 of the game's pixels from (125, 54) of its
   screen, where the HD art's table is drawn, at any scale (it is at 4).
   The playfield inside its rails is its whole width and SCORECARD_ART_ROWS
   rows from SCORECARD_ART_TOP down; the bottom rail is under them. To make
   it again: LOONY_HD=hd-art/loony-labyrinth, a fixed-clock run with an
   empty data folder, and a script's screenshot at tick 900, cut at
   (500, 216), 1344x2108. */
#define SCORECARD_ART_DIR "share-art"
#define SCORECARD_ART_W 336
#define SCORECARD_ART_TOP 2
#define SCORECARD_ART_ROWS 512

typedef enum { SCORECARD_TALL, SCORECARD_WIDE } scorecard_shape;

typedef struct {
    const char *title;             /* the game's, ASCII: "Loony Labyrinth" */
    const game_high_score *scores; /* best first */
    int n;                         /* at most GAME_HIGH_SCORES */
    /* The table's picture, a PNG file. NULL, or a file that can't be
       read, leaves the table's place black. */
    const char *table;
    time_t when; /* the date shown, local time */
} scorecard;

/* Draws the picture: malloc'd RGBA rows, top to bottom, *w x *h, opaque.
   NULL if there are no scores or the picture can't be drawn. */
uint8_t *scorecard_render(const scorecard *c, scorecard_shape shape, int *w, int *h);

/* Draws the picture and writes it to path as a compressed PNG. */
bool scorecard_write(const scorecard *c, scorecard_shape shape, const char *path);

/* The same of the game being played (scorecard_init's): the scores it
   holds now (cf's live preferences) and today's date. False, with err a
   sentence saying why, if it has no scores yet or the file can't be
   written. */
bool scorecard_share(scorecard_shape shape, const char *path, char *err, size_t errlen);

/* Before the first share: the game's id and title. Finds the table's
   picture beside the program, in the app's Resources/share-art or, for a
   program run from its build folder, the repo's share-art; without it the
   table's place stays black, and that is logged.
   LOONY_SCORECARD=<file> and LOONY_SCORECARD_WIDE=<file> write the tall
   and the wide picture there when the process exits, as LOONY_SCREENSHOT
   writes the screen. */
void scorecard_init(const char *game_id, const char *title);
