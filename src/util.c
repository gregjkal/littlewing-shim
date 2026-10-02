#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static util_failure_fn failure_hook;

void util_set_failure_hook(util_failure_fn fn) { failure_hook = fn; }

void util_report_failure(const char *msg) {
    util_failure_fn fn = failure_hook;
    failure_hook = NULL; /* once, even if the hook itself fails */
    if (fn)
        fn(msg);
}

void fatal(const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "loony: fatal: %s\n", msg);
    util_report_failure(msg);
    exit(2);
}

void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("loony: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

uint8_t *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 65536;
            uint8_t *grown = realloc(buf, cap);
            if (!grown) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = grown;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0)
            break;
    }
    int failed = ferror(f);
    fclose(f);
    if (failed) {
        free(buf);
        return NULL;
    }
    *len_out = len;
    return buf;
}

bool make_dirs(const char *path) {
    char p[1024];
    if (snprintf(p, sizeof p, "%s", path) >= (int)sizeof p) {
        errno = ENAMETOOLONG;
        return false;
    }
    for (char *s = p + 1;; s++) {
        if (*s != '/' && *s != '\0')
            continue;
        char c = *s;
        *s = '\0';
        if (mkdir(p, 0755) != 0 && errno != EEXIST)
            return false;
        *s = c;
        if (!c)
            break;
    }
    struct stat st;
    if (stat(p, &st) != 0)
        return false;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return false;
    }
    return true;
}

uint32_t fnv1a32(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}
