#include "test.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
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

/* Each mark's clip runs from before it to after it; clips that overlap or
   nearly meet are one. */
TEST(clip_marks_make_spans_merged_where_they_meet) {
    uint32_t marks[] = {1800, 2000, 9000, 9000 + 180 + CLIP_MERGE_GAP + 1800,
                        9000 + 2 * (180 + CLIP_MERGE_GAP + 1800) + 1};
    clip_span s[5];
    CHECK_EQ(clip_mark_spans(marks, 5, 1800, 180, s), 3);
    CHECK_EQ(s[0].start, 0);
    CHECK_EQ(s[0].end, 2180);
    CHECK_EQ(s[0].first_mark, 1);
    CHECK_EQ(s[0].last_mark, 2);
    CHECK_EQ(s[1].start, 7200);
    CHECK_EQ(s[1].end, marks[3] + 180); /* CLIP_MERGE_GAP apart: one clip */
    CHECK_EQ(s[1].first_mark, 3);
    CHECK_EQ(s[1].last_mark, 4);
    CHECK_EQ(s[2].start, marks[4] - 1800); /* a tick more: another */
    CHECK_EQ(s[2].first_mark, 5);
    uint32_t zero = 0;
    CHECK_EQ(clip_mark_spans(&zero, 1, 60, 0, s), 1);
    CHECK_EQ(s[0].start, 0);
    CHECK_EQ(s[0].end, 1);
}

/* What clip_format_time writes, clip_parse_span reads back as the same
   ticks, hundredths and all. */
TEST(clip_times_read_back_as_written) {
    char a[32], b[32], s[80];
    bool all = true;
    for (uint32_t t = 0; t < 2 * 3600 * 60; t += t < 7200 ? 1 : 997) {
        snprintf(s, sizeof s, "%s-%s", clip_format_time(t, a, sizeof a),
                 clip_format_time(t + 1, b, sizeof b));
        if (!span(s, t, t + 1)) {
            fprintf(stderr, "%u: %s\n", t, s);
            all = false;
        }
    }
    CHECK(all);
    CHECK_STR(clip_format_time(3600 * 60 + 61, a, sizeof a), "1:00:01.01");
    CHECK_STR(clip_format_time(600, a, sizeof a), "0:10");
}

/* A clip's file is named for its span, with no colons in it, and no two
   spans share a name. */
TEST(clip_span_names_are_times_without_colons) {
    char a[64], b[64];
    CHECK_STR(clip_span_name(178 * 60, 200 * 60, a, sizeof a), "2m58-3m20");
    CHECK_STR(clip_span_name(339, 429, a, sizeof a), "0m05.65-0m07.15");
    CHECK_STR(clip_span_name(3600 * 60 + 61, 3605 * 60, a, sizeof a), "1h00m01.01-1h00m05");
    CHECK_STR(clip_span_name(0, 600, a, sizeof a), "0m00-0m10");
    bool distinct = true;
    for (uint32_t t = 0; t < 2 * 3600 * 60; t += t < 7200 ? 1 : 997) {
        clip_span_name(t, t + 1, a, sizeof a);
        clip_span_name(t + 1, t + 2, b, sizeof b);
        if (strcmp(a, b) == 0 || strchr(a, ':') || strchr(a, '/')) {
            fprintf(stderr, "%u: %s\n", t, a);
            distinct = false;
        }
    }
    CHECK(distinct);
    CHECK_STR(clip_span_name(0, 600, a, 6), "0m00-"); /* cut short, but ended */
}

/* A list of clips (watching a replay saves one): a span a line. */
TEST(clip_lists_are_a_span_a_line) {
    clip_span *s;
    size_t n;
    char err[256] = "";
    CHECK(clip_parse_list("# ramps\n9:40-10:15\n\n  1:00-1:30 \r\n600-700", &s, &n, err, sizeof err));
    CHECK_EQ(n, 3);
    CHECK_EQ(s[0].start, 34800);
    CHECK_EQ(s[0].end, 36900);
    CHECK_EQ(s[1].start, 3600);
    CHECK_EQ(s[1].end, 5400);
    CHECK_EQ(s[2].start, 600);
    CHECK_EQ(s[2].end, 700);
    CHECK_EQ(s[2].first_mark, 0);
    free(s);
    CHECK(clip_parse_list("", &s, &n, err, sizeof err));
    CHECK_EQ(n, 0);
    free(s);
    CHECK(!clip_parse_list("1:00-1:30\n1:30-1:00\n", &s, &n, err, sizeof err));
    CHECK_CONTAINS(err, "line 2: 1:30-1:00 isn't <start>-<end>");
    CHECK(!s);
    CHECK_EQ(n, 0);
}

