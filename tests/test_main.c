#include "test.h"

#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_TESTS 512

static struct {
    const char *name;
    test_fn fn;
} tests[MAX_TESTS];
static int ntests;
static bool cur_failed, cur_skipped;

void test_register(const char *name, test_fn fn) {
    if (ntests == MAX_TESTS) {
        fprintf(stderr, "too many tests\n");
        exit(1);
    }
    tests[ntests].name = name;
    tests[ntests].fn = fn;
    ntests++;
}

void test_fail(const char *file, int line, const char *msg) {
    fprintf(stderr, "  %s:%d: CHECK failed: %s\n", file, line, msg);
    cur_failed = true;
}

void test_skip(const char *reason) {
    fprintf(stderr, "  skipped: %s\n", reason);
    cur_skipped = true;
}

const char *test_game_dir(void) {
    const char *d = getenv("LOONY_GAME_DIR");
    return d && *d ? d : "/Applications/Loony Labyrinth";
}

const char *test_game_exe_path(void) {
    static char path[1024];
    snprintf(path, sizeof path, "%s/LOONY LABYRINTH 3.0.1", test_game_dir());
    return path;
}

bool test_game_present(void) {
    return access(test_game_exe_path(), R_OK) == 0;
}

int test_run_child(void (*fn)(void *), void *arg, char *out, size_t outlen) {
    int fds[2];
    if (pipe(fds) != 0)
        return -2;
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        fn(arg);
        _exit(0);
    }
    close(fds[1]);
    size_t len = 0;
    for (;;) {
        char tmp[4096];
        ssize_t n = read(fds[0], tmp, sizeof tmp);
        if (n <= 0)
            break;
        size_t take = (size_t)n;
        if (take > outlen - 1 - len)
            take = outlen - 1 - len;
        memcpy(out + len, tmp, take);
        len += take;
    }
    out[len] = '\0';
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(int argc, char **argv) {
    /* No test may open a real window; children inherit this too. */
    setenv("SDL_VIDEO_DRIVER", "dummy", 1);
    const char *filter = argc > 1 ? argv[1] : NULL;
    int passed = 0, failed = 0, skipped = 0;
    for (int i = 0; i < ntests; i++) {
        if (filter && !strstr(tests[i].name, filter))
            continue;
        cur_failed = cur_skipped = false;
        fprintf(stderr, "%s\n", tests[i].name);
        tests[i].fn();
        if (cur_failed)
            failed++;
        else if (cur_skipped)
            skipped++;
        else
            passed++;
    }
    fprintf(stderr, "\n%d passed, %d failed, %d skipped\n", passed, failed, skipped);
    return failed ? 1 : 0;
}
