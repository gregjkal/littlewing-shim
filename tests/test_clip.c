#include "test.h"

#include <math.h>
#include <stdlib.h>
#include <unistd.h>

#include "clip.h"
#include "mp4.h"

static bool span(const char *s, uint32_t start, uint32_t end) {
    uint32_t a = 0, b = 0;
    return clip_parse_span(s, &a, &b) && a == start && b == end;
}

static bool bad_span(const char *s) {
    uint32_t a, b;
    return !clip_parse_span(s, &a, &b);
}

TEST(clip_spans_are_times_or_ticks) {
    CHECK(span("9:40-10:15", 9 * 3600 + 40 * 60, 10 * 3600 + 15 * 60));
    CHECK(span("0:00-0:01", 0, 60));
    CHECK(span("34800-36900", 34800, 36900));
    CHECK(span("1:02:03-1:02:04", (3600 + 2 * 60 + 3) * 60, (3600 + 2 * 60 + 4) * 60));
    CHECK(span("0:05.5-0:06", 330, 360));
    CHECK(span("0:05.125-0:06", 308, 360)); /* 5.125 s is 307.5 ticks: rounded */
    CHECK(span("100-1:00", 100, 3600));     /* the forms mix */
    CHECK(bad_span("10:15-9:40"));
    CHECK(bad_span("5-5"));
    CHECK(bad_span("9:40"));
    CHECK(bad_span(""));
    CHECK(bad_span("9:4-10:00"));  /* seconds are two digits */
    CHECK(bad_span("9:60-10:00"));
    CHECK(bad_span("1:60:00-2:00:00"));
    CHECK(bad_span("a-b"));
    CHECK(bad_span("-100"));
    CHECK(bad_span("1:00.-2:00"));
    CHECK(bad_span("1:00.1234-2:00"));
    CHECK(bad_span("1:00-2:00 "));
    CHECK(bad_span("99999999999-99999999999999"));
}

TEST(clip_default_bitrate_grows_with_the_picture) {
    CHECK(clip_default_kbps(800, 600) < clip_default_kbps(1600, 1200));
    CHECK(clip_default_kbps(1600, 1200) < clip_default_kbps(3200, 2400));
    CHECK(clip_default_kbps(16, 16) >= 100);
}

static void tmp_mp4(char *path, size_t cap) {
    const char *t = getenv("TMPDIR");
    snprintf(path, cap, "%s/loony-clip-XXXXXX.mp4", t && *t ? t : "/tmp");
    int fd = mkstemps(path, 4);
    if (fd >= 0)
        close(fd);
}

enum { W = 320, H = 240, FRAMES = 120, RATE = 44100 };

/* A small video at quality, or at 300 kbit/s with quality 0. */
static mp4_settings small(double quality) {
    return (mp4_settings){.w = W, .h = H, .fps = 60, .quality = quality, .video_kbps = 300,
                          .rate = RATE, .audio_kbps = 128};
}

/* Two seconds at 60 fps, every other frame a repeat, and a 440 Hz tone at
   half scale: all of it comes back, out_w x out_h (0: W x H). */
