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

   The module state is one buffer, zeroed before it is written: a header
   (the walker's version and a hash of the inventory's layout, its blocks'
   names and struct sizes in order), then a block for each registered
   variable with its tag (a hash of the file and the name), the size of
   the variable and the length of what follows. Each block is written by
   its walk (below) and read back by the same walk, so what is saved and
   what is loaded can't drift apart, and putting a snapshot back fails
   hard, naming the block, on a header, a tag, a size or a length that
   doesn't match. Pointers a walk follows are written as zeros, and so are
   the host's own fields, so two snapshots of the same state are the same
   bytes. The module state holds no pointer, path or other value that
   means something only in this process, so a snapshot can be kept in a
   recording and put back by another process on another Mac (#46,
   gamestart.h): events.c's modal sink is walked as whether there is one,
   dialogs.c's front dialog as an index, the folders files.c and cf.c use
   are the host's, and so is the replay's file header. A file's contents
   aren't in a snapshot, only which files are open and their marks
   (files.c). What describes the host stack isn't in a block (trap.c's
   depth and levels, events.c's frame in progress, both reset by
   snapshot_reenter), and neither is host state the game never sees (the
   window, the viewer, HD's copies, a script's actions, sound's output and
   volume).

   snapshot_verify compares a snapshot with the emulator as it is: guest
   memory region by region, the CPU and the saved contexts register by
   register, the frame, and the module state by writing it again and
   comparing the bytes, naming each block that differs and where. */

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

/* Whether the emulator as it is now is the one s holds, at a trap entry
   where s fits (snapshot_fits): if not, what differs, as a list of the
   parts, each with where it first differs ("guest memory at 0x00123400 (3
   pages)", "cf.c C at byte 1200 of 5678, in C"), in what. */
bool snapshot_verify(const snapshot *s, char *what, size_t cap);

/* A snapshot as bytes, to keep in a recording (#46, gamestart.h): the
   CPU and each level's saved context as their registers
   (cpu_context_regs), the frame, the clock and the module state as they
   are, and the guest mapping as the pages that differ from base's, each
   XORed with base's page, so what didn't change since base is zeros. The
   bytes in each range of same (guest addresses) are written as base has
   them, to be put back by other means (snapshot_poke). The encoding is
   malloc'd, *len bytes. snapshot_encode reads which pages each side holds
   (mincore, in memory or paged out); a page neither holds is zeros on
   both. */
typedef struct {
    uint32_t addr, len;
} snapshot_range;
uint8_t *snapshot_encode(const snapshot *s, const snapshot *base, const snapshot_range *same,
                         size_t nsame, size_t *len);

/* The snapshot data encodes, against the guest mapping as it is now, which
   must be what base was when it was encoded; the contexts are the CPU's as
   it is now with the registers put in. NULL, with err, if data isn't a
   snapshot this version reads, or its guest mapping isn't this one's
   size. */
snapshot *snapshot_decode(const uint8_t *data, size_t len, char *err, size_t errlen);

/* For a snapshot decoded from a file another install may have written
   (gamestart.h): whether it brings the emulator data and register values
   alone, never code. Its guest memory must be the same as now in the
   game's code section (trap_code), which at the baseline the game's own
   files made; the program counter, link and count registers of the CPU
   and of each level's saved context, each level's function called and
   where its trap returns, must be in the code section, at an import's
   trap address, at guest_call's return address, or zero; each level's
   trap must be an import's; and the machine state register must be the
   CPU's as it is now. If not, what isn't, in why. Not a sandbox: the
   game's own code runs on whatever data it brings. */
bool snapshot_brings_no_code(const snapshot *s, char *why, size_t cap);

/* Whether s's guest memory holds text (as it is, in capitals or in small
   letters) where it isn't what base holds there: what snapshot_encode
   would put in the file, XORed out of sight. */
bool snapshot_changed_holds(const snapshot *s, const snapshot *base, const char *text);

/* Writes len bytes at guest address addr in s's guest memory. */
void snapshot_poke(snapshot *s, uint32_t addr, const void *p, size_t len);

