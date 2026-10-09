#include "game.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

static const game_info games[] = {
    {"loony-labyrinth", "Loony Labyrinth", "Loony Labyrinth", "LOONY LABYRINTH 3.0.1",
     GAME_PEF_FOLDER},
    {"crystal-caliburn", "Crystal Caliburn", "Crystal Caliburn", "CRYSTAL CALIBURN 3.0.1",
     GAME_PEF_FOLDER},
    {"monster-fair", "MONSTER FAIR", "MONSTER FAIR.app", "Contents/MacOS/MONSTER FAIR",
     GAME_MACHO_BUNDLE},
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

#define LEGACY_GAME "loony-labyrinth"

int game_move_legacy_data(const char *root) {
    DIR *d = opendir(root);
    if (!d)
        return 0;
    char dest[PATH_MAX];
    snprintf(dest, sizeof dest, "%s/%s", root, LEGACY_GAME);
    bool dest_ok = false;
    int moved = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strcmp(name, GAME_PICKER_FILE) == 0 ||
            game_by_id(name))
            continue;
        if (!dest_ok && !(dest_ok = make_dirs(dest))) {
            log_msg("can't create %s: %s", dest, strerror(errno));
            break;
        }
        char from[PATH_MAX], to[PATH_MAX];
        snprintf(from, sizeof from, "%s/%s", root, name);
        snprintf(to, sizeof to, "%s/%s", dest, name);
        if (access(to, F_OK) == 0) {
            log_msg("left %s where it is: %s already exists", from, to);
            continue;
        }
        if (rename(from, to) != 0) {
            log_msg("can't move %s to %s: %s", from, to, strerror(errno));
            continue;
        }
        log_msg("moved %s to %s", from, to);
        moved++;
    }
    closedir(d);
    return moved;
}
