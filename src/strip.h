#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "display.h"

/* The replay viewer's controls: a strip STRIP_HEIGHT points tall, below
   the game in a window and over its bottom edge in full screen, which the
   display draws as its overlay (display.h), never into the emulated
   screen. From the left: play or pause, back and ahead 10 s, slower, the
   speed and faster, the time and the recording's length (watch_get_state),
   and a bar of how far the replay has got. A button does what its key does
   (watch_act), dimmed when watch_can says it can't, and the pointer resting
   on one says its name and key, from watch_bindings.

   The strip comes up when the pointer moves over the window or one of the
   viewer's keys is pressed (strip_poke), and goes STRIP_HIDE_AFTER seconds
   after the last, fading out, but not while the pointer is on it. Down, a
   thin line across the bottom of the window still says how far the replay
   has got, and the pointer hides with the strip. It's up for its first
   STRIP_HIDE_AFTER seconds, so that it's seen. */

#define STRIP_HEIGHT 32       /* points */
#define STRIP_MIN_WIDTH 440   /* points: the narrowest window that holds the controls */
#define STRIP_HIDE_AFTER 2.25 /* seconds */
#define STRIP_FADE 0.2        /* seconds, in or out */
#define STRIP_TIP_AFTER 0.5   /* seconds the pointer rests on a button before its tip shows */

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

/* What a button's tip says: its name and its key, "Back 10 s (J)". */
void strip_tip(strip_button b, char *out, size_t cap);

/* For tests: the overlay strip_install sets. */
const display_overlay *strip_overlay(void);

/* For tests: forget everything, and read the time from now (seconds)
   rather than the wall clock (NULL for the wall clock). */
void strip_reset(double (*now)(void));
