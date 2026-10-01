#pragma once
#include <stdbool.h>

/* Shows the emulated screen (qd_screen) in an SDL window, scaled to fit with
   the right aspect ratio and nearest-neighbor sampling. The window is created
   on the first present. With SDL_VIDEO_DRIVER=dummy nothing appears on
   screen, which the tests use. */

/* If LOONY_SCREENSHOT names a file, the screen is written there as a PNG when
   the process exits, including after a crash. */
void display_init(void);

/* Draws the current screen. Also handles window events: closing the window
   exits cleanly. */
void display_present(void);

/* Writes the current screen as a PNG. */
bool display_write_png(const char *path);

/* Presents only if something drew to the screen since the last present. */
void display_present_if_dirty(void);

/* Number of presents so far. */
unsigned display_frames(void);
