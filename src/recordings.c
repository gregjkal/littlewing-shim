#include "recordings.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "clip.h"
#include "display.h"
#include "files.h"
#include "game.h"
#include "gamestart.h"
#include "replay.h"
#include "util.h"
#include "watch.h"

bool recordings_dir(char *out, size_t cap) {
    const char *replay = getenv("LOONY_REPLAY"), *data = getenv("LOONY_DATA_DIR");
    char root[PATH_MAX];
    if (replay && *replay) {
        const char *home = getenv("HOME");
        if (!home || !*home)
            return false;
        snprintf(out, cap, "%s/Library/Application Support/littlewing-shim/recordings", home);
        return true;
    }
    if (files_data_root(root, sizeof root))
        snprintf(out, cap, "%s/recordings", root);
    else if (data && *data)
        snprintf(out, cap, "%s/recordings", data);
    else
        return false;
    return true;
}

/* A file that isn't a replay this version reads, as it was when it was
   read, so it is neither read nor logged again until it changes. */
typedef struct {
    char path[PATH_MAX];
    int64_t mtime_ns;
    off_t size;
} unreadable;

static struct {
    recording *list;
    size_t n;
    unreadable *bad;
    size_t nbad;
} C;

void recordings_forget(void) {
    for (size_t i = 0; i < C.n; i++)
        free(C.list[i].marks);
    free(C.list);
    free(C.bad);
    memset(&C, 0, sizeof C);
}

static bool ends_with(const char *s, const char *end) {
    size_t n = strlen(s), m = strlen(end);
    return n >= m && strcmp(s + n - m, end) == 0;
}

/* The start and the game in that second from name, "<game_id>-<date>-<time>
   [-<n>][-marked].lwreplay" (replay.h); false for a name not like that. */
static bool start_in_name(const char *name, const char *game_id, time_t *start, int *second) {
    size_t id_len = strlen(game_id);
    struct tm tm = {.tm_isdst = -1};
    if (strncmp(name, game_id, id_len) != 0 || name[id_len] != '-')
        return false;
    const char *rest = strptime(name + id_len + 1, "%Y-%m-%d-%H%M%S", &tm);
    if (!rest || rest != name + id_len + 1 + strlen("YYYY-mm-dd-HHMMSS"))
        return false;
    *start = mktime(&tm);
    *second = rest[0] == '-' && rest[1] >= '1' && rest[1] <= '9' ? (int)strtol(rest + 1, NULL, 10) : 1;
    return *start != (time_t)-1;
}

/* Newest first: by start, then by the game in that second, then by path. */
static int newest_first(const void *a, const void *b) {
    const recording *x = a, *y = b;
    if (x->start != y->start)
        return x->start > y->start ? -1 : 1;
    if (x->second != y->second)
        return x->second > y->second ? -1 : 1;
    return -strcmp(x->path, y->path);
}

/* Reads the recording at path into r: false if it isn't one. */
static bool read_recording(const char *path, const char *name, recording *r) {
    replay_summary s;
    char err[256];
    if (!replay_summarize(path, &s, err, sizeof err)) {
        log_msg("recordings: leaving out %s: %s", path, err);
        return false;
    }
    snprintf(r->path, sizeof r->path, "%s", path);
    snprintf(r->game_id, sizeof r->game_id, "%s", s.game_id);
    if (!start_in_name(name, s.game_id, &r->start, &r->second)) {
        r->start = (time_t)s.start_unix;
        r->second = 1;
    }
    r->length = s.play_ticks;
    r->marks = s.marks;
    r->nmarks = s.nmarks;
    r->marked = ends_with(name, "-marked.lwreplay");
    return true;
}

size_t recordings_list(const char *dir, const recording **list) {
    recording *fresh = NULL;
    unreadable *bad = NULL;
    size_t n = 0, cap = 0, nbad = 0, bad_cap = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char path[PATH_MAX];
        struct stat st;
        if (e->d_name[0] == '.' || !ends_with(e->d_name, ".lwreplay") ||
            (size_t)snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= sizeof path ||
            stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        int64_t mtime_ns = (int64_t)st.st_mtimespec.tv_sec * 1000000000 + st.st_mtimespec.tv_nsec;
        recording *was = NULL;
        bool known_bad = false;
        for (size_t i = 0; i < C.n && !was; i++)
            if (C.list[i].mtime_ns == mtime_ns && C.list[i].size == st.st_size &&
                strcmp(C.list[i].path, path) == 0)
                was = &C.list[i];
        for (size_t i = 0; i < C.nbad && !known_bad; i++)
            known_bad = C.bad[i].mtime_ns == mtime_ns && C.bad[i].size == st.st_size &&
                        strcmp(C.bad[i].path, path) == 0;
        recording r;
        memset(&r, 0, sizeof r);
        if (was) {
            r = *was;
            was->marks = NULL; /* moved */
        } else if (known_bad || !read_recording(path, e->d_name, &r)) {
            if (nbad == bad_cap && !(bad = realloc(bad, (bad_cap = bad_cap ? bad_cap * 2 : 8) * sizeof *bad)))
                fatal("out of memory");
            snprintf(bad[nbad].path, sizeof bad[nbad].path, "%s", path);
            bad[nbad].mtime_ns = mtime_ns;
            bad[nbad++].size = st.st_size;
            continue;
        }
        r.mtime_ns = mtime_ns;
        r.size = st.st_size;
        char clips[PATH_MAX];
        r.clips = clip_list_path(path, clips, sizeof clips) && access(clips, F_OK) == 0;
        if (n == cap && !(fresh = realloc(fresh, (cap = cap ? cap * 2 : 16) * sizeof *fresh)))
            fatal("out of memory");
        fresh[n++] = r;
    }
    if (d)
        closedir(d);
    recordings_forget();
    if (n)
        qsort(fresh, n, sizeof *fresh, newest_first);
    C.list = fresh;
    C.n = n;
    C.bad = bad;
    C.nbad = nbad;
    *list = C.list;
    return n;
}

