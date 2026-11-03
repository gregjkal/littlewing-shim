#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The app's own settings, apart from any game's preferences: string and
   integer values by key, in GAME_PICKER_FILE in the save root
   (files_data_root). Setting a key rewrites the file and keeps the other keys.
   Without a save root (LOONY_DATA_DIR set, or no HOME) nothing is read or
   saved, unless LOONY_SETTINGS names the file. settings_pin sets it, so a
   replay, which sets LOONY_DATA_DIR to a folder of its own, and every
   restart after it still keep the app's settings. */

#define SETTINGS_LAST_GAME "last game" /* string: the game the picker last chose */
#define SETTINGS_VOLUME "volume"       /* integer: sound_volume, 0-100 */
#define SETTINGS_WINDOW_WIDTH "window width"   /* integer: display_save_size, points */
#define SETTINGS_WINDOW_HEIGHT "window height" /* integer: the game's part, without the strip */

/* Sets LOONY_SETTINGS to the file in the save root, if there is one and
   it isn't set already. Called at launch, before anything changes
   LOONY_DATA_DIR. */
void settings_pin(void);

/* False, leaving out alone, if the key is missing or has the other type. */
bool settings_get_str(const char *key, char *out, size_t cap);
bool settings_get_int(const char *key, int64_t *out);

/* Failures are logged. */
void settings_set_str(const char *key, const char *value);
void settings_set_int(const char *key, int64_t value);
