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

/* A stretch of the game's clock in a clip: the ticks [start, end), at ticks
   into its clip's file. A clip is a piece of each span of play it covers
   (replay_play_spans), so what isn't play is left out. */
typedef struct {
    uint32_t start, end, at;
    size_t clip;
} piece;

static struct {
    bool on, finished;
    clip_span *spans; /* the clips to make, in order, in play time */
    size_t nspans;
    piece *pieces; /* and their pieces, in order */
    size_t npieces, cur;
    bool named;          /* LOONY_CLIP=marks or clips: the files are named for their spans */
    char base[PATH_MAX]; /* the file, or what the named ones start with */
    char ext[32];        /* and end with */
    size_t clip;         /* the clip being made */
    uint32_t start, end, at; /* and the piece of it being made */
    char out[PATH_MAX];  /* and its file */
    char part[PATH_MAX + 2]; /* and where it is written until it is finished */
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

const char *clip_format_time(uint32_t ticks, char *buf, size_t cap) {
    uint32_t s = ticks / 60, rem = ticks % 60;
    int n = s >= 3600 ? snprintf(buf, cap, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60)
                      : snprintf(buf, cap, "%u:%02u", s / 60, s % 60);
    if (rem && n > 0 && (size_t)n < cap)
        snprintf(buf + n, cap - (size_t)n, ".%02u", rem * 100 / 60);
    return buf;
}

/* One end of a span in a file's name: clip_format_time's, with letters
   for the colons. */
static int name_time(uint32_t ticks, char *buf, size_t cap) {
    uint32_t s = ticks / 60, rem = ticks % 60;
    int n = s >= 3600 ? snprintf(buf, cap, "%uh%02um%02u", s / 3600, s / 60 % 60, s % 60)
                      : snprintf(buf, cap, "%um%02u", s / 60, s % 60);
    if (rem && n > 0 && (size_t)n < cap)
        n += snprintf(buf + n, cap - (size_t)n, ".%02u", rem * 100 / 60);
    return n;
}

const char *clip_span_name(uint32_t start, uint32_t end, char *buf, size_t cap) {
    int n = name_time(start, buf, cap);
    if (n > 0 && (size_t)n + 1 < cap) {
        buf[n] = '-';
        name_time(end, buf + n + 1, cap - (size_t)n - 1);
    }
    return buf;
}

/* ---- the file ---- */

/* 1.2 Mbit/s at 1600x1200, about what CLIP_QUALITY comes to there (Apple's
   encoder holds its quality on fewer bits than it needs at a set bitrate),
   scaled with the picture's side. */
bool clip_list_path(const char *replay, char *out, size_t cap) {
    size_t len = strlen(replay), ext = strlen(".lwreplay");
    if (len > ext && strcmp(replay + len - ext, ".lwreplay") == 0)
        len -= ext;
    return (size_t)snprintf(out, cap, "%.*s.clips", (int)len, replay) < cap;
}

bool clip_parse_list(const char *text, clip_span **spans, size_t *n, char *err, size_t errlen) {
    size_t cap = 1;
    for (const char *p = text; *p; p++)
        cap += *p == '\n';
    if (!(*spans = malloc(cap * sizeof **spans)))
        fatal("out of memory");
    *n = 0;
    int line_no = 0;
    for (const char *p = text; *p;) {
        const char *eol = strchr(p, '\n');
        if (!eol)
            eol = p + strlen(p);
        line_no++;
        const char *a = p, *b = eol;
        while (a < b && (*a == ' ' || *a == '\t'))
            a++;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r'))
            b--;
        p = *eol ? eol + 1 : eol;
        if (a == b || *a == '#')
            continue;
        char span[64];
        clip_span *s = &(*spans)[*n];
        if ((size_t)(b - a) >= sizeof span) {
            snprintf(err, errlen, "line %d is too long for a span", line_no);
            goto bad;
        }
        memcpy(span, a, (size_t)(b - a));
        span[b - a] = '\0';
        if (!clip_parse_span(span, &s->start, &s->end)) {
            snprintf(err, errlen, "line %d: %s isn't <start>-<end>, each m:ss, h:mm:ss or ticks, start first",
                     line_no, span);
            goto bad;
        }
        s->first_mark = s->last_mark = 0;
        (*n)++;
    }
    return true;
bad:
    free(*spans);
    *spans = NULL;
    *n = 0;
    return false;
}

static int by_start(const void *a, const void *b) {
    const clip_span *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

size_t clip_merge_spans(clip_span *spans, size_t n) {
    qsort(spans, n, sizeof *spans, by_start);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (k && spans[i].start <= spans[k - 1].end) {
            if (spans[i].end > spans[k - 1].end)
                spans[k - 1].end = spans[i].end;
            continue;
        }
        spans[k++] = spans[i];
    }
    return k;
}

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
    C.mp4 = mp4_open(C.part, &s, err, sizeof err);
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
            clip_format_time(C.spans[C.clip].start, a, sizeof a), since_launch(), C.out, size, CLIP_FPS,
            how);
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
        unlink(C.part);
        return false;
    }
    struct stat st;
    double mb = stat(C.part, &st) == 0 ? (double)st.st_size / 1e6 : 0;
    if (rename(C.part, C.out) != 0) {
        log_msg("clip: can't move %s to %s: %s", C.part, C.out, strerror(errno));
        unlink(C.part);
        return false;
    }
    log_msg("clip: wrote %s-%s to %s: %llu frames, %.1f MB, %.1f s after launch",
            clip_format_time(C.spans[C.clip].start, a, sizeof a),
            clip_format_time(C.spans[C.clip].start + (uint32_t)C.frames, b, sizeof b),
            C.out, (unsigned long long)C.frames, mb, since_launch());
    C.wrote = true;
    return true;
}

