#include "clip.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hd.h"
#include "misc.h"
#include "mixer.h"
#include "mp4.h"
#include "qd.h"
#include "replay.h"
#include "sound.h"
#include "util.h"

static struct {
    bool on, finished;
    clip_span *spans; /* the clips to make, in order */
    size_t nspans, cur;
    bool marks;          /* LOONY_CLIP=marks: the files are numbered */
    char base[PATH_MAX]; /* the file, or what the numbered ones are named from */
    uint32_t start, end; /* ticks: the clip being made is [start, end) */
    char out[PATH_MAX];  /* and its file */
    bool wrote;          /* a clip was written */
    double quality; /* or 0 for kbps */
    int kbps;       /* 0: clip_default_kbps */
    int width; /* LOONY_CLIP_WIDTH, or 0 */
    mp4_file *mp4;
    int w, h;          /* the picture */
    uint8_t *rgba;     /* the screen as RGBA, when not in HD */
    uint32_t next;     /* the next tick to add a frame for */
    bool changed;      /* the screen changed since the last frame */
    uint64_t frames;   /* frames added */
    uint64_t rendered; /* sound frames the mixer has rendered since launch */
    /* Sound in the clip that came before the file was opened (the pump
       renders sound before it presents). */
    int16_t *early;
    size_t early_frames, early_cap;
    uint64_t early_at;
    uint32_t logged; /* the minute of the seek last logged */
    struct timespec t0;
} C;

/* ---- times ---- */

/* Whole digits in [p, end), at most max_digits of them (0: any number). */
static bool digits(const char *p, const char *end, int max_digits, uint64_t *out) {
    if (p == end || (max_digits && end - p > max_digits))
        return false;
    uint64_t v = 0;
    for (; p < end; p++) {
        if (*p < '0' || *p > '9' || v > UINT32_MAX)
            return false;
        v = v * 10 + (uint64_t)(*p - '0');
    }
    *out = v;
    return true;
}

/* One time in [p, end): ticks, or [h:]m:ss[.fraction]. */
static bool parse_time(const char *p, const char *end, uint32_t *ticks) {
    const char *c1 = memchr(p, ':', (size_t)(end - p));
    uint64_t v;
    if (!c1) {
        if (!digits(p, end, 0, &v) || v > UINT32_MAX)
            return false;
        *ticks = (uint32_t)v;
        return true;
    }
    const char *c2 = memchr(c1 + 1, ':', (size_t)(end - c1 - 1));
    uint64_t h = 0, m, s, ms = 0;
    const char *sec = (c2 ? c2 : c1) + 1, *dot = memchr(sec, '.', (size_t)(end - sec));
    const char *sec_end = dot ? dot : end;
    if (c2 && (!digits(p, c1, 0, &h) || !digits(c1 + 1, c2, 2, &m) || m >= 60))
        return false;
    if (!c2 && !digits(p, c1, 0, &m))
        return false;
    if (sec_end - sec != 2 || !digits(sec, sec_end, 2, &s) || s >= 60)
        return false;
    if (dot) {
        uint64_t f;
        int n = (int)(end - dot - 1);
        if (!digits(dot + 1, end, 3, &f))
            return false;
        ms = f * (n == 1 ? 100 : n == 2 ? 10 : 1);
    }
    uint64_t total_ms = ((h * 60 + m) * 60 + s) * 1000 + ms;
    uint64_t t = (total_ms * 60 + 500) / 1000;
    if (t > UINT32_MAX)
        return false;
    *ticks = (uint32_t)t;
    return true;
}

bool clip_parse_span(const char *s, uint32_t *start, uint32_t *end) {
    const char *dash = strchr(s, '-');
    if (!dash || !parse_time(s, dash, start) || !parse_time(dash + 1, dash + 1 + strlen(dash + 1), end))
        return false;
    return *start < *end;
}

