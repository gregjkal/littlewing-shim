#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "recordings.h"
#include "replay.h"
#include "util.h"

/* The variables recordings_dir and recordings_watch_env read or set, as
   they were before the test. */
static const char *const env_names[] = {
    "HOME",       "LOONY_DATA_DIR",   "LOONY_REPLAY",      "LOONY_WATCH",
    "LOONY_GAME", "LOONY_WATCH_FROM", "LOONY_WATCH_SPEED", "LOONY_RECORD",
};
static char *env_saved[sizeof env_names / sizeof env_names[0]];
static char dir[1024], recs[1100];

static int begin_test(void) {
    for (size_t i = 0; i < sizeof env_names / sizeof env_names[0]; i++) {
        const char *v = getenv(env_names[i]);
        env_saved[i] = v ? strdup(v) : NULL;
    }
    test_tmp_dir(dir, sizeof dir);
    snprintf(recs, sizeof recs, "%s/recordings", dir);
    mkdir(recs, 0755);
    return 0;
}

static void end_test(int *unused) {
    (void)unused;
    recordings_forget();
    char received[1100];
    snprintf(received, sizeof received, "%s/received", dir);
    chmod(received, 0755); /* made read-only by receive */
    for (size_t i = 0; i < sizeof env_names / sizeof env_names[0]; i++) {
        if (env_saved[i])
            setenv(env_names[i], env_saved[i], 1);
        else
            unsetenv(env_names[i]);
        free(env_saved[i]);
        env_saved[i] = NULL;
    }
    test_remove_tree(dir);
}

#define BEGIN_TEST() __attribute__((cleanup(end_test))) int test_guard_ = begin_test()

/* A recording's bytes, built up: a format 5 header, then records. */
typedef struct {
    uint8_t b[4096];
    size_t n;
} bytes;

static void varint(bytes *b, uint64_t v) {
    do {
        b->b[b->n++] = (uint8_t)((v & 0x7F) | (v > 0x7F ? 0x80 : 0));
        v >>= 7;
    } while (v);
}

static void header(bytes *b, const char *game_id) {
    b->n = 0;
    memcpy(b->b, "LWREPLAY", 8);
    b->n = 8;
    varint(b, 5);
    varint(b, strlen(game_id));
    memcpy(b->b + b->n, game_id, strlen(game_id));
    b->n += strlen(game_id);
    memset(b->b + b->n, 0, 32); /* the SHA-256 */
    b->n += 32;
    varint(b, 0);          /* no flags */
    varint(b, 1760000000); /* the launch */
    varint(b, 3600);       /* an hour east */
    varint(b, 0);          /* no preferences */
}

enum { END = 0, CLOCK = 1, MARK = 11, PLAY = 13 };

static void clock_ms(bytes *b, uint32_t ms) {
    varint(b, 1);
    varint(b, CLOCK);
    varint(b, ms);
}

static void mark(bytes *b) {
    varint(b, 1);
    varint(b, MARK);
}

static void play(bytes *b, bool on) {
    varint(b, 1);
    varint(b, PLAY);
    varint(b, on);
}

static void end(bytes *b) {
    varint(b, 1);
    varint(b, END);
}

/* Writes b to recs/name, compressed, or as it is (zlib reads either). */
static void write_file(const char *name, const bytes *b, bool compressed) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", recs, name);
    if (compressed) {
        gzFile f = gzopen(path, "wb");
        gzwrite(f, b->b, (unsigned)b->n);
        gzclose(f);
    } else {
        FILE *f = fopen(path, "wb");
        fwrite(b->b, 1, b->n, f);
        fclose(f);
    }
}

static void touch(const char *name) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", recs, name);
    FILE *f = fopen(path, "w");
    if (f)
        fclose(f);
}

static time_t local_time(int year, int mon, int day, int hour, int min, int sec) {
    struct tm tm = {.tm_year = year - 1900, .tm_mon = mon - 1, .tm_mday = day,
                    .tm_hour = hour, .tm_min = min, .tm_sec = sec, .tm_isdst = -1};
    return mktime(&tm);
}

