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

/* The window's title. Sets it at once on an open window. */
void display_set_title(const char *title);

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
} display_input;
void display_set_input(const display_input *in);

/* Handles pending SDL events: keys go to the input callbacks, except Cmd-Q
   (quit), Cmd-F (toggle full screen) and Cmd-V (paste), which the game never
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
