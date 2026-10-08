#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The app's own settings, apart from any game's preferences: string and
   integer values by key, in GAME_PICKER_FILE in the save root
   (files_data_root). Setting a key rewrites the file and keeps the other keys.
   Without a save root (LOONY_DATA_DIR set, or no HOME) nothing is read or
   saved. */

#define SETTINGS_LAST_GAME "last game" /* string: the game the picker last chose */
#define SETTINGS_VOLUME "volume"       /* integer: sound_volume, 0-100 */

/* False, leaving out alone, if the key is missing or has the other type. */
bool settings_get_str(const char *key, char *out, size_t cap);
bool settings_get_int(const char *key, int64_t *out);

/* Failures are logged. */
void settings_set_str(const char *key, const char *value);
void settings_set_int(const char *key, int64_t value);
