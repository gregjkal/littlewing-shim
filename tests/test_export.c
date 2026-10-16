#include "test.h"

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "export.h"

TEST(export_line_reads_each_kind) {
    export_progress p = {0};
    CHECK(export_line(&p, "seek 120 600"));
    CHECK_EQ(p.phase, EXPORT_SEEKING);
    CHECK_EQ(p.tick, 120);
    CHECK_EQ(p.target, 600);
    CHECK(export_line(&p, "clip 2 3 30 60"));
    CHECK_EQ(p.phase, EXPORT_CLIPPING);
    CHECK_EQ(p.k, 2);
    CHECK_EQ(p.n, 3);
    CHECK_EQ(p.frame, 30);
    CHECK_EQ(p.frames, 60);
    CHECK(export_line(&p, "wrote /tmp/a b.mp4"));
    CHECK(export_line(&p, "skipped /tmp/c.mp4"));
    CHECK(export_line(&p, "error the replay has no marks"));
    CHECK_EQ(p.nwrote, 1);
    CHECK_STR(p.wrote[0], "/tmp/a b.mp4");
    CHECK_EQ(p.nskipped, 1);
    CHECK_STR(p.skipped[0], "/tmp/c.mp4");
    CHECK_STR(p.error, "the replay has no marks");
    export_progress_free(&p);
    CHECK_EQ(p.nwrote, 0);
}

TEST(export_line_refuses_what_isnt_its) {
    static const char *const junk[] = {
        "", "hello", "seek", "seek 1", "seek 1 2 3", "seek -1 2", "seek  1 2", "seekx 1 2",
        "seek 1 99999999999", "clip 1 2 3", "clip 0 2 0 60", "clip 3 2 0 60", "clip 1 2 61 60",
        "clip 1 2 0 60x", "wrote ", "skipped ", "wrote", "Wrote /a",
    };
    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++) {
        export_progress p = {0};
        CHECK(!export_line(&p, junk[i]));
        CHECK_EQ(p.phase, EXPORT_STARTING);
        CHECK_EQ(p.nwrote + p.nskipped, 0);
    }
}

TEST(export_feed_joins_lines_split_across_reads) {
    export_progress p = {0};
    CHECK_EQ(export_feed(&p, "seek 1", 6), 0);
    CHECK_EQ(p.phase, EXPORT_STARTING); /* not ended yet */
    CHECK_EQ(export_feed(&p, "0 20\nclip 1 1 0 6", 17), 0);
    CHECK_EQ(p.tick, 10);
    CHECK_EQ(p.target, 20);
    CHECK_EQ(p.phase, EXPORT_SEEKING);
    CHECK_EQ(export_feed(&p, "0\nnonsense\nwrote /x.mp4\n", 25), 1);
    CHECK_EQ(p.frames, 60);
    CHECK_EQ(p.nwrote, 1);
    CHECK_STR(p.wrote[0], "/x.mp4");
    /* A line longer than it keeps is dropped whole, and the next is read. */
    char big[3000];
    memset(big, 'a', sizeof big);
    memcpy(big, "wrote /", 7);
    CHECK_EQ(export_feed(&p, big, sizeof big), 0);
    CHECK_EQ(export_feed(&p, "\nseek 5 10\n", 11), 1);
    CHECK_EQ(p.nwrote, 1);
    CHECK_EQ(p.tick, 5);
    export_progress_free(&p);
}

TEST(export_title_says_how_far_it_has_got) {
    export_progress p = {0};
    char t[128];
    export_title(&p, t, sizeof t);
    CHECK_STR(t, ", exporting");
    export_feed(&p, "seek 150 600\n", 13);
    export_title(&p, t, sizeof t);
    CHECK_STR(t, ", exporting, finding clip, 25%");
    export_feed(&p, "clip 1 3 0 60\n", 14);
    export_title(&p, t, sizeof t);
    CHECK_STR(t, ", exporting clip 1 of 3, 0%");
    export_feed(&p, "clip 1 3 24 60\n", 15);
    export_title(&p, t, sizeof t);
    CHECK_STR(t, ", exporting clip 1 of 3, 40%");
    export_feed(&p, "clip 1 3 60 60\nwrote /a.mp4\nseek 900 1000\n", 42);
    export_title(&p, t, sizeof t);
    CHECK_STR(t, ", exporting, finding clip 2 of 3, 90%");
    export_feed(&p, "seek 1200 1000\n", 15); /* never past 100% */
    export_title(&p, t, sizeof t);
    CHECK_STR(t, ", exporting, finding clip 2 of 3, 100%");
    export_progress_free(&p);
}

