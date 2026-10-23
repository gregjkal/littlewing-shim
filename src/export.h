#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Exporting from the viewer (watch.h): renders the watched recording's clips
   in a second copy of this program, run with LOONY_CLIP and
   LOONY_CLIP_PROGRESS=1 (clip.h), and follows the lines it writes on stdout
   to say how far it has got. One export runs at a time. The viewer polls it
   from its main loop: nothing here blocks, and the pipe is read in full each
   time, so it never fills.

   The clips are written next to the recording, as LOONY_CLIP writes them
   when run by hand. */

typedef enum { EXPORT_CLIPS, EXPORT_MARKS } export_kind;

/* What an export has said so far. */
typedef struct {
    char partial[1100]; /* the start of a line not yet ended */
    size_t partial_len;
    bool overlong; /* the line being read is longer than partial: dropped */
    enum { EXPORT_STARTING, EXPORT_SEEKING, EXPORT_CLIPPING } phase; /* the last line's */
    uint32_t tick, target;   /* the last seek line's */
    size_t k, n;             /* the last clip line's (n is 0 before one) */
    uint64_t frame, frames;  /* the last clip line's */
    char error[512];         /* the last error line's message, or "" */
    char **wrote, **skipped; /* the files written and skipped, in order */
    size_t nwrote, nskipped;
} export_progress;

/* Takes one line (without its line break) into p. False if it isn't a line
   LOONY_CLIP_PROGRESS writes, which leaves p as it was. */
bool export_line(export_progress *p, const char *line);

/* Takes len bytes the export wrote into p, a line at a time: a line not
   yet ended waits for the rest in the next call. The number of lines that
   weren't the export's (ignored). */
int export_feed(export_progress *p, const char *data, size_t len);

/* Frees what p holds and empties it. */
void export_progress_free(export_progress *p);

/* What p says for the window's title, after the viewer's own: ", exporting
   clip 2 of 3, 40%", ", exporting, finding clip 2 of 3, 15%" while
   replaying to it (", exporting, finding clip, 15%" before the first clip's
   line says how many there are), or ", exporting" before any line. */
void export_title(const export_progress *p, char *buf, size_t cap);

/* The environment for an export of kind of the recording at replay (an
   absolute path), from env (as environ): the viewer's, less its own
   settings (LOONY_WATCH*, LOONY_SCRIPT, LOONY_SCREENSHOT, LOONY_FULLSCREEN,
   its save folder LOONY_DATA_DIR, and others that would change what the
   export does or where it writes), with LOONY_CLIP, LOONY_CLIP_PROGRESS=1,
   LOONY_REPLAY and LOONY_LOG_INHERITED=1 (so the app's export logs where
   the viewer does, rather than starting a new log). NULL-terminated; free
   it with export_free_env. */
char **export_child_env(char *const *env, export_kind kind, const char *replay);
void export_free_env(char **env);

/* The same for any second copy of this program: env less those settings
   and the names set sets, then set ("NAME=value" entries, NULL-terminated).
   Free it with export_free_env. */
char **export_env_with(char *const *env, const char *const *set);

/* Starts an export of kind of the recording at replay (absolute), running
   the program at self. False, with err, if it can't start, or one is
   running. */
bool export_start(const char *self, export_kind kind, const char *replay, char *err, size_t errlen);

/* True from export_start until export_poll says it ended. */
bool export_running(void);

/* The running export's kind. */
export_kind export_running_kind(void);

/* What the running export (or the one that just ended) has said. */
const export_progress *export_now(void);

/* Reads what the export wrote since the last call, and whether it has
   ended, without waiting. True once, when it has: *status is then its exit
   status, or 128 plus the signal that killed it, and export_now holds all
   it said until the next export_start. */
bool export_poll(int *status);

/* Asks the running export to stop (SIGTERM, after which it removes its
   unfinished file and exits with 143). export_poll says when it has. */
void export_cancel(void);

/* Whether export_cancel was called for the running export (or the one that
   just ended). */
bool export_cancelled(void);

/* Stops the running export, if there is one, and waits up to seconds for
   it to end, so nothing outlives the viewer. True if it had ended by then
   (with *status as export_poll gives it). */
bool export_stop(double seconds, int *status);

/* For tests: forgets the export (without stopping it). */
void export_reset(void);
