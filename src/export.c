#include "export.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

/* ---- the lines ---- */

/* Reads count whole numbers from s, each after one space, to its end. */
static bool numbers(const char *s, uint64_t *out, int count) {
    for (int i = 0; i < count; i++) {
        if (*s++ != ' ' || *s < '0' || *s > '9')
            return false;
        char *e;
        errno = 0;
        unsigned long long v = strtoull(s, &e, 10);
        if (errno)
            return false;
        out[i] = v;
        s = e;
    }
    return *s == '\0';
}

static bool add_file(char ***files, size_t *n, const char *path) {
    if (!*path)
        return false;
    char **grown = realloc(*files, (*n + 1) * sizeof **files);
    if (!grown)
        return false;
    *files = grown;
    if (!(grown[*n] = strdup(path)))
        return false;
    (*n)++;
    return true;
}

bool export_line(export_progress *p, const char *line) {
    uint64_t v[4];
    if (strncmp(line, "seek", 4) == 0 && numbers(line + 4, v, 2)) {
        if (v[0] > UINT32_MAX || v[1] > UINT32_MAX)
            return false;
        p->phase = EXPORT_SEEKING;
        p->tick = (uint32_t)v[0];
        p->target = (uint32_t)v[1];
        return true;
    }
    if (strncmp(line, "clip", 4) == 0 && numbers(line + 4, v, 4)) {
        if (v[0] < 1 || v[0] > v[1] || v[2] > v[3])
            return false;
        p->phase = EXPORT_CLIPPING;
        p->k = (size_t)v[0];
        p->n = (size_t)v[1];
        p->frame = v[2];
        p->frames = v[3];
        return true;
    }
    if (strncmp(line, "wrote ", 6) == 0)
        return add_file(&p->wrote, &p->nwrote, line + 6);
    if (strncmp(line, "skipped ", 8) == 0)
        return add_file(&p->skipped, &p->nskipped, line + 8);
    if (strncmp(line, "error ", 6) == 0) {
        snprintf(p->error, sizeof p->error, "%s", line + 6);
        return true;
    }
    return false;
}

int export_feed(export_progress *p, const char *data, size_t len) {
    int junk = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] != '\n') {
            if (p->partial_len + 1 < sizeof p->partial)
                p->partial[p->partial_len++] = data[i];
            else
                p->overlong = true;
            continue;
        }
        p->partial[p->partial_len] = '\0';
        if (p->overlong || !export_line(p, p->partial))
            junk++;
        p->partial_len = 0;
        p->overlong = false;
    }
    return junk;
}

void export_progress_free(export_progress *p) {
    for (size_t i = 0; i < p->nwrote; i++)
        free(p->wrote[i]);
    for (size_t i = 0; i < p->nskipped; i++)
        free(p->skipped[i]);
    free(p->wrote);
    free(p->skipped);
    memset(p, 0, sizeof *p);
}

/* part of whole as a whole percentage, at most 100. */
static int percent(uint64_t part, uint64_t whole) {
    if (!whole || part >= whole)
        return whole ? 100 : 0;
    return (int)(part * 100 / whole);
}

void export_title(const export_progress *p, char *buf, size_t cap) {
    size_t next = p->nwrote + 1; /* each clip rendered is written, so this is the one to find */
    if (p->phase == EXPORT_CLIPPING)
        snprintf(buf, cap, ", exporting clip %zu of %zu, %d%%", p->k, p->n, percent(p->frame, p->frames));
    else if (p->phase == EXPORT_SEEKING && p->n && next <= p->n)
        snprintf(buf, cap, ", exporting, finding clip %zu of %zu, %d%%", next, p->n,
                 percent(p->tick, p->target));
    else if (p->phase == EXPORT_SEEKING)
        snprintf(buf, cap, ", exporting, finding clip, %d%%", percent(p->tick, p->target));
    else
        snprintf(buf, cap, ", exporting");
}

/* ---- the environment ---- */

/* The viewer's settings, and others that would change what an export does
   or where it writes: none reach it. */
static const char *const dropped[] = {
    "LOONY_WATCH",       "LOONY_WATCH_FROM",   "LOONY_WATCH_SPEED", "LOONY_WATCH_PAUSED",
    "LOONY_WATCH_IN",    "LOONY_WATCH_EXPORT", "LOONY_WATCH_EXPORT_SAID",
    "LOONY_SCRIPT",      "LOONY_SCREENSHOT",   "LOONY_FULLSCREEN",  "LOONY_DATA_DIR",
    "LOONY_EXIT_AFTER",  "LOONY_CLIP_OUT",     "LOONY_WAV",         "LOONY_RECORD",
    "LOONY_PICK",        "LOONY_PICKER_SHOT",  "LOONY_HD_DUMP",
    /* set below */
    "LOONY_CLIP",        "LOONY_CLIP_PROGRESS", "LOONY_REPLAY",     "LOONY_LOG_INHERITED",
};