/* Four recordings, a damaged file and a file that isn't a recording. */
static void write_four(void) {
    bytes b;
    /* Play from 1 s to 6 s, marked at 3 s: 2 s into play. */
    header(&b, "loony-labyrinth");
    clock_ms(&b, 1000);
    play(&b, true);
    clock_ms(&b, 2000);
    mark(&b);
    clock_ms(&b, 3000);
    play(&b, false);
    clock_ms(&b, 5000);
    end(&b);
    write_file("loony-labyrinth-2026-10-15-201500.lwreplay", &b, true);
    /* The second game in that second, ending in play (it crashed, or is
       still being recorded), marked a minute in. */
    header(&b, "loony-labyrinth");
    play(&b, true);
    clock_ms(&b, 60000);
    mark(&b);
    clock_ms(&b, 30000);
    write_file("loony-labyrinth-2026-10-15-201500-2-marked.lwreplay", &b, true);
    /* No play records: all of it is play. */
    header(&b, "loony-labyrinth");
    clock_ms(&b, 2000);
    mark(&b);
    clock_ms(&b, 500);
    end(&b);
    write_file("loony-labyrinth-2026-10-15-201459.lwreplay", &b, true);
    /* The newest, with clips. */
    header(&b, "loony-labyrinth");
    play(&b, true);
    clock_ms(&b, 1000);
    play(&b, false);
    end(&b);
    write_file("loony-labyrinth-2026-10-16-090000.lwreplay", &b, true);
    touch("loony-labyrinth-2026-10-16-090000.clips");
    bytes junk = {.b = "not a replay", .n = 12};
    write_file("loony-labyrinth-2026-10-16-100000.lwreplay", &junk, true);
    touch("notes.txt");
}

TEST(recordings_lists_newest_first_with_play_lengths_and_marks) {
    BEGIN_TEST();
    write_four();
    const recording *r;
    size_t n = recordings_list(recs, &r);
    CHECK_EQ(n, 4);
    if (n != 4)
        return;
    CHECK_CONTAINS(r[0].path, "/loony-labyrinth-2026-10-16-090000.lwreplay");
    CHECK_CONTAINS(r[1].path, "/loony-labyrinth-2026-10-15-201500-2-marked.lwreplay");
    CHECK_CONTAINS(r[2].path, "/loony-labyrinth-2026-10-15-201500.lwreplay");
    CHECK_CONTAINS(r[3].path, "/loony-labyrinth-2026-10-15-201459.lwreplay");
    CHECK_EQ(r[0].start, local_time(2026, 10, 16, 9, 0, 0));
    CHECK_EQ(r[2].start, local_time(2026, 10, 15, 20, 15, 0));
    CHECK_EQ(r[1].second, 2);
    CHECK_STR(r[0].game_id, "loony-labyrinth");
    CHECK_EQ(r[0].length, 60);
    CHECK_EQ(r[0].nmarks, 0);
    CHECK(r[0].clips);
    CHECK(!r[0].marked);
    CHECK_EQ(r[1].length, 90 * 60); /* to the last record */
    CHECK_EQ(r[1].nmarks, 1);
    CHECK_EQ(r[1].marks[0], 60 * 60);
    CHECK(r[1].marked);
    CHECK(!r[1].clips);
    CHECK_EQ(r[2].length, 5 * 60);
    CHECK_EQ(r[2].nmarks, 1);
    CHECK_EQ(r[2].marks[0], 2 * 60);
    CHECK_EQ(r[3].length, 150);
    CHECK_EQ(r[3].marks[0], 120);
    char missing[1200];
    snprintf(missing, sizeof missing, "%s/none", dir);
    CHECK_EQ(recordings_list(missing, &r), 0);
}

/* A name without the time in it starts when the header says. */
TEST(recordings_without_a_time_in_the_name_start_at_the_launch) {
    BEGIN_TEST();
    bytes b;
    header(&b, "loony-labyrinth");
    end(&b);
    write_file("game.lwreplay", &b, true);
    const recording *r;
    CHECK_EQ(recordings_list(recs, &r), 1);
    CHECK_EQ(r[0].start, 1760000000);
    CHECK_EQ(r[0].length, 0);
}

static void list_twice(void *unused) {
    (void)unused;
    const recording *r;
    recordings_list(recs, &r);
    recordings_list(recs, &r);
}

