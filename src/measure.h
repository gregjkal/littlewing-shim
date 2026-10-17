#pragma once

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
   the numbers. The spike changes nothing the game sees. */

/* After hd_init: reads the environment. */
void measure_init(void);

/* The event pump's present: runs the spike when its time comes. */
void measure_present(void);
