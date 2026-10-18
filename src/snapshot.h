#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A snapshot of the emulator, for seeking in a replay (seek.h, #44). One is
   taken at a trap entry at depth 2, in one of the two places the run loop
   (RunApplicationEventLoop, at depth 1) calls the game: a handler it
   dispatched an event to, where Loony's games run, or a timer its pump
   fired, where the opening and the time between games run. It holds the
   guest mapping (a copy-on-write copy, vm_copy), the CPU, the context each
   guest_call level saved with the function it called and the trap it is in,
   the run loop's frame above them (events_loop_frame), the replay's clock,
   and a copy of every block of module state registered with
   SNAPSHOT_STATE.

   It is put back exactly (the replay's frame checks go on matching) in one
   of two ways. In place (snapshot_restore), at a depth-2 trap entry in the
   same kind of frame with the same values, where the host stack above is
   the same as when it was taken: memory, the CPU and the saved contexts go
   back, and the trap it was taken at is dispatched in place of the one
   entered. Otherwise from main, after a longjmp out of every guest call
   (snapshot_reenter): depth 1 is rebuilt at its RunApplicationEventLoop
   trap with guest_resume, the run loop goes straight back into its frame
   (events_resume_loop), and depth 2 is rebuilt at the trap it was taken at.
   Either way the snapshot stays as it was, to be put back again.

   A block is copied crudely, pointers and all, unless it owns heap objects
   that come and go (the mixer's sounds, the resource map's entries,
   dialogs' saved screens and nibs, Core Foundation's objects): its save
   copies them too, its load copies them back, and its free frees the copy.
   What describes the host stack isn't in a block (trap.c's depth and
   levels, events.c's frame in progress, both reset by snapshot_reenter),
   and neither is host state the game never sees (the window, the viewer,
   HD's copies, a script's actions, sound's output and volume).

   Not covered yet: files.c's open files (a FILE* and its position) and
   cgimage.c's images are copied crudely, so a file opened or closed, or an
   image released, between a snapshot and its restore would be wrong after
   it. Loony opens its one file before the run loop and never closes it,
   and only MONSTER FAIR, which has no replays, uses cgimage. */

typedef struct snapshot snapshot;

/* From the trap entry hook, entering trap index: a snapshot, or NULL if
   none can be taken here (not depth 2, or not in the run loop's handler or
   timer). Free it with snapshot_free. */
snapshot *snapshot_take(uint32_t index);
void snapshot_free(snapshot *s);

/* The replay's clock when s was taken, in us (replay_now_us). */
uint64_t snapshot_clock_us(const snapshot *s);

/* At a trap entry: whether s can be put back here in place. */
bool snapshot_fits(const snapshot *s);

/* Puts s back in place (snapshot_fits) and returns the trap to dispatch in
   place of the one entered. */
uint32_t snapshot_restore(const snapshot *s);

/* From main, with no guest call on the host stack (after a longjmp out of
   them): puts s back, calls after with the emulator as s holds it, before
   any guest code runs, then runs the game on from s. Returns when its main
   returns. */
void snapshot_reenter(const snapshot *s, void (*after)(void));

/* The inventory: a module's file-scope state, by name, registered before
   main. SNAPSHOT_STATE(X) goes after X's definition, and a snapshot copies
   X's bytes. SNAPSHOT_STATE_WITH(X, save, load, drop) is for state that owns
   heap objects: save returns a copy that owns copies of them, load puts a
   copy back into X (freeing what X held, and copying the copy's objects
   again, since the copy is kept), and drop frees a copy. */
typedef void *(*snapshot_save_fn)(void);
typedef void (*snapshot_load_fn)(const void *copy);
typedef void (*snapshot_drop_fn)(void *copy);
void snapshot_register(const char *file, const char *name, void *p, size_t len,
                       snapshot_save_fn save, snapshot_load_fn load, snapshot_drop_fn drop);
#define SNAPSHOT_STATE_WITH(var, save, load, drop)                                                 \
    __attribute__((constructor)) static void snapshot_state_##var(void) {                         \
        snapshot_register(__FILE__, #var, &(var), sizeof(var), save, load, drop);                 \
    }
#define SNAPSHOT_STATE(var) SNAPSHOT_STATE_WITH(var, NULL, NULL, NULL)

/* The blocks registered, and the bytes they take (as the module holds
   them, not counting what a save copies too). */
int snapshot_blocks(size_t *bytes);
