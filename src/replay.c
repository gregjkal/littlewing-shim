#include "replay.h"

#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "util.h"

#define MAGIC "LWREPLAY"
/* Format 1 had no start time or time zone: its replays see the host's
   calendar, which only the dates the game saves depend on, never its frames.
   Format 3 added marks, which format 2's reader would take for the end. */
#define FORMAT_VERSION 3
#define OLDEST_FORMAT 1
#define FLAG_AUTO_ALERTS 1u
#define FLAG_DST 2u /* daylight saving time was on when the recording started */

enum { OFF, RECORDING, PLAYING };

static const char *const kind_names[REPLAY_KINDS] = {
    "end", "clock", "key", "mouse", "text", "focus", "quit",
    "sound callback", "sound busy", "sound queue full", "frame", "mark",
};

/* Values per record of each kind. */
static const int kind_values[REPLAY_KINDS] = {0, 1, 3, 3, 0, 1, 0, 3, 0, 0, 1, 0};

typedef struct {
    uint64_t step;
    replay_kind kind;
    uint32_t v[REPLAY_MAX_VALUES];
    char text[REPLAY_MAX_TEXT];
} record;

static struct {
    int mode;
    uint64_t step;
    uint64_t clock_ms; /* the clock as the game last saw it */
    unsigned presents, frames_checked;
    int marks; /* marks logged, or taken */
    bool at_exit;
    /* recording */
    char auto_path[PATH_MAX]; /* replay_record_into's file, or "" */
    char path[PATH_MAX];      /* the file being written */
    gzFile out;
    uint64_t last_step;
    bool write_failed;
    char cut_text[REPLAY_MAX_TEXT];
    /* replay: the whole file, decompressed, and the next record */
    bool loaded;
    bool cut_short; /* the file ends without an end record */
    bool desynced, reached_end;
    uint8_t *data;
    size_t len, pos;
    char game_id[64];
    uint8_t sha[CC_SHA256_DIGEST_LENGTH];
    int version; /* the file's format */
    uint32_t flags;
    int64_t start_unix; /* the wall clock when the recording started */
    int32_t gmtoff;     /* and its time zone, in seconds east of UTC */
    const uint8_t *prefs;
    size_t prefs_len;
    record next;
    uint32_t *mark_ticks; /* every mark in the file */
    size_t nmark_ticks;
    void (*pace)(uint64_t clock_us);
    void (*end)(void);
} R;

bool replay_recording(void) { return R.mode == RECORDING; }
bool replay_playing(void) { return R.mode == PLAYING; }

bool replay_calendar(int64_t *start_unix, int32_t *gmtoff, bool *dst) {
    if (R.mode == OFF || (R.mode == PLAYING && R.version < 2))
        return false;
    *start_unix = R.start_unix;
    *gmtoff = R.gmtoff;
    *dst = (R.flags & FLAG_DST) != 0;
    return true;
}

void replay_reset(void) {
    if (R.out)
        gzclose(R.out);
    free(R.data);
    free(R.mark_ticks);
    memset(&R, 0, sizeof R);
}

static uint32_t tick(void) { return (uint32_t)(R.clock_ms * 60 / 1000); }

_Noreturn void replay_desync(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    log_msg("replay: desync at step %llu (tick %u): %s", (unsigned long long)R.step, tick(), msg);
    R.mode = OFF; /* nothing more to report at exit */
    R.desynced = true;
    exit(REPLAY_DESYNC_STATUS);
}

void replay_set_pace(void (*fn)(uint64_t clock_us)) { R.pace = fn; }
void replay_set_end(void (*fn)(void)) { R.end = fn; }

bool replay_desynced(void) { return R.desynced; }
bool replay_reached_end(void) { return R.reached_end; }

/* ---- the file ---- */

static void put(const void *p, size_t n) {
    if (n && gzwrite(R.out, p, (unsigned)n) != (int)n && !R.write_failed) {
        int code;
        log_msg("replay: can't write the recording (%s)", gzerror(R.out, &code));
        R.write_failed = true;
    }
}

static void put_varint(uint64_t v) {
    uint8_t b[10];
    int n = 0;
    do {
        b[n] = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v)
            b[n] |= 0x80;
        n++;
    } while (v);
    put(b, (size_t)n);
}