/* A damaged file is logged once, not each time the menu opens. */
TEST(recordings_logs_a_damaged_file_once) {
    BEGIN_TEST();
    write_four();
    char out[4096];
    CHECK_EQ(test_run_child(list_twice, NULL, out, sizeof out), 0);
    const char *first = strstr(out, "recordings: leaving out ");
    CHECK(first);
    CHECK(first && !strstr(first + 1, "recordings: leaving out "));
    CHECK_CONTAINS(out, "loony-labyrinth-2026-10-16-100000.lwreplay: ");
}

/* Sets the file's modification time to t. */
static void set_mtime(const char *name, time_t t) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", recs, name);
    struct timespec ts[2] = {{t, 0}, {t, 0}};
    CHECK_EQ(utimensat(AT_FDCWD, path, ts, 0), 0);
}

/* What was read is kept while the file's size and modification time stay;
   a change to either reads it again. */
TEST(recordings_reads_a_file_again_when_it_changes) {
    BEGIN_TEST();
    static const char name[] = "loony-labyrinth-2026-10-15-201500.lwreplay";
    bytes b;
    header(&b, "loony-labyrinth");
    clock_ms(&b, 1000); /* two bytes of ms, as 2000 is */
    mark(&b);
    write_file(name, &b, false);
    set_mtime(name, 1760000000);
    const recording *r;
    CHECK_EQ(recordings_list(recs, &r), 1);
    CHECK_EQ(r[0].marks[0], 60);
    /* The same size and time: not read again. */
    header(&b, "loony-labyrinth");
    clock_ms(&b, 2000);
    mark(&b);
    write_file(name, &b, false);
    set_mtime(name, 1760000000);
    CHECK_EQ(recordings_list(recs, &r), 1);
    CHECK_EQ(r[0].marks[0], 60);
    set_mtime(name, 1760000001);
    CHECK_EQ(recordings_list(recs, &r), 1);
    CHECK_EQ(r[0].marks[0], 120);
    /* Recorded on: it grows. */
    clock_ms(&b, 1000);
    mark(&b);
    write_file(name, &b, false);
    set_mtime(name, 1760000001);
    CHECK_EQ(recordings_list(recs, &r), 1);
    CHECK_EQ(r[0].nmarks, 2);
    CHECK_EQ(r[0].marks[1], 180);
}

TEST(recordings_dir_is_under_home_while_a_replay_plays) {
    BEGIN_TEST();
    char out[1200];
    unsetenv("LOONY_REPLAY");
    setenv("HOME", "/Users/me", 1);
    setenv("LOONY_DATA_DIR", "/data", 1);
    CHECK(recordings_dir(out, sizeof out));
    CHECK_STR(out, "/data/recordings");
    unsetenv("LOONY_DATA_DIR");
    CHECK(recordings_dir(out, sizeof out));
    CHECK_STR(out, "/Users/me/Library/Application Support/littlewing-shim/recordings");
    /* The viewer's data folder is the replay's own. */
    setenv("LOONY_REPLAY", "/x/game.lwreplay", 1);
    setenv("LOONY_DATA_DIR", "/tmp/lwreplay-abc123", 1);
    CHECK(recordings_dir(out, sizeof out));
    CHECK_STR(out, "/Users/me/Library/Application Support/littlewing-shim/recordings");
    unsetenv("HOME");
    CHECK(!recordings_dir(out, sizeof out));
    unsetenv("LOONY_REPLAY");
    unsetenv("LOONY_DATA_DIR");
    CHECK(!recordings_dir(out, sizeof out));
}

