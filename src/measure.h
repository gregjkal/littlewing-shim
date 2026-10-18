#pragma once

/* Measurements for the replay seek engine (#44, docs/littlewing-shim-steal-
   replay-seek-engine.md, step 1), taken on a headless replay. With
   LOONY_MEASURE=1, at exit: the guest memory resident (mincore over the
   guest mapping, by region), the bytes a saved CPU context takes, the HD
   copies and their bytes, and how many traps the guest entered at each
   guest_call depth (which says where a snapshot can be taken: the depth
   the game's loop runs at, 2 for Loony, in its event loop timer's callback
   and its event handlers; snapshot.h). With
   LOONY_MEASURE_COW=<seconds>, once the game's clock passes that: the
   copy-on-write spike, which copies the guest mapping with mach_vm_remap
   and with vm_copy, times each copy, then times the page faults the live
   side pays on its first write to each resident page afterwards, and logs
   the numbers. The spike changes nothing the game sees.

   With LOONY_MEASURE_DELTA=<folder>, on a replay that seeks nothing (#46,
   step 1): at the baseline, the first depth-2 trap entry inside the run
   loop's timer callback after launch, and at the game's start, the first
   depth-2 trap entry inside a handler the run loop dispatched to once the
   replay's first span of play has begun, writes every resident page of the
   guest mapping (mincore) to baseline.pages and start.pages in the folder,
   each page as its offset (8 bytes, little-endian) and its bytes. Logs the
   time each took to reach from the process's start and the replay's clock
   there, checks at the game's start that no page mincore leaves out holds
   anything but zeros, and exits. LOONY_MEASURE_DELTA_AT=<seconds> moves the
   baseline to the first such entry once the replay's clock passes that. */

/* After hd_init and replay_start: reads the environment. */
void measure_init(void);

/* The event pump's present: runs the spike when its time comes. */
void measure_present(void);