/* Spans from a list are sorted, and those that overlap or touch are one. */
TEST(clip_list_spans_are_sorted_and_merged_where_they_overlap) {
    clip_span s[] = {{600, 700, 0, 0}, {100, 200, 0, 0}, {650, 900, 0, 0}, {200, 300, 0, 0},
                     {301, 400, 0, 0}, {610, 620, 0, 0}};
    CHECK_EQ(clip_merge_spans(s, 6), 3);
    CHECK_EQ(s[0].start, 100);
    CHECK_EQ(s[0].end, 300); /* touching */
    CHECK_EQ(s[1].start, 301);
    CHECK_EQ(s[1].end, 400);
    CHECK_EQ(s[2].start, 600);
    CHECK_EQ(s[2].end, 900); /* overlapping, and one inside another */
}

TEST(clip_lists_go_beside_the_replay) {
    char p[64];
    CHECK(clip_list_path("/a/game.lwreplay", p, sizeof p));
    CHECK_STR(p, "/a/game.clips");
    CHECK(clip_list_path("game.rec", p, sizeof p));
    CHECK_STR(p, "game.rec.clips");
    CHECK(!clip_list_path("/a/very-long-name-of-a-recorded-game-from-last-tuesday.lwreplay", p, 32));
}

TEST(clip_default_bitrate_grows_with_the_picture) {
    CHECK(clip_default_kbps(800, 600) < clip_default_kbps(1600, 1200));
    CHECK(clip_default_kbps(1600, 1200) < clip_default_kbps(3200, 2400));
    CHECK(clip_default_kbps(16, 16) >= 100);
}

/* LOONY_CLIP_PROGRESS's lines are whole, one to an event, and say a
   path or message as it is, but on one line. */
TEST(clip_progress_lines_are_whole_lines) {
    char buf[64];
    CHECK_EQ(clip_progress_text("wrote", "/tmp/a b/game-0m01-0m04.mp4", buf, sizeof buf), 34);
    CHECK_STR(buf, "wrote /tmp/a b/game-0m01-0m04.mp4\n");
    CHECK_EQ(clip_progress_text("error", "no clips\nin game.clips\r", buf, sizeof buf), 30);
    CHECK_STR(buf, "error no clips in game.clips \n");
    CHECK_EQ(clip_progress_text("skipped", "abcdefghij", buf, 12), 11);
    CHECK_STR(buf, "skipped ab\n");
    CHECK_EQ(clip_progress_text("skipped", "abc", buf, 9), 0);
    CHECK_EQ(clip_progress_seek(1234, 34800, buf, sizeof buf), 16);
    CHECK_STR(buf, "seek 1234 34800\n");
    CHECK_EQ(clip_progress_clip(2, 3, 90, 180, buf, sizeof buf), 16);
    CHECK_STR(buf, "clip 2 3 90 180\n");
    CHECK_EQ(clip_progress_clip(2, 3, 90, 180, buf, 10), 0);
}

/* A clip line comes at the first frame and each time the percentage
   changes, so a long clip says no more than a short one; a seek line
   comes at most CLIP_PROGRESS_SEEK_EVERY. */
