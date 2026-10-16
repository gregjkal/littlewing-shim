#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Clips (a prototype): with LOONY_REPLAY, LOONY_CLIP=<start>-<end> renders
   that stretch of the replay straight to a video file (LOONY_CLIP_OUT,
   clip.mp4 by default) and exits once it is written. Up to the start, the
   replay runs at full speed and nothing is shown; from there, every tick of
   the game's clock is a frame (60 fps), and the sound is what the mixer
   renders for the same span. No window opens and no frame is written to
   disk.

   Times are m:ss or h:mm:ss from launch, with an optional fraction of a
   second (9:40, 1:02:03.5), or a whole number of ticks (34800).

   LOONY_CLIP=marks makes a clip of each mark in the replay (Cmd-S while
   recording; replay.h) instead, in the same pass: from LOONY_CLIP_BEFORE
   seconds before the mark (CLIP_MARK_BEFORE by default) to LOONY_CLIP_AFTER
   seconds after it (CLIP_MARK_AFTER), since the key is pressed after the
   moment. Marks whose clips overlap, or come within CLIP_MERGE_GAP of each
   other, share one clip. Each file is named for its span (clip_span_name)
   before the extension: <replay>-2m58-3m20.mp4 and so on next to the
   replay (without the replay's -marked), or LOONY_CLIP_OUT-2m58-3m20.mp4
   when it names one. A clip whose file is there already is skipped, so
   exporting again renders only the new ones, and editing the list never
   renames a file (spans that come to merge, or stop merging, get files of
   their own); with none left to render, clip_open exits.

   LOONY_CLIP=clips makes a clip of each span in the replay's list of clips
   (clip_list_path; watching a replay saves them there, watch.h), named as
   for marks. Spans that overlap or touch share one clip.

   A clip is written to a hidden file beside its own (.<name>) and renamed
   when it is finished, so one cut short never passes for a whole one. A
   clip the recording ends inside keeps what there is, even when the replay
   fails there as the recording did; one cut short before the recording's
   end (a desync, a failure, LOONY_EXIT_AFTER) is thrown away. SIGTERM,
   SIGINT or SIGHUP removes the hidden file and exits with 128 plus the
   signal (143 for SIGTERM).

   The picture is the window's: in HD mode (hd.h) the HD frame, at 2x unless
   LOONY_HD_SCALE says otherwise, scaled down to CLIP_HD_WIDTH pixels wide;
   else the 800x600 screen. LOONY_CLIP_WIDTH sets the width instead (the
   height follows), but a picture is never scaled up. The video is
   H.264 at LOONY_CLIP_QUALITY (above 0, at most 1; CLIP_QUALITY by default), or at
   LOONY_CLIP_KBPS kbit/s on average instead; the sound is AAC at 160 kbit/s.

   LOONY_CLIP_PROGRESS=1 reports the export on stdout for a program running
   it, a line for each event, each written whole:
     skipped <file>               a clip whose file is there already
     seek <tick> <target>         replaying, at most 4 a second, on the
                                  game's clock, up to the next clip's start
     clip <k> <n> <frame> <frames>  making clip k of the n to render (from
                                  1), frame of its frames written, at 0 and
                                  each time the percentage changes
     wrote <file>                 a clip finished and renamed into place
     error <message>              an error before the game starts
   <file> is the clip's file as LOONY_CLIP_OUT or the replay names it (so
   relative when that is). Without it, nothing is written to stdout. */

#define CLIP_FPS 60
/* 4/5 of HD at 2x (1280x960): the 1600x1200 picture was more than a phone
   or a chat window shows. Scaling down from 2x keeps it sharper than
   drawing it smaller would. */
#define CLIP_HD_WIDTH 1280
#define CLIP_AUDIO_KBPS 160
/* Apple's H.264 encoder at this quality matched x264 at CRF 33 on a ramp run
   in HD at 2x (PSNR 31.2 dB against 31.0, from a CRF 18 render): about
   1.2 Mbit/s there, where x264 needed 0.4. At a set bitrate it needs more
   bits for the same picture. */
#define CLIP_QUALITY 0.65
/* The exit status when the recording ended before the clip's start, so
   nothing was written. */
#define CLIP_NOTHING_STATUS 4
/* LOONY_CLIP=marks: seconds before and after each mark, and the ticks
   between two clips below which they are one. */
#define CLIP_MARK_BEFORE 30
#define CLIP_MARK_AFTER 3
#define CLIP_MERGE_GAP 60

/* A stretch of the replay to clip: the ticks [start, end), and the marks it
   holds, numbered from 1 (0 and 0 for a LOONY_CLIP span). */
