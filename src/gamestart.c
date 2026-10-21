#include "gamestart.h"

#include <CommonCrypto/CommonDigest.h>
#include <ctype.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <pthread/spawn.h>
#include <errno.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/qos.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cf.h"
#include "events.h"
#include "export.h"
#include "guest_mem.h"
#include "replay.h"
#include "rsrc.h"
#include "seek.h"
#include "snapshot.h"
#include "trap.h"
#include "util.h"

/* The section's header, all little-endian: magic and version (32 bits
   each), the build's UUID (16 bytes), the recording's identity (32), the
   baseline's clock and trap entry (64 bits each) and its hash (32 bytes),
   the game's start's clock and trap entry (64 bits each), the resources
   left out (a count, 32 bits, then for each its index in the fork, its
   guest address and its length, 32 bits each), the snapshot
   (snapshot_encode's bytes, after their length, 64 bits). Version 1
   ended in an HMAC under the install's own key, as only that install's
   were used. */
#define SECTION_MAGIC 0x4C574753u /* 'LWGS' */
#define SECTION_VERSION 2u
#define HASH_LEN CC_SHA256_DIGEST_LENGTH

typedef struct {
    uint32_t entry, addr, len;
} refill;

/* A section, parsed; its refills and body point into the bytes parsed. */
typedef struct {
    uint8_t build[16], identity[HASH_LEN], base_hash[HASH_LEN];
    uint64_t base_clock, base_entries, start_clock, start_entries;
    uint32_t nrefills;
    const uint8_t *refills; /* nrefills * 12 bytes */
    const uint8_t *body;
    size_t body_len;
} section;

static struct {
    bool on;
    bool take; /* LOONY_GAMESTART=take: exit once done */
    trap_entry_fn next; /* the hook this one runs after it (seek's) */
    enum { TO_BASELINE, TO_START, VERIFYING, DONE } phase;
    uint64_t limit_us;  /* gamestart_limit's, or UINT64_MAX */
    snapshot *base;     /* taking: the baseline */
    uint8_t base_hash[HASH_LEN];
    uint64_t base_clock, base_entries;
    snapshot *loaded;   /* verifying: the snapshot decoded */
    uint64_t start_entries;
} G = {.limit_us = UINT64_MAX};

static double secs(uint64_t us) { return (double)us / 1e6; }

static double process_start; /* wall_seconds() before main */

__attribute__((constructor)) static void note_process_start(void) { process_start = wall_seconds(); }

/* This build's UUID (LC_UUID), or zeros. */
static void build_id(uint8_t out[16]) {
    memset(out, 0, 16);
    const struct mach_header_64 *h = (const struct mach_header_64 *)_dyld_get_image_header(0);
    if (!h || h->magic != MH_MAGIC_64)
        return;
    const uint8_t *p = (const uint8_t *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)p;
        if (lc->cmd == LC_UUID) {
            memcpy(out, ((const struct uuid_command *)lc)->uuid, 16);
            return;
        }
        p += lc->cmdsize;
    }
}

typedef struct {
    uint32_t addr, len;
} span;

/* Adds [addr, addr + len) to *v (n of them, room for *cap). */
static void add_span(span **v, size_t *n, size_t *cap, uint64_t addr, uint64_t len) {
    if (*n == *cap) {
        *cap = *cap ? 2 * *cap : 16;
        if (!(*v = realloc(*v, *cap * sizeof **v)))
            fatal("out of memory");
    }
    (*v)[(*n)++] = (span){(uint32_t)addr, (uint32_t)len};
}

/* Where guest memory holds the license, which a baseline's hash leaves
   out: the buffers the game copied it into (cf_kept_out_fields), and
   wherever else its text is, as it is, in capitals or in small letters,
   in the pages held (vec, from mincore). Malloc'd, *n of them. */