TEST(clip_progress_lines_are_rate_limited) {
    int percent = 50, lines = 0;
    for (uint64_t f = 0; f <= 18000; f++)
        lines += clip_progress_frame_due(f, 18000, &percent);
    CHECK_EQ(lines, 101);
    CHECK_EQ(percent, 100);
    percent = 100;
    CHECK(clip_progress_frame_due(0, 18000, &percent)); /* the next clip's first frame */
    CHECK_EQ(percent, 0);
    CHECK(!clip_progress_frame_due(179, 18000, &percent));
    CHECK(clip_progress_frame_due(180, 18000, &percent));
    CHECK_EQ(percent, 1);
    lines = 0;
    percent = 0;
    for (uint64_t f = 0; f <= 30; f++) /* fewer frames than percents: each one */
        lines += clip_progress_frame_due(f, 30, &percent);
    CHECK_EQ(lines, 31);
    double last = -1;
    CHECK(clip_progress_seek_due(0.0, &last));
    CHECK(!clip_progress_seek_due(0.1, &last));
    CHECK(!clip_progress_seek_due(0.24, &last));
    CHECK(clip_progress_seek_due(0.25, &last));
    lines = 0;
    last = -1;
    for (int ms = 0; ms < 3000; ms++)
        lines += clip_progress_seek_due(ms / 1000.0, &last);
    CHECK(lines >= 10 && lines <= 12);
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

static const char *const env_names[] = {"LOONY_CLIP",         "LOONY_CLIP_OUT",   "LOONY_CLIP_KBPS",
                                        "LOONY_REPLAY",       "LOONY_CLIP_QUALITY", "LOONY_CLIP_WIDTH",
                                        "LOONY_CLIP_BEFORE",  "LOONY_CLIP_AFTER"};

static void clear_env(void) {
    for (size_t i = 0; i < sizeof env_names / sizeof env_names[0]; i++)
        unsetenv(env_names[i]);
}

/* LOONY_CLIP_QUALITY, _WIDTH, _BEFORE and _AFTER for clip_open_with */
static const char *quality, *width, *before, *after;

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
    const char *v[] = {clip, out, kbps, replay, quality, width, before, after};
    for (int i = 0; i < 8; i++)
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
    CHECK_CONTAINS(out, "isn't marks, clips or <start>-<end>");
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
    before = "10";
    CHECK_EQ(clip_open_with(NULL, NULL, NULL, "r.lwreplay", out, sizeof out), 1);
    CHECK_CONTAINS(out, "LOONY_CLIP_AFTER need LOONY_CLIP");
    CHECK_EQ(clip_open_with("1:00-1:30", NULL, NULL, "r.lwreplay", out, sizeof out), 1);
    CHECK_CONTAINS(out, "are for LOONY_CLIP=marks");
    before = "-1";
    CHECK_EQ(clip_open_with("marks", NULL, NULL, "r.lwreplay", out, sizeof out), 1);
    CHECK_CONTAINS(out, "LOONY_CLIP_BEFORE=-1 isn't 0 to 3600 (seconds)");
    before = "0";
    after = "0";
    CHECK_EQ(clip_open_with("marks", NULL, NULL, "r.lwreplay", out, sizeof out), 1);
    CHECK_CONTAINS(out, "can't both be 0");
    before = after = NULL;
    /* No replay was opened here, so it has no marks. */
    CHECK_EQ(clip_open_with("marks", NULL, NULL, "r.lwreplay", out, sizeof out), 1);
    CHECK_CONTAINS(out, "the replay r.lwreplay has no marks");
    CHECK_EQ(clip_open_with("clips", NULL, NULL, "/nonexistent/r.lwreplay", out, sizeof out), 1);
    CHECK_CONTAINS(out, "can't read the clips of /nonexistent/r.lwreplay from /nonexistent/r.clips");
    before = "10";
    CHECK_EQ(clip_open_with("clips", NULL, NULL, "r.lwreplay", out, sizeof out), 1);
    CHECK_CONTAINS(out, "are for LOONY_CLIP=marks");
    before = NULL;
}

/* LOONY_CLIP=clips: a clip of each span in the replay's list, sorted and
   merged, named for their spans as for marks; a list that is wrong or
   empty is refused. */
