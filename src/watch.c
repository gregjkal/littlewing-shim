#include "watch.h"

#include <SDL3/SDL.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "clip.h"
#include "display.h"
#include "hd.h"
#include "qd.h"
#include "replay.h"
#include "sound.h"
#include "util.h"

/* The speeds, slowest first; 0 is as fast as it can. */
static const int speeds[] = {1, 2, 4, 8, 0};
#define NSPEEDS (int)(sizeof speeds / sizeof speeds[0])

/* Seconds of wall clock between draws: while going to a time, and above 2x,
   where the game draws faster than the display shows. */
#define GOING_DRAW_GAP 0.25
#define FAST_DRAW_GAP (1.0 / 60)
/* More than this behind the wall clock, a frame isn't drawn: a cut scene
   presents up to twice a frame, more than a window can show, and drawing
   each would leave the replay further behind. The latest is drawn once it
   has caught up, or after BEHIND_DRAW_GAP without one: a game that runs
   slower than the speed asked (HD at 8x) never catches up. */
#define BEHIND_DRAW_LAG (1.0 / 60)
#define BEHIND_DRAW_GAP GOING_DRAW_GAP
/* While waiting (paused, held at the end, or a wait longer than this): how
   often the window is polled, in seconds. */
#define IDLE_GAP 0.01

static struct {
    bool on, started, at_end;
    const char *title;
    void (*restart)(void);
    int speed; /* in speeds */
    bool paused;
    bool going; /* to from, without pacing or drawing every frame */
    uint32_t from;
    bool skipping; /* what isn't play, without pacing, drawing or sound */
    bool have_in;
    uint32_t in;
    char list[PATH_MAX]; /* the clips' list */
    /* Times shown and keyed (from, in, the list) are in play time; the
       replay's own clock is clock_us (replay_play_us maps one to the
       other). */
    uint64_t clock_us;   /* the replay's clock as the game has seen it */
    /* Pacing: the wall clock was anchor_wall when the clock was anchor_us. */
    double anchor_wall;
    uint64_t anchor_us;
    double paced_wall; /* the wall clock when the clock last moved */
    unsigned minutes;  /* whole minutes of the clock logged since the anchor */
    bool pending;      /* the screen changed and isn't drawn yet */
    double drawn, polled;
    char text[256]; /* the window's title */
} W;

void watch_reset(void) { memset(&W, 0, sizeof W); }

bool watch_on(void) { return W.on; }

