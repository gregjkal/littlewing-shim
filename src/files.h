#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* File Manager, read side: FSSpecs and data-fork reads from the game folder.
   Writing files arrives with milestone 6; until then calls that would write
   crash with a report.

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

/* FSSpec: vRefNum (2), parID (4), name (Str63, 64 bytes). */
#define FSSPEC_SIZE 70

/* The writable folder: $LOONY_DATA_DIR, or ~/Library/Application
   Support/loony-shim. False if neither LOONY_DATA_DIR nor HOME is set. */
bool files_data_dir(char *out, size_t cap);

/* Sets the game folder (read-only). Closes open files and forgets directory IDs. */
void files_init(const char *game_dir);

/* Converts a Mac Roman name to UTF-8, with '/' (legal in Mac names) becoming ':'. */
void files_mac_to_utf8(const char *mac, char *out, size_t cap);

/* Registers FSMakeFSSpec, FSpOpenDF, PBReadSync, GetEOF, SetFPos, GetFPos and FSClose. */
void files_register(void);
