#pragma once
#include <stdbool.h>

#include "game.h"

/* Every game starts registered. LittleWing's store is gone, and the unlock
   (unlock.c) already makes any e-mail address and a key code of 24 digits
   register, so the shim saves this license where the game keeps one
   (game_info's license_file) before the game first looks: the game finds
   it and skips its shareware screens, as if the player had typed it into
   Register. A license already saved, the player's own or this one, is
   left alone. LOONY_UNREGISTERED=1 saves none, so a save folder without
   one yet shows the shareware screens and Register (the tests set it, and
   their frames are of an unregistered game). */
#define LICENSE_EMAIL "tester@example.com"
#define LICENSE_KEY "123456789012345678901234"

/* Saves the license into data_dir, the game's save folder, unless one is
   there or LOONY_UNREGISTERED is set. A replay's saves come from its
   recording, so main.c doesn't call this for one. False, logged, if it
   can't be written. */
bool license_save_default(const game_info *g, const char *data_dir);