TEST(recordings_labels_say_when_how_long_and_what_is_in_it) {
    BEGIN_TEST();
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int year = tm.tm_year + 1900;
    char buf[256];
    uint32_t marks[2] = {0, 0};
    recording r = {.game_id = "loony-labyrinth", .length = (14 * 60 + 32) * 60 + 59};
    r.start = local_time(year, 10, 15, 20, 15, 30);
    CHECK_STR(recordings_label(&r, false, buf, sizeof buf), "Oct 15, 8:15 PM, 14:32");
    r.marks = marks;
    r.nmarks = 2;
    r.clips = true;
    CHECK_STR(recordings_label(&r, false, buf, sizeof buf), "Oct 15, 8:15 PM, 14:32, 2 marks, clips");
    CHECK_STR(recordings_label(&r, true, buf, sizeof buf),
              "Loony Labyrinth, Oct 15, 8:15 PM, 14:32, 2 marks, clips");
    r.nmarks = 1;
    r.clips = false;
    r.start = local_time(2020, 3, 3, 0, 5, 0);
    r.length = 3723 * 60;
    CHECK_STR(recordings_label(&r, false, buf, sizeof buf), "Mar 3, 2020, 12:05 AM, 1:02:03, 1 mark");
    r.start = local_time(year, 1, 1, 12, 0, 0);
    r.length = 30;
    r.nmarks = 0;
    CHECK_STR(recordings_label(&r, false, buf, sizeof buf), "Jan 1, 12:00 PM, 0:00");
    snprintf(r.game_id, sizeof r.game_id, "no-such-game");
    CHECK_STR(recordings_label(&r, true, buf, sizeof buf), "no-such-game, Jan 1, 12:00 PM, 0:00");
    CHECK_STR(recordings_mark_label((3 * 60 + 12) * 60 + 30, buf, sizeof buf), "Play from 3:12");
    CHECK_STR(recordings_mark_label(0, buf, sizeof buf), "Play from 0:00");
}

/* A mark gets an item unless it would repeat the one above it: the
   recording's own in the first second, the mark before's in its second. */
TEST(recordings_mark_shown_skips_items_that_repeat_the_one_above) {
    uint32_t marks[] = {30, 90, 100, 119, 120, 3600};
    recording r;
    memset(&r, 0, sizeof r);
    r.marks = marks;
    r.nmarks = sizeof marks / sizeof marks[0];
    CHECK(!recordings_mark_shown(&r, 0)); /* 0:00, like the recording's item */
    CHECK(recordings_mark_shown(&r, 1));  /* 0:01 */
    CHECK(!recordings_mark_shown(&r, 2)); /* 0:01 again */
    CHECK(!recordings_mark_shown(&r, 3));
    CHECK(recordings_mark_shown(&r, 4)); /* 0:02 */
    CHECK(recordings_mark_shown(&r, 5)); /* 1:00 */
}

TEST(recordings_watch_env_starts_the_viewer_at_a_mark) {
    BEGIN_TEST();
    setenv("LOONY_GAME", "loony-labyrinth", 1);
    setenv("LOONY_DATA_DIR", "/tmp/lwreplay-abc123", 1);
    setenv("LOONY_WATCH_SPEED", "2", 1);
    setenv("LOONY_RECORD", "0", 1);
    recordings_watch_env("/x/game.lwreplay", 3600);
    CHECK_STR(getenv("LOONY_REPLAY"), "/x/game.lwreplay");
    CHECK_STR(getenv("LOONY_WATCH"), "1");
    CHECK_STR(getenv("LOONY_WATCH_FROM"), "3600");
    CHECK(!getenv("LOONY_GAME"));
    CHECK(!getenv("LOONY_DATA_DIR"));
    CHECK(!getenv("LOONY_WATCH_SPEED"));
    CHECK(!getenv("LOONY_RECORD"));
    recordings_watch_env("/x/other.lwreplay", 0);
    CHECK_STR(getenv("LOONY_REPLAY"), "/x/other.lwreplay");
    CHECK(!getenv("LOONY_WATCH_FROM"));
}

/* recordings_delete's trash, standing in for the Finder's: moves path into
   dir/Trash, or refuses when trash_refuses. */
static int trashed;
static bool trash_refuses;
static bool fake_trash(const char *path, char *err, size_t errlen) {
    if (trash_refuses) {
        snprintf(err, errlen, "the Trash refused");
        return false;
    }
    char to[1200];
    snprintf(to, sizeof to, "%s/Trash", dir);
    mkdir(to, 0755);
    snprintf(to, sizeof to, "%s/Trash/%s", dir, strrchr(path, '/') + 1);
    if (rename(path, to) != 0) {
        snprintf(err, errlen, "can't move it");
        return false;
    }
    trashed++;
    return true;
}