static double wall(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_for(double seconds) {
    struct timespec ts = {(time_t)seconds, (long)((seconds - (double)(time_t)seconds) * 1e9)};
    nanosleep(&ts, NULL);
}

static uint32_t tick_of(uint64_t us) { return (uint32_t)(us * 60 / 1000000); }

/* Play time: the replay's clock less what isn't play. */
static uint32_t play_tick_of(uint64_t clock_us) { return tick_of(replay_play_us(clock_us)); }

static uint32_t now_tick(void) { return play_tick_of(W.clock_us); }

/* ticks as m:ss, in whole seconds. */
static const char *fmt_seconds(uint32_t ticks, char *buf, size_t cap) {
    return clip_format_time(ticks - ticks % 60, buf, cap);
}

/* ---- the window ---- */

static void update_title(void) {
    char text[sizeof W.text], t[32], u[32], in[48] = "";
    const char *title = W.title ? W.title : "LittleWing";
    fmt_seconds(now_tick(), t, sizeof t);
    if (W.have_in)
        snprintf(in, sizeof in, ", clip from %s", fmt_seconds(W.in, u, sizeof u));
    const replay_span *spans;
    if (W.going && replay_play_spans(&spans) && W.clock_us < spans[0].on_us)
        snprintf(text, sizeof text, "%s: replaying what came before the game, %s of %s", title,
                 fmt_seconds(tick_of(W.clock_us), t, sizeof t),
                 fmt_seconds(tick_of(spans[0].on_us), u, sizeof u));
    else if (W.going)
        snprintf(text, sizeof text, "%s: going to %s (at %s)%s", title,
                 fmt_seconds(W.from, u, sizeof u), t, in);
    else if (W.at_end)
        snprintf(text, sizeof text, "%s: %s, the end%s", title, t, in);
    else if (W.paused)
        snprintf(text, sizeof text, "%s: %s, paused%s", title, t, in);
    else if (speeds[W.speed])
        snprintf(text, sizeof text, "%s: %s at %dx%s", title, t, speeds[W.speed], in);
    else
        snprintf(text, sizeof text, "%s: %s, fastest%s", title, t, in);
    if (strcmp(text, W.text) != 0) {
        snprintf(W.text, sizeof W.text, "%s", text);
        display_set_title(W.text);
    }
}

static void flash(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void flash(const char *fmt, ...) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    display_flash(msg);
}

static void draw(void) {
    display_present();
    W.pending = false;
    W.drawn = wall();
}

bool watch_draws(double since, double lag, int speed, bool going) {
    double gap = going ? GOING_DRAW_GAP : speed == 1 || speed == 2 ? 0 : FAST_DRAW_GAP;
    bool behind = !going && speed && lag > BEHIND_DRAW_LAG;
    return since >= gap && (!behind || since >= BEHIND_DRAW_GAP);
}

void watch_present(bool dirty) {
    if (!W.started)
        return;
    W.pending |= dirty;
    if (!W.pending)
        return;
    int speed = speeds[W.speed];
    double now = wall();
    double lag = W.paused || !speed
                     ? 0
                     : now - (W.anchor_wall + (double)(W.clock_us - W.anchor_us) / 1e6 / speed);
    if (!W.skipping && watch_draws(now - W.drawn, lag, speed, W.going)) {
        draw();
    } else if (dirty && hd_scale()) { /* the HD copy kept in step, as when every frame is drawn */
        qd_pixels px;
        qd_palette pal;
        int w, h;
        qd_screen(&px, &pal);
        hd_frame(&px, &w, &h);
    }
}

/* Waiting: the window stays live (and opens, if nothing was drawn yet). */
static void idle(void) {
    display_poll();
    if (W.pending || !W.drawn)
        draw();
    update_title();
    sleep_for(IDLE_GAP);
}

/* ---- pacing ---- */

static bool sound_on(void) {
    return speeds[W.speed] == 1 && !W.paused && !W.going && !W.skipping && !W.at_end;
}

/* The wall clock and the replay's clock line up from now. */
static void anchor(void) {
    W.anchor_wall = W.paced_wall = wall();
    W.anchor_us = W.clock_us;
    W.minutes = 0;
    sound_monitor(sound_on());
}

/* At 1x: how long the game's time since the anchor took on the wall clock. */
static void log_run(const char *what) {
    if (speeds[W.speed] != 1 || W.clock_us <= W.anchor_us)
        return;
    double game = (double)(W.clock_us - W.anchor_us) / 1e6, took = W.paced_wall - W.anchor_wall;
    char a[32], b[32];
    log_msg("watch: %s, %s-%s at 1x, took %.3f s on the wall clock (%+.1f ms)", what,
            clip_format_time(play_tick_of(W.anchor_us), a, sizeof a),
            clip_format_time(now_tick(), b, sizeof b), took, (took - game) * 1000);
}

/* Going or skipping: the window stays live, a few times a second. */
static void poll_now_and_then(void) {
    if (wall() - W.polled >= GOING_DRAW_GAP / 4) {
        W.polled = wall();
        display_poll();
        update_title();
    }
}

/* The replay's clock is about to move to clock_us: waits until the wall
   clock has reached it at this speed. What isn't play goes by unpaced. */
static void pace(uint64_t clock_us) {
    uint64_t next_on;
    bool in_play = replay_in_play(clock_us, &next_on);
    if (W.going) {
        W.clock_us = clock_us;
        if (!in_play || play_tick_of(clock_us) < W.from) {
            poll_now_and_then();
            return;
        }
        W.going = W.skipping = false;
        char t[32];
        log_msg("watch: reached %s", clip_format_time(W.from, t, sizeof t));
        anchor();
        update_title();
        return;
    }
    if (!in_play) {
        if (!W.skipping) {
            char t[32];
            W.skipping = true;
            log_run("the last stretch");
            log_msg("watch: leaving out what isn't play from %s", clip_format_time(now_tick(), t, sizeof t));
            sound_monitor(false);
        }
        W.clock_us = clock_us;
        poll_now_and_then();
        return;
    }
    if (W.skipping) { /* play again: paced from here */
        W.skipping = false;
        W.clock_us = clock_us;
        anchor();
    }
    for (;;) {
        if (W.going) /* L while waiting: from the next clock on; paused or not, it goes */
            break;
        if (W.paused) {
            idle();
            continue;
        }
        int speed = speeds[W.speed];
        double now = wall();
        if (!speed) {
            W.anchor_wall = now;
            W.anchor_us = clock_us;
            break;
        }
        double at = W.anchor_wall + (double)(clock_us - W.anchor_us) / 1e6 / speed;
        if (now >= at) {
            if (now - at > WATCH_MAX_LAG) {
                char t[32];
                if (speed == 1)
                    log_msg("watch: %.2f s behind at %s; going on from there", now - at,
                            clip_format_time(play_tick_of(clock_us), t, sizeof t));
                W.clock_us = clock_us;
                anchor();
            }
            break;
        }
        if (at - now > 2 * IDLE_GAP) {
            display_poll(); /* a key can change the speed or pause */
            sleep_for(IDLE_GAP);
        } else {
            sleep_for(at - now);
        }
    }
    W.clock_us = clock_us;
    W.paced_wall = wall();
    if (speeds[W.speed] == 1 && (W.clock_us - W.anchor_us) / 60000000 > W.minutes) {
        W.minutes++;
        log_run("a minute");
    }
    update_title();
}

/* ---- keys ---- */

static void set_speed(int i) {
    W.speed = i;
    anchor();
    if (speeds[i])
        log_msg("watch: %dx", speeds[i]);
    else
        log_msg("watch: as fast as it can");
}

/* Goes back to tick: restarts the program, which goes there. */
static _Noreturn void go_back(uint32_t tick) {
    char v[32], t[32];
    log_run("the last stretch");
    log_msg("watch: back to %s: restarting", clip_format_time(tick, t, sizeof t));
    snprintf(v, sizeof v, "%u", tick);
    setenv("LOONY_WATCH_FROM", v, 1);
    snprintf(v, sizeof v, "%d", speeds[W.speed]);
    setenv("LOONY_WATCH_SPEED", v, 1);
    if (W.paused)
        setenv("LOONY_WATCH_PAUSED", "1", 1);
    else
        unsetenv("LOONY_WATCH_PAUSED");
    if (W.have_in) {
        snprintf(v, sizeof v, "%u", W.in);
        setenv("LOONY_WATCH_IN", v, 1);
    } else {
        unsetenv("LOONY_WATCH_IN");
    }
    unsetenv("LOONY_SCRIPT");
    sound_monitor(false);
    W.restart();
    fatal("watch: can't restart to go back");
}

/* J and L: by step ticks back (step < 0) or ahead, from the time going to
   while going. */
static void step_by(int64_t step) {
    int64_t to = (int64_t)(W.going ? W.from : now_tick()) + step;
    uint32_t tick = to < 0 ? 0 : to > UINT32_MAX ? UINT32_MAX : (uint32_t)to;
    if (tick < now_tick()) {
        if (W.restart)
            go_back(tick);
        return;
    }
    if (W.at_end) /* nothing ahead */
        return;
    char t[32];
    log_msg("watch: going to %s", clip_format_time(tick, t, sizeof t));
    W.from = tick;
    W.going = tick > now_tick();
    if (!W.going)
        anchor();
    sound_monitor(sound_on());
}

/* The clips in the list, or -1 if it can't be read. */
static int clips_in_list(void) {
    size_t len;
    char *text = (char *)read_file(W.list, &len);
    if (!text)
        return -1;
    clip_span *spans;
    size_t n;
    char err[256];
    int count = clip_parse_list(text, &spans, &n, err, sizeof err) ? (int)n : -1;
    free(text);
    if (count >= 0)
        free(spans);
    return count;
}

/* O: the clip from I to now goes into the list. */
static void save_clip(void) {
    if (!W.have_in) {
        flash("I marks where a clip starts, then O where it ends");
        return;
    }
    uint32_t out = now_tick();
    char a[32], b[32];
    clip_format_time(W.in, a, sizeof a);
    clip_format_time(out, b, sizeof b);
    if (out <= W.in) {
        flash("the clip would end at %s, before it starts at %s", b, a);
        return;
    }
    FILE *f = fopen(W.list, "a");
    bool ok = f && fprintf(f, "%s-%s\n", a, b) > 0;
    if (f && fclose(f) != 0)
        ok = false;
    if (!ok) {
        log_msg("watch: can't write %s: %s", W.list, strerror(errno));
        flash("can't save the clip (%s)", strerror(errno));
        return;
    }
    W.have_in = false;
    int n = clips_in_list();
    log_msg("watch: clip %d, %s-%s, saved in %s", n, a, b, W.list);
    flash("clip %d saved: %s-%s", n, a, b);
}

void watch_key(int scancode, bool down, bool repeat) {
    if (!W.started || !down)
        return;
    bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
    int64_t step = shift ? WATCH_BIG_STEP_TICKS : WATCH_STEP_TICKS;
    char t[32];
    switch (scancode) {
    case SDL_SCANCODE_SPACE:
    case SDL_SCANCODE_K: /* as on YouTube */
        if (repeat || W.at_end)
            break;
        W.paused = !W.paused;
        log_msg("watch: %s at %s", W.paused ? "paused" : "going on",
                clip_format_time(now_tick(), t, sizeof t));
        if (W.paused)
            log_run("the last stretch");
        anchor();
        break;
    case SDL_SCANCODE_RIGHT:
        if (!repeat && W.speed + 1 < NSPEEDS)
            set_speed(W.speed + 1);
        break;
    case SDL_SCANCODE_LEFT:
        if (!repeat && W.speed > 0)
            set_speed(W.speed - 1);
        break;
    case SDL_SCANCODE_I:
        if (repeat)
            break;
        W.in = now_tick();
        W.have_in = true;
        log_msg("watch: a clip starts at %s", clip_format_time(W.in, t, sizeof t));
        flash("a clip starts at %s: O ends it", t);
        break;
    case SDL_SCANCODE_O:
        if (!repeat)
            save_clip();
        break;
    case SDL_SCANCODE_J:
        step_by(-step);
        break;
    case SDL_SCANCODE_L:
        step_by(step);
        break;
    default:
        break;
    }
    update_title();
}

/* ---- setup ---- */

/* name's value, a whole number up to max, or dflt when unset. */
static bool env_number(const char *name, uint32_t max, uint32_t dflt, uint32_t *out, char *err,
                       size_t errlen) {
    const char *s = getenv(name);
    *out = dflt;
    if (!s || !*s)
        return true;
    char *e;
    errno = 0;
    unsigned long v = strtoul(s, &e, 10);
    if (*e || errno || v > max || s[0] == '-') {
        snprintf(err, errlen, "%s=%s isn't a whole number up to %u", name, s, max);
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

bool watch_open(char *err, size_t errlen) {
    watch_reset();
    const char *on = getenv("LOONY_WATCH"), *replay = getenv("LOONY_REPLAY");
    if (!on || strcmp(on, "1") != 0)
        return true;
    if (!replay || !*replay) {
        snprintf(err, errlen, "LOONY_WATCH needs a replay (LOONY_REPLAY)");
        return false;
    }
    if (clip_on()) {
        snprintf(err, errlen, "LOONY_WATCH and LOONY_CLIP can't be used together");
        return false;
    }
    uint32_t speed, in;
    if (!env_number("LOONY_WATCH_FROM", UINT32_MAX, 0, &W.from, err, errlen) ||
        !env_number("LOONY_WATCH_SPEED", 8, 1, &speed, err, errlen) ||
        !env_number("LOONY_WATCH_IN", UINT32_MAX, UINT32_MAX, &in, err, errlen))
        return false;
    W.speed = -1;
    for (int i = 0; i < NSPEEDS; i++)
        if ((uint32_t)speeds[i] == speed)
            W.speed = i;
    if (W.speed < 0) {
        snprintf(err, errlen, "LOONY_WATCH_SPEED=%u isn't 1, 2, 4, 8 or 0 (as fast as it can)", speed);
        return false;
    }
    W.have_in = in != UINT32_MAX;
    W.in = W.have_in ? in : 0;
    const char *paused = getenv("LOONY_WATCH_PAUSED");
    W.paused = paused && strcmp(paused, "1") == 0;
    uint64_t next_on;
    W.going = W.from > 0 || !replay_in_play(0, &next_on); /* to the game's start */
    if (!clip_list_path(replay, W.list, sizeof W.list)) {
        snprintf(err, errlen, "LOONY_REPLAY is longer than %d characters", PATH_MAX - 16);
        return false;
    }
    W.on = true;
    return true;
}

static void log_at_exit(void) {
    if (!W.paused && !W.going && !W.skipping && !W.at_end) /* logged already */
        log_run("the last stretch");
}

static void held_at_end(void) { watch_hold(); }

void watch_start(const char *title, void (*restart)(void)) {
    if (!W.on)
        return;
    W.title = title;
    W.restart = restart;
    W.started = true;
    replay_set_pace(pace);
    replay_set_end(held_at_end);
    display_raise_on_open();
    sound_open_monitor();
    atexit(log_at_exit);
    char t[32], speed[32];
    if (speeds[W.speed])
        snprintf(speed, sizeof speed, "at %dx", speeds[W.speed]);
    else
        snprintf(speed, sizeof speed, "as fast as it can");
    log_msg("watch: %s%s; clips go in %s", speed, W.paused ? ", paused" : "", W.list);
    if (W.going)
        log_msg("watch: going to %s", clip_format_time(W.from, t, sizeof t));
    anchor();
    update_title();
}

void watch_hold(void) {
    if (!W.started || W.at_end)
        return;
    const char *video = getenv("SDL_VIDEO_DRIVER");
    if (video && strcmp(video, "dummy") == 0) /* headless: nobody to close the window */
        return;
    if (!W.skipping) /* else logged as it began */
        log_run("the last stretch");
    W.at_end = true;
    W.going = W.skipping = false;
    sound_monitor(false);
    char t[32];
    log_msg("watch: the end, at %s: close the window to quit", clip_format_time(now_tick(), t, sizeof t));
    for (;;)
        idle();
}
