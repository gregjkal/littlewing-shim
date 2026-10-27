#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Shows the emulated screen (qd_screen) in an SDL window, scaled to fit with
   the right aspect ratio and nearest-neighbor sampling. The window is created
   on the first present. With SDL_VIDEO_DRIVER=dummy nothing appears on
   screen, which the tests use.

   An overlay (the replay viewer's controls, strip.h) can draw in the window
   too, in the window's pixels and never into the emulated screen, so frame
   hashes, screenshots and exported clips leave it out. In a window it gets
   a strip of its own below the game; in full screen it goes over the
   game's bottom edge. */

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

/* Shows msg for DISPLAY_FLASH_MS: on the overlay when there is one (its
   say), else after the title, on an open window. A window shows
   LOONY_FLASH this way as it opens, for a message that goes across a
   restart (main.c), and unsets it. */
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

/* Whether a present now would wait for the display only briefly: three
   quarters of a refresh or more have passed since the last one. A present
   sooner waits for most of a refresh, and a game that draws a frame in
   pieces, pumping after each (Angel Egg), would wait once a piece. Always
   true without vsync or a window. */
bool display_due(void);

/* A rectangle in the window's pixels. */
typedef struct {
    float x, y, w, h;
} display_rect;

/* Where things go in a window w x h pixels. The game, a screen lw x lh, is
   letterboxed: scaled to fit with its aspect kept, and centered, as SDL's
   logical presentation does it. The strip is strip_h pixels across the
   bottom of the window: in a window the game fits above it, in full screen
   it goes over the game. With strip_h 0 the game has the whole window.
   density is left at 1 for the caller to set. */
typedef struct {
    display_rect game, strip;
    bool fullscreen;
    float density; /* pixels a point */
} display_layout;
display_layout display_lay_out(float w, float h, int lw, int lh, float strip_h, bool fullscreen);

/* What the display draws over the game, and the pointer events on it.
   Coordinates are in the window's pixels. */
typedef struct {
    float height;    /* the strip, in points */
    float min_width; /* the narrowest window that holds it, in points */
    /* Draws into renderer (an SDL_Renderer), after the game. */
    void (*draw)(void *renderer, const display_layout *l);
    /* Whether draw would draw something new: the display then draws again
       by itself (at most once a frame), paused or not. */
    bool (*changed)(void);
    /* How far the strip is up, 0 to 1. The pointer shows only while it's
       up, and in full screen a press on its place is the game's while
       it's down. */
    float (*shown)(void);
    /* The pointer moved to (x, y), or left the window (inside false). */
    void (*pointer)(const display_layout *l, float x, float y, bool inside);
    /* The left button went down or up at (x, y), on the strip, or up
       anywhere after going down on it. */
    void (*press)(const display_layout *l, float x, float y, bool down);
    /* Shows msg for DISPLAY_FLASH_MS (display_flash), in place of the
       window's title. */
    void (*say)(const char *msg);
} display_overlay;

/* Sets the overlay, or none (NULL). Before the window opens, so that the
   window makes room for it: its default size grows by the strip, and it
   gets a minimum size. */
void display_set_overlay(const display_overlay *o);

/* For tests: draws the window as the next present would show it, and reads
   it back: malloc'd RGBA, *w x *h, or NULL with no window. */
uint8_t *display_read_window(int *w, int *h);

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
   said as display_flash says things), which the game never
   sees. Left-button clicks are mapped from the window to the emulated
   screen, through the letterboxing and scaling, unless they are on the
   overlay's strip, which gets them and the pointer's movements. A press on
   the strip is a drag: the strip gets its release wherever it comes, and
   one where the pointer last was when the window loses focus first or the
   button goes down again without one. With an
   overlay, the window is drawn again whenever it changes. */
void display_poll(void);

/* Shows or hides the mouse pointer over the window. With an overlay, the
   overlay decides instead (display_overlay's shown). */
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
