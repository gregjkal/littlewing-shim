#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Testing switches that change a game's code after it is loaded and before
   any of it runs. Each one first checks that the code is the version it
   was written for, and changes nothing otherwise. */

/* LOONY_MF_SKIP_LICENSE_RECHECK=1: MONSTER FAIR 1.2.5 checks its license a
   second time in play (at an Esc more than 7200 ticks after the first key)
   and, refusing it, ends the session and erases the license as it quits.
   It refuses the public test key in docs/test_key.txt that way. This keeps
   the checks but drops their verdict: the two stores that set the
   "refused" flag become nops. The Register window's own check is unchanged.
   Returns false, with err set, if the code there isn't 1.2.5's. */
bool patch_mf_skip_license_recheck(char *err, size_t errlen);