/* The inventory: a module's file-scope state, by name, registered before
   main. SNAPSHOT_STATE(X) goes after X's definition, and a snapshot holds
   X's bytes. SNAPSHOT_STATE_WALK(X, walk) is for state that holds pointers
   or host fields: walk lists what is the game's, with the calls below, and
   runs both ways. Saving, it writes the bytes into the snapshot; loading
   (snapshot_loading), it reads them back, so before its first call it
   frees what X owns, which the snapshot's copies replace. */
typedef struct snapshot_walker snapshot_walker;
typedef void (*snapshot_walk_fn)(snapshot_walker *w);
void snapshot_register(const char *file, const char *name, void *p, size_t len,
                       snapshot_walk_fn walk);
#define SNAPSHOT_STATE_WALK(var, walk)                                                             \
    __attribute__((constructor)) static void snapshot_state_##var(void) {                         \
        snapshot_register(__FILE__, #var, &(var), sizeof(var), walk);                             \
    }
#define SNAPSHOT_STATE(var) SNAPSHOT_STATE_WALK(var, NULL)

/* In a walk: whether it is loading. */
bool snapshot_loading(const snapshot_walker *w);

/* Loading: ok must hold of what the walk loaded so far, or putting the
   snapshot back fails hard, naming the block and what (a printf format).
   A snapshot can come from a file another install wrote (gamestart.h), so
   every walk checks each index and count it loaded against the host array
   it indexes or bounds, before anything uses it. Saving, it does nothing. */
void snapshot_check(snapshot_walker *w, bool ok, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* len bytes at p: the game's. */
void snapshot_bytes(snapshot_walker *w, void *p, size_t len);

/* field, a pointer inside bytes already walked, points to len bytes of the
   heap the block owns, or is NULL. Loading, field gets a fresh copy (NULL
   for none) and len must be what was saved. What the copy holds can be
   walked after it, field by field. SNAPSHOT_OWNED takes the pointer and a
   count of what it points to. */
void snapshot_owned(snapshot_walker *w, void *field, size_t len);
#define SNAPSHOT_OWNED(w, ptr, count) snapshot_owned(w, &(ptr), (size_t)(count) * sizeof *(ptr))

/* The same for a string the block owns (NUL-terminated, or NULL). */
void snapshot_string(snapshot_walker *w, char **field);

/* len bytes at field, inside bytes already walked, are the host's: not in
   the snapshot (zeros there), and loading keeps what they hold now. */
void snapshot_host(snapshot_walker *w, void *field, size_t len);
#define SNAPSHOT_HOST(w, field) snapshot_host(w, &(field), sizeof(field))

/* len bytes at field, inside bytes already walked, are rebuilt by the walk
   from what else it walks (a FILE* from a file's name and position): not
   in the snapshot (zeros there), and zeros after loading until the walk
   sets them. */
void snapshot_rebuilt(snapshot_walker *w, void *field, size_t len);
#define SNAPSHOT_REBUILT(w, field) snapshot_rebuilt(w, &(field), sizeof(field))

/* The padding after last, the last field of the struct s (walked already),
   which may hold whatever the heap held: zeros in a snapshot. */
#define SNAPSHOT_PADDING(w, s, last)                                                               \
    snapshot_rebuilt(w, (unsigned char *)&(s) + offsetof(typeof(s), last) + sizeof((s).last),    \
                     sizeof(s) - offsetof(typeof(s), last) - sizeof((s).last))

/* For tests: the module state alone, as a snapshot holds it (malloc'd,
   *len bytes), put back, and compared as snapshot_verify does. */
uint8_t *snapshot_state_save(size_t *len);
void snapshot_state_load(const uint8_t *state, size_t len);
bool snapshot_state_verify(const uint8_t *state, size_t len, char *what, size_t cap);

/* The blocks registered, and the bytes they take (as the module holds
   them, not counting what they own). */
int snapshot_blocks(size_t *bytes);

/* For tests: block i of the inventory, 0 to snapshot_blocks() - 1. */
void snapshot_block(int i, const char **file, const char **name, void **p, size_t *len);