static void write_and_read(double quality, int out_w, int out_h) {
    char path[1024];
    tmp_mp4(path, sizeof path); /* there already: mp4_open replaces it */
    char err[256] = "";
    mp4_settings s = small(quality);
    if (out_w) {
        s.in_w = W;
        s.in_h = H;
        s.w = out_w;
        s.h = out_h;
    }
    mp4_file *f = mp4_open(path, &s, err, sizeof err);
    if (!f)
        fprintf(stderr, "%s\n", err);
    CHECK(f != NULL);
    if (!quality)
        CHECK(!mp4_at_quality(f));
    uint8_t *rgba = malloc(W * H * 4);
    int16_t *tone = malloc(RATE / 60 * 4);
    CHECK(rgba && tone);
    bool ok = true;
    for (int i = 0; i < FRAMES && ok; i++) {
        for (int k = 0; k < RATE / 60; k++) {
            int16_t v = (int16_t)lround(16384 * sin(2 * M_PI * 440 * (i * (RATE / 60) + k) / RATE));
            tone[2 * k] = tone[2 * k + 1] = v;
        }
        ok = mp4_audio(f, tone, RATE / 60, (int64_t)i * (RATE / 60));
        for (int p = 0; p < W * H; p++) { /* a bar that moves */
            bool bar = p % W / 8 == i / 2 % (W / 8);
            rgba[4 * p] = bar ? 255 : 20;
            rgba[4 * p + 1] = (uint8_t)(p / W);
            rgba[4 * p + 2] = 80;
            rgba[4 * p + 3] = 255;
        }
        ok = ok && mp4_video(f, i % 2 ? NULL : rgba, i);
    }
    free(rgba);
    free(tone);
    CHECK(ok);
    bool closed = mp4_close(f, err, sizeof err);
    if (!closed)
        fprintf(stderr, "%s\n", err);
    CHECK(closed);
    mp4_info info;
    bool read = mp4_read_info(path, &info, err, sizeof err);
    unlink(path);
    if (!read)
        fprintf(stderr, "%s\n", err);
    CHECK(read);
    CHECK_EQ(info.width, s.w);
    CHECK_EQ(info.height, s.h);
    CHECK_EQ(info.video_frames, FRAMES);
    CHECK(fabs(info.video_seconds - 2.0) < 0.001);
    CHECK(fabs(info.audio_seconds - 2.0) < 0.03);
    CHECK(info.peak > 0.4 && info.peak < 0.6);
}

TEST(mp4_writes_frames_and_sound_at_a_quality) {
    RUN_UNFORKED();
    write_and_read(CLIP_QUALITY, 0, 0);
}

TEST(mp4_writes_frames_and_sound_at_a_bitrate) {
    RUN_UNFORKED();
    write_and_read(0, 0, 0);
}

TEST(mp4_scales_the_frames_it_gets) {
    RUN_UNFORKED();
    write_and_read(CLIP_QUALITY, 256, 192);
}

/* The writer may refuse at once or only when it writes: either way, it says so. */
TEST(mp4_reports_a_file_it_cant_write) {
    RUN_UNFORKED();
    char err[256] = "";
    mp4_settings s = small(CLIP_QUALITY);
    mp4_file *f = mp4_open("/nonexistent/folder/clip.mp4", &s, err, sizeof err);
    if (f) {
        static uint8_t black[W * H * 4];
        mp4_video(f, black, 0);
        CHECK(!mp4_close(f, err, sizeof err));
    }
    CHECK(err[0] != '\0');
}

TEST(mp4_needs_a_first_frame_to_repeat) {
    RUN_UNFORKED();
    char path[1024], err[256] = "";
    tmp_mp4(path, sizeof path);
    mp4_settings s = small(CLIP_QUALITY);
    mp4_file *f = mp4_open(path, &s, err, sizeof err);
    CHECK(f != NULL);
    CHECK(!mp4_video(f, NULL, 0));
    CHECK(!mp4_close(f, err, sizeof err));
    unlink(path);
    CHECK_CONTAINS(err, "no frame to show again");
}

static const char *const env_names[] = {"LOONY_CLIP", "LOONY_CLIP_OUT", "LOONY_CLIP_KBPS",
                                        "LOONY_REPLAY", "LOONY_CLIP_QUALITY", "LOONY_CLIP_WIDTH"};

static void clear_env(void) {
    for (size_t i = 0; i < sizeof env_names / sizeof env_names[0]; i++)
        unsetenv(env_names[i]);
}

static const char *quality, *width; /* LOONY_CLIP_QUALITY and _WIDTH for clip_open_with */

/* clip_open, in a child: it turns the clip on for good. Its err is the
   output. The child exits with exit, so what a clip logs at exit is there.
   It runs in the temporary folder, so a relative LOONY_CLIP_OUT is in a
   folder that can be written. */
