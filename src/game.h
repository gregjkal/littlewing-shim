#pragma once
#include <stddef.h>

/* The LittleWing games the shim can play. They share one engine and make
   the same 132 calls, so they differ only in names. Adding a game is a row
   in game.c (the picker is laid out for two; a third needs a new layout). */
typedef struct {
    const char *id;          /* "loony-labyrinth": its save folder's name, LOONY_GAME's value */
    const char *title;       /* "Loony Labyrinth": the window title and the picker */
    const char *folder_name; /* its folder in the applications folder */
    const char *exe;         /* the program's file name in that folder */
} game_info;

size_t game_count(void);

/* The i-th game, or NULL past the end. */
const game_info *game_at(size_t i);

/* The game with this id, or NULL. */
const game_info *game_by_id(const char *id);

/* Where games are installed: $LOONY_APPS_DIR (for tests), or /Applications. */
const char *game_apps_dir(void);

/* The game's folder: <game_apps_dir()>/<folder_name>. */
void game_folder(const game_info *g, char *out, size_t cap);

/* The first game, in table order, whose program is readable in dir, or NULL. */
const game_info *game_in_folder(const char *dir);

/* Writes the games installed in their folders, in table order, to out (at
   most cap) and returns how many there are. */
int game_installed(const game_info **out, int cap);
