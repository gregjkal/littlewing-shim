#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

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
   started from), then the records. Formats 5 and 6 are read: format 5
   logs where a game starts and ends and where it is played (play.h), and
   format 6, which this version writes, is format 5 that may also hold a
   snapshot of the game's start after its records (gamestart.h). A shared
   copy is either with a flag. The prototype's formats 1 to 4 are refused.
   A replay starts from those preferences in a folder of its own
   (LOONY_DATA_DIR, or a new temporary folder), and refuses another game or
   another program.

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
    REPLAY_MARK,           /* the player marked a moment to clip */
    REPLAY_GAME,           /* started: a game starts (1) or ends (0) (play.h) */
    REPLAY_PLAY,           /* on: the game is played from here (1), or not (0) */
    REPLAY_KINDS
} replay_kind;

#define REPLAY_MAX_VALUES 3
#define REPLAY_MAX_TEXT 1024

bool replay_recording(void);
bool replay_playing(void);

/* On a recording or a replay: the wall clock (Unix seconds) and the time
   zone (seconds east of UTC, daylight saving on or off) when the recording
   started. The calendar the game sees is that plus its own clock, so dates
   replay too. False otherwise. */
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

/* The app records every game by itself (Loony Labyrinth only, for now),
   and keeps the last REPLAY_KEEP_GAMES games of each, every game with a
   mark, and every game that set a high score still in the table. */
#define REPLAY_KEEP_GAMES 3

/* Before replay_start: unless LOONY_RECORD is set (LOONY_RECORD=0 turns
   recording off), a replay is playing or LOONY_FIXED_CLOCK=1, each game
   played (replay_game) is recorded into dir as
   <game_id>-<date>-<time>.lwreplay, named for when it starts and renamed
   <...>-marked.lwreplay at its first mark. A replay can only start at
   launch, so the file holds what a replay needs of the time before the game
   (but not the marks, games and play there), then the game until it ends.
   Nothing is written outside a game. The game's recordings there beyond the
   last REPLAY_KEEP_GAMES are deleted first, except the marked ones, those
   with a list of clips (<...>.clips, from watching them; watch.h) and those
   that were recording when one of the high_scores (n times, from
   game_high_score_times) was set: from the time in a recording's name to
   its last write, give or take REPLAY_SCORE_SLACK seconds. So a recording
   goes once its score has left the table; the games of this run stay until
   the next. */
#define REPLAY_SCORE_SLACK 2
void replay_record_into(const char *dir, const char *game_id, const time_t *high_scores, size_t n);

/* The kept recording in dir of the game that set a high score at when (from
   the score's timestamp): the newest whose span holds it, as
   replay_record_into keeps them. Its path goes to out. False if none does. */
bool replay_find_recording(const char *dir, const char *game_id, time_t when, char *out,
                           size_t cap);

/* Writes a copy of the recording src to dst (replacing it) that can be
   given away: the license (e-mail address and key code) is emptied in its
   preferences, and the copy is marked shared. A replay of a
   shared copy puts in the license from LOONY_REPLAY_LICENSE's preferences:
   by default, where the viewer's own copy of the game saves them (set by
   replay_open, so a restart keeps it); without a license there it refuses.
   The game only checks the license, and never shows it, so the frames are
   the same. Everything else is shared as recorded: the high-score names,
   and anything typed while playing. A recording still being written copies
   to its last frame check. The copy keeps src's snapshot of the game's
   start (replay_snapshot), which fits it too (replay_identity), so its
   replay starts at the game on the recipient's Mac (gamestart.h); one
   whose bytes hold the license as text is left out. False, with err, if
   src can't be read, was played unregistered (with the viewer's license
   its replay would start differently), or dst can't be written. */
bool replay_share(const char *src, const char *dst, char *err, size_t errlen);

/* After replay_open: the snapshot section of the file being replayed
   (gamestart.h), its bytes as replay_set_snapshot wrote them, or NULL. */
const uint8_t *replay_snapshot(size_t *len);

/* After replay_open: a SHA-256 of all of the file a replay sees, so a
   snapshot is put back only into a replay of the recording it was taken
   from. A recording and its shared copy have the same: their headers
   differ only in the license and the shared flag, which aren't in it. */
void replay_identity(uint8_t out[32]);

/* The snapshot section of the recording at path (malloc'd, or NULL for
   none) and the recording's identity (replay_identity), without touching
   the replay being played or recorded. False, with err, if it isn't a
   replay this version reads. */
bool replay_file_snapshot(const char *path, uint8_t **section, size_t *len,
                          uint8_t identity[32], char *err, size_t errlen);

