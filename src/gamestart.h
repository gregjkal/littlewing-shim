#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The snapshot of the game's start kept in a recording (#46). A replay can
   only start at launch, and each game's recording holds the session before
   the game too (replay.h), so a replay of the tenth game of a session
   replays the first nine, unseen, before its first frame: minutes, for a
   game an hour in. A snapshot of the emulator at the game's start
   (snapshot.h), kept in the recording, lets the replay skip all that.

   Where. The baseline is the first trap entry at depth 2 inside the run
   loop's timer callback once the replay's clock reaches
   GAMESTART_BASELINE_US: Loony's opening waits on its splash screen until
   then, so a replay gets there a quarter of a second after the process
   starts, and every replay of one recording gets there with the same
   guest memory, given the same game files, the same build and the same
   registration (the game copies the license into memory at launch). The
   game's start is the first trap entry at depth 2 inside a handler the run
   loop dispatched to once the recording's first span of play
   (replay_play_spans) has begun.

   What is kept. Guest memory as the pages that differ from the baseline's,
   each XORed with it (snapshot_encode), so what didn't change since the
   baseline isn't in the file: the game's code, what the opening loaded,
   and the license the game copied at launch. The game's resources loaded
   since and unchanged since they were read are left out too, and filled
   in again from the resource fork when the snapshot is put back. The
   module state never holds the license (cf_keep_out_of_snapshots), and a
   snapshot whose guest memory holds it where it changed since the
   baseline isn't kept. The section's header says which build of
   LittleWing took it (the UUID the linker gave the program), which
   recording it is of (replay_identity), the baseline's clock, trap entry
   and a SHA-256 of its guest memory (every page that isn't zeros, with
   its offset), and the game's start's clock and trap entry. An
   HMAC-SHA256 under this install's key (snapshot.key, made at its first
   use beside every game's save folder) closes it.

   Whose. A snapshot is the whole emulator, guest code and registers
   included, so putting back one made elsewhere would run whatever it
   holds as the game. Only this install's are used: a shared copy carries
   none (replay_share), and one that came some other way is ignored like
   any other that doesn't fit, and replaced.

   Taking it. A replay of a recording with no snapshot it can use takes
   one: it keeps a copy of the emulator at the baseline (snapshot_take),
   and at the game's start encodes a snapshot against it and writes it into
   the recording (replay_set_snapshot). So the first replay or export of a
   recording pays the prelude, and the ones after don't. Not when the game
   starts within GAMESTART_MIN_PRELUDE_US of launch (its prelude is a few
   seconds, and its snapshot would be several times its recording), when
   the recording has no play, or when it can't be written.

   Using it. At the baseline, a replay of a recording with a snapshot
   checks its HMAC and its header against the build, the recording and the
   baseline as the replay has it, decodes it against guest memory there,
   fills in the resources, and goes to it from main (seek_enter): the
   replay goes on from the game's start. Anything that doesn't match
   (another install, another build, another copy of the game, a damaged
   file) is logged with why, and the replay plays from launch and takes
   the snapshot again to replace it. gamestart_limit keeps a snapshot from
   being used past a time a replay needs to see.

   Verifying (LOONY_SNAPSHOT_VERIFY=1, seek.h). The snapshot is decoded at
   the baseline but not used: the replay plays on from launch, and at the
   trap entry where the snapshot was taken compares it with the emulator
   (snapshot_verify), byte for byte, the resources filled in included. A
   difference is fatal and names what differs.

   LOONY_GAMESTART=0 neither uses a snapshot nor takes one. */

#define GAMESTART_BASELINE_US 8000000ull
#define GAMESTART_MIN_PRELUDE_US 20000000ull

/* After seek_start, on a replay: reads LOONY_GAMESTART and starts
   watching for the baseline. */
void gamestart_init(void);

/* A snapshot put back past clock_us (on the replay's clock) wouldn't do:
   the replay must see what comes before. Call before the baseline. */
void gamestart_limit(uint64_t clock_us);

/* For tests: forget everything. */
void gamestart_reset(void);
