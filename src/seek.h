#pragma once
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>

#include "snapshot.h"

/* Seeking in a watched replay (#44): snapshots of the emulator
   (snapshot.h) taken as it plays, and a seek that puts back the nearest one
   at or before the time wanted, from where the viewer (watch.h) replays to
   that time unpaced.

   The store. Once started, a snapshot is taken at the first trap entry
   where one can be (snapshot_take) every SEEK_SPACING_US of the replay's
   clock, unless the store has one less than that before. They are kept in
   the clock's order. Every time the viewer goes to is in play, so outside
   play (replay_in_play) only the latest is kept of each stretch, for a seek
   to where play goes on. Past SEEK_MAX_SNAPSHOTS one is thinned out: the one
   whose neighbours are closest together, the oldest first among equals, and
   never the first or the last. So the oldest end up 10 s apart, then 20.

   A seek. seek_to asks for a time on the replay's clock; a newer ask
   replaces one not served yet. It is served at the next trap entry at
   depth 2: there the viewer has stopped waiting (its waits end while a seek
   is pending), so the replay gets there unpaced. The snapshot it goes to is
   the last at or before the time, or the first if every one is later.
   Going ahead to a time with no snapshot between it and the replay's clock,
   nothing is put back: the replay just goes on. Otherwise the snapshot is
   put back in place where it fits (snapshot_fits), or the replay unwinds
   to main (longjmp to seek_unwound) and seek_reenter puts it back there.
   Either way, the function seek_start was given is told where the replay's
   clock is now, and whether a snapshot was put back. Each line it logs says
   which way and what it took.

   Where no trap entry at depth 2 will come again (the replay's or the
   game's end), seek_serve_here serves the seek by unwinding to main.

   Verifying (LOONY_SNAPSHOT_VERIFY=1). Each snapshot, once taken, is put
   back at once in place and compared with the emulator (snapshot_verify),
   which catches a walk that loads something other than what it saved. And
   once a seek has put one back, the replay is compared with each snapshot
   after it as it comes to the trap entry where that one was taken
   (trap_entries), which catches state no snapshot holds, before the frame
   checks would. A difference is fatal and names the parts that differ. A
   replay that isn't watched takes snapshots too while verifying, so its
   output can be compared with one that takes none. */

#define SEEK_SPACING_US 5000000ull
#define SEEK_MAX_SNAPSHOTS 120

/* Before seek_start: reads LOONY_SNAPSHOT_VERIFY. */
void seek_init(void);
bool seek_verifying(void);

/* Once watching, or replaying while verifying: starts taking snapshots and
   serving seeks. landed is called when a seek has been served, with the
   replay's clock (from the snapshot put back, or where the replay is when
   none was), whether one was put back, and the seek's generation
   (seek_to's); NULL when nothing will seek. */
typedef void (*seek_landed_fn)(uint64_t clock_us, bool restored, unsigned generation);
void seek_start(seek_landed_fn landed);

/* Asks for clock_us: returns the ask's generation, one more than the last. */
unsigned seek_to(uint64_t clock_us);

/* Whether a seek is waiting to be served. */
bool seek_pending(void);

/* Whether going ahead from after_us to to_us would put a snapshot back: the
   store has one later than after_us, at or before to_us. */
bool seek_snapshot_ahead(uint64_t after_us, uint64_t to_us);

/* Serves the pending seek where no trap entry at depth 2 will come: from
   inside a trap, or from main once the game's main has returned, unwinds to
   seek_unwound. Returns only if there is no snapshot to go to (and the
   seek is served as landing where the replay is). */
void seek_serve_here(void);

/* From a trap entry at depth 2, for the snapshot of the game's start
   (gamestart.h): unwinds to main, where seek_reenter puts s back and runs
   the game on from there, as a seek that can't be put back in place does.
   Once it is back, the store forgets every snapshot it took (all of them
   before s) and keeps s as its first, and landed is told, unless a seek
   is waiting, which will say where it lands. s is the store's from then
   on, or, when the store isn't started, kept until seek_reset. */
_Noreturn void seek_enter(snapshot *s);

/* Where a seek unwinds to: main calls setjmp(seek_unwound) before its guest
   call to the game's main, and seek_reenter when it returns there a second
   time. seek_reenter puts the snapshot back and runs the game from there,
   returning when the game's main returns. */
extern jmp_buf seek_unwound;
void seek_reenter(void);

/* For tests: forget every snapshot and the pending seek, and stop. */
void seek_reset(void);