static span *license_spans(const char *vec, size_t npages, size_t *n) {
    span *v = NULL;
    size_t cap = 0;
    *n = 0;
    const cf_field *fields;
    size_t nfields = cf_kept_out_fields(&fields);
    for (size_t i = 0; i < nfields; i++)
        add_span(&v, n, &cap, fields[i].addr, fields[i].len);
    size_t page = vm_page_size;
    const uint8_t *base = gm_host_base();
    for (size_t i = 0; i < cf_kept_out_count(); i++) {
        const char *text = cf_kept_out(i);
        size_t len = text ? strlen(text) : 0;
        if (!len || len > 255)
            continue;
        char forms[3][256];
        for (size_t c = 0; c <= len; c++) {
            forms[0][c] = text[c];
            forms[1][c] = (char)toupper((unsigned char)text[c]);
            forms[2][c] = (char)tolower((unsigned char)text[c]);
        }
        /* Each run of pages held, so a copy across two pages is found. */
        for (size_t k = 0; k < npages;) {
            if (!(vec[k] & (MINCORE_INCORE | MINCORE_PAGED_OUT))) {
                k++;
                continue;
            }
            size_t end = k;
            while (end < npages && (vec[end] & (MINCORE_INCORE | MINCORE_PAGED_OUT)))
                end++;
            const uint8_t *lo = base + k * page, *hi = base + end * page;
            for (int f = 0; f < 3; f++)
                for (const uint8_t *at = lo;
                     at < hi && (at = memmem(at, (size_t)(hi - at), forms[f], len)); at += len)
                    add_span(&v, n, &cap, (uint64_t)(at - base), len);
            k = end;
        }
    }
    return v;
}

/* A SHA-256 of guest memory: every page that isn't zeros, after its
   index, with the license blanked (license_spans), so replays of one
   recording under two licenses have the same. A page the host doesn't
   hold (mincore: in memory, or compressed or swapped out) is zeros, and
   isn't read, which would make it resident. */
static void memory_hash(uint8_t out[HASH_LEN]) {
    size_t page = vm_page_size, npages = gm_host_size() / page;
    const uint8_t *base = gm_host_base();
    char *vec = malloc(npages ? npages : 1);
    uint8_t *buf = malloc(page);
    if (!vec || !buf)
        fatal("out of memory");
    if (mincore((void *)(uintptr_t)base, npages * page, vec) != 0)
        memset(vec, MINCORE_INCORE, npages);
    size_t nspans;
    span *spans = license_spans(vec, npages, &nspans);
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    for (size_t k = 0; k < npages; k++) {
        if (!(vec[k] & (MINCORE_INCORE | MINCORE_PAGED_OUT)))
            continue;
        memcpy(buf, base + k * page, page);
        uint64_t lo = (uint64_t)k * page, hi = lo + page;
        for (size_t i = 0; i < nspans; i++) {
            uint64_t a = spans[i].addr, e = a + spans[i].len;
            if (a < lo)
                a = lo;
            if (e > hi)
                e = hi;
            if (a < e)
                memset(buf + (a - lo), 0, (size_t)(e - a));
        }
        size_t i = 0;
        while (i < page && !buf[i])
            i++;
        if (i == page)
            continue;
        uint32_t index = (uint32_t)k;
        CC_SHA256_Update(&c, &index, sizeof index);
        CC_SHA256_Update(&c, buf, (CC_LONG)page);
    }
    CC_SHA256_Final(out, &c);
    free(spans);
    free(buf);
    free(vec);
}

/* ---- the section ---- */

typedef struct {
    uint8_t *p;
    size_t len, cap;
} buf;

static void add(buf *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2;
        if (!(b->p = realloc(b->p, b->cap)))
            fatal("out of memory");
    }
    memcpy(b->p + b->len, p, n);
    b->len += n;
}

static void add_le(buf *b, uint64_t v, int bytes) {
    uint8_t x[8];
    for (int i = 0; i < bytes; i++)
        x[i] = (uint8_t)(v >> (8 * i));
    add(b, x, (size_t)bytes);
}

