#pragma once
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

/* The recordings kept in the recordings folder (replay.h), as the Replays
   menu lists them (menu.h). */

typedef struct {
    char path[PATH_MAX];
    char game_id[64];
    time_t start;       /* when the game started: the time in the file's name, else
                           the header's start of the launch */
    int second;         /* which game in that second ("-2" in the name), from 1 */
    uint32_t length;    /* ticks of play (replay_summary's play_ticks) */
    uint32_t *marks;    /* the marks in play ticks, nmarks of them */
    size_t nmarks;
    bool clips;         /* a list of clips is beside it (clip_list_path) */
    bool marked;        /* named "...-marked.lwreplay" */
    int64_t mtime_ns;   /* the file as it was read: a change reads it again */
    off_t size;
} recording;

/* The recordings folder into out: recordings in the data root under HOME,
   or in LOONY_DATA_DIR when that is set (main.c records there). While a
   replay plays, LOONY_DATA_DIR is the replay's own save folder, so the
   folder is the one under HOME. False when neither names one. */
bool recordings_dir(char *out, size_t cap);

/* The readable recordings (*.lwreplay) in dir, newest first: by start,
   then by the game in that second. *list stays valid until the next call
   or recordings_forget. A file that isn't a replay this version reads is
   left out, and logged once. Each file is read once while its size and
   modification time stay the same, so listing again is quick; the one
   being recorded grows, and is read again. A missing folder lists none. */
size_t recordings_list(const char *dir, const recording **list);

/* Drops what recordings_list has read. */
void recordings_forget(void);

/* A recording's menu item into buf: "Oct 15, 8:15 PM, 14:32" (the start,
   with the year when it isn't this one, and the length), then ", 2 marks"
   and ", clips" when it has them. with_game puts the game's title first
   ("Loony Labyrinth, Oct 15, ..."), for a list of more than one game's.
   In English, on the local clock. Returns buf. */
const char *recordings_label(const recording *r, bool with_game, char *buf, size_t cap);

/* A mark's menu item into buf: "Play from 3:12" (whole seconds of play).
   Returns buf. */
const char *recordings_mark_label(uint32_t ticks, char *buf, size_t cap);

/* Sets up the environment for this program to watch the recording at path
   (watch.h) from from_ticks of play when it restarts: LOONY_REPLAY and
   LOONY_WATCH, LOONY_WATCH_FROM unless it is 0, and none of what the
   last game or replay set (its game, data folder, speed and the like). */
void recordings_watch_env(const char *path, uint32_t from_ticks);
