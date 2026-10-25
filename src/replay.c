#include "replay.h"

#include <CommonCrypto/CommonDigest.h>
#include <copyfile.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "cf.h"
#include "files.h"
#include "game.h"
#include "snapshot.h"
#include "plist.h"
#include "util.h"

#define MAGIC "LWREPLAY"
/* The format this program writes. Format 5 logs where a game starts and
   ends and where it is played (play.h), so each game can have a file of
   its own, and is recorded with the menus' Microseconds and Delay(0) spins
   waited out (misc.c). Format 6 is format 5 that may have a snapshot of
   the game's start after its end record (gamestart.h), followed by the
   trailer: the section's length (8 bytes, little-endian) and
   SECTION_MAGIC. Both are read; the prototype's formats 1 to 4 are not. A
   shared copy is either with FLAG_SHARED. */
#define FORMAT_VERSION 6
#define OLDEST_VERSION 5
#define SECTION_MAGIC "LWSTART6"
#define TRAILER 16
#define FLAG_AUTO_ALERTS 1u
#define FLAG_DST 2u    /* daylight saving time was on when the recording started */
#define FLAG_SHARED 4u /* the license is out of the preferences */

/* The preferences that hold the license (the classic games' names). A
   shared copy has them empty, and a replay of it fills them in from
   LOONY_REPLAY_LICENSE's. */
static const char *const license_keys[] = {"user email", "user id"};
#define NLICENSE_KEYS (sizeof license_keys / sizeof license_keys[0])

enum { OFF, RECORDING, PLAYING };

static const char *const kind_names[REPLAY_KINDS] = {
    "end", "clock", "key", "mouse", "text", "focus", "quit",
    "sound callback", "sound busy", "sound queue full", "frame", "mark", "game", "play",
};

/* Values per record of each kind. */
static const int kind_values[REPLAY_KINDS] = {0, 1, 3, 3, 0, 1, 0, 3, 0, 0, 1, 0, 1, 1};

typedef struct {
    uint64_t step;
    replay_kind kind;
    uint32_t v[REPLAY_MAX_VALUES];
    char text[REPLAY_MAX_TEXT];
} record;

/* A place in a decompressed file. */
typedef struct {
    const uint8_t *data;
    size_t len, pos;
} cursor;

typedef struct {
    char game_id[64];
    uint8_t sha[CC_SHA256_DIGEST_LENGTH];
    uint32_t flags;
    int64_t start_unix; /* the wall clock when the recording started */
    int32_t gmtoff;     /* and its time zone, in seconds east of UTC */
    const uint8_t *prefs;
    size_t prefs_len;
} header;

/* The most a record takes in the file: its step, kind and values as
   varints, and its text with its length. */
#define RECORD_MAX (10 * (2 + REPLAY_MAX_VALUES) + 10 + REPLAY_MAX_TEXT)

typedef struct {
    uint8_t *p;
    size_t len, cap;
} bytes;

static struct {
    int mode;
    uint64_t step;
    uint64_t clock_ms; /* the clock as the game last saw it */
    unsigned presents, frames_checked;
    int marks; /* marks logged, or taken */
    bool at_exit;
    /* recording */
    char auto_dir[PATH_MAX]; /* replay_record_into's folder, or "": a file a game */
    char auto_id[64];        /* and the game's id, which starts the names */
    bytes header;            /* the file's header */
    bytes session;           /* auto: the records since launch that a replay needs */
    uint64_t session_step;   /* the step of its last record */
    char path[PATH_MAX];     /* the file being written */
    gzFile out;              /* or NULL (auto, between games) */
    uint64_t game_play_ms;   /* auto: where play first went on in its file, or UINT64_MAX */
    void (*game_recorded)(const char *path, uint64_t play_on_us);
    uint64_t last_step;
    bool write_failed;
    char cut_text[REPLAY_MAX_TEXT];
    /* replay: the whole file, decompressed, its header and the next record */
    bool loaded;
    bool cut_short; /* the file ends without an end record */
    bool desynced, reached_end;
    double started_wall; /* the wall clock when playing began, for the speed */
    uint8_t *data;
    cursor in;
    header h; /* recording: what replay_calendar needs */
    record next;
    uint32_t *mark_ticks; /* every mark in the file */
    size_t nmark_ticks;
    replay_span *spans; /* where it is played (REPLAY_PLAY records) */
    size_t nspans;
    size_t records_at;    /* where the records start in data (the header's length) */
    bool ended;           /* the records end with an end record */
    uint64_t end_us;      /* the clock at the last record */
    const uint8_t *snap;  /* the snapshot section in data, or NULL */
    size_t snap_len;
    uint8_t identity[CC_SHA256_DIGEST_LENGTH]; /* replay_identity */
    void (*pace)(uint64_t clock_us);
    void (*end)(void);
    void (*desync)(const char *msg);
} R;

/* A snapshot's walk (snapshot.h): the cursor, the step and the counts are
   the replay's; whether this is a recording or a replay, the file, what
   was parsed out of it, the recording's paths and buffers and the
   viewer's hooks are the host's, and stay as they are. The header is the
   host's too, and the cursor is walked as where it is among the records,
   so a snapshot of a recording fits its shared copy, whose header differs
   (replay_identity). */
static void snap_walk(snapshot_walker *w) {
    snapshot_bytes(w, &R, sizeof R);
    SNAPSHOT_HOST(w, R.mode);
    SNAPSHOT_HOST(w, R.auto_dir);
    SNAPSHOT_HOST(w, R.auto_id);
    SNAPSHOT_HOST(w, R.session_step);
    SNAPSHOT_HOST(w, R.path);
    SNAPSHOT_HOST(w, R.last_step);
    SNAPSHOT_HOST(w, R.write_failed);
    SNAPSHOT_HOST(w, R.cut_text);
    SNAPSHOT_HOST(w, R.loaded);
    SNAPSHOT_HOST(w, R.cut_short);
    SNAPSHOT_HOST(w, R.nmark_ticks);
    SNAPSHOT_HOST(w, R.nspans);
    SNAPSHOT_HOST(w, R.ended);
    SNAPSHOT_HOST(w, R.end_us);
    snapshot_check(w, R.next.kind >= 0 && R.next.kind < REPLAY_KINDS &&
                          memchr(R.next.text, 0, sizeof R.next.text),
                   "the next record");
    SNAPSHOT_HOST(w, R.header);
    SNAPSHOT_HOST(w, R.session);
    SNAPSHOT_HOST(w, R.out);
    SNAPSHOT_HOST(w, R.game_play_ms);
    SNAPSHOT_HOST(w, R.game_recorded);
    SNAPSHOT_HOST(w, R.started_wall);
    SNAPSHOT_HOST(w, R.data);
    SNAPSHOT_HOST(w, R.in.data);
    SNAPSHOT_HOST(w, R.in.len);
    SNAPSHOT_HOST(w, R.h);
    SNAPSHOT_HOST(w, R.mark_ticks);
    SNAPSHOT_HOST(w, R.spans);
    SNAPSHOT_HOST(w, R.records_at);
    SNAPSHOT_HOST(w, R.snap);
    SNAPSHOT_HOST(w, R.snap_len);
    SNAPSHOT_HOST(w, R.identity);
    SNAPSHOT_HOST(w, R.pace);
    SNAPSHOT_HOST(w, R.end);
    SNAPSHOT_HOST(w, R.desync);
    uint64_t pos = R.in.pos - R.records_at;
    SNAPSHOT_HOST(w, R.in.pos);
    snapshot_bytes(w, &pos, sizeof pos);
    if (snapshot_loading(w)) {
        if (pos > R.in.len - R.records_at)
            fatal("snapshot: the replay is at byte %llu of its records, which are %zu bytes",
                  (unsigned long long)pos, R.in.len - R.records_at);
        R.in.pos = R.records_at + (size_t)pos;
    }
}
SNAPSHOT_STATE_WALK(R, snap_walk)