/* Replay: writes the file being replayed again with section as its
   snapshot section, in its place (in format 6), keeping the file's mode
   and the time of its last write (which says which high score it holds).
   False, logged, when the recording has no end record yet, when section
   holds the license as text (the recording's, or the one this replay gave
   the game, in any case), or when the file can't be written. That scan of
   the section's bytes is only a backstop: it can't see into the guest
   pages, which the section holds XORed with the baseline's. What keeps the
   license out of those is the scan of the snapshot's changed pages at
   capture (snapshot_changed_holds, gamestart.h). */
bool replay_set_snapshot(const uint8_t *section, size_t len);

/* Before the app restarts to watch path, so a file it can't play doesn't
   end the game being played: whether path is a replay of a game it knows,
   in a format it reads, and, if shared, whether the viewer's own
   preferences (under HOME) hold a license. False, with err saying why
   ("it was shared without a license: ..."). The game's program is
   checked once the replay starts. */
bool replay_check(const char *path, char *err, size_t errlen);

/* What a list of recordings shows of one (recordings.h). */
typedef struct {
    char game_id[64];
    int64_t start_unix; /* the header's: when the launch that recorded it started */
    int32_t gmtoff;     /* and its time zone, in seconds east of UTC */
    uint32_t play_ticks; /* its length in play time (replay_play_ticks): to its
                            last record when it ends in play */
    uint32_t *marks;     /* its marks in play ticks, as the viewer counts them
                            (LOONY_WATCH_FROM); malloc'd */
    size_t nmarks;
} replay_summary;

/* Reads the recording at path into s, decompressing all of it, without
   touching the replay being played or recorded. A file still being
   written reads to where it was last flushed. False, with err saying why,
   when it isn't a replay this version reads. Free s with
   replay_summary_free. */
bool replay_summarize(const char *path, replay_summary *s, char *err, size_t errlen);
void replay_summary_free(replay_summary *s);

/* Replay: fn is called with the clock, in us, each time it is about to move,
   before the game sees it (watch.h paces the replay there). */
void replay_set_pace(void (*fn)(uint64_t clock_us));

/* Replay: fn is called where the recording stopped, before the program
   exits there. */
void replay_set_end(void (*fn)(void));

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

/* The clock, in us, as replay_clock last returned it. Not a step. */
uint64_t replay_now_us(void);

/* Recording: logs a record at this step. */
void replay_log(replay_kind kind, const uint32_t *values);

/* Recording: logs text at this step, cut at a character to under
   REPLAY_MAX_TEXT bytes. Returns the text to post (utf8, or the cut text),
   so the game gets what a replay will give it. */
const char *replay_log_text(const char *utf8);

/* Marks: while recording, Cmd-S marks the moment for a clip (clip.h). A
   replay only logs them. Recording: logs a mark at this step and returns its
   number in the file, from 1; 0 when nothing is recorded (outside a game
   when each game has a file, and on a replay). */
int replay_mark(void);

/* Recording: a game starts (true) or ends, and play goes on or off (play.h
   says when). */
void replay_game(bool started);
void replay_play(bool on);

/* Recording each game into a file of its own (replay_record_into), when
   games can't be told apart (play_init found another program): nothing
   would ever be written, so recording stops, and says so. */
void replay_no_games(void);

/* Replay: takes a mark at this step and logs it. */
bool replay_take_mark(void);

/* Replay: takes a game or play record at this step (a replay only logs a
   game starting or ending; it knows where play is from replay_play_spans). */
bool replay_take_play(void);

/* After replay_open: the ticks of the replay's marks, in order, from a scan
   of the whole file. */
size_t replay_marks(const uint32_t **ticks);

/* After replay_open, from the same scan: where the game is played, as
   spans of the clock in us, from where play goes on to where it goes off
   (UINT64_MAX when the recording stops in play). None in a recording where
   no game was played: all of it is play then. */
typedef struct {
    uint64_t on_us, off_us;
} replay_span;
size_t replay_play_spans(const replay_span **spans);

/* Play time: the clock less what isn't play, from the start of the first
   span. replay_play_us gives it at clock_us; replay_clock_us is the clock
   where play time reaches play_us (where the recording's play ends, past
   it). With no spans, both are the clock. */
uint64_t replay_play_us(uint64_t clock_us);
uint64_t replay_clock_us(uint64_t play_us);

/* Whether clock_us is in play. If not, *next_on_us gets where play goes on
   next, or UINT64_MAX. */
bool replay_in_play(uint64_t clock_us, uint64_t *next_on_us);

/* The same in ticks (1/60 s), as clips and the viewer count. */
uint32_t replay_play_ticks(uint32_t clock_ticks);
uint32_t replay_clock_ticks(uint32_t play_ticks);

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
/* For what runs at exit: true once replay_desync has run, and true once the
   replay has exited where the recording stopped (run past its end, or
   finished at it, as when the game quits there on its own). */
bool replay_desynced(void);
bool replay_reached_end(void);

/* For tests: forget everything (no file is written). */
void replay_reset(void);
