#include "test.h"

#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "game.h"

#define MAX_TESTS 1024

static struct {
    const char *name;
    test_fn fn;
    bool slow;
} tests[MAX_TESTS];
static int ntests;
static bool cur_failed, cur_skipped;

void test_register(const char *name, test_fn fn, bool slow) {
    if (ntests == MAX_TESTS) {
        fprintf(stderr, "too many tests\n");
        exit(1);
    }
    tests[ntests].name = name;
    tests[ntests].fn = fn;
    tests[ntests].slow = slow;
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

/* $var, or where the app would look for the game with this id. */
static const char *game_dir(const char *var, const char *id, char *buf, size_t cap) {
    const char *d = getenv(var);
    if (d && *d)
        return d;
    game_folder(game_by_id(id), buf, cap);
    return buf;
}

const char *test_game_dir(void) {
    static char dir[1024];
    return game_dir("LOONY_GAME_DIR", "loony-labyrinth", dir, sizeof dir);
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
    static char dir[1024];
    return game_dir("LOONY_CC_DIR", "crystal-caliburn", dir, sizeof dir);
}

bool test_cc_present(void) {
    char path[1100];
    snprintf(path, sizeof path, "%s/CRYSTAL CALIBURN 3.0.1", test_cc_dir());
    return access(path, R_OK) == 0;
}

const char *test_mf_app(void) {
    static char dir[1024];
    return game_dir("LOONY_MF_APP", "monster-fair", dir, sizeof dir);
}

const char *test_mf_exe_path(void) {
    static char path[1100];
    snprintf(path, sizeof path, "%s/Contents/MacOS/MONSTER FAIR", test_mf_app());
    return path;
}

bool test_mf_present(void) {
    return access(test_mf_exe_path(), R_OK) == 0;
}

const char *test_jz_dir(void) {
    static char dir[1024];
    return game_dir("LOONY_JZ_DIR", "jinni-zeala", dir, sizeof dir);
}

const char *test_jz_exe_path(void) {
    static char path[1100];
    snprintf(path, sizeof path, "%s/JINNI ZEALA 1.2.2", test_jz_dir());
    return path;
}

bool test_jz_present(void) {
    return access(test_jz_exe_path(), R_OK) == 0;
}

const char *test_md_app(void) {
    static char dir[1024];
    return game_dir("LOONY_MD_APP", "mad-daedalus", dir, sizeof dir);
}

const char *test_md_exe_path(void) {
    static char path[1100];
    snprintf(path, sizeof path, "%s/Contents/MacOS/Mad Daedalus", test_md_app());
    return path;
}

bool test_md_present(void) {
    return access(test_md_exe_path(), R_OK) == 0;
}

const char *test_ae_app(void) {
    const char *d = getenv("LOONY_AE_APP");
    return d && *d ? d : "/Applications/Angel Egg.app";
}

const char *test_ae_exe_path(void) {
    static char path[1100];
    snprintf(path, sizeof path, "%s/Contents/MacOS/Angel Egg", test_ae_app());
    return path;
}

bool test_ae_present(void) {
    return access(test_ae_exe_path(), R_OK) == 0;
}

const char *test_gl_app(void) {
    const char *d = getenv("LOONY_GL_APP");
    return d && *d ? d : "/Applications/Golden Logres.app";
}

const char *test_gl_exe_path(void) {
    static char path[1100];
    snprintf(path, sizeof path, "%s/Contents/MacOS/Golden Logres", test_gl_app());
    return path;
}

bool test_gl_present(void) {
    return access(test_gl_exe_path(), R_OK) == 0;
}

const char *test_tr_dir(void) {
    static char dir[1024];
    return game_dir("LOONY_TR_DIR", "tristan", dir, sizeof dir);
}

const char *test_tr_exe_path(void) {
    static char path[1100];
    snprintf(path, sizeof path, "%s/TRISTAN IE 2.5.1", test_tr_dir());
    return path;
}

bool test_tr_present(void) {
    return access(test_tr_exe_path(), R_OK) == 0;
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

/* The output of the last KEPT_CHILDREN children test_run_child (or
   test_run_children) ran in the current test, each its last KEPT_BYTES: printed if the test fails, since
   a test keeps a child's output in a buffer it checks, and a crash, an
   unexpected status or a check of what a child said otherwise leaves
   nothing to go on. */
#define KEPT_CHILDREN 8
#define KEPT_BYTES 16384
static struct {
    char *text;
    int status;
} kept[KEPT_CHILDREN];
static int nkept;

static void keep_child(const char *out, int status) {
    size_t len = strlen(out);
    const char *from = len > KEPT_BYTES ? out + len - KEPT_BYTES : out;
    int slot = nkept++ % KEPT_CHILDREN;
    free(kept[slot].text);
    kept[slot].text = strdup(from);
    kept[slot].status = status;
}

static void print_kept(void) {
    int first = nkept > KEPT_CHILDREN ? nkept - KEPT_CHILDREN : 0;
    for (int k = first; k < nkept; k++) {
        int slot = k % KEPT_CHILDREN;
        fprintf(stderr, "  child %d of the test exited with status %d; its output%s:\n%s\n", k + 1,
                kept[slot].status, strlen(kept[slot].text) >= KEPT_BYTES ? ", the end of it" : "",
                kept[slot].text ? kept[slot].text : "");
    }
}

static void forget_kept(void) {
    for (int k = 0; k < KEPT_CHILDREN; k++) {
        free(kept[k].text);
        kept[k].text = NULL;
    }
    nkept = 0;
}

#define MAX_CHILDREN 16 /* test_run_children's most at once */

void test_run_children(size_t n, void (*fn)(void *), void *const *args, char *const *outs,
                       size_t outlen, int *codes) {
    if (n > MAX_CHILDREN) {
        fprintf(stderr, "test_run_children: %zu children, at most %d\n", n, MAX_CHILDREN);
        abort();
    }
    pid_t pids[MAX_CHILDREN];
    size_t lens[MAX_CHILDREN];
    struct pollfd polls[MAX_CHILDREN];
    for (size_t i = 0; i < n; i++) {
        lens[i] = 0;
        outs[i][0] = '\0';
        pids[i] = -1;
        polls[i] = (struct pollfd){.fd = -1, .events = POLLIN};
        int fds[2];
        if (pipe(fds) != 0)
            continue;
        fflush(stdout);
        fflush(stderr);
        pids[i] = fork();
        if (pids[i] == 0) {
            close(fds[0]);
            dup2(fds[1], STDERR_FILENO);
            close(fds[1]);
            fn(args[i]);
            _exit(0);
        }
        close(fds[1]);
        if (pids[i] < 0)
            close(fds[0]);
        else
            polls[i].fd = fds[0];
    }
    /* Read each as it writes, so none waits on a full pipe; poll skips the
       ones closed (fd -1). */
    for (;;) {
        bool open = false;
        for (size_t i = 0; i < n; i++)
            open |= polls[i].fd >= 0;
        if (!open || poll(polls, (nfds_t)n, -1) < 0)
            break;
        for (size_t i = 0; i < n; i++) {
            if (polls[i].fd < 0 || !(polls[i].revents & (POLLIN | POLLHUP)))
                continue;
            char tmp[4096];
            ssize_t got = read(polls[i].fd, tmp, sizeof tmp);
            if (got <= 0) {
                close(polls[i].fd);
                polls[i].fd = -1;
                continue;
            }
            size_t take = (size_t)got;
            if (take > outlen - 1 - lens[i])
                take = outlen - 1 - lens[i];
            memcpy(outs[i] + lens[i], tmp, take);
            lens[i] += take;
            outs[i][lens[i]] = '\0';
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (pids[i] < 0) {
            codes[i] = -2;
            continue;
        }
        int status = 0;
        waitpid(pids[i], &status, 0);
        codes[i] = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        keep_child(outs[i], codes[i]);
    }
}

int test_run_child(void (*fn)(void *), void *arg, char *out, size_t outlen) {
    int code;
    test_run_children(1, fn, (void *const[]){arg}, (char *const[]){out}, outlen, &code);
    return code;
}

/* Set in the forked child a test runs in (start_test). */
static bool forked;

static void exec_self(void *filter) {
    char self[PATH_MAX];
    uint32_t size = sizeof self;
    if (_NSGetExecutablePath(self, &size) == 0)
        execl(self, "loony_tests", "-j", "1", (const char *)filter, (char *)NULL);
    _exit(127);
}

bool test_rerun_unforked(const char *name) {
    if (!forked)
        return false;
    char filter[256];
    snprintf(filter, sizeof filter, "=%s", name);
    static char out[65536];
    int status = test_run_child(exec_self, filter, out, sizeof out);
    if (status == 0 && strstr(out, "\n1 passed, 0 failed"))
        return true;
    fputs(out, stderr);
    if (status == 0 && strstr(out, "0 failed, 1 skipped"))
        cur_skipped = true;
    else
        test_fail(__FILE__, __LINE__, "the test failed in a process of its own");
    return true;
}

/* What the command line picks: with --slow, the slow tests alone; with
   filters, the tests any one of them matches, each once. A filter is a part
   of a test's name, or with a leading '=', the whole name. */
#define MAX_FILTERS 64
static const char *filters[MAX_FILTERS];
static int nfilters;
static bool slow_only;

static bool selected(int i) {
    if (slow_only && !tests[i].slow)
        return false;
    if (nfilters == 0)
        return true;
    for (int f = 0; f < nfilters; f++) {
        const char *filter = filters[f];
        if (filter[0] == '=' ? strcmp(tests[i].name, filter + 1) == 0
                             : strstr(tests[i].name, filter) != NULL)
            return true;
    }
    return false;
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
    forked = true;
    tests[i].fn();
    if (cur_failed)
        print_kept();
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
       one per CPU); -j 1 runs them one after another in this process. --slow
       picks the slow tests alone and runs them; --list prints the names of
       the tests picked, and runs none. */
    static const char usage[] = "usage: loony_tests [-j N] [--slow] [--list] [substring | =name]...\n";
    long jobs = sysconf(_SC_NPROCESSORS_ONLN);
    bool list = false;
    for (int a = 1; a < argc; a++) {
        if (strncmp(argv[a], "-j", 2) == 0) {
            const char *n = argv[a][2] ? argv[a] + 2 : a + 1 < argc ? argv[++a] : "";
            char *end;
            jobs = strtol(n, &end, 10);
            if (!*n || *end || jobs < 1) {
                fputs(usage, stderr);
                return 2;
            }
        } else if (strcmp(argv[a], "--slow") == 0) {
            slow_only = true;
        } else if (strcmp(argv[a], "--list") == 0) {
            list = true;
        } else if (argv[a][0] == '-') {
            fputs(usage, stderr);
            return 2;
        } else if (nfilters == MAX_FILTERS) {
            fprintf(stderr, "loony_tests: at most %d filters\n", MAX_FILTERS);
            return 2;
        } else {
            filters[nfilters++] = argv[a];
        }
    }
    if (jobs < 1)
        jobs = 1;
    if (list) {
        for (int i = 0; i < ntests; i++)
            if (selected(i))
                printf("%s\n", tests[i].name);
        return 0;
    }
    if (slow_only)
        setenv("LOONY_SLOW_TESTS", "1", 1);
    /* Nor touch the real preferences in ~/Library/Application Support. */
    char data_dir[1024];
    test_tmp_dir(data_dir, sizeof data_dir);
    setenv("LOONY_DATA_DIR", data_dir, 1);
    int passed = 0, failed = 0, skipped = 0;
    const char *failures[MAX_TESTS];
    if (jobs == 1) {
        for (int i = 0; i < ntests; i++) {
            if (!selected(i))
                continue;
            cur_failed = cur_skipped = false;
            forget_kept();
            fprintf(stderr, "%s\n", tests[i].name);
            tests[i].fn();
            if (cur_failed) {
                print_kept();
                failures[failed++] = tests[i].name;
            }
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
                if (!selected(i))
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

/* The runner's choice of tests, from the names `--list` prints in a
   loony_tests of its own, its stdout sent to the stderr test_run_child
   reads: args ends in NULL. */
static void exec_list(void *args) {
    char self[PATH_MAX];
    uint32_t size = sizeof self;
    char *argv[MAX_FILTERS + 4] = {"loony_tests", "--list"};
    int n = 2;
    for (char *const *a = args; *a && n < MAX_FILTERS + 3; a++)
        argv[n++] = *a;
    argv[n] = NULL;
    dup2(STDERR_FILENO, STDOUT_FILENO);
    if (_NSGetExecutablePath(self, &size) == 0)
        execv(self, argv);
    _exit(127);
}

/* How many lines of out are name, exactly. */
static int lines_named(const char *out, const char *name) {
    int n = 0;
    size_t len = strlen(name);
    for (const char *p = out; *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : p + strlen(p))
        n += strncmp(p, name, len) == 0 && (p[len] == '\n' || p[len] == '\0');
    return n;
}

/* Whether out lists exactly the tests want picks, each once. */
static bool lists_exactly(const char *out, bool (*want)(int)) {
    int lines = 0;
    for (const char *p = out; (p = strchr(p, '\n')); p++)
        lines++;
    int wanted = 0;
    for (int i = 0; i < ntests; i++) {
        int n = lines_named(out, tests[i].name);
        if (n != (want(i) ? 1 : 0))
            return false;
        wanted += n;
    }
    return lines == wanted;
}

static bool is_slow(int i) { return tests[i].slow; }

static bool is_slow_three_minutes_or_monster_fair(int i) {
    return tests[i].slow && (strstr(tests[i].name, "three_minutes") || strstr(tests[i].name, "monster_fair"));
}

static bool is_three_minutes_or_recorded(int i) {
    return strstr(tests[i].name, "three_minutes") || strstr(tests[i].name, "recorded");
}

TEST(tests_slow_picks_the_slow_tests_and_filters_add_up) {
    static char out[65536];
    CHECK_EQ(test_run_child(exec_list, (char *[]){"--slow", NULL}, out, sizeof out), 0);
    CHECK(lists_exactly(out, is_slow));
    CHECK_EQ(lines_named(out, "run_three_minutes_of_play_match_the_recording"), 1);
    CHECK_EQ(lines_named(out, "run_monster_fair_starts_a_game"), 0);
    /* A test two filters match runs once. */
    CHECK_EQ(test_run_child(exec_list, (char *[]){"--slow", "three_minutes", "monster_fair", NULL}, out,
                            sizeof out), 0);
    CHECK(lists_exactly(out, is_slow_three_minutes_or_monster_fair));
    CHECK_EQ(lines_named(out, "run_three_minutes_of_monster_fair_match_the_recording"), 1);
    CHECK_EQ(lines_named(out, "run_monster_fair_keeps_a_license_with_the_recheck_skipped"), 1);
    /* Without --slow, every test any filter matches, not the last filter's. */
    CHECK_EQ(test_run_child(exec_list, (char *[]){"three_minutes", "recorded", NULL}, out, sizeof out), 0);
    CHECK(lists_exactly(out, is_three_minutes_or_recorded));
    CHECK_EQ(test_run_child(exec_list, (char *[]){"--bogus", NULL}, out, sizeof out), 2);
    static char *many[MAX_FILTERS + 2];
    for (int f = 0; f <= MAX_FILTERS; f++)
        many[f] = "x";
    CHECK_EQ(test_run_child(exec_list, many, out, sizeof out), 2);
    CHECK_CONTAINS(out, "at most 64 filters");
}

/* A slow test the runner is asked for by name still skips without
   LOONY_SLOW_TESTS. */
static void exec_one_fast(void *name) {
    unsetenv("LOONY_SLOW_TESTS");
    exec_self(name);
}

TEST(tests_a_slow_test_skips_unless_slow_tests_are_on) {
    static char out[65536];
    CHECK_EQ(test_run_child(exec_one_fast, "=run_three_minutes_of_play_match_the_recording", out, sizeof out),
             0);
    CHECK_CONTAINS(out, "skipped: slow; set LOONY_SLOW_TESTS=1 or pass --slow to run");
    CHECK_CONTAINS(out, "0 passed, 0 failed, 1 skipped");
}
