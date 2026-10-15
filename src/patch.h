#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Permanent workarounds that change a game's code after it is loaded and
   before any of it runs. Each one first checks that the code is the version
   it was written for, and changes nothing otherwise. */

/* MONSTER FAIR 1.2.5, the only version supported, checks its license a
   second time in play (at an Esc more than 7200 ticks after the first key)
   and, refusing it, ends the session and erases the license as it quits.
   It refuses the public key in docs/test_key.txt that way. This keeps the
   checks but drops their verdict: the two stores that set the "refused"
   flag become nops. The Register window's own check is unchanged. main.c
   applies it on every MONSTER FAIR launch. Returns false, with err set, if
   the code there isn't 1.2.5's. */
bool patch_mf_skip_license_recheck(char *err, size_t errlen);

/* LOONY LABYRINTH 3.0.1 notices Esc (pause) only in its play loop, which
   polls the key's state. A cut-scene (the ball draining, the bonus count, a
   journal scene) runs loops of its own that handle events but don't look at
   that key, so an Esc pressed and released during one is lost. This makes
   Esc, while a game runs, also set the pause request losing focus sets: the
   game then pauses as soon as the cut-scene ends, as a normal Esc would.
   code_base is where the code section was placed. Returns false, with err
   set, if the code there isn't 3.0.1's. */
bool patch_loony_esc_in_cutscenes(uint32_t code_base, char *err, size_t errlen);
