#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* The LittleWing games the shim can play. The classic games share one
   engine: Loony Labyrinth and Crystal Caliburn (3.0.1) make the same 132
   calls, so they differ only in names. Jinni Zeala (1.2.2, an older build
   of the engine) makes 120 of those and 3 more. MONSTER FAIR and Mad
   Daedalus (1.1.9) are Mac OS X bundles with Mach-O programs; Mad Daedalus
   plays its sound through CoreAudio and wants a 1024x768 display. Angel
   Egg (3.0.1) is a Mach-O bundle too, a classic Toolbox program built with
   CodeWarrior, and plays in a 1024x768 window. Adding a
   game is a row in game.c (the picker lays out two to PICKER_MAX games). */
typedef enum {
    GAME_PEF_FOLDER,   /* a folder holding a PEF program with a resource fork */
    GAME_MACHO_BUNDLE, /* an .app bundle holding a Mach-O program */
} game_kind;

typedef struct {
    const char *id;          /* "loony-labyrinth": its save folder's name, LOONY_GAME's value */
    const char *title;       /* "Loony Labyrinth": the window title and the picker */
    const char *folder_name; /* its folder (or bundle) in the applications folder */
    const char *exe;         /* the program's path in that folder */
    game_kind kind;
    int screen_width, screen_height; /* the emulated display's size at launch */
} game_info;

size_t game_count(void);

/* The i-th game, or NULL past the end. */
const game_info *game_at(size_t i);

/* The game with this id, or NULL. */
const game_info *game_by_id(const char *id);

/* Where games are installed: $LOONY_APPS_DIR (for tests), or /Applications. */
const char *game_apps_dir(void);

/* The game's folder: <game_apps_dir()>/<folder_name> if its program is
   there, or else the first folder one level down, by name, that holds it
   (/Applications/LittleWing/Loony Labyrinth). Apps and hidden folders aren't
   looked in. If neither has it, <game_apps_dir()>/<folder_name>, for
   messages that say where to put it. */
void game_folder(const game_info *g, char *out, size_t cap);

/* The first game, in table order, whose program is readable in dir, or NULL. */
const game_info *game_in_folder(const char *dir);

/* Writes the games installed in their folders, in table order, to out (at
   most cap) and returns how many there are. */
int game_installed(const game_info **out, int cap);

/* When the high scores in Loony Labyrinth's preferences file were set:
   its "highscore timestamp <n>" values, local time, at most cap of them.
   Scores without one (those the game starts with) are left out. 0 when the
   file is missing or unreadable. */
size_t game_high_score_times(const char *prefs, time_t *out, size_t cap);

/* "2026-10-14 15:20:00", local time, the way the game stamps a high
   score. False for anything else, such as the empty stamp of a score the
   game starts with, or NULL. */
bool game_parse_score_time(const char *s, time_t *out);

/* Loony Labyrinth's high-score table, best first, as get_str and get_int
   read it from the preferences ("highscore <n>", "highscore name <n>" and
   "highscore timestamp <n>", n from 1; cf_pref_string and cf_pref_int for
   the table the game holds now). Writes the entries that have a score to
   out and returns how many. */
#define GAME_HIGH_SCORES 4
#define GAME_SCORE_SCALE 10 /* the game saves a score over 10 (its last digit is always 0) */
typedef struct {
    int64_t score; /* as the game shows it */
    char name[32]; /* Mac Roman */
    time_t when;   /* 0 when the score has no stamp (one the game starts with) */
} game_high_score;
int game_high_scores(const char *(*get_str)(const char *key),
                     bool (*get_int)(const char *key, int64_t *out),
                     game_high_score out[GAME_HIGH_SCORES]);

/* The app's own file in the save root (files_data_root): settings.c's, the
   last game picked and the volume. Named for the picker, which came first. */
#define GAME_PICKER_FILE "picker.plist"

/* Before Plan 8 there was one game, and its files (prefs.plist) were saved
   directly in the save root. Moves every entry of root that is neither a
   known game's folder nor GAME_PICKER_FILE into root/loony-labyrinth. An
   entry already there is left in place and logged, never overwritten.
   Returns the number moved; a missing root moves nothing. */
int game_move_legacy_data(const char *root);