/* What is written so far reaches the file. */
static void flush(void) {
    if (gzflush(R.out, Z_SYNC_FLUSH) != Z_OK && !R.write_failed) {
        log_msg("replay: can't write the recording");
        R.write_failed = true;
    }
}

static bool get_varint(uint64_t *out) {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (R.pos >= R.len)
            return false;
        uint8_t b = R.data[R.pos++];
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return true;
        }
    }
    return false;
}

static bool get_bytes(const uint8_t **out, size_t n) {
    if (n > R.len - R.pos)
        return false;
    *out = R.data + R.pos;
    R.pos += n;
    return true;
}

/* Reads the record at R.pos into r, which holds the one before it. False
   where the file stops early (a recording that crashed before writing its
   end). */
static bool parse_record(record *r) {
    uint64_t delta, kind, v;
    const uint8_t *s;
    if (!get_varint(&delta) || !get_varint(&kind) || kind >= REPLAY_KINDS)
        return false;
    r->kind = (replay_kind)kind;
    for (int i = 0; i < kind_values[kind]; i++) {
        if (!get_varint(&v))
            return false;
        r->v[i] = (uint32_t)v;
    }
    r->text[0] = '\0';
    if (kind == REPLAY_TEXT) {
        if (!get_varint(&v) || v >= sizeof r->text || !get_bytes(&s, (size_t)v))
            return false;
        memcpy(r->text, s, (size_t)v);
        r->text[v] = '\0';
    }
    r->step += delta;
    return true;
}

/* Reads the next record into R.next. A file that stops early ends as if it
   had an end record. */
static void read_next(void) {
    if (parse_record(&R.next))
        return;
    log_msg("replay: the file ends without an end record (after step %llu)",
            (unsigned long long)R.next.step);
    R.next.kind = REPLAY_END;
    R.cut_short = true;
}

/* Finds the ticks of the marks ahead of the replay, so a clip can start
   before them: the clock records say what the clock was at each. */
static void scan_marks(void) {
    static record r;
    size_t pos = R.pos, cap = 0;
    uint64_t clock_ms = 0;
    memset(&r, 0, sizeof r);
    while (parse_record(&r) && r.kind != REPLAY_END) {
        if (r.kind == REPLAY_CLOCK) {
            clock_ms += r.v[0];
        } else if (r.kind == REPLAY_MARK) {
            if (R.nmark_ticks == cap) {
                cap = cap ? cap * 2 : 16;
                if (!(R.mark_ticks = realloc(R.mark_ticks, cap * sizeof *R.mark_ticks)))
                    fatal("out of memory");
            }
            R.mark_ticks[R.nmark_ticks++] = (uint32_t)(clock_ms * 60 / 1000);
        }
    }
    R.pos = pos;
}

static uint8_t *gunzip_file(const char *path, size_t *len) {
    gzFile f = gzopen(path, "rb");
    if (!f)
        return NULL;
    size_t cap = 1 << 16, n = 0;
    uint8_t *buf = malloc(cap);
    for (;;) {
        if (!buf)
            fatal("out of memory");
        /* A stream cut short (a recording that crashed) reads to where it stops. */
        int got = gzread(f, buf + n, (unsigned)(cap - n));
        if (got < 0) {
            free(buf);
            gzclose(f);
            return NULL;
        }
        if (got == 0)
            break;
        n += (size_t)got;
        if (n == cap)
            buf = realloc(buf, cap *= 2);
    }
    gzclose(f);
    *len = n;
    return buf;
}