bool replay_recording(void) { return R.mode == RECORDING; }

const char *replay_recording_path(void) { return R.mode == RECORDING && R.out ? R.path : NULL; }
bool replay_playing(void) { return R.mode == PLAYING; }

bool replay_calendar(int64_t *start_unix, int32_t *gmtoff, bool *dst) {
    if (R.mode == OFF)
        return false;
    *start_unix = R.h.start_unix;
    *gmtoff = R.h.gmtoff;
    *dst = (R.h.flags & FLAG_DST) != 0;
    return true;
}

void replay_reset(void) {
    if (R.out)
        gzclose(R.out);
    free(R.data);
    free(R.mark_ticks);
    free(R.spans);
    free(R.header.p);
    free(R.session.p);
    memset(&R, 0, sizeof R);
}

static uint32_t tick(void) { return (uint32_t)(R.clock_ms * 60 / 1000); }

static void (*retry)(void); /* replay_set_retry's */

_Noreturn void replay_desync(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    log_msg("replay: desync at step %llu (tick %u): %s", (unsigned long long)R.step, tick(), msg);
    R.desynced = true; /* replay_finish, if retry restarts, says nothing more */
    if (retry)
        retry();
    R.mode = OFF; /* nothing more to report at exit */
    if (R.desync)
        R.desync(msg);
    exit(REPLAY_DESYNC_STATUS);
}

void replay_set_pace(void (*fn)(uint64_t clock_us)) { R.pace = fn; }
void replay_set_end(void (*fn)(void)) { R.end = fn; }
void replay_set_desync(void (*fn)(const char *msg)) { R.desync = fn; }
void replay_set_retry(void (*fn)(void)) { retry = fn; }

bool replay_desynced(void) { return R.desynced; }
bool replay_reached_end(void) { return R.reached_end; }

/* ---- the file ---- */

static size_t varint(uint8_t *out, uint64_t v) {
    size_t n = 0;
    do {
        out[n] = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v)
            out[n] |= 0x80;
        n++;
    } while (v);
    return n;
}

static void bytes_add(bytes *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 > 4096 ? (b->len + n) * 2 : 4096;
        if (!(b->p = realloc(b->p, b->cap)))
            fatal("out of memory");
    }
    memcpy(b->p + b->len, p, n);
    b->len += n;
}

static void bytes_varint(bytes *b, uint64_t v) {
    uint8_t buf[10];
    bytes_add(b, buf, varint(buf, v));
}

/* A record as the file holds it, delta steps after the one before. */
static size_t encode(uint8_t *out, uint64_t delta, replay_kind kind, const uint32_t *values,
                     const char *text, size_t len) {
    size_t n = varint(out, delta);
    n += varint(out + n, (uint64_t)kind);
    for (int i = 0; i < kind_values[kind]; i++)
        n += varint(out + n, values[i]);
    if (kind == REPLAY_TEXT) {
        n += varint(out + n, len);
        memcpy(out + n, text, len);
        n += len;
    }
    return n;
}

/* Writes to the file being recorded, if one is open. */
static void put(const void *p, size_t n) {
    if (R.out && n && gzwrite(R.out, p, (unsigned)n) != (int)n && !R.write_failed) {
        int code;
        log_msg("replay: can't write the recording (%s)", gzerror(R.out, &code));
        R.write_failed = true;
    }
}

/* What is written so far reaches the file. */
static void flush(void) {
    if (R.out && gzflush(R.out, Z_SYNC_FLUSH) != Z_OK && !R.write_failed) {
        log_msg("replay: can't write the recording");
        R.write_failed = true;
    }
}

static bool get_varint(cursor *c, uint64_t *out) {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (c->pos >= c->len)
            return false;
        uint8_t b = c->data[c->pos++];
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return true;
        }
    }
    return false;
}

static bool get_bytes(cursor *c, const uint8_t **out, size_t n) {
    if (n > c->len - c->pos)
        return false;
    *out = c->data + c->pos;
    c->pos += n;
    return true;
}

/* Reads the record at c into r, which holds the one before it. False
   where the file stops early (a recording that crashed before writing its
   end). */