/* The file of clip i: the base, or for LOONY_CLIP=marks and clips, the
   base, its span and the extension. False if that is too long. */
static bool file_of(size_t i, char *out, size_t cap) {
    if (!C.named)
        return (size_t)snprintf(out, cap, "%s", C.base) < cap;
    char span[64];
    clip_span_name(C.spans[i].start, C.spans[i].end, span, sizeof span);
    return (size_t)snprintf(out, cap, "%s-%s%s", C.base, span, C.ext) < cap;
}

/* The hidden file beside path that a clip is written to: .<name>, keeping
   the extension the encoder goes by. */
static void part_of(const char *path, char *out, size_t cap) {
    const char *slash = strrchr(path, '/');
    if (slash)
        snprintf(out, cap, "%.*s.%s", (int)(slash + 1 - path), path, slash + 1);
    else
        snprintf(out, cap, ".%s", path);
}

/* Makes piece i the one being made, and its clip, if it starts one. */
static void begin_piece(size_t i) {
    const piece *p = &C.pieces[i];
    bool first = i == 0 || C.pieces[i - 1].clip != p->clip;
    C.cur = i;
    C.clip = p->clip;
    C.start = p->start;
    C.end = p->end;
    C.at = p->at;
    C.next = C.start;
    if (first) {
        C.frames = 0;
        C.changed = false;
        file_of(p->clip, C.out, sizeof C.out); /* clip_open checked it fits */
        part_of(C.out, C.part, sizeof C.part);
    }
}

/* Closes the clip's file and removes it, so nothing of the clip is left. err
   says why the close failed, if it did. */
static void discard(char *err, size_t cap) {
    mp4_file *f = C.mp4;
    C.mp4 = NULL;
    mp4_close(f, err, cap);
    unlink(C.part);
}

