#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Shows the emulated screen (qd_screen) in an SDL window, scaled to fit with
   the right aspect ratio and nearest-neighbor sampling. The window is created
   on the first present. With SDL_VIDEO_DRIVER=dummy nothing appears on
   screen, which the tests use. */

/* If LOONY_SCREENSHOT names a file, the screen is written there as a PNG when
   the process exits, including after a crash. */
void display_init(void);

#define DISPLAY_FLASH_MS 2500

/* The window's title. Sets it at once on an open window (after a message
   display_poll shows there). */
void display_set_title(const char *title);

/* The window comes to the front, with the app activated, when it opens
   (watch.h: after a restart, it would otherwise open behind the others). */
void display_raise_on_open(void);

/* Shows msg after the title for DISPLAY_FLASH_MS, on an open window. A
   window shows LOONY_FLASH this way as it opens, for a message that goes
   across a restart (main.c), and unsets it. */
void display_flash(const char *msg);

/* Draws the current screen. */
void display_present(void);

/* Draws an RGBA frame (w x h, rows top to bottom), opening the window on the first one. */
void display_present_rgba(const uint8_t *rgba, int w, int h);

/* Whether the window is full screen. LOONY_FULLSCREEN=1 opens it full screen. */
bool display_fullscreen(void);

/* Whether presenting waits for the display's refresh (the default). Turned
   off for fixed-clock runs, where waiting would only slow the run down. */
void display_set_vsync(bool on);

/* Where display_poll sends host input. */
typedef struct {
    void (*key)(int scancode, bool down, bool repeat); /* SDL scancode */
    void (*focus)(bool active);
    void (*quit)(void); /* window closed or Cmd-Q */
    void (*mouse)(int x, int y, bool down); /* left button, emulated screen coordinates */
    void (*paste)(const char *utf8);        /* Cmd-V: the clipboard's text */
    int (*mark)(void); /* Cmd-S: marks a clip; its number, or 0 if nothing is recorded */
} display_input;
void display_set_input(const display_input *in);

/* Where display_poll sends a file opened from Finder (one of the app's
   documents), dropped on the window or chosen from a menu: fn gets its
   path and where to start watching it, in ticks of play (0 for the
   start). Apart from display_set_input, so it holds in the picker too. */
void display_set_open(void (*fn)(const char *path, uint32_t from_ticks));

/* Asks for path to be opened from from_ticks once display_poll has handled
   the events it is handling, rather than at once: a menu item's action
   runs inside AppKit's handling of an event, where the restart that opens
   a replay must not happen. A later request before then replaces an
   earlier one, unless the earlier one is a replay (.lwreplay): of several
   files dropped at once, the first replay opens. Drops come here too. */
void display_request_open(const char *path, uint32_t from_ticks);

/* Where display_poll sends a request to close the replay being watched
   (recordings_delete): fn gets why, to say once the program has restarted
   (display_flash's LOONY_FLASH). */
void display_set_close(void (*fn)(const char *why));

/* Asks for the replay being watched to be closed, saying why, once
   display_poll has handled the events it is handling, as
   display_request_open does. It goes before any file asked to be opened. */
void display_request_close(const char *why);

/* At launch, before anything is shown: starts SDL's video and handles the
   events macOS sends within wait_ms, so a file opened from Finder to launch
   the app reaches display_set_open's fn before the game starts. Other
   events wait for display_poll. Does nothing with a video driver other than
   Cocoa. */
void display_take_launch_events(uint32_t wait_ms);

/* Handles pending SDL events: keys go to the input callbacks, except Cmd-Q
   (quit), Cmd-F (toggle full screen), Cmd-V (paste) and Cmd-S (mark a clip,
   said in the window's title for DISPLAY_FLASH_MS), which the game never
   sees. Left-button clicks are mapped from the window to the emulated
   screen, through the letterboxing and scaling. */
void display_poll(void);

/* Shows or hides the mouse pointer over the window. */
void display_set_cursor(bool visible);

/* Writes the current screen as a PNG. */
bool display_write_png(const char *path);

/* Presents only if something drew to the screen since the last present. */
void display_present_if_dirty(void);

/* Number of presents so far. */
unsigned display_frames(void);

/* Logs what presenting has cost: the presents that reached the window, the
   bytes uploaded to the texture per present on average, and the average and
   longest time spent uploading, and uploading plus drawing and presenting.
   Runs at exit (registered on the first present) and before going back to
   the picker; logs once, and only if something was presented. */
void display_log_stats(void);
