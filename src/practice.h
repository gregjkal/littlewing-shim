#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Practice: playing on from a moment of a recorded game, as many times as
   wanted, with nothing kept. LOONY_PRACTICE=<time>, with LOONY_REPLAY,
   replays the recording unseen and silent, as fast as it can, to that
   moment in play time (as the viewer counts it: ticks, or [h:]m:ss), then
   hands the game to the player: the replay goes live (replay_go_live),
   the keys the recording held come up, the window opens and the sound
   starts. From there it is an ordinary game, but nothing of it is kept: it
   isn't recorded, and what it saves, high scores included, goes into the
   replay's own folder (a new temporary one, as for every replay), never
   the player's preferences.

   The first practice from a moment pays the replay there; it keeps a
   snapshot of the moment in a file beside the recording (practice_path,
   gamestart_practice), and each one after goes straight to it, a few
   seconds after launch. One file a recording: a practice from another
   moment replaces it.

   It goes live at the first trap entry at depth 2 inside a handler the run
   loop dispatched to once the replay's clock reaches the moment, where a
   snapshot can be taken (snapshot.h) and the game is between frames. A
   replay that desyncs on the way there fails as any replay does. Not with
   LOONY_WATCH or LOONY_CLIP. */

/* After seek_start and before gamestart_init (its hook runs this one's,
   and puts back this one when it is done): reads LOONY_PRACTICE.
   True if it is unset, or set and valid; false, with err, if it can't be
   used. */
bool practice_init(const char *title, char *err, size_t errlen);

/* The file beside the recording replay that keeps the snapshot of its
   practice moment: its path with .practice in place of .lwreplay (or
   after it, for another name). False if that is too long. */
bool practice_path(const char *replay, char *out, size_t cap);

/* Whether this run is a practice still on its way to the moment: nothing
   is drawn then. */
bool practice_unseen(void);

/* The practices the app ships: a recording and the snapshot of its moment
   each, in practice/ (Contents/Resources/practice in the app, the repo's
   practice/ beside a build's folder otherwise), named in this module's
   table, for the Practice menu. Their recordings are shared copies
   (replay_share), so the player's own license goes in. How many, and the
   menu's name for i. */
size_t practice_bundled_count(void);
const char *practice_bundled_title(size_t i);

/* Copies practice i's files to ~/Library/Application Support/
   littlewing-shim/practice, where a practice can replace its snapshot
   (the app's own files are only read), and gives the copy's path and the
   moment in ticks of play. Each file is copied if the copy is missing or
   older than the app's, so an update's snapshot replaces an old one and
   one the practice made since is kept. False, logged, if it can't be. */
bool practice_bundled_open(size_t i, char *out, size_t cap, uint32_t *ticks);
