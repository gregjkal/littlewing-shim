#include "settings.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "files.h"
#include "game.h"
#include "plist.h"
#include "util.h"

static bool path(char *out, size_t cap) {
    char root[PATH_MAX];
    if (!files_data_root(root, sizeof root))
        return false;
    snprintf(out, cap, "%s/" GAME_PICKER_FILE, root);
    return true;
}

/* The entry for key, if the file has one. Free *all with plist_free. */
static const plist_entry *find(const char *key, plist_entry **all, uint32_t *n) {
    char p[PATH_MAX], err[256];
    *all = NULL;
    *n = 0;
    if (!path(p, sizeof p) || plist_read(p, all, n, err, sizeof err) != PLIST_OK)
        return NULL;
    for (uint32_t i = 0; i < *n; i++)
        if (strcmp((*all)[i].key, key) == 0)
            return &(*all)[i];
    return NULL;
}

bool settings_get_str(const char *key, char *out, size_t cap) {
    plist_entry *all;
    uint32_t n;
    const plist_entry *e = find(key, &all, &n);
    bool ok = e && !e->is_number;
    if (ok)
        snprintf(out, cap, "%s", e->str);
    plist_free(all, n);
    return ok;
}

bool settings_get_int(const char *key, int64_t *out) {
    plist_entry *all;
    uint32_t n;
    const plist_entry *e = find(key, &all, &n);
    bool ok = e && e->is_number;
    if (ok)
        *out = e->num;
    plist_free(all, n);
    return ok;
}

/* Writes the file's entries with v in place of (or after) the one for v.key. */
static void set(plist_entry v) {
    char p[PATH_MAX], err[256];
    if (!path(p, sizeof p))
        return;
    plist_entry *all = NULL;
    uint32_t n = 0;
    plist_status st = plist_read(p, &all, &n, err, sizeof err);
    if (st != PLIST_OK && st != PLIST_MISSING)
        log_msg("settings: %s (rewriting it)", err);
    plist_entry *e = malloc(((size_t)n + 1) * sizeof *e);
    if (!e)
        fatal("out of memory");
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(all[i].key, v.key) != 0)
            e[k++] = all[i];
    e[k++] = v;
    if (!plist_write(p, e, k, err, sizeof err))
        log_msg("settings: can't save %s: %s", v.key, err);
    free(e);
    plist_free(all, n);
}

void settings_set_str(const char *key, const char *value) {
    set((plist_entry){.key = (char *)key, .str = (char *)value});
}

void settings_set_int(const char *key, int64_t value) {
    set((plist_entry){.key = (char *)key, .is_number = true, .num = value});
}