/* The value of name in env, "(none)" or "(twice)". */
static const char *entry(char **env, const char *name) {
    size_t n = strlen(name);
    const char *found = NULL;
    int count = 0;
    for (size_t i = 0; env[i]; i++)
        if (strncmp(env[i], name, n) == 0 && env[i][n] == '=') {
            found = env[i] + n + 1;
            count++;
        }
    return count == 1 ? found : count ? "(twice)" : "(none)";
}

TEST(export_child_env_drops_the_viewers_settings) {
    char *const env[] = {"PATH=/usr/bin",
                         "LOONY_WATCH=1",
                         "LOONY_WATCH_FROM=600",
                         "LOONY_WATCH_SPEED=2",
                         "LOONY_WATCH_PAUSED=1",
                         "LOONY_WATCH_IN=60",
                         "LOONY_WATCH_EXPORT=1 2 0 0",
                         "LOONY_WATCH_EXPORT_SAID=wrote /a",
                         "LOONY_SCRIPT=/tmp/keys",
                         "LOONY_SCREENSHOT=/tmp/shot.png",
                         "LOONY_FULLSCREEN=1",
                         "LOONY_DATA_DIR=/tmp/lwreplay-x",
                         "LOONY_CLIP_OUT=/tmp/elsewhere.mp4",
                         "LOONY_CLIP=0:01-0:02",
                         "LOONY_REPLAY=game.lwreplay",
                         "LOONY_REPLAY_LICENSE=/prefs.plist",
                         "LOONY_HD=/art",
                         "LOONY_HD_SCALE=3",
                         "LOONY_CLIP_QUALITY=0.5",
                         "LOONY_WATCHFUL=kept",
                         NULL};
    char **out = export_child_env(env, EXPORT_MARKS, "/r/game.lwreplay");
    CHECK(out != NULL);
    if (!out)
        return;
    static const char *const gone[] = {"LOONY_WATCH",      "LOONY_WATCH_FROM",   "LOONY_WATCH_SPEED",
                                       "LOONY_WATCH_PAUSED", "LOONY_WATCH_IN",   "LOONY_WATCH_EXPORT",
                                       "LOONY_WATCH_EXPORT_SAID", "LOONY_SCRIPT", "LOONY_SCREENSHOT",
                                       "LOONY_FULLSCREEN", "LOONY_DATA_DIR",     "LOONY_CLIP_OUT"};
    for (size_t i = 0; i < sizeof gone / sizeof gone[0]; i++)
        CHECK_STR(entry(out, gone[i]), "(none)");
    CHECK_STR(entry(out, "PATH"), "/usr/bin");
    CHECK_STR(entry(out, "LOONY_REPLAY_LICENSE"), "/prefs.plist");
    CHECK_STR(entry(out, "LOONY_HD"), "/art");
    CHECK_STR(entry(out, "LOONY_HD_SCALE"), "3");
    CHECK_STR(entry(out, "LOONY_CLIP_QUALITY"), "0.5");
    CHECK_STR(entry(out, "LOONY_WATCHFUL"), "kept");
    CHECK_STR(entry(out, "LOONY_CLIP"), "marks");
    CHECK_STR(entry(out, "LOONY_CLIP_PROGRESS"), "1");
    CHECK_STR(entry(out, "LOONY_REPLAY"), "/r/game.lwreplay");
    CHECK_STR(entry(out, "LOONY_LOG_INHERITED"), "1");
    export_free_env(out);
    out = export_child_env(env, EXPORT_CLIPS, "/r/game.lwreplay");
    CHECK(out != NULL);
    if (out)
        CHECK_STR(entry(out, "LOONY_CLIP"), "clips");
    export_free_env(out);
}

/* A stand-in for the export: a shell script with this body, in a new
   folder (path gets the script). */
static void fake_export(const char *body, char *dir, size_t dcap, char *path, size_t pcap) {
    test_tmp_dir(dir, dcap);
    snprintf(path, pcap, "%s/export.sh", dir);
    FILE *f = fopen(path, "w");
    fprintf(f, "#!/bin/sh\n%s\n", body);
    fclose(f);
    chmod(path, 0755);
}

/* Polls until the export ends, for up to 10 s: its status, or -1. */
static int wait_for_export(void) {
    struct timespec gap = {0, 10 * 1000000};
    int status;
    for (int i = 0; i < 1000; i++) {
        if (export_poll(&status))
            return status;
        nanosleep(&gap, NULL);
    }
    return -1;
}

