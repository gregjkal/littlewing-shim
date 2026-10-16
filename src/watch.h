#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Watching a replay (a prototype): with LOONY_REPLAY, LOONY_WATCH=1 shows it
   in the window at the speed it was played, with its sound, to find a
   moment in a long game and mark a clip of it. The host takes the keys (a
   replay's game never sees the host's input):

     Space, K      pause, or go on
     Right, Left   faster or slower: 1x, 2x, 4x, 8x, as fast as it can
     I, O          the start and end of a clip; O saves it
     J, L          back or ahead 10 seconds (Shift: a minute)
     E             export the clips (LOONY_CLIP=clips, export.h)
     Esc           cancel the export

   The window's title says the game's time and the speed. Only 1x has sound.

   Exporting runs in a second copy of this program while the viewer goes on,
   one export at a time, and the title says how far it has got. Once it is
   done, Finder shows the clips it wrote (or, when every clip was there
   already, those), next to the recording. The Replay menu (menu.h) exports
   the clips or the marks, and cancels. Going back takes the export through
   the restart (LOONY_WATCH_EXPORT and LOONY_WATCH_EXPORT_SAID, export.h);
   quitting stops it, which removes its unfinished file.

   Pacing: the replay's clock never runs ahead of the wall clock times the
   speed. When it falls more than WATCH_MAX_LAG behind (HD at 8x, or the
   window held still), it carries on from there rather than racing to catch
   up. At 1x, each minute of the game's time logs how long it took on the
   wall clock.

   Clips go into the list next to the replay (clip_list_path: game.clips
   beside game.lwreplay), one "<start>-<end>" a line as LOONY_CLIP takes
   them, and LOONY_CLIP=clips renders them (clip.h).

   A replay only runs forward: going back restarts the program, which
   replays from the start without pacing to the time wanted, and going ahead
   does the same without the restart. LOONY_WATCH_FROM (ticks) is that time,
   LOONY_WATCH_SPEED (1, 2, 4, 8 or 0 for as fast as it can) the speed,
   LOONY_WATCH_PAUSED=1 pauses there, and LOONY_WATCH_IN (ticks) is a clip's
   start not yet ended; the restart sets them, and drops LOONY_SCRIPT, whose
   actions would come again. At the
   replay's end, or when the game quits, the last frame stays up until the
   window is closed (unless SDL_VIDEO_DRIVER=dummy, where it exits once an
   export has finished). */

#define WATCH_MAX_LAG 0.1 /* seconds */
#define WATCH_STEP_TICKS 600        /* J and L: 10 s */
#define WATCH_BIG_STEP_TICKS 3600   /* with Shift: a minute */

/* After replay_open and clip_open: reads LOONY_WATCH and the settings a
   restart passes on. False, with err, if they're wrong, or LOONY_WATCH is
   set without a replay or with LOONY_CLIP. */
bool watch_open(char *err, size_t errlen);

/* True when a replay is being watched. */
bool watch_on(void);

/* Once the window and sound are set up: starts watching, with the game's
   title for the window's. restart, called to go back once the environment
   for it is set, restarts the program; it doesn't return. */
void watch_start(const char *title, void (*restart)(void));

/* A key from the host, which the game doesn't see (events_host_key on a
   replay). */
void watch_key(int scancode, bool down, bool repeat);

/* Exports the watched recording's clips (or its marks, if marks), unless
   an export is running already, or there are no clips: the title says why
   not. */
void watch_export(bool marks);

/* Cancels the running export, if there is one. */
void watch_cancel_export(void);

/* Whether an export is running. */
bool watch_exporting(void);

/* Stops the running export, if there is one, and waits a moment for it to
   end: before the program quits or restarts for something else. */
void watch_stop_export(void);

/* The event pump's present, in place of the window's: dirty if the screen
   changed since the last one. Draws at most once a frame of wall clock,
   and a few times a second while going to a time. */
void watch_present(bool dirty);

/* The replay reached its end, or the game quit: keeps the window up until
   it is closed, or a key goes back. Returns at once unless watching, and
   when SDL_VIDEO_DRIVER=dummy once no export is running. */
void watch_hold(void);

/* For tests: forget everything. */
void watch_reset(void);

/* For tests: whether watch_present draws a changed screen, since seconds
   after the last draw, lag seconds behind the wall clock (0 when paused),
   at speed (1 to 8, 0 as fast as it can), going to a time or not. */
bool watch_draws(double since, double lag, int speed, bool going);