static bool is_dropped(const char *entry) {
    for (size_t i = 0; i < sizeof dropped / sizeof dropped[0]; i++) {
        size_t n = strlen(dropped[i]);
        if (strncmp(entry, dropped[i], n) == 0 && entry[n] == '=')
            return true;
    }
    return false;
}

static char *joined(const char *name, const char *value) {
    size_t n = strlen(name) + strlen(value) + 2;
    char *s = malloc(n);
    if (s)
        snprintf(s, n, "%s=%s", name, value);
    return s;
}

char **export_child_env(char *const *env, export_kind kind, const char *replay) {
    size_t count = 0;
    while (env && env[count])
        count++;
    char **out = calloc(count + 5, sizeof *out);
    if (!out)
        return NULL;
    size_t n = 0;
    for (size_t i = 0; i < count; i++)
        if (!is_dropped(env[i]) && !(out[n++] = strdup(env[i])))
            goto fail;
    if (!(out[n++] = joined("LOONY_CLIP", kind == EXPORT_MARKS ? "marks" : "clips")) ||
        !(out[n++] = joined("LOONY_CLIP_PROGRESS", "1")) || !(out[n++] = joined("LOONY_REPLAY", replay)) ||
        !(out[n++] = joined("LOONY_LOG_INHERITED", "1")))
        goto fail;
    return out;
fail:
    export_free_env(out);
    return NULL;
}

void export_free_env(char **env) {
    for (size_t i = 0; env && env[i]; i++)
        free(env[i]);
    free(env);
}

/* ---- the process ---- */

extern char **environ;

static struct {
    bool running, cancelled;
    pid_t pid;
    int fd; /* the read end of its stdout */
    export_kind kind;
    export_progress said;
} X = {.fd = -1};

void export_reset(void) {
    if (X.fd >= 0)
        close(X.fd);
    export_progress_free(&X.said);
    memset(&X, 0, sizeof X);
    X.fd = -1;
}

bool export_running(void) { return X.running; }

export_kind export_running_kind(void) { return X.kind; }

const export_progress *export_now(void) { return &X.said; }

bool export_cancelled(void) { return X.cancelled; }

static bool set_fd_flag(int fd, int get, int set, int flag, bool on) {
    int flags = fcntl(fd, get);
    return flags >= 0 && fcntl(fd, set, on ? flags | flag : flags & ~flag) == 0;
}

bool export_start(const char *self, export_kind kind, const char *replay, char *err, size_t errlen) {
    if (X.running) {
        snprintf(err, errlen, "an export is running");
        return false;
    }
    int fds[2];
    if (pipe(fds) != 0) {
        snprintf(err, errlen, "can't make a pipe: %s", strerror(errno));
        return false;
    }
    char **env = export_child_env(environ, kind, replay);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attr);
    /* Only stdin, stdout (the pipe) and stderr (the viewer's log) reach it. */
    posix_spawn_file_actions_addinherit_np(&actions, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK |
                                        POSIX_SPAWN_SETSIGDEF);
    pid_t pid = -1;
    int rc = env ? posix_spawn(&pid, self, &actions, &attr, (char *const[]){(char *)self, NULL}, env)
                 : ENOMEM;
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    export_free_env(env);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        snprintf(err, errlen, "can't start %s: %s", self, strerror(rc));
        return false;
    }
    set_fd_flag(fds[0], F_GETFD, F_SETFD, FD_CLOEXEC, true);
    set_fd_flag(fds[0], F_GETFL, F_SETFL, O_NONBLOCK, true);
    export_reset();
    X.running = true;
    X.pid = pid;
    X.fd = fds[0];
    X.kind = kind;
    return true;
}