TEST(clip_open_makes_a_clip_of_each_span_in_the_list) {
    char out[4096], dir[1024], replay[1100], list[1100];
    test_tmp_dir(dir, sizeof dir);
    snprintf(replay, sizeof replay, "%s/game.lwreplay", dir);
    snprintf(list, sizeof list, "%s/game.clips", dir);
    FILE *f = fopen(list, "w");
    fputs("0:20-0:30\n0:01-0:02.5\n0:02-0:03\n", f);
    fclose(f);
    int listed = clip_open_with("clips", NULL, NULL, replay, out, sizeof out);
    char clip1[1200], clip2[1200], log2[4096];
    snprintf(clip1, sizeof clip1, "clip: 0:01-0:03 to %s/game-0m01-0m03.mp4", dir);
    snprintf(clip2, sizeof clip2, "clip: 0:20-0:30 to %s/game-0m20-0m30.mp4", dir);
    f = fopen(list, "w");
    fputs("# nothing yet\n", f);
    fclose(f);
    int empty = clip_open_with("clips", NULL, NULL, replay, log2, sizeof log2);
    test_remove_tree(dir);
    CHECK_EQ(listed, 0);
    CHECK_CONTAINS(out, "clip: 3 listed in");
    CHECK_CONTAINS(out, ", some overlapping");
    CHECK_CONTAINS(out, clip1);
    CHECK_CONTAINS(out, clip2);
    CHECK_EQ(empty, 1);
    CHECK_CONTAINS(log2, "game.clips has no clips");
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

/* Exporting again renders only the clips with no file yet. Taking a line
   out of the list renames none of the others' files, so with each of them
   written, there is nothing to render, and the export exits at once. */
TEST(clip_open_skips_the_clips_already_written) {
    char dir[1024], replay[1100], list[1100], first[1100], others[2][1100];
    char out[3][4096], want[3][1300];
    test_tmp_dir(dir, sizeof dir);
    snprintf(replay, sizeof replay, "%s/game.lwreplay", dir);
    snprintf(list, sizeof list, "%s/game.clips", dir);
    snprintf(first, sizeof first, "%s/game-0m01-0m02.mp4", dir);
    snprintf(others[0], sizeof others[0], "%s/game-0m10-0m12.mp4", dir);
    snprintf(others[1], sizeof others[1], "%s/game-0m20-0m30.mp4", dir);
    write_text(list, "0:01-0:02\n0:10-0:12\n0:20-0:30\n");
    write_text(first, "");
    int some = clip_open_with("clips", NULL, NULL, replay, out[0], sizeof out[0]);
    snprintf(want[0], sizeof want[0], "clip: 0:01-0:02 is in %s already: skipped", first);
    snprintf(want[1], sizeof want[1], "clip: 0:10-0:12 to %s", others[0]);
    snprintf(want[2], sizeof want[2], "clip: 0:20-0:30 to %s", others[1]);
    write_text(others[0], "");
    write_text(others[1], "");
    int all = clip_open_with("clips", NULL, NULL, replay, out[1], sizeof out[1]);
    write_text(list, "0:10-0:12\n0:20-0:30\n"); /* the first line taken out */
    unlink(first);
    int edited = clip_open_with("clips", NULL, NULL, replay, out[2], sizeof out[2]);
    test_remove_tree(dir);
    CHECK_EQ(some, 0);
    CHECK_CONTAINS(out[0], want[0]);
    CHECK_CONTAINS(out[0], want[1]);
    CHECK_CONTAINS(out[0], want[2]);
    CHECK_CONTAINS(out[0], "clip on: 1");
    CHECK_EQ(all, 0);
    CHECK_CONTAINS(out[1], "clip: all 3 clips are written already");
    CHECK(!strstr(out[1], "clip on:"));
    CHECK_EQ(edited, 0);
    CHECK_CONTAINS(out[2], "clip: all 2 clips are written already");
    CHECK(!strstr(out[2], "clip on:"));
}

/* The clips are named after the replay without its -marked, or after
   LOONY_CLIP_OUT, keeping its extension. */
TEST(clip_open_names_the_clips_after_the_replay_or_the_out) {
    char dir[1024], replay[1100], out_mov[1100], log[3][4096], want[3][1300];
    test_tmp_dir(dir, sizeof dir);
    snprintf(replay, sizeof replay, "%s/loony-2026-10-15-201530-marked.lwreplay", dir);
    char list[1100];
    snprintf(list, sizeof list, "%s/loony-2026-10-15-201530-marked.clips", dir);
    write_text(list, "2:58-3:20\n");
    snprintf(out_mov, sizeof out_mov, "%s/ramps.mov", dir);
    int named[3] = {clip_open_with("clips", NULL, NULL, replay, log[0], sizeof log[0]),
                    clip_open_with("clips", "ramps.mp4", NULL, replay, log[1], sizeof log[1]),
                    clip_open_with("clips", out_mov, NULL, replay, log[2], sizeof log[2])};
    snprintf(want[0], sizeof want[0], "clip: 2:58-3:20 to %s/loony-2026-10-15-201530-2m58-3m20.mp4", dir);
    snprintf(want[1], sizeof want[1], "clip: 2:58-3:20 to ramps-2m58-3m20.mp4");
    snprintf(want[2], sizeof want[2], "clip: 2:58-3:20 to %s/ramps-2m58-3m20.mov", dir);
    test_remove_tree(dir);
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(named[i], 0);
        CHECK_CONTAINS(log[i], want[i]);
    }
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
