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
   guest memory, given the same game files and the same build, but for the
   license, which the game copies into memory at launch. The game's start
   is the first trap entry at depth 2 inside a handler the run loop
   dispatched to once the recording's first span of play
   (replay_play_spans) has begun.

   What is kept. Guest memory as the pages that differ from the baseline's,
   each XORed with it (snapshot_encode), so what didn't change since the
   baseline isn't in the file: the game's code, what the opening loaded,
   and the license the game copied at launch. Decoded against another
   viewer's baseline, those bytes are that viewer's license. The game's
   resources loaded since and unchanged since they were read are left out
   too, and filled in again from the resource fork when the snapshot is
   put back. The module state never holds the license
   (cf_keep_out_of_snapshots), and a snapshot whose guest memory holds it
   where it changed since the baseline isn't kept. The section's header
   says which build of LittleWing took it (the UUID the linker gave the
   program), which recording it is of (replay_identity), the baseline's
   clock, trap entry and a SHA-256 of its guest memory (every page that
   isn't zeros, with its offset, the license blanked: below), and the
   game's start's clock and trap entry.

   The license in the baseline. Loony copies the e-mail address and the
   key code into buffers of its own with CFStringGetCString, then decrypts
   the key code in place, so it is in no form the preferences hold. cf
   notes each buffer it copies a license string into (cf_kept_out_fields),
   and the hash blanks those, at the size the game gave, and the license's
   text wherever else it is. Measured on Loony 3.0.1 with a write watch on
   its license code: nothing else it wrote at launch survives to the
   baseline but saved registers and zeros. So a recording's baseline hashes
   the same under any license, and a shared copy's snapshot fits the
   recipient's replay.

   Whose. Any install's: a shared copy keeps its snapshot (replay_share),
   and a share of a recording without one takes one first, from the shared
   copy, through a second copy of the program (gamestart_take_start). A
   snapshot is the whole emulator, so one from a file is checked before it
   is used, as cheap hygiene and not a sandbox: it must leave the game's
   code section as the recipient's own game files made it, and its
   registers must point into that code or at trap addresses
   (snapshot_brings_no_code); only the code section is executable (main.c);
   and every module block checks the indices and counts it loads
   (snapshot_check). The game's code then runs on the data a snapshot
   brings, never on code it brings.

   Taking it. A replay of a recording with no snapshot it can use takes
   one: it keeps a copy of the emulator at the baseline (snapshot_take),
   and at the game's start encodes a snapshot against it and writes it into
   the recording (replay_set_snapshot). So the first replay or export of a
   recording pays the prelude, and the ones after don't. Not when the game
   starts within GAMESTART_MIN_PRELUDE_US of launch (its prelude is a few
   seconds, and its snapshot would be several times its recording), when
   the recording has no play, or when it can't be written.
   LOONY_GAMESTART=take does only that, and exits.

   Using it. At the baseline, a replay of a recording with a snapshot
   checks its header against the build, the recording and the baseline as
   the replay has it, decodes it against guest memory there, fills in the
   resources, checks it brings no code, and goes to it from main
   (seek_enter): the replay goes on from the game's start. Anything that
   doesn't fit (another build, another copy of the game, a damaged or
   crafted file) is logged with why, and the replay plays from launch and
   takes the snapshot again to replace it. gamestart_limit keeps a
   snapshot from being used past a time a replay needs to see.

   Verifying (LOONY_SNAPSHOT_VERIFY=1, seek.h). The snapshot is decoded at
   the baseline but not used: the replay plays on from launch, and at the
   trap entry where the snapshot was taken compares it with the emulator
   (snapshot_verify), byte for byte, the resources filled in included. A
   difference is fatal and names what differs.

   LOONY_GAMESTART=0 neither uses a snapshot nor takes one. */

#define GAMESTART_BASELINE_US 8000000ull
#define GAMESTART_MIN_PRELUDE_US 20000000ull

/* After seek_start, on a replay: reads LOONY_GAMESTART (0: off; take:
   take the snapshot and exit) and starts watching for the baseline. */
void gamestart_init(void);

/* A snapshot put back past clock_us (on the replay's clock) wouldn't do:
   the replay must see what comes before. Call before the baseline. */
void gamestart_limit(uint64_t clock_us);

/* For tests: the hash a baseline would have now, the license blanked. */
void gamestart_memory_hash(uint8_t out[32]);

/* Whether the recording at path holds a snapshot of the game's start. */
bool gamestart_in_file(const char *path);

/* Sharing (menu.m): a shared copy of a recording with no snapshot takes
   one before it goes, from the shared copy itself, since that is what the
   recipient replays. A second copy of this program, self, replays path
   headless and silent with LOONY_GAMESTART=take, which exits once the
   snapshot is kept or none can be. gamestart_take_start starts it (false,
   with err, if it can't); gamestart_take_poll returns whether it has ended,
   without waiting, and then in *kept whether path holds a snapshot;
   gamestart_take_stop ends it. One at a time. */
bool gamestart_take_start(const char *self, const char *path, char *err, size_t errlen);
bool gamestart_take_poll(bool *kept);
void gamestart_take_stop(void);
