#pragma once
#include <setjmp.h>
#include <stddef.h>

/* Measurements for the replay seek engine (#44, docs/littlewing-shim-steal-
   replay-seek-engine.md, step 1), taken on a headless replay. With
   LOONY_MEASURE=1, at exit: the guest memory resident (mincore over the
   guest mapping, by region), the bytes a saved CPU context takes, the HD
   copies and their bytes, and how many traps the guest entered at each
   guest_call depth (which says where a snapshot could be taken: the depth
   the game's loop runs at, 2 for Loony, inside its event loop timer). With
   LOONY_MEASURE_COW=<seconds>, once the game's clock passes that: the
   copy-on-write spike, which copies the guest mapping with mach_vm_remap
   and with vm_copy, times each copy, then times the page faults the live
   side pays on its first write to each resident page afterwards, and logs
   the numbers. The spike changes nothing the game sees.

   Spike A (#44, "Spike A, exactly"): with LOONY_SPIKE_A=<N>,<M>, seconds of
   the game's clock, a replay captures the emulator at the first depth-2 trap
   entry once the clock passes N (the guest mapping with vm_copy, the CPU,
   every block registered with MEASURE_STATE copied crudely, pointers and
   all, and the context each guest_call level saved), runs on, and at the
   first depth-2 trap entry once the clock passes M puts all of it back and
   dispatches the trap it captured, as if the run had been at N all along.
   The replay's frame checks then say whether the restore was exact. It logs
   what it captured and what each step took. Throwaway: the walker replaces
   it.

   Spike B (#44, "Spike B, exactly"): with LOONY_SPIKE_B=<N>, seconds of the
   game's clock, a replay captures the emulator as Spike A does at the first
   depth-2 trap entry past N inside a handler the run loop dispatched an
   event to (where Loony's games run), with the frames above it: the trap
   depth 1 is in (RunApplicationEventLoop) and the run loop's dispatch
   (events_loop_frame). It lets the replay run to its end: the game's main
   returning, ExitToShell (where Loony's quit ends), or the end of the
   recording inside a trap (replay_set_end). Inside a trap, it longjmps to
   measure_unwound, which main set before its guest call. Then, with no
   guest call left on the host stack, it puts memory and the module blocks
   back, rebuilds depth 1 at its trap with guest_resume, has the run loop
   go back into its dispatch (events_resume_loop), rebuilds depth 2 at the
   captured trap, and the replay runs to its end a second time, its frame
   checks saying whether the re-entry was exact. Headless only: a watched
   replay's viewer takes over the replay's end. Throwaway too. */

/* After hd_init and replay_start: reads the environment. */
void measure_init(void);

/* Spike B: main's way back from inside a trap, and what main calls when
   the game ended, its main returning or ExitToShell (the exit hook), and
   again where the longjmp lands. With a capture, inside a trap it longjmps;
   in main it re-enters the game, returning once the game ends again. */
extern jmp_buf measure_unwound;
void measure_game_ended(void);

/* The event pump's present: runs the spike when its time comes. */
void measure_present(void);

/* Spike A's inventory: a module's file-scope state, by name, registered
   before main. MEASURE_STATE(X) goes after X's definition, and the spike
   copies X's bytes. MEASURE_STATE_WITH(X, save, load) is for state that owns
   heap objects: save returns a copy that owns copies of them, and load takes
   it back into X, freeing what X held. */
typedef void *(*measure_save_fn)(void);
typedef void (*measure_load_fn)(void *copy);
void measure_register(const char *file, const char *name, void *p, size_t len,
                      measure_save_fn save, measure_load_fn load);
#define MEASURE_STATE_WITH(var, save, load)                                                        \
    __attribute__((constructor)) static void measure_state_##var(void) {                          \
        measure_register(__FILE__, #var, &(var), sizeof(var), save, load);                        \
    }
#define MEASURE_STATE(var) MEASURE_STATE_WITH(var, NULL, NULL)
