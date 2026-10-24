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
   directory (vRefNum 0, dirID 0) unless files_set_default_dir moves it. Mac
   paths use ':' separators and Mac Roman names; host paths use '/' and
   UTF-8. */

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
   ~/Library/Application Support/littlewing-shim. False if LOONY_DATA_DIR is
   set (it is then the save folder itself) or HOME isn't. */
bool files_data_root(char *out, size_t cap);

/* Moves the save root from its name before the rename,
   ~/Library/Application Support/loony-shim, to files_data_root's, unless
   that already exists. Returns whether it moved. */
bool files_move_old_data_root(void);

/* The writable folder: $LOONY_DATA_DIR, or <files_data_root>/<game_id>.
   False if neither LOONY_DATA_DIR nor HOME is set. */
bool files_data_dir(const char *game_id, char *out, size_t cap);

/* Sets the game folder (read-only) and the writable folder (created when
   first needed; NULL means writes fail with wrPermErr). Closes open files
   and forgets directory IDs. Returns whether the writable folder is in use:
   one that is, contains or is inside the game folder is refused (logged),
   since writing there could change the game's files. */
bool files_init(const char *game_dir, const char *data_dir);

/* Makes the default directory (vRefNum 0 and dirID 0 in FSMakeFSSpec) the
   folder rel, relative to the game folder: a bundle's is the folder its
   program is in, Contents/MacOS. It is the game folder itself until then,
   and again after files_init. */
void files_set_default_dir(const char *rel);

/* Converts a Mac Roman name to UTF-8, with '/' (legal in Mac names) becoming ':'. */
void files_mac_to_utf8(const char *mac, char *out, size_t cap);

/* The Mac Roman byte for a Unicode code point, or -1 if Mac Roman has none. */
int files_unicode_to_mac(uint32_t c);

/* Resource forks, for rsrc.c. spec's file as a path relative to the game
   folder (crashes on a bad FSSpec, as the File Manager calls do). */
void files_spec_rel(uint32_t spec, char *out, size_t cap);

/* rel's resource fork, from the data folder's copy if there is one, else
   the game folder's: malloc'd into *out, *len bytes (NULL and 0 for an
   empty fork). Returns 0, or fnfErr if there is no such file. */
int16_t files_read_rsrc_fork(const char *rel, uint8_t **out, size_t *len);

/* Replaces rel's resource fork with len bytes of data, in the data folder:
   a file there only in the game folder is copied first, both forks, and a
   file that is nowhere is created with an empty data fork. Returns an
   OSErr. */
int16_t files_write_rsrc_fork(const char *rel, const uint8_t *data, size_t len);

/* Registers FSMakeFSSpec, FSpCreate, FSpOpenDF, PBReadSync, FSWrite, GetEOF,
   SetEOF, SetFPos, GetFPos, PBFlushFileSync and FSClose, and for a Mach-O
   game the FSRef and fork calls: FSPathMakeRef (host paths inside the game
   or data folder), FSGetDataForkName, FSOpenFork, FSGetForkSize,
   FSGetForkPosition, FSSetForkPosition and FSCloseFork. Fork refnums are
   File Manager refnums, so the game can mix the two. Also FSRead,
   FindFolder (the Preferences folder alone), FSpGetFInfo, ResolveAliasFile
   (there are no aliases), NewAlias and FlushVol. */
void files_register(void);