static bool parse_header(char *err, size_t errlen) {
    const uint8_t *p;
    uint64_t version, id_len, flags, prefs_len;
    if (!get_bytes(&p, 8) || memcmp(p, MAGIC, 8) != 0) {
        snprintf(err, errlen, "not a replay file");
        return false;
    }
    if (!get_varint(&version))
        goto bad;
    if (version < OLDEST_FORMAT || version > FORMAT_VERSION) {
        snprintf(err, errlen, "replay format %llu (this program reads %d to %d)",
                 (unsigned long long)version, OLDEST_FORMAT, FORMAT_VERSION);
        return false;
    }
    R.version = (int)version;
    if (!get_varint(&id_len) || id_len >= sizeof R.game_id || !get_bytes(&p, (size_t)id_len))
        goto bad;
    memcpy(R.game_id, p, (size_t)id_len);
    R.game_id[id_len] = '\0';
    if (!get_bytes(&p, sizeof R.sha))
        goto bad;
    memcpy(R.sha, p, sizeof R.sha);
    uint64_t start = 0, gmtoff = 0;
    if (!get_varint(&flags) ||
        (R.version >= 2 && (!get_varint(&start) || !get_varint(&gmtoff))) ||
        !get_varint(&prefs_len) || !get_bytes(&R.prefs, (size_t)prefs_len))
        goto bad;
    R.flags = (uint32_t)flags;
    R.start_unix = (int64_t)start;
    R.gmtoff = (int32_t)(uint32_t)gmtoff;
    /* A start before 2038 and a time zone within a day of UTC, so the dates
       the game sees can't overflow. */
    if (start >= 0x80000000u || gmtoff > UINT32_MAX || R.gmtoff < -86400 || R.gmtoff > 86400)
        goto bad;
    R.prefs_len = (size_t)prefs_len;
    return true;
bad:
    snprintf(err, errlen, "the replay's header is damaged");
    return false;
}

/* LOONY_RECORD, or NULL when it is unset, empty or 0 (off). */
static const char *record_env(void) {
    const char *rec = getenv("LOONY_RECORD");
    return rec && *rec && strcmp(rec, "0") != 0 ? rec : NULL;
}

bool replay_open(char *err, size_t errlen) {
    const char *path = getenv("LOONY_REPLAY");
    if (!path || !*path)
        return true;
    if (record_env()) {
        snprintf(err, errlen, "LOONY_RECORD and LOONY_REPLAY can't be used together");
        return false;
    }
    replay_reset();
    R.data = gunzip_file(path, &R.len);
    if (!R.data) {
        snprintf(err, errlen, "can't read the replay %s", path);
        return false;
    }
    if (!parse_header(err, errlen))
        return false;
    scan_marks();
    R.loaded = true;
    const char *game = getenv("LOONY_GAME");
    if (!game || !*game)
        setenv("LOONY_GAME", R.game_id, 1);
    const char *data = getenv("LOONY_DATA_DIR");
    if (!data || !*data) {
        const char *t = getenv("TMPDIR");
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s/lwreplay-XXXXXX", t && *t ? t : "/tmp");
        if (!mkdtemp(dir)) {
            snprintf(err, errlen, "can't create a folder for the replay's saves");
            return false;
        }
        setenv("LOONY_DATA_DIR", dir, 1);
    }
    if (R.flags & FLAG_AUTO_ALERTS)
        setenv("LOONY_AUTO_ALERTS", "1", 1);
    else
        unsetenv("LOONY_AUTO_ALERTS");
    unsetenv("LOONY_FIXED_CLOCK");
    return true;
}

static void prefs_path(char *out, size_t cap, const char *data_dir) {
    snprintf(out, cap, "%s/prefs.plist", data_dir);
}

static bool start_playing(const char *game_id, const uint8_t *sha, const char *data_dir, char *err,
                          size_t errlen) {
    if (strcmp(game_id, R.game_id) != 0) {
        snprintf(err, errlen, "the replay is of %s, not %s", R.game_id, game_id);
        return false;
    }
    if (memcmp(sha, R.sha, sizeof R.sha) != 0) {
        snprintf(err, errlen, "the replay was recorded with another version of %s's program",
                 R.game_id);
        return false;
    }
    if (!data_dir) {
        snprintf(err, errlen, "a replay needs a save folder (LOONY_DATA_DIR)");
        return false;
    }
    char path[PATH_MAX];
    prefs_path(path, sizeof path, data_dir);
    if (!make_dirs(data_dir)) {
        snprintf(err, errlen, "can't create %s", data_dir);
        return false;
    }
    if (access(path, F_OK) == 0) { /* someone's own preferences: their license and scores */
        snprintf(err, errlen, "%s already has preferences: replay into a new folder", data_dir);
        return false;
    }
    if (R.prefs_len) {
        FILE *f = fopen(path, "wbx");
        if (!f || fwrite(R.prefs, 1, R.prefs_len, f) != R.prefs_len) {
            if (f)
                fclose(f);
            snprintf(err, errlen, "can't write %s", path);
            return false;
        }
        fclose(f);
    }
    read_next();
    R.mode = PLAYING;
    log_msg("replay: playing %s, saving in %s", getenv("LOONY_REPLAY"), data_dir);
    return true;
}