static bool parse_record(cursor *c, record *r) {
    uint64_t delta, kind, v;
    const uint8_t *s;
    if (!get_varint(c, &delta) || !get_varint(c, &kind) || kind >= REPLAY_KINDS)
        return false;
    r->kind = (replay_kind)kind;
    for (int i = 0; i < kind_values[kind]; i++) {
        if (!get_varint(c, &v))
            return false;
        r->v[i] = (uint32_t)v;
    }
    r->text[0] = '\0';
    if (kind == REPLAY_TEXT) {
        if (!get_varint(c, &v) || v >= sizeof r->text || !get_bytes(c, &s, (size_t)v))
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
    if (parse_record(&R.in, &R.next))
        return;
    log_msg("replay: the file ends without an end record (after step %llu)",
            (unsigned long long)R.next.step);
    R.next.kind = REPLAY_END;
    R.cut_short = true;
}

/* What scan finds in a file's records. */
typedef struct {
    uint32_t *mark_ticks; /* the clock's ticks at each mark, malloc'd */
    size_t nmark_ticks;
    replay_span *spans; /* where it is played, malloc'd */
    size_t nspans;
    uint64_t end_ms; /* the clock at the last record */
    bool ended;      /* with an end record */
} scan_result;

/* Finds the marks and the spans of play in the records at c, so a clip
   can start before a mark and a replay can leave out what isn't play: the
   clock records say what the clock was at each. */
static void scan(cursor c, scan_result *out) {
    static record r;
    size_t mark_cap = 0, span_cap = 0;
    uint64_t clock_ms = 0;
    bool on = false;
    memset(&r, 0, sizeof r);
    memset(out, 0, sizeof *out);
    while (parse_record(&c, &r)) {
        if (r.kind == REPLAY_END) {
            out->ended = true;
            break;
        }
        if (r.kind == REPLAY_CLOCK) {
            clock_ms += r.v[0];
        } else if (r.kind == REPLAY_MARK) {
            if (out->nmark_ticks == mark_cap) {
                mark_cap = mark_cap ? mark_cap * 2 : 16;
                if (!(out->mark_ticks = realloc(out->mark_ticks, mark_cap * sizeof *out->mark_ticks)))
                    fatal("out of memory");
            }
            out->mark_ticks[out->nmark_ticks++] = (uint32_t)(clock_ms * 60 / 1000);
        } else if (r.kind == REPLAY_PLAY && (r.v[0] != 0) != on) {
            on = r.v[0] != 0;
            if (on) {
                if (out->nspans == span_cap) {
                    span_cap = span_cap ? span_cap * 2 : 16;
                    if (!(out->spans = realloc(out->spans, span_cap * sizeof *out->spans)))
                        fatal("out of memory");
                }
                out->spans[out->nspans++] = (replay_span){clock_ms * 1000, UINT64_MAX};
            } else {
                out->spans[out->nspans - 1].off_us = clock_ms * 1000;
            }
        }
    }
    /* Play that goes on and off at the same ms is no play. */
    size_t n = 0;
    for (size_t i = 0; i < out->nspans; i++)
        if (out->spans[i].off_us > out->spans[i].on_us)
            out->spans[n++] = out->spans[i];
    out->nspans = n;
    out->end_ms = clock_ms;
}

/* replay_play_us over these spans. */
static uint64_t play_us_in(const replay_span *spans, size_t nspans, uint64_t clock_us) {
    if (!nspans)
        return clock_us;
    uint64_t play = 0;
    for (size_t i = 0; i < nspans && spans[i].on_us < clock_us; i++) {
        uint64_t off = spans[i].off_us < clock_us ? spans[i].off_us : clock_us;
        play += off - spans[i].on_us;
    }
    return play;
}

size_t replay_play_spans(const replay_span **spans) {
    *spans = R.spans;
    return R.nspans;
}

uint64_t replay_play_us(uint64_t clock_us) { return play_us_in(R.spans, R.nspans, clock_us); }

uint64_t replay_clock_us(uint64_t play_us) {
    if (!R.nspans)
        return play_us;
    uint64_t before = 0; /* play in the spans before span i */
    for (size_t i = 0; i < R.nspans; i++) {
        uint64_t len = R.spans[i].off_us - R.spans[i].on_us;
        if (play_us < before + len)
            return R.spans[i].on_us + (play_us - before);
        before += len;
    }
    return R.spans[R.nspans - 1].off_us;
}

/* A tick's start in us, rounded up so it converts back to the same tick. */
static uint64_t us_of_tick(uint32_t ticks) { return ((uint64_t)ticks * 1000000 + 59) / 60; }
static uint32_t tick_of_us(uint64_t us) { return (uint32_t)(us * 60 / 1000000); }

uint32_t replay_play_ticks(uint32_t clock_ticks) {
    return tick_of_us(replay_play_us(us_of_tick(clock_ticks)));
}

uint32_t replay_play_length(void) { return tick_of_us(play_us_in(R.spans, R.nspans, R.end_us)); }

uint32_t replay_clock_ticks(uint32_t play_ticks) {
    uint64_t us = replay_clock_us(us_of_tick(play_ticks));
    return us >= us_of_tick(UINT32_MAX) ? UINT32_MAX : tick_of_us(us);
}

bool replay_in_play(uint64_t clock_us, uint64_t *next_on_us) {
    if (!R.nspans)
        return true;
    for (size_t i = 0; i < R.nspans; i++) {
        if (clock_us < R.spans[i].on_us) {
            *next_on_us = R.spans[i].on_us;
            return false;
        }
        if (clock_us < R.spans[i].off_us)
            return true;
    }
    *next_on_us = UINT64_MAX;
    return false;
}

/* The most a replay decompresses to: shared files come from other people. */
#define MAX_FILE ((size_t)256 << 20)

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
        if (n == cap && cap >= MAX_FILE) { /* an hour is about 2 MB; this is a damaged or hostile file */
            free(buf);
            gzclose(f);
            return NULL;
        }
        if (n == cap)
            buf = realloc(buf, cap *= 2);
    }
    gzclose(f);
    *len = n;
    return buf;
}