static uint64_t le(const uint8_t *p, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

/* Parses the section in [p, p + len) into s. False, with why, if it isn't
   one this version reads. */
static bool parse(const uint8_t *p, size_t len, section *s, char *why, size_t cap) {
    size_t fixed = 8 + 16 + HASH_LEN + 16 + HASH_LEN + 16 + 4;
    if (len < fixed + 8 || le(p, 4) != SECTION_MAGIC || le(p + 4, 4) != SECTION_VERSION) {
        snprintf(why, cap, "it isn't one this version of LittleWing reads");
        return false;
    }
    const uint8_t *q = p + 8;
    memcpy(s->build, q, 16);
    q += 16;
    memcpy(s->identity, q, HASH_LEN);
    q += HASH_LEN;
    s->base_clock = le(q, 8);
    s->base_entries = le(q + 8, 8);
    q += 16;
    memcpy(s->base_hash, q, HASH_LEN);
    q += HASH_LEN;
    s->start_clock = le(q, 8);
    s->start_entries = le(q + 8, 8);
    q += 16;
    s->nrefills = (uint32_t)le(q, 4);
    q += 4;
    size_t left = len - (size_t)(q - p);
    if ((uint64_t)s->nrefills * 12 + 8 > left) {
        snprintf(why, cap, "it is damaged");
        return false;
    }
    s->refills = q;
    q += (size_t)s->nrefills * 12;
    uint64_t body = le(q, 8);
    q += 8;
    if (body != len - (size_t)(q - p)) {
        snprintf(why, cap, "it is damaged");
        return false;
    }
    s->body = q;
    s->body_len = (size_t)body;
    return true;
}

/* Whether s was taken by this build, of the recording whose identity is
   identity (replay_identity); if not, why. */
static bool same_recording(const section *s, const uint8_t identity[HASH_LEN], char *why, size_t cap) {
    uint8_t build[16];
    build_id(build);
    if (memcmp(build, s->build, 16) != 0) {
        snprintf(why, cap, "it was taken by another build of LittleWing");
        return false;
    }
    if (memcmp(identity, s->identity, HASH_LEN) != 0) {
        snprintf(why, cap, "it is of another recording");
        return false;
    }
    return true;
}

/* ---- the baseline and the game's start ---- */

/* Stops watching: the hook goes back to the one it runs after. Taking
   alone, the replay has done what it was for. */
static void done(void) {
    G.phase = DONE;
    trap_on_entry(G.next);
    if (G.take) {
        log_msg("gamestart: done taking, at %.3f s", secs(replay_now_us()));
        exit(0);
    }
}

/* The snapshot in the recording, decoded at the baseline (where guest
   memory is what it was encoded against) with its resources filled in;
   NULL, with why, if it can't be used here. */
static snapshot *load(const section *s, char *why, size_t cap) {
    uint8_t identity[HASH_LEN];
    replay_identity(identity);
    if (!same_recording(s, identity, why, cap))
        return NULL;
    if (s->base_clock != G.base_clock || s->base_entries != G.base_entries ||
        memcmp(s->base_hash, G.base_hash, HASH_LEN) != 0) {
        snprintf(why, cap,
                 "the replay's baseline (%.3f s, trap entry %llu) isn't the one it was taken "
                 "against (%.3f s, trap entry %llu): another copy of the game, or another license",
                 secs(G.base_clock), (unsigned long long)G.base_entries, secs(s->base_clock),
                 (unsigned long long)s->base_entries);
        return NULL;
    }
    if (s->start_clock > G.limit_us) {
        snprintf(why, cap, "it is at %.3f s, past %.3f s, which this replay must see",
                 secs(s->start_clock), secs(G.limit_us));
        return NULL;
    }
    char err[256];
    snapshot *snap = snapshot_decode(s->body, s->body_len, err, sizeof err);
    if (!snap) {
        snprintf(why, cap, "%s", err);
        return NULL;
    }
    uint32_t n = rsrc_total();
    for (uint32_t i = 0; i < s->nrefills; i++) {
        const uint8_t *r = s->refills + 12 * i;
        uint32_t entry = (uint32_t)le(r, 4), addr = (uint32_t)le(r + 4, 4), len = (uint32_t)le(r + 8, 4);
        const rsrc_entry *e = entry < n ? rsrc_at(entry) : NULL;
        if (!e || e->len != len || addr > gm_host_size() || len > gm_host_size() - addr) {
            snprintf(why, cap, "its resource %u doesn't match the game's", i);
            snapshot_free(snap);
            return NULL;
        }
        snapshot_poke(snap, addr, rsrc_data(e), len);
    }
    if (!snapshot_brings_no_code(snap, err, sizeof err)) {
        snprintf(why, cap, "%s", err);
        snapshot_free(snap);
        return NULL;
    }
    return snap;
}

/* At the baseline, entering trap index. */
static void at_baseline(uint32_t index) {
    double t0 = wall_seconds();
    memory_hash(G.base_hash);
    G.base_clock = replay_now_us();
    G.base_entries = trap_entries();
    size_t len;
    const uint8_t *p = replay_snapshot(&len);
    char why[512] = "";
    if (p) {
        section s;
        snapshot *snap = parse(p, len, &s, why, sizeof why) ? load(&s, why, sizeof why) : NULL;
        if (snap && seek_verifying()) {
            log_msg("gamestart: verify: decoded the recording's snapshot at the baseline, %.3f s "
                    "after the process started; replaying to %.3f s to compare it",
                    wall_seconds() - process_start, secs(s.start_clock));
            G.loaded = snap;
            G.start_entries = s.start_entries;
            G.phase = VERIFYING;
            return;
        }
        if (snap) {
            log_msg("gamestart: going to the game's start at %.3f s from the baseline at %.3f s, "
                    "%.3f s after the process started (decoded in %.1f ms)",
                    secs(s.start_clock), secs(G.base_clock), wall_seconds() - process_start,
                    (wall_seconds() - t0) * 1000);
            done();
            seek_enter(snap);
        }
        log_msg("gamestart: the recording's snapshot can't be used: %s; replaying from launch", why);
    }
    const replay_span *spans;
    size_t nspans = replay_play_spans(&spans);
    if (!nspans) {
        done();
        return;
    }
    if (spans[0].on_us < GAMESTART_MIN_PRELUDE_US) {
        log_msg("gamestart: the game starts %.3f s after launch, too soon for a snapshot", secs(spans[0].on_us));
        done();
        return;
    }
    G.base = snapshot_take(index);
    G.phase = G.base ? TO_START : DONE;
    if (!G.base)
        done();
}

/* The resources loaded and the same as in the fork, which a snapshot
   leaves out: malloc'd, *n of them, as refills and as ranges. */
static refill *unchanged_resources(snapshot_range **ranges, size_t *n) {
    uint32_t total = rsrc_total();
    refill *out = malloc(((size_t)total + 1) * sizeof *out);
    *ranges = malloc(((size_t)total + 1) * sizeof **ranges);
    if (!out || !*ranges)
        fatal("out of memory");
    *n = 0;
    for (uint32_t i = 0; i < total; i++) {
        const rsrc_entry *e = rsrc_at(i);
        if (!e->handle || !e->len || !gm_is_backed(e->handle, 4))
            continue;
        uint32_t addr = gm_r32(e->handle);
        if (!addr || !gm_is_backed(addr, e->len) || memcmp(gm_ptr(addr, e->len), rsrc_data(e), e->len) != 0)
            continue;
        out[*n] = (refill){i, addr, e->len};
        (*ranges)[*n] = (snapshot_range){addr, e->len};
        (*n)++;
    }
    return out;
}

/* At the game's start, entering trap index: takes the snapshot and keeps
   it in the recording. */
static void at_start(uint32_t index) {
    double t0 = wall_seconds();
    snapshot *s = snapshot_take(index);
    if (!s)
        return; /* the next entry, then */
    for (size_t i = 0; i < cf_kept_out_count(); i++)
        if (cf_kept_out(i) && snapshot_changed_holds(s, G.base, cf_kept_out(i))) {
            log_msg("gamestart: the game wrote the license after the baseline, so no snapshot "
                    "is kept");
            snapshot_free(s);
            snapshot_free(G.base);
            G.base = NULL;
            done();
            return;
        }
    snapshot_range *ranges;
    size_t nrefills;
    refill *refills = unchanged_resources(&ranges, &nrefills);
    size_t body_len;
    uint8_t *body = snapshot_encode(s, G.base, ranges, nrefills, &body_len);
    buf b = {0};
    add_le(&b, SECTION_MAGIC, 4);
    add_le(&b, SECTION_VERSION, 4);
    uint8_t build[16], identity[HASH_LEN];
    build_id(build);
    replay_identity(identity);
    add(&b, build, 16);
    add(&b, identity, HASH_LEN);
    add_le(&b, G.base_clock, 8);
    add_le(&b, G.base_entries, 8);
    add(&b, G.base_hash, HASH_LEN);
    add_le(&b, snapshot_clock_us(s), 8);
    add_le(&b, trap_entries(), 8);
    add_le(&b, nrefills, 4);
    for (size_t i = 0; i < nrefills; i++) {
        add_le(&b, refills[i].entry, 4);
        add_le(&b, refills[i].addr, 4);
        add_le(&b, refills[i].len, 4);
    }
    add_le(&b, body_len, 8);
    add(&b, body, body_len);
    uint64_t left_out = 0;
    for (size_t i = 0; i < nrefills; i++)
        left_out += refills[i].len;
    bool kept = replay_set_snapshot(b.p, b.len);
    if (kept)
        log_msg("gamestart: kept a snapshot of the game's start at %.3f s in the recording: %zu "
                "bytes before compression, %zu resources (%llu bytes) left out, in %.1f ms",
                secs(snapshot_clock_us(s)), b.len, nrefills, (unsigned long long)left_out,
                (wall_seconds() - t0) * 1000);
    free(b.p);
    free(body);
    free(refills);
    free(ranges);
    snapshot_free(s);
    snapshot_free(G.base);
    G.base = NULL;
    done();
}

/* Verifying, at the trap entry where the snapshot was taken. */
static void verify(void) {
    char what[2048];
    if (!snapshot_fits(G.loaded) || !snapshot_verify(G.loaded, what, sizeof what))
        fatal("gamestart: verify: at %.3f s, the replay from launch isn't the recording's snapshot: %s",
              secs(replay_now_us()), snapshot_fits(G.loaded) ? what : "another frame");
    log_msg("gamestart: verify: at %.3f s, the replay from launch is the recording's snapshot",
            secs(replay_now_us()));
    snapshot_free(G.loaded);
    G.loaded = NULL;
    done();
}

static uint32_t on_entry(uint32_t index) {
    if (G.phase == VERIFYING && trap_entries() >= G.start_entries) {
        if (trap_entries() > G.start_entries)
            fatal("gamestart: verify: the replay went past trap entry %llu, where the snapshot "
                  "was taken, without coming to it",
                  (unsigned long long)G.start_entries);
        verify();
    }
    ev_frame f;
    uint32_t loop_index, loop_resume;
    if ((G.phase == TO_BASELINE || G.phase == TO_START) && trap_depth() == 2 &&
        trap_level(1, &loop_index, &loop_resume) &&
        strcmp(trap_import_name(loop_index), "RunApplicationEventLoop") == 0 &&
        events_loop_frame(&f)) {
        const replay_span *spans;
        if (G.phase == TO_BASELINE && f.timer >= 0 && replay_now_us() >= GAMESTART_BASELINE_US)
            at_baseline(index);
        else if (G.phase == TO_START && f.timer < 0 && replay_play_spans(&spans) &&
                 replay_now_us() >= spans[0].on_us)
            at_start(index);
    }
    return G.next ? G.next(index) : index;
}

/* LOONY_GAMESTART: "0", "take" or unset. */
static const char *setting(void) {
    const char *v = getenv("LOONY_GAMESTART");
    return v ? v : "";
}

#define TAKE_TEND_GAP 0.25 /* seconds between polls of a take, or of its parent */

/* Taking: a kqueue that fires when the program that started this one
   ends (-1: not taking), and when gamestart_tend last looked. */
static struct {
    int kq;
    double polled; /* wall clock */
} parent = {.kq = -1};

static void end_with_parent(void) {
    log_msg("gamestart: the program that started this take has ended, so it ends too");
    exit(0);
}

/* Taking: ends this program when the one that started it has, as
   gamestart_take_end would have if that one hadn't crashed or been
   force quit. Otherwise its next launch, pruning its recordings, could
   delete the one this take is about to rename its write over. */
static void watch_parent(void) {
    pid_t ppid = getppid();
    struct kevent ev;
    EV_SET(&ev, ppid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, NULL);
    parent.kq = ppid == 1 ? -1 : kqueue(); /* 1: launchd, it has ended already */
    if (parent.kq >= 0 && kevent(parent.kq, &ev, 1, NULL, 0, NULL) == 0)
        return;
    if (ppid == 1 || errno == ESRCH)
        end_with_parent();
    log_msg("gamestart: can't watch the program that started this take: %s", strerror(errno));
    if (parent.kq >= 0)
        close(parent.kq);
    parent.kq = -1;
}

/* Taking: whether the program that started this one has ended. */
static bool parent_ended(void) {
    double now = wall_seconds();
    if (now - parent.polled < TAKE_TEND_GAP)
        return false;
    parent.polled = now;
    struct kevent ev;
    return kevent(parent.kq, NULL, 0, &ev, 1, &(struct timespec){0, 0}) > 0;
}

void gamestart_init(void) {
    G.on = replay_playing() && strcmp(setting(), "0") != 0;
    G.take = G.on && strcmp(setting(), "take") == 0;
    if (G.take)
        watch_parent();
    if (!G.on)
        return;
    G.phase = TO_BASELINE;
    G.next = trap_entry_hook();
    trap_on_entry(on_entry);
}

bool gamestart_taking(void) { return G.take; }

void gamestart_limit(uint64_t clock_us) {
    if (clock_us < G.limit_us)
        G.limit_us = clock_us;
}

void gamestart_memory_hash(uint8_t out[HASH_LEN]) { memory_hash(out); }

bool gamestart_in_file(const char *path) {
    uint8_t *section, identity[HASH_LEN];
    size_t len;
    char err[256];
    bool has = replay_file_snapshot(path, &section, &len, identity, err, sizeof err) && section;
    free(section);
    return has;
}

/* The second copy taking a snapshot: Share's, or a background one of a
   game just recorded, and the background ones waiting. */
#define TAKE_QUEUE 8
#define TAKE_STOP_WAIT 3.0 /* seconds a take has to end after SIGTERM */
static struct {
    pid_t pid; /* 0: none */
    bool background;
    double stop_by; /* wall clock: SIGKILL then, after stop_later's SIGTERM (0: not stopping) */
    char path[PATH_MAX];
    char share[PATH_MAX]; /* Share's take, waiting for a background one to stop ("": none) */
    char share_self[PATH_MAX];
    char self[PATH_MAX]; /* the program background takes run */
    char queue[TAKE_QUEUE][PATH_MAX]; /* waiting, oldest first */
    size_t nqueue;
    double polled; /* wall clock */
    bool at_exit;
} child;

extern char **environ;

/* Starts the second copy, self, taking a snapshot into path, at utility
   priority if background: below the game being played, which the
   scheduler serves first, but not QOS_CLASS_BACKGROUND, which measured 5
   times slower on an idle Mac (a 35 s prelude in 29.6 s, against 5.3 s). */
static bool spawn(const char *self, const char *path, bool background, char *err, size_t errlen) {
    char replay[PATH_MAX + 16];
    snprintf(replay, sizeof replay, "LOONY_REPLAY=%s", path);
    const char *set[] = {replay, "LOONY_GAMESTART=take", "LOONY_HD=off", "SDL_VIDEO_DRIVER=dummy",
                         "SDL_AUDIO_DRIVER=dummy", "LOONY_LOG_INHERITED=1", NULL};
    char **env = export_env_with(environ, set);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attr);
    /* Only stdin, and stderr (this program's log), reach it. */
    posix_spawn_file_actions_addinherit_np(&actions, STDIN_FILENO);
    posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK |
                                        POSIX_SPAWN_SETSIGDEF);
    if (background)
        posix_spawnattr_set_qos_class_np(&attr, QOS_CLASS_UTILITY);
    pid_t pid = 0;
    int rc = env ? posix_spawn(&pid, self, &actions, &attr, (char *const[]){(char *)self, NULL}, env)
                 : ENOMEM;
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    export_free_env(env);
    if (rc != 0) {
        snprintf(err, errlen, "can't start %s: %s", self, strerror(rc));
        return false;
    }
    child.pid = pid;
    child.background = background;
    snprintf(child.path, sizeof child.path, "%s", path);
    log_msg("gamestart: taking a snapshot of the game's start into %s%s (process %d)", path,
            background ? " in the background" : "", (int)pid);
    return true;
}