size_t clip_mark_spans(const uint32_t *marks, size_t n, uint32_t before, uint32_t after,
                       clip_span *out) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t start = marks[i] > before ? marks[i] - before : 0;
        uint32_t end = marks[i] > UINT32_MAX - after ? UINT32_MAX : marks[i] + after;
        if (end == start) /* a mark at 0 with nothing after it */
            end++;
        int mark = (int)i + 1;
        if (k && start <= (uint64_t)out[k - 1].end + CLIP_MERGE_GAP) {
            if (end > out[k - 1].end)
                out[k - 1].end = end;
            out[k - 1].last_mark = mark;
            continue;
        }
        out[k++] = (clip_span){start, end, mark, mark};
    }
    return k;
}

/* ticks as m:ss (h:mm:ss past an hour), with hundredths when not whole. */
static const char *fmt_ticks(uint32_t ticks, char *buf, size_t cap) {
    uint32_t s = ticks / 60, rem = ticks % 60;
    int n = s >= 3600 ? snprintf(buf, cap, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60)
                      : snprintf(buf, cap, "%u:%02u", s / 60, s % 60);
    if (rem && n > 0 && (size_t)n < cap)
        snprintf(buf + n, cap - (size_t)n, ".%02u", rem * 100 / 60);
    return buf;
}

/* ---- the file ---- */

/* 1.2 Mbit/s at 1600x1200, about what CLIP_QUALITY comes to there (Apple's
   encoder holds its quality on fewer bits than it needs at a set bitrate),
   scaled with the picture's side. */
int clip_default_kbps(int w, int h) {
    int kbps = (int)lround(1200.0 * sqrt((double)w * h / (1600.0 * 1200.0)));
    return kbps < 100 ? 100 : kbps;
}

static uint64_t sound_frame_of(uint32_t tick) { return (uint64_t)tick * MIX_RATE / CLIP_FPS; }

static double since_launch(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - C.t0.tv_sec) + (now.tv_nsec - C.t0.tv_nsec) / 1e9;
}

static void open_file(void) {
    qd_pixels px;
    qd_palette pal;
    qd_screen(&px, &pal);
    int n = hd_scale() ? hd_scale() : 1;
    C.w = rect_w(px.bounds) * n;
    C.h = rect_h(px.bounds) * n;
    int vw = C.width ? C.width : hd_scale() ? CLIP_HD_WIDTH : C.w;
    if (vw > C.w)
        vw = C.w; /* never scaled up */
    int vh = (int)lround((double)vw * C.h / C.w / 2) * 2;
    mp4_settings s = {.w = vw,
                      .h = vh,
                      .fps = CLIP_FPS,
                      .in_w = C.w,
                      .in_h = C.h,
                      .quality = C.quality,
                      .video_kbps = C.kbps ? C.kbps : clip_default_kbps(vw, vh),
                      .rate = MIX_RATE,
                      .audio_kbps = CLIP_AUDIO_KBPS};
    char err[256], a[32], how[96], size[64];
    C.mp4 = mp4_open(C.out, &s, err, sizeof err);
    if (!C.mp4)
        fatal("clip: can't write %s: %s", C.out, err);
    if (mp4_at_quality(C.mp4))
        snprintf(how, sizeof how, "quality %.2f", s.quality);
    else
        snprintf(how, sizeof how, "%d kbit/s%s", s.video_kbps,
                 s.quality ? " (this Mac's encoder has no quality setting)" : "");
    if (vw == C.w && vh == C.h)
        snprintf(size, sizeof size, "%dx%d", vw, vh);
    else
        snprintf(size, sizeof size, "%dx%d (scaled from %dx%d)", vw, vh, C.w, C.h);
    log_msg("clip: reached %s after %.1f s; writing to %s, %s at %d fps, %s",
            fmt_ticks(C.start, a, sizeof a), since_launch(), C.out, size, CLIP_FPS, how);
    if (C.early_frames) /* a failure shows at the next frame */
        mp4_audio(C.mp4, C.early, (uint32_t)C.early_frames, (int64_t)C.early_at);
    free(C.early);
    C.early = NULL;
    C.early_frames = C.early_cap = 0;
}

