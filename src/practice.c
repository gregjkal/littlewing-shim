#include "practice.h"

#include <copyfile.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "clip.h"
#include "display.h"
#include "events.h"
#include "gamestart.h"
#include "replay.h"
#include "sound.h"
#include "trap.h"
#include "util.h"

static struct {
    bool unseen;        /* on the way to the moment */
    uint64_t target_us; /* the moment, on the replay's clock */
    trap_entry_fn next; /* the hook this one runs after it (seek's) */
    char title[128];
    char at[32];        /* the moment, as the title shows it */
    char progress[160]; /* the title on the way there */
    int shown;          /* the percentage it shows */
} P;

/* The window's title on the way to the moment says how far along it is,
   so a long first practice doesn't look stuck. */
static void show_progress(void) {
    int pct = (int)(replay_now_us() * 100 / P.target_us);
    if (pct == P.shown)
        return;
    P.shown = pct;
    snprintf(P.progress, sizeof P.progress, "Getting to %s of play: %d%%", P.at, pct);
    display_set_title(P.progress);
}

static void go_live(uint32_t index) {
    P.unseen = false; /* the hook stays, as gamestart's may run it */
    gamestart_practice_here(index);
    replay_go_live();
    events_release_keys();
    display_set_title(P.title);
    display_set_vsync(true);
    sound_go_live();
    log_msg("practice: playing on from %.3f s on the replay's clock", (double)replay_now_us() / 1e6);
}

static uint32_t on_entry(uint32_t index) {
    ev_frame f;
    uint32_t loop_index, loop_resume;
    if (P.unseen)
        show_progress();
    if (P.unseen && replay_now_us() >= P.target_us && trap_depth() == 2 &&
        trap_level(1, &loop_index, &loop_resume) &&
        strcmp(trap_import_name(loop_index), "RunApplicationEventLoop") == 0 &&
        events_loop_frame(&f) && f.timer < 0)
        go_live(index);
    return P.next ? P.next(index) : index;
}

bool practice_path(const char *replay, char *out, size_t cap) {
    size_t n = strlen(replay), k = strlen(".lwreplay");
    if (n > k && strcmp(replay + n - k, ".lwreplay") == 0)
        n -= k;
    return (size_t)snprintf(out, cap, "%.*s.practice", (int)n, replay) < cap;
}

bool practice_init(const char *title, char *err, size_t errlen) {
    const char *v = getenv("LOONY_PRACTICE");
    if (!v || !*v)
        return true;
    if (!replay_playing()) {
        snprintf(err, errlen, "LOONY_PRACTICE needs LOONY_REPLAY");
        return false;
    }
    const char *watch = getenv("LOONY_WATCH"), *clip = getenv("LOONY_CLIP");
    if ((watch && *watch && strcmp(watch, "0") != 0) || (clip && *clip)) {
        snprintf(err, errlen, "LOONY_PRACTICE can't be used with LOONY_WATCH or LOONY_CLIP");
        return false;
    }
    const uint32_t *marks;
    size_t nmarks = replay_marks(&marks);
    for (size_t i = 0; i < nmarks; i++) {
        char t[32];
        log_msg("practice: the recording's mark %zu is at %s of play", i + 1,
                clip_format_time(replay_play_ticks(marks[i]), t, sizeof t));
    }
    uint32_t ticks;
    if (!clip_parse_time(v, &ticks)) {
        snprintf(err, errlen, "LOONY_PRACTICE=%s isn't a time (ticks, or [h:]m:ss)", v);
        return false;
    }
    char length[32];
    if (ticks >= replay_play_length()) {
        snprintf(err, errlen, "LOONY_PRACTICE=%s is past the recording's end, at %s", v,
                 clip_format_time(replay_play_length(), length, sizeof length));
        return false;
    }
    P.target_us = replay_clock_us((uint64_t)ticks * 1000000 / 60);
    P.unseen = true;
    P.shown = -1;
    char path[PATH_MAX];
    if (practice_path(getenv("LOONY_REPLAY"), path, sizeof path))
        gamestart_practice(P.target_us, path);
    snprintf(P.title, sizeof P.title, "%s (practice from %s)", title,
             clip_format_time(ticks, P.at, sizeof P.at));
    P.next = trap_entry_hook();
    trap_on_entry(on_entry);
    log_msg("practice: replaying unseen to %s of play (%.3f s on the replay's clock)", P.at,
            (double)P.target_us / 1e6);
    return true;
}

bool practice_unseen(void) { return P.unseen; }

static const struct {
    const char *title, *name, *at; /* the menu's, the files', the moment */
} bundled[] = {
    {"2000 BC", "2000-bc", "32:16.05"}, /* as LANDED AT 2000 BC first shows */
};

size_t practice_bundled_count(void) { return sizeof bundled / sizeof bundled[0]; }

const char *practice_bundled_title(size_t i) { return bundled[i].title; }

/* The folder of the shipped practices, from the program's path. */
static bool bundled_dir(char *out, size_t cap) {
    char self[PATH_MAX];
    uint32_t size = sizeof self;
    if (_NSGetExecutablePath(self, &size) != 0)
        return false;
    const char *macos = strstr(self, ".app/Contents/MacOS/");
    char *slash = strrchr(self, '/');
    int n = macos ? snprintf(out, cap, "%.*s.app/Contents/Resources/practice", (int)(macos - self), self)
                  : snprintf(out, cap, "%.*s/../practice", slash ? (int)(slash - self) : 1, slash ? self : ".");
    return n > 0 && (size_t)n < cap;
}

/* Copies from to to, times and all, unless to is as new. */
static bool refresh(const char *from, const char *to) {
    struct stat a, b;
    if (stat(from, &a) != 0) {
        log_msg("practice: can't find %s", from);
        return false;
    }
    if (stat(to, &b) == 0 && b.st_mtimespec.tv_sec >= a.st_mtimespec.tv_sec)
        return true;
    if (copyfile(from, to, NULL, COPYFILE_DATA | COPYFILE_STAT) != 0) {
        log_msg("practice: can't copy %s to %s", from, to);
        unlink(to);
        return false;
    }
    chmod(to, 0644);
    return true;
}

bool practice_bundled_open(size_t i, char *out, size_t cap, uint32_t *ticks) {
    char from[PATH_MAX], dir[PATH_MAX], a[PATH_MAX], b[PATH_MAX];
    const char *home = getenv("HOME");
    if (i >= practice_bundled_count() || !home || !*home || !bundled_dir(from, sizeof from) ||
        !clip_parse_time(bundled[i].at, ticks))
        return false;
    snprintf(dir, sizeof dir, "%s/Library/Application Support/littlewing-shim/practice", home);
    if (!make_dirs(dir)) {
        log_msg("practice: can't create %s", dir);
        return false;
    }
    snprintf(a, sizeof a, "%s/%s.lwreplay", from, bundled[i].name);
    snprintf(b, sizeof b, "%s/%s.lwreplay", dir, bundled[i].name);
    if (!refresh(a, b))
        return false;
    /* Without its snapshot it still plays, from launch. */
    snprintf(a, sizeof a, "%s/%s.practice", from, bundled[i].name);
    snprintf(b, sizeof b, "%s/%s.practice", dir, bundled[i].name);
    refresh(a, b);
    return (size_t)snprintf(out, cap, "%s/%s.lwreplay", dir, bundled[i].name) < cap;
}