/* Removes what a take killed mid-write left: replay_set_snapshot's hidden
   file beside path, ".<name>.XXXXXX". */
static void remove_temp(const char *path) {
    char dir[PATH_MAX], prefix[PATH_MAX + 2];
    const char *slash = strrchr(path, '/');
    snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - path) : 1, slash ? path : ".");
    snprintf(prefix, sizeof prefix, ".%s.", slash ? slash + 1 : path);
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, prefix, strlen(prefix)) != 0 || strlen(e->d_name) != strlen(prefix) + 6)
            continue;
        char file[PATH_MAX * 2];
        snprintf(file, sizeof file, "%s/%s", dir, e->d_name);
        if (unlink(file) == 0)
            log_msg("gamestart: removed %s, which the take left", file);
    }
    if (d)
        closedir(d);
}

/* Collects the running take if it has ended, without waiting: 0 if it
   hasn't, or the result of waitpid, with its status in *st. A take
   stop_later stopped is killed once its TAKE_STOP_WAIT is up, and what
   that left is removed. */
static pid_t reap(int *st) {
    *st = 0;
    pid_t r = waitpid(child.pid, st, WNOHANG);
    if (r == 0 || (r < 0 && errno == EINTR)) {
        if (!child.stop_by || wall_seconds() < child.stop_by)
            return 0;
        log_msg("gamestart: the take of %s hasn't ended, killing it", child.path);
        kill(child.pid, SIGKILL);
        r = waitpid(child.pid, st, 0);
        remove_temp(child.path);
    }
    child.pid = 0;
    child.stop_by = 0;
    return r;
}

