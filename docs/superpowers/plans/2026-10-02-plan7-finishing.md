# Plan 7: Finishing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish the project.
- A double-clickable `Loony Labyrinth.app` that keeps working when Homebrew upgrades its libraries.
- An hour of play with no crash and no leak, on the fixed clock and in real time with the audio thread running.
- A recorded headless regression run that pins down the emulation, drawing and sound.

**Architecture:**
- **The app:** `tools/make_app.sh` (the CMake `app` target) wraps the Release binary in a bundle. It copies in the two Homebrew libraries, rewrites their install names to `@rpath`, and signs everything ad hoc with the hardened runtime.
- **Launching from Finder:** `main.c` detects that it's running from a bundle with no terminal. It then writes its log to `~/Library/Logs/loony-shim`, and reports failures in a message box through a hook that `fatal` and `trap_crash` call.
- **Scripts of any length:** `script.c` holds actions in a growable array. `tools/soak_script.py` writes an hour of play.
- **Regression:** a test plays three scripted minutes and compares three frames and the whole recording with recorded hashes.

**Tech Stack:** C (gnu11), clang, CMake ≥ 3.20, Unicorn 2, SDL3, macOS `codesign`, `install_name_tool` and `otool`, Python 3 (the soak script only).

**Spec:** `docs/superpowers/specs/2026-09-30-loony-shim-design.md` (milestone 7, finish; Build and repository, revised in Task 4). Plan 6 (`docs/superpowers/plans/2026-10-02-plan6-dialogs-files-prefs.md`) finished the game's features. The user signed it off on 2026-10-02: "Everything looks and works perfectly. Gameplay is perfect, sound is perfect, no lag, key code worked, high scores survive quitting and relaunch."

## Global Constraints

- Repo: `~/dev/loony-shim`, branch `main`. All paths below are relative to it.
- The game files are read-only inputs. Nothing from them goes into the repo or into the bundle: the app reads the game from `/Applications/Loony Labyrinth` at run time.
- The user's key code and e-mail address never appear anywhere.
- C11 with GNU extensions, `-Wall -Wextra -Werror`. Debug builds add the sanitizers.
- No test opens a window, plays sound, shows a message box (they are skipped under `SDL_VIDEO_DRIVER=dummy`), or touches the real `~/Library`.
- All approved golden frames and recordings stay as they are.

## Facts measured (the tests and the soak assert these)

