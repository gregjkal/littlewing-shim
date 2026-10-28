#include "license.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "plist.h"
#include "util.h"

static const char *const keys[] = {"user email", "user id"};
static const char *const values[] = {LICENSE_EMAIL, LICENSE_KEY};
#define NKEYS (sizeof keys / sizeof keys[0])

static plist_entry *find(plist_entry *e, uint32_t n, const char *key) {
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(e[i].key, key) == 0)
            return &e[i];
    return NULL;
}

static char *copy_str(const char *s) {
    char *d = strdup(s);
    if (!d)
        fatal("out of memory");
    return d;
}

/* "user email" and "user id" in prefs.plist, unless both hold a string. */
static bool save_in_prefs(const char *data_dir) {
    char path[PATH_MAX], err[256];
    snprintf(path, sizeof path, "%s/prefs.plist", data_dir);
    plist_entry *e = NULL;
    uint32_t n = 0;
    plist_status st = plist_read(path, &e, &n, err, sizeof err);
    if (st != PLIST_OK && st != PLIST_MISSING) {
        log_msg("license: %s can't be read, so no license is saved: %s", path, err);
        return false;
    }
    bool licensed = true;
    for (size_t i = 0; i < NKEYS; i++) {
        plist_entry *k = find(e, n, keys[i]);
        licensed = licensed && k && !k->is_number && k->str[0];
    }
    bool ok = true;
    if (!licensed) {
        if (!(e = realloc(e, (n + NKEYS) * sizeof *e)))
            fatal("out of memory");
        for (size_t i = 0; i < NKEYS; i++) {
            plist_entry *k = find(e, n, keys[i]);
            if (!k) {
                k = &e[n++];
                memset(k, 0, sizeof *k);
                k->key = copy_str(keys[i]);
            }
            free(k->str);
            k->is_number = false;
            k->str = copy_str(values[i]);
        }
        ok = plist_write(path, e, n, err, sizeof err);
        if (ok)
            log_msg("license: registered in %s", path);
        else
            log_msg("license: can't write %s: %s", path, err);
    }
    plist_free(e, n);
    return ok;
}

/* The game's own license file, unless it is there. */
static bool save_in_file(const game_info *g, const char *data_dir) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", data_dir, g->license_file);
    if (access(path, F_OK) == 0)
        return true;
    uint8_t *b = calloc(1, (size_t)g->license_size);
    if (!b)
        fatal("out of memory");
    memcpy(b, LICENSE_EMAIL, strlen(LICENSE_EMAIL));
    memcpy(b + g->license_key_at, LICENSE_KEY, strlen(LICENSE_KEY));
    FILE *f = make_dirs(data_dir) ? fopen(path, "wb") : NULL;
    bool ok = f && fwrite(b, 1, (size_t)g->license_size, f) == (size_t)g->license_size;
    if (f && fclose(f) != 0)
        ok = false;
    free(b);
    if (ok)
        log_msg("license: registered in %s", path);
    else
        log_msg("license: can't write %s", path);
    return ok;
}

bool license_save_default(const game_info *g, const char *data_dir) {
    const char *off = getenv("LOONY_UNREGISTERED");
    if (off && strcmp(off, "1") == 0)
        return true;
    return g->license_file ? save_in_file(g, data_dir) : save_in_prefs(data_dir);
}
