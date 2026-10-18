#include "seek.h"

#include <mach/mach.h>
#include <stdlib.h>
#include <string.h>

#include "replay.h"
#include "snapshot.h"
#include "trap.h"
#include "util.h"

jmp_buf seek_unwound;

typedef struct {
    snapshot *s;
    uint64_t clock_us;
    bool in_play;
    uint64_t next_on_us; /* outside play: where it goes on next (replay_in_play) */
} entry;

static struct {
    bool on;
    seek_landed_fn landed;
    entry e[SEEK_MAX_SNAPSHOTS + 1]; /* in the clock's order */
    int n;
    uint64_t due_us; /* the clock from which the next snapshot is taken */
    bool pending;
    uint64_t target_us;
    unsigned generation;
    snapshot *entering; /* what seek_reenter puts back */
    uint64_t from_us;   /* and where the replay's clock was when it unwound */
    int unwound;        /* and from what depth */
    double unwound_wall;
} K;

static double secs(uint64_t us) { return (double)us / 1e6; }

/* The last snapshot at or before clock_us, the first if every one is
   later, or -1 if there is none. */
static int nearest(uint64_t clock_us) {
    if (K.n == 0)
        return -1;
    int i = K.n - 1;
    while (i > 0 && K.e[i].clock_us > clock_us)
        i--;
    return i;
}

static void drop(int i) {
    snapshot_free(K.e[i].s);
    memmove(&K.e[i], &K.e[i + 1], (size_t)(K.n - i - 1) * sizeof K.e[0]);
    K.n--;
}

/* Past the most kept: the one whose neighbours are closest goes. */
static void thin(void) {
    int best = -1;
    uint64_t gap = UINT64_MAX;
    for (int i = 1; i + 1 < K.n; i++) {
        uint64_t g = K.e[i + 1].clock_us - K.e[i - 1].clock_us;
        if (g < gap) {
            gap = g;
            best = i;
        }
    }
    if (best < 0)
        return;
    log_msg("seek: %d snapshots, past the most kept: thinning out the one at %.3f s, which leaves "
            "%.1f s between its neighbours",
            K.n, secs(K.e[best].clock_us), secs(gap));
    drop(best);
}

/* A snapshot is due at clock now, entering trap index: takes one, unless
   the store has one less than SEEK_SPACING_US before now. */
static void take(uint32_t index, uint64_t now) {
    int last = nearest(now);
    if (last >= 0 && K.e[last].clock_us <= now && now < K.e[last].clock_us + SEEK_SPACING_US) {
        K.due_us = K.e[last].clock_us + SEEK_SPACING_US;
        return;
    }
    double t0 = wall_seconds();
    snapshot *s = snapshot_take(index);
    if (!s) /* not where one can be taken: the next trap entry */
        return;
    double ms = (wall_seconds() - t0) * 1000;
    uint64_t next_on;
    bool in_play = replay_in_play(now, &next_on);
    int i = K.n;
    while (i > 0 && K.e[i - 1].clock_us > now)
        i--;
    /* Outside play, only the latest of the stretch is kept. */
    if (!in_play && i > 0 && !K.e[i - 1].in_play && K.e[i - 1].next_on_us == next_on) {
        drop(i - 1);
        i--;
    }
    memmove(&K.e[i + 1], &K.e[i], (size_t)(K.n - i) * sizeof K.e[0]);
    K.e[i] = (entry){s, now, in_play, in_play ? 0 : next_on};
    K.n++;
    K.due_us = now + SEEK_SPACING_US;
    log_msg("seek: snapshot at %.3f s of the replay's clock%s, taken in %.2f ms (%d kept)",
            secs(now), in_play ? "" : ", outside play", ms, K.n);
    if (K.n > SEEK_MAX_SNAPSHOTS)
        thin();
}

static void after_reentry(void) {
    uint64_t clock = replay_now_us();
    K.due_us = clock + SEEK_SPACING_US;
    log_msg("seek: put back the snapshot at %.3f s from main, from %.3f s (%d guest call%s "
            "unwound), in %.2f ms",
            secs(clock), secs(K.from_us), K.unwound, K.unwound == 1 ? "" : "s",
            (wall_seconds() - K.unwound_wall) * 1000);
    K.landed(clock, true, K.generation);
}

