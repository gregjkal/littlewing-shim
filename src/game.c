#include "game.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "plist.h"
#include "util.h"

static const game_info games[] = {
    {"loony-labyrinth", "Loony Labyrinth", "Loony Labyrinth", "LOONY LABYRINTH 3.0.1",
     GAME_PEF_FOLDER, 800, 600},
    {"crystal-caliburn", "Crystal Caliburn", "Crystal Caliburn", "CRYSTAL CALIBURN 3.0.1",
     GAME_PEF_FOLDER, 800, 600},
    {"monster-fair", "MONSTER FAIR", "MONSTER FAIR.app", "Contents/MacOS/MONSTER FAIR",
     GAME_MACHO_BUNDLE, 800, 600},
    {"jinni-zeala", "Jinni Zeala", "Jinni Zeala", "JINNI ZEALA 1.2.2", GAME_PEF_FOLDER, 800, 600},
    /* Its table is drawn for 768 lines, and it plays at the display's size. */
    {"mad-daedalus", "Mad Daedalus", "Mad Daedalus.app", "Contents/MacOS/Mad Daedalus",
     GAME_MACHO_BUNDLE, 1024, 768},
    {"angel-egg", "Angel Egg", "Angel Egg.app", "Contents/MacOS/Angel Egg", GAME_MACHO_BUNDLE, 1024,
     768},
    /* It takes the screen at 1024x768 itself (BeginFullScreen). */
    {"golden-logres", "Golden Logres", "Golden Logres.app", "Contents/MacOS/Golden Logres",
     GAME_MACHO_BUNDLE, 1024, 768},
    {"tristan", "Tristan", "TRISTAN IE", "TRISTAN IE 2.5.1", GAME_PEF_FOLDER, 640, 480},
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

bool game_parse_score_time(const char *s, time_t *out) {
    struct tm tm = {.tm_isdst = -1};
    const char *end = s ? strptime(s, "%Y-%m-%d %H:%M:%S", &tm) : NULL;
    if (!end || *end)
        return false;
    *out = mktime(&tm);
    return true;
}

size_t game_high_score_times(const char *prefs, time_t *out, size_t cap) {
    plist_entry *e;
    uint32_t n;
    char err[256];
    if (plist_read(prefs, &e, &n, err, sizeof err) != PLIST_OK)
        return 0;
    size_t k = 0;
    for (uint32_t i = 0; i < n && k < cap; i++)
        if (!e[i].is_number && strncmp(e[i].key, "highscore timestamp ", 20) == 0 &&
            game_parse_score_time(e[i].str, &out[k]))
            k++;
    plist_free(e, n);
    return k;
}

int game_high_scores(const char *(*get_str)(const char *key),
                     bool (*get_int)(const char *key, int64_t *out),
                     game_high_score out[GAME_HIGH_SCORES]) {
    int n = 0;
    for (int i = 0; i < GAME_HIGH_SCORES; i++) {
        char key[64];
        game_high_score *h = &out[n];
        memset(h, 0, sizeof *h);
        snprintf(key, sizeof key, "highscore %d", i + 1);
        if (!get_int(key, &h->score))
            continue;
        h->score *= GAME_SCORE_SCALE;
        snprintf(key, sizeof key, "highscore name %d", i + 1);
        const char *name = get_str(key);
        snprintf(h->name, sizeof h->name, "%s", name ? name : "");
        snprintf(key, sizeof key, "highscore timestamp %d", i + 1);
        if (!game_parse_score_time(get_str(key), &h->when))
            h->when = 0;
        n++;
    }
    return n;
}

const char *game_apps_dir(void) {
    const char *d = getenv("LOONY_APPS_DIR");
    return d && *d ? d : "/Applications";
}

static bool has_program(const char *dir, const game_info *g) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, g->exe);
    return access(path, R_OK) == 0;
}

/* Fills out with g's folder and says whether its program is there. The
   first folder by name wins, so the same disk always gives the same answer. */
static bool find_folder(const game_info *g, char *out, size_t cap) {
    const char *apps = game_apps_dir();
    snprintf(out, cap, "%s/%s", apps, g->folder_name);
    if (has_program(out, g))
        return true;
    DIR *d = opendir(apps);
    if (!d)
        return false;
    char best[NAME_MAX + 1] = "";
    struct dirent *ent;
    while ((ent = readdir(d))) {
        const char *name = ent->d_name;
        if (name[0] == '.' || ends_with(name, ".app") || (*best && strcmp(name, best) >= 0))
            continue;
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s/%s/%s", apps, name, g->folder_name);
        if (has_program(dir, g))
            snprintf(best, sizeof best, "%s", name);
    }
    closedir(d);
    if (*best)
        snprintf(out, cap, "%s/%s/%s", apps, best, g->folder_name);
    return *best;
}

void game_folder(const game_info *g, char *out, size_t cap) {
    find_folder(g, out, cap);
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
        if (find_folder(&games[i], dir, sizeof dir) && n < cap)
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