/* Finishes the file. False if it couldn't be written. */
static bool finish(void) {
    if (!C.mp4)
        return true;
    mp4_file *f = C.mp4;
    C.mp4 = NULL;
    char err[256], a[32], b[32];
    if (!mp4_close(f, err, sizeof err)) {
        log_msg("clip: can't write %s: %s", C.out, err);
        return false;
    }
    struct stat st;
    double mb = stat(C.out, &st) == 0 ? (double)st.st_size / 1e6 : 0;
    log_msg("clip: wrote %s-%s to %s: %llu frames, %.1f MB, %.1f s after launch",
            fmt_ticks(C.start, a, sizeof a), fmt_ticks(C.start + (uint32_t)C.frames, b, sizeof b),
            C.out, (unsigned long long)C.frames, mb, since_launch());
    C.wrote = true;
    return true;
}

/* The file of clip i: the base, numbered from 1 before its extension for
   LOONY_CLIP=marks. False if that is too long. */
static bool file_of(size_t i, char *out, size_t cap) {
    if (!C.marks)
        return (size_t)snprintf(out, cap, "%s", C.base) < cap;
    const char *slash = strrchr(C.base, '/'), *name = slash ? slash + 1 : C.base;
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name)
        dot = name + strlen(name);
    return (size_t)snprintf(out, cap, "%.*s-%zu%s", (int)(dot - C.base), C.base, i + 1, dot) < cap;
}

/* Makes clip i the one being made. */
static void begin_span(size_t i) {
    C.cur = i;
    C.start = C.spans[i].start;
    C.end = C.spans[i].end;
    C.next = C.start;
    C.frames = 0;
    C.changed = false;
    file_of(i, C.out, sizeof C.out); /* clip_open checked it fits */
}

/* The program exited before the clip's end: what there is is kept. The
   status says when that is nothing (the recording ended before the clip's
   start: CLIP_NOTHING_STATUS) or the file couldn't be finished (1), unless a
   failure or a desync has said so already. This is the program's first
   atexit handler (clip_open must stay before any other sets one), so it runs
   last and _exit skips none of the program's. */
static void finish_at_exit(void) {
    if (!C.on || C.finished)
        return;
    C.finished = true;
    bool failed = util_failed() || replay_desynced();
    char a[32], b[32];
    if (!C.mp4) {
        if (failed)
            return;
        log_msg("clip: the replay stopped before %s, where the clip starts: nothing written",
                fmt_ticks(C.start, a, sizeof a));
        if (!C.wrote && replay_reached_end()) {
            fflush(NULL);
            _exit(CLIP_NOTHING_STATUS);
        }
        return;
    }
    log_msg("clip: the replay stopped at %s, before the clip's end at %s",
            fmt_ticks(C.next, a, sizeof a), fmt_ticks(C.end, b, sizeof b));
    if (!finish() && !failed) {
        fflush(NULL);
        _exit(1);
    }
}

static _Noreturn void write_failed(void) {
    char err[256];
    mp4_file *f = C.mp4;
    C.mp4 = NULL;
    C.finished = true;
    mp4_close(f, err, sizeof err);
    fatal("clip: can't write %s: %s", C.out, err);
}

/* ---- frames and sound ---- */

/* The picture the window would show now. */
static const uint8_t *screen_rgba(void) {
    qd_pixels px;
    qd_palette pal;
    qd_screen(&px, &pal);
    int w, h;
    const uint8_t *rgba;
    if (hd_scale()) {
        rgba = hd_frame(&px, &w, &h);
    } else {
        w = rect_w(px.bounds);
        h = rect_h(px.bounds);
        if (!C.rgba && !(C.rgba = malloc((size_t)w * (size_t)h * 4)))
            fatal("out of memory");
        if (w == C.w && h == C.h)
            qd_to_rgba(&px, C.rgba);
        rgba = C.rgba;
    }
    if (w != C.w || h != C.h)
        fatal("clip: the screen changed size, from %dx%d to %dx%d", C.w, C.h, w, h);
    return rgba;
}