static void child_clip_open(void *unused) {
    (void)unused;
    const char *t = getenv("TMPDIR");
    if (chdir(t && *t ? t : "/tmp") != 0)
        _exit(127);
    char err[256];
    if (!clip_open(err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        _exit(1);
    }
    fprintf(stderr, "clip on: %d\n", clip_on());
    exit(0);
}

static int clip_open_with(const char *clip, const char *out, const char *kbps, const char *replay,
                          char *log, size_t cap) {
    clear_env();
    const char *v[] = {clip, out, kbps, replay, quality, width};
    for (int i = 0; i < 6; i++)
        if (v[i])
            setenv(env_names[i], v[i], 1);
    int status = test_run_child(child_clip_open, NULL, log, cap);
    clear_env();
    return status;
}

TEST(clip_open_checks_its_settings) {
    char out[4096];
    CHECK_EQ(clip_open_with(NULL, NULL, NULL, NULL, out, sizeof out), 0);
    CHECK_CONTAINS(out, "clip on: 0");
    CHECK_EQ(clip_open_with("1:00-1:30", NULL, NULL, NULL, out, sizeof out), 1);
    CHECK_CONTAINS(out, "LOONY_CLIP needs a replay");
    CHECK_EQ(clip_open_with(NULL, "x.mp4", NULL, NULL, out, sizeof out), 1);
    CHECK_CONTAINS(out, "need LOONY_CLIP");
    CHECK_EQ(clip_open_with("1:30-1:00", NULL, NULL, "r.lwreplay", out, sizeof out), 1);
    CHECK_CONTAINS(out, "isn't <start>-<end>");
    CHECK_EQ(clip_open_with("1:00-1:30", NULL, "fast", "r.lwreplay", out, sizeof out), 1);
    CHECK_CONTAINS(out, "LOONY_CLIP_KBPS=fast");
    /* The child exits before the clip starts, but not at the end of a
       recording: nothing written, and the exit keeps its status. */
    CHECK_EQ(clip_open_with("1:00-1:30", "x.mp4", "800", "r.lwreplay", out, sizeof out), 0);
    CHECK_CONTAINS(out, "clip: replaying to 1:00, then writing 1:00-1:30 to x.mp4");
    CHECK_CONTAINS(out, "clip on: 1");
    CHECK_CONTAINS(out, "clip: the replay stopped before 1:00"); /* at exit, never started */
    quality = "1.5";
    CHECK_EQ(clip_open_with("1:00-1:30", NULL, NULL, "r.lwreplay", out, sizeof out), 1);
    CHECK_CONTAINS(out, "LOONY_CLIP_QUALITY=1.5 isn't");
    quality = "0.8";
    CHECK_EQ(clip_open_with("1:00-1:30", NULL, "800", "r.lwreplay", out, sizeof out), 1);
    CHECK_CONTAINS(out, "can't be used together");
    CHECK_EQ(clip_open_with("1:00-1:30", NULL, NULL, "r.lwreplay", out, sizeof out), 0);
    CHECK_CONTAINS(out, "to clip.mp4"); /* the default name */
    quality = NULL;
    width = "1281";
    CHECK_EQ(clip_open_with("1:00-1:30", NULL, NULL, "r.lwreplay", out, sizeof out), 1);
    CHECK_CONTAINS(out, "LOONY_CLIP_WIDTH=1281 isn't an even number");
    width = "1600";
    CHECK_EQ(clip_open_with("1:00-1:30", NULL, NULL, "r.lwreplay", out, sizeof out), 0);
    width = NULL;
}

/* An output the clip couldn't write is refused before the seek, and so is
   one that would write over the replay. */
TEST(clip_open_checks_where_the_clip_goes) {
    char out[4096], replay[1024];
    tmp_mp4(replay, sizeof replay); /* a file there, standing in for the replay */
    CHECK_EQ(clip_open_with("1:00-1:30", replay, NULL, replay, out, sizeof out), 1);
    CHECK_CONTAINS(out, "is the replay");
    CHECK_EQ(clip_open_with("1:00-1:30", "/nonexistent/folder/x.mp4", NULL, replay, out, sizeof out), 1);
    CHECK_CONTAINS(out, "LOONY_CLIP_OUT=/nonexistent/folder/x.mp4: can't write in /nonexistent/folder");
    CHECK_EQ(clip_open_with("1:00-1:30", "/tmp", NULL, replay, out, sizeof out), 1);
    CHECK_CONTAINS(out, "LOONY_CLIP_OUT=/tmp is a folder");
    CHECK_EQ(clip_open_with("1:00-1:30", "/tmp/new-folder/", NULL, replay, out, sizeof out), 1);
    CHECK_CONTAINS(out, "LOONY_CLIP_OUT=/tmp/new-folder/ is a folder");
    unlink(replay);
}