static bool in(const char *folder, const char *name) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", folder, name);
    return access(path, F_OK) == 0;
}

TEST(recordings_delete_moves_a_recording_and_its_clips_to_the_trash) {
    BEGIN_TEST();
    write_four();
    touch("loony-labyrinth-2026-10-16-090000 clip 1.mp4"); /* exported from it */
    const recording *r;
    CHECK_EQ(recordings_list(recs, &r), 4);
    char path[1200], err[256], trash[1100];
    snprintf(path, sizeof path, "%s", r[0].path);
    CHECK(recordings_deletable(path));
    trashed = 0;
    trash_refuses = false;
    CHECK(recordings_delete(path, fake_trash, err, sizeof err));
    CHECK_EQ(trashed, 2);
    snprintf(trash, sizeof trash, "%s/Trash", dir);
    CHECK(in(trash, "loony-labyrinth-2026-10-16-090000.lwreplay"));
    CHECK(in(trash, "loony-labyrinth-2026-10-16-090000.clips"));
    CHECK(!in(recs, "loony-labyrinth-2026-10-16-090000.lwreplay"));
    CHECK(in(recs, "loony-labyrinth-2026-10-16-090000 clip 1.mp4"));
    CHECK_EQ(recordings_list(recs, &r), 3);
    CHECK_CONTAINS(r[0].path, "/loony-labyrinth-2026-10-15-201500-2-marked.lwreplay");
    /* One without a list of clips moves alone. */
    snprintf(path, sizeof path, "%s", r[0].path);
    CHECK(recordings_delete(path, fake_trash, err, sizeof err));
    CHECK_EQ(trashed, 3);
    CHECK_EQ(recordings_list(recs, &r), 2);
}

TEST(recordings_delete_keeps_a_recording_the_trash_refuses) {
    BEGIN_TEST();
    write_four();
    const recording *r;
    CHECK_EQ(recordings_list(recs, &r), 4);
    char path[1200], err[256] = "";
    snprintf(path, sizeof path, "%s", r[0].path);
    trashed = 0;
    trash_refuses = true;
    CHECK(!recordings_delete(path, fake_trash, err, sizeof err));
    trash_refuses = false;
    CHECK_STR(err, "the Trash refused");
    CHECK(in(recs, "loony-labyrinth-2026-10-16-090000.lwreplay"));
    CHECK(in(recs, "loony-labyrinth-2026-10-16-090000.clips"));
    CHECK_EQ(recordings_list(recs, &r), 4);
}

TEST(recordings_delete_refuses_the_recording_being_made) {
    BEGIN_TEST();
    write_four();
    char live[1200], err[256] = "";
    snprintf(live, sizeof live, "%s/live.lwreplay", recs);
    unsetenv("LOONY_REPLAY");
    setenv("LOONY_RECORD", live, 1);
    static const uint8_t exe[] = {1, 2, 3, 4};
    replay_reset();
    CHECK(replay_start("loony-labyrinth", exe, sizeof exe, dir, err, sizeof err));
    CHECK(replay_recording_path());
    CHECK(!recordings_deletable(live));
    trashed = 0;
    CHECK(!recordings_delete(live, fake_trash, err, sizeof err));
    CHECK_STR(err, "it is being recorded");
    CHECK_EQ(trashed, 0);
    CHECK(in(recs, "live.lwreplay"));
    char other[1200];
    snprintf(other, sizeof other, "%s/loony-labyrinth-2026-10-15-201459.lwreplay", recs);
    CHECK(recordings_deletable(other));
    replay_reset();
}

TEST(recordings_leave_env_starts_the_program_as_if_launched) {
    BEGIN_TEST();
    recordings_watch_env("/x/game.lwreplay", 3600);
    setenv("LOONY_GAME", "loony-labyrinth", 1); /* as replay_open sets them */
    setenv("LOONY_DATA_DIR", "/tmp/lwreplay-abc123", 1);
    recordings_leave_env();
    CHECK(!getenv("LOONY_REPLAY"));
    CHECK(!getenv("LOONY_WATCH"));
    CHECK(!getenv("LOONY_WATCH_FROM"));
    CHECK(!getenv("LOONY_GAME"));
    CHECK(!getenv("LOONY_DATA_DIR"));
}