/* The frame for tick: the screen now if it changed since the last frame and
   now is true, else the last frame again (the first frame is always the
   screen). */
static void add_frame(uint32_t tick, bool now) {
    const uint8_t *rgba = NULL;
    if (!C.frames || (now && C.changed)) {
        rgba = screen_rgba();
        C.changed = false;
    }
    if (!mp4_video(C.mp4, rgba, (int64_t)(tick - C.start)))
        write_failed();
    C.frames++;
}

/* The tick of the clock as the game last read it (reading it again would be
   a step of the replay). */
static uint32_t game_tick(void) {
    uint64_t us = (uint64_t)llround(misc_last_seconds() * 1e6);
    return (uint32_t)(us * 60 / 1000000);
}

void clip_present(bool dirty) {
    if (!C.on || C.finished)
        return;
    if (dirty && hd_scale()) { /* the HD copy kept in step, as the window would keep it */
        qd_pixels px;
        qd_palette pal;
        int w, h;
        qd_screen(&px, &pal);
        hd_frame(&px, &w, &h);
    }
    C.changed |= dirty;
    uint32_t t = game_tick();
    for (;;) {
        if (t < C.start) {
            if (t / 3600 > C.logged) {
                char a[32], b[32];
                C.logged = t / 3600;
                log_msg("clip: replaying, at %s of %s after %.1f s", fmt_ticks(t, a, sizeof a),
                        fmt_ticks(C.start, b, sizeof b), since_launch());
            }
            return;
        }
        if (!C.mp4)
            open_file();
        /* Ticks the clock skipped (nothing presented) show the last frame;
           this tick shows the screen. */
        for (; C.next < C.end && C.next <= t; C.next++)
            add_frame(C.next, C.next == t);
        if (t < C.end)
            return;
        bool ok = finish();
        if (!ok || C.cur + 1 == C.nspans) {
            C.finished = true;
            exit(ok ? 0 : 1);
        }
        begin_span(C.cur + 1);
    }
}

/* The clip being made takes its sound; the first one without a file yet
   (this one, or the next) keeps it until the file opens. */
void clip_sound(const int16_t *samples, uint32_t frames) {
    uint64_t a = C.rendered, b = a + frames;
    C.rendered = b;
    if (!C.on || C.finished)
        return;
    for (size_t i = C.cur; i < C.nspans && i <= C.cur + (C.mp4 != NULL); i++) {
        uint64_t lo = sound_frame_of(C.spans[i].start), hi = sound_frame_of(C.spans[i].end);
        if (b <= lo || a >= hi)
            continue;
        uint64_t from = a > lo ? a : lo, to = b < hi ? b : hi;
        const int16_t *s = samples + 2 * (from - a);
        uint32_t n = (uint32_t)(to - from);
        if (i == C.cur && C.mp4) {
            if (!mp4_audio(C.mp4, s, n, (int64_t)(from - lo)))
                write_failed();
            continue;
        }
        if (!C.early_frames)
            C.early_at = from - lo;
        if (C.early_frames + n > C.early_cap) {
            C.early_cap = (C.early_frames + n) * 2;
            if (!(C.early = realloc(C.early, C.early_cap * 4)))
                fatal("out of memory");
        }
        memcpy(C.early + 2 * C.early_frames, s, (size_t)n * 4);
        C.early_frames += n;
    }
}

/* ---- setup ---- */

bool clip_on(void) { return C.on; }