/* Reads the header at c into h, whose prefs point into c's data. */
static bool parse_header(cursor *c, header *h, char *err, size_t errlen) {
    const uint8_t *p;
    uint64_t version, id_len, flags, prefs_len;
    memset(h, 0, sizeof *h);
    if (!get_bytes(c, &p, 8) || memcmp(p, MAGIC, 8) != 0) {
        snprintf(err, errlen, "not a replay file");
        return false;
    }
    if (!get_varint(c, &version))
        goto bad;
    if (version < OLDEST_VERSION) {
        snprintf(err, errlen, "the replay was recorded by an early prototype of LittleWing, "
                              "and this version can't play it");
        return false;
    }
    if (version > FORMAT_VERSION) {
        snprintf(err, errlen, "the replay was recorded by a newer version of LittleWing");
        return false;
    }
    if (!get_varint(c, &id_len) || id_len >= sizeof h->game_id || !get_bytes(c, &p, (size_t)id_len))
        goto bad;
    memcpy(h->game_id, p, (size_t)id_len);
    h->game_id[id_len] = '\0';
    if (!get_bytes(c, &p, sizeof h->sha))
        goto bad;
    memcpy(h->sha, p, sizeof h->sha);
    uint64_t start = 0, gmtoff = 0;
    if (!get_varint(c, &flags) ||
        !get_varint(c, &start) || !get_varint(c, &gmtoff) ||
        !get_varint(c, &prefs_len) || !get_bytes(c, &h->prefs, (size_t)prefs_len))
        goto bad;
    h->flags = (uint32_t)flags;
    h->start_unix = (int64_t)start;
    h->gmtoff = (int32_t)(uint32_t)gmtoff;
    /* A start before 2038 and a time zone within a day of UTC, so the dates
       the game sees can't overflow. */
    if (start >= 0x80000000u || gmtoff > UINT32_MAX || h->gmtoff < -86400 || h->gmtoff > 86400)
        goto bad;
    h->prefs_len = (size_t)prefs_len;
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

static uint8_t *with_license(const uint8_t *prefs, size_t len, plist_entry *from, uint32_t nfrom,
                             size_t *out_len, char *err, size_t errlen);

/* Where a decompressed file's records end: before its snapshot section,
   which goes to *section (NULL for none) and *section_len. */
static size_t split_section(const uint8_t *data, size_t len, const uint8_t **section,
                            size_t *section_len) {
    *section = NULL;
    *section_len = 0;
    if (len < 9 || data[8] < 6 || len < TRAILER || memcmp(data + len - 8, SECTION_MAGIC, 8) != 0)
        return len;
    uint64_t n = 0;
    for (int b = 0; b < 8; b++)
        n |= (uint64_t)data[len - TRAILER + b] << (8 * b);
    if (n > len - TRAILER)
        return len;
    *section = data + len - TRAILER - n;
    *section_len = (size_t)n;
    return len - TRAILER - (size_t)n;
}

/* The identity of the recording whose header is h and whose records are
   [records, records + len): a SHA-256 of everything a replay of it sees,
   which a recording and its shared copy have alike (replay.h): the
   header's fields but FLAG_SHARED, the preferences with the license
   emptied, as a shared copy has them, and the records. */
static void identity_of(const header *h, const uint8_t *records, size_t len, uint8_t *out) {
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    uint8_t b[10];
    CC_SHA256_Update(&c, "LWID", 4);
    CC_SHA256_Update(&c, h->game_id, (CC_LONG)strlen(h->game_id) + 1);
    CC_SHA256_Update(&c, h->sha, sizeof h->sha);
    CC_SHA256_Update(&c, b, (CC_LONG)varint(b, h->flags & ~FLAG_SHARED));
    CC_SHA256_Update(&c, b, (CC_LONG)varint(b, (uint64_t)h->start_unix));
    CC_SHA256_Update(&c, b, (CC_LONG)varint(b, (uint32_t)h->gmtoff));
    char err[256];
    size_t plen = 0;
    uint8_t *prefs =
        h->prefs_len ? with_license(h->prefs, h->prefs_len, NULL, 0, &plen, err, sizeof err) : NULL;
    CC_SHA256_Update(&c, b, (CC_LONG)varint(b, prefs ? plen : 0));
    if (prefs)
        CC_SHA256_Update(&c, prefs, (CC_LONG)plen);
    free(prefs);
    CC_SHA256_Update(&c, records, (CC_LONG)len);
    CC_SHA256_Final(out, &c);
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
    R.data = gunzip_file(path, &R.in.len);
    if (!R.data) {
        snprintf(err, errlen, "can't read the replay %s", path);
        return false;
    }
    R.in.data = R.data;
    R.in.len = split_section(R.data, R.in.len, &R.snap, &R.snap_len);
    if (!parse_header(&R.in, &R.h, err, errlen))
        return false;
    R.records_at = R.in.pos;
    scan_result found;
    scan(R.in, &found);
    R.mark_ticks = found.mark_ticks;
    R.nmark_ticks = found.nmark_ticks;
    R.spans = found.spans;
    R.nspans = found.nspans;
    R.ended = found.ended;
    R.end_us = found.end_ms * 1000;
    identity_of(&R.h, R.data + R.records_at, R.in.len - R.records_at, R.identity);
    R.loaded = true;
    const char *game = getenv("LOONY_GAME");
    if (!game || !*game)
        setenv("LOONY_GAME", R.h.game_id, 1);
    const char *license = getenv("LOONY_REPLAY_LICENSE");
    char root[PATH_MAX];
    if ((R.h.flags & FLAG_SHARED) && (!license || !*license) && game_by_id(R.h.game_id) &&
        files_data_root(root, sizeof root)) {
        /* The viewer's own preferences, found before LOONY_DATA_DIR names
           the replay's folder, and kept for a restart (watch.h). */
        char path[PATH_MAX + 96];
        snprintf(path, sizeof path, "%s/%s/prefs.plist", root, R.h.game_id);
        setenv("LOONY_REPLAY_LICENSE", path, 1);
    }
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
    if (R.h.flags & FLAG_AUTO_ALERTS)
        setenv("LOONY_AUTO_ALERTS", "1", 1);
    else
        unsetenv("LOONY_AUTO_ALERTS");
    unsetenv("LOONY_FIXED_CLOCK");
    return true;
}

static void prefs_path(char *out, size_t cap, const char *data_dir) {
    snprintf(out, cap, "%s/prefs.plist", data_dir);
}

static plist_entry *find_entry(plist_entry *e, uint32_t n, const char *key) {
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(e[i].key, key) == 0)
            return &e[i];
    return NULL;
}

/* Whether the preferences hold a license: a string in each license key. */
static bool has_license(plist_entry *e, uint32_t n) {
    for (size_t i = 0; i < NLICENSE_KEYS; i++) {
        plist_entry *k = find_entry(e, n, license_keys[i]);
        if (!k || k->is_number || !k->str[0])
            return false;
    }
    return true;
}

/* The preferences in prefs (len bytes) with the license from `from`'s, or
   empty when from is NULL, as a malloc'd property list (*out_len bytes).
   NULL, with err, if they can't be read or written. */
static uint8_t *with_license(const uint8_t *prefs, size_t len, plist_entry *from, uint32_t nfrom,
                             size_t *out_len, char *err, size_t errlen) {
    plist_entry *e;
    uint32_t n;
    char perr[256];
    if (plist_parse(prefs, len, &e, &n, perr, sizeof perr) != PLIST_OK) {
        snprintf(err, errlen, "the replay's preferences are damaged");
        return NULL;
    }
    if (!(e = realloc(e, (n + NLICENSE_KEYS) * sizeof *e)))
        fatal("out of memory");
    for (size_t i = 0; i < NLICENSE_KEYS; i++) {
        plist_entry *src = from ? find_entry(from, nfrom, license_keys[i]) : NULL;
        plist_entry *dst = find_entry(e, n, license_keys[i]);
        if (!dst) {
            dst = &e[n++];
            memset(dst, 0, sizeof *dst);
            if (!(dst->key = strdup(license_keys[i])))
                fatal("out of memory");
        }
        free(dst->str);
        dst->is_number = false;
        if (!(dst->str = strdup(src && !src->is_number ? src->str : "")))
            fatal("out of memory");
    }
    uint8_t *out = plist_encode(e, n, out_len);
    plist_free(e, n);
    if (!out)
        snprintf(err, errlen, "can't write the replay's preferences");
    return out;
}

/* A shared replay's preferences with the viewer's license in them (from
   LOONY_REPLAY_LICENSE), malloc'd; NULL, with err, without one. */