static bool start_recording(const char *path, const char *game_id, const uint8_t *sha,
                            const char *data_dir, char *err, size_t errlen) {
    const char *fixed = getenv("LOONY_FIXED_CLOCK");
    if (fixed && strcmp(fixed, "1") == 0) {
        snprintf(err, errlen, "LOONY_RECORD records play on the real clock, not LOONY_FIXED_CLOCK=1");
        return false;
    }
    size_t prefs_len = 0;
    uint8_t *prefs = NULL;
    if (data_dir) {
        char p[PATH_MAX];
        prefs_path(p, sizeof p, data_dir);
        prefs = read_file(p, &prefs_len);
    }
    snprintf(R.path, sizeof R.path, "%s", path);
    R.out = gzopen(path, "wb6");
    if (!R.out) {
        free(prefs);
        snprintf(err, errlen, "can't create the replay %s", path);
        return false;
    }
    put(MAGIC, 8);
    put_varint(FORMAT_VERSION);
    put_varint(strlen(game_id));
    put(game_id, strlen(game_id));
    put(sha, CC_SHA256_DIGEST_LENGTH);
    const char *alerts = getenv("LOONY_AUTO_ALERTS");
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    R.start_unix = (int64_t)now;
    R.gmtoff = (int32_t)lt.tm_gmtoff;
    R.flags = (alerts && strcmp(alerts, "1") == 0 ? FLAG_AUTO_ALERTS : 0) | (lt.tm_isdst > 0 ? FLAG_DST : 0);
    put_varint(R.flags);
    put_varint((uint64_t)R.start_unix);
    put_varint((uint32_t)R.gmtoff);
    put_varint(prefs ? prefs_len : 0);
    if (prefs && prefs_len)
        put(prefs, prefs_len);
    free(prefs);
    if (gzflush(R.out, Z_SYNC_FLUSH) != Z_OK) /* the header, so even a crash at once replays */
        log_msg("replay: can't write the recording");
    R.mode = RECORDING;
    log_msg("replay: recording to %s", path);
    return true;
}

static void finish_at_exit(void) { replay_finish(); }

static int by_name(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool ends_with(const char *s, const char *end) {
    size_t n = strlen(s), m = strlen(end);
    return n >= m && strcmp(s + n - m, end) == 0;
}

/* Whether the recording name in dir was running when one of the
   high_scores was set: from the time in its name (after "<game_id>-") to its
   last write. */
static bool set_a_high_score(const char *dir, const char *name, size_t id_len,
                             const time_t *high_scores, size_t n) {
    struct tm tm = {.tm_isdst = -1};
    char path[PATH_MAX];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (!strptime(name + id_len + 1, "%Y-%m-%d-%H%M%S", &tm) || stat(path, &st) != 0)
        return false;
    time_t from = mktime(&tm) - REPLAY_SCORE_SLACK, to = st.st_mtime + REPLAY_SCORE_SLACK;
    for (size_t i = 0; i < n; i++)
        if (high_scores[i] >= from && high_scores[i] <= to)
            return true;
    return false;
}

/* Whether the recording name in dir has a list of clips beside it. */
static bool has_clips(const char *dir, const char *name) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%.*s.clips", dir, (int)(strlen(name) - strlen(".lwreplay")), name);
    return access(path, F_OK) == 0;
}

/* Deletes the game's recordings in dir but the newest keep, unless marked,
   with clips or holding a high score. The names start with the date and
   time, so they sort oldest first. */
