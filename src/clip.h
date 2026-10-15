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
   other, share one clip. The files are numbered from 1 before the
   extension: <replay>-clip-1.mp4 and so on next to the replay, or
   LOONY_CLIP_OUT-1.mp4 when it names one.

   The picture is the window's: in HD mode (hd.h) the HD frame, at 2x unless
   LOONY_HD_SCALE says otherwise, scaled down to CLIP_HD_WIDTH pixels wide;
   else the 800x600 screen. LOONY_CLIP_WIDTH sets the width instead (the
   height follows), but a picture is never scaled up. The video is
   H.264 at LOONY_CLIP_QUALITY (above 0, at most 1; CLIP_QUALITY by default), or at
   LOONY_CLIP_KBPS kbit/s on average instead; the sound is AAC at 160 kbit/s. */

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
   has none. */
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