/* Asks the running take to end, without waiting: SIGTERM, which waits
   for replay_set_snapshot's rename if it is writing, and reap kills it
   if it hasn't ended in TAKE_STOP_WAIT. */
static void stop_later(void) {
    if (!child.pid || child.stop_by)
        return;
    kill(child.pid, SIGTERM);
    child.stop_by = wall_seconds() + TAKE_STOP_WAIT;
}

/* Ends the running take, waiting for it: stop_later, then reap until it
   has ended. */
static void stop_child(void) {
    stop_later();
    int st;
    while (child.pid && !reap(&st))
        usleep(10000);
}

/* Ends the running take at once: SIGKILL, for a take whose recording is
   going, so whatever it was writing is no loss. Its hidden file is
   removed; a rename it got to first is the file going. */
static void kill_child(void) {
    if (!child.pid)
        return;
    kill(child.pid, SIGKILL);
    waitpid(child.pid, NULL, 0);
    remove_temp(child.path);
    child.pid = 0;
    child.stop_by = 0;
}

/* At exit: the background takes stop with this program. */
static void end_at_exit(void) { gamestart_take_end(); }

bool gamestart_take_start(const char *self, const char *path, char *err, size_t errlen) {
    if ((child.pid && !child.background) || child.share[0]) {
        snprintf(err, errlen, "a snapshot is being taken already");
        return false;
    }
    if (!child.pid)
        return spawn(self, path, false, err, errlen);
    /* Share's goes first, once the background one has stopped (polled by
       gamestart_take_poll): that one starts again after */
    if (!child.stop_by) { /* else a cancelled share's put it off already */
        log_msg("gamestart: putting off the take of %s for a share", child.path);
        if (child.nqueue == TAKE_QUEUE)
            child.nqueue--;
        memmove(child.queue[1], child.queue[0], child.nqueue * sizeof child.queue[0]);
        snprintf(child.queue[0], sizeof child.queue[0], "%s", child.path);
        child.nqueue++;
    }
    snprintf(child.share, sizeof child.share, "%s", path);
    snprintf(child.share_self, sizeof child.share_self, "%s", self);
    stop_later();
    return true;
}