TEST(export_runs_a_program_and_follows_what_it_says) {
    char dir[1024], path[1100], err[256] = "";
    fake_export("echo 'clip 1 1 0 60'; echo 'clip 1 1 60 60'; echo \"wrote $LOONY_REPLAY.mp4\"; exit 0",
                dir, sizeof dir, path, sizeof path);
    export_reset();
    CHECK(export_start(path, EXPORT_CLIPS, "/r/game.lwreplay", err, sizeof err));
    CHECK(export_running());
    CHECK(!export_start(path, EXPORT_CLIPS, "/r/game.lwreplay", err, sizeof err)); /* one at a time */
    CHECK_CONTAINS(err, "an export is running");
    int status = wait_for_export();
    CHECK_EQ(status, 0);
    CHECK(!export_running());
    CHECK(!export_cancelled());
    CHECK_EQ(export_now()->nwrote, 1);
    if (export_now()->nwrote)
        CHECK_STR(export_now()->wrote[0], "/r/game.lwreplay.mp4");
    CHECK_EQ(export_now()->frames, 60);
    export_reset();
    test_remove_tree(dir);
}

TEST(export_cancel_stops_it) {
    char dir[1024], path[1100], err[256] = "";
    fake_export("echo 'seek 1 100'; exec sleep 30", dir, sizeof dir, path, sizeof path);
    export_reset();
    CHECK(export_start(path, EXPORT_CLIPS, "/r/game.lwreplay", err, sizeof err));
    export_cancel();
    CHECK(export_cancelled());
    CHECK_EQ(wait_for_export(), 128 + SIGTERM);
    export_reset();
    CHECK(!export_stop(0.1, NULL)); /* none running */
    test_remove_tree(dir);
}

/* What the viewer passes on through its restart, it takes up again: the
   same process, its pipe, and what it said. */
TEST(export_goes_on_through_a_restart) {
    char dir[1024], path[1100], err[256] = "";
    fake_export("echo 'skipped /a.mp4'; echo 'clip 1 2 60 60'; echo 'wrote /b.mp4'; echo 'seek 5 10'\n"
                "sleep 5; echo 'clip 2 2 60 60'; echo 'wrote /c.mp4'",
                dir, sizeof dir, path, sizeof path);
    export_reset();
    CHECK(export_start(path, EXPORT_MARKS, "/r/game.lwreplay", err, sizeof err));
    int status = -1;
    struct timespec gap = {0, 10 * 1000000};
    for (int i = 0; i < 500 && export_now()->phase != EXPORT_SEEKING; i++) {
        export_poll(&status);
        nanosleep(&gap, NULL);
    }
    CHECK_EQ(export_now()->phase, EXPORT_SEEKING);
    export_pass_on();
    const char *v = getenv("LOONY_WATCH_EXPORT");
    int pid = 0, fd = -1, kind = -1, cancelled = -1;
    CHECK(v && sscanf(v, "%d %d %d %d", &pid, &fd, &kind, &cancelled) == 4);
    CHECK_EQ(kind, EXPORT_MARKS);
    CHECK_EQ(cancelled, 0);
    CHECK(fd >= 0 && !(fcntl(fd, F_GETFD) & FD_CLOEXEC)); /* outlives the exec */
    /* As the exec would: forgets it, but keeps the pipe. */
    int saved = dup(fd);
    export_reset();
    dup2(saved, fd);
    close(saved);
    CHECK(export_take_up(err, sizeof err));
    CHECK(getenv("LOONY_WATCH_EXPORT") == NULL);
    CHECK(getenv("LOONY_WATCH_EXPORT_SAID") == NULL);
    CHECK(export_running());
    CHECK_EQ(export_running_kind(), EXPORT_MARKS);
    CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    char t[128];
    export_title(export_now(), t, sizeof t);
    CHECK_STR(t, ", exporting, finding clip 2 of 2, 50%");
    /* It hasn't read its stdin (the tests' own): end it the other way. */
    export_cancel();
    CHECK_EQ(wait_for_export(), 128 + SIGTERM);
    CHECK_EQ(export_now()->nwrote, 1);
    CHECK_EQ(export_now()->nskipped, 1);
    export_reset();
    /* Wrong, or not a child: refused, and cleared. */
    setenv("LOONY_WATCH_EXPORT", "nonsense", 1);
    CHECK(!export_take_up(err, sizeof err));
    CHECK(getenv("LOONY_WATCH_EXPORT") == NULL);
    setenv("LOONY_WATCH_EXPORT", "1 -1 0 0", 1); /* launchd */
    CHECK(!export_take_up(err, sizeof err));
    CHECK(!export_running());
    CHECK(export_take_up(err, sizeof err)); /* unset: nothing to take up */
    test_remove_tree(dir);
}