| Fact | Value |
|---|---|
| Libraries the binary links from Homebrew | `libunicorn.2.dylib` and `libSDL3.0.dylib`, neither of which links anything else outside the system |
| Hardened runtime with ad-hoc signatures | Library validation refuses the bundled libraries ("different Team IDs"), so the app needs `com.apple.security.cs.disable-library-validation` as well as `allow-jit` |
| The bundled, hardened binary | Reproduces the approved menu frame (`0xADE78151`) |
| Fixed-clock soak (`tools/soak_script.py 216000`, Release, about 12,700 actions, a game started every minute) | The full hour in 19.5 minutes, no crash, RSS flat at 117.9 MB after warm-up. At the end the game took longer than the 3-second grace to quit (Task 5) |
| Real-time soak (the signed app, SDL's dummy video and audio drivers, so the audio thread runs, about one wall-clock hour) | 3,601 seconds, no crash, a clean quit at the end, RSS 107-119 MB |
| `leaks` on the soak process, after 15 minutes | 0 leaks. RSS rose from 115 MB to 118 MB in the first 13 minutes, then stayed flat (Unicorn's translation cache filling) |
| Regression run (fixed clock, 10,800 ticks) | Frames `0xAAD1E97F` (minute 1, ball 1), `0x015482C8` (minute 2, ball 3), `0x66E6FBF1` (minute 3, a new game), recording `0x3663C0FE`. The same in Debug and Release. Release takes about 1 minute, Debug with the sanitizers about 4 |

## Decisions this plan makes

- **The libraries are copied into the bundle,** not linked from `/opt/homebrew`, so `brew upgrade` can't break the app. The build fails if any Homebrew path is left.
- **The hardened runtime stays on,** with `allow-jit` (the spec's requirement) and `disable-library-validation` (needed because ad-hoc signatures carry no team ID). Signing without the hardened runtime would also work. This keeps the runtime's other protections.
- **The app mode is recognized by its path,** `.app/Contents/MacOS/` in `argv[0]`, together with stderr not being a terminal. Running the bundled binary from a terminal still logs to the terminal.
- **One log per launch:** `loony.log`, with the run before it kept as `loony.previous.log`.
- **Message boxes are shown for startup failures, `fatal` and `trap_crash` only,** once, and never under the dummy video driver. A crash report still goes to the log in full.
- **A `-psn_…` argument is ignored.** Older macOS passed it to apps launched from Finder.
- **No app icon.** Making one would mean extracting the game's icon, and nothing from the game goes into the repo.
- **The regression run is three game minutes.** That's enough to cover the opening, the menu, a full time-limited game and the start of the next, while keeping the Debug suite under ten minutes.

## Review Focus

1. **The bundle is self-contained.** Expect no `/opt/homebrew` path in any Mach-O file in it, and every library signed before the bundle. Read `tools/make_app.sh`; the test is in Task 2.
2. **Failure reporting can't loop or hang.** The hook runs once, and is skipped headless. Read `util.c` and `main.c`.
3. **The soak is real.** Expect a full hour of game time, no crash, and memory flat after warm-up. The numbers are in Task 5.

---
### Task 1: Scripts of any length

**Files:**
- Modify: `src/script.c`
- Modify: `tests/test_script.c`

**Interfaces:**
- Changes: `script_parse` accepts up to 1,000,000 actions (was 1024).

- [ ] **Step 1: Write the failing tests**

```diff
diff --git a/tests/test_script.c b/tests/test_script.c
index 2d9979c..83162db 100644
--- a/tests/test_script.c
+++ b/tests/test_script.c
@@ -1,6 +1,7 @@
 #include "test.h"
 
 #include <SDL3/SDL_scancode.h>
+#include <stdlib.h>
 
 #include "script.h"
 
@@ -63,3 +64,23 @@ TEST(script_clicks_and_typing) {
     CHECK(!script_parse("5 click 320\n", err, sizeof err));
     CHECK_CONTAINS(err, "line 1: click needs x and y");
 }
+
+/* An hour of scripted play is thousands of actions. */
+TEST(script_holds_long_scripts) {
+    size_t cap = 20000 * 16;
+    char *text = malloc(cap), *p = text;
+    for (int i = 0; i < 20000; i++)
+        p += snprintf(p, cap - (size_t)(p - text), "%d %s z\n", i, i % 2 ? "up" : "down");
+    char err[256] = "";
+    CHECK(script_parse(text, err, sizeof err));
+    free(text);
+    CHECK_EQ(script_remaining(), 20000);
+    script_action a;
+    for (int i = 0; i < 19999; i++)
+        script_next(100000, &a);
+    CHECK(script_next(100000, &a));
+    CHECK_EQ(a.tick, 19999);
+    CHECK_EQ(a.kind, SCRIPT_KEY_UP);
+    CHECK(script_parse("", err, sizeof err));
+    CHECK_EQ(script_remaining(), 0);
+}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build && ./build/loony_tests script_`
Expected: `script_holds_long_scripts` fails: "more than 1024 actions".

- [ ] **Step 3: Implement**

```diff
diff --git a/src/script.c b/src/script.c
index 7c1d772..2470cf5 100644
--- a/src/script.c
+++ b/src/script.c
@@ -7,14 +7,15 @@
 #include "keymap.h"
 #include "util.h"
 
-#define MAX_ACTIONS 1024
+#define MAX_ACTIONS 1000000
 
 static struct {
-    script_action a[MAX_ACTIONS];
-    int n, next;
+    script_action *a;
+    int n, cap, next;
 } SC;
 
 bool script_parse(const char *text, char *err, size_t errlen) {
+    free(SC.a);
     memset(&SC, 0, sizeof SC);
     int line_no = 0;
     uint32_t last = 0;
@@ -51,7 +52,14 @@ bool script_parse(const char *text, char *err, size_t errlen) {
             snprintf(err, errlen, "more than %d actions", MAX_ACTIONS);
             return false;
         }
+        if (SC.n == SC.cap) {
+            SC.cap = SC.cap ? SC.cap * 2 : 256;
+            SC.a = realloc(SC.a, (size_t)SC.cap * sizeof *SC.a);
+            if (!SC.a)
+                fatal("out of memory");
+        }
         script_action *a = &SC.a[SC.n];
+        memset(a, 0, sizeof *a);
         a->tick = (uint32_t)tick;
         if (strcmp(verb, "down") == 0 || strcmp(verb, "up") == 0) {
             a->kind = verb[0] == 'd' ? SCRIPT_KEY_DOWN : SCRIPT_KEY_UP;
```

- [ ] **Step 4: Run the tests**

Run: `cmake -S . -B build && cmake --build build && ./build/loony_tests script_ && ./build/loony_tests`
Expected: all pass (the real-key registration test skips); no sanitizer reports.

- [ ] **Step 5: Commit**

```bash
git add src/script.c tests/test_script.c
git commit -m "Scripts of any length, for hour-long runs"
```

---

### Task 2: The app

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `src/main.c`
- Modify: `src/trap.c`
- Modify: `src/util.c`
- Modify: `src/util.h`
- Modify: `tests/test_run.c`
- Create: `tools/loony.entitlements`
- Create: `tools/make_app.sh`

**Interfaces:**
- Produces: `tools/make_app.sh`, `tools/loony.entitlements`, the CMake target `app`; `util_set_failure_hook`/`util_report_failure` (`util.h`); `LOONY_SRC_DIR` for tests.
- Changes: `fatal` and `trap_crash` call the failure hook; `main` ignores `-psn_` arguments and, as the app, logs to `~/Library/Logs/loony-shim/loony.log`.

- [ ] **Step 1: Write the failing tests**

```diff
diff --git a/tests/test_run.c b/tests/test_run.c
index 8d1ccff..fe92456 100644
--- a/tests/test_run.c
+++ b/tests/test_run.c
@@ -348,3 +348,97 @@ TEST(run_reports_missing_game_folder) {
     CHECK_EQ(status, 1);
     CHECK_CONTAINS(out, "can't read /nonexistent/loony/LOONY LABYRINTH 3.0.1");
 }
+
+/* Launched as the app, the log goes to ~/Library/Logs/loony-shim instead
+   of the (absent) terminal. HOME is a temporary folder here. */
+static char app_home[1024], app_bin[1200];
+
+static void run_as_app(void *dir) {
+    setenv("HOME", app_home, 1);
+    execl(app_bin, app_bin, (const char *)dir, (char *)NULL);
+    _exit(127);
+}
+
+TEST(run_as_the_app_logs_to_library_logs) {
+    test_tmp_dir(app_home, sizeof app_home);
+    char macos[1100], cmd[2600];
+    snprintf(macos, sizeof macos, "%s/Loony.app/Contents/MacOS", app_home);
+    CHECK(make_dirs(macos));
+    snprintf(app_bin, sizeof app_bin, "%s/loony", macos);
+    snprintf(cmd, sizeof cmd, "cp '%s' '%s'", LOONY_BIN, app_bin);
+    CHECK(system(cmd) == 0);
+    char log[1200];
+    snprintf(log, sizeof log, "%s/Library/Logs/loony-shim/loony.log", app_home);
+    char out[4096];
+    for (int run = 0; run < 2; run++) { /* the second run keeps the first log as loony.previous.log */
+        int status = test_run_child(run_as_app, (void *)"/nonexistent/loony", out, sizeof out);
+        CHECK_EQ(status, 1);
+        CHECK_STR(out, ""); /* nothing on stderr */
+    }
+    size_t len = 0;
+    char *text = (char *)read_file(log, &len);
+    CHECK(text != NULL);
+    text = realloc(text, len + 1);
+    text[len] = '\0';
+    CHECK_CONTAINS(text, "can't read /nonexistent/loony/LOONY LABYRINTH 3.0.1");
+    CHECK_CONTAINS(text, "needs the original game in /nonexistent/loony");
+    free(text);
+    snprintf(log, sizeof log, "%s/Library/Logs/loony-shim/loony.previous.log", app_home);
+    CHECK(access(log, F_OK) == 0);
+    test_remove_tree(app_home);
+}
+
+static char bundle_bin[1300];
+
+static void run_bundle_scripted(void *dir) {
+    setenv("LOONY_FIXED_CLOCK", "1", 1);
+    setenv("LOONY_SCRIPT", script_path, 1);
+    setenv("LOONY_AUTO_ALERTS", "1", 1);
+    setenv("LOONY_DATA_DIR", run_data, 1);
+    execl(bundle_bin, bundle_bin, (const char *)dir, (char *)NULL);
+    _exit(127);
+}
+
+/* tools/make_app.sh: the bundle carries its own libraries, is signed with
+   the hardened runtime and allow-jit, and still emulates the game exactly
+   (the approved menu frame). */
+TEST(run_the_app_bundle_is_self_contained_and_plays) {
+    SKIP_UNLESS_GAME();
+    char out_dir[1024], cmd[3000], text[8192];
+    test_tmp_dir(out_dir, sizeof out_dir);
+    snprintf(cmd, sizeof cmd, "'%s/tools/make_app.sh' '%s' '%s' >/dev/null 2>&1", LOONY_SRC_DIR,
+             LOONY_BIN, out_dir);
+    CHECK(system(cmd) == 0);
+    snprintf(bundle_bin, sizeof bundle_bin, "%s/Loony Labyrinth.app/Contents/MacOS/loony", out_dir);
+    snprintf(cmd, sizeof cmd, "otool -L '%s' && codesign -d --entitlements - '%s/Loony Labyrinth.app' 2>&1",
+             bundle_bin, out_dir);
+    FILE *p = popen(cmd, "r");
+    CHECK(p != NULL);
+    size_t n = fread(text, 1, sizeof text - 1, p);
+    text[n] = '\0';
+    pclose(p);
+    CHECK(!strstr(text, "/opt/homebrew/"));
+    CHECK_CONTAINS(text, "@rpath/libunicorn");
+    CHECK_CONTAINS(text, "com.apple.security.cs.allow-jit");
+
+    char png[1024];
+    tmp_name(png, sizeof png, "shot");
+    tmp_name(script_path, sizeof script_path, "script");
+    FILE *f = fopen(script_path, "w");
+    fprintf(f, "1720 down esc\n1724 up esc\n1800 down esc\n1804 up esc\n1880 screenshot %s\n1900 quit\n", png);
+    fclose(f);
+    test_tmp_dir(run_data, sizeof run_data);
+    char out[32768];
+    int status = test_run_child(run_bundle_scripted, (void *)test_game_dir(), out, sizeof out);
+    test_remove_tree(run_data);
+    run_data[0] = '\0';
+    unlink(script_path);
+    size_t len = 0;
+    uint8_t *shot = read_file(png, &len);
+    unlink(png);
+    test_remove_tree(out_dir);
+    CHECK_EQ(status, 0);
+    CHECK(shot != NULL);
+    CHECK_EQ(fnv1a32(shot, len), 0xADE78151u); /* the menu */
+    free(shot);
+}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build && ./build/loony_tests run_as_the_app`
Expected: the two new tests fail (there is no `tools/make_app.sh`; the app logs to stderr).

- [ ] **Step 3: Implement**

```diff
diff --git a/CMakeLists.txt b/CMakeLists.txt
index 6e2612b..fb7b311 100644
--- a/CMakeLists.txt
+++ b/CMakeLists.txt
@@ -28,11 +28,19 @@ target_link_libraries(loony_core PUBLIC PkgConfig::UNICORN PkgConfig::SDL3 "-fra
 add_executable(loony src/main.c)
 target_link_libraries(loony PRIVATE loony_core)
 
+# The double-clickable app: cmake --build build-release --target app
+add_custom_target(app
+  COMMAND ${CMAKE_SOURCE_DIR}/tools/make_app.sh $<TARGET_FILE:loony> ${CMAKE_BINARY_DIR}
+  DEPENDS loony
+  COMMENT "Building Loony Labyrinth.app"
+  VERBATIM)
+
 file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.c)
 add_executable(loony_tests ${TEST_SOURCES})
 target_include_directories(loony_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
 target_link_libraries(loony_tests PRIVATE loony_core)
-target_compile_definitions(loony_tests PRIVATE LOONY_BIN="$<TARGET_FILE:loony>")
+target_compile_definitions(loony_tests PRIVATE LOONY_BIN="$<TARGET_FILE:loony>"
+                                               LOONY_SRC_DIR="${CMAKE_SOURCE_DIR}")
 add_dependencies(loony_tests loony)
 
 enable_testing()
diff --git a/src/main.c b/src/main.c
index f998c25..d5b7daa 100644
--- a/src/main.c
+++ b/src/main.c
@@ -1,8 +1,11 @@
+#include <SDL3/SDL.h>
 #include <errno.h>
 #include <limits.h>
+#include <stdarg.h>
 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
+#include <unistd.h>
 
 #include "cf.h"
 #include "cpu.h"
@@ -24,43 +27,87 @@
 #define DEFAULT_GAME_DIR "/Applications/Loony Labyrinth"
 #define GAME_EXE_NAME "LOONY LABYRINTH 3.0.1"
 
+/* Launched as the app (from Finder or `open`), there is no terminal: the
+   log goes to ~/Library/Logs/loony-shim/loony.log (the one before it is kept
+   as loony.previous.log), and failures are shown in a message box. */
+static char log_path[PATH_MAX];
+
+static void show_failure(const char *msg) {
+    const char *video = getenv("SDL_VIDEO_DRIVER");
+    if (video && strcmp(video, "dummy") == 0) /* headless: nobody to click it */
+        return;
+    char text[2048];
+    snprintf(text, sizeof text, "%s\n\nThe log is in %s", msg, log_path);
+    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Loony Labyrinth", text, NULL);
+}
+
+static void log_to_file_if_app(const char *argv0) {
+    const char *home = getenv("HOME");
+    if (!strstr(argv0, ".app/Contents/MacOS/") || isatty(STDERR_FILENO) || !home)
+        return;
+    char dir[PATH_MAX], prev[PATH_MAX + 32];
+    snprintf(dir, sizeof dir, "%s/Library/Logs/loony-shim", home);
+    snprintf(log_path, sizeof log_path, "%s/loony.log", dir);
+    snprintf(prev, sizeof prev, "%s/loony.previous.log", dir);
+    if (!make_dirs(dir))
+        return;
+    rename(log_path, prev);
+    if (freopen(log_path, "w", stderr))
+        setvbuf(stderr, NULL, _IOLBF, 0);
+    util_set_failure_hook(show_failure);
+}
+
+/* An error before the game starts: printed, and shown when running as the app. */
+static int startup_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
+static int startup_error(const char *fmt, ...) {
+    char msg[2048];
+    va_list ap;
+    va_start(ap, fmt);
+    vsnprintf(msg, sizeof msg, fmt, ap);
+    va_end(ap);
+    fprintf(stderr, "loony: %s\n", msg);
+    util_report_failure(msg);
+    return 1;
+}
+
 int main(int argc, char **argv) {
-    if (argc > 2) {
+    log_to_file_if_app(argv[0]);
+    const char *dir = DEFAULT_GAME_DIR;
+    int nargs = 0;
+    for (int i = 1; i < argc; i++) {
+        if (strncmp(argv[i], "-psn_", 5) == 0) /* older macOS adds this when launching an app */
+            continue;
+        dir = argv[i];
+        nargs++;
+    }
+    if (nargs > 1) {
         fprintf(stderr, "usage: loony [game-folder]\n");
         return 1;
     }
-    const char *dir = argc == 2 ? argv[1] : DEFAULT_GAME_DIR;
 
     char path[PATH_MAX];
     snprintf(path, sizeof path, "%s/%s", dir, GAME_EXE_NAME);
     size_t len = 0;
     uint8_t *buf = read_file(path, &len);
-    if (!buf) {
-        fprintf(stderr, "loony: can't read %s: %s\n", path, strerror(errno));
-        return 1;
-    }
+    if (!buf)
+        return startup_error("can't read %s: %s. Loony Labyrinth needs the original game in %s.", path,
+                             strerror(errno), dir);
 
     char fork_path[PATH_MAX + 32];
     snprintf(fork_path, sizeof fork_path, "%s/..namedfork/rsrc", path);
     size_t fork_len = 0;
     uint8_t *fork = read_file(fork_path, &fork_len);
-    if (!fork) {
-        fprintf(stderr, "loony: can't read %s: %s\n", fork_path, strerror(errno));
-        return 1;
-    }
+    if (!fork)
+        return startup_error("can't read %s: %s", fork_path, strerror(errno));
 
     gm_init();
     cpu_init();
     loaded_image img;
     char err[256];
-    if (!image_load(buf, len, &img, err, sizeof err)) {
-        fprintf(stderr, "loony: can't load %s: %s\n", path, err);
-        return 1;
-    }
-    if (!rsrc_open(fork, fork_len, err, sizeof err)) {
-        fprintf(stderr, "loony: can't load the resources of %s: %s\n", path, err);
-        return 1;
-    }
+    if (!image_load(buf, len, &img, err, sizeof err))
+        return startup_error("can't load %s: %s", path, err);
+    if (!rsrc_open(fork, fork_len, err, sizeof err))
+        return startup_error("can't load the resources of %s: %s", path, err);
     mm_init();
     misc_init();
     cf_init();
diff --git a/src/trap.c b/src/trap.c
index 9211dc7..e46d51d 100644
--- a/src/trap.c
+++ b/src/trap.c
@@ -112,13 +112,14 @@ static void report_state(void) {
 }
 
 void trap_crash(const char *fmt, ...) {
+    char msg[1024];
     va_list ap;
     va_start(ap, fmt);
-    fputs("loony: crash: ", stderr);
-    vfprintf(stderr, fmt, ap);
-    fputc('\n', stderr);
+    vsnprintf(msg, sizeof msg, fmt, ap);
     va_end(ap);
+    fprintf(stderr, "loony: crash: %s\n", msg);
     report_state();
+    util_report_failure(msg);
     exit(2);
 }
 
diff --git a/src/util.c b/src/util.c
index 9a22e51..6ffe138 100644
--- a/src/util.c
+++ b/src/util.c
@@ -7,13 +7,25 @@
 #include <string.h>
 #include <sys/stat.h>
 
+static util_failure_fn failure_hook;
+
+void util_set_failure_hook(util_failure_fn fn) { failure_hook = fn; }
+
+void util_report_failure(const char *msg) {
+    util_failure_fn fn = failure_hook;
+    failure_hook = NULL; /* once, even if the hook itself fails */
+    if (fn)
+        fn(msg);
+}
+
 void fatal(const char *fmt, ...) {
+    char msg[1024];
     va_list ap;
     va_start(ap, fmt);
-    fputs("loony: fatal: ", stderr);
-    vfprintf(stderr, fmt, ap);
-    fputc('\n', stderr);
+    vsnprintf(msg, sizeof msg, fmt, ap);
     va_end(ap);
+    fprintf(stderr, "loony: fatal: %s\n", msg);
+    util_report_failure(msg);
     exit(2);
 }
 
diff --git a/src/util.h b/src/util.h
index 5d40efb..5bc8774 100644
--- a/src/util.h
+++ b/src/util.h
@@ -6,6 +6,12 @@
 /* Prints "loony: fatal: <msg>" to stderr and exits with status 2. */
 _Noreturn void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
 
+/* Called by fatal() and trap_crash() with their message just before the
+   process exits with status 2 (the app shows it in a message box). */
+typedef void (*util_failure_fn)(const char *msg);
+void util_set_failure_hook(util_failure_fn fn);
+void util_report_failure(const char *msg);
+
 /* Prints "loony: <msg>" to stderr. */
 void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
 
diff --git a/tools/loony.entitlements b/tools/loony.entitlements
new file mode 100644
index 0000000..a508641
--- /dev/null
+++ b/tools/loony.entitlements
@@ -0,0 +1,13 @@
+<?xml version="1.0" encoding="UTF-8"?>
+<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
+<plist version="1.0">
+<dict>
+	<!-- Unicorn translates the game's PowerPC code into native code at run time. -->
+	<key>com.apple.security.cs.allow-jit</key>
+	<true/>
+	<!-- Signed ad hoc, the bundled libraries have no team ID to match the
+	     app's, so the hardened runtime would refuse to load them. -->
+	<key>com.apple.security.cs.disable-library-validation</key>
+	<true/>
+</dict>
+</plist>
diff --git a/tools/make_app.sh b/tools/make_app.sh
new file mode 100755
index 0000000..7d01810
--- /dev/null
+++ b/tools/make_app.sh
@@ -0,0 +1,52 @@
+#!/bin/sh
+# Builds "Loony Labyrinth.app" around a loony binary: the Unicorn and SDL3
+# libraries are copied into the bundle (so a Homebrew upgrade can't break
+# it), and the bundle is signed ad hoc with the hardened runtime and the
+# allow-jit entitlement.
+#   tools/make_app.sh <loony binary> <output folder>
+set -eu
+bin=$1
+out=$2
+here=$(cd "$(dirname "$0")" && pwd)
+app="$out/Loony Labyrinth.app"
+rm -rf "$app"
+mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks"
+cp "$bin" "$app/Contents/MacOS/loony"
+cat > "$app/Contents/Info.plist" <<PLIST
+<?xml version="1.0" encoding="UTF-8"?>
+<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
+<plist version="1.0">
+<dict>
+	<key>CFBundleExecutable</key><string>loony</string>
+	<key>CFBundleIdentifier</key><string>local.loony-shim</string>
+	<key>CFBundleName</key><string>Loony Labyrinth</string>
+	<key>CFBundleDisplayName</key><string>Loony Labyrinth</string>
+	<key>CFBundlePackageType</key><string>APPL</string>
+	<key>CFBundleShortVersionString</key><string>3.0.1</string>
+	<key>CFBundleVersion</key><string>1</string>
+	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
+	<key>LSMinimumSystemVersion</key><string>13.0</string>
+	<key>LSApplicationCategoryType</key><string>public.app-category.arcade-games</string>
+	<key>NSHighResolutionCapable</key><true/>
+</dict>
+</plist>
+PLIST
+# Copy each Homebrew library the binary links and point the binary at the copy.
+for lib in $(otool -L "$bin" | awk '/\/opt\/homebrew\// {print $1}'); do
+    name=$(basename "$lib")
+    cp "$lib" "$app/Contents/Frameworks/$name"
+    chmod u+w "$app/Contents/Frameworks/$name"
+    install_name_tool -id "@rpath/$name" "$app/Contents/Frameworks/$name"
+    install_name_tool -change "$lib" "@rpath/$name" "$app/Contents/MacOS/loony"
+done
+install_name_tool -add_rpath "@executable_path/../Frameworks" "$app/Contents/MacOS/loony"
+if otool -L "$app/Contents/MacOS/loony" "$app"/Contents/Frameworks/*.dylib | grep -q /opt/homebrew/; then
+    echo "make_app.sh: the bundle still refers to Homebrew libraries" >&2
+    exit 1
+fi
+for lib in "$app"/Contents/Frameworks/*.dylib; do
+    codesign --force --sign - --options runtime "$lib"
+done
+codesign --force --sign - --options runtime --entitlements "$here/loony.entitlements" "$app"
+codesign --verify --strict "$app"
+echo "built $app"
```

- [ ] **Step 4: Run the tests**

Run: `cmake -S . -B build && cmake --build build && ./build/loony_tests run_as_the_app && ./build/loony_tests run_the_app_bundle && ./build/loony_tests run_reports && ./build/loony_tests`
Expected: all pass (the real-key registration test skips); no sanitizer reports.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/main.c src/trap.c src/util.c src/util.h tests/test_run.c tools/loony.entitlements tools/make_app.sh
git commit -m "Loony Labyrinth.app: bundled libraries, ad-hoc signed with allow-jit; log file and message boxes when launched from Finder"
```

---

### Task 3: The regression run

**Files:**
- Modify: `tests/test_run.c`

**Interfaces:**
- Produces: `run_three_minutes_of_play_match_the_recording`.

- [ ] **Step 1: Write the failing tests**

```diff
diff --git a/tests/test_run.c b/tests/test_run.c
index fe92456..e0bebb8 100644
--- a/tests/test_run.c
+++ b/tests/test_run.c
@@ -442,3 +442,75 @@ TEST(run_the_app_bundle_is_self_contained_and_plays) {
     CHECK_EQ(fnv1a32(shot, len), 0xADE78151u); /* the menu */
     free(shot);
 }
+
+/* The regression run: three minutes of scripted play on the fixed clock,
+   from the opening through a game (plunger, flippers, nudges), with frames
+   at each minute and the whole recording hashed. Any change to the
+   emulation, the physics the game computes, drawing or sound shows up here.
+   Recorded on 2026-10-02 after the user approved Plan 6's build. */
+#define REGRESSION_TICKS 10800
+
+static void write_regression_script(const char *path, char shots[3][1024]) {
+    FILE *f = fopen(path, "w");
+    /* Start a game: Esc ends the demo, Esc opens the menu, Return twice. */
+    fprintf(f, "1720 down esc\n1724 up esc\n1800 down esc\n1804 up esc\n"
+               "1900 down return\n1906 up return\n2000 down return\n2006 up return\n");
+    int shot = 0;
+    for (int t = 2100; t < REGRESSION_TICKS - 200;) {
+        fprintf(f, "%d down return\n%d up return\n", t, t + 80); /* the plunger */
+        t += 120;
+        for (int i = 0; i < 12; i++, t += 25)
+            fprintf(f, "%d down %s\n%d up %s\n", t, i % 2 ? "slash" : "z", t + 8, i % 2 ? "slash" : "z");
+        if ((t / 1000) % 3 == 0) {
+            fprintf(f, "%d down space\n%d up space\n", t, t + 5);
+            t += 20;
+        }
+        while (shot < 3 && t >= 3600 * (shot + 1) - 300) { /* just before each minute ends */
+            fprintf(f, "%d screenshot %s\n", t, shots[shot]);
+            shot++;
+            t += 2;
+        }
+    }
+    fclose(f);
+}
+
+TEST(run_three_minutes_of_play_match_the_recording) {
+    SKIP_UNLESS_GAME();
+    char shots[3][1024];
+    for (int i = 0; i < 3; i++)
+        tmp_name(shots[i], sizeof shots[i], "shot");
+    tmp_name(script_path, sizeof script_path, "script");
+    write_regression_script(script_path, shots);
+    tmp_name(wav_path, sizeof wav_path, "wav");
+    test_tmp_dir(run_data, sizeof run_data);
+    char ticks[16];
+    snprintf(ticks, sizeof ticks, "%d", REGRESSION_TICKS);
+    setenv("LOONY_EXIT_AFTER", ticks, 1);
+    char out[32768];
+    int status = test_run_child(run_loony_scripted, (void *)test_game_dir(), out, sizeof out);
+    unsetenv("LOONY_EXIT_AFTER");
+    test_remove_tree(run_data);
+    run_data[0] = '\0';
+    unlink(script_path);
+    uint32_t h[3];
+    for (int i = 0; i < 3; i++) {
+        size_t len = 0;
+        uint8_t *png = read_file(shots[i], &len);
+        h[i] = png ? fnv1a32(png, len) : 0;
+        free(png);
+        unlink(shots[i]);
+    }
+    size_t len = 0;
+    uint8_t *wav = read_file(wav_path, &len);
+    uint32_t wh = wav ? fnv1a32(wav, len) : 0;
+    free(wav);
+    unlink(wav_path);
+    wav_path[0] = '\0';
+    CHECK_EQ(status, 0);
+    CHECK(!strstr(out, "runtime error"));
+    CHECK_EQ(len, 44 + (size_t)REGRESSION_TICKS * 44100 / 60 * 4);
+    CHECK_EQ(h[0], 0xAAD1E97Fu); /* minute 1: ball 1 in play */
+    CHECK_EQ(h[1], 0x015482C8u); /* minute 2: ball 3, 13 seconds of demo time left */
+    CHECK_EQ(h[2], 0x66E6FBF1u); /* minute 3: the time ran out; a new game, ball 1 */
+    CHECK_EQ(wh, 0x3663C0FEu);
+}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build && ./build/loony_tests run_three`
Expected: n/a: the hashes are recorded from the first run (Release and Debug must agree).

- [ ] **Step 3: Run the tests**

Run: `cmake -S . -B build && cmake --build build && ./build/loony_tests run_three && ./build/loony_tests`
Expected: all pass (the real-key registration test skips); no sanitizer reports.

Record the four hashes from a first run with the CHECKs commented out, then check that Debug and Release agree before writing them in.

- [ ] **Step 4: Commit**

```bash
git add tests/test_run.c
git commit -m "Regression run: three minutes of scripted play, frames and recording hashed"
```

---

### Task 4: README and spec

**Files:**
- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-09-30-loony-shim-design.md`
- Create: `tools/soak_script.py`

- [ ] **Step 1: Write the docs**

```diff
diff --git a/README.md b/README.md
index 71666aa..81d3af8 100644
--- a/README.md
+++ b/README.md
@@ -14,10 +14,22 @@ cmake --build build
 
 cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release  # for playing
 cmake --build build-release
+cmake --build build-release --target app                # build-release/Loony Labyrinth.app
 ```
 
 ## Play
 
+Copy `build-release/Loony Labyrinth.app` to `/Applications` (or anywhere) and
+double-click it. It plays the game in `/Applications/Loony Labyrinth`, and
+carries its own copies of Unicorn and SDL3, so Homebrew upgrades don't affect
+it. It is signed ad hoc for this Mac only; if macOS refuses to open it after
+copying it from elsewhere, right-click it and choose Open. When the app can't
+start or the game crashes, it says so in a message box; its log is
+`~/Library/Logs/loony-shim/loony.log` (the run before is kept as
+`loony.previous.log`).
+
+From a terminal:
+
 ```bash
 ./build-release/loony                       # uses /Applications/Loony Labyrinth
 ./build-release/loony "/path/to/game folder"
@@ -83,7 +95,15 @@ action per line:
 ```
 
 Scripts can also click (`20 click 460 270`, in emulated-screen pixels) and type
-into a dialog (`50 type me@example.com`, the rest of the line).
+into a dialog (`50 type me@example.com`, the rest of the line), and may be any
+length. `tools/soak_script.py` writes one that keeps playing for an hour (game
+starts, plunger, flippers, nudges, a screenshot every 5 minutes):
+
+```bash
+python3 tools/soak_script.py 216000 /tmp/soak > /tmp/soak.txt
+LOONY_DATA_DIR=$(mktemp -d) LOONY_AUTO_ALERTS=1 LOONY_FIXED_CLOCK=1 LOONY_SCRIPT=/tmp/soak.txt \
+  SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy ./build-release/loony
+```
 
 Key names are those in `src/keymap.c` (`z`, `slash`, `return`, `space`, `esc`,
 `lshift`, `rshift`, ...).
diff --git a/docs/superpowers/specs/2026-09-30-loony-shim-design.md b/docs/superpowers/specs/2026-09-30-loony-shim-design.md
index 27aadf7..7372d4c 100644
--- a/docs/superpowers/specs/2026-09-30-loony-shim-design.md
+++ b/docs/superpowers/specs/2026-09-30-loony-shim-design.md
@@ -255,7 +255,7 @@ Used for: the init and main entry points, Carbon event handlers, event loop time
 | 4 | Events and input | Event loop, timers, keyboard. The table plays silently (user playtest) |
 | 5 | Sound | Sound Manager and mixer. Effects and music are correct (user playtest) |
 | 6 | Dialogs, files, prefs | High scores and preferences persist across launches (user playtest) |
-| 7 | Finish | One-hour run with no crash. `.app` bundle, ad-hoc signed, with the `allow-jit` entitlement. Headless regression test recorded |
+| 7 | Finish | One-hour run with no crash. `.app` bundle, ad-hoc signed, with the `allow-jit` entitlement. Headless regression test recorded (three minutes of scripted play, `run_three_minutes_of_play_match_the_recording`) |
 
 Milestones 1–3 have the most unknowns. Each later milestone's details may be adjusted based on what the import trace shows the game actually does.
 
@@ -275,3 +275,4 @@ Milestones 1–3 have the most unknowns. Each later milestone's details may be a
 - `~/dev/loony-shim`, a git repo on `main`. C11, `-Wall -Wextra -Werror` in all builds. `Debug` adds the sanitizers, and `Release` is `-O2`.
 - `.gitignore` excludes build output, PNG dumps and anything copied from the game folder.
 - Dependencies come from Homebrew: `unicorn`, `sdl3`, `cmake`, `pkg-config`. The preferences file also uses macOS's own CoreFoundation framework.
+- **The app** (`cmake --build build-release --target app`, `tools/make_app.sh`): `Loony Labyrinth.app`, with `libunicorn` and `libSDL3` copied into `Contents/Frameworks` and referenced through `@rpath`, signed ad hoc with the hardened runtime and the entitlements `allow-jit` (Unicorn's translated code) and `disable-library-validation` (ad-hoc signatures carry no team ID, so the hardened runtime would otherwise refuse the bundled libraries). Launched as the app, with no terminal, the log goes to `~/Library/Logs/loony-shim/loony.log` and startup failures and crashes appear in a message box. (Revised during Plan 7.)
diff --git a/tools/soak_script.py b/tools/soak_script.py
new file mode 100755
index 0000000..29ce1c8
--- /dev/null
+++ b/tools/soak_script.py
@@ -0,0 +1,32 @@
+#!/usr/bin/env python3
+"""Writes a LOONY_SCRIPT that keeps the game playing: every minute it starts a
+game (Esc, Esc, Return, Return), and in between pulls the plunger, works the
+flippers and nudges. A screenshot every 5 minutes goes to <shot prefix>NN.png,
+and the script asks the game to quit a second before the end.
+
+    tools/soak_script.py <ticks> <shot prefix> > soak.txt
+"""
+import sys
+
+ticks, prefix = int(sys.argv[1]), sys.argv[2]
+lines, t = [], 1700
+while t < ticks - 600:
+    for key in ('esc', 'esc', 'return', 'return'):
+        lines += [(t, f'down {key}'), (t + 4, f'up {key}')]
+        t += 90
+    end = t + 3600
+    while t < end:
+        lines += [(t, 'down return'), (t + 80, 'up return')]
+        t += 120
+        for i in range(12):
+            key = 'z' if i % 2 == 0 else 'slash'
+            lines += [(t, f'down {key}'), (t + 8, f'up {key}')]
+            t += 25
+        if (t // 1000) % 7 == 0:
+            lines += [(t, 'down space'), (t + 5, 'up space')]
+            t += 20
+for minute in range(1, ticks // 3600 + 1, 5):
+    lines.append((minute * 3600, f'screenshot {prefix}{minute:02d}.png'))
+lines.append((ticks - 60, 'quit'))
+lines.sort(key=lambda a: a[0])
+sys.stdout.write(''.join(f'{t} {a}\n' for t, a in lines))
```

- [ ] **Step 2: Commit**

```bash
git add README.md docs/superpowers/specs/2026-09-30-loony-shim-design.md tools/soak_script.py
git commit -m "README and spec: the app, its log, and the soak script"
```

---

### Task 5: The one-hour runs

Both runs use `tools/soak_script.py 216000`: about 12,700 actions, starting a game every minute and working the plunger, the flippers and the nudge in between.

- [ ] **Step 1: The fixed-clock hour** (Release, `LOONY_FIXED_CLOCK=1`, dummy drivers, a fresh `LOONY_DATA_DIR`)

Result (2026-10-02):
- All 216,000 ticks ran in 19.5 minutes of wall time.
- No crash, no sanitizer or sound messages.
- RSS rose from 113.6 MB to 117.9 MB in the first 15 minutes, then stayed flat. `leaks` reported 0 leaks.
- The screenshots every 5 minutes show attract mode, menus and games in play.

At the end, the game didn't quit within the 3-second grace period, which is about 550 fixed-clock ticks. A replay with tracing switched on near the end (deterministic, so the same state) showed why. The game's quit handler ran and called `QuitApplicationEventLoop`, but the game was inside an inner frame loop that waits for a sound channel to go idle, calling `ReceiveNextEvent` with timeout 0 and `SndChannelStatus` many times a frame. It reaches the application loop only when that ends. The forced exit then skipped `CFPreferencesAppSynchronize`, and with it anything changed since launch.

Quits mid-game, from the menu, from the pause screen and from attract mode all take effect at once. Task 7 fixes the lost preferences.

- [ ] **Step 2: The real-time hour** (the signed `Loony Labyrinth.app`, dummy video and audio drivers, so the audio thread and the clock-driven timers run as for the user)

Result (2026-10-02):
- 3,601 seconds. The script's quit at the end went through the game's own handler (`ExitToShell`).
- No crash and no errors.
- RSS stayed between 107 and 119 MB.
- At minute 56 a game was in play: ball 2, 3,176,000 points.

---

### Task 6: Review fixes

A review of Tasks 1-4 found these defects:
- **The bundle test let the app log into the real `~/Library/Logs`.** The bundled binary's path makes it an app, and its stderr is a pipe. The test now gives it a temporary HOME.
- **A sanitizer build could be bundled though it needs the compiler's runtime.** `make_app.sh` now allows only system libraries and libraries in Frameworks, checks every search path, and fails with "build it from a Release build". It also deletes the `/opt/homebrew/lib` search path the link adds. The bundle test expects that refusal from a Debug binary.
- **Smaller fixes:**
  - The failure hook is set even when the log folder can't be made, and then the message box doesn't name a log.
  - A usage error is reported as a startup error.
  - A script with an error keeps none of its actions.
  - The soak script's last screenshot comes before its quit.

- [ ] **Step 1: Apply**

```diff
diff --git a/src/main.c b/src/main.c
index d5b7daa..93b3357 100644
--- a/src/main.c
+++ b/src/main.c
@@ -37,7 +37,10 @@ static void show_failure(const char *msg) {
     if (video && strcmp(video, "dummy") == 0) /* headless: nobody to click it */
         return;
     char text[2048];
-    snprintf(text, sizeof text, "%s\n\nThe log is in %s", msg, log_path);
+    if (log_path[0])
+        snprintf(text, sizeof text, "%s\n\nThe log is in %s", msg, log_path);
+    else
+        snprintf(text, sizeof text, "%s", msg);
     SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Loony Labyrinth", text, NULL);
 }
 
@@ -49,12 +52,16 @@ static void log_to_file_if_app(const char *argv0) {
     snprintf(dir, sizeof dir, "%s/Library/Logs/loony-shim", home);
     snprintf(log_path, sizeof log_path, "%s/loony.log", dir);
     snprintf(prev, sizeof prev, "%s/loony.previous.log", dir);
-    if (!make_dirs(dir))
+    util_set_failure_hook(show_failure);
+    if (!make_dirs(dir)) {
+        log_path[0] = '\0';
         return;
+    }
     rename(log_path, prev);
     if (freopen(log_path, "w", stderr))
         setvbuf(stderr, NULL, _IOLBF, 0);
-    util_set_failure_hook(show_failure);
+    else
+        log_path[0] = '\0';
 }
 
 /* An error before the game starts: printed, and shown when running as the app. */
@@ -80,10 +87,8 @@ int main(int argc, char **argv) {
         dir = argv[i];
         nargs++;
     }
-    if (nargs > 1) {
-        fprintf(stderr, "usage: loony [game-folder]\n");
-        return 1;
-    }
+    if (nargs > 1)
+        return startup_error("usage: loony [game-folder]");
 
     char path[PATH_MAX];
     snprintf(path, sizeof path, "%s/%s", dir, GAME_EXE_NAME);
diff --git a/src/script.c b/src/script.c
index 2470cf5..7866b17 100644
--- a/src/script.c
+++ b/src/script.c
@@ -14,9 +14,7 @@ static struct {
     int n, cap, next;
 } SC;
 
-bool script_parse(const char *text, char *err, size_t errlen) {
-    free(SC.a);
-    memset(&SC, 0, sizeof SC);
+static bool parse(const char *text, char *err, size_t errlen) {
     int line_no = 0;
     uint32_t last = 0;
     const char *p = text;
@@ -102,6 +100,15 @@ bool script_parse(const char *text, char *err, size_t errlen) {
     return true;
 }
 
+bool script_parse(const char *text, char *err, size_t errlen) {
+    free(SC.a);
+    memset(&SC, 0, sizeof SC);
+    if (parse(text, err, errlen))
+        return true;
+    SC.n = 0; /* nothing from a script with an error */
+    return false;
+}
+
 bool script_load(const char *path, char *err, size_t errlen) {
     size_t len;
     uint8_t *text = read_file(path, &len);
diff --git a/tests/test_run.c b/tests/test_run.c
index e0bebb8..bc3fa35 100644
--- a/tests/test_run.c
+++ b/tests/test_run.c
@@ -391,6 +391,7 @@ TEST(run_as_the_app_logs_to_library_logs) {
 static char bundle_bin[1300];
 
 static void run_bundle_scripted(void *dir) {
+    setenv("HOME", run_data, 1); /* as an app it logs under HOME */
     setenv("LOONY_FIXED_CLOCK", "1", 1);
     setenv("LOONY_SCRIPT", script_path, 1);
     setenv("LOONY_AUTO_ALERTS", "1", 1);
@@ -401,14 +402,27 @@ static void run_bundle_scripted(void *dir) {
 
 /* tools/make_app.sh: the bundle carries its own libraries, is signed with
    the hardened runtime and allow-jit, and still emulates the game exactly
-   (the approved menu frame). */
+   (the approved menu frame). A sanitizer build (Debug) can't be bundled:
+   it needs the compiler's runtime, and the script says so. */
 TEST(run_the_app_bundle_is_self_contained_and_plays) {
     SKIP_UNLESS_GAME();
     char out_dir[1024], cmd[3000], text[8192];
     test_tmp_dir(out_dir, sizeof out_dir);
-    snprintf(cmd, sizeof cmd, "'%s/tools/make_app.sh' '%s' '%s' >/dev/null 2>&1", LOONY_SRC_DIR,
-             LOONY_BIN, out_dir);
-    CHECK(system(cmd) == 0);
+    snprintf(cmd, sizeof cmd, "'%s/tools/make_app.sh' '%s' '%s' 2>&1", LOONY_SRC_DIR, LOONY_BIN, out_dir);
+    FILE *mk = popen(cmd, "r");
+    CHECK(mk != NULL);
+    size_t got = fread(text, 1, sizeof text - 1, mk);
+    text[got] = '\0';
+    int made = pclose(mk);
+    snprintf(cmd, sizeof cmd, "otool -L '%s' | grep -q libclang_rt", LOONY_BIN);
+    if (system(cmd) == 0) {
+        test_remove_tree(out_dir);
+        CHECK(made != 0);
+        CHECK_CONTAINS(text, "libclang_rt");
+        CHECK_CONTAINS(text, "isn't self-contained");
+        return;
+    }
+    CHECK(made == 0);
     snprintf(bundle_bin, sizeof bundle_bin, "%s/Loony Labyrinth.app/Contents/MacOS/loony", out_dir);
     snprintf(cmd, sizeof cmd, "otool -L '%s' && codesign -d --entitlements - '%s/Loony Labyrinth.app' 2>&1",
              bundle_bin, out_dir);
diff --git a/tests/test_script.c b/tests/test_script.c
index 83162db..0e3056c 100644
--- a/tests/test_script.c
+++ b/tests/test_script.c
@@ -40,6 +40,8 @@ TEST(script_reports_errors_with_line_numbers) {
     CHECK(!script_parse("down z\n", err, sizeof err));
     CHECK_CONTAINS(err, "line 1: expected");
     CHECK_EQ(script_remaining(), 0);
+    CHECK(!script_parse("1 down z\n2 jump\n", err, sizeof err)); /* the good line isn't kept */
+    CHECK_EQ(script_remaining(), 0);
 }
 
 TEST(script_load_missing_file) {
diff --git a/tools/make_app.sh b/tools/make_app.sh
index 7d01810..2796e21 100755
--- a/tools/make_app.sh
+++ b/tools/make_app.sh
@@ -32,16 +32,36 @@ cat > "$app/Contents/Info.plist" <<PLIST
 </plist>
 PLIST
 # Copy each Homebrew library the binary links and point the binary at the copy.
-for lib in $(otool -L "$bin" | awk '/\/opt\/homebrew\// {print $1}'); do
+for lib in $(otool -L "$bin" | awk '/\/opt\/homebrew\// {print $1}'); do  # paths without spaces
     name=$(basename "$lib")
     cp "$lib" "$app/Contents/Frameworks/$name"
     chmod u+w "$app/Contents/Frameworks/$name"
     install_name_tool -id "@rpath/$name" "$app/Contents/Frameworks/$name"
     install_name_tool -change "$lib" "@rpath/$name" "$app/Contents/MacOS/loony"
 done
+# Search only Frameworks (the link adds /opt/homebrew/lib).
+for rp in $(otool -l "$app/Contents/MacOS/loony" | awk '/cmd LC_RPATH/ {getline; getline; print $2}'); do
+    install_name_tool -delete_rpath "$rp" "$app/Contents/MacOS/loony"
+done
 install_name_tool -add_rpath "@executable_path/../Frameworks" "$app/Contents/MacOS/loony"
-if otool -L "$app/Contents/MacOS/loony" "$app"/Contents/Frameworks/*.dylib | grep -q /opt/homebrew/; then
-    echo "make_app.sh: the bundle still refers to Homebrew libraries" >&2
+# Self-contained: every library is the system's or in Frameworks, and the
+# only search path is Frameworks. (A sanitizer build fails here: it needs
+# the compiler's runtime library.)
+bad=0
+for f in "$app/Contents/MacOS/loony" "$app"/Contents/Frameworks/*.dylib; do
+    for dep in $(otool -L "$f" | tail -n +2 | awk '{print $1}'); do
+        case "$dep" in
+        /usr/lib/* | /System/*) ;;
+        @rpath/*) [ -f "$app/Contents/Frameworks/${dep#@rpath/}" ] || { echo "make_app.sh: $f needs $dep, which isn't bundled" >&2; bad=1; } ;;
+        *) echo "make_app.sh: $f needs $dep, outside the bundle" >&2; bad=1 ;;
+        esac
+    done
+    for rp in $(otool -l "$f" | awk '/cmd LC_RPATH/ {getline; getline; print $2}'); do
+        [ "$rp" = "@executable_path/../Frameworks" ] || { echo "make_app.sh: $f searches $rp" >&2; bad=1; }
+    done
+done
+if [ "$bad" != 0 ]; then
+    echo "make_app.sh: the bundle isn't self-contained (build it from a Release build)" >&2
     exit 1
 fi
 for lib in "$app"/Contents/Frameworks/*.dylib; do
diff --git a/tools/soak_script.py b/tools/soak_script.py
index 29ce1c8..5e5ede8 100755
--- a/tools/soak_script.py
+++ b/tools/soak_script.py
@@ -25,7 +25,7 @@ while t < ticks - 600:
         if (t // 1000) % 7 == 0:
             lines += [(t, 'down space'), (t + 5, 'up space')]
             t += 20
-for minute in range(1, ticks // 3600 + 1, 5):
+for minute in range(1, (ticks - 61) // 3600 + 1, 5):  # all before the quit
     lines.append((minute * 3600, f'screenshot {prefix}{minute:02d}.png'))
 lines.append((ticks - 60, 'quit'))
 lines.sort(key=lambda a: a[0])
```

- [ ] **Step 2: Run the tests** (`./build/loony_tests` and `./build-release/loony_tests`): 292 passed, 1 skipped. `~/Library/Logs` holds nothing from the tests.

- [ ] **Step 3: Commit** ("Review fixes: the bundle test keeps out of ~/Library; make_app.sh allows only system and bundled libraries and drops foreign rpaths; app failures reported even without a log")

---

### Task 7: Preferences saved at any exit but a crash

The finding from Task 5:
- On macOS, preference values an application has set are kept even if it never synchronizes.
- `main` now registers an exit handler that calls `cf_save_prefs` unless `util_failed()`, which `fatal` and `trap_crash` set.
- `cf_save_prefs` leaves out a key whose value the game released too often, rather than crashing during exit.

- [ ] **Step 1: Apply**

```diff
diff --git a/README.md b/README.md
index 81d3af8..e2e88cd 100644
--- a/README.md
+++ b/README.md
@@ -58,7 +58,7 @@ The keys can be changed from the game's OPTIONS menu. Unregistered, games are
 time-limited.
 
 The game's preferences (options, keys, the high-score table and the license)
-are saved when it quits, in `~/Library/Application Support/loony-shim/prefs.plist`.
+are saved when it quits (and at any exit but a crash), in `~/Library/Application Support/loony-shim/prefs.plist`.
 Any file the game writes goes to the same folder, never into the game folder.
 Delete the folder to start over.
 
diff --git a/docs/superpowers/specs/2026-09-30-loony-shim-design.md b/docs/superpowers/specs/2026-09-30-loony-shim-design.md
index 7372d4c..d06459b 100644
--- a/docs/superpowers/specs/2026-09-30-loony-shim-design.md
+++ b/docs/superpowers/specs/2026-09-30-loony-shim-design.md
@@ -210,7 +210,7 @@ Used for: the init and main entry points, Carbon event handlers, event loop time
 - **Reading:** looks in the writable folder first, then the game folder.
 - **Writing:** writes to any path inside the game folder go to the matching path in the writable folder, copying the file there at its first write (not when it is opened) if it exists. This keeps the original files untouched while the game believes it saved in place. `LOONY_DATA_DIR` names another writable folder (the tests use temporary ones); one that is, contains or is inside the game folder is refused, and nothing is saved. A copy is made under a temporary name and renamed when complete, so a failed copy never hides the original. Names `.` and `..` are refused, and `::` goes up one folder but never above the game folder. (Measured in Plan 6: the game writes no files in normal play; everything it saves is a preference.)
 - **FSSpec calls:** `FSMakeFSSpec` resolves vRefNum/dirID/name to a host path, using a small table of fake volume and directory IDs. `FSpCreate`, `FSpOpenDF`, `PBReadSync`, `FSWrite`, `GetEOF`, `SetEOF`, `GetFPos`, `SetFPos`, `FSClose` and `PBFlushFileSync` map to POSIX calls. Mac-Roman file names are converted to UTF-8, and `:` becomes `/`.
-- **CFPreferences:** stored in `prefs.plist` in the writable folder, an XML property list of strings and integers written with the host's CoreFoundation, read at startup and written on `CFPreferencesAppSynchronize` (the game calls it as it quits). A file that can't be read is moved to `prefs.plist.bad`. CFString and CFNumber are host objects referred to by tag-space IDs, with reference counts. `kCFPreferencesCurrentApplication` is a pre-made CFString ID. (Measured in Plan 6: the game keeps its options, key assignments, the four-entry high-score table with a checksum, "highscore id", and the license, "user email" and "user id", in the preferences.)
+- **CFPreferences:** stored in `prefs.plist` in the writable folder, an XML property list of strings and integers written with the host's CoreFoundation, read at startup and written on `CFPreferencesAppSynchronize` (the game calls it as it quits) and at any other exit but a crash, as macOS keeps values an application set without synchronizing. (Revised during Plan 7: a quit during a sequence that outlasted the 3-second quit grace lost the game's unsaved changes.) A file that can't be read is moved to `prefs.plist.bad`. CFString and CFNumber are host objects referred to by tag-space IDs, with reference counts. `kCFPreferencesCurrentApplication` is a pre-made CFString ID. (Measured in Plan 6: the game keeps its options, key assignments, the four-entry high-score table with a checksum, "highscore id", and the license, "user email" and "user id", in the preferences.)
 
 ### Miscellaneous (`misc.c`)
 
diff --git a/src/cf.c b/src/cf.c
index e79a27f..b47a299 100644
--- a/src/cf.c
+++ b/src/cf.c
@@ -327,15 +327,21 @@ bool cf_save_prefs(void) {
     plist_entry *e = calloc(C.nprefs + 1, sizeof *e);
     if (!e)
         fatal("out of memory");
+    uint32_t n = 0;
     for (uint32_t i = 0; i < C.nprefs; i++) {
-        cf_obj *o = need("CFPreferencesAppSynchronize", C.prefs[i].value);
-        e[i].key = C.prefs[i].key;
-        e[i].is_number = o->type_id == CF_NUMBER_TYPE_ID;
-        e[i].num = o->num;
-        e[i].str = o->str;
+        cf_obj *o = lookup(C.prefs[i].value);
+        if (!o) { /* the game released a stored value too often; leave the key out */
+            log_msg("preferences: \"%s\" no longer has a value; not saving it", C.prefs[i].key);
+            continue;
+        }
+        e[n].key = C.prefs[i].key;
+        e[n].is_number = o->type_id == CF_NUMBER_TYPE_ID;
+        e[n].num = o->num;
+        e[n].str = o->str;
+        n++;
     }
     char err[512];
-    bool ok = plist_write(C.prefs_path, e, C.nprefs, err, sizeof err);
+    bool ok = plist_write(C.prefs_path, e, n, err, sizeof err);
     free(e); /* the strings belong to the preferences */
     if (!ok)
         log_msg("preferences: %s", err);
diff --git a/src/cf.h b/src/cf.h
index ee7650f..d7c6541 100644
--- a/src/cf.h
+++ b/src/cf.h
@@ -7,7 +7,8 @@
    (CF_TAG_BASE + 16 * index), which it never dereferences. Preferences live
    in memory; with a preferences file (cf_load_prefs) they are read from it
    at startup and written back by CFPreferencesAppSynchronize, which the game
-   calls as it quits. */
+   calls as it quits. main also saves them at any exit but a crash, as macOS
+   keeps values an application set without synchronizing. */
 
 #define CF_TAG_BASE       0x08000000u
 #define CF_TAG_LIMIT      0x09000000u
@@ -27,7 +28,8 @@ void cf_init(void);
 void cf_load_prefs(const char *path);
 
 /* Writes the preferences to the file named by cf_load_prefs. False (logged)
-   if that fails. True, doing nothing, if there's no file. */
+   if that fails. True, doing nothing, if there's no file. A key whose value
+   is no longer a live object is left out (logged). */
 bool cf_save_prefs(void);
 
 /* The CFStringRef stored in the kCFPreferencesCurrentApplication data import. */
diff --git a/src/main.c b/src/main.c
index 93b3357..98718d5 100644
--- a/src/main.c
+++ b/src/main.c
@@ -64,6 +64,15 @@ static void log_to_file_if_app(const char *argv0) {
         log_path[0] = '\0';
 }
 
+/* At exit, save what the game put in its preferences, unless it crashed:
+   macOS would have kept those values even if the game never called
+   CFPreferencesAppSynchronize (say, quit during a sequence that outlasted
+   the quit grace period). */
+static void save_prefs_at_exit(void) {
+    if (!util_failed())
+        cf_save_prefs();
+}
+
 /* An error before the game starts: printed, and shown when running as the app. */
 static int startup_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
 static int startup_error(const char *fmt, ...) {
@@ -124,6 +133,7 @@ int main(int argc, char **argv) {
         char prefs[PATH_MAX + 16];
         snprintf(prefs, sizeof prefs, "%s/prefs.plist", data_dir);
         cf_load_prefs(prefs);
+        atexit(save_prefs_at_exit);
     }
     qd_init(800, 600, 8);
     dialogs_init();
diff --git a/src/util.c b/src/util.c
index 6ffe138..8cedb55 100644
--- a/src/util.c
+++ b/src/util.c
@@ -8,10 +8,14 @@
 #include <sys/stat.h>
 
 static util_failure_fn failure_hook;
+static bool failed;
 
 void util_set_failure_hook(util_failure_fn fn) { failure_hook = fn; }
 
+bool util_failed(void) { return failed; }
+
 void util_report_failure(const char *msg) {
+    failed = true;
     util_failure_fn fn = failure_hook;
     failure_hook = NULL; /* once, even if the hook itself fails */
     if (fn)
diff --git a/src/util.h b/src/util.h
index 5bc8774..381f0af 100644
--- a/src/util.h
+++ b/src/util.h
@@ -12,6 +12,9 @@ typedef void (*util_failure_fn)(const char *msg);
 void util_set_failure_hook(util_failure_fn fn);
 void util_report_failure(const char *msg);
 
+/* True once util_report_failure has run (a crash or a fatal error). */
+bool util_failed(void);
+
 /* Prints "loony: <msg>" to stderr. */
 void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
 
diff --git a/tests/test_run.c b/tests/test_run.c
index bc3fa35..e06a56a 100644
--- a/tests/test_run.c
+++ b/tests/test_run.c
@@ -329,6 +329,28 @@ TEST(run_a_key_code_registers_and_survives_a_relaunch) {
     CHECK(!strstr(out, key));
 }
 
+/* A run that ends without the game synchronizing (here LOONY_EXIT_AFTER;
+   for the user, a quit during a sequence that outlasts the 3-second grace)
+   still saves what the game set, as macOS would. */
+TEST(run_preferences_are_saved_even_without_synchronize) {
+    SKIP_UNLESS_GAME();
+    test_tmp_dir(run_data, sizeof run_data);
+    char prefs[1100];
+    snprintf(prefs, sizeof prefs, "%s/prefs.plist", run_data);
+    setenv("LOONY_EXIT_AFTER", "300", 1);
+    char out[32768];
+    int status = run_script("", NULL, NULL, 0, out, sizeof out);
+    unsetenv("LOONY_EXIT_AFTER");
+    char *xml = read_text(prefs);
+    test_remove_tree(run_data);
+    run_data[0] = '\0';
+    CHECK_EQ(status, 0);
+    CHECK(!strstr(out, "sending the quit Apple Event")); /* the game never quit */
+    CHECK(xml != NULL);
+    CHECK_CONTAINS(xml, "<string>SNOWMAN</string>");
+    free(xml);
+}
+
 static void run_loony_bad_script(void *dir) {
     setenv("LOONY_SCRIPT", "/nonexistent/loony.script", 1);
     run_loony(dir);
```

- [ ] **Step 2: Run the tests:** 293 passed, 1 skipped, in Debug and Release.

- [ ] **Step 3: Commit** ("Save the game's preferences at any exit but a crash, as macOS would; a quit that outlasts the grace period no longer loses them")

---

## Done

All seven milestones are complete. Spec success criteria 1-5 were confirmed by the user on 2026-10-02. Criterion 6 (an hour with no crash) is met by Task 5.
