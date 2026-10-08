#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MISC_GESTALT_UNDEF_SELECTOR_ERR (-5551)
/* The game sees a Mac that booted a minute before launch: TickCount and
   Microseconds start here, never at 0, which the game uses to mean "not
   scheduled" (BGMKickOff). misc_ticks() and scripts still count from launch. */
#define MISC_BOOT_TICKS 3600u
#define MISC_IC_INSTANCE 0x0FFF0001u /* opaque ICInstance returned by ICStart */

/* Resets the clock (misc_ticks() starts at 0), cursor and Apple Event state.
   With LOONY_FIXED_CLOCK=1 the clock is virtual: it moves only when the game
   waits (Delay, misc_wait), one tick per Delay(0), or by one tick after 200
   time polls in a row (TickCount, Microseconds, misc_poll), so runs don't
   depend on host speed. */
void misc_init(void);

/* Called by ExitToShell before the process exits; it may not return (main
   uses it to go back to the picker). */
void misc_set_exit_hook(void (*fn)(void));

/* What ExitToShell and the C library's exit do: logs why, runs the exit
   hook, and exits with status. */
_Noreturn void misc_exit(const char *why, int status);

/* Seconds since 1970 (UTC). On the virtual clock, the same calendar as
   GetDateTime: 2003-01-01 00:00:00 plus the virtual time. */
int64_t misc_unix_time(void);

/* Sleeps us microseconds the way Delay does: in steps of at most a quarter
   tick on the real clock, or by moving the virtual one, running the idle
   hook at each new tick. A sleep of 0 counts as a time poll. */
void misc_sleep_us(uint64_t us);

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

/* Opens a URL on the host. The default runs /usr/bin/open; tests replace
   it. Returns false if it couldn't. */
typedef bool (*misc_url_fn)(const char *url);
void misc_set_url_opener(misc_url_fn fn);

/* SANE's decimal record (fp.h): the value is sgn, then the digits in sig
   times 10^exp. sig holds "0" for zero, "I" for infinity, "N" for a NaN and
   "?" if the digits don't fit. */
#define MISC_SIGDIGLEN 36
typedef struct {
    bool negative;
    int16_t exp;
    char sig[MISC_SIGDIGLEN + 1];
} misc_decimal;

/* num2dec: style 0 (FLOATDECIMAL) gives digits significant digits (1 to
   MISC_SIGDIGLEN), style 1 (FIXEDDECIMAL) gives digits digits after the
   decimal point. */
void misc_num2dec(int style, int digits, double x, misc_decimal *out);

/* Registers Gestalt, time, string, cursor, Apple Event, Internet Config,
   KeyScript, GetMBarHeight, BlockMoveData, num2dec and ExitToShell imports. */
void misc_register(void);
