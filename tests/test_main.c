#include "test.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
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

const char *test_cc_dir(void) {
    const char *d = getenv("LOONY_CC_DIR");
    return d && *d ? d : "/Applications/Crystal Caliburn";
}

bool test_cc_present(void) {
    char path[1100];
    snprintf(path, sizeof path, "%s/CRYSTAL CALIBURN 3.0.1", test_cc_dir());
    return access(path, R_OK) == 0;
}

const char *test_mf_app(void) {
    const char *d = getenv("LOONY_MF_APP");
    return d && *d ? d : "/Applications/MONSTER FAIR.app";
}

const char *test_mf_exe_path(void) {
    static char path[1100];
    snprintf(path, sizeof path, "%s/Contents/MacOS/MONSTER FAIR", test_mf_app());
    return path;
}

bool test_mf_present(void) {
    return access(test_mf_exe_path(), R_OK) == 0;
}

bool test_slow_enabled(void) {
    const char *s = getenv("LOONY_SLOW_TESTS");
    return s && *s && strcmp(s, "0") != 0;
}

void test_tmp_dir(char *out, size_t cap) {
    const char *t = getenv("TMPDIR");
    snprintf(out, cap, "%s/loony-test-XXXXXX", t && *t ? t : "/tmp");
    if (!mkdtemp(out)) {
        fprintf(stderr, "mkdtemp %s failed\n", out);
        exit(1);
    }
}

void test_remove_tree(const char *path) {
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", path);
    if (system(cmd) != 0)
        fprintf(stderr, "can't remove %s\n", path);
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

/* A test's exit status when it runs in a child of its own. */
#define CHILD_PASSED 0
#define CHILD_FAILED 1
#define CHILD_SKIPPED 77

/* Runs test i in a forked child with stdout and stderr in out_path and a
   save folder of its own under data_dir, so tests can run side by side. */
static pid_t start_test(int i, const char *data_dir, const char *out_path) {
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    close(fd);
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/%d", data_dir, i);
    mkdir(dir, 0700);
    setenv("LOONY_DATA_DIR", dir, 1);
    cur_failed = cur_skipped = false;
    tests[i].fn();
    fflush(stdout);
    fflush(stderr);
    _exit(cur_failed ? CHILD_FAILED : cur_skipped ? CHILD_SKIPPED : CHILD_PASSED);
}

static void print_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        fwrite(buf, 1, n, stderr);
    fclose(f);
}

int main(int argc, char **argv) {
    /* No test may open a real window or play sound; children inherit this too. */
    setenv("SDL_VIDEO_DRIVER", "dummy", 1);
    setenv("SDL_AUDIO_DRIVER", "dummy", 1);
    /* -j N runs N tests at a time, each in a child of its own (the default is
       one per CPU); -j 1 runs them one after another in this process. */
    long jobs = sysconf(_SC_NPROCESSORS_ONLN);
    const char *filter = NULL;
    for (int a = 1; a < argc; a++) {
        if (strncmp(argv[a], "-j", 2) == 0) {
            const char *n = argv[a][2] ? argv[a] + 2 : a + 1 < argc ? argv[++a] : "";
            char *end;
            jobs = strtol(n, &end, 10);
            if (!*n || *end || jobs < 1) {
                fprintf(stderr, "usage: loony_tests [-j N] [substring]\n");
                return 2;
            }
        } else {
            filter = argv[a];
        }
    }
    if (jobs < 1)
        jobs = 1;
    /* Nor touch the real preferences in ~/Library/Application Support. */
    char data_dir[1024];
    test_tmp_dir(data_dir, sizeof data_dir);
    setenv("LOONY_DATA_DIR", data_dir, 1);
    int passed = 0, failed = 0, skipped = 0;
    const char *failures[MAX_TESTS];
    if (jobs == 1) {
        for (int i = 0; i < ntests; i++) {
            if (filter && !strstr(tests[i].name, filter))
                continue;
            cur_failed = cur_skipped = false;
            fprintf(stderr, "%s\n", tests[i].name);
            tests[i].fn();
            if (cur_failed)
                failures[failed++] = tests[i].name;
            else if (cur_skipped)
                skipped++;
            else
                passed++;
        }
    } else {
        /* Each test's output is printed as one block when it finishes. */
        pid_t pids[MAX_TESTS] = {0};
        int running = 0, next = 0;
        for (;;) {
            while (running < jobs && next < ntests) {
                int i = next++;
                if (filter && !strstr(tests[i].name, filter))
                    continue;
                char out_path[1100];
                snprintf(out_path, sizeof out_path, "%s/%d.out", data_dir, i);
                pids[i] = start_test(i, data_dir, out_path);
                if (pids[i] < 0) {
                    pids[i] = 0;
                    fprintf(stderr, "%s\n  can't fork\n", tests[i].name);
                    failures[failed++] = tests[i].name;
                    continue;
                }
                running++;
            }
            if (running == 0)
                break;
            int status;
            pid_t pid = wait(&status);
            if (pid < 0)
                break;
            int i = 0;
            while (i < ntests && pids[i] != pid)
                i++;
            if (i == ntests)
                continue; /* not one of ours */
            pids[i] = 0;
            running--;
            char out_path[1100];
            snprintf(out_path, sizeof out_path, "%s/%d.out", data_dir, i);
            fprintf(stderr, "%s\n", tests[i].name);
            print_file(out_path);
            int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            if (code == CHILD_PASSED) {
                passed++;
            } else if (code == CHILD_SKIPPED) {
                skipped++;
            } else {
                if (WIFSIGNALED(status))
                    fprintf(stderr, "  crashed: signal %d\n", WTERMSIG(status));
                else if (code != CHILD_FAILED)
                    fprintf(stderr, "  exited with status %d\n", code);
                failures[failed++] = tests[i].name;
            }
        }
    }
    test_remove_tree(data_dir);
    fprintf(stderr, "\n%d passed, %d failed, %d skipped\n", passed, failed, skipped);
    for (int f = 0; f < failed; f++)
        fprintf(stderr, "  failed: %s\n", failures[f]);
    return failed ? 1 : 0;
}