/* A recording named name in dir/received, then made read-only, as
   Messages' attachments are to this program. */
static void receive(const char *name, uint32_t ms, char *path, size_t cap) {
    char from[1200], received[1100];
    bytes b;
    header(&b, "loony-labyrinth");
    clock_ms(&b, ms);
    end(&b);
    write_file(".staged", &b, true);
    snprintf(received, sizeof received, "%s/received", dir);
    chmod(received, 0755);
    mkdir(received, 0755);
    snprintf(from, sizeof from, "%s/.staged", recs);
    snprintf(path, cap, "%s/%s", received, name);
    rename(from, path);
    chmod(received, 0555);
}

TEST(recordings_bring_in_copies_a_recording_its_folder_wont_take_files_beside) {
    BEGIN_TEST();
    char path[1200], out[1200], err[256], mine[1200], received[1100];
    snprintf(received, sizeof received, "%s/received", dir);
    receive("Loony 1,531,255,830 GREG.lwreplay", 1000, path, sizeof path);
    struct stat a, b;
    struct timespec old[2] = {{.tv_sec = 1760000000}, {.tv_sec = 1760000000}};
    chmod(received, 0755);
    utimensat(AT_FDCWD, path, old, 0);
    chmod(received, 0555);
    CHECK(recordings_bring_in(path, recs, out, sizeof out, err, sizeof err));
    snprintf(mine, sizeof mine, "%s/Loony 1,531,255,830 GREG.lwreplay", recs);
    CHECK_STR(out, mine);
    CHECK(stat(path, &a) == 0 && stat(out, &b) == 0);
    CHECK_EQ(a.st_size, b.st_size);
    CHECK_EQ(b.st_mtimespec.tv_sec, 1760000000); /* which high score it holds */
    CHECK_EQ(b.st_mode & 0777, 0644);
    /* Opened again, the copy is opened, even once it holds more (a snapshot). */
    FILE *f = fopen(out, "ab");
    CHECK(f != NULL);
    fclose(f);
    CHECK(recordings_bring_in(path, recs, out, sizeof out, err, sizeof err));
    CHECK_STR(out, mine);
    /* Another recording of that name gets a name of its own. */
    chmod(received, 0755);
    unlink(path);
    receive("Loony 1,531,255,830 GREG.lwreplay", 2000, path, sizeof path);
    CHECK(recordings_bring_in(path, recs, out, sizeof out, err, sizeof err));
    snprintf(mine, sizeof mine, "%s/Loony 1,531,255,830 GREG 2.lwreplay", recs);
    CHECK_STR(out, mine);
    const recording *r;
    CHECK_EQ(recordings_list(recs, &r), 2);
}

TEST(recordings_bring_in_leaves_a_recording_its_folder_takes_files_beside) {
    BEGIN_TEST();
    char path[1200], out[1200], err[256], other[1100];
    receive("game.lwreplay", 1000, path, sizeof path);
    snprintf(other, sizeof other, "%s/received", dir);
    chmod(other, 0755);
    CHECK(recordings_bring_in(path, recs, out, sizeof out, err, sizeof err));
    CHECK_STR(out, path);
    const recording *r;
    CHECK_EQ(recordings_list(recs, &r), 0);
    /* Nor is anything left behind by the try. */
    char cmd[1300];
    snprintf(cmd, sizeof cmd, "test \"$(ls -A '%s')\" = game.lwreplay", other);
    CHECK_EQ(system(cmd), 0);
}

TEST(recordings_bring_in_refuses_a_file_that_isnt_a_recording) {
    BEGIN_TEST();
    char path[1200], out[1200], err[256], received[1100];
    snprintf(received, sizeof received, "%s/received", dir);
    mkdir(received, 0755);
    snprintf(path, sizeof path, "%s/junk.lwreplay", received);
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    fputs("not a replay", f);
    fclose(f);
    chmod(received, 0555);
    CHECK(!recordings_bring_in(path, recs, out, sizeof out, err, sizeof err));
    const recording *r;
    CHECK_EQ(recordings_list(recs, &r), 0);
}
