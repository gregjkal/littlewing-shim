#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MISC_GESTALT_UNDEF_SELECTOR_ERR (-5551)
#define MISC_IC_INSTANCE 0x0FFF0001u /* opaque ICInstance returned by ICStart */

/* Resets the clock (TickCount starts at 0), cursor and Apple Event state.
   With LOONY_FIXED_CLOCK=1 the clock is virtual: it moves only when the game
   waits (Delay, misc_wait), one tick per Delay(0), or by one tick after 200
   time polls in a row (TickCount, Microseconds, misc_poll), so runs don't
   depend on host speed. */
void misc_init(void);

/* True if the virtual clock is in use. */
bool misc_fixed_clock(void);

/* Waits: sleeps on the real clock, or advances the virtual one. */
void misc_wait(double seconds);

/* Notes that the game polled for time or events without waiting (see
   LOONY_FIXED_CLOCK). */
void misc_poll(void);

/* Ticks (1/60 s) since misc_init(). */
uint32_t misc_ticks(void);

/* Seconds since misc_init(), with microsecond resolution. */
double misc_seconds(void);

/* False while HideCursor has hidden the cursor (until InitCursor). */
bool misc_cursor_visible(void);

/* The handler AEInstallEventHandler recorded for (event class, event ID).
   Returns false if there is none. */
bool misc_ae_handler(uint32_t event_class, uint32_t event_id, uint32_t *handler,
                     uint32_t *refcon);

/* Called by Delay, and by TickCount whenever the tick count has changed,
   so a game waiting in its own loop still pumps events, sound and the
   screen. */
typedef void (*misc_idle_fn)(void);
void misc_set_idle(misc_idle_fn fn);

/* Registers Gestalt, time, string, cursor, Apple Event, Internet Config,
   KeyScript, GetMBarHeight, BlockMoveData and ExitToShell imports. */
void misc_register(void);
