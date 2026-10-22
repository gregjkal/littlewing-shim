#include "watch.h"

#include <SDL3/SDL.h>
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "clip.h"
#include "display.h"
#include "events.h"
#include "export.h"
#include "hd.h"
#include "menu.h"
#include "qd.h"
#include "replay.h"
#include "seek.h"
#include "sound.h"
#include "strip.h"
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
/* While exporting: how often what the export says is read, and the title
   shows it, in seconds. */
#define EXPORT_POLL_GAP 0.25
/* How long the viewer waits, quitting, for the export it stops. */
#define EXPORT_STOP_WAIT 2.0
/* The speed reached (watch_state's reached) is measured over the last
   RATE_SAMPLES - 1 gaps of RATE_GAP seconds of paced play. */
#define RATE_GAP 0.25
#define RATE_SAMPLES 5

static struct {
    bool on, started, at_end;
    const char *title;
    int speed; /* in speeds */
    bool paused;
    bool going; /* to from, without pacing or drawing every frame */
    bool seeking; /* to from, through a seek not served yet (seek.h), the same way */
    unsigned seek_generation; /* that seek's */
    uint32_t from;
    bool skipping; /* what isn't play, without pacing, drawing or sound */
    bool have_in;
    uint32_t in;
    uint32_t *marks; /* in play time, malloc'd */
    size_t nmarks;
    clip_span *clips; /* the list's, as last read, malloc'd */
    size_t nclips;
    SDL_Keymod held; /* the modifier keys down, as watch_key has seen them */
    char list[PATH_MAX]; /* the clips' list */
    char replay[PATH_MAX]; /* the recording, as an absolute path, for an export */
    double export_polled; /* the wall clock when the export was last read */
    /* Times shown and keyed (from, in, the list) are in play time; the
       replay's own clock is clock_us (replay_play_us maps one to the
       other). */
    uint64_t clock_us;   /* the replay's clock as the game has seen it */
    /* Pacing: the wall clock was anchor_wall when the clock was anchor_us. */
    double anchor_wall;
    uint64_t anchor_us;
    double paced_wall; /* the wall clock when the clock last moved */
    unsigned minutes;  /* whole minutes of the clock logged since the anchor */
    /* The clock against the wall clock, oldest first, for the speed reached. */
    double rate_wall[RATE_SAMPLES];
    uint64_t rate_us[RATE_SAMPLES];
    int nrate;
    bool pending;      /* the screen changed and isn't drawn yet */
    double drawn, polled;
    /* An unpaced stretch (going or skipping): when it began on both clocks,
       what the mixer had rendered by then, and what its output work has
       cost so far, in seconds of wall clock (log_stretch). */
    double stretch_wall, stretch_mixer, stretch_hd, stretch_poll, stretch_draw;
    uint64_t stretch_us;
    unsigned stretch_presents;
    char text[256]; /* the window's title */
} W;

void watch_reset(void) {
    free(W.marks);
    free(W.clips);
    memset(&W, 0, sizeof W);
}

bool watch_on(void) { return W.on; }

const char *watch_replay_path(void) { return W.on ? W.replay : NULL; }

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

static void tend_export(void);
static void toggle_pause(void);

/* Sets the title to what it says now. */
static void set_title(void) {
    char text[sizeof W.text], t[32], u[32], in[48] = "";
    const char *title = W.title ? W.title : "LittleWing";
    fmt_seconds(now_tick(), t, sizeof t);
    if (W.have_in)
        snprintf(in, sizeof in, ", clip from %s", fmt_seconds(W.in, u, sizeof u));
    const replay_span *spans;
    bool going = W.going || W.seeking;
    if (going && replay_play_spans(&spans) && W.clock_us < spans[0].on_us)
        snprintf(text, sizeof text, "%s: replaying what came before the game, %s of %s", title,
                 fmt_seconds(tick_of(W.clock_us), t, sizeof t),
                 fmt_seconds(tick_of(spans[0].on_us), u, sizeof u));
    else if (going)
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
    if (export_running()) {
        char exporting[96];
        size_t len = strlen(text);
        export_title(export_now(), exporting, sizeof exporting);
        snprintf(text + len, sizeof text - len, "%s", exporting);
    }
    if (strcmp(text, W.text) != 0) {
        snprintf(W.text, sizeof W.text, "%s", text);
        display_set_title(W.text);
    }
}