/* The program exited before the clip's end: what there is is kept when the
   recording ended there too (replay_reached_end, which the replay's own
   handler sets before this one runs), and thrown away when it goes on (a
   desync, a failure, LOONY_EXIT_AFTER), so a later export renders the clip
   instead of skipping it. The status says when nothing was written (the
   recording ended before the clip's start: CLIP_NOTHING_STATUS) or the clip
   was thrown away or couldn't be finished (1), unless a failure or a desync
   has said so already. This is the program's first atexit handler
   (clip_open must stay before any other sets one), so it runs last and
   _exit skips none of the program's. */
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
                clip_format_time(C.spans[C.clip].start, a, sizeof a));
        if (!C.wrote && replay_reached_end()) {
            fflush(NULL);
            _exit(CLIP_NOTHING_STATUS);
        }
        return;
    }
    log_msg("clip: the replay stopped at %s, before the clip's end at %s",
            clip_format_time(C.spans[C.clip].start + (uint32_t)C.frames, a, sizeof a),
            clip_format_time(C.spans[C.clip].end, b, sizeof b));
    bool kept;
    if (replay_reached_end()) {
        kept = finish();
    } else {
        char err[256];
        discard(err, sizeof err);
        log_msg("clip: %s isn't written: the recording goes on past where the replay stopped", C.out);
        kept = false;
    }
    if (!kept && !failed) {
        fflush(NULL);
        _exit(1);
    }
}

static _Noreturn void write_failed(void) {
    char err[256];
    C.finished = true;
    discard(err, sizeof err);
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
    if (!mp4_video(C.mp4, rgba, (int64_t)(tick - C.start + C.at)))
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
            if (!C.mp4 && t / 3600 > C.logged) {
                char a[32], b[32];
                C.logged = t / 3600;
                log_msg("clip: replaying, at %s of %s on the game's clock after %.1f s",
                        clip_format_time(t, a, sizeof a), clip_format_time(C.start, b, sizeof b),
                        since_launch());
            }
            return; /* before the clip, or where it leaves out a pause */
        }
        if (!C.mp4)
            open_file();
        /* Ticks the clock skipped (nothing presented) show the last frame;
           this tick shows the screen. */
        for (; C.next < C.end && C.next <= t; C.next++)
            add_frame(C.next, C.next == t);
        if (t < C.end)
            return;
        if (C.cur + 1 == C.npieces || C.pieces[C.cur + 1].clip != C.clip) {
            bool ok = finish();
            if (!ok || C.cur + 1 == C.npieces) {
                C.finished = true;
                exit(ok ? 0 : 1);
            }
        }
        begin_piece(C.cur + 1);
    }
}

/* The clip being made takes its sound; the first one without a file yet
   (this one, or the next) keeps it until the file opens. */
