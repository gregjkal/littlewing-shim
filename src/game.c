#include "game.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const game_info games[] = {
    {"loony-labyrinth", "Loony Labyrinth", "Loony Labyrinth", "LOONY LABYRINTH 3.0.1"},
    {"crystal-caliburn", "Crystal Caliburn", "Crystal Caliburn", "CRYSTAL CALIBURN 3.0.1"},
};
#define NGAMES (sizeof games / sizeof games[0])

size_t game_count(void) { return NGAMES; }

const game_info *game_at(size_t i) { return i < NGAMES ? &games[i] : NULL; }

const game_info *game_by_id(const char *id) {
    for (size_t i = 0; i < NGAMES; i++)
        if (strcmp(games[i].id, id) == 0)
            return &games[i];
    return NULL;
}

const char *game_apps_dir(void) {
    const char *d = getenv("LOONY_APPS_DIR");
    return d && *d ? d : "/Applications";
}

void game_folder(const game_info *g, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", game_apps_dir(), g->folder_name);
}

static bool has_program(const char *dir, const game_info *g) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, g->exe);
    return access(path, R_OK) == 0;
}

const game_info *game_in_folder(const char *dir) {
    for (size_t i = 0; i < NGAMES; i++)
        if (has_program(dir, &games[i]))
            return &games[i];
    return NULL;
}

int game_installed(const game_info **out, int cap) {
    int n = 0;
    for (size_t i = 0; i < NGAMES; i++) {
        char dir[PATH_MAX];
        game_folder(&games[i], dir, sizeof dir);
        if (has_program(dir, &games[i]) && n < cap)
            out[n++] = &games[i];
    }
    return n;
}
