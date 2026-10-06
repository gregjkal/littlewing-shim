#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* File Manager: FSSpecs and data-fork reads and writes.

   Two folders back one fake volume: the game folder, which is never
   modified, and a writable data folder that overlays it. A path is looked up
   in the data folder first, then in the game folder. FSpCreate makes new
   files in the data folder, and the first write to a game-folder file copies
   it to the matching path there, so the game believes it saved in place.

   There is one fake volume (FILES_VREFNUM). Directory IDs are handed out per
   folder: FILES_ROOT_DIRID is the game folder, which is also the default
   directory (vRefNum 0, dirID 0). Mac paths use ':' separators and Mac Roman
   names; host paths use '/' and UTF-8. */

#define FILES_VREFNUM    (-1)
#define FILES_ROOT_DIRID 2
#define FILES_FNF_ERR    (-43)
#define FILES_DIR_NF_ERR (-120)
#define FILES_EOF_ERR    (-39)
#define FILES_RF_NUM_ERR (-51)
#define FILES_POS_ERR    (-40)
#define FILES_BD_NAM_ERR (-37)
#define FILES_DUP_FN_ERR (-48)
#define FILES_WR_PERM_ERR (-61)
#define FILES_IO_ERR     (-36)

/* FSSpec: vRefNum (2), parID (4), name (Str63, 64 bytes). */
#define FSSPEC_SIZE 70

/* The folder holding each game's save folder (and the picker's file):
   ~/Library/Application Support/loony-shim. False if LOONY_DATA_DIR is
   set (it is then the save folder itself) or HOME isn't. */
bool files_data_root(char *out, size_t cap);

/* The writable folder: $LOONY_DATA_DIR, or <files_data_root>/<game_id>.
   False if neither LOONY_DATA_DIR nor HOME is set. */
bool files_data_dir(const char *game_id, char *out, size_t cap);

/* Sets the game folder (read-only) and the writable folder (created when
   first needed; NULL means writes fail with wrPermErr). Closes open files
   and forgets directory IDs. Returns whether the writable folder is in use:
   one that is, contains or is inside the game folder is refused (logged),
   since writing there could change the game's files. */
bool files_init(const char *game_dir, const char *data_dir);

/* Converts a Mac Roman name to UTF-8, with '/' (legal in Mac names) becoming ':'. */
void files_mac_to_utf8(const char *mac, char *out, size_t cap);

/* Registers FSMakeFSSpec, FSpCreate, FSpOpenDF, PBReadSync, FSWrite, GetEOF,
   SetEOF, SetFPos, GetFPos, PBFlushFileSync and FSClose. */
void files_register(void);