/* Unwinds to main, where seek_reenter puts snapshot i back. */
static _Noreturn void unwind_to(int i, uint64_t now) {
    K.entering = K.e[i].s;
    K.from_us = now;
    K.unwound = trap_depth();
    K.unwound_wall = wall_seconds();
    longjmp(seek_unwound, 1);
}

/* The pending seek, at a trap entry at depth 2 with the replay's clock at
   now, entering trap index: returns the trap to dispatch. */
static uint32_t serve(uint32_t index, uint64_t now) {
    K.pending = false;
    int i = nearest(K.target_us);
    if (i < 0 || (K.target_us >= now && K.e[i].clock_us <= now)) {
        log_msg("seek: to %.3f s from %.3f s: no snapshot between, going on", secs(K.target_us),
                secs(now));
        K.landed(now, false, K.generation);
        return index;
    }
    snapshot *s = K.e[i].s;
    if (!snapshot_fits(s)) {
        log_msg("seek: to %.3f s from %.3f s: the snapshot at %.3f s was taken in another frame, "
                "unwinding to main",
                secs(K.target_us), secs(now), secs(K.e[i].clock_us));
        unwind_to(i, now);
    }
    double t0 = wall_seconds();
    index = snapshot_restore(s);
    K.due_us = K.e[i].clock_us + SEEK_SPACING_US;
    log_msg("seek: to %.3f s from %.3f s: put back the snapshot at %.3f s in place, in %.2f ms",
            secs(K.target_us), secs(now), secs(K.e[i].clock_us), (wall_seconds() - t0) * 1000);
    K.landed(K.e[i].clock_us, true, K.generation);
    return index;
}

static uint32_t on_entry(uint32_t index) {
    if (trap_depth() != 2)
        return index;
    uint64_t now = replay_now_us();
    if (K.pending)
        return serve(index, now);
    if (now >= K.due_us)
        take(index, now);
    return index;
}

/* At exit: what the store came to. */
static void log_store(void) {
    if (!K.n)
        return;
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        log_msg("seek: %d snapshots kept, from %.3f to %.3f s; the process's footprint %.1f MB",
                K.n, secs(K.e[0].clock_us), secs(K.e[K.n - 1].clock_us),
                (double)info.phys_footprint / (1024 * 1024));
}

void seek_start(seek_landed_fn landed) {
    if (!K.on)
        atexit(log_store);
    K.on = true;
    K.landed = landed;
    trap_on_entry(on_entry);
}

unsigned seek_to(uint64_t clock_us) {
    K.pending = true;
    K.target_us = clock_us;
    return ++K.generation;
}

bool seek_pending(void) { return K.pending; }

bool seek_snapshot_ahead(uint64_t after_us, uint64_t to_us) {
    for (int i = 0; i < K.n; i++)
        if (K.e[i].clock_us > after_us && K.e[i].clock_us <= to_us)
            return true;
    return false;
}

void seek_serve_here(void) {
    if (!K.on || !K.pending)
        return;
    K.pending = false;
    uint64_t now = replay_now_us();
    int i = nearest(K.target_us);
    if (i < 0) {
        log_msg("seek: to %.3f s: no snapshot to go to", secs(K.target_us));
        K.landed(now, false, K.generation);
        return;
    }
    log_msg("seek: to %.3f s from %.3f s, where the replay has ended: unwinding to main",
            secs(K.target_us), secs(now));
    unwind_to(i, now);
}

void seek_reenter(void) {
    snapshot *s = K.entering;
    K.entering = NULL;
    if (!s)
        fatal("seek: back in main with no snapshot to put back");
    snapshot_reenter(s, after_reentry);
}

void seek_reset(void) {
    while (K.n)
        drop(K.n - 1);
    if (K.on)
        trap_on_entry(NULL);
    memset(&K, 0, sizeof K);
}
