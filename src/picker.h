#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "game.h"

/* The game picker LittleWing.app shows when more than one game is
   installed: each game's thumbnail from LittleWing's download page
   (picker-art/<game id>.png, bundled in the app's Resources, 288x288 so it
   fills a card) on a square card, or, for a game without one, one whose
   thumbnail can't be read, or a loony run outside the app, its title
   picture (PICT 800 in its own resource fork, read from the user's copy);
   its name under it, and the selection marked in gold. A bundle game
   (MONSTER FAIR, Mad Daedalus, Golden Logres, Angel Egg, FAIRY TOWER) has
   no title picture the shim reads: its card shows its icon centered,
   appl.png or, without one, the 128x128 image in app.icns. Neither does a
   classic game without PICT 800 (Jinni Zeala), whose card shows the
   128x128 icon in its resource fork's icns 128, and Tristan, whose PICT
   800 is its credits and which has no icns, shows its title, PICT 900; a
   title picture is reduced to fit the card and centered on it. Each card
   is a square centered in a 4:3 slot as tall as it. Two games get two
   large slots side by side (384x288); three get three smaller ones
   (240x180) in a row; four get two rows of two (224x168), five and six two
   rows of three, seven a row of four over three and eight two rows of four
   (176x132), and nine three rows of three (160x120, the columns 272 pixels
   apart, the heading and the hint nearer the screen's top and bottom),
   and ten rows of four, four and two (144x108, the columns 200 pixels
   apart, the heading and the hint as nine's), each row centered, the art
   reduced to fit. A name wider than its column
   (slot and gap), with seven or eight wider than its slot, or with ten
   closer than 24 pixels to its neighbour's, breaks onto two lines at a
   space; with nine, every name fits its column on one line, 24 pixels or
   more from its neighbour's. This file draws and reads input;
   picker_run (Task 5) shows it.
   Pixels here are QuickDraw 32-bit: big-endian xRGB. */

#define PICKER_W 800
#define PICKER_H 600
#define PICKER_ART_W 288 /* square, as the download page's thumbnails are */
#define PICKER_ART_H 288
#define PICKER_MAX 10    /* the layouts fit two to ten games */

typedef struct {
    const game_info *game;
    uint8_t *art; /* PICKER_ART_W x PICKER_ART_H, or NULL for a plain card */
} picker_entry;

/* Reads the program's resource fork (exe_path/..namedfork/rsrc), draws
   its PICT 800 and reduces it to fit art, centered; a fork without a
   512x384 PICT 800 but with an icns 128 gets that icon's card instead, and one with neither
   (Tristan) its title, PICT 900, reduced to fit and centered. False, with
   err naming the file, if the fork or the picture can't be read, it has
   none of the three, or the icon has no 128x128 image.
   Opens and closes the Resource Manager's fork, so call it before the
   game's own is opened. */
bool picker_load_art(const char *exe_path, uint8_t *art, char *err, size_t errlen);

/* A card from an icns resource (len bytes at icns): its 128x128 image
   (it32, with the t8mk mask) blended over the plain card color and
   centered, into art. False if either is missing or malformed. */
bool picker_load_icns(const uint8_t *icns, size_t len, uint8_t *art);

/* A card from a PNG (a bundled thumbnail, or a bundle game's appl.png):
   the PNG at png_path (at most PICKER_ART_W x PICKER_ART_H), centered on
   the plain card color, into art. False, with err saying why, if it can't
   be read or is too large. */
bool picker_load_icon(const char *png_path, uint8_t *art, char *err, size_t errlen);

/* A bundle game's card from its .app at app: Contents/Resources/appl.png
   if there is one (picker_load_icon), otherwise the 128x128 image in
   Contents/Resources/app.icns (picker_load_icns). False, with err saying
   why, if neither gives a card. */
bool picker_load_bundle_art(const char *app, uint8_t *art, char *err, size_t errlen);

/* When exe_path is inside an app bundle (<name>.app/Contents/MacOS/...) that
   has Contents/Resources/picker-art/<game_id>.png, writes that file's path
   to out and returns true. */
bool picker_bundled_art(const char *exe_path, const char *game_id, char *out, size_t cap);

/* Draws the whole picker into screen (PICKER_W x PICKER_H) with entry
   `selected` marked. n is at most PICKER_MAX. */
void picker_draw(const picker_entry *e, int n, int selected, uint8_t *screen);

/* The entry whose card (with its border and name) holds the point, or -1. */
int picker_hit(int n, int x, int y);

/* The selection after an SDL scancode: Left and Right move (stopping at
   the ends), Up and Down move between rows (four or five cards; Down from
   a card with none below goes to the last card), Return and keypad Enter
   set *choose. Other keys change nothing. */
int picker_key(int n, int selected, int scancode, bool *choose);

/* The entry for last_id (the last game picked), or 0. */
int picker_initial(const picker_entry *e, int n, const char *last_id);

/* Shows the picker for these installed games (2 to PICKER_MAX) and returns
   the one chosen. Remembers it in <files_data_root>/picker.plist ("last
   game") and starts on the one remembered. The selected card plays its
   game's opening music (theme.h). Cmd-Q or closing the window
   exits the process. For tests: LOONY_PICK=<id|quit>[,...] acts on its
   first item after the first frame and passes the rest on (as
   LOONY_PICK); LOONY_PICKER_SHOT=<png> writes that first frame. */
const game_info *picker_run(const game_info *const *games, int n);