/* Reads all the pipe has now. False at its end (or on an error). */
static bool drain(void) {
    char buf[4096];
    for (;;) {
        ssize_t n = read(X.fd, buf, sizeof buf);
        if (n > 0) {
            int junk = export_feed(&X.said, buf, (size_t)n);
            if (junk)
                log_msg("export: %d line%s from the export not understood", junk, junk == 1 ? "" : "s");
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }
}

bool export_poll(int *status) {
    if (!X.running)
        return false;
    if (X.fd >= 0 && !drain()) {
        close(X.fd);
        X.fd = -1;
    }
    int st = 0;
    pid_t r = waitpid(X.pid, &st, WNOHANG);
    if (r == 0)
        return false;
    if (r < 0) /* not ours any more: nothing to wait for, so it counts as failed */
        log_msg("export: can't wait for %d: %s", (int)X.pid, strerror(errno));
    if (X.fd >= 0) { /* what it wrote just before it ended */
        drain();
        close(X.fd);
        X.fd = -1;
    }
    X.running = false;
    *status = r < 0 ? 127 : WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 127;
    return true;
}

void export_cancel(void) {
    if (!X.running)
        return;
    X.cancelled = true;
    if (kill(X.pid, SIGTERM) != 0)
        log_msg("export: can't stop %d: %s", (int)X.pid, strerror(errno));
}

bool export_stop(double seconds, int *status) {
    if (!X.running)
        return false;
    export_cancel();
    struct timespec gap = {0, 10 * 1000000};
    for (double waited = 0; waited < seconds; waited += 0.01) {
        if (export_poll(status))
            return true;
        nanosleep(&gap, NULL);
    }
    return export_poll(status);
}

/* What has been said, as the lines that say it again. */
static char *said_lines(void) {
    const export_progress *p = &X.said;
    size_t cap = 256 + strlen(p->error) + p->partial_len;
    for (size_t i = 0; i < p->nwrote; i++)
        cap += strlen(p->wrote[i]) + 8;
    for (size_t i = 0; i < p->nskipped; i++)
        cap += strlen(p->skipped[i]) + 10;
    char *s = malloc(cap);
    if (!s)
        return NULL;
    size_t len = 0;
    s[0] = '\0';
    for (size_t i = 0; i < p->nskipped; i++)
        len += (size_t)snprintf(s + len, cap - len, "skipped %s\n", p->skipped[i]);
    for (size_t i = 0; i < p->nwrote; i++)
        len += (size_t)snprintf(s + len, cap - len, "wrote %s\n", p->wrote[i]);
    if (p->error[0])
        len += (size_t)snprintf(s + len, cap - len, "error %s\n", p->error);
    if (p->n) /* the last clip line, then the seek after it, if that came last */
        len += (size_t)snprintf(s + len, cap - len, "clip %zu %zu %llu %llu\n", p->k, p->n,
                                (unsigned long long)p->frame, (unsigned long long)p->frames);
    if (p->phase == EXPORT_SEEKING)
        len += (size_t)snprintf(s + len, cap - len, "seek %u %u\n", p->tick, p->target);
    /* A line read only in part (one longer than PIPE_BUF can come in two
       writes): its start, unended, so the next read ends it. */
    memcpy(s + len, p->partial, p->partial_len);
    s[len + p->partial_len] = '\0';
    return s;
}

void export_pass_on(void) {
    unsetenv("LOONY_WATCH_EXPORT");
    unsetenv("LOONY_WATCH_EXPORT_SAID");
    if (!X.running)
        return;
    if (X.fd >= 0 && !drain()) {
        close(X.fd);
        X.fd = -1;
    }
    char v[64];
    snprintf(v, sizeof v, "%d %d %d %d", (int)X.pid, X.fd, (int)X.kind, X.cancelled);
    char *said = said_lines();
    if (X.fd >= 0)
        set_fd_flag(X.fd, F_GETFD, F_SETFD, FD_CLOEXEC, false);
    setenv("LOONY_WATCH_EXPORT", v, 1);
    setenv("LOONY_WATCH_EXPORT_SAID", said ? said : "", 1);
    free(said);
}

/* Whether pid is a child of this process, without reaping it if it has
   ended. */
static bool is_child(pid_t pid) {
    siginfo_t info;
    return waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT) == 0 || errno != ECHILD;
}

bool export_take_up(char *err, size_t errlen) {
    const char *v = getenv("LOONY_WATCH_EXPORT");
    char *said = getenv("LOONY_WATCH_EXPORT_SAID");
    bool ok = true;
    if (v && *v) {
        int pid, fd, kind, cancelled;
        char end;
        if (sscanf(v, "%d %d %d %d%c", &pid, &fd, &kind, &cancelled, &end) != 4 || pid <= 0 ||
            fd < -1 || (kind != EXPORT_CLIPS && kind != EXPORT_MARKS)) {
            snprintf(err, errlen, "LOONY_WATCH_EXPORT=%s isn't \"<pid> <fd> <kind> <cancelled>\"", v);
            ok = false;
        } else if (!is_child(pid)) {
            snprintf(err, errlen, "LOONY_WATCH_EXPORT names %d, which isn't this program's", pid);
            ok = false;
        } else {
            export_reset();
            X.running = true;
            X.cancelled = cancelled != 0;
            X.pid = pid;
            X.fd = fd >= 0 && fcntl(fd, F_GETFD) >= 0 ? fd : -1;
            X.kind = (export_kind)kind;
            if (X.fd >= 0) {
                set_fd_flag(X.fd, F_GETFD, F_SETFD, FD_CLOEXEC, true);
                set_fd_flag(X.fd, F_GETFL, F_SETFL, O_NONBLOCK, true);
            }
            if (said)
                export_feed(&X.said, said, strlen(said));
        }
    }
    unsetenv("LOONY_WATCH_EXPORT");
    unsetenv("LOONY_WATCH_EXPORT_SAID");
    return ok;
}
