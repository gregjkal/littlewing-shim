#pragma once
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
   it. */

/* After hd_init: reads the environment. */
void measure_init(void);

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