const char *recordings_label(const recording *r, bool with_game, char *buf, size_t cap) {
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm at, now_tm;
    time_t now = time(NULL);
    localtime_r(&r->start, &at);
    localtime_r(&now, &now_tm);
    char date[32], year[16] = "", length[32];
    if (at.tm_year != now_tm.tm_year)
        snprintf(year, sizeof year, ", %d", at.tm_year + 1900);
    snprintf(date, sizeof date, "%s %d%s, %d:%02d %s", months[at.tm_mon], at.tm_mday, year,
             at.tm_hour % 12 ? at.tm_hour % 12 : 12, at.tm_min, at.tm_hour < 12 ? "AM" : "PM");
    clip_format_time(r->length / 60 * 60, length, sizeof length);
    const game_info *g = game_by_id(r->game_id);
    char marks[32] = "";
    if (r->nmarks)
        snprintf(marks, sizeof marks, ", %zu mark%s", r->nmarks, r->nmarks == 1 ? "" : "s");
    snprintf(buf, cap, "%s%s%s, %s%s%s", with_game ? (g ? g->title : r->game_id) : "",
             with_game ? ", " : "", date, length, marks, r->clips ? ", clips" : "");
    return buf;
}

const char *recordings_mark_label(uint32_t ticks, char *buf, size_t cap) {
    char t[32];
    snprintf(buf, cap, "Play from %s", clip_format_time(ticks / 60 * 60, t, sizeof t));
    return buf;
}

bool recordings_mark_shown(const recording *r, size_t m) {
    uint32_t s = r->marks[m] / 60;
    return s > 0 && (m == 0 || s != r->marks[m - 1] / 60);
}

/* What the last game or replay set that a viewer, or what comes after one,
   mustn't inherit. */
static void clear_env(void) {
    static const char *const clear[] = {"LOONY_GAME", "LOONY_DATA_DIR", "LOONY_RECORD",
                                        "LOONY_CLIP", "LOONY_REPLAY_LICENSE", "LOONY_WATCH_FROM",
                                        "LOONY_WATCH_SPEED", "LOONY_WATCH_PAUSED", "LOONY_WATCH_IN"};
    for (size_t i = 0; i < sizeof clear / sizeof clear[0]; i++)
        unsetenv(clear[i]);
}

void recordings_watch_env(const char *path, uint32_t from_ticks) {
    setenv("LOONY_REPLAY", path, 1);
    setenv("LOONY_WATCH", "1", 1);
    clear_env();
    if (from_ticks) {
        char v[16];
        snprintf(v, sizeof v, "%u", from_ticks);
        setenv("LOONY_WATCH_FROM", v, 1);
    }
}

void recordings_leave_env(void) {
    unsetenv("LOONY_REPLAY");
    unsetenv("LOONY_WATCH");
    unsetenv("LOONY_AUTO_ALERTS"); /* the replay's own (replay_open) */
    clear_env();
}

bool recordings_deletable(const char *path) {
    const char *live = replay_recording_path();
    return !live || !same_file(path, live);
}

bool recordings_delete(const char *path, recordings_trash_fn trash, char *err, size_t errlen) {
    if (!recordings_deletable(path)) {
        snprintf(err, errlen, "it is being recorded");
        return false;
    }
    const char *watched = watch_replay_path();
    bool watching = watched && same_file(path, watched); /* before it is gone */
    gamestart_take_forget(path);
    if (!trash(path, err, errlen))
        return false;
    log_msg("recordings: moved %s to the Trash", path);
    char list[PATH_MAX], why[PATH_MAX + 64];
    if (clip_list_path(path, list, sizeof list) && access(list, F_OK) == 0) {
        if (trash(list, why, sizeof why))
            log_msg("recordings: moved %s to the Trash", list);
        else /* the recording is gone, and the list is no use without it */
            log_msg("recordings: can't move %s to the Trash: %s", list, why);
    }
    recordings_forget();
    if (watching)
        display_request_close("The recording was moved to the Trash");
    return true;
}