static void update_title(void) {
    tend_export();
    set_title();
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
    W.drawn = wall_seconds();
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
    bool going = W.going || W.seeking, unpaced = going || W.skipping;
    if (dirty && unpaced)
        W.stretch_presents++;
    int speed = speeds[W.speed];
    double now = wall_seconds();
    double lag = W.paused || !speed
                     ? 0
                     : now - (W.anchor_wall + (double)(W.clock_us - W.anchor_us) / 1e6 / speed);
    if (!W.skipping && watch_draws(now - W.drawn, lag, speed, going)) {
        draw();
        if (unpaced)
            W.stretch_draw += W.drawn - now;
    } else if (dirty && hd_scale()) { /* the HD copy kept in step, as when every frame is drawn */
        qd_pixels px;
        qd_palette pal;
        int w, h;
        qd_screen(&px, &pal);
        hd_frame(&px, &w, &h);
        if (unpaced)
            W.stretch_hd += wall_seconds() - now;
    }
}

/* No window (SDL_VIDEO_DRIVER=dummy): nobody but the script presses keys. */
static bool headless(void) {
    const char *video = getenv("SDL_VIDEO_DRIVER");
    return video && strcmp(video, "dummy") == 0;
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
    return speeds[W.speed] == 1 && !W.paused && !W.going && !W.seeking && !W.skipping && !W.at_end;
}

/* The wall clock and the replay's clock line up from now, after falling
   behind: the speed reached is measured on across it. */
static void realign(void) {
    W.anchor_wall = W.paced_wall = wall_seconds();
    W.anchor_us = W.clock_us;
    W.minutes = 0;
    sound_monitor(sound_on());
}

/* They line up from now, after anything but falling behind: the speed
   reached is measured afresh. */
static void anchor(void) {
    realign();
    W.nrate = 0;
}

/* The replay's clock moved, paced: a sample for the speed reached, at most
   every RATE_GAP. */
static void sample_rate(void) {
    double now = wall_seconds();
    if (W.nrate && now - W.rate_wall[W.nrate - 1] < RATE_GAP)
        return;
    if (W.nrate == RATE_SAMPLES) {
        memmove(W.rate_wall, W.rate_wall + 1, (RATE_SAMPLES - 1) * sizeof W.rate_wall[0]);
        memmove(W.rate_us, W.rate_us + 1, (RATE_SAMPLES - 1) * sizeof W.rate_us[0]);
        W.nrate--;
    }
    W.rate_wall[W.nrate] = now;
    W.rate_us[W.nrate++] = W.clock_us;
}

/* The speed reached over the samples, or 0 until they span RATE_SAMPLES
   - 1 gaps. */