/* LOONY_CLIP_BEFORE or _AFTER (name), s or the default: seconds, as ticks. */
static bool parse_seconds(const char *name, const char *s, int dflt, uint32_t *ticks, char *err,
                          size_t errlen) {
    double v = dflt;
    if (s && *s) {
        char *e;
        v = strtod(s, &e);
        if (*e || !(v >= 0 && v <= 3600)) {
            snprintf(err, errlen, "%s=%s isn't 0 to 3600 (seconds)", name, s);
            return false;
        }
    }
    *ticks = (uint32_t)lround(v * 60);
    return true;
}

/* A file a clip can go to: not a folder, not the replay, in a folder that
   can be written. */
static bool check_out(const char *path, const char *replay, char *err, size_t errlen) {
    struct stat so, sr;
    bool exists = stat(path, &so) == 0;
    if ((exists && S_ISDIR(so.st_mode)) || path[strlen(path) - 1] == '/') {
        snprintf(err, errlen, "LOONY_CLIP_OUT=%s is a folder, not a file", path);
        return false;
    }
    if (exists && stat(replay, &sr) == 0 && so.st_dev == sr.st_dev && so.st_ino == sr.st_ino) {
        snprintf(err, errlen, "LOONY_CLIP_OUT=%s is the replay (it would be written over)", path);
        return false;
    }
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash)
        snprintf(dir, sizeof dir, ".");
    else
        slash[slash == dir] = '\0'; /* "/x.mp4" is in "/" */
    if (access(dir, W_OK) != 0) {
        snprintf(err, errlen, "LOONY_CLIP_OUT=%s: can't write in %s (%s)", path, dir, strerror(errno));
        return false;
    }
    return true;
}

/* LOONY_CLIP=marks: a clip for each mark in the replay, merged where they
   meet, named from out or else the replay's name. */
static bool open_marks(const char *out, const char *replay, const char *before, const char *after,
                       char *err, size_t errlen) {
    uint32_t b, a;
    if (!parse_seconds("LOONY_CLIP_BEFORE", before, CLIP_MARK_BEFORE, &b, err, errlen) ||
        !parse_seconds("LOONY_CLIP_AFTER", after, CLIP_MARK_AFTER, &a, err, errlen))
        return false;
    if (a + b == 0) {
        snprintf(err, errlen, "LOONY_CLIP_BEFORE and LOONY_CLIP_AFTER can't both be 0");
        return false;
    }
    const uint32_t *marks;
    size_t n = replay_marks(&marks);
    if (!n) {
        snprintf(err, errlen, "the replay %s has no marks (Cmd-S marks one while recording)", replay);
        return false;
    }
    if (!(C.spans = malloc(n * sizeof *C.spans)))
        fatal("out of memory");
    C.nspans = clip_mark_spans(marks, n, b, a, C.spans);
    C.marks = true;
    if (out && *out) {
        snprintf(C.base, sizeof C.base, "%s", out);
    } else {
        size_t len = strlen(replay), ext = strlen(".lwreplay");
        if (len > ext && strcmp(replay + len - ext, ".lwreplay") == 0)
            len -= ext;
        snprintf(C.base, sizeof C.base, "%.*s-clip.mp4", (int)len, replay);
    }
    char t[32];
    for (size_t i = 0; i < n; i++)
        log_msg("clip: mark %zu at tick %u (%s)", i + 1, marks[i], fmt_ticks(marks[i], t, sizeof t));
    return true;
}

