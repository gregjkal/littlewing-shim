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
   snapshot of a second before the moment (below) in a file beside the
   recording (practice_path, gamestart_practice), and each one after goes
   straight to it, a second or so after launch. One file a recording: a practice from another
   moment replaces it.

   The snapshot is of a second before the moment, so a replay put there
   from it is checked against the recording's frames before it goes live,
   and one that no longer fits desyncs and is dropped (gamestart_drop)
   rather than played on wrong. Both are at the first trap entry at depth
   2 inside a handler the run loop dispatched to once the replay's clock
   reaches them, where a snapshot can be taken (snapshot.h) and the game
   is between frames. The window opens at the first frame and shows how
   far along the replay is in its title, and takes a quit; nothing else
   is drawn until the moment. A replay that desyncs on the way there fails
   as any replay does. Not with
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
   is drawn then but the first frame, which opens the window. */
bool practice_unseen(void);

/* The practices the app ships: a recording and the snapshot of its moment
   each, in practice/ (Contents/Resources/practice in the app, the repo's
   practice/ beside a build's folder otherwise), named in this module's
   table, for the Practice menu. Their recordings are shared copies
   (replay_share), with no license in them, and a practice puts in the one
   every game starts with (recordings_practice_env), so the snapshot fits
   every player. A change to what snapshots hold (gamestart.h) needs each
   shipped snapshot taken again, which a test checks: a headless practice
   of practice/<name>.lwreplay from its moment, with
   LOONY_REPLAY_LICENSE=default and an empty LOONY_DATA_DIR, writes
   practice/<name>.practice. How many, and the menu's name for i. */
size_t practice_bundled_count(void);
const char *practice_bundled_title(size_t i);

/* Copies practice i's files to ~/Library/Application Support/
   littlewing-shim/practice, where a practice can replace its snapshot
   (the app's own files are only read), and gives the copy's path and the
   moment in ticks of play. Each file is copied if the copy is missing or
   older than the app's, so an update's snapshot replaces an old one and
   one the practice made since is kept. False, logged, if it can't be. */
bool practice_bundled_open(size_t i, char *out, size_t cap, uint32_t *ticks);