static float rate(void) {
    if (W.nrate < RATE_SAMPLES)
        return 0;
    double wall = W.rate_wall[W.nrate - 1] - W.rate_wall[0];
    return wall > 0 ? (float)((double)(W.rate_us[W.nrate - 1] - W.rate_us[0]) / 1e6 / wall) : 0;
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

/* ---- unpaced stretches: going to a time, or leaving out what isn't play ---- */

/* One begins now. */
static void begin_stretch(void) {
    W.stretch_wall = wall_seconds();
    W.stretch_us = W.clock_us;
    W.stretch_mixer = sound_render_seconds();
    W.stretch_hd = W.stretch_poll = W.stretch_draw = 0;
    W.stretch_presents = 0;
}

/* It ends: logs how fast it went, and what each kind of output work cost
   (the numbers the seek engine is sized by; #59). */
static void log_stretch(const char *what) {
    double took = wall_seconds() - W.stretch_wall, game = (double)(W.clock_us - W.stretch_us) / 1e6;
    if (took <= 0 || game <= 0)
        return;
    log_msg("watch: %s: %.1f s of the game's clock in %.2f s on the wall clock (%.1fx); "
            "%u presents; HD sync %.2f s, mixer %.2f s, polling %.2f s, drawing %.2f s",
            what, game, took, game / took, W.stretch_presents, W.stretch_hd,
            sound_render_seconds() - W.stretch_mixer, W.stretch_poll, W.stretch_draw);
}

/* Going or skipping: the window stays live, a few times a second. */
static void poll_now_and_then(void) {
    if (wall_seconds() - W.polled >= GOING_DRAW_GAP / 4) {
        W.polled = wall_seconds();
        display_poll();
        update_title();
        W.stretch_poll += wall_seconds() - W.polled;
    }
}

/* The replay's clock is about to move to clock_us: waits until the wall
   clock has reached it at this speed. What isn't play goes by unpaced. */
static void pace(uint64_t clock_us) {
    if (W.seeking) { /* to the next trap entry where the seek is served */
        W.clock_us = clock_us;
        poll_now_and_then();
        return;
    }
    uint64_t next_on;
    bool in_play = replay_in_play(clock_us, &next_on);
    if (W.going) {
        W.clock_us = clock_us;
        if (!in_play || play_tick_of(clock_us) < W.from) {
            poll_now_and_then();
            return;
        }
        W.going = W.skipping = false;
        char t[32], what[64];
        snprintf(what, sizeof what, "went to %s", clip_format_time(W.from, t, sizeof t));
        log_stretch(what);
        log_msg("watch: reached %s", t);
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
            begin_stretch();
        }
        W.clock_us = clock_us;
        poll_now_and_then();
        return;
    }
    if (W.skipping) { /* play again: paced from here */
        W.skipping = false;
        W.clock_us = clock_us;
        log_stretch("left out what isn't play");
        anchor();
    }
    for (;;) {
        if (W.going || W.seeking) { /* a key while waiting: paused or not, it goes */
            pace(clock_us); /* from this read, which may already be there */
            return;
        }
        if (W.paused) {
            /* headless, the script's next action comes now: its clock has
               stopped, and nothing else would go on. With none left, nobody
               can press Space, so it goes on by itself. */
            if (!headless())
                idle();
            else if (!events_script_rest())
                toggle_pause();
            continue;
        }
        int speed = speeds[W.speed];
        double now = wall_seconds();
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
                realign();
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
    W.paced_wall = wall_seconds();
    sample_rate();
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

/* Goes to tick through a seek (seek.h): the replay runs unpaced to where it
   is served, and from the snapshot put back there to tick. */
static void seek_tick(uint32_t tick, bool back) {
    char t[32];
    if (!W.seeking) {
        if (W.going || W.skipping)
            log_stretch("the last stretch, unpaced");
        else
            log_run("the last stretch");
        begin_stretch();
    }
    log_msg("watch: %s %s", back ? "back to" : "going to", clip_format_time(tick, t, sizeof t));
    W.from = tick;
    W.seeking = true;
    W.going = W.skipping = false;
    W.seek_generation = seek_to(replay_clock_us((uint64_t)tick * 1000000 / 60));
    sound_monitor(false);
}

/* A seek was served (seek.h): the replay's clock is at clock_us, from a
   snapshot put back (restored) or where it was. From there it goes on to
   the time wanted, unpaced, keeping the pause and the speed. */
static void landed(uint64_t clock_us, bool restored, unsigned generation) {
    if (generation != W.seek_generation) /* a newer one is pending */
        return;
    W.seeking = false;
    if (restored) {
        W.clock_us = clock_us;
        W.at_end = false;
        W.pending = true; /* the screen put back is drawn, whether the game draws or not */
        begin_stretch(); /* from the snapshot's clock */
    }
    uint64_t next_on;
    W.going = !W.at_end && (W.from > now_tick() || !replay_in_play(W.clock_us, &next_on));
    if (!W.going) {
        char t[32], what[64];
        snprintf(what, sizeof what, "went to %s", clip_format_time(now_tick(), t, sizeof t));
        log_stretch(what);
        log_msg("watch: reached %s", t);
        anchor();
    }
    sound_monitor(sound_on());
    update_title();
}

/* To tick of play time. Back, or ahead past a snapshot, goes through a
   seek. */
static void go_to(uint32_t tick) {
    bool back = tick < now_tick();
    if (W.at_end && !back) /* nothing ahead */
        return;
    if (back || W.seeking ||
        seek_snapshot_ahead(W.clock_us, replay_clock_us((uint64_t)tick * 1000000 / 60))) {
        seek_tick(tick, back);
        return;
    }
    char t[32];
    log_msg("watch: going to %s", clip_format_time(tick, t, sizeof t));
    W.from = tick;
    bool was_going = W.going;
    W.going = tick > now_tick();
    if (!W.going) { /* landed on now: the stretch ends here */
        if (was_going && !W.skipping) {
            char what[64];
            snprintf(what, sizeof what, "went to %s", t);
            log_stretch(what);
        }
        anchor();
    } else if (!was_going && !W.skipping) {
        begin_stretch();
    }
    sound_monitor(sound_on());
}

/* J, L, the arrows, comma and period: by step ticks back (step < 0) or
   ahead, from the time going to while going. */
static void step_by(int64_t step) {
    int64_t to = (int64_t)(W.going || W.seeking ? W.from : now_tick()) + step;
    go_to(to < 0 ? 0 : to > UINT32_MAX ? UINT32_MAX : (uint32_t)to);
}

/* Reads the list again, for the strip: the clips in it, or -1 if it can't
   be read (none are shown then). */
static int read_clips(void) {
    free(W.clips);
    W.clips = NULL;
    W.nclips = 0;
    size_t len;
    char *text = (char *)read_file(W.list, &len);
    if (!text)
        return -1;
    char err[256];
    bool ok = clip_parse_list(text, &W.clips, &W.nclips, err, sizeof err);
    free(text);
    if (!ok)
        log_msg("watch: %s", err);
    return ok ? (int)W.nclips : -1;
}

/* The replay's marks, in play time, for the strip and [ and ]. */
static void read_marks(void) {
    const uint32_t *ticks;
    size_t n = replay_marks(&ticks);
    free(W.marks);
    W.marks = NULL;
    W.nmarks = 0;
    if (!n)
        return;
    if (!(W.marks = malloc(n * sizeof *W.marks)))
        fatal("out of memory");
    for (size_t i = 0; i < n; i++)
        W.marks[i] = replay_play_ticks(ticks[i]);
    W.nmarks = n;
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
    int n = read_clips();
    log_msg("watch: clip %d, %s-%s, saved in %s", n, a, b, W.list);
    flash("clip %d saved: %s-%s", n, a, b);
}

/* ---- exporting (export.h) ---- */

static const char *export_what(export_kind kind) { return kind == EXPORT_MARKS ? "marks" : "clips"; }

/* Shows each file in Finder, selected. */
static void reveal(char *const *files, size_t n) {
    for (size_t i = 0; i < n; i++)
        log_msg("watch: revealing %s", files[i]);
    menu_reveal((const char *const *)files, n);
}

/* The export ended with status: says how, and shows what it wrote. */
static void export_ended(int status) {
    const export_progress *p = export_now();
    set_title(); /* without the progress, which the flash would repeat */
    if (export_cancelled() && status == 128 + SIGTERM) {
        log_msg("watch: export cancelled");
        flash("export cancelled");
    } else if (status == 0 && p->nwrote) {
        log_msg("watch: exported %zu clip%s", p->nwrote, p->nwrote == 1 ? "" : "s");
        flash("exported %zu clip%s", p->nwrote, p->nwrote == 1 ? "" : "s");
        reveal(p->wrote, p->nwrote);
    } else if (status == 0 && p->nskipped) {
        log_msg("watch: the %zu clip%s exported already", p->nskipped,
                p->nskipped == 1 ? " is" : "s are");
        flash("clips are exported already");
        reveal(p->skipped, p->nskipped);
    } else if (p->error[0]) {
        log_msg("watch: can't export: %s", p->error);
        flash("%s", p->error);
    } else if (status == CLIP_NOTHING_STATUS) {
        log_msg("watch: export: the recording ends before the clip starts, nothing written");
        flash("the recording ends before the clip starts: nothing exported");
    } else {
        log_msg("watch: export failed (status %d)", status);
        flash("export failed (status %d), see the log", status);
    }
}

/* A few times a second while exporting: reads what the export said, and
   says how it ended once it has. */
static void tend_export(void) {
    if (!export_running())
        return;
    double now = wall_seconds();
    if (now - W.export_polled < EXPORT_POLL_GAP)
        return;
    W.export_polled = now;
    int status;
    if (export_poll(&status))
        export_ended(status);
}

void watch_export(bool marks) {
    if (!W.started)
        return;
    export_kind kind = marks ? EXPORT_MARKS : EXPORT_CLIPS;
    if (export_running()) {
        flash("already exporting: Esc cancels it");
        return;
    }
    if (kind == EXPORT_CLIPS && (access(W.list, F_OK) != 0 || read_clips() == 0)) {
        log_msg("watch: no clips to export in %s", W.list);
        flash("no clips to export: I marks where one starts, O where it ends");
        return;
    }
    char self[PATH_MAX], err[PATH_MAX + 64];
    uint32_t size = sizeof self;
    bool found = _NSGetExecutablePath(self, &size) == 0;
    if (!found)
        snprintf(err, sizeof err, "can't find this program");
    if (!found || !export_start(self, kind, W.replay, err, sizeof err)) {
        log_msg("watch: can't export: %s", err);
        flash("can't export: %s", err);
        return;
    }
    log_msg("watch: exporting the %s of %s", export_what(kind), W.replay);
    W.export_polled = wall_seconds();
    update_title();
}

void watch_cancel_export(void) {
    if (!export_running() || export_cancelled())
        return;
    log_msg("watch: cancelling the export");
    export_cancel();
}

bool watch_exporting(void) { return export_running(); }

void watch_stop_export(void) {
    if (!export_running())
        return;
    log_msg("watch: stopping the export");
    int status;
    if (export_stop(EXPORT_STOP_WAIT, &status))
        log_msg("watch: the export stopped (status %d)", status);
    else
        log_msg("watch: the export hasn't stopped after %.0f s", EXPORT_STOP_WAIT);
}

const watch_binding watch_bindings[] = {
    {WATCH_PLAY_PAUSE, SDL_SCANCODE_SPACE, 0, "Play/Pause", false},
    {WATCH_PLAY_PAUSE, SDL_SCANCODE_K, 0, NULL, false}, /* as on YouTube */
    {WATCH_BACK_SMALL, SDL_SCANCODE_LEFT, 0, "Back 5 s", true},
    {WATCH_BACK, SDL_SCANCODE_J, 0, "Back 10 s", false},
    {WATCH_BACK_BIG, SDL_SCANCODE_J, SDL_KMOD_SHIFT, "Back 1 min", false},
    {WATCH_AHEAD_SMALL, SDL_SCANCODE_RIGHT, 0, "Ahead 5 s", false},
    {WATCH_AHEAD, SDL_SCANCODE_L, 0, "Ahead 10 s", false},
    {WATCH_AHEAD_BIG, SDL_SCANCODE_L, SDL_KMOD_SHIFT, "Ahead 1 min", false},
    {WATCH_FRAME_BACK, SDL_SCANCODE_COMMA, 0, "Frame Back", true}, /* as on YouTube */
    {WATCH_FRAME_AHEAD, SDL_SCANCODE_PERIOD, 0, "Frame Ahead", false},
    {WATCH_TENTH_0, SDL_SCANCODE_0, 0, NULL, false}, /* as on YouTube */
    {WATCH_TENTH_1, SDL_SCANCODE_1, 0, NULL, false},
    {WATCH_TENTH_2, SDL_SCANCODE_2, 0, NULL, false},
    {WATCH_TENTH_3, SDL_SCANCODE_3, 0, NULL, false},
    {WATCH_TENTH_4, SDL_SCANCODE_4, 0, NULL, false},
    {WATCH_TENTH_5, SDL_SCANCODE_5, 0, NULL, false},
    {WATCH_TENTH_6, SDL_SCANCODE_6, 0, NULL, false},
    {WATCH_TENTH_7, SDL_SCANCODE_7, 0, NULL, false},
    {WATCH_TENTH_8, SDL_SCANCODE_8, 0, NULL, false},
    {WATCH_TENTH_9, SDL_SCANCODE_9, 0, NULL, false},
    {WATCH_TENTH_0, SDL_SCANCODE_KP_0, 0, NULL, false}, /* and on the keypad */
    {WATCH_TENTH_1, SDL_SCANCODE_KP_1, 0, NULL, false},
    {WATCH_TENTH_2, SDL_SCANCODE_KP_2, 0, NULL, false},
    {WATCH_TENTH_3, SDL_SCANCODE_KP_3, 0, NULL, false},
    {WATCH_TENTH_4, SDL_SCANCODE_KP_4, 0, NULL, false},
    {WATCH_TENTH_5, SDL_SCANCODE_KP_5, 0, NULL, false},
    {WATCH_TENTH_6, SDL_SCANCODE_KP_6, 0, NULL, false},
    {WATCH_TENTH_7, SDL_SCANCODE_KP_7, 0, NULL, false},
    {WATCH_TENTH_8, SDL_SCANCODE_KP_8, 0, NULL, false},
    {WATCH_TENTH_9, SDL_SCANCODE_KP_9, 0, NULL, false},
    {WATCH_PREV_MARK, SDL_SCANCODE_LEFTBRACKET, 0, "Previous Mark", true},
    {WATCH_NEXT_MARK, SDL_SCANCODE_RIGHTBRACKET, 0, "Next Mark", false},
    {WATCH_SLOWER, SDL_SCANCODE_COMMA, SDL_KMOD_SHIFT, "Slower", true}, /* <, as on YouTube */
    {WATCH_FASTER, SDL_SCANCODE_PERIOD, SDL_KMOD_SHIFT, "Faster", false}, /* > */
    {WATCH_MARK_IN, SDL_SCANCODE_I, 0, "Mark In", true},
    {WATCH_MARK_OUT, SDL_SCANCODE_O, 0, "Mark Out", false},
    {WATCH_EXPORT_CLIPS, SDL_SCANCODE_E, 0, "Export Clips", true},
    {WATCH_EXPORT_MARKS, 0, 0, "Export Marks", false},
    {WATCH_CANCEL_EXPORT, SDL_SCANCODE_ESCAPE, 0, "Cancel Export", true},
};
const size_t watch_nbindings = sizeof watch_bindings / sizeof watch_bindings[0];

static void toggle_pause(void) {
    char t[32];
    W.paused = !W.paused;
    log_msg("watch: %s at %s", W.paused ? "paused" : "going on",
            clip_format_time(now_tick(), t, sizeof t));
    if (W.paused)
        log_run("the last stretch");
    anchor();
}

static void mark_in(void) {
    char t[32];
    W.in = now_tick();
    W.have_in = true;
    log_msg("watch: a clip starts at %s", clip_format_time(W.in, t, sizeof t));
    flash("a clip starts at %s: O ends it", t);
}

void watch_get_state(watch_state *s) {
    s->now = W.going || W.seeking ? W.from : now_tick();
    s->length = replay_play_length();
    s->paused = W.paused;
    s->at_end = W.at_end;
    s->speed = speeds[W.speed];
    s->reached = W.paused || W.at_end || W.going || W.seeking || W.skipping ? 0 : rate();
    s->have_in = W.have_in;
    s->in = W.in;
    s->marks = W.marks;
    s->nmarks = W.nmarks;
    s->clips = W.clips;
    s->nclips = W.nclips;
}

int watch_mark_from(const watch_state *s, int dir) {
    if (dir > 0) {
        for (size_t i = 0; i < s->nmarks; i++)
            if (s->marks[i] > s->now)
                return (int)i;
        return -1;
    }
    for (size_t i = s->nmarks; i-- > 0;)
        if (s->marks[i] + WATCH_MARK_GRACE_TICKS < s->now)
            return (int)i;
    return -1;
}

/* [ and ]: to the mark before or after now. */
static void to_mark(int dir) {
    watch_state s;
    watch_get_state(&s);
    int i = watch_mark_from(&s, dir);
    if (i < 0) {
        if (s.nmarks)
            flash("no mark %s here", dir > 0 ? "after" : "before");
        else
            flash("no marks: Cmd-S marks one while recording");
        return;
    }
    char t[32];
    log_msg("watch: to mark %d, at %s", i + 1, clip_format_time(s.marks[i], t, sizeof t));
    go_to(s.marks[i]);
}

void watch_go_to(uint32_t tick) {
    if (!W.started)
        return;
    strip_poke();
    go_to(tick);
    update_title();
}

bool watch_can(watch_action action) {
    if (!W.started)
        return false;
    switch (action) {
    case WATCH_PLAY_PAUSE: return !W.at_end;
    case WATCH_FRAME_BACK: return W.paused || W.at_end;
    case WATCH_FRAME_AHEAD: return W.paused && !W.at_end;
    case WATCH_EXPORT_CLIPS:
    case WATCH_EXPORT_MARKS: return !export_running();
    case WATCH_CANCEL_EXPORT: return export_running();
    case WATCH_PREV_MARK:
    case WATCH_NEXT_MARK: return W.nmarks > 0;
    default: return true;
    }
}

void watch_act(watch_action action, bool repeat) {
    bool step = action >= WATCH_BACK_SMALL && action <= WATCH_FRAME_AHEAD;
    if (!W.started || (repeat && !step)) /* only the steps go on while their key is held */
        return;
    strip_poke();
    switch (action) {
    case WATCH_PLAY_PAUSE:
        if (!W.at_end)
            toggle_pause();
        break;
    case WATCH_BACK_SMALL: step_by(-WATCH_SMALL_STEP_TICKS); break;
    case WATCH_BACK: step_by(-WATCH_STEP_TICKS); break;
    case WATCH_BACK_BIG: step_by(-WATCH_BIG_STEP_TICKS); break;
    case WATCH_AHEAD_SMALL: step_by(WATCH_SMALL_STEP_TICKS); break;
    case WATCH_AHEAD: step_by(WATCH_STEP_TICKS); break;
    case WATCH_AHEAD_BIG: step_by(WATCH_BIG_STEP_TICKS); break;
    case WATCH_FRAME_BACK:
        if (watch_can(action)) {
            W.paused = true; /* from the end too: the frame stays up */
            step_by(-WATCH_FRAME_TICKS);
        }
        break;
    case WATCH_FRAME_AHEAD:
        if (watch_can(action))
            step_by(WATCH_FRAME_TICKS);
        break;
    case WATCH_TENTH_0:
    case WATCH_TENTH_1:
    case WATCH_TENTH_2:
    case WATCH_TENTH_3:
    case WATCH_TENTH_4:
    case WATCH_TENTH_5:
    case WATCH_TENTH_6:
    case WATCH_TENTH_7:
    case WATCH_TENTH_8:
    case WATCH_TENTH_9:
        go_to((uint32_t)((uint64_t)replay_play_length() * (action - WATCH_TENTH_0) / 10));
        break;
    case WATCH_SLOWER:
        if (W.speed > 0)
            set_speed(W.speed - 1);
        break;
    case WATCH_FASTER:
        if (W.speed + 1 < NSPEEDS)
            set_speed(W.speed + 1);
        break;
    case WATCH_PREV_MARK: to_mark(-1); break;
    case WATCH_NEXT_MARK: to_mark(1); break;
    case WATCH_MARK_IN: mark_in(); break;
    case WATCH_MARK_OUT: save_clip(); break;
    case WATCH_EXPORT_CLIPS: watch_export(false); break;
    case WATCH_EXPORT_MARKS: watch_export(true); break;
    case WATCH_CANCEL_EXPORT: watch_cancel_export(); break;
    }
    update_title();
}

/* The modifier a key is, or 0. */
static SDL_Keymod modifier(int scancode) {
    switch (scancode) {
    case SDL_SCANCODE_LSHIFT: return SDL_KMOD_LSHIFT;
    case SDL_SCANCODE_RSHIFT: return SDL_KMOD_RSHIFT;
    case SDL_SCANCODE_LCTRL: return SDL_KMOD_LCTRL;
    case SDL_SCANCODE_RCTRL: return SDL_KMOD_RCTRL;
    case SDL_SCANCODE_LALT: return SDL_KMOD_LALT;
    case SDL_SCANCODE_RALT: return SDL_KMOD_RALT;
    case SDL_SCANCODE_LGUI: return SDL_KMOD_LGUI;
    case SDL_SCANCODE_RGUI: return SDL_KMOD_RGUI;
    default: return 0;
    }
}

/* Shift, Ctrl, Alt and Cmd as held, whichever side: as watch_bindings'
   mods say them. */
static uint16_t held_mods(void) {
    static const SDL_Keymod each[] = {SDL_KMOD_SHIFT, SDL_KMOD_CTRL, SDL_KMOD_ALT, SDL_KMOD_GUI};
    uint16_t mods = 0;
    for (size_t i = 0; i < sizeof each / sizeof each[0]; i++)
        if (W.held & each[i])
            mods |= each[i];
    return mods;
}

void watch_key(int scancode, bool down, bool repeat) {
    /* The modifiers are followed from the keys, not SDL's state, so that a
       script's (LOONY_SCRIPT) count too. */
    SDL_Keymod m = modifier(scancode);
    if (m) {
        W.held = down ? W.held | m : W.held & ~m;
        return;
    }
    if (!W.started || !down)
        return;
    uint16_t mods = held_mods();
    for (size_t i = 0; i < watch_nbindings; i++)
        if (watch_bindings[i].scancode == scancode && watch_bindings[i].mods == mods) {
            watch_act(watch_bindings[i].action, repeat);
            return;
        }
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

/* Quitting: nothing outlives the viewer. */
static void stop_export_at_exit(void) { watch_stop_export(); }

/* From now on, an exit stops the export: once is enough. */
static void stop_export_on_exit(void) {
    static bool set;
    if (!set)
        atexit(stop_export_at_exit);
    set = true;
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
    /* Absolute, so the export names its files that way, but not resolved:
       they are named as the viewer names the recording. */
    char cwd[PATH_MAX];
    if (replay[0] != '/' && getcwd(cwd, sizeof cwd))
        snprintf(W.replay, sizeof W.replay, "%s/%s", cwd, replay);
    else
        snprintf(W.replay, sizeof W.replay, "%s", replay);
    W.on = true;
    return true;
}

static void log_at_exit(void) {
    if (W.going || W.seeking || W.skipping)
        log_stretch("the last stretch, unpaced");
    else if (!W.paused && !W.at_end) /* logged already */
        log_run("the last stretch");
}

static void held_at_end(void) { watch_hold(); }

/* The replay can't go on: said in a message box (main.c's failure hook),
   while the window is still up. */
static void desynced(const char *msg) {
    char t[32], text[256];
    (void)msg; /* logged already, with the step */
    snprintf(text, sizeof text,
             "The replay went out of step with the recording at %s, so it can't go on.",
             fmt_seconds(now_tick(), t, sizeof t));
    util_report_failure(text);
}

void watch_start(const char *title) {
    if (!W.on)
        return;
    W.title = title;
    W.started = true;
    read_marks();
    read_clips();
    replay_set_pace(pace);
    replay_set_end(held_at_end);
    replay_set_desync(desynced);
    seek_start(landed);
    strip_install();
    display_raise_on_open();
    sound_open_monitor();
    atexit(log_at_exit);
    stop_export_on_exit();
    char t[32], speed[32];
    if (speeds[W.speed])
        snprintf(speed, sizeof speed, "at %dx", speeds[W.speed]);
    else
        snprintf(speed, sizeof speed, "as fast as it can");
    log_msg("watch: %s%s; clips go in %s", speed, W.paused ? ", paused" : "", W.list);
    if (W.going)
        log_msg("watch: going to %s", clip_format_time(W.from, t, sizeof t));
    anchor();
    if (W.going)
        begin_stretch();
    update_title();
}

void watch_hold(void) {
    if (!W.started || W.at_end)
        return;
    if (W.going || W.seeking || W.skipping)
        log_stretch("the stretch to the end, unpaced");
    else
        log_run("the last stretch");
    W.at_end = true;
    W.going = W.skipping = false;
    if (headless()) { /* nobody to close the window */
        /* or to press a key but the script, whose actions left come now */
        while (!W.seeking && events_script_rest())
            ;
        if (W.seeking)
            seek_serve_here(); /* back into the game, unless there's nowhere to go */
        if (export_running())
            log_msg("watch: the end: waiting for the export");
        while (export_running()) /* so a scripted run ends with it done */
            idle();
        return;
    }
    sound_monitor(false);
    char t[32];
    log_msg("watch: the end, at %s: close the window to quit", clip_format_time(now_tick(), t, sizeof t));
    for (;;) {
        if (W.seeking)
            seek_serve_here(); /* back into the game, unless there's nowhere to go */
        idle();
    }
}