static void prune(const char *dir, const char *game_id, size_t keep, const time_t *high_scores,
                  size_t nscores) {
    DIR *d = opendir(dir);
    if (!d)
        return;
    char **names = NULL;
    size_t n = 0, cap = 0, id_len = strlen(game_id);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, game_id, id_len) != 0 || e->d_name[id_len] != '-' ||
            !ends_with(e->d_name, ".lwreplay"))
            continue;
        if (n == cap && !(names = realloc(names, (cap = cap ? cap * 2 : 16) * sizeof *names)))
            fatal("out of memory");
        if (!(names[n++] = strdup(e->d_name)))
            fatal("out of memory");
    }
    closedir(d);
    qsort(names, n, sizeof *names, by_name);
    for (size_t i = 0; i < n; i++) {
        if (i + keep < n && !ends_with(names[i], "-marked.lwreplay") && !has_clips(dir, names[i]) &&
            !set_a_high_score(dir, names[i], id_len, high_scores, nscores)) {
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s/%s", dir, names[i]);
            if (unlink(path) == 0)
                log_msg("replay: deleted the old recording %s", path);
            else
                log_msg("replay: can't delete the old recording %s", path);
        }
        free(names[i]);
    }
    free(names);
}

void replay_record_into(const char *dir, const char *game_id, const time_t *high_scores, size_t n) {
    const char *rec = getenv("LOONY_RECORD"), *fixed = getenv("LOONY_FIXED_CLOCK");
    if ((rec && *rec) || R.loaded || (fixed && strcmp(fixed, "1") == 0))
        return;
    if (!make_dirs(dir)) {
        log_msg("replay: can't create %s: this game isn't recorded", dir);
        return;
    }
    prune(dir, game_id, REPLAY_KEEP_GAMES - 1, high_scores, n);
    char stamp[32];
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    strftime(stamp, sizeof stamp, "%Y-%m-%d-%H%M%S", &lt);
    for (int i = 1;; i++) { /* two games in one second */
        char base[PATH_MAX], marked[PATH_MAX + 16];
        if (i == 1)
            snprintf(base, sizeof base, "%s/%s-%s", dir, game_id, stamp);
        else
            snprintf(base, sizeof base, "%s/%s-%s-%d", dir, game_id, stamp, i);
        snprintf(R.auto_path, sizeof R.auto_path, "%s.lwreplay", base);
        snprintf(marked, sizeof marked, "%s-marked.lwreplay", base);
        if (access(R.auto_path, F_OK) != 0 && access(marked, F_OK) != 0)
            return;
    }
}

