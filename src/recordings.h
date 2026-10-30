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

/* A recording opened from a folder this program can't write in, such as
   Messages' attachments (macOS refuses there whatever the folder's mode
   says, so it is tried): its list of clips, its exported clips and its
   snapshot of the game's start all go beside it, so it is copied into dir
   (the recordings folder) and the copy opened instead. out is path itself
   when its folder takes a new file, else the copy: dir/<its name>, or
   "<its name> 2.lwreplay" and on when another recording has that name. A
   file there already that is the same recording (replay_identity) is
   opened again rather than copied, whatever snapshot it has. The copy
   keeps the time of the file's last write, which says which high score it
   holds. False, with err, if it can't be copied. */
bool recordings_bring_in(const char *path, const char *dir, char *out, size_t cap, char *err,
                         size_t errlen);

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

/* The item under it to practice from the mark (practice.h): "Practice
   from 3:12.50", to the hundredth, since practice starts at the mark
   itself. Returns buf. */
const char *recordings_practice_label(uint32_t ticks, char *buf, size_t cap);

/* Whether r's mark m gets a menu item of its own: not one in the first
   second, which the recording's own item already plays from, nor one in
   the same second as the mark before it, whose item it would repeat. */
bool recordings_mark_shown(const recording *r, size_t m);

/* Sets up the environment for this program to watch the recording at path
   (watch.h) from from_ticks of play when it restarts: LOONY_REPLAY and
   LOONY_WATCH, LOONY_WATCH_FROM unless it is 0, and none of what the
   last game or replay set (its game, data folder, speed and the like). */
void recordings_watch_env(const char *path, uint32_t from_ticks);

/* The same, to practice from from_ticks of play in it (practice.h):
   LOONY_REPLAY and LOONY_PRACTICE, without LOONY_WATCH, and
   LOONY_REPLAY_LICENSE=default (replay.h). */
void recordings_practice_env(const char *path, uint32_t from_ticks);

/* Sets up the environment for this program to leave the recording it is
   watching when it restarts: none of recordings_watch_env's variables, nor
   what replay_open set, so it starts as if launched (main.c). */
void recordings_leave_env(void);

/* Deleting a recording (the Replays menu's Delete). The one being recorded
   (replay_recording_path) can't be, so recordings_deletable(path) is false
   for it. recordings_delete moves path to the Trash with trash (menu.m's
   calls NSFileManager's trashItemAtURL, so a slip can be undone), and its
   list of clips (clip_list_path) after it, but not the clips exported
   from it: they are what it was kept for. A take of its snapshot of the
   game's start that waits is dropped, and one running is killed first
   (gamestart_take_forget), so it doesn't put the file back; if the Trash
   then refuses, the recording keeps no take this session, and its first
   replay takes the snapshot instead. When this program is watching it,
   the viewer closes (display_request_close), saying why. False, with err,
   if path can't be deleted or moved; the list of clips not moving is only
   logged. The next recordings_list reads the folder again. */
typedef bool (*recordings_trash_fn)(const char *path, char *err, size_t errlen);
bool recordings_deletable(const char *path);
bool recordings_delete(const char *path, recordings_trash_fn trash, char *err, size_t errlen);
