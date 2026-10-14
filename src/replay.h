#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Replays (a prototype, tested with Loony Labyrinth): LOONY_RECORD=file logs
   everything from outside the emulator that reaches the game while it is
   played normally, and LOONY_REPLAY=file plays that log back, frame for
   frame, as fast as it can.

   Positions in a run are counted in steps: each place where the host can
   change what the game sees (a clock read, the event pump's input poll, a
   sound check) is one step. The emulator and the shim are deterministic
   between steps, so a replay passes the same steps in the same order. A
   record holds what one step produced, and is written only when that differs
   from the default (the clock changed, a key was pressed, a sound callback
   was due). While recording, the clock the game sees moves in whole
   milliseconds, so most clock reads repeat the last value and log nothing.

   The file is gzip-compressed: a header (magic, format version, the game's
   id and the SHA-256 of its program, whether LOONY_AUTO_ALERTS was on, the
   wall clock and time zone at the start, and the preferences file the run
   started from), then the records. Format 1 had no wall clock or time zone:
   its replays see the host's dates, which no frame depends on. A replay
   starts from those preferences in a folder of its own (LOONY_DATA_DIR, or a
   new temporary folder), and refuses another game or another program.

   Every REPLAY_FRAME_EVERY presents, a hash of the screen is logged; a
   replay compares them and exits with status 3 at the first difference (a
   desync), naming the step and tick. */

#define REPLAY_FRAME_EVERY 30
#define REPLAY_DESYNC_STATUS 3

typedef enum {
    REPLAY_END,            /* the recording stopped */
    REPLAY_CLOCK,          /* ms the clock moved since the last change */
    REPLAY_KEY,            /* scancode, down, repeat */
    REPLAY_MOUSE,          /* x, y, down */
    REPLAY_TEXT,           /* the text pasted or typed (no values) */
    REPLAY_FOCUS,          /* active */
    REPLAY_QUIT,           /* the host asked to quit */
    REPLAY_SOUND_CALLBACK, /* channel, param1, param2: a callback delivered */
    REPLAY_SOUND_BUSY,     /* a channel's status was busy */
    REPLAY_SOUND_FULL,     /* a channel's queue was full */
    REPLAY_FRAME,          /* the screen's hash */
    REPLAY_KINDS
} replay_kind;

#define REPLAY_MAX_VALUES 3
#define REPLAY_MAX_TEXT 1024

bool replay_recording(void);
bool replay_playing(void);

/* On a recording or a replay: the wall clock (Unix seconds) and the time
   zone (seconds east of UTC, daylight saving on or off) when the recording
   started. The calendar the game sees is that plus its own clock, so dates
   replay too. False otherwise, and on a replay of format 1, which has no
   wall clock (it sees the host's). */
bool replay_calendar(int64_t *start_unix, int32_t *gmtoff, bool *dst);

/* Before anything reads the environment: with LOONY_REPLAY set, loads the
   file and sets LOONY_GAME (unless set), LOONY_DATA_DIR (to a new temporary
   folder, unless set) and LOONY_AUTO_ALERTS as they were, and turns off
   LOONY_FIXED_CLOCK. False, with err, if the file can't be read. */
bool replay_open(char *err, size_t errlen);

/* Once the game's program is read and its save folder known (data_dir may
   be NULL), before its preferences load: starts recording (LOONY_RECORD), or
   checks the replay is of this program and writes its preferences into
   data_dir. From here on, steps count. False, with err, if a replay can't
   start. */
bool replay_start(const char *game_id, const uint8_t *exe, size_t exe_len, const char *data_dir,
                  char *err, size_t errlen);

/* Ends a recording (the REPLAY_END record) and closes the file; at exit, a
   replay logs whether it reached the end of its file. Runs at exit; call it
   before exec. */
void replay_finish(void);

/* One step. On a replay, ends the run (status 0) once past the recording's
   last step. */
void replay_step(void);

/* The clock, in us since launch, as the game sees it. Recording: host_us in
   whole ms, logged when it changes. Replay: the logged value (host_us is
   ignored). Each call is a step. */
uint64_t replay_clock(uint64_t host_us);

/* Recording: logs a record at this step. */
void replay_log(replay_kind kind, const uint32_t *values);

/* Recording: logs text at this step, cut at a character to under
   REPLAY_MAX_TEXT bytes. Returns the text to post (utf8, or the cut text),
   so the game gets what a replay will give it. */
const char *replay_log_text(const char *utf8);

/* Replay: takes the next record if it is of this kind and at this step. A
   record left behind at an earlier step is a desync. */
bool replay_take(replay_kind kind, uint32_t *values);
bool replay_take_text(char *out, size_t cap);

/* Call exactly once per present: it counts them. True every
   REPLAY_FRAME_EVERY presents on a recording or a replay, when replay_frame
   should log, or check, the screen's hash. */
bool replay_frame_due(void);
void replay_frame(uint32_t hash);

/* Replay: a desync (or a damaged file). Logs it with the step and tick, and
   exits with REPLAY_DESYNC_STATUS. */
_Noreturn void replay_desync(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* True once replay_desync has run (for what runs at exit). */
bool replay_desynced(void);

/* For tests: forget everything (no file is written). */
void replay_reset(void);