bool replay_start(const char *game_id, const uint8_t *exe, size_t exe_len, const char *data_dir,
                  char *err, size_t errlen) {
    const char *rec = record_env();
    if (!rec && R.auto_path[0])
        rec = R.auto_path;
    if (!R.loaded && !rec)
        return true;
    uint8_t sha[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(exe, (CC_LONG)exe_len, sha);
    bool ok;
    if (R.loaded)
        ok = start_playing(game_id, sha, data_dir, err, errlen);
    else
        ok = start_recording(rec, game_id, sha, data_dir, err, errlen);
    if (ok && R.mode != OFF && !R.at_exit) {
        R.at_exit = true;
        atexit(finish_at_exit);
    }
    return ok;
}

void replay_finish(void) {
    if (R.mode == RECORDING) {
        replay_log(REPLAY_END, NULL);
        if (gzclose(R.out) != Z_OK)
            log_msg("replay: can't finish the recording");
        R.out = NULL;
        log_msg("replay: recorded %llu steps", (unsigned long long)R.step);
    } else if (R.mode == PLAYING) {
        if (R.next.kind == REPLAY_END && R.next.step == R.step)
            log_msg("replay: reached %s at step %llu (tick %u), %u frames checked, no desync",
                    R.cut_short ? "where the recording was cut short" : "the end of the recording",
                    (unsigned long long)R.step, tick(), R.frames_checked);
        else
            log_msg("replay: stopped at step %llu (tick %u), before the end of the recording",
                    (unsigned long long)R.step, tick());
    }
    R.mode = OFF;
}

/* ---- steps and records ---- */

void replay_step(void) {
    if (R.mode == OFF)
        return;
    R.step++;
    if (R.mode != PLAYING)
        return;
    if (R.next.kind == REPLAY_END && R.step > R.next.step) {
        R.step--; /* where the recording stopped */
        R.reached_end = true;
        if (R.end)
            R.end();
        log_msg("replay: exiting where the recording stopped");
        exit(0);
    }
    if (R.next.step < R.step)
        replay_desync("the %s record of step %llu wasn't used", kind_names[R.next.kind],
                      (unsigned long long)R.next.step);
}

uint64_t replay_clock(uint64_t host_us) {
    replay_step();
    if (R.mode == RECORDING) {
        uint64_t ms = host_us / 1000;
        if (ms > R.clock_ms) {
            uint32_t d = (uint32_t)(ms - R.clock_ms);
            replay_log(REPLAY_CLOCK, &d);
            R.clock_ms = ms;
        }
    } else if (R.mode == PLAYING) {
        uint32_t d;
        if (replay_take(REPLAY_CLOCK, &d)) {
            if (R.pace)
                R.pace((R.clock_ms + d) * 1000);
            R.clock_ms += d;
        }
    }
    return R.clock_ms * 1000;
}

void replay_log(replay_kind kind, const uint32_t *values) {
    if (R.mode != RECORDING)
        return;
    put_varint(R.step - R.last_step);
    put_varint((uint64_t)kind);
    for (int i = 0; i < kind_values[kind]; i++)
        put_varint(values[i]);
    R.last_step = R.step;
}

const char *replay_log_text(const char *utf8) {
    if (R.mode != RECORDING)
        return utf8;
    size_t n = strlen(utf8);
    if (n >= REPLAY_MAX_TEXT) {
        n = REPLAY_MAX_TEXT - 1;
        while (n > 0 && ((uint8_t)utf8[n] & 0xC0) == 0x80) /* not inside a character */
            n--;
        memcpy(R.cut_text, utf8, n);
        R.cut_text[n] = '\0';
        utf8 = R.cut_text;
    }
    replay_log(REPLAY_TEXT, NULL);
    put_varint(n);
    put(utf8, n);
    return utf8;
}

int replay_mark(void) {
    if (R.mode != RECORDING)
        return 0;
    replay_log(REPLAY_MARK, NULL);
    R.marks++;
    flush(); /* kept even if the game crashes before the next frame check */
    log_msg("replay: mark %d at tick %u", R.marks, tick());
    if (R.marks == 1 && R.auto_path[0] && strcmp(R.path, R.auto_path) == 0) {
        /* Marked: kept for good. The open file follows the rename. */
        char marked[PATH_MAX + 16];
        snprintf(marked, sizeof marked, "%.*s-marked.lwreplay",
                 (int)(strlen(R.path) - strlen(".lwreplay")), R.path);
        if (rename(R.path, marked) == 0) {
            log_msg("replay: kept as %s", marked);
            snprintf(R.path, sizeof R.path, "%s", marked);
        } else {
            log_msg("replay: can't rename %s to %s", R.path, marked);
        }
    }
    return R.marks;
}

bool replay_take_mark(void) {
    if (!replay_take(REPLAY_MARK, NULL))
        return false;
    log_msg("replay: mark %d at tick %u", ++R.marks, tick());
    return true;
}

size_t replay_marks(const uint32_t **ticks) {
    *ticks = R.mark_ticks;
    return R.nmark_ticks;
}

bool replay_take(replay_kind kind, uint32_t *values) {
    if (R.mode != PLAYING || R.next.kind != kind || R.next.step != R.step)
        return false;
    for (int i = 0; i < kind_values[kind]; i++)
        values[i] = R.next.v[i];
    read_next();
    return true;
}

bool replay_take_text(char *out, size_t cap) {
    if (R.mode != PLAYING || R.next.kind != REPLAY_TEXT || R.next.step != R.step)
        return false;
    snprintf(out, cap, "%s", R.next.text);
    read_next();
    return true;
}

bool replay_frame_due(void) {
    return R.mode != OFF && ++R.presents % REPLAY_FRAME_EVERY == 0;
}

void replay_frame(uint32_t hash) {
    if (R.mode == RECORDING) {
        replay_log(REPLAY_FRAME, &hash);
        flush(); /* at each frame check, so a recording that crashes replays to here */
        return;
    }
    if (R.mode != PLAYING)
        return;
    uint32_t want;
    if (!replay_take(REPLAY_FRAME, &want))
        replay_desync("present %u has no frame check in the recording (next: %s at step %llu)",
                      R.presents, kind_names[R.next.kind], (unsigned long long)R.next.step);
    if (want != hash)
        replay_desync("present %u: the screen's hash is %08x, not %08x as recorded", R.presents,
                      hash, want);
    R.frames_checked++;
}
