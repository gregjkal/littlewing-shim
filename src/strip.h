#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "display.h"
#include "watch.h"

/* The replay viewer's controls: a strip STRIP_HEIGHT points tall, below
   the game in a window and over its bottom edge in full screen, which the
   display draws as its overlay (display.h), never into the emulated
   screen. From the left: play or pause, back and ahead 10 s, slower, the
   speed and faster, the time and the recording's length (watch_get_state),
   and a bar of how far the replay has got. A button does what its key does
   (watch_act), dimmed when watch_can says it can't, and the pointer resting
   on one says its name and key, from watch_bindings.

   The bar is the recording's timeline: the marks (Cmd-S while recording)
   are diamonds on it, the saved clips tinted spans, and a clip's start not
   yet ended (I) a bracket, with the span from it to now tinted lighter. The
   pointer over the bar says the time under it at once, or a mark's number
   and time, or a clip's number and span when it's on the mark or the
   clip's start; a click goes there: to a mark or a clip's start exactly,
   and elsewhere to the time under the pointer.

   A press on the bar is a drag (watch_scrub_start and _end): it pauses,
   and until the release only the caret and its tip move, along the bar
   wherever the pointer goes, snapping to marks and clips' starts as a
   click does. The release goes there, and goes on playing if the press
   paused. While the replay is on its way to a time, the bar shows where
   playback is, and a caret where it's going.

   The speed says the speed the replay reaches (watch_state's reached)
   when it falls short of the one asked, in amber, and at "max" once it's
   known; it lights up for a moment when the speed changes. Pausing or
   going on shows a pause or play sign over the middle of the game, which
   fades in STRIP_SIGN seconds. What the viewer has to say (display_flash,
   through the overlay's say) is shown above the strip, at its left, on
   two rows if it takes them, for DISPLAY_FLASH_MS, with the strip up all
   the while; a tip goes above it.

   The strip comes up when the pointer moves over the window or one of the
   viewer's keys is pressed (strip_poke), and goes STRIP_HIDE_AFTER seconds
   after the last, fading out, but not while the pointer is on it or a
   message is shown. Down, a thin line across the bottom of the window
   still says how far the replay has got, and the pointer hides with the
   strip. It's up for its first STRIP_HIDE_AFTER seconds, so that it's
   seen. */

#define STRIP_HEIGHT 32       /* points */
#define STRIP_MIN_WIDTH 440   /* points: the narrowest window that holds the controls */
#define STRIP_HIDE_AFTER 2.25 /* seconds */
#define STRIP_FADE 0.2        /* seconds, in or out */
#define STRIP_TIP_AFTER 0.5   /* seconds the pointer rests on a button before its tip shows */
#define STRIP_SIGN 0.6        /* seconds the pause or play sign takes to fade */
#define STRIP_SPEED_LIT 0.6   /* seconds the speed stays lit after it changes, fading */
/* Below this much of the speed asked, the speed reached is shown. */
#define STRIP_SHORT_OF 0.9

/* Makes the strip the display's overlay. Before the window opens. */
void strip_install(void);

/* One of the viewer's keys: the strip comes up. */
void strip_poke(void);

typedef enum {
    STRIP_NONE = -1,
    STRIP_PLAY,
    STRIP_BACK,
    STRIP_AHEAD,
    STRIP_SLOWER,
    STRIP_FASTER,
    STRIP_NBUTTONS,
} strip_button;

/* The button at (x, y) in the window's pixels, or STRIP_NONE. */
strip_button strip_button_at(const display_layout *l, float x, float y);

/* What's on the bar at a point: nothing (off the bar), a time, a mark
   (index in watch_state's marks), or a clip's start (index in its clips).
   tick is where a click there goes. */
typedef enum { STRIP_ON_NOTHING, STRIP_ON_TIME, STRIP_ON_MARK, STRIP_ON_CLIP } strip_on;
typedef struct {
    strip_on on;
    int index;
    uint32_t tick;
} strip_spot;

/* The spot at (x, y) in the window's pixels: a mark or a clip's start
   within STRIP_SNAP points of x, the nearest, marks first; else the time
   under x, on the bar or the strip's height above and below it. */
#define STRIP_SNAP 5 /* points */
strip_spot strip_spot_at(const display_layout *l, const watch_state *w, float x, float y);

/* What the bar's tip says at a spot: "9:40", "Mark 2: 3:12" or
   "Clip 1: 2:58-3:20", in whole seconds. */
void strip_spot_tip(const strip_spot *s, const watch_state *w, char *out, size_t cap);

/* For tests: the bar, in the window's pixels. */
display_rect strip_bar(const display_layout *l, const watch_state *w);

/* What the speed says: "2x", "max", or the speed reached (w->reached)
   when it's below STRIP_SHORT_OF of the speed asked, or at max once it's
   known: "5.2x", "18x". True when that is short of the speed asked. */
bool strip_speed_text(const watch_state *w, char *out, size_t cap);

/* A message on rows of fits characters: all of text in a, or its words
   that fit in a and the rest in b, cut with "..." if it doesn't fit
   either. The rows: 1 or 2. */
int strip_wrap(const char *text, size_t fits, char *a, char *b, size_t cap);

/* text as the strip's font can draw it, which is ASCII only: curly quotes
   straight, an ellipsis "...", a dash "-", a control character a space,
   anything else "?". A replacement goes in whole or not at all. */
void strip_ascii(const char *text, char *out, size_t cap);

/* What a button's tip says: its name and its key, "Back 10 s (J)". */
void strip_tip(strip_button b, char *out, size_t cap);

/* For tests: the overlay strip_install sets. */
const display_overlay *strip_overlay(void);

/* For tests: forget everything, and read the time from now (seconds)
   rather than the wall clock (NULL for the wall clock). */
void strip_reset(double (*now)(void));