bool clip_open(char *err, size_t errlen) {
    const char *span = getenv("LOONY_CLIP"), *out = getenv("LOONY_CLIP_OUT");
    const char *kbps = getenv("LOONY_CLIP_KBPS"), *quality = getenv("LOONY_CLIP_QUALITY");
    const char *width = getenv("LOONY_CLIP_WIDTH"), *replay = getenv("LOONY_REPLAY");
    const char *before = getenv("LOONY_CLIP_BEFORE"), *after = getenv("LOONY_CLIP_AFTER");
    bool around = (before && *before) || (after && *after);
    if (!span || !*span) {
        if ((out && *out) || (kbps && *kbps) || (quality && *quality) || (width && *width) || around) {
            snprintf(err, errlen,
                     "LOONY_CLIP_OUT, LOONY_CLIP_QUALITY, LOONY_CLIP_KBPS, LOONY_CLIP_WIDTH, "
                     "LOONY_CLIP_BEFORE and LOONY_CLIP_AFTER need LOONY_CLIP");
            return false;
        }
        return true;
    }
    if (!replay || !*replay) {
        snprintf(err, errlen, "LOONY_CLIP needs a replay (LOONY_REPLAY)");
        return false;
    }
    if (strcmp(span, "marks") == 0) {
        if (!open_marks(out, replay, before, after, err, errlen))
            return false;
    } else {
        static clip_span one;
        if (!clip_parse_span(span, &one.start, &one.end)) {
            snprintf(err, errlen,
                     "LOONY_CLIP=%s isn't marks or <start>-<end>, each m:ss, h:mm:ss or ticks, "
                     "start first",
                     span);
            return false;
        }
        if (around) {
            snprintf(err, errlen, "LOONY_CLIP_BEFORE and LOONY_CLIP_AFTER are for LOONY_CLIP=marks");
            return false;
        }
        C.spans = &one;
        C.nspans = 1;
        snprintf(C.base, sizeof C.base, "%s", out && *out ? out : "clip.mp4");
    }
    C.quality = CLIP_QUALITY;
    if (kbps && *kbps && quality && *quality) {
        snprintf(err, errlen, "LOONY_CLIP_QUALITY and LOONY_CLIP_KBPS can't be used together");
        return false;
    }
    if (kbps && *kbps) {
        char *e;
        long v = strtol(kbps, &e, 10);
        if (*e || v < 50 || v > 100000) {
            snprintf(err, errlen, "LOONY_CLIP_KBPS=%s isn't 50 to 100000 (kbit/s)", kbps);
            return false;
        }
        C.kbps = (int)v;
        C.quality = 0;
    }
    if (quality && *quality) {
        char *e;
        double v = strtod(quality, &e);
        if (*e || !(v > 0 && v <= 1)) {
            snprintf(err, errlen, "LOONY_CLIP_QUALITY=%s isn't above 0 and at most 1", quality);
            return false;
        }
        C.quality = v;
    }
    if (width && *width) {
        char *e;
        long v = strtol(width, &e, 10);
        if (*e || v < 160 || v > 8192 || v % 2) {
            snprintf(err, errlen, "LOONY_CLIP_WIDTH=%s isn't an even number of pixels, 160 to 8192",
                     width);
            return false;
        }
        C.width = (int)v;
    }
    char path[PATH_MAX];
    for (size_t i = 0; i < C.nspans; i++) {
        if (!file_of(i, path, sizeof path)) {
            snprintf(err, errlen, "LOONY_CLIP_OUT is longer than %d characters", PATH_MAX - 1);
            return false;
        }
        if (!check_out(path, replay, err, errlen))
            return false;
    }
    const char *scale = getenv("LOONY_HD_SCALE");
    if (!scale || !*scale)
        setenv("LOONY_HD_SCALE", "2", 1);
    begin_span(0);
    C.on = true;
    clock_gettime(CLOCK_MONOTONIC, &C.t0);
    sound_set_tap(clip_sound);
    atexit(finish_at_exit);
    char a[32], b[32];
    for (size_t i = 0; i < C.nspans; i++) {
        const clip_span *c = &C.spans[i];
        file_of(i, path, sizeof path);
        fmt_ticks(c->start, a, sizeof a);
        fmt_ticks(c->end, b, sizeof b);
        if (!C.marks)
            log_msg("clip: replaying to %s, then writing %s-%s to %s", a, a, b, path);
        else if (c->first_mark == c->last_mark)
            log_msg("clip: %s-%s (mark %d) to %s", a, b, c->first_mark, path);
        else
            log_msg("clip: %s-%s (marks %d to %d) to %s", a, b, c->first_mark, c->last_mark, path);
    }
    return true;
}
