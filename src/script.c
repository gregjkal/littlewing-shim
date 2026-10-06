#include "script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keymap.h"
#include "util.h"

#define MAX_ACTIONS 1000000

static struct {
    script_action *a;
    int n, cap, next;
} SC;

static bool parse(const char *text, char *err, size_t errlen) {
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
        if (SC.n == SC.cap) {
            SC.cap = SC.cap ? SC.cap * 2 : 256;
            SC.a = realloc(SC.a, (size_t)SC.cap * sizeof *SC.a);
            if (!SC.a)
                fatal("out of memory");
        }
        script_action *a = &SC.a[SC.n];
        memset(a, 0, sizeof *a);
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
        } else if (strcmp(verb, "blur") == 0) {
            a->kind = SCRIPT_BLUR;
        } else if (strcmp(verb, "focus") == 0) {
            a->kind = SCRIPT_FOCUS;
        } else if (strcmp(verb, "click") == 0) {
            a->kind = SCRIPT_CLICK;
            if (sscanf(s, "%*u %*s %d %d", &a->x, &a->y) != 2) {
                snprintf(err, errlen, "line %d: click needs x and y", line_no);
                return false;
            }
        } else if (strcmp(verb, "type") == 0) {
            a->kind = SCRIPT_TYPE;
            const char *text = strstr(s, "type") + 4;
            if (*text == ' ')
                text++;
            snprintf(a->path, sizeof a->path, "%s", text);
            size_t tl = strlen(a->path);
            if (tl && a->path[tl - 1] == '\r')
                a->path[tl - 1] = '\0';
        } else {
            snprintf(err, errlen, "line %d: unknown action \"%s\"", line_no, verb);
            return false;
        }
        last = a->tick;
        SC.n++;
    }
    return true;
}

bool script_parse(const char *text, char *err, size_t errlen) {
    free(SC.a);
    memset(&SC, 0, sizeof SC);
    if (parse(text, err, errlen))
        return true;
    SC.n = 0; /* nothing from a script with an error */
    return false;
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