/* Logs a take that ended with no snapshot in its file, of waitpid's r and st. */
static void log_none(pid_t r, int st) {
    log_msg("gamestart: no snapshot was taken into %s (%s %d)", child.path,
            r > 0 && WIFSIGNALED(st) ? "signal" : "status",
            r > 0 ? (WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st)) : -1);
}

/* Logs how a background take, reap's r and st, ended. */
static void log_background(pid_t r, int st) {
    if (gamestart_in_file(child.path))
        log_msg("gamestart: %s has its snapshot of the game's start", child.path);
    else
        log_none(r, st);
}

bool gamestart_take_poll(bool *kept) {
    *kept = false;
    int st = 0;
    if (child.share[0]) {
        if (child.pid) {
            pid_t r = reap(&st);
            if (!r)
                return false;
            log_background(r, st);
        }
        char path[PATH_MAX], err[PATH_MAX + 64];
        snprintf(path, sizeof path, "%s", child.share);
        child.share[0] = '\0';
        if (!spawn(child.share_self, path, false, err, sizeof err)) {
            log_msg("gamestart: can't take a snapshot into %s: %s", path, err);
            return true;
        }
    }
    if (!child.pid || child.background) /* no share's take: a background one isn't polled here */
        return true;
    pid_t r = reap(&st);
    if (!r)
        return false;
    *kept = gamestart_in_file(child.path);
    if (!*kept)
        log_none(r, st);
    return true;
}

