#include "script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keymap.h"
#include "util.h"

#define MAX_ACTIONS 1024

static struct {
    script_action a[MAX_ACTIONS];
    int n, next;
} SC;

bool script_parse(const char *text, char *err, size_t errlen) {
    memset(&SC, 0, sizeof SC);
    int line_no = 0;
    uint32_t last = 0;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        char line[512];
        if (n >= sizeof line) {
            snprintf(err, errlen, "line %d is too long", line_no + 1);
            return false;
        }
        memcpy(line, p, n);
        line[n] = '\0';
        p = eol ? eol + 1 : p + n;
        line_no++;
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s || *s == '#' || *s == '\r')
            continue;
        unsigned long tick;
        char verb[32], arg[256] = "";
        int got = sscanf(s, "%lu %31s %255s", &tick, verb, arg);
        if (got < 2) {
            snprintf(err, errlen, "line %d: expected \"<tick> <action> [argument]\"", line_no);
            return false;
        }
        if (tick < last) {
            snprintf(err, errlen, "line %d: tick %lu is before tick %u", line_no, tick, last);
            return false;
        }
        if (SC.n == MAX_ACTIONS) {
            snprintf(err, errlen, "more than %d actions", MAX_ACTIONS);
            return false;
        }
        script_action *a = &SC.a[SC.n];
        a->tick = (uint32_t)tick;
        if (strcmp(verb, "down") == 0 || strcmp(verb, "up") == 0) {
            a->kind = verb[0] == 'd' ? SCRIPT_KEY_DOWN : SCRIPT_KEY_UP;
            a->scancode = keymap_scancode_for_name(arg);
            if (a->scancode < 0) {
                snprintf(err, errlen, "line %d: unknown key \"%s\"", line_no, arg);
                return false;
            }
        } else if (strcmp(verb, "screenshot") == 0) {
            if (got < 3) {
                snprintf(err, errlen, "line %d: screenshot needs a file name", line_no);
                return false;
            }
            a->kind = SCRIPT_SCREENSHOT;
            snprintf(a->path, sizeof a->path, "%s", arg);
        } else if (strcmp(verb, "quit") == 0) {
            a->kind = SCRIPT_QUIT;
        } else {
            snprintf(err, errlen, "line %d: unknown action \"%s\"", line_no, verb);
            return false;
        }
        last = a->tick;
        SC.n++;
    }
    return true;
}

bool script_load(const char *path, char *err, size_t errlen) {
    size_t len;
    uint8_t *text = read_file(path, &len);
    if (!text) {
        snprintf(err, errlen, "can't read %s", path);
        return false;
    }
    char *z = realloc(text, len + 1);
    if (!z) {
        free(text);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    z[len] = '\0';
    bool ok = script_parse(z, err, errlen);
    free(z);
    return ok;
}

bool script_next(uint32_t tick, script_action *out) {
    if (SC.next >= SC.n || SC.a[SC.next].tick > tick)
        return false;
    *out = SC.a[SC.next++];
    return true;
}

int script_remaining(void) { return SC.n - SC.next; }