static uint8_t *licensed_prefs(size_t *len, char *err, size_t errlen) {
    const char *from = getenv("LOONY_REPLAY_LICENSE");
    const game_info *g = game_by_id(R.h.game_id);
    const char *title = g ? g->title : R.h.game_id;
    if (!from || !*from) {
        snprintf(err, errlen,
                 "this replay was shared without a license: set LOONY_REPLAY_LICENSE to the "
                 "preferences of a registered %s",
                 title);
        return NULL;
    }
    plist_entry *e;
    uint32_t n;
    char perr[256];
    plist_status st = plist_read(from, &e, &n, perr, sizeof perr);
    bool licensed = st == PLIST_OK && has_license(e, n);
    uint8_t *out = licensed ? with_license(R.h.prefs, R.h.prefs_len, e, n, len, err, errlen) : NULL;
    if (st == PLIST_OK)
        plist_free(e, n);
    if (!licensed)
        snprintf(err, errlen,
                 "this replay was shared without a license: register %s in LittleWing to watch "
                 "it (%s has none)",
                 title, from);
    return out;
}

static bool start_playing(const char *game_id, const uint8_t *sha, const char *data_dir, char *err,
                          size_t errlen) {
    if (strcmp(game_id, R.h.game_id) != 0) {
        snprintf(err, errlen, "the replay is of %s, not %s", R.h.game_id, game_id);
        return false;
    }
    if (memcmp(sha, R.h.sha, sizeof R.h.sha) != 0) {
        snprintf(err, errlen, "the replay was recorded with another version of %s's program",
                 R.h.game_id);
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
    const uint8_t *prefs = R.h.prefs;
    size_t prefs_len = R.h.prefs_len;
    uint8_t *licensed = NULL;
    if (R.h.flags & FLAG_SHARED) {
        if (!(licensed = licensed_prefs(&prefs_len, err, errlen)))
            return false;
        prefs = licensed;
    }
    if (prefs_len) {
        FILE *f = fopen(path, "wbx");
        bool ok = f && fwrite(prefs, 1, prefs_len, f) == prefs_len;
        if (f && fclose(f) != 0)
            ok = false;
        if (!ok) {
            free(licensed);
            snprintf(err, errlen, "can't write %s", path);
            return false;
        }
    }
    free(licensed);
    cf_keep_out_of_snapshots(license_keys, NLICENSE_KEYS);
    read_next();
    R.mode = PLAYING;
    R.started_wall = wall_seconds();
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
    bytes *h = &R.header;
    bytes_add(h, MAGIC, 8);
    bytes_varint(h, FORMAT_VERSION);
    bytes_varint(h, strlen(game_id));
    bytes_add(h, game_id, strlen(game_id));
    bytes_add(h, sha, CC_SHA256_DIGEST_LENGTH);
    const char *alerts = getenv("LOONY_AUTO_ALERTS");
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    R.h.start_unix = (int64_t)now;
    R.h.gmtoff = (int32_t)lt.tm_gmtoff;
    R.h.flags = (alerts && strcmp(alerts, "1") == 0 ? FLAG_AUTO_ALERTS : 0) | (lt.tm_isdst > 0 ? FLAG_DST : 0);
    bytes_varint(h, R.h.flags);
    bytes_varint(h, (uint64_t)R.h.start_unix);
    bytes_varint(h, (uint32_t)R.h.gmtoff);
    bytes_varint(h, prefs ? prefs_len : 0);
    if (prefs && prefs_len)
        bytes_add(h, prefs, prefs_len);
    free(prefs);
    R.mode = RECORDING;
    if (!path) { /* each game gets a file when it starts */
        log_msg("replay: recording each game into %s", R.auto_dir);
        return true;
    }
    snprintf(R.path, sizeof R.path, "%s", path);
    R.out = gzopen(path, "wb6");
    if (!R.out) {
        R.mode = OFF;
        snprintf(err, errlen, "can't create the replay %s", path);
        return false;
    }
    put(h->p, h->len);
    flush(); /* the header, so even a crash at once replays */
    log_msg("replay: recording to %s", path);
    return true;
}

static void finish_at_exit(void) { replay_finish(); }

/* Which game a recording was in the second it started, from what follows
   the time in its name: the nth for "-<n>" (open_game_file), else the first. */
static long game_in_second(const char *rest) {
    return rest[0] == '-' && rest[1] >= '1' && rest[1] <= '9' ? strtol(rest + 1, NULL, 10) : 1;
}

static size_t time_end; /* by_start's: where the time ends in each name */

/* Oldest first: by the time in the name, then by the game in that second.
   (By name alone, "-2" would come before the second's first game: '-'
   sorts before '.'.) */
static int by_start(const void *a, const void *b) {
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    if (strlen(x) < time_end || strlen(y) < time_end)
        return strcmp(x, y);
    int c = strncmp(x, y, time_end);
    if (c)
        return c;
    long m = game_in_second(x + time_end), n = game_in_second(y + time_end);
    return m < n ? -1 : m > n ? 1 : strcmp(x, y);
}

/* When the recording name in dir was running: from the time in its name
   (after "<game_id>-") to its last write, give or take REPLAY_SCORE_SLACK. */
static bool recording_span(const char *dir, const char *name, size_t id_len, time_t *from,
                           time_t *to) {
    struct tm tm = {.tm_isdst = -1};
    char path[PATH_MAX];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (!strptime(name + id_len + 1, "%Y-%m-%d-%H%M%S", &tm) || stat(path, &st) != 0)
        return false;
    *from = mktime(&tm) - REPLAY_SCORE_SLACK;
    *to = st.st_mtime + REPLAY_SCORE_SLACK;
    return true;
}

/* Whether the recording name in dir was running when one of the
   high_scores was set. */
static bool set_a_high_score(const char *dir, const char *name, size_t id_len,
                             const time_t *high_scores, size_t n) {
    time_t from, to;
    if (!recording_span(dir, name, id_len, &from, &to))
        return false;
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

/* The names of the game's recordings in dir, malloc'd, oldest first (the
   names start with the date and time; by_start). Returns how many. */
static size_t list_recordings(const char *dir, const char *game_id, char ***out) {
    *out = NULL;
    DIR *d = opendir(dir);
    if (!d)
        return 0;
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
    time_end = id_len + strlen("-YYYY-mm-dd-HHMMSS");
    if (n)
        qsort(names, n, sizeof *names, by_start);
    *out = names;
    return n;
}

/* Deletes the game's recordings in dir but the newest keep, unless marked,
   with clips or holding a high score. */
static void prune(const char *dir, const char *game_id, size_t keep, const time_t *high_scores,
                  size_t nscores) {
    char **names;
    size_t n = list_recordings(dir, game_id, &names), id_len = strlen(game_id);
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

bool replay_find_recording(const char *dir, const char *game_id, time_t when, char *out,
                           size_t cap) {
    char **names;
    size_t n = list_recordings(dir, game_id, &names), id_len = strlen(game_id);
    bool found = false;
    for (size_t i = n; i-- > 0;) {
        time_t from, to;
        if (!found && recording_span(dir, names[i], id_len, &from, &to) && when >= from && when <= to) {
            snprintf(out, cap, "%s/%s", dir, names[i]);
            found = true;
        }
        free(names[i]);
    }
    free(names);
    return found;
}

/* Whether text, or text in capitals or in small letters, is in [p, p + n). */
static bool mentions(const uint8_t *p, size_t n, const char *text) {
    size_t len = strlen(text);
    if (!len || len > 255)
        return false;
    char forms[3][256];
    for (size_t i = 0; i <= len; i++) {
        forms[0][i] = text[i];
        forms[1][i] = (char)toupper((unsigned char)text[i]);
        forms[2][i] = (char)tolower((unsigned char)text[i]);
    }
    for (int k = 0; k < 3; k++)
        if (memmem(p, n, forms[k], len))
            return true;
    return false;
}

/* Whether [p, p + n) holds the license in h's preferences, or the one
   this replay gave the game (cf_kept_out). */
static bool holds_license(const uint8_t *p, size_t n, const header *h) {
    plist_entry *e;
    uint32_t ne;
    char perr[256];
    bool found = false;
    if (h->prefs_len && plist_parse(h->prefs, h->prefs_len, &e, &ne, perr, sizeof perr) == PLIST_OK) {
        for (size_t i = 0; i < NLICENSE_KEYS && !found; i++) {
            plist_entry *k = find_entry(e, ne, license_keys[i]);
            found = k && !k->is_number && mentions(p, n, k->str);
        }
        plist_free(e, ne);
    }
    for (size_t i = 0; i < NLICENSE_KEYS && !found; i++) {
        const char *v = cf_kept_out(i);
        found = v && mentions(p, n, v);
    }
    return found;
}

/* Writes a snapshot section and its trailer. */
static bool write_section(gzFile f, const uint8_t *section, size_t len) {
    uint8_t trailer[TRAILER];
    for (int b = 0; b < 8; b++)
        trailer[b] = (uint8_t)((uint64_t)len >> (8 * b));
    memcpy(trailer + 8, SECTION_MAGIC, 8);
    size_t done = 0;
    while (done < len) { /* gzwrite takes an unsigned count */
        unsigned n = len - done > (1u << 30) ? 1u << 30 : (unsigned)(len - done);
        if (gzwrite(f, section + done, n) != (int)n)
            return false;
        done += n;
    }
    return gzwrite(f, trailer, TRAILER) == TRAILER;
}

const uint8_t *replay_snapshot(size_t *len) {
    *len = R.snap_len;
    return R.snap;
}

void replay_identity(uint8_t out[32]) { memcpy(out, R.identity, sizeof R.identity); }

bool replay_set_snapshot(const uint8_t *section, size_t len) {
    const char *path = getenv("LOONY_REPLAY");
    if (R.mode != PLAYING || !path || !*path)
        return false;
    if (!R.ended) {
        log_msg("replay: the recording has no end record, so no snapshot is kept in it");
        return false;
    }
    if (section && holds_license(section, len, &R.h)) {
        log_msg("replay: the snapshot of the game's start holds the license, so it isn't kept");
        return false;
    }
    /* A hidden file beside it, renamed over it, with its mode, its extended
       attributes and its last write kept, all before the rename, so no
       moment shows another: that time says which high score it holds
       (replay.h). The signals that end a replay wait until it is done, so
       none leaves the hidden file behind. */
    sigset_t ending, was;
    sigemptyset(&ending);
    sigaddset(&ending, SIGTERM);
    sigaddset(&ending, SIGINT);
    sigaddset(&ending, SIGHUP);
    sigprocmask(SIG_BLOCK, &ending, &was);
    struct stat st;
    char tmp[PATH_MAX + 16];
    const char *slash = strrchr(path, '/');
    if (slash)
        snprintf(tmp, sizeof tmp, "%.*s/.%s.XXXXXX", (int)(slash - path), path, slash + 1);
    else
        snprintf(tmp, sizeof tmp, ".%s.XXXXXX", path);
    int fd = stat(path, &st) == 0 ? mkstemp(tmp) : -1;
    if (fd < 0) {
        log_msg("replay: can't keep the snapshot in %s: %s", path, strerror(errno));
        sigprocmask(SIG_SETMASK, &was, NULL);
        return false;
    }
    fchmod(fd, st.st_mode & 0777);
    gzFile f = gzdopen(fd, "wb6");
    uint8_t version = FORMAT_VERSION; /* the header as it is, but its format */
    bool ok = f && gzwrite(f, R.data, 8) == 8 && gzwrite(f, &version, 1) == 1 &&
              gzwrite(f, R.data + 9, (unsigned)(R.records_at - 9)) == (int)(R.records_at - 9) &&
              gzwrite(f, R.data + R.records_at, (unsigned)(R.in.len - R.records_at)) ==
                  (int)(R.in.len - R.records_at) &&
              (!section || write_section(f, section, len));
    if (f ? gzclose(f) != Z_OK : close(fd) != 0)
        ok = false;
    struct timespec times[2] = {st.st_atimespec, st.st_mtimespec};
    ok = ok && copyfile(path, tmp, NULL, COPYFILE_XATTR | COPYFILE_SECURITY) == 0 &&
         utimensat(AT_FDCWD, tmp, times, 0) == 0 && rename(tmp, path) == 0;
    if (!ok) {
        log_msg("replay: can't keep the snapshot in %s", path);
        unlink(tmp);
    }
    sigprocmask(SIG_SETMASK, &was, NULL);
    return ok;
}

bool replay_share(const char *src, const char *dst, char *err, size_t errlen) {
    size_t len;
    uint8_t *data = gunzip_file(src, &len);
    if (!data) {
        snprintf(err, errlen, "can't read %s", src);
        return false;
    }
    const uint8_t *section;
    size_t section_len, records_end = split_section(data, len, &section, &section_len);
    cursor c = {data, records_end, 0};
    header h;
    uint8_t *prefs = NULL;
    size_t prefs_len = 0;
    gzFile f = NULL;
    bool made = false; /* tmp is ours to delete */
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    if (!parse_header(&c, &h, err, errlen))
        goto fail;
    if (h.flags & FLAG_SHARED) {
        prefs_len = h.prefs_len;
        if (!(prefs = malloc(prefs_len ? prefs_len : 1)))
            fatal("out of memory");
        memcpy(prefs, h.prefs, prefs_len);
    } else {
        /* A game played unregistered can't be shared: its replay, given the
           viewer's license, would not start the way it did. */
        plist_entry *e;
        uint32_t n;
        char perr[256];
        bool licensed = h.prefs_len && plist_parse(h.prefs, h.prefs_len, &e, &n, perr, sizeof perr) == PLIST_OK;
        if (licensed) {
            licensed = has_license(e, n);
            plist_free(e, n);
        }
        if (!licensed) {
            const game_info *g = game_by_id(h.game_id);
            snprintf(err, errlen, "this game was played before %s was registered, so it can't be shared",
                     g ? g->title : h.game_id);
            goto fail;
        }
        if (!(prefs = with_license(h.prefs, h.prefs_len, NULL, 0, &prefs_len, err, errlen)))
            goto fail;
    }
    if (!(f = gzopen(tmp, "wb6"))) {
        snprintf(err, errlen, "can't create %s", tmp);
        goto fail;
    }
    made = true;
    uint8_t b[10];
    size_t id_len = strlen(h.game_id);
    bool ok = gzwrite(f, MAGIC, 8) == 8;
    ok = ok && gzwrite(f, b, (unsigned)varint(b, FORMAT_VERSION)) > 0;
    ok = ok && gzwrite(f, b, (unsigned)varint(b, id_len)) > 0;
    ok = ok && gzwrite(f, h.game_id, (unsigned)id_len) == (int)id_len;
    ok = ok && gzwrite(f, h.sha, sizeof h.sha) == (int)sizeof h.sha;
    ok = ok && gzwrite(f, b, (unsigned)varint(b, h.flags | FLAG_SHARED)) > 0;
    ok = ok && gzwrite(f, b, (unsigned)varint(b, (uint64_t)h.start_unix)) > 0;
    ok = ok && gzwrite(f, b, (unsigned)varint(b, (uint32_t)h.gmtoff)) > 0;
    ok = ok && gzwrite(f, b, (unsigned)varint(b, prefs_len)) > 0;
    ok = ok && (!prefs_len || gzwrite(f, prefs, (unsigned)prefs_len) == (int)prefs_len);
    /* The records as they are; a recording still being written stops where
       it was last flushed, and the copy plays to there. Then the snapshot
       of the game's start, which fits the copy as it does the recording
       (replay_identity), unless the license is in it as it stands. */
    size_t rest = records_end - c.pos;
    ok = ok && (!rest || gzwrite(f, data + c.pos, (unsigned)rest) == (int)rest);
    if (section && holds_license(section, section_len, &h)) {
        log_msg("replay: the snapshot of the game's start in %s holds the license, so the shared "
                "copy leaves it out",
                src);
        section = NULL;
    }
    ok = ok && (!section || write_section(f, section, section_len));
    if (gzclose(f) != Z_OK)
        ok = false;
    f = NULL;
    if (!ok || rename(tmp, dst) != 0) {
        snprintf(err, errlen, "can't write %s", dst);
        goto fail;
    }
    free(prefs);
    free(data);
    log_msg("replay: shared %s as %s, without its license, %s a snapshot of the game's start", src, dst,
            section ? "with" : "without");
    return true;
fail:
    if (f)
        gzclose(f);
    if (made)
        unlink(tmp);
    free(prefs);
    free(data);
    return false;
}

bool replay_file_snapshot(const char *path, uint8_t **section, size_t *len,
                          uint8_t identity[32], char *err, size_t errlen) {
    *section = NULL;
    *len = 0;
    size_t n;
    uint8_t *data = gunzip_file(path, &n);
    if (!data) {
        snprintf(err, errlen, "it can't be read");
        return false;
    }
    const uint8_t *snap;
    size_t snap_len;
    cursor c = {data, split_section(data, n, &snap, &snap_len), 0};
    header h;
    if (!parse_header(&c, &h, err, errlen)) {
        free(data);
        return false;
    }
    identity_of(&h, data + c.pos, c.len - c.pos, identity);
    if (snap) {
        if (!(*section = malloc(snap_len ? snap_len : 1)))
            fatal("out of memory");
        memcpy(*section, snap, snap_len);
        *len = snap_len;
    }
    free(data);
    return true;
}

bool replay_check(const char *path, char *err, size_t errlen) {
    size_t len;
    uint8_t *data = gunzip_file(path, &len);
    if (!data) {
        snprintf(err, errlen, "it can't be read");
        return false;
    }
    cursor c = {data, len, 0};
    header h;
    bool ok = parse_header(&c, &h, err, errlen);
    const game_info *g = ok ? game_by_id(h.game_id) : NULL;
    if (ok && !g) {
        snprintf(err, errlen, "it is of a game LittleWing doesn't know");
        ok = false;
    }
    if (ok && (h.flags & FLAG_SHARED)) {
        /* Where replay_open will look, once LOONY_DATA_DIR is unset. */
        const char *home = getenv("HOME");
        char prefs[PATH_MAX], perr[256];
        plist_entry *e;
        uint32_t n;
        snprintf(prefs, sizeof prefs, "%s/Library/Application Support/littlewing-shim/%s/prefs.plist",
                 home ? home : "", h.game_id);
        plist_status st = plist_read(prefs, &e, &n, perr, sizeof perr);
        ok = st == PLIST_OK && has_license(e, n);
        if (st == PLIST_OK)
            plist_free(e, n);
        if (!ok)
            snprintf(err, errlen, "it was shared without a license: register %s in LittleWing to watch it",
                     g->title);
    }
    free(data);
    return ok;
}

bool replay_summarize(const char *path, replay_summary *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    size_t len;
    uint8_t *data = gunzip_file(path, &len);
    if (!data) {
        snprintf(err, errlen, "it can't be read");
        return false;
    }
    const uint8_t *section;
    size_t section_len;
    cursor c = {data, split_section(data, len, &section, &section_len), 0};
    header h;
    if (!parse_header(&c, &h, err, errlen)) {
        free(data);
        return false;
    }
    scan_result found;
    scan(c, &found);
    free(data);
    snprintf(out->game_id, sizeof out->game_id, "%s", h.game_id);
    out->start_unix = h.start_unix;
    out->gmtoff = h.gmtoff;
    /* Play to the last record: a span still open there ends with it. */
    out->play_ticks = tick_of_us(play_us_in(found.spans, found.nspans, found.end_ms * 1000));
    for (size_t i = 0; i < found.nmark_ticks; i++)
        found.mark_ticks[i] =
            tick_of_us(play_us_in(found.spans, found.nspans, us_of_tick(found.mark_ticks[i])));
    out->marks = found.mark_ticks;
    out->nmarks = found.nmark_ticks;
    free(found.spans);
    return true;
}

void replay_summary_free(replay_summary *s) {
    free(s->marks);
    s->marks = NULL;
    s->nmarks = 0;
}

void replay_record_into(const char *dir, const char *game_id, const time_t *high_scores, size_t n) {
    const char *rec = getenv("LOONY_RECORD"), *fixed = getenv("LOONY_FIXED_CLOCK");
    if ((rec && *rec) || R.loaded || (fixed && strcmp(fixed, "1") == 0))
        return;
    if (!make_dirs(dir)) {
        log_msg("replay: can't create %s: no game is recorded", dir);
        return;
    }
    prune(dir, game_id, REPLAY_KEEP_GAMES, high_scores, n);
    snprintf(R.auto_dir, sizeof R.auto_dir, "%s", dir);
    snprintf(R.auto_id, sizeof R.auto_id, "%s", game_id);
}

/* A game starts: its file, named for now, gets the header and what a replay
   needs from launch to here, then the game as it goes. */
static void open_game_file(void) {
    char stamp[32];
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    strftime(stamp, sizeof stamp, "%Y-%m-%d-%H%M%S", &lt);
    for (int i = 1;; i++) { /* two games in one second */
        char base[PATH_MAX], marked[PATH_MAX + 16];
        if (i == 1)
            snprintf(base, sizeof base, "%s/%s-%s", R.auto_dir, R.auto_id, stamp);
        else
            snprintf(base, sizeof base, "%s/%s-%s-%d", R.auto_dir, R.auto_id, stamp, i);
        snprintf(R.path, sizeof R.path, "%s.lwreplay", base);
        snprintf(marked, sizeof marked, "%s-marked.lwreplay", base);
        if (access(R.path, F_OK) != 0 && access(marked, F_OK) != 0)
            break;
    }
    R.write_failed = false;
    R.marks = 0;
    R.game_play_ms = UINT64_MAX;
    R.out = gzopen(R.path, "wb6");
    if (!R.out) {
        log_msg("replay: can't create %s: this game isn't recorded", R.path);
        return;
    }
    put(R.header.p, R.header.len);
    put(R.session.p, R.session.len);
    R.last_step = R.session_step;
    flush();
    log_msg("replay: recording the game to %s", R.path);
}

static void close_file(void) {
    if (gzclose(R.out) != Z_OK)
        log_msg("replay: can't finish the recording");
    R.out = NULL;
}

/* Ends a game's file with its end record and closes it (saying so, if
   say), and says it is recorded (replay_set_game_recorded). */
static void end_game_file(bool say) {
    replay_log(REPLAY_END, NULL);
    close_file();
    if (say)
        log_msg("replay: recorded the game to %s", R.path);
    if (R.game_recorded && !R.write_failed)
        R.game_recorded(R.path, R.game_play_ms == UINT64_MAX ? UINT64_MAX : R.game_play_ms * 1000);
}

bool replay_start(const char *game_id, const uint8_t *exe, size_t exe_len, const char *data_dir,
                  char *err, size_t errlen) {
    const char *rec = record_env();
    if (!R.loaded && !rec && !R.auto_dir[0])
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
        if (R.out) {
            replay_log(REPLAY_END, NULL);
            close_file();
            log_msg("replay: recorded %llu steps", (unsigned long long)R.step);
        }
    } else if (R.mode == PLAYING && !R.desynced) {
        /* The game quit where it did when it was recorded. */
        if (R.next.kind == REPLAY_END && R.next.step == R.step) {
            R.reached_end = true;
            log_msg("replay: reached %s at step %llu (tick %u), %u frames checked, no desync",
                    R.cut_short ? "where the recording was cut short" : "the end of the recording",
                    (unsigned long long)R.step, tick(), R.frames_checked);
        } else {
            log_msg("replay: stopped at step %llu (tick %u), before the end of the recording",
                    (unsigned long long)R.step, tick());
        }
        /* The speed: paced by the viewer (watch.h), or as fast as it can. */
        double took = wall_seconds() - R.started_wall, game = (double)R.clock_ms / 1000;
        if (took > 0 && game > 0)
            log_msg("replay: %.1f s of the game's clock in %.2f s on the wall clock (%.1fx)", game,
                    took, game / took);
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

uint64_t replay_now_us(void) { return R.clock_ms * 1000; }

/* A record at this step: into the file being written, and (but for what a
   replay doesn't need) into the session the next game's file starts with. */
static void emit(replay_kind kind, const uint32_t *values, const char *text, size_t len) {
    uint8_t buf[RECORD_MAX];
    if (R.out) {
        put(buf, encode(buf, R.step - R.last_step, kind, values, text, len));
        R.last_step = R.step;
    }
    if (R.auto_dir[0] && kind != REPLAY_MARK && kind != REPLAY_GAME && kind != REPLAY_PLAY &&
        kind != REPLAY_END) {
        bytes_add(&R.session, buf, encode(buf, R.step - R.session_step, kind, values, text, len));
        R.session_step = R.step;
    }
}

void replay_log(replay_kind kind, const uint32_t *values) {
    if (R.mode == RECORDING)
        emit(kind, values, NULL, 0);
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
    emit(REPLAY_TEXT, NULL, utf8, n);
    return utf8;
}

int replay_mark(void) {
    if (R.mode != RECORDING || !R.out)
        return 0;
    replay_log(REPLAY_MARK, NULL);
    R.marks++;
    flush(); /* kept even if the game crashes before the next frame check */
    log_msg("replay: mark %d at tick %u", R.marks, tick());
    if (R.marks == 1 && R.auto_dir[0]) {
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

void replay_game(bool started) {
    if (R.mode != RECORDING)
        return;
    if (started && R.auto_dir[0]) {
        if (R.out) /* the last game's file, if it never ended */
            end_game_file(false);
        open_game_file();
    }
    replay_log(REPLAY_GAME, (uint32_t[1]){started});
    if (!started && R.auto_dir[0] && R.out)
        end_game_file(true);
}

void replay_play(bool on) {
    if (on && R.mode == RECORDING && R.out && R.game_play_ms == UINT64_MAX)
        R.game_play_ms = R.clock_ms;
    replay_log(REPLAY_PLAY, (uint32_t[1]){on});
}

void replay_set_game_recorded(void (*fn)(const char *path, uint64_t play_on_us)) { R.game_recorded = fn; }

void replay_no_games(void) {
    if (R.mode != RECORDING || !R.auto_dir[0])
        return;
    log_msg("replay: games can't be told apart, so none is recorded");
    free(R.session.p);
    memset(&R.session, 0, sizeof R.session);
    free(R.header.p);
    memset(&R.header, 0, sizeof R.header);
    R.auto_dir[0] = '\0';
    R.mode = OFF;
}

bool replay_take_play(void) {
    uint32_t v;
    if (replay_take(REPLAY_GAME, &v)) {
        log_msg("replay: the game %s at tick %u", v ? "starts" : "ends", tick());
        return true;
    }
    return replay_take(REPLAY_PLAY, &v);
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
