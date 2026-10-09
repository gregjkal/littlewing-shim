#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "game.h"

/* The game picker LittleWing.app shows when more than one game is
   installed: each game's title picture (PICT 800 in its own resource fork,
   read from the user's copy) on a card, its name under it, and the
   selection marked in gold. A bundle game (MONSTER FAIR) has no title
   picture the shim reads: its card shows its icon, appl.png, centered.
   Two games get two large cards side by side; three get three smaller
   ones (240x180) in a row, the art reduced to fit. This file draws and
   reads input; picker_run (Task 5) shows it. Pixels here are QuickDraw
   32-bit: big-endian xRGB. */

#define PICKER_W 800
#define PICKER_H 600
#define PICKER_ART_W 384 /* PICT 800 (512x384) at 3/4 */
#define PICKER_ART_H 288
#define PICKER_MAX 3     /* the layouts fit two or three games */

typedef struct {
    const game_info *game;
    uint8_t *art; /* PICKER_ART_W x PICKER_ART_H, or NULL for a plain card */
} picker_entry;

/* Reads the program's resource fork (exe_path/..namedfork/rsrc), draws its
   PICT 800 and reduces it into art. False, with err naming the file, if
   the fork or the picture can't be read or the picture isn't 512x384.
   Opens and closes the Resource Manager's fork, so call it before the
   game's own is opened. */
bool picker_load_art(const char *exe_path, uint8_t *art, char *err, size_t errlen);

/* A bundle game's card: the PNG at png_path (at most PICKER_ART_W x
   PICKER_ART_H), centered on the plain card color, into art. False, with
   err saying why, if it can't be read or is too large. */
bool picker_load_icon(const char *png_path, uint8_t *art, char *err, size_t errlen);

/* Draws the whole picker into screen (PICKER_W x PICKER_H) with entry
   `selected` marked. n is at most PICKER_MAX. */
void picker_draw(const picker_entry *e, int n, int selected, uint8_t *screen);

/* The entry whose card (with its border and name) holds the point, or -1. */
int picker_hit(int n, int x, int y);

/* The selection after an SDL scancode: Left and Right move (stopping at
   the ends), Return and keypad Enter set *choose. Other keys change nothing. */
int picker_key(int n, int selected, int scancode, bool *choose);

/* The entry for last_id (the last game picked), or 0. */
int picker_initial(const picker_entry *e, int n, const char *last_id);

/* Shows the picker for these installed games (2 to PICKER_MAX) and returns
   the one chosen. Remembers it in <files_data_root>/picker.plist ("last
   game") and starts on the one remembered. Cmd-Q or closing the window
   exits the process. For tests: LOONY_PICK=<id|quit>[,...] acts on its
   first item after the first frame and passes the rest on (as
   LOONY_PICK); LOONY_PICKER_SHOT=<png> writes that first frame. */
const game_info *picker_run(const game_info *const *games, int n);
