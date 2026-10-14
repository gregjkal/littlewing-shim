#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Writes a video file with AVFoundation: H.264 video from RGBA frames and
   AAC sound from 16-bit stereo samples, so no ffmpeg is needed. An .mov path
   makes a QuickTime movie; anything else, an MP4. Frames and sound are
   placed by their own timestamps, starting at 0. */

typedef struct mp4_file mp4_file;

typedef struct {
    int w, h, fps;  /* the video: w x h (both even), fps frames a second */
    int in_w, in_h; /* the frames mp4_video gets, scaled to w x h (high-quality
                       resampling); 0 for w x h */
    double quality; /* 0 to 1: the video's quality, or 0 for video_kbps */
    int video_kbps; /* the video's average bitrate without a quality, or when
                       the encoder has no quality setting (mp4_at_quality) */
    int rate, audio_kbps; /* the sound: stereo, at rate Hz, audio_kbps kbit/s */
} mp4_settings;

/* Starts a video file with these settings. Replaces a file already at path.
   NULL, with err, on failure. */
mp4_file *mp4_open(const char *path, const mp4_settings *s, char *err, size_t errlen);

/* Whether the video is at the quality asked for (else at video_kbps). */
bool mp4_at_quality(const mp4_file *f);

/* Frame number frame (shown from frame / fps seconds): rgba is in_w x in_h,
   rows top to bottom, or NULL to show the frame before it again. Frames
   come in order. */
bool mp4_video(mp4_file *f, const uint8_t *rgba, int64_t frame);

/* frames stereo frames of sound (2 * frames samples), the first at sample
   at (at / rate seconds). Sound comes in order. */
bool mp4_audio(mp4_file *f, const int16_t *samples, uint32_t frames, int64_t at);

/* Finishes the file and frees f. False, with err, if anything failed on the
   way; the file is then incomplete. */
bool mp4_close(mp4_file *f, char *err, size_t errlen);

/* What a video file holds, read back (the tests use it). */
typedef struct {
    int width, height;     /* of the video track */
    int64_t video_frames;  /* frames in the video track */
    double video_seconds;
    double audio_seconds;  /* 0 without a sound track */
    double peak;           /* the loudest decoded sample, 0 to 1 */
} mp4_info;
bool mp4_read_info(const char *path, mp4_info *out, char *err, size_t errlen);