void clip_sound(const int16_t *samples, uint32_t frames) {
    uint64_t a = C.rendered, b = a + frames;
    C.rendered = b;
    if (!C.on || C.finished)
        return;
    for (size_t i = C.cur; i < C.npieces && i <= C.cur + (C.mp4 != NULL); i++) {
        const piece *p = &C.pieces[i];
        uint64_t lo = sound_frame_of(p->start), hi = sound_frame_of(p->end);
        if (b <= lo || a >= hi)
            continue;
        uint64_t from = a > lo ? a : lo, to = b < hi ? b : hi;
        const int16_t *s = samples + 2 * (from - a);
        uint32_t n = (uint32_t)(to - from);
        uint64_t at = from - lo + sound_frame_of(p->at);
        if (p->clip == C.clip && C.mp4) {
            if (!mp4_audio(C.mp4, s, n, (int64_t)at))
                write_failed();
            continue;
        }
        if (!C.early_frames)
            C.early_at = at;
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

/* What strip says at the end of the first len bytes of s: len without it. */
static size_t without(const char *s, size_t len, const char *strip) {
    size_t n = strlen(strip);
    return len > n && strncmp(s + len - n, strip, n) == 0 ? len - n : len;
}

/* The clips' files are named from out (its extension after the span), or
   else the replay, without .lwreplay and -marked (a recording is renamed
   when it is marked). */
static void named_base(const char *out, const char *replay) {
    C.named = true;
    if (out && *out) {
        const char *slash = strrchr(out, '/'), *name = slash ? slash + 1 : out;
        const char *dot = strrchr(name, '.');
        if (!dot || dot == name || strlen(dot) >= sizeof C.ext)
            dot = name + strlen(name);
        snprintf(C.base, sizeof C.base, "%.*s", (int)(dot - out), out);
        snprintf(C.ext, sizeof C.ext, "%s", dot);
    } else {
        size_t len = without(replay, strlen(replay), ".lwreplay");
        len = without(replay, len, "-marked");
        snprintf(C.base, sizeof C.base, "%.*s", (int)len, replay);
        snprintf(C.ext, sizeof C.ext, ".mp4");
    }
}

/* Leaves out the clips whose files are there already, and exits if that is
   all of them. */
static void skip_written(void) {
    char path[PATH_MAX], a[32], b[32];
    size_t kept = 0;
    for (size_t i = 0; i < C.nspans; i++) {
        file_of(i, path, sizeof path); /* clip_open checked it fits */
        if (access(path, F_OK) == 0) {
            log_msg("clip: %s-%s is in %s already: skipped", clip_format_time(C.spans[i].start, a, sizeof a),
                    clip_format_time(C.spans[i].end, b, sizeof b), path);
            continue;
        }
        C.spans[kept++] = C.spans[i];
    }
    if (!kept) {
        if (C.nspans == 1)
            log_msg("clip: the clip is written already");
        else
            log_msg("clip: all %zu clips are written already", C.nspans);
        exit(0);
    }
    C.nspans = kept;
}

/* LOONY_CLIP=clips: a clip for each span in the replay's list. */
static bool open_list(const char *out, const char *replay, char *err, size_t errlen) {
    char path[PATH_MAX];
    if (!clip_list_path(replay, path, sizeof path)) {
        snprintf(err, errlen, "LOONY_REPLAY is longer than %d characters", PATH_MAX - 16);
        return false;
    }
    size_t len;
    char *text = (char *)read_file(path, &len);
    if (!text) {
        snprintf(err, errlen, "can't read the clips of %s from %s: %s (LOONY_WATCH=1 saves them)",
                 replay, path, strerror(errno));
        return false;
    }
    char why[200];
    bool ok = clip_parse_list(text, &C.spans, &C.nspans, why, sizeof why);
    free(text);
    if (!ok) {
        snprintf(err, errlen, "%s, %s", path, why);
        return false;
    }
    if (!C.nspans) {
        snprintf(err, errlen, "%s has no clips", path);
        free(C.spans);
        C.spans = NULL;
        return false;
    }
    size_t listed = C.nspans;
    C.nspans = clip_merge_spans(C.spans, C.nspans);
    named_base(out, replay);
    log_msg("clip: %zu listed in %s%s", listed, path, C.nspans < listed ? ", some overlapping" : "");
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
    uint32_t *played = malloc(n * sizeof *played); /* the marks in play time */
    if (!played || !(C.spans = malloc(n * sizeof *C.spans)))
        fatal("out of memory");
    for (size_t i = 0; i < n; i++)
        played[i] = replay_play_ticks(marks[i]);
    C.nspans = clip_mark_spans(played, n, b, a, C.spans);
    named_base(out, replay);
    char t[32];
    for (size_t i = 0; i < n; i++)
        log_msg("clip: mark %zu at tick %u (%s)", i + 1, played[i], clip_format_time(played[i], t, sizeof t));
    free(played);
    return true;
}

/* tick's start in us, rounded up so it converts back to the same tick. */
static uint64_t us_of(uint32_t tick) { return ((uint64_t)tick * 1000000 + 59) / 60; }
static uint32_t tick_of(uint64_t us) { return (uint32_t)(us * 60 / 1000000); }

/* The clips' pieces: where each one's play time falls on the game's clock.
   Play past the last span is the clock after it, so a clip there runs to
   the end of the recording as it would without spans. */
static void make_pieces(void) {
    const replay_span *spans;
    size_t nspans = replay_play_spans(&spans), cap = C.nspans * (nspans ? nspans : 1);
    if (!(C.pieces = malloc(cap * sizeof *C.pieces)))
        fatal("out of memory");
    C.npieces = 0;
    for (size_t i = 0; i < C.nspans; i++) {
        const clip_span *c = &C.spans[i];
        if (!nspans) {
            C.pieces[C.npieces++] = (piece){c->start, c->end, 0, i};
            continue;
        }
        uint64_t from = us_of(c->start), to = us_of(c->end), play = 0;
        uint32_t at = 0;
        for (size_t k = 0; k < nspans; k++) {
            uint64_t on = spans[k].on_us;
            uint64_t len = k + 1 == nspans ? UINT64_MAX - on : spans[k].off_us - on;
            uint64_t lo = from > play ? from : play, hi = to < play + len ? to : play + len;
            if (lo < hi) {
                uint32_t start = tick_of(on + (lo - play)), end = tick_of(on + (hi - play));
                if (end > start) {
                    C.pieces[C.npieces++] = (piece){start, end, at, i};
                    at += end - start;
                }
            }
            play += len;
        }
    }
}

/* How many pieces clip i has. */
static size_t pieces_of(size_t clip) {
    size_t n = 0;
    for (size_t k = 0; k < C.npieces; k++)
        n += C.pieces[k].clip == clip;
    return n;
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
    } else if (strcmp(span, "clips") == 0) {
        if (around) {
            snprintf(err, errlen, "LOONY_CLIP_BEFORE and LOONY_CLIP_AFTER are for LOONY_CLIP=marks");
            return false;
        }
        if (!open_list(out, replay, err, errlen))
            return false;
    } else {
        static clip_span one;
        if (!clip_parse_span(span, &one.start, &one.end)) {
            snprintf(err, errlen,
                     "LOONY_CLIP=%s isn't marks, clips or <start>-<end>, each m:ss, h:mm:ss or ticks, "
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
    if (C.named)
        skip_written();
    const char *scale = getenv("LOONY_HD_SCALE");
    if (!scale || !*scale)
        setenv("LOONY_HD_SCALE", "2", 1);
    make_pieces();
    char a[32], b[32];
    for (size_t i = 0; i < C.nspans; i++) {
        if (!pieces_of(i)) { /* its play rounds away to nothing */
            clip_format_time(C.spans[i].start, a, sizeof a);
            clip_format_time(C.spans[i].end, b, sizeof b);
            snprintf(err, errlen, "the clip %s-%s holds less than a tick of play", a, b);
            return false;
        }
    }
    begin_piece(0);
    C.on = true;
    clock_gettime(CLOCK_MONOTONIC, &C.t0);
    sound_set_tap(clip_sound);
    atexit(finish_at_exit);
    for (size_t i = 0; i < C.nspans; i++) {
        const clip_span *c = &C.spans[i];
        file_of(i, path, sizeof path);
        clip_format_time(c->start, a, sizeof a);
        clip_format_time(c->end, b, sizeof b);
        size_t pieces = pieces_of(i);
        if (pieces > 1)
            log_msg("clip: %s-%s leaves out %zu stretches that aren't play", a, b, pieces - 1);
        if (!C.named)
            log_msg("clip: replaying to %s, then writing %s-%s to %s", a, a, b, path);
        else if (!c->first_mark)
            log_msg("clip: %s-%s to %s", a, b, path);
        else if (c->first_mark == c->last_mark)
            log_msg("clip: %s-%s (mark %d) to %s", a, b, c->first_mark, path);
        else
            log_msg("clip: %s-%s (marks %d to %d) to %s", a, b, c->first_mark, c->last_mark, path);
    }
    return true;
}