typedef struct {
    uint32_t start, end;
    int first_mark, last_mark;
} clip_span;

/* Parses a span "<start>-<end>" into ticks. False unless start < end. */
bool clip_parse_span(const char *s, uint32_t *start, uint32_t *end);

/* ticks as m:ss (h:mm:ss past an hour), with hundredths when not whole, as
   clip_parse_span reads them back. Returns buf. */
const char *clip_format_time(uint32_t ticks, char *buf, size_t cap);

/* A span as a clip's file names it: clip_format_time's, with m for the
   colon before the seconds and h for the one before the minutes, with
   hundredths when not whole
   (2m58-3m20, 0m05.65-0m07.15, 1h00m01.01-1h00m05). Two spans never get
   the same name. Returns buf. */
const char *clip_span_name(uint32_t start, uint32_t end, char *buf, size_t cap);

/* The list of clips beside the replay: its path with .clips in place of
   .lwreplay (or after it, for another name). False if that is too long. */
bool clip_list_path(const char *replay, char *out, size_t cap);

/* Parses a list of clips: a span a line, as clip_parse_span takes it, with
   spaces around it, blank lines and lines starting with # ignored. *spans
   (to free) gets them in the list's order, 0 and 0 for their marks. False,
   with err naming the line, if one isn't a span. */
bool clip_parse_list(const char *text, clip_span **spans, size_t *n, char *err, size_t errlen);

/* Sorts n spans by start and merges those that overlap or touch, in place:
   returns how many there are then. */
size_t clip_merge_spans(clip_span *spans, size_t n);

/* The clips of n marks at these ticks, in order: from before ticks before
   each (but not before 0) to after ticks after it, merged as LOONY_CLIP=marks
   merges them. before + after must be above 0. out has room for n; returns
   how many it holds. */
size_t clip_mark_spans(const uint32_t *marks, size_t n, uint32_t before, uint32_t after,
                       clip_span *out);

/* The video bitrate for a w x h clip, in kbit/s, when the encoder has no
   quality setting. */
int clip_default_kbps(int w, int h);

/* After replay_open, before hd_init: reads LOONY_CLIP, LOONY_CLIP_OUT,
   LOONY_CLIP_QUALITY, LOONY_CLIP_KBPS, LOONY_CLIP_WIDTH, LOONY_CLIP_BEFORE
   and LOONY_CLIP_AFTER (and sets LOONY_HD_SCALE to 2 unless it is set).
   False, with err, if they're wrong, or set without a replay, or if a file
   to write is the replay itself or in a folder that can't be written (found
   now rather than after the seek), or if LOONY_CLIP=marks and the replay
   has none, or LOONY_CLIP=clips and its list can't be read, is wrong or is
   empty. */
bool clip_open(char *err, size_t errlen);

/* True when a clip is being made: the window shows nothing. */
bool clip_on(void);

/* The event pump's present, in place of the window's: dirty if the screen
   changed since the last one. Adds a frame for each tick the game's clock
   passed; at a clip's end, finishes its file, and exits after the last. */
void clip_present(bool dirty);

/* The mixer's output, in order from launch (sound_set_tap): keeps what falls
   in the clip. */
void clip_sound(const int16_t *samples, uint32_t frames);

/* LOONY_CLIP_PROGRESS's lines, without writing them: what (skipped, wrote,
   error) and text, the text's line breaks made spaces and the text cut
   short if it doesn't fit; seek and clip with their numbers. Each ends in a
   line break. The length, or 0 if cap can't hold even what and the break. */
size_t clip_progress_text(const char *what, const char *text, char *buf, size_t cap);
size_t clip_progress_seek(uint32_t tick, uint32_t target, char *buf, size_t cap);
size_t clip_progress_clip(size_t k, size_t n, uint64_t frame, uint64_t frames, char *buf, size_t cap);

/* Whether a clip line is due for frame of frames: at frame 0, and when the
   whole percentage differs from *percent, which then becomes it. */
bool clip_progress_frame_due(uint64_t frame, uint64_t frames, int *percent);

/* Whether a seek line is due now (in seconds): the first, and each
   CLIP_PROGRESS_SEEK_EVERY after the last (*last, which then becomes now;
   start it below 0). */
#define CLIP_PROGRESS_SEEK_EVERY 0.25
bool clip_progress_seek_due(double now, double *last);

/* With LOONY_CLIP_PROGRESS=1, writes msg as an error line, for a parent to
   show: for errors before the game starts. */
void clip_progress_error(const char *msg);