void gamestart_take_stop(void) {
    if (child.share[0]) { /* not started: the background one stopping is gamestart_tend's */
        child.share[0] = '\0';
        return;
    }
    stop_child();
}

bool gamestart_take_later(const char *self, const char *path, uint64_t play_on_us) {
    if (strcmp(setting(), "0") == 0)
        return false;
    if (play_on_us == UINT64_MAX) {
        log_msg("gamestart: %s has no play, so no snapshot is taken", path);
        return false;
    }
    if (play_on_us < GAMESTART_MIN_PRELUDE_US) {
        log_msg("gamestart: the game in %s starts %.3f s after launch, too soon for a snapshot", path,
                secs(play_on_us));
        return false;
    }
    if (child.nqueue == TAKE_QUEUE) {
        log_msg("gamestart: %d takes are waiting already, so none of %s", TAKE_QUEUE, path);
        return false;
    }
    snprintf(child.self, sizeof child.self, "%s", self);
    snprintf(child.queue[child.nqueue++], sizeof child.queue[0], "%s", path);
    if (!child.at_exit) {
        child.at_exit = true;
        atexit(end_at_exit);
    }
    child.polled = 0;
    gamestart_tend();
    return true;
}

void gamestart_tend(void) {
    if (parent.kq >= 0 && parent_ended()) /* outside replay_set_snapshot's write */
        end_with_parent();
    if (child.pid && !child.background) /* Share's: it polls it */
        return;
    if (child.pid) {
        double now = wall_seconds();
        if (now - child.polled < TAKE_TEND_GAP)
            return;
        child.polled = now;
        int st = 0;
        pid_t r = reap(&st);
        if (!r)
            return;
        log_background(r, st);
    }
    if (child.share[0]) /* Share's starts first, when it polls */
        return;
    while (child.nqueue && !child.pid) {
        char path[PATH_MAX], err[PATH_MAX + 64];
        snprintf(path, sizeof path, "%s", child.queue[0]);
        memmove(child.queue[0], child.queue[1], --child.nqueue * sizeof child.queue[0]);
        if (!spawn(child.self, path, true, err, sizeof err))
            log_msg("gamestart: can't take a snapshot into %s: %s", path, err);
    }
}

void gamestart_take_forget(const char *path) {
    size_t kept = 0;
    for (size_t i = 0; i < child.nqueue; i++)
        if (!same_file(child.queue[i], path))
            memmove(child.queue[kept++], child.queue[i], sizeof child.queue[0]);
    child.nqueue = kept;
    if (child.pid && same_file(child.path, path)) {
        log_msg("gamestart: stopping the take of %s, which is being deleted", child.path);
        kill_child();
    }
}

void gamestart_take_end(void) {
    child.nqueue = 0;
    child.share[0] = '\0';
    if (child.pid)
        log_msg("gamestart: stopping the take of %s", child.path);
    stop_child();
}
