# Plan 6: Dialogs, Files and Preferences Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the game's state persist and its dialogs work. Preferences (options, keys, the high-score table and the license) are saved when the game quits and read at the next launch. Alerts and the key-code registration form are drawn on the emulated screen and answered with the mouse and keyboard, so the user can register the game. Every one of the game's 132 imports gets an implementation.

**Architecture:**
- **Preferences:** `cf.c` keeps CFPreferences in memory as before. `plist.c` reads and writes them as `prefs.plist`, an XML property list, through the host's CoreFoundation.
- **The writable folder:** `~/Library/Application Support/loony-shim`, or `$LOONY_DATA_DIR`. `files.c` overlays it on the read-only game folder. Lookups try the writable folder first, and the first write to a game file copies it there.
- **Dialogs:** `dialogs.c` draws ALRT, DLOG and DITL resources straight onto the emulated screen, with the 8x8 font from `font.c`, and saves and restores the pixels underneath. A modal loop pumps events until a button is chosen.
- **Input:** while a dialog is open, `events.c` routes input to it instead of queueing Carbon events. That covers keys, mouse clicks (mapped from the window by `display.c`), Cmd-V and the scripted `click` and `type` actions.
- **Pictures:** `pict.c` gains DirectBitsRect and the QuickTime matte the dialog icons carry. `blit.c` gains direct-to-indexed copies.

**Tech Stack:** C (gnu11), clang, CMake ≥ 3.20, Unicorn 2, SDL3, and macOS CoreFoundation for the plist.

**Spec:** `docs/superpowers/specs/2026-09-30-loony-shim-design.md` (milestone 6: dialogs, files and preferences; the Dialogs, Files and preferences, QuickDraw and Miscellaneous sections, revised in Task 7). Plan 5 (`docs/superpowers/plans/2026-10-01-plan5-sound.md`) finished sound. Plan 2 built CFPreferences in memory, and Plan 3 built read-only files and the auto-answered startup alerts that this plan replaces.

## Global Constraints

- Repo: `~/dev/loony-shim`, branch `main`. All paths below are relative to it.
- Game files in `/Applications/Loony Labyrinth` are read-only inputs. Never write, move or modify them, and never copy them, or anything extracted from them, into the repo.
- **The user's key code and e-mail address never go into the repo, a log, a test fixture or memory.** The positive registration test reads them from `LOONY_TEST_EMAIL` and `LOONY_TEST_KEY` at run time and is skipped without them. Dialogs never log typed text.
- C11 with GNU extensions (`gnu11`), clang, `-Wall -Wextra -Werror` in every build. Debug builds (the default) add `-fsanitize=address,undefined`. Release is `-O2`.
- Dependencies come from Homebrew (`unicorn`, `sdl3`, `cmake`, `pkg-config`) plus macOS's CoreFoundation framework.
- No test opens a real window or plays sound. No test touches the real `~/Library/Application Support/loony-shim`: the runner points `LOONY_DATA_DIR` at a temporary folder, and each scripted game run gets a fresh one.
- The golden frames and the golden recording the user approved on 2026-10-02 must not change. Those runs set `LOONY_AUTO_ALERTS=1`, which keeps Plan 3's behavior of answering startup alerts at once.

## Facts measured from the real game (the tests assert these)

| Fact | Value |
|---|---|
| Imports still missing at the start | `GetNewDialog`, `ModalDialog`, `GetDialogItem`, `GetDialogItemText`, `DisposeDialog`, `FSpCreate`, `FSWrite`, `SetEOF`, `PBFlushFileSync`, `ICLaunchURL`, `num2dec`, `GetEntryColor`, `DisposePalette` (13 of 131 functions) |
| Startup (`InternetEditionStartUp`, code+0xdcb0) | Unless `VerifyLicense` passes, Alert 901: item 1 Play Demo (then Alert 900, the key list), 2 Quit, 3 Buy Now (`ICLaunchURL`), 4 Enter Key-Code. Item 4 runs `GetNewDialog(911, NULL, -1)` and loops on `ModalDialog(NULL, &item)` until item 3 (Register) or 4 (Cancel). On Register it reads items 5 (e-mail) and 6 (key) with `GetDialogItem` and `GetDialogItemText`, calls `DisposeDialog`, then `RegisterLicense`, then Alert 902 (refused) or 903 (certified). Then it checks again |
| License | Kept in the preferences ("user email", "user id"). The check is offline and depends only on `Gestalt('mach')` (stored as "signet"), which is fixed here, so a license survives relaunches |
| Preferences | Options ("switch sound", "switch music", "mode score dsiplay" [sic], ...), the key assignments, and a 4-entry high-score table ("highscore N", "highscore name N", "highscore aux N", "highscore timestamp N") with a checksum, "highscore id" 1549460. A table whose checksum doesn't match is reset to the defaults (SNOWMAN 10000, LUNA 8000, RYU 6000, MIO 4000) |
| `CFPreferencesAppSynchronize` | Called once, from `main` as the game quits (code+0x2472c) |
| "switch music" = 0 | The opening is silent: every sample of a 300-tick fixed-clock recording is 0 |
| File writes | None in normal play. `FSpCreate`, `FSWrite` and `SetEOF` are reached only through `TArchive`/`TFileOutputStream` writing paths that a full game never takes |
| Dialog resources | ALRT 900, 901, 902, 903 and DLOG 911 use alert position (`0x300A`). 911 is `dBoxProc` with items: 1 PICT 128, 2 text, 3 Register, 4 Cancel, 5 and 6 empty edit fields, 7 and 8 labels (static text has the disabled bit set) |
| PICT 128 and 129 (the dialog icons) | 104x128. A clip region, an `UncompressedQuickTime` opcode (0x8201) holding a 104x128 8-bit gray QuickTime Animation ('rle ') matte, then a 32-bit `DirectBitsRect` (pack type 4, 3 components, mode 64 = ditherCopy) |
| SDL's debug font | 8x8, printable ASCII only. 'A' is 30 78 CC CC FC CC CC 00 |
| Golden frames (pending the user's approval) | Fixed clock, real alerts: Alert 901 at tick 30 `0x94D533D8`; the registration form filled in with a made-up address and key at tick 80 `0x6E85582A`; Alert 902 at tick 110 `0x8CF0D4FD`. The same in Debug and Release |

## Decisions this plan makes

- **The host's CoreFoundation writes the plist.** The format and its escaping are then right by construction, and the file can be read and edited with `plutil` or `defaults`. Strings are Mac Roman in the guest and UTF-8 in the file.
- **A damaged preferences file is set aside, not overwritten.** It's renamed to `prefs.plist.bad`, logged, and the game starts with none. Values that aren't strings or integers are skipped and logged.
- **Writes are atomic:** to `prefs.plist.tmp`, then a rename.
- **Preferences are written only on `CFPreferencesAppSynchronize`,** as the game asks, not at every exit. A crash doesn't save half-changed state.
- **Copy on the first write, not on open.** The game opens `effect.bin` with `fsCurPerm` (0), so copying at open would duplicate 580 KB for nothing. Every permission except `fsRdPerm` (1) allows writing.
- **`.` and `..` are refused as names** (`bdNamErr`), since they would move around on the host. `::` goes up one folder but never above the game folder (`dirNFErr`). This closes Plan 3's deferred "'..' escapes the folder" and "'::' crashes".
- **No writable folder means writes fail cleanly** with `wrPermErr`, logged once. Reading still works.
- **Dialogs draw onto the emulated screen,** with no windows of their own. They save the pixels under the dialog and its 4-pixel frame, and put them back on close, unless the screen changed size or depth meanwhile.
- **The font is SDL's built-in debug font,** rasterized once at startup with SDL's software renderer, so no font data goes in the repo. Mac Roman text is spelled in ASCII (`é`→`e`, `™`→`TM`). Lines are 12 pixels apart, and text wraps between words at its item's width.
- **Keys in a dialog:**
  - Return or Enter chooses the default button: the ALRT's stage bits pick it for alerts; for a DLOG it's item 1 if that is a button, otherwise the first button. On a Mac, item 1 would be returned even when it's a picture; choosing Register is what the user means.
  - Esc and Cmd-. choose a button titled "Cancel". Nothing in Alert 901 is titled that, so Esc can't quit the game by accident.
  - Tab and Shift-Tab move between edit fields, and Delete (backspace) erases.
  - Printable ASCII is typed. Cmd-V and scripted `type` insert printable ASCII only.
- **`ModalDialog` returns only for enabled buttons** (and check boxes and radio buttons); typing is handled inside. The game ignores every item but 3 and 4.
- **No game timers fire while a dialog is open.** This matches the shim's rule that timers fire only from the game's event loop, and every dialog in this game appears outside it.
- **Filter procs and caller-supplied dialog storage fail loudly.** The game passes neither.
- **`LOONY_AUTO_ALERTS=1`** answers alerts with their default item at once, without drawing: Plan 3's behavior, kept so the approved golden frames don't move. `ModalDialog` crashes under it, since a form can't be answered automatically.
- **Text items get a guest handle holding their text,** kept current as the user types (a new host-side `mm_set_handle_size`). `GetDialogItemText` reads any handle, as on a Mac.
- **The pointer shows whenever the game hasn't hidden it, or a dialog is open.** Until now it always showed.
- **QuickTime mattes are honored.** Only an 8-bit gray 'rle ' matte is decoded, because that's all the game has. The image after it is drawn only where the matte is at least half black. Without this, the icon's transparent area shows magenta stripes. `CompressedQuickTime` (0x8200) is skipped: its picture repeats the image as QuickDraw.
- **Direct-to-indexed copies pick the nearest palette color.** This closes Plan 3's deferred "16/32-bit to indexed conversion is unsupported". ditherCopy draws as srcCopy.
- **`ICLaunchURL` opens only http and https URLs,** through `/usr/bin/open`. Tests substitute the opener, so no test launches a browser.
- **`num2dec`** uses the host's exact `printf` conversions. 'I' means infinity, 'N' a NaN, and '?' digits that don't fit in 36.

## Review Focus

1. **Untrusted input from the game's resources and memory.** The PICT, QuickTime RLE, DITL, DLOG and ALRT parsing, and guest-supplied lengths in `FSWrite`, `ICLaunchURL` and `GetDialogItemText`. Expect every read and write bounds-checked, and a malformed resource to fail loudly or be skipped, never to corrupt memory. Read `pict.c` and `dialogs.c`.
2. **The game folder is never written.** Expect every `fopen` for writing, and every `ftruncate`, to name a path under the data folder. Copy-on-write, `FSpCreate` and the `.`/`..` checks are in `files.c`. Tests in Task 2.
3. **Dialogs restore exactly what they covered,** at any screen depth, and dispose of their handles. Tests in Task 6.
4. **Preferences survive a quit and a relaunch.** Reference counts must stay right across load, save and set, and a damaged file must be set aside. Tests in Tasks 1 and 6.
5. **Nothing secret is logged.** Typed text, the e-mail address and the key code never appear in output. Tests in Task 6.

---
### Task 1: Preferences saved to a file

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `src/cf.c`
- Modify: `src/cf.h`
- Modify: `src/files.c`
- Modify: `src/files.h`
- Modify: `src/main.c`
- Create: `src/plist.c`
- Create: `src/plist.h`
- Modify: `src/util.c`
- Modify: `src/util.h`
- Modify: `tests/test.h`
- Modify: `tests/test_cf.c`
- Modify: `tests/test_main.c`
- Modify: `tests/test_run.c`

**Interfaces:**
- Produces: `plist_read`/`plist_write`/`plist_free` (`plist.h`), `make_dirs` (`util.h`), `cf_load_prefs`/`cf_save_prefs` (`cf.h`), `files_data_dir` (`files.h`), `test_tmp_dir`/`test_remove_tree` (`tests/test.h`).
- Changes: `CFPreferencesAppSynchronize` writes the file named by `cf_load_prefs`; `main` loads `<data folder>/prefs.plist` at startup; the test runner sets `LOONY_DATA_DIR`, and each scripted run gets a fresh folder.

- [ ] **Step 1: Write the failing tests**

```diff
diff --git a/tests/test.h b/tests/test.h
index d9985b2..43649d8 100644
--- a/tests/test.h
+++ b/tests/test.h
@@ -16,6 +16,12 @@ const char *test_game_dir(void);
 const char *test_game_exe_path(void);
 bool test_game_present(void);
 
+/* Creates a fresh, empty folder under $TMPDIR and writes its path to out. */
+void test_tmp_dir(char *out, size_t cap);
+
+/* Deletes a folder and everything in it. */
+void test_remove_tree(const char *path);
+
 /* Runs fn(arg) in a forked child with its stderr captured into out
    (NUL-terminated, truncated to outlen - 1 bytes). Returns the child's exit
    status, or -1 if it was killed by a signal. The child exits 0 if fn returns. */
diff --git a/tests/test_cf.c b/tests/test_cf.c
index bca6834..a164c86 100644
--- a/tests/test_cf.c
+++ b/tests/test_cf.c
@@ -1,5 +1,8 @@
 #include "test.h"
 
+#include <stdlib.h>
+#include <unistd.h>
+
 #include "cf.h"
 #include "harness.h"
 
@@ -211,3 +214,151 @@ TEST(cf_prefs_over_released_number_crashes_with_a_report) {
     CHECK_EQ(status, 2);
     CHECK_CONTAINS(out, "loony: crash: CFPreferencesGetAppIntegerValue: 0x08000");
 }
+
+/* ---- the preferences file ---- */
+
+static char prefs_dir[1024], prefs_path[1100];
+
+static void file_setup(void) {
+    setup();
+    test_tmp_dir(prefs_dir, sizeof prefs_dir);
+    snprintf(prefs_path, sizeof prefs_path, "%s/sub/folder/prefs.plist", prefs_dir);
+    cf_load_prefs(prefs_path);
+}
+
+static void set_str(const char *key, const char *value) {
+    uint32_t k = str(key), v = str(value);
+    call_import("CFPreferencesSetAppValue", 3, k, v, app());
+    call_import("CFRelease", 1, v);
+    call_import("CFRelease", 1, k);
+}
+
+static void get_str(const char *key, char *out, size_t cap) {
+    out[0] = '\0';
+    uint32_t v = call_import("CFPreferencesCopyAppValue", 2, str(key), app());
+    if (!v)
+        return;
+    uint32_t buf = scratch((uint32_t)cap);
+    call_import("CFStringGetCString", 4, v, buf, (uint32_t)cap, 0u);
+    gm_read_cstr(buf, out, cap);
+    call_import("CFRelease", 1, v);
+}
+
+static void write_text(const char *path, const char *text) {
+    FILE *f = fopen(path, "w");
+    fputs(text, f);
+    fclose(f);
+}
+
+TEST(cf_prefs_survive_synchronize_and_reload) {
+    file_setup();
+    set_str("highscore name 1", "CAF\x8e"); /* Mac Roman e-acute */
+    uint32_t k = str("highscore 1");
+    call_import("CFPreferencesSetAppValue", 3, k, num(123456), app());
+    set_str("gone", "x");
+    call_import("CFPreferencesSetAppValue", 3, str("gone"), 0u, app());
+    CHECK_EQ(call_import("CFPreferencesAppSynchronize", 1, app()), 1);
+
+    size_t len;
+    char *xml = (char *)read_file(prefs_path, &len);
+    CHECK(xml != NULL);
+    xml = realloc(xml, len + 1);
+    xml[len] = '\0';
+    CHECK_CONTAINS(xml, "<key>highscore name 1</key>");
+    CHECK_CONTAINS(xml, "<string>CAF\xc3\xa9</string>"); /* UTF-8 in the file */
+    CHECK_CONTAINS(xml, "<integer>123456</integer>");
+    CHECK(!strstr(xml, "gone"));
+    free(xml);
+
+    cf_init();
+    cf_load_prefs(prefs_path);
+    char name[64];
+    get_str("highscore name 1", name, sizeof name);
+    CHECK_STR(name, "CAF\x8e");
+    uint32_t valid = scratch(1);
+    CHECK_EQ(call_import("CFPreferencesGetAppIntegerValue", 3, str("highscore 1"), app(), valid), 123456);
+    CHECK_EQ(gm_r8(valid), 1);
+    test_remove_tree(prefs_dir);
+}
+
+TEST(cf_prefs_start_empty_without_a_file) {
+    file_setup();
+    char name[64];
+    get_str("highscore name 1", name, sizeof name);
+    CHECK_STR(name, "");
+    CHECK_EQ(cf_live_objects(), 1 + 1); /* the application ID and the key just made */
+    test_remove_tree(prefs_dir);
+}
+
+static void child_bad_file(void *unused) {
+    (void)unused;
+    setup();
+    cf_load_prefs(prefs_path);
+}
+
+/* Review Focus 1: a damaged preferences file is kept, not overwritten. */
+TEST(cf_prefs_set_a_damaged_file_aside) {
+    setup();
+    test_tmp_dir(prefs_dir, sizeof prefs_dir);
+    snprintf(prefs_path, sizeof prefs_path, "%s/sub/folder/prefs.plist", prefs_dir);
+    char sub[1100];
+    snprintf(sub, sizeof sub, "%s/sub/folder", prefs_dir);
+    CHECK(make_dirs(sub));
+    write_text(prefs_path, "this is not a plist");
+    char out[16384];
+    CHECK_EQ(test_run_child(child_bad_file, NULL, out, sizeof out), 0);
+    CHECK_CONTAINS(out, "is not a property list dictionary; moved it to");
+    char bad[1200];
+    snprintf(bad, sizeof bad, "%s.bad", prefs_path);
+    size_t len;
+    uint8_t *kept = read_file(bad, &len);
+    CHECK(kept != NULL);
+    CHECK_EQ(len, strlen("this is not a plist"));
+    free(kept);
+    CHECK(access(prefs_path, F_OK) != 0);
+    test_remove_tree(prefs_dir);
+}
+
+static void child_odd_values(void *unused) {
+    (void)unused;
+    setup();
+    cf_load_prefs(prefs_path);
+    char v[64];
+    get_str("name", v, sizeof v);
+    if (strcmp(v, "MIO") != 0)
+        exit(3);
+    if (call_import("CFPreferencesCopyAppValue", 2, str("ratio"), app()) != 0)
+        exit(4);
+}
+
+TEST(cf_prefs_skip_values_that_arent_strings_or_integers) {
+    setup();
+    test_tmp_dir(prefs_dir, sizeof prefs_dir);
+    snprintf(prefs_path, sizeof prefs_path, "%s/sub/folder/prefs.plist", prefs_dir);
+    char sub[1100];
+    snprintf(sub, sizeof sub, "%s/sub/folder", prefs_dir);
+    CHECK(make_dirs(sub));
+    write_text(prefs_path, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>"
+                           "<key>name</key><string>MIO</string><key>ratio</key><real>1.5</real>"
+                           "<key>on</key><true/></dict></plist>\n");
+    char out[16384];
+    CHECK_EQ(test_run_child(child_odd_values, NULL, out, sizeof out), 0);
+    CHECK_CONTAINS(out, "skipped \"ratio\" (not a string or an integer)");
+    CHECK_CONTAINS(out, "skipped \"on\"");
+    test_remove_tree(prefs_dir);
+}
+
+static void child_unwritable(void *unused) {
+    (void)unused;
+    setup();
+    cf_load_prefs("/dev/null/loony/prefs.plist");
+    set_str("a", "b");
+    if (call_import("CFPreferencesAppSynchronize", 1, app()) != 0)
+        exit(3);
+}
+
+TEST(cf_prefs_synchronize_reports_a_write_failure) {
+    char out[16384];
+    CHECK_EQ(test_run_child(child_unwritable, NULL, out, sizeof out), 0);
+    CHECK_CONTAINS(out, "loony: preferences: can't create the folder for /dev/null/loony/prefs.plist");
+}
diff --git a/tests/test_main.c b/tests/test_main.c
index fa31d40..85d72f6 100644
--- a/tests/test_main.c
+++ b/tests/test_main.c
@@ -48,6 +48,22 @@ bool test_game_present(void) {
     return access(test_game_exe_path(), R_OK) == 0;
 }
 
+void test_tmp_dir(char *out, size_t cap) {
+    const char *t = getenv("TMPDIR");
+    snprintf(out, cap, "%s/loony-test-XXXXXX", t && *t ? t : "/tmp");
+    if (!mkdtemp(out)) {
+        fprintf(stderr, "mkdtemp %s failed\n", out);
+        exit(1);
+    }
+}
+
+void test_remove_tree(const char *path) {
+    char cmd[1200];
+    snprintf(cmd, sizeof cmd, "rm -rf '%s'", path);
+    if (system(cmd) != 0)
+        fprintf(stderr, "can't remove %s\n", path);
+}
+
 int test_run_child(void (*fn)(void *), void *arg, char *out, size_t outlen) {
     int fds[2];
     if (pipe(fds) != 0)
@@ -86,6 +102,10 @@ int main(int argc, char **argv) {
     /* No test may open a real window or play sound; children inherit this too. */
     setenv("SDL_VIDEO_DRIVER", "dummy", 1);
     setenv("SDL_AUDIO_DRIVER", "dummy", 1);
+    /* Nor touch the real preferences in ~/Library/Application Support. */
+    char data_dir[1024];
+    test_tmp_dir(data_dir, sizeof data_dir);
+    setenv("LOONY_DATA_DIR", data_dir, 1);
     const char *filter = argc > 1 ? argv[1] : NULL;
     int passed = 0, failed = 0, skipped = 0;
     for (int i = 0; i < ntests; i++) {
@@ -101,6 +121,7 @@ int main(int argc, char **argv) {
         else
             passed++;
     }
+    test_remove_tree(data_dir);
     fprintf(stderr, "\n%d passed, %d failed, %d skipped\n", passed, failed, skipped);
     return failed ? 1 : 0;
 }
diff --git a/tests/test_run.c b/tests/test_run.c
index 6558d59..b0a3664 100644
--- a/tests/test_run.c
+++ b/tests/test_run.c
@@ -5,7 +5,14 @@
 
 #include "util.h"
 
+/* LOONY_DATA_DIR for the runs that follow: each scripted run gets a fresh
+   one unless a test set this, so preferences saved by one run never change
+   another's frames. Otherwise the runner's own temporary folder is used. */
+static char run_data[1024];
+
 static void run_loony(void *dir) {
+    if (run_data[0])
+        setenv("LOONY_DATA_DIR", run_data, 1);
     execl(LOONY_BIN, "loony", (const char *)dir, (char *)NULL);
     fprintf(stderr, "exec %s failed\n", LOONY_BIN);
     _exit(127);
@@ -27,7 +34,10 @@ TEST(run_plays_the_opening_headless) {
     CHECK(fd >= 0);
     close(fd);
     char out[32768];
+    test_tmp_dir(run_data, sizeof run_data);
     int status = test_run_child(run_loony_headless, (void *)test_game_dir(), out, sizeof out);
+    test_remove_tree(run_data);
+    run_data[0] = '\0';
     size_t len = 0;
     uint8_t *png = read_file(shot, &len);
     unlink(shot);
@@ -99,7 +109,14 @@ static int run_script(const char *actions, const uint32_t *ticks, uint32_t *shot
     for (int i = 0; i < n; i++)
         fprintf(f, "%s\n", order[i]);
     fclose(f);
+    bool own_data = !run_data[0];
+    if (own_data)
+        test_tmp_dir(run_data, sizeof run_data);
     int status = test_run_child(run_loony_scripted, (void *)test_game_dir(), out, outlen);
+    if (own_data) {
+        test_remove_tree(run_data);
+        run_data[0] = '\0';
+    }
     for (int i = 0; i < nshots; i++) {
         size_t len = 0;
         uint8_t *png = read_file(pngs[i], &len);
@@ -173,6 +190,63 @@ TEST(run_the_opening_music_is_recorded) {
     free(wav);
 }
 
+static char *read_text(const char *path) {
+    size_t len = 0;
+    char *t = (char *)read_file(path, &len);
+    if (!t)
+        return NULL;
+    t = realloc(t, len + 1);
+    t[len] = '\0';
+    return t;
+}
+
+/* Spec success criterion 5: what the game keeps in its preferences (the
+   high-score table, the options, the license) is saved when it quits and
+   read at the next launch. The game rejects a high-score table it didn't
+   write (it checks "highscore id"), so this test changes an option instead:
+   with "switch music" off, the opening is silent. */
+TEST(run_preferences_are_saved_at_quit_and_read_at_launch) {
+    SKIP_UNLESS_GAME();
+    test_tmp_dir(run_data, sizeof run_data);
+    char prefs[1100];
+    snprintf(prefs, sizeof prefs, "%s/prefs.plist", run_data);
+    char out[32768];
+    int status = run_script("300 quit\n", NULL, NULL, 0, out, sizeof out);
+    char *xml = read_text(prefs);
+    CHECK_EQ(status, 0);
+    CHECK(xml != NULL);
+    CHECK_CONTAINS(xml, "<key>highscore name 1</key>");
+    CHECK_CONTAINS(xml, "<string>SNOWMAN</string>");
+    const char *music_on = "<key>switch music</key>\n\t<integer>1</integer>";
+    char *at = strstr(xml, music_on);
+    CHECK(at != NULL);
+    at[strlen(music_on) - strlen("1</integer>")] = '0';
+    FILE *f = fopen(prefs, "w");
+    fputs(xml, f);
+    fclose(f);
+    free(xml);
+
+    tmp_name(wav_path, sizeof wav_path, "wav");
+    setenv("LOONY_EXIT_AFTER", "300", 1);
+    status = run_script("", NULL, NULL, 0, out, sizeof out);
+    unsetenv("LOONY_EXIT_AFTER");
+    size_t len = 0;
+    uint8_t *wav = read_file(wav_path, &len);
+    unlink(wav_path);
+    wav_path[0] = '\0';
+    test_remove_tree(run_data);
+    run_data[0] = '\0';
+    CHECK_EQ(status, 0);
+    CHECK(!strstr(out, "preferences:"));
+    CHECK(wav != NULL);
+    CHECK_EQ(len, 44 + 300 * 44100 / 60 * 4);
+    bool silent = true;
+    for (size_t i = 44; i < len; i++)
+        silent = silent && wav[i] == 0;
+    CHECK(silent);
+    free(wav);
+}
+
 static void run_loony_bad_script(void *dir) {
     setenv("LOONY_SCRIPT", "/nonexistent/loony.script", 1);
     run_loony(dir);
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build`
Expected: the new `cf_prefs_*` tests and `run_preferences_are_saved_at_quit_and_read_at_launch` fail to build (no `cf_load_prefs`, `test_tmp_dir`).

- [ ] **Step 3: Implement**

```diff
diff --git a/CMakeLists.txt b/CMakeLists.txt
index e409e46..6e2612b 100644
--- a/CMakeLists.txt
+++ b/CMakeLists.txt
@@ -23,7 +23,7 @@ list(REMOVE_ITEM CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/main.c)
 
 add_library(loony_core STATIC ${CORE_SOURCES})
 target_include_directories(loony_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
-target_link_libraries(loony_core PUBLIC PkgConfig::UNICORN PkgConfig::SDL3)
+target_link_libraries(loony_core PUBLIC PkgConfig::UNICORN PkgConfig::SDL3 "-framework CoreFoundation")
 
 add_executable(loony src/main.c)
 target_link_libraries(loony PRIVATE loony_core)
diff --git a/src/cf.c b/src/cf.c
index bc93ddf..e06a771 100644
--- a/src/cf.c
+++ b/src/cf.c
@@ -1,10 +1,12 @@
 #include "cf.h"
 
 #include <errno.h>
+#include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 
 #include "guest_mem.h"
+#include "plist.h"
 #include "trap.h"
 #include "util.h"
 
@@ -30,6 +32,7 @@ static struct {
     pref *prefs;
     uint32_t nprefs, prefs_cap;
     uint32_t current_app;
+    char *prefs_path; /* NULL: preferences are never saved */
 } C;
 
 static uint32_t ref_of(uint32_t index) { return CF_TAG_BASE + 16u * index; }
@@ -83,6 +86,7 @@ void cf_init(void) {
         free(C.prefs[i].key);
     free(C.objs);
     free(C.prefs);
+    free(C.prefs_path);
     memset(&C, 0, sizeof C);
     C.current_app = cf_string("com.littlewing.loonylabyrinth");
 }
@@ -195,13 +199,11 @@ static pref *find_pref(const char *key) {
     return NULL;
 }
 
-/* CFPreferencesSetAppValue(key, value, appID). A NULL value removes the key. */
-static void h_prefs_set_app_value(void) {
-    const char *key = key_string("CFPreferencesSetAppValue", trap_arg(0));
-    uint32_t value = trap_arg(1);
-    need_current_app("CFPreferencesSetAppValue", trap_arg(2));
+/* Sets key to value, which the preferences now hold a reference to (the
+   caller's reference is unchanged). A value of 0 removes the key. */
+static void set_pref(const char *key, uint32_t value) {
     if (value)
-        need("CFPreferencesSetAppValue", value)->refs++;
+        lookup(value)->refs++;
     pref *p = find_pref(key);
     if (p) {
         release(&C.objs[(p->value - CF_TAG_BASE) / 16u]);
@@ -227,6 +229,16 @@ static void h_prefs_set_app_value(void) {
     C.prefs[C.nprefs++] = (pref){copy, value};
 }
 
+/* CFPreferencesSetAppValue(key, value, appID). A NULL value removes the key. */
+static void h_prefs_set_app_value(void) {
+    const char *key = key_string("CFPreferencesSetAppValue", trap_arg(0));
+    uint32_t value = trap_arg(1);
+    need_current_app("CFPreferencesSetAppValue", trap_arg(2));
+    if (value)
+        need("CFPreferencesSetAppValue", value);
+    set_pref(key, value);
+}
+
 /* CFPreferencesCopyAppValue(key, appID): a retained value, or NULL. */
 static void h_prefs_copy_app_value(void) {
     const char *key = key_string("CFPreferencesCopyAppValue", trap_arg(0));
@@ -269,10 +281,59 @@ static void h_prefs_get_app_integer_value(void) {
     trap_return(valid ? (uint32_t)(int32_t)v : 0);
 }
 
-/* CFPreferencesAppSynchronize(appID) -> Boolean. In memory only, so always true. */
+/* CFPreferencesAppSynchronize(appID) -> Boolean: writes the preferences file. */
 static void h_prefs_app_synchronize(void) {
     need_current_app("CFPreferencesAppSynchronize", trap_arg(0));
-    trap_return(1);
+    trap_return(cf_save_prefs());
+}
+
+/* ---- the preferences file ---- */
+
+void cf_load_prefs(const char *path) {
+    free(C.prefs_path);
+    C.prefs_path = strdup(path);
+    if (!C.prefs_path)
+        fatal("out of memory");
+    plist_entry *e;
+    uint32_t n;
+    char err[512];
+    plist_status st = plist_read(path, &e, &n, err, sizeof err);
+    if (st == PLIST_BAD) {
+        char bad[1100];
+        snprintf(bad, sizeof bad, "%s.bad", path);
+        rename(path, bad);
+        log_msg("preferences: %s; moved it to %s and starting with none", err, bad);
+        return;
+    }
+    if (err[0])
+        log_msg("preferences: %s: %s", path, err);
+    for (uint32_t i = 0; i < n; i++) {
+        uint32_t v = e[i].is_number ? new_obj(CF_NUMBER_TYPE_ID, NULL, e[i].num) : cf_string(e[i].str);
+        set_pref(e[i].key, v);
+        release(lookup(v));
+    }
+    plist_free(e, n);
+}
+
+bool cf_save_prefs(void) {
+    if (!C.prefs_path)
+        return true;
+    plist_entry *e = calloc(C.nprefs + 1, sizeof *e);
+    if (!e)
+        fatal("out of memory");
+    for (uint32_t i = 0; i < C.nprefs; i++) {
+        cf_obj *o = need("CFPreferencesAppSynchronize", C.prefs[i].value);
+        e[i].key = C.prefs[i].key;
+        e[i].is_number = o->type_id == CF_NUMBER_TYPE_ID;
+        e[i].num = o->num;
+        e[i].str = o->str;
+    }
+    char err[512];
+    bool ok = plist_write(C.prefs_path, e, C.nprefs, err, sizeof err);
+    free(e); /* the strings belong to the preferences */
+    if (!ok)
+        log_msg("preferences: %s", err);
+    return ok;
 }
 
 void cf_register(void) {
diff --git a/src/cf.h b/src/cf.h
index 42cd3eb..718e66d 100644
--- a/src/cf.h
+++ b/src/cf.h
@@ -4,8 +4,10 @@
 
 /* Core Foundation subset: CFString and CFNumber objects and CFPreferences.
    Objects live in host memory; the guest sees opaque IDs in tag space
-   (CF_TAG_BASE + 16 * index), which it never dereferences. Preferences are
-   kept in memory only (saved to disk in a later milestone). */
+   (CF_TAG_BASE + 16 * index), which it never dereferences. Preferences live
+   in memory; with a preferences file (cf_load_prefs) they are read from it
+   at startup and written back by CFPreferencesAppSynchronize, which the game
+   calls as it quits. */
 
 #define CF_TAG_BASE       0x08000000u
 #define CF_TAG_LIMIT      0x09000000u
@@ -16,6 +18,17 @@
    kCFPreferencesCurrentApplication refers to. */
 void cf_init(void);
 
+/* Loads the preferences in path (a property list of strings and integers)
+   and remembers path for CFPreferencesAppSynchronize. A missing file means
+   no preferences yet. A file that can't be read is logged and renamed to
+   path.bad, so the next synchronize doesn't destroy it, and the game starts
+   with no preferences. */
+void cf_load_prefs(const char *path);
+
+/* Writes the preferences to the file named by cf_load_prefs. False (logged)
+   if that fails. True, doing nothing, if there's no file. */
+bool cf_save_prefs(void);
+
 /* The CFStringRef stored in the kCFPreferencesCurrentApplication data import. */
 uint32_t cf_current_app(void);
 
diff --git a/src/files.c b/src/files.c
index 6b9745d..a3db2f6 100644
--- a/src/files.c
+++ b/src/files.c
@@ -2,6 +2,7 @@
 
 #include <errno.h>
 #include <stdio.h>
+#include <stdlib.h>
 #include <string.h>
 #include <sys/stat.h>
 
@@ -73,6 +74,17 @@ void files_mac_to_utf8(const char *mac, char *out, size_t cap) {
     out[o] = '\0';
 }
 
+bool files_data_dir(char *out, size_t cap) {
+    const char *d = getenv("LOONY_DATA_DIR"), *home = getenv("HOME");
+    if (d && *d)
+        snprintf(out, cap, "%s", d);
+    else if (home && *home)
+        snprintf(out, cap, "%s/Library/Application Support/loony-shim", home);
+    else
+        return false;
+    return true;
+}
+
 void files_init(const char *game_dir) {
     for (int i = 0; i < MAX_FILES; i++)
         if (F.files[i].f)
diff --git a/src/files.h b/src/files.h
index a9673a1..a1464d7 100644
--- a/src/files.h
+++ b/src/files.h
@@ -24,6 +24,10 @@
 /* FSSpec: vRefNum (2), parID (4), name (Str63, 64 bytes). */
 #define FSSPEC_SIZE 70
 
+/* The writable folder: $LOONY_DATA_DIR, or ~/Library/Application
+   Support/loony-shim. False if neither LOONY_DATA_DIR nor HOME is set. */
+bool files_data_dir(char *out, size_t cap);
+
 /* Sets the game folder (read-only). Closes open files and forgets directory IDs. */
 void files_init(const char *game_dir);
 
diff --git a/src/main.c b/src/main.c
index c757096..7ff09c3 100644
--- a/src/main.c
+++ b/src/main.c
@@ -64,6 +64,14 @@ int main(int argc, char **argv) {
     mm_init();
     misc_init();
     cf_init();
+    char data_dir[PATH_MAX];
+    if (files_data_dir(data_dir, sizeof data_dir)) {
+        char prefs[PATH_MAX + 16];
+        snprintf(prefs, sizeof prefs, "%s/prefs.plist", data_dir);
+        cf_load_prefs(prefs);
+    } else {
+        log_msg("neither LOONY_DATA_DIR nor HOME is set: preferences won't be saved");
+    }
     qd_init(800, 600, 8);
     dialogs_init();
     events_init();
diff --git a/src/plist.c b/src/plist.c
new file mode 100644
index 0000000..4ef17fb
--- /dev/null
+++ b/src/plist.c
@@ -0,0 +1,140 @@
+#include "plist.h"
+
+#include <CoreFoundation/CoreFoundation.h>
+#include <errno.h>
+#include <libgen.h>
+#include <stdio.h>
+#include <stdlib.h>
+#include <string.h>
+
+#include "util.h"
+
+/* A malloc'd Mac Roman copy of s; characters Mac Roman lacks become '?'. */
+static char *mac_roman(CFStringRef s) {
+    CFRange all = CFRangeMake(0, CFStringGetLength(s));
+    CFIndex n = 0;
+    CFStringGetBytes(s, all, kCFStringEncodingMacRoman, '?', false, NULL, 0, &n);
+    char *out = malloc((size_t)n + 1);
+    if (!out)
+        fatal("out of memory");
+    CFStringGetBytes(s, all, kCFStringEncodingMacRoman, '?', false, (UInt8 *)out, n, NULL);
+    out[n] = '\0';
+    return out;
+}
+
+static CFStringRef cf_str(const char *mac) {
+    CFStringRef s = CFStringCreateWithBytes(NULL, (const UInt8 *)mac, (CFIndex)strlen(mac),
+                                            kCFStringEncodingMacRoman, false);
+    if (!s)
+        fatal("out of memory");
+    return s;
+}
+
+plist_status plist_read(const char *path, plist_entry **out, uint32_t *n, char *err, size_t errlen) {
+    *out = NULL;
+    *n = 0;
+    err[0] = '\0';
+    size_t len;
+    uint8_t *buf = read_file(path, &len);
+    if (!buf) {
+        if (errno == ENOENT)
+            return PLIST_MISSING;
+        snprintf(err, errlen, "can't read %s: %s", path, strerror(errno));
+        return PLIST_BAD;
+    }
+    CFDataRef data = CFDataCreate(NULL, buf, (CFIndex)len);
+    free(buf);
+    if (!data)
+        fatal("out of memory");
+    CFPropertyListRef root = CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL);
+    CFRelease(data);
+    if (!root || CFGetTypeID(root) != CFDictionaryGetTypeID()) {
+        if (root)
+            CFRelease(root);
+        snprintf(err, errlen, "%s is not a property list dictionary", path);
+        return PLIST_BAD;
+    }
+    CFIndex count = CFDictionaryGetCount(root);
+    const void **keys = calloc((size_t)count + 1, sizeof *keys);
+    const void **values = calloc((size_t)count + 1, sizeof *values);
+    plist_entry *e = calloc((size_t)count + 1, sizeof *e);
+    if (!keys || !values || !e)
+        fatal("out of memory");
+    CFDictionaryGetKeysAndValues(root, keys, values);
+    uint32_t m = 0;
+    for (CFIndex i = 0; i < count; i++) {
+        bool is_str = CFGetTypeID(values[i]) == CFStringGetTypeID();
+        bool is_num = CFGetTypeID(values[i]) == CFNumberGetTypeID() && !CFNumberIsFloatType(values[i]);
+        if (CFGetTypeID(keys[i]) != CFStringGetTypeID() || (!is_str && !is_num)) {
+            char *k = CFGetTypeID(keys[i]) == CFStringGetTypeID() ? mac_roman(keys[i]) : NULL;
+            size_t used = strlen(err);
+            snprintf(err + used, errlen - used, "%sskipped \"%s\" (not a string or an integer)",
+                     used ? "; " : "", k ? k : "?");
+            free(k);
+            continue;
+        }
+        e[m].key = mac_roman(keys[i]);
+        e[m].is_number = is_num;
+        if (is_num) {
+            SInt64 v;
+            CFNumberGetValue(values[i], kCFNumberSInt64Type, &v);
+            e[m].num = v;
+        } else {
+            e[m].str = mac_roman(values[i]);
+        }
+        m++;
+    }
+    free(keys);
+    free(values);
+    CFRelease(root);
+    *out = e;
+    *n = m;
+    return PLIST_OK;
+}
+
+bool plist_write(const char *path, const plist_entry *e, uint32_t n, char *err, size_t errlen) {
+    char dir[1024];
+    snprintf(dir, sizeof dir, "%s", path);
+    if (!make_dirs(dirname(dir))) {
+        snprintf(err, errlen, "can't create the folder for %s: %s", path, strerror(errno));
+        return false;
+    }
+    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, n, &kCFTypeDictionaryKeyCallBacks,
+                                                         &kCFTypeDictionaryValueCallBacks);
+    for (uint32_t i = 0; i < n; i++) {
+        CFStringRef k = cf_str(e[i].key);
+        CFTypeRef v = e[i].is_number ? (CFTypeRef)CFNumberCreate(NULL, kCFNumberSInt64Type, &e[i].num)
+                                     : (CFTypeRef)cf_str(e[i].str);
+        CFDictionarySetValue(d, k, v);
+        CFRelease(k);
+        CFRelease(v);
+    }
+    CFDataRef data = CFPropertyListCreateData(NULL, d, kCFPropertyListXMLFormat_v1_0, 0, NULL);
+    CFRelease(d);
+    if (!data) {
+        snprintf(err, errlen, "can't encode %s", path);
+        return false;
+    }
+    char tmp[1100];
+    snprintf(tmp, sizeof tmp, "%s.tmp", path);
+    FILE *f = fopen(tmp, "wb");
+    size_t len = (size_t)CFDataGetLength(data);
+    bool ok = f && fwrite(CFDataGetBytePtr(data), 1, len, f) == len;
+    if (f && fclose(f) != 0)
+        ok = false;
+    CFRelease(data);
+    if (!ok || rename(tmp, path) != 0) {
+        snprintf(err, errlen, "can't write %s: %s", path, strerror(errno));
+        remove(tmp);
+        return false;
+    }
+    return true;
+}
+
+void plist_free(plist_entry *e, uint32_t n) {
+    for (uint32_t i = 0; i < n; i++) {
+        free(e[i].key);
+        free(e[i].str);
+    }
+    free(e);
+}
diff --git a/src/plist.h b/src/plist.h
new file mode 100644
index 0000000..460b21e
--- /dev/null
+++ b/src/plist.h
@@ -0,0 +1,30 @@
+#pragma once
+#include <stdbool.h>
+#include <stddef.h>
+#include <stdint.h>
+
+/* Property list files holding one dictionary of string and integer values,
+   read and written with the host's CoreFoundation. Keys and strings are Mac
+   Roman bytes here and Unicode in the file, so the file reads normally in a
+   text editor, `plutil` or `defaults`. */
+
+typedef struct {
+    char *key;
+    bool is_number;
+    int64_t num;
+    char *str; /* when !is_number */
+} plist_entry;
+
+typedef enum { PLIST_OK, PLIST_MISSING, PLIST_BAD } plist_status;
+
+/* Reads path into a malloc'd array (free with plist_free). PLIST_MISSING if
+   the file doesn't exist; PLIST_BAD, with err set, if it can't be read or
+   isn't a dictionary. Values that aren't strings or integers are skipped and
+   named in err (which is "" otherwise). */
+plist_status plist_read(const char *path, plist_entry **out, uint32_t *n, char *err, size_t errlen);
+
+/* Writes the entries as an XML property list, replacing path atomically
+   (through path.tmp and a rename). Creates missing parent folders. */
+bool plist_write(const char *path, const plist_entry *e, uint32_t n, char *err, size_t errlen);
+
+void plist_free(plist_entry *e, uint32_t n);
diff --git a/src/util.c b/src/util.c
index e3d2176..9a22e51 100644
--- a/src/util.c
+++ b/src/util.c
@@ -1,8 +1,11 @@
 #include "util.h"
 
+#include <errno.h>
 #include <stdarg.h>
 #include <stdio.h>
 #include <stdlib.h>
+#include <string.h>
+#include <sys/stat.h>
 
 void fatal(const char *fmt, ...) {
     va_list ap;
@@ -55,6 +58,33 @@ uint8_t *read_file(const char *path, size_t *len_out) {
     return buf;
 }
 
+bool make_dirs(const char *path) {
+    char p[1024];
+    if (snprintf(p, sizeof p, "%s", path) >= (int)sizeof p) {
+        errno = ENAMETOOLONG;
+        return false;
+    }
+    for (char *s = p + 1;; s++) {
+        if (*s != '/' && *s != '\0')
+            continue;
+        char c = *s;
+        *s = '\0';
+        if (mkdir(p, 0755) != 0 && errno != EEXIST)
+            return false;
+        *s = c;
+        if (!c)
+            break;
+    }
+    struct stat st;
+    if (stat(p, &st) != 0)
+        return false;
+    if (!S_ISDIR(st.st_mode)) {
+        errno = ENOTDIR;
+        return false;
+    }
+    return true;
+}
+
 uint32_t fnv1a32(const void *data, size_t len) {
     const uint8_t *p = data;
     uint32_t h = 0x811C9DC5u;
diff --git a/src/util.h b/src/util.h
index f8a17a3..5d40efb 100644
--- a/src/util.h
+++ b/src/util.h
@@ -1,4 +1,5 @@
 #pragma once
+#include <stdbool.h>
 #include <stddef.h>
 #include <stdint.h>
 
@@ -11,6 +12,10 @@ void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
 /* Reads a whole file into a malloc'd buffer. Returns NULL (errno set) on failure. */
 uint8_t *read_file(const char *path, size_t *len_out);
 
+/* Creates a folder and any missing parents, like mkdir -p. False (errno set)
+   on failure. */
+bool make_dirs(const char *path);
+
 uint32_t fnv1a32(const void *data, size_t len);
 
 /* A Mac four-character code, e.g. FOURCC('P','I','C','T'). */
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build build && ./build/loony_tests cf_ && ./build/loony_tests run_preferences && ./build/loony_tests`
Expected: all pass (the registration test skips without `LOONY_TEST_EMAIL`/`LOONY_TEST_KEY`); no sanitizer reports.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/cf.c src/cf.h src/files.c src/files.h src/main.c src/plist.c src/plist.h src/util.c src/util.h tests/test.h tests/test_cf.c tests/test_main.c tests/test_run.c
git commit -m "Preferences saved to prefs.plist in the writable folder"
```

---

### Task 2: Writing files through the data folder

**Files:**
- Modify: `src/files.c`
- Modify: `src/files.h`
- Modify: `src/main.c`
- Modify: `tests/test_files.c`

**Interfaces:**
- Changes: `files_init(game_dir, data_dir)` takes the writable folder (NULL: none).
- Produces: `FSpCreate`, `FSWrite`, `SetEOF`, `PBFlushFileSync`; `FILES_DUP_FN_ERR`, `FILES_WR_PERM_ERR`, `FILES_IO_ERR`.

- [ ] **Step 1: Write the failing tests**

```diff
diff --git a/tests/test_files.c b/tests/test_files.c
index 79fc3f9..83a807c 100644
--- a/tests/test_files.c
+++ b/tests/test_files.c
@@ -9,9 +9,10 @@
 
 static const char *const names[] = {
     "FSMakeFSSpec", "FSpOpenDF", "PBReadSync", "GetEOF", "SetFPos", "GetFPos", "FSClose",
+    "FSpCreate", "FSWrite", "SetEOF", "PBFlushFileSync",
 };
 
-static char dir[1024];
+static char dir[1024], data[1100];
 
 static void write_file(const char *rel, const char *text) {
     char p[1200];
@@ -21,27 +22,47 @@ static void write_file(const char *rel, const char *text) {
     fclose(f);
 }
 
-/* A fresh game folder: "LL Data/effect.bin" and "Café" (Mac Roman "Caf\x8e"). */
+/* A fresh game folder, "game", with "LL Data/effect.bin" and "Café" (Mac
+   Roman "Caf\x8e"), both read-only, and a data folder "data/sub" that
+   doesn't exist yet. */
 static void setup(void) {
-    const char *t = getenv("TMPDIR");
-    snprintf(dir, sizeof dir, "%s/loony-files-XXXXXX", t && *t ? t : "/tmp");
-    if (!mkdtemp(dir))
-        fatal("mkdtemp failed");
+    char root[1024];
+    test_tmp_dir(root, sizeof root);
+    snprintf(dir, sizeof dir, "%s/game", root);
+    snprintf(data, sizeof data, "%s/data/sub", root);
     char sub[1100];
     snprintf(sub, sizeof sub, "%s/LL Data", dir);
-    mkdir(sub, 0755);
+    if (!make_dirs(sub))
+        fatal("can't create %s", sub);
     write_file("LL Data/effect.bin", "hello world");
     write_file("Caf\xc3\xa9", "x");
+    char p[1200];
+    snprintf(p, sizeof p, "%s/LL Data/effect.bin", dir);
+    chmod(p, 0444);
     harness_init(names, sizeof names / sizeof names[0]);
-    files_init(dir);
+    files_init(dir, data);
     files_register();
 }
 
 static void teardown(void) {
-    char cmd[1200];
-    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
-    if (system(cmd) != 0)
-        fprintf(stderr, "can't remove %s\n", dir);
+    char root[1024];
+    snprintf(root, sizeof root, "%s", dir);
+    *strrchr(root, '/') = '\0';
+    test_remove_tree(root);
+}
+
+/* The contents of a host file ("" if it doesn't exist). */
+static const char *contents(const char *base, const char *rel) {
+    static char buf[256];
+    char p[1200];
+    snprintf(p, sizeof p, "%s/%s", base, rel);
+    buf[0] = '\0';
+    FILE *f = fopen(p, "rb");
+    if (f) {
+        buf[fread(buf, 1, sizeof buf - 1, f)] = '\0';
+        fclose(f);
+    }
+    return buf;
 }
 
 static int16_t make_spec(const char *mac, uint32_t spec) {
@@ -122,30 +143,182 @@ TEST(files_open_missing_file) {
     teardown();
 }
 
-static void child_open_for_writing(void *unused) {
+static uint16_t open_df(const char *mac, int perm) {
+    uint32_t spec = scratch(FSSPEC_SIZE), ref = scratch(2);
+    make_spec(mac, spec);
+    if (call_import("FSpOpenDF", 3, spec, (uint32_t)perm, ref) != 0)
+        return 0;
+    return gm_r16(ref);
+}
+
+static int16_t fs_write(uint16_t ref, const char *text) {
+    uint32_t count = scratch(4), buf = scratch((uint32_t)strlen(text) + 1);
+    gm_w32(count, (uint32_t)strlen(text));
+    memcpy(gm_ptr(buf, (uint32_t)strlen(text) + 1), text, strlen(text));
+    int16_t err = (int16_t)call_import("FSWrite", 3, (uint32_t)ref, count, buf);
+    if (!err && gm_r32(count) != strlen(text))
+        return 99;
+    return err;
+}
+
+static uint32_t eof_of(uint16_t ref) {
+    uint32_t eof = scratch(4);
+    call_import("GetEOF", 2, (uint32_t)ref, eof);
+    return gm_r32(eof);
+}
+
+/* Reads n bytes from offset off (fsFromStart). */
+static const char *read_at(uint16_t ref, uint32_t off, uint32_t n) {
+    static char out[64];
+    uint32_t pb = scratch(80), buf = scratch(64);
+    gm_w16(pb + 24, ref);
+    gm_w32(pb + 32, buf);
+    gm_w32(pb + 36, n);
+    gm_w16(pb + 44, 1);
+    gm_w32(pb + 46, off);
+    call_import("PBReadSync", 1, pb);
+    memcpy(out, gm_ptr(buf, 64), 63);
+    out[gm_r32(pb + 40)] = '\0';
+    return out;
+}
+
+TEST(files_create_write_and_read_back) {
+    setup();
+    uint32_t spec = scratch(FSSPEC_SIZE);
+    CHECK_EQ(make_spec(":LL Data:scores", spec), FILES_FNF_ERR);
+    CHECK_EQ(call_import("FSpCreate", 4, spec, 0x664C6F4Fu, 0x54455854u, 0u), 0);
+    CHECK_STR(contents(data, "LL Data/scores"), ""); /* created, in the data folder */
+    char p[1200];
+    snprintf(p, sizeof p, "%s/LL Data/scores", data);
+    CHECK(access(p, F_OK) == 0);
+    snprintf(p, sizeof p, "%s/LL Data/scores", dir);
+    CHECK(access(p, F_OK) != 0);
+    CHECK_EQ(make_spec(":LL Data:scores", spec), 0);
+    CHECK_EQ((int16_t)call_import("FSpCreate", 4, spec, 0u, 0u, 0u), FILES_DUP_FN_ERR);
+
+    uint16_t r = open_df(":LL Data:scores", 3);
+    CHECK(r != 0);
+    CHECK_EQ(fs_write(r, "abcdef"), 0);
+    CHECK_EQ(eof_of(r), 6);
+    CHECK_STR(read_at(r, 2, 3), "cde");
+    CHECK_EQ(fs_write(r, "XY"), 0); /* at the mark, after the read */
+    CHECK_STR(read_at(r, 0, 10), "abcdeXY");
+    CHECK_EQ(call_import("SetEOF", 2, (uint32_t)r, 2u), 0);
+    CHECK_EQ(eof_of(r), 2);
+    uint32_t pos = scratch(4);
+    call_import("GetFPos", 2, (uint32_t)r, pos);
+    CHECK_EQ(gm_r32(pos), 2);
+    CHECK_EQ(call_import("SetEOF", 2, (uint32_t)r, 4u), 0); /* extends with zeros */
+    CHECK_EQ(eof_of(r), 4);
+    uint32_t pb = scratch(80);
+    gm_w16(pb + 24, r);
+    CHECK_EQ(call_import("PBFlushFileSync", 1, pb), 0);
+    CHECK_EQ(call_import("FSClose", 1, (uint32_t)r), 0);
+    snprintf(p, sizeof p, "%s/LL Data/scores", data);
+    size_t len;
+    uint8_t *got = read_file(p, &len);
+    CHECK(got != NULL);
+    CHECK_EQ(len, 4);
+    CHECK(memcmp(got, "ab\0\0", 4) == 0);
+    free(got);
+    teardown();
+}
+
+/* Review Focus 2: the game folder is never modified. */
+TEST(files_the_first_write_copies_a_game_file) {
+    setup();
+    uint16_t r = open_df(":LL Data:effect.bin", 0); /* fsCurPerm */
+    CHECK(r != 0);
+    CHECK_STR(read_at(r, 0, 5), "hello");
+    CHECK_STR(contents(data, "LL Data/effect.bin"), ""); /* reading copies nothing */
+    CHECK_EQ(fs_write(r, " W"), 0);
+    CHECK_STR(read_at(r, 0, 20), "hello World");
+    CHECK_EQ(eof_of(r), 11);
+    CHECK_EQ(call_import("FSClose", 1, (uint32_t)r), 0);
+    CHECK_STR(contents(dir, "LL Data/effect.bin"), "hello world");
+    CHECK_STR(contents(data, "LL Data/effect.bin"), "hello World");
+    r = open_df(":LL Data:effect.bin", 1); /* the copy now hides the original */
+    CHECK_STR(read_at(r, 0, 20), "hello World");
+    call_import("FSClose", 1, (uint32_t)r);
+    teardown();
+}
+
+TEST(files_read_only_files_refuse_writes) {
+    setup();
+    uint16_t r = open_df(":LL Data:effect.bin", 1);
+    CHECK(r != 0);
+    CHECK_EQ(fs_write(r, "x"), FILES_WR_PERM_ERR);
+    CHECK_EQ((int16_t)call_import("SetEOF", 2, (uint32_t)r, 0u), FILES_WR_PERM_ERR);
+    CHECK_EQ(eof_of(r), 11);
+    CHECK_EQ(call_import("FSClose", 1, (uint32_t)r), 0);
+    char p[1200];
+    snprintf(p, sizeof p, "%s", data);
+    CHECK(access(p, F_OK) != 0); /* nothing was copied or created */
+    teardown();
+}
+
+/* The children below run on the parent's setup(), so the parent cleans up. */
+static void child_no_data_folder(void *unused) {
     (void)unused;
+    files_init(dir, NULL);
+    uint32_t spec = scratch(FSSPEC_SIZE);
+    make_spec("new", spec);
+    if ((int16_t)call_import("FSpCreate", 4, spec, 0u, 0u, 0u) != FILES_WR_PERM_ERR)
+        exit(3);
+    uint16_t r = open_df(":LL Data:effect.bin", 3);
+    if (!r || strcmp(read_at(r, 0, 5), "hello") != 0)
+        exit(4);
+    if (fs_write(r, "x") != FILES_WR_PERM_ERR)
+        exit(5);
+}
+
+TEST(files_without_a_writable_folder_writes_fail_cleanly) {
     setup();
-    uint32_t spec = scratch(FSSPEC_SIZE), ref = scratch(2);
-    make_spec(":LL Data:effect.bin", spec);
-    call_import("FSpOpenDF", 3, spec, 3u, ref);
+    char out[16384];
+    int status = test_run_child(child_no_data_folder, NULL, out, sizeof out);
+    teardown();
+    CHECK_EQ(status, 0);
+    CHECK_CONTAINS(out, "FSpCreate: there is no writable folder");
+    CHECK(!strstr(out, "FSWrite: there is no writable folder")); /* logged once */
 }
 
-TEST(files_writing_is_not_supported_yet) {
+/* Review Focus 3: names that would leave the game folder on the host. */
+TEST(files_dot_names_and_double_colons) {
+    setup();
+    uint32_t spec = scratch(FSSPEC_SIZE);
+    CHECK_EQ((int16_t)make_spec(":..:x", spec), FILES_BD_NAM_ERR);
+    CHECK_EQ((int16_t)make_spec("..", spec), FILES_BD_NAM_ERR);
+    CHECK_EQ((int16_t)make_spec("::x", spec), FILES_DIR_NF_ERR); /* above the game folder */
+    CHECK_EQ(make_spec(":LL Data::Caf\x8e", spec), 0);
+    CHECK_EQ(gm_r32(spec + 2), FILES_ROOT_DIRID);
+    CHECK_EQ((int16_t)make_spec(":", spec), FILES_BD_NAM_ERR);
+    teardown();
+}
+
+static void child_bad_permission(void *unused) {
+    (void)unused;
+    open_df(":LL Data:effect.bin", 9);
+}
+
+TEST(files_unknown_permissions_crash) {
+    setup();
     char out[16384];
-    CHECK_EQ(test_run_child(child_open_for_writing, NULL, out, sizeof out), 2);
-    CHECK_CONTAINS(out, "FSpOpenDF: opening files for writing (permission 3) is not supported yet");
+    int status = test_run_child(child_bad_permission, NULL, out, sizeof out);
     teardown();
+    CHECK_EQ(status, 2);
+    CHECK_CONTAINS(out, "FSpOpenDF: unknown permission 9");
 }
 
 static void child_full_path(void *unused) {
     (void)unused;
-    setup();
     make_spec("Macintosh HD:x", scratch(FSSPEC_SIZE));
 }
 
 TEST(files_full_paths_crash) {
+    setup();
     char out[16384];
-    CHECK_EQ(test_run_child(child_full_path, NULL, out, sizeof out), 2);
-    CHECK_CONTAINS(out, "full path names");
+    int status = test_run_child(child_full_path, NULL, out, sizeof out);
     teardown();
+    CHECK_EQ(status, 2);
+    CHECK_CONTAINS(out, "full path names");
 }
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build`
Expected: `tests/test_files.c` fails to build (`files_init` takes one argument; `FSpCreate` and friends are not in the import table).

- [ ] **Step 3: Implement**

```diff
diff --git a/src/files.c b/src/files.c
index a3db2f6..ca6951c 100644
--- a/src/files.c
+++ b/src/files.c
@@ -5,6 +5,7 @@
 #include <stdlib.h>
 #include <string.h>
 #include <sys/stat.h>
+#include <unistd.h>
 
 #include "guest_mem.h"
 #include "trap.h"
@@ -13,6 +14,9 @@
 #define MAX_DIRS 64
 #define MAX_FILES 16
 #define FIRST_REFNUM 20
+#define REL_CAP 1024  /* a path relative to the game folder */
+#define PATH_CAP 2100 /* a host path */
+#define FILES_PARAM_ERR (-50)
 
 /* ParamBlockRec (IOParam) offsets. */
 #define PB_RESULT     16
@@ -29,14 +33,21 @@
 #define FS_FROM_LEOF  2
 #define FS_FROM_MARK  3
 
+typedef struct {
+    FILE *f;
+    long eof;
+    bool writable; /* opened with a permission that allows writing */
+    bool in_data;  /* f is the copy in the data folder */
+    char rel[REL_CAP];
+} open_file;
+
 static struct {
     char game_dir[1024];
-    char dirs[MAX_DIRS][512]; /* relative host path of each directory ID - 2 ("" = game folder) */
+    char data_dir[1024]; /* "" = none */
+    char dirs[MAX_DIRS][REL_CAP]; /* relative host path of each directory ID - 2 ("" = game folder) */
     int ndirs;
-    struct {
-        FILE *f;
-        long eof;
-    } files[MAX_FILES];
+    open_file files[MAX_FILES];
+    bool warned_no_data;
 } F;
 
 /* Unicode code points for Mac Roman 0x80-0xFF. */
@@ -85,12 +96,14 @@ bool files_data_dir(char *out, size_t cap) {
     return true;
 }
 
-void files_init(const char *game_dir) {
+void files_init(const char *game_dir, const char *data_dir) {
     for (int i = 0; i < MAX_FILES; i++)
         if (F.files[i].f)
             fclose(F.files[i].f);
     memset(&F, 0, sizeof F);
     snprintf(F.game_dir, sizeof F.game_dir, "%s", game_dir);
+    if (data_dir)
+        snprintf(F.data_dir, sizeof F.data_dir, "%s", data_dir);
     F.ndirs = 1; /* ID 2: the game folder */
 }
 
@@ -110,23 +123,43 @@ static uint32_t dir_id(const char *rel) {
     return FILES_ROOT_DIRID + (uint32_t)F.ndirs++;
 }
 
-static void host_path(const char *rel, char *out, size_t cap) {
-    snprintf(out, cap, "%s%s%s", F.game_dir, *rel ? "/" : "", rel);
+/* rel inside root (the game or data folder). */
+static void under(const char *root, const char *rel, char *out, size_t cap) {
+    if (snprintf(out, cap, "%s%s%s", root, *rel ? "/" : "", rel) >= (int)cap)
+        trap_crash("the path %s/%s is too long", root, rel);
 }
 
-static bool is_dir(const char *rel) {
-    char p[1600];
-    host_path(rel, p, sizeof p);
+static bool exists(const char *path, bool want_dir) {
     struct stat st;
-    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
+    return stat(path, &st) == 0 && (want_dir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode));
+}
+
+/* Where rel is on the host: in the data folder if it's there, otherwise in
+   the game folder (whether or not it exists). */
+static void locate(const char *rel, bool want_dir, char *out, size_t cap) {
+    if (F.data_dir[0]) {
+        under(F.data_dir, rel, out, cap);
+        if (exists(out, want_dir))
+            return;
+    }
+    under(F.game_dir, rel, out, cap);
+}
+
+static bool is_dir(const char *rel) {
+    char p[PATH_CAP];
+    locate(rel, true, p, sizeof p);
+    return exists(p, true);
 }
 
 /* Joins a relative directory and a UTF-8 component. */
 static void join(const char *dir, const char *name, char *out, size_t cap) {
-    snprintf(out, cap, "%s%s%s", dir, *dir ? "/" : "", name);
+    if (snprintf(out, cap, "%s%s%s", dir, *dir ? "/" : "", name) >= (int)cap)
+        trap_crash("the path %s/%s is too long", dir, name);
 }
 
-/* FSMakeFSSpec(vRefNum, dirID, fileName, FSSpec *spec) -> OSErr */
+/* FSMakeFSSpec(vRefNum, dirID, fileName, FSSpec *spec) -> OSErr. A leading
+   ':' means relative; each further empty component ("::") goes up one
+   folder, but never above the game folder. */
 static void h_fs_make_fsspec(void) {
     int16_t vref = (int16_t)trap_arg(0);
     uint32_t dir = trap_arg(1), name_p = trap_arg(2), spec = trap_arg(3);
@@ -139,8 +172,7 @@ static void h_fs_make_fsspec(void) {
         trap_crash("FSMakeFSSpec: unknown directory ID %u", dir);
     char mac[256];
     gm_read_pstr(name_p, mac);
-    /* Walk ':'-separated components; a leading ':' means relative. */
-    char rel[512], comp[256], utf[512];
+    char rel[REL_CAP], comp[256], utf[REL_CAP];
     snprintf(rel, sizeof rel, "%s", base);
     const char *p = mac[0] == ':' ? mac + 1 : mac;
     if (strchr(mac, ':') && mac[0] != ':')
@@ -148,14 +180,34 @@ static void h_fs_make_fsspec(void) {
     for (;;) {
         const char *colon = strchr(p, ':');
         size_t n = colon ? (size_t)(colon - p) : strlen(p);
-        if (n == 0 || n > 63)
-            trap_crash("FSMakeFSSpec: bad path \"%s\"", mac);
+        if (n > 63) {
+            trap_return((uint32_t)FILES_BD_NAM_ERR);
+            return;
+        }
+        if (n == 0 && colon) { /* "::": the parent folder */
+            char *slash = strrchr(rel, '/');
+            if (!*rel) {
+                trap_return((uint32_t)FILES_DIR_NF_ERR);
+                return;
+            }
+            if (slash)
+                *slash = '\0';
+            else
+                rel[0] = '\0';
+            p = colon + 1;
+            continue;
+        }
         memcpy(comp, p, n);
         comp[n] = '\0';
+        /* "." and ".." are ordinary Mac names but would move around on the host. */
+        if (n == 0 || strcmp(comp, ".") == 0 || strcmp(comp, "..") == 0) {
+            trap_return((uint32_t)FILES_BD_NAM_ERR);
+            return;
+        }
         if (!colon)
             break;
         files_mac_to_utf8(comp, utf, sizeof utf);
-        char next[512];
+        char next[REL_CAP];
         join(rel, utf, next, sizeof next);
         if (!is_dir(next)) {
             trap_return((uint32_t)FILES_DIR_NF_ERR);
@@ -169,39 +221,80 @@ static void h_fs_make_fsspec(void) {
     memset(gm_ptr(spec + 6, 64), 0, 64);
     gm_write_pstr(spec + 6, comp);
     files_mac_to_utf8(comp, utf, sizeof utf);
-    char file_rel[1024], hp[1600];
+    char file_rel[REL_CAP], hp[PATH_CAP];
     join(rel, utf, file_rel, sizeof file_rel);
-    host_path(file_rel, hp, sizeof hp);
-    struct stat st;
-    trap_return(stat(hp, &st) == 0 ? 0 : (uint32_t)FILES_FNF_ERR);
+    locate(file_rel, false, hp, sizeof hp);
+    trap_return(exists(hp, false) ? 0 : (uint32_t)FILES_FNF_ERR);
 }
 
-static void spec_host_path(uint32_t spec, char *out, size_t cap) {
+/* The path of spec's file relative to the game folder. */
+static void spec_rel_path(uint32_t spec, char *out, size_t cap) {
     uint32_t dir = gm_r32(spec + 2);
     const char *base = dir_path(dir);
     if (!base)
         trap_crash("FSSpec has an unknown directory ID %u", dir);
-    char mac[256], utf[512], rel[1024];
+    char mac[256], utf[REL_CAP];
     gm_read_pstr(spec + 6, mac);
+    if (!mac[0] || strcmp(mac, ".") == 0 || strcmp(mac, "..") == 0 || strchr(mac, ':'))
+        trap_crash("FSSpec has a bad name \"%s\"", mac);
     files_mac_to_utf8(mac, utf, sizeof utf);
-    join(base, utf, rel, sizeof rel);
-    host_path(rel, out, cap);
+    join(base, utf, out, cap);
 }
 
-/* FSpOpenDF(const FSSpec *spec, SInt8 permission, short *refNum) -> OSErr */
+/* Logs (once) that there's nowhere to write. */
+static int16_t no_data_dir(const char *call) {
+    if (!F.warned_no_data)
+        log_msg("%s: there is no writable folder; the game can't save files", call);
+    F.warned_no_data = true;
+    return FILES_WR_PERM_ERR;
+}
+
+/* FSpCreate(const FSSpec *spec, OSType creator, OSType fileType, ScriptCode) -> OSErr */
+static void h_fsp_create(void) {
+    char rel[REL_CAP], path[PATH_CAP];
+    spec_rel_path(trap_arg(0), rel, sizeof rel);
+    locate(rel, false, path, sizeof path);
+    if (exists(path, false)) {
+        trap_return((uint32_t)FILES_DUP_FN_ERR);
+        return;
+    }
+    if (!F.data_dir[0]) {
+        trap_return((uint32_t)no_data_dir("FSpCreate"));
+        return;
+    }
+    under(F.data_dir, rel, path, sizeof path);
+    char parent[PATH_CAP];
+    snprintf(parent, sizeof parent, "%s", path);
+    *strrchr(parent, '/') = '\0';
+    FILE *f = make_dirs(parent) ? fopen(path, "wb") : NULL;
+    if (!f || fclose(f) != 0) {
+        log_msg("FSpCreate: can't create %s: %s", path, strerror(errno));
+        trap_return((uint32_t)FILES_IO_ERR);
+        return;
+    }
+    trap_return(0);
+}
+
+/* FSpOpenDF(const FSSpec *spec, SInt8 permission, short *refNum) -> OSErr.
+   Every permission but fsRdPerm (1) allows writing; the file is opened for
+   reading either way and copied to the data folder at its first write. */
 static void h_fsp_open_df(void) {
     uint32_t spec = trap_arg(0), out = trap_arg(2);
     int perm = (int8_t)trap_arg(1);
-    if (perm != 0 && perm != 1)
-        trap_crash("FSpOpenDF: opening files for writing (permission %d) is not supported yet", perm);
-    char path[1600];
-    spec_host_path(spec, path, sizeof path);
+    if (perm < 0 || perm > 4)
+        trap_crash("FSpOpenDF: unknown permission %d", perm);
+    char rel[REL_CAP], path[PATH_CAP];
+    spec_rel_path(spec, rel, sizeof rel);
+    locate(rel, false, path, sizeof path);
     int slot = 0;
     while (slot < MAX_FILES && F.files[slot].f)
         slot++;
     if (slot == MAX_FILES)
         trap_crash("FSpOpenDF: more than %d open files", MAX_FILES);
-    FILE *f = fopen(path, "rb");
+    bool in_data = F.data_dir[0] && strncmp(path, F.data_dir, strlen(F.data_dir)) == 0 &&
+                   path[strlen(F.data_dir)] == '/';
+    bool writable = perm != 1;
+    FILE *f = exists(path, false) ? fopen(path, in_data && writable ? "r+b" : "rb") : NULL;
     if (!f) {
         trap_return((uint32_t)FILES_FNF_ERR);
         return;
@@ -209,24 +302,64 @@ static void h_fsp_open_df(void) {
     fseek(f, 0, SEEK_END);
     F.files[slot].f = f;
     F.files[slot].eof = ftell(f);
+    F.files[slot].writable = writable;
+    F.files[slot].in_data = in_data;
+    snprintf(F.files[slot].rel, sizeof F.files[slot].rel, "%s", rel);
     fseek(f, 0, SEEK_SET);
     gm_w16(out, (uint16_t)(FIRST_REFNUM + slot));
     trap_return(0);
 }
 
-static FILE *file_of(int16_t ref, long *eof) {
+static open_file *file_of(int16_t ref) {
     int slot = ref - FIRST_REFNUM;
     if (slot < 0 || slot >= MAX_FILES || !F.files[slot].f)
         return NULL;
-    *eof = F.files[slot].eof;
-    return F.files[slot].f;
+    return &F.files[slot];
+}
+
+/* Makes an open file writable: copies a game-folder file into the data
+   folder and reopens the copy at the same mark. Returns an OSErr. */
+static int16_t make_writable(const char *call, open_file *o) {
+    if (!o->writable)
+        return FILES_WR_PERM_ERR;
+    if (o->in_data)
+        return 0;
+    if (!F.data_dir[0])
+        return no_data_dir(call);
+    char src[PATH_CAP], dst[PATH_CAP], parent[PATH_CAP];
+    under(F.game_dir, o->rel, src, sizeof src);
+    under(F.data_dir, o->rel, dst, sizeof dst);
+    snprintf(parent, sizeof parent, "%s", dst);
+    *strrchr(parent, '/') = '\0';
+    long mark = ftell(o->f);
+    FILE *out = make_dirs(parent) ? fopen(dst, "wb") : NULL;
+    bool ok = out != NULL;
+    fseek(o->f, 0, SEEK_SET);
+    char buf[65536];
+    size_t n;
+    while (ok && (n = fread(buf, 1, sizeof buf, o->f)) > 0)
+        ok = fwrite(buf, 1, n, out) == n;
+    ok = ok && !ferror(o->f);
+    if (out && fclose(out) != 0)
+        ok = false;
+    FILE *f = ok ? fopen(dst, "r+b") : NULL;
+    if (!f) {
+        log_msg("%s: can't copy %s to %s: %s", call, src, dst, strerror(errno));
+        fseek(o->f, mark, SEEK_SET);
+        return FILES_IO_ERR;
+    }
+    fclose(o->f);
+    o->f = f;
+    o->in_data = true;
+    fseek(f, mark, SEEK_SET);
+    return 0;
 }
 
 /* Moves the mark; returns an OSErr. */
 static int16_t set_pos(FILE *f, long eof, int mode, int32_t off) {
     long base;
     switch (mode & 3) {
-    case FS_AT_MARK: return 0;
+    case FS_AT_MARK: fseek(f, 0, SEEK_CUR); return 0; /* C needs a seek between writes and reads */
     case FS_FROM_START: base = 0; break;
     case FS_FROM_LEOF: base = eof; break;
     default: base = ftell(f); break;
@@ -245,81 +378,141 @@ static int16_t set_pos(FILE *f, long eof, int mode, int32_t off) {
 /* PBReadSync(ParmBlkPtr) -> OSErr */
 static void h_pb_read_sync(void) {
     uint32_t pb = trap_arg(0);
-    long eof;
-    FILE *f = file_of((int16_t)gm_r16(pb + PB_REFNUM), &eof);
+    open_file *o = file_of((int16_t)gm_r16(pb + PB_REFNUM));
     int16_t err = 0;
     uint32_t got = 0;
-    if (!f) {
+    if (!o) {
         err = FILES_RF_NUM_ERR;
     } else {
         int mode = (int16_t)gm_r16(pb + PB_POS_MODE);
         if (mode & 0x80)
             trap_crash("PBReadSync: newline mode is not supported");
-        err = set_pos(f, eof, mode, (int32_t)gm_r32(pb + PB_POS_OFFSET));
+        err = set_pos(o->f, o->eof, mode, (int32_t)gm_r32(pb + PB_POS_OFFSET));
         int32_t want = (int32_t)gm_r32(pb + PB_REQ_COUNT);
         if (!err && want > 0) {
             uint8_t *buf = gm_ptr(gm_r32(pb + PB_BUFFER), (uint32_t)want);
-            got = (uint32_t)fread(buf, 1, (size_t)want, f);
+            got = (uint32_t)fread(buf, 1, (size_t)want, o->f);
             if (got < (uint32_t)want)
                 err = FILES_EOF_ERR;
         }
-        gm_w32(pb + PB_POS_OFFSET, (uint32_t)ftell(f));
+        gm_w32(pb + PB_POS_OFFSET, (uint32_t)ftell(o->f));
     }
     gm_w32(pb + PB_ACT_COUNT, got);
     gm_w16(pb + PB_RESULT, (uint16_t)err);
     trap_return((uint32_t)(int32_t)err);
 }
 
+/* FSWrite(short refNum, long *count, const void *buffer) -> OSErr: writes at
+   the mark, extending the file as needed. *count becomes the bytes written. */
+static void h_fs_write(void) {
+    open_file *o = file_of((int16_t)trap_arg(0));
+    uint32_t count_p = trap_arg(1);
+    if (!o) {
+        trap_return((uint32_t)FILES_RF_NUM_ERR);
+        return;
+    }
+    int32_t want = (int32_t)gm_r32(count_p);
+    int16_t err = want < 0 ? FILES_PARAM_ERR : make_writable("FSWrite", o);
+    size_t put = 0;
+    if (!err && want > 0) {
+        fseek(o->f, 0, SEEK_CUR);
+        put = fwrite(gm_ptr(trap_arg(2), (uint32_t)want), 1, (size_t)want, o->f);
+        if (put < (size_t)want)
+            err = FILES_IO_ERR;
+        long mark = ftell(o->f);
+        if (mark > o->eof)
+            o->eof = mark;
+    }
+    gm_w32(count_p, (uint32_t)put);
+    trap_return((uint32_t)(int32_t)err);
+}
+
 static void h_get_eof(void) {
-    long eof;
-    FILE *f = file_of((int16_t)trap_arg(0), &eof);
-    if (!f) {
+    open_file *o = file_of((int16_t)trap_arg(0));
+    if (!o) {
         trap_return((uint32_t)FILES_RF_NUM_ERR);
         return;
     }
-    gm_w32(trap_arg(1), (uint32_t)eof);
+    gm_w32(trap_arg(1), (uint32_t)o->eof);
     trap_return(0);
 }
 
+/* SetEOF(short refNum, long logEOF) -> OSErr: truncates or extends with
+   zeros. A mark past the new end moves to it. */
+static void h_set_eof(void) {
+    open_file *o = file_of((int16_t)trap_arg(0));
+    int32_t eof = (int32_t)trap_arg(1);
+    if (!o) {
+        trap_return((uint32_t)FILES_RF_NUM_ERR);
+        return;
+    }
+    if (eof < 0) {
+        trap_return((uint32_t)FILES_PARAM_ERR);
+        return;
+    }
+    int16_t err = make_writable("SetEOF", o);
+    if (!err) {
+        long mark = ftell(o->f);
+        fflush(o->f);
+        if (ftruncate(fileno(o->f), eof) != 0) {
+            err = FILES_IO_ERR;
+        } else {
+            o->eof = eof;
+            fseek(o->f, mark > eof ? eof : mark, SEEK_SET);
+        }
+    }
+    trap_return((uint32_t)(int32_t)err);
+}
+
 static void h_set_fpos(void) {
-    long eof;
-    FILE *f = file_of((int16_t)trap_arg(0), &eof);
-    if (!f) {
+    open_file *o = file_of((int16_t)trap_arg(0));
+    if (!o) {
         trap_return((uint32_t)FILES_RF_NUM_ERR);
         return;
     }
-    trap_return((uint32_t)(int32_t)set_pos(f, eof, (int16_t)trap_arg(1), (int32_t)trap_arg(2)));
+    trap_return((uint32_t)(int32_t)set_pos(o->f, o->eof, (int16_t)trap_arg(1), (int32_t)trap_arg(2)));
 }
 
 static void h_get_fpos(void) {
-    long eof;
-    FILE *f = file_of((int16_t)trap_arg(0), &eof);
-    if (!f) {
+    open_file *o = file_of((int16_t)trap_arg(0));
+    if (!o) {
         trap_return((uint32_t)FILES_RF_NUM_ERR);
         return;
     }
-    gm_w32(trap_arg(1), (uint32_t)ftell(f));
+    gm_w32(trap_arg(1), (uint32_t)ftell(o->f));
     trap_return(0);
 }
 
+/* PBFlushFileSync(ParmBlkPtr) -> OSErr: pushes buffered writes to the host. */
+static void h_pb_flush_file_sync(void) {
+    uint32_t pb = trap_arg(0);
+    open_file *o = file_of((int16_t)gm_r16(pb + PB_REFNUM));
+    int16_t err = !o ? FILES_RF_NUM_ERR : fflush(o->f) != 0 ? FILES_IO_ERR : 0;
+    gm_w16(pb + PB_RESULT, (uint16_t)err);
+    trap_return((uint32_t)(int32_t)err);
+}
+
 static void h_fs_close(void) {
     int16_t ref = (int16_t)trap_arg(0);
-    long eof;
-    FILE *f = file_of(ref, &eof);
-    if (!f) {
+    open_file *o = file_of(ref);
+    if (!o) {
         trap_return((uint32_t)FILES_RF_NUM_ERR);
         return;
     }
-    fclose(f);
-    F.files[ref - FIRST_REFNUM].f = NULL;
-    trap_return(0);
+    int failed = fclose(o->f);
+    memset(o, 0, sizeof *o);
+    trap_return(failed ? (uint32_t)FILES_IO_ERR : 0);
 }
 
 void files_register(void) {
     trap_register("FSMakeFSSpec", h_fs_make_fsspec);
+    trap_register("FSpCreate", h_fsp_create);
     trap_register("FSpOpenDF", h_fsp_open_df);
     trap_register("PBReadSync", h_pb_read_sync);
+    trap_register("FSWrite", h_fs_write);
     trap_register("GetEOF", h_get_eof);
+    trap_register("SetEOF", h_set_eof);
+    trap_register("PBFlushFileSync", h_pb_flush_file_sync);
     trap_register("SetFPos", h_set_fpos);
     trap_register("GetFPos", h_get_fpos);
     trap_register("FSClose", h_fs_close);
diff --git a/src/files.h b/src/files.h
index a1464d7..ce89b8c 100644
--- a/src/files.h
+++ b/src/files.h
@@ -3,9 +3,13 @@
 #include <stddef.h>
 #include <stdint.h>
 
-/* File Manager, read side: FSSpecs and data-fork reads from the game folder.
-   Writing files arrives with milestone 6; until then calls that would write
-   crash with a report.
+/* File Manager: FSSpecs and data-fork reads and writes.
+
+   Two folders back one fake volume: the game folder, which is never
+   modified, and a writable data folder that overlays it. A path is looked up
+   in the data folder first, then in the game folder. FSpCreate makes new
+   files in the data folder, and the first write to a game-folder file copies
+   it to the matching path there, so the game believes it saved in place.
 
    There is one fake volume (FILES_VREFNUM). Directory IDs are handed out per
    folder: FILES_ROOT_DIRID is the game folder, which is also the default
@@ -20,6 +24,9 @@
 #define FILES_RF_NUM_ERR (-51)
 #define FILES_POS_ERR    (-40)
 #define FILES_BD_NAM_ERR (-37)
+#define FILES_DUP_FN_ERR (-48)
+#define FILES_WR_PERM_ERR (-61)
+#define FILES_IO_ERR     (-36)
 
 /* FSSpec: vRefNum (2), parID (4), name (Str63, 64 bytes). */
 #define FSSPEC_SIZE 70
@@ -28,11 +35,14 @@
    Support/loony-shim. False if neither LOONY_DATA_DIR nor HOME is set. */
 bool files_data_dir(char *out, size_t cap);
 
-/* Sets the game folder (read-only). Closes open files and forgets directory IDs. */
-void files_init(const char *game_dir);
+/* Sets the game folder (read-only) and the writable folder (created when
+   first needed; NULL means writes fail with wrPermErr). Closes open files
+   and forgets directory IDs. */
+void files_init(const char *game_dir, const char *data_dir);
 
 /* Converts a Mac Roman name to UTF-8, with '/' (legal in Mac names) becoming ':'. */
 void files_mac_to_utf8(const char *mac, char *out, size_t cap);
 
-/* Registers FSMakeFSSpec, FSpOpenDF, PBReadSync, GetEOF, SetFPos, GetFPos and FSClose. */
+/* Registers FSMakeFSSpec, FSpCreate, FSpOpenDF, PBReadSync, FSWrite, GetEOF,
+   SetEOF, SetFPos, GetFPos, PBFlushFileSync and FSClose. */
 void files_register(void);
diff --git a/src/main.c b/src/main.c
index 7ff09c3..26f0ac0 100644
--- a/src/main.c
+++ b/src/main.c
@@ -65,17 +65,18 @@ int main(int argc, char **argv) {
     misc_init();
     cf_init();
     char data_dir[PATH_MAX];
-    if (files_data_dir(data_dir, sizeof data_dir)) {
+    bool have_data = files_data_dir(data_dir, sizeof data_dir);
+    if (have_data) {
         char prefs[PATH_MAX + 16];
         snprintf(prefs, sizeof prefs, "%s/prefs.plist", data_dir);
         cf_load_prefs(prefs);
     } else {
-        log_msg("neither LOONY_DATA_DIR nor HOME is set: preferences won't be saved");
+        log_msg("neither LOONY_DATA_DIR nor HOME is set: nothing will be saved");
     }
     qd_init(800, 600, 8);
     dialogs_init();
     events_init();
-    files_init(dir);
+    files_init(dir, have_data ? data_dir : NULL);
     sound_init();
     display_init();
     events_set_present(display_present_if_dirty);
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build build && ./build/loony_tests files_ && ./build/loony_tests`
Expected: all pass (the registration test skips without `LOONY_TEST_EMAIL`/`LOONY_TEST_KEY`); no sanitizer reports.

- [ ] **Step 5: Commit**

```bash
git add src/files.c src/files.h src/main.c tests/test_files.c
git commit -m "Write files through a data-folder overlay; the game folder stays untouched"
```

---

### Task 3: The last small imports

**Files:**
- Modify: `src/misc.c`
- Modify: `src/misc.h`
- Modify: `src/qd.c`
- Modify: `tests/test_misc.c`
- Modify: `tests/test_qd.c`

**Interfaces:**
- Produces: `ICLaunchURL`, `num2dec` (`misc_num2dec`, `misc_decimal`, `misc_set_url_opener` in `misc.h`), `GetEntryColor`, `DisposePalette`.

- [ ] **Step 1: Write the failing tests**

```diff
diff --git a/tests/test_misc.c b/tests/test_misc.c
index 4653393..99fc600 100644
--- a/tests/test_misc.c
+++ b/tests/test_misc.c
@@ -1,8 +1,10 @@
 #include "test.h"
 
+#include <math.h>
 #include <stdlib.h>
 #include <time.h>
 
+#include "cpu.h"
 #include "harness.h"
 #include "misc.h"
 
@@ -10,7 +12,7 @@ static const char *const names[] = {
     "Gestalt", "TickCount", "Microseconds", "Delay", "GetDateTime", "ReadLocation",
     "NumToString", "p2cstrcpy", "c2pstrcpy", "BlockMoveData", "InitCursor", "HideCursor",
     "SetThemeCursor", "KeyScript", "GetMBarHeight", "NewAEEventHandlerUPP",
-    "AEInstallEventHandler", "ICStart", "ICStop", "ExitToShell",
+    "AEInstallEventHandler", "ICStart", "ICStop", "ExitToShell", "ICLaunchURL", "num2dec",
 };
 
 static void setup(void) {
@@ -295,3 +297,74 @@ TEST(misc_fixed_clock_location_is_fixed) {
     fixed_teardown();
     CHECK_EQ(delta, 0); /* GMT, no daylight saving */
 }
+
+static char opened[256];
+static bool fake_open(const char *url) {
+    snprintf(opened, sizeof opened, "%s", url);
+    return true;
+}
+
+static int32_t launch(const char *data, int32_t start, int32_t end) {
+    uint32_t d = scratch(256), sp = scratch(4), ep = scratch(4);
+    memcpy(gm_ptr(d, 256), data, strlen(data));
+    gm_w32(sp, (uint32_t)start);
+    gm_w32(ep, (uint32_t)end);
+    return (int32_t)call_import("ICLaunchURL", 6, MISC_IC_INSTANCE, scratch(2), d,
+                                (uint32_t)strlen(data), sp, ep);
+}
+
+TEST(misc_ic_launch_url_opens_the_selection) {
+    misc_set_url_opener(fake_open);
+    setup();
+    opened[0] = '\0';
+    const char *u = "http://www.LittleWingPinball.com/";
+    CHECK_EQ(launch(u, 0, (int32_t)strlen(u)), 0);
+    CHECK_STR(opened, u);
+    opened[0] = '\0';
+    CHECK_EQ(launch("xx https://a.example/ yy", 3, 21), 0);
+    CHECK_STR(opened, "https://a.example/");
+    opened[0] = '\0';
+    CHECK_EQ(launch("file:///etc/passwd", 0, 18), -50); /* only the web */
+    CHECK_EQ(launch("http://a/", 0, 99), -50);           /* past the data */
+    CHECK_STR(opened, "");
+}
+
+static void check_dec(int style, int digits, double x, bool neg, const char *sig, int exp) {
+    misc_decimal d;
+    misc_num2dec(style, digits, x, &d);
+    if (d.negative != neg || strcmp(d.sig, sig) != 0 || d.exp != exp) {
+        char m[200];
+        snprintf(m, sizeof m, "num2dec(%d, %d, %g) = %s%se%d, expected %s%se%d", style, digits, x,
+                 d.negative ? "-" : "", d.sig, d.exp, neg ? "-" : "", sig, exp);
+        test_fail(__FILE__, __LINE__, m);
+    }
+}
+
+TEST(misc_num2dec_float_and_fixed) {
+    check_dec(0, 5, 3.14159265, false, "31416", -4);
+    check_dec(0, 3, -1234567.0, true, "123", 4);
+    check_dec(0, 2, 9.99, false, "10", 0); /* rounding carries */
+    check_dec(0, 4, 0.0, false, "0", 0);
+    check_dec(0, 4, -0.0, true, "0", 0);
+    check_dec(0, 6, 1e300 * 1e300, false, "I", 0);
+    check_dec(0, 6, NAN, false, "N", 0);
+    check_dec(0, 32, 0.1, false, "10000000000000000555111512312578", -32);
+    check_dec(1, 2, 3.14159, false, "314", -2);
+    check_dec(1, 0, 2.5, false, "2", 0); /* round half to even, like printf */
+    check_dec(1, 3, 0.0004, false, "0", 0);
+    check_dec(1, 2, -0.5, true, "50", -2);
+    check_dec(1, 2, 1e40, false, "?", 0);
+}
+
+TEST(misc_num2dec_writes_the_decimal_record) {
+    setup();
+    uint32_t f = scratch(4), d = scratch(42);
+    gm_w8(f, 0);
+    gm_w16(f + 2, 4);
+    cpu_set_fpr(1, -2.5);
+    call_import("num2dec", 4, f, 0u, 0u, d);
+    CHECK_EQ(gm_r8(d), 1);
+    CHECK_EQ((int16_t)gm_r16(d + 2), -3);
+    CHECK_EQ(gm_r8(d + 4), 4);
+    CHECK(memcmp(gm_ptr(d + 5, 4), "2500", 4) == 0);
+}
diff --git a/tests/test_qd.c b/tests/test_qd.c
index 3f9a73f..2d88735 100644
--- a/tests/test_qd.c
+++ b/tests/test_qd.c
@@ -14,6 +14,7 @@ static const char *const names[] = {
     "GetPortBounds", "GetWindowPortBounds", "GetPortBitMapForCopyBits", "GetQDGlobalsScreenBits",
     "ShowWindow", "HideWindow", "InvalWindowRect", "QDFlushPortBuffer", "BeginFullScreen",
     "EndFullScreen", "ClipRect", "RGBForeColor", "PaintRect", "CopyBits", "DrawPicture",
+    "GetEntryColor", "DisposePalette",
 };
 
 static void setup(void) {
@@ -294,3 +295,35 @@ TEST(qd_indexed_pixmap_without_a_color_table_crashes) {
     CHECK_EQ(test_run_child(child_bad_pixmap, &which, out, sizeof out), 2);
     CHECK_CONTAINS(out, "CopyBits: 8-bit pixmap has no color table");
 }
+
+TEST(qd_palette_entries_and_dispose) {
+    setup();
+    uint32_t pal = mm_new_handle(16 + 2 * 16, true);
+    uint32_t p = gm_r32(pal);
+    gm_w16(p, 2);
+    gm_w16(p + 32, 0x1111);
+    gm_w16(p + 34, 0x2222);
+    gm_w16(p + 36, 0x3333);
+    uint32_t rgb = scratch(6);
+    call_import("GetEntryColor", 3, pal, 1u, rgb);
+    CHECK_EQ(gm_r16(rgb), 0x1111);
+    CHECK_EQ(gm_r16(rgb + 2), 0x2222);
+    CHECK_EQ(gm_r16(rgb + 4), 0x3333);
+    call_import("DisposePalette", 1, 0u); /* NULL is fine */
+    call_import("DisposePalette", 1, pal);
+    CHECK(!mm_is_handle(pal));
+}
+
+static void child_entry_outside(void *unused) {
+    (void)unused;
+    setup();
+    uint32_t pal = mm_new_handle(16 + 16, true);
+    gm_w16(gm_r32(pal), 1);
+    call_import("GetEntryColor", 3, pal, 1u, scratch(6));
+}
+
+TEST(qd_palette_entry_outside_crashes) {
+    char out[16384];
+    CHECK_EQ(test_run_child(child_entry_outside, NULL, out, sizeof out), 2);
+    CHECK_CONTAINS(out, "GetEntryColor: entry 1 is outside the palette (1 entries)");
+}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build`
Expected: the new tests fail to build (`misc_set_url_opener`, `misc_num2dec`).

- [ ] **Step 3: Implement**

```diff
diff --git a/src/misc.c b/src/misc.c
index 80dcec6..8c7c9f2 100644
--- a/src/misc.c
+++ b/src/misc.c
@@ -2,11 +2,14 @@
 
 #include <ctype.h>
 #include <math.h>
+#include <spawn.h>
 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
+#include <sys/wait.h>
 #include <time.h>
 
+#include "cpu.h"
 #include "guest_mem.h"
 #include "trap.h"
 #include "util.h"
@@ -27,14 +30,30 @@ static struct {
     bool fixed;
     uint64_t virtual_us;
     int polls; /* time polls since the virtual clock last moved */
+    misc_url_fn open_url;
 } M;
 
+extern char **environ;
+
+static bool open_with_open(const char *url) {
+    char *argv[] = {"/usr/bin/open", (char *)url, NULL};
+    pid_t pid;
+    if (posix_spawn(&pid, argv[0], NULL, NULL, argv, environ) != 0)
+        return false;
+    int status;
+    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
+}
+
+void misc_set_url_opener(misc_url_fn fn) { M.open_url = fn; }
+
 void misc_set_idle(misc_idle_fn fn) { M.idle = fn; }
 
 void misc_init(void) {
     misc_idle_fn idle = M.idle;
+    misc_url_fn open_url = M.open_url;
     memset(&M, 0, sizeof M);
     M.idle = idle;
+    M.open_url = open_url ? open_url : open_with_open;
     clock_gettime(CLOCK_MONOTONIC, &M.start);
     const char *f = getenv("LOONY_FIXED_CLOCK");
     M.fixed = f && strcmp(f, "1") == 0;
@@ -276,6 +295,98 @@ static void h_ic_start(void) {
 
 static void h_ic_stop(void) { trap_return(0); }
 
+/* ICLaunchURL(ICInstance, ConstStr255Param hint, const void *data, long len,
+   long *selStart, long *selEnd) -> OSStatus. The URL is data[*selStart,
+   *selEnd). Only http and https URLs are opened. */
+static void h_ic_launch_url(void) {
+    uint32_t data = trap_arg(2), start_p = trap_arg(4), end_p = trap_arg(5);
+    int32_t len = (int32_t)trap_arg(3);
+    int32_t start = (int32_t)gm_r32(start_p), end = (int32_t)gm_r32(end_p);
+    if (len < 0 || start < 0 || end < start || end > len || end - start > 1023) {
+        trap_return((uint32_t)-50); /* paramErr */
+        return;
+    }
+    char url[1024];
+    memcpy(url, gm_ptr(data + (uint32_t)start, (uint32_t)(end - start) + 1), (size_t)(end - start));
+    url[end - start] = '\0';
+    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
+        log_msg("ICLaunchURL: not opening \"%s\" (only http and https)", url);
+        trap_return((uint32_t)-50);
+        return;
+    }
+    log_msg("opening %s", url);
+    if (!M.open_url(url))
+        log_msg("ICLaunchURL: couldn't open %s", url);
+    trap_return(0);
+}
+
+/* ---- SANE ---- */
+
+void misc_num2dec(int style, int digits, double x, misc_decimal *out) {
+    memset(out, 0, sizeof *out);
+    out->negative = signbit(x) != 0;
+    double a = fabs(x);
+    if (isnan(x) || isinf(x)) {
+        out->sig[0] = isnan(x) ? 'N' : 'I';
+        return;
+    }
+    char buf[400];
+    if (style == 0) {
+        if (digits < 1)
+            digits = 1;
+        if (digits > MISC_SIGDIGLEN)
+            digits = MISC_SIGDIGLEN;
+        if (a == 0) {
+            out->sig[0] = '0';
+            return;
+        }
+        snprintf(buf, sizeof buf, "%.*e", digits - 1, a); /* d.ddde[+-]x */
+        int e = atoi(strchr(buf, 'e') + 1);
+        int n = 0;
+        for (const char *p = buf; *p != 'e'; p++)
+            if (isdigit((unsigned char)*p))
+                out->sig[n++] = *p;
+        out->exp = (int16_t)(e - (n - 1));
+        return;
+    }
+    if (digits < 0)
+        digits = 0;
+    if (digits > 80)
+        digits = 80;
+    snprintf(buf, sizeof buf, "%.*f", digits, a);
+    char d[400];
+    int n = 0;
+    for (const char *p = buf; *p; p++)
+        if (isdigit((unsigned char)*p))
+            d[n++] = *p;
+    d[n] = '\0';
+    const char *first = d;
+    while (*first == '0' && first[1])
+        first++;
+    if (strlen(first) > MISC_SIGDIGLEN) {
+        out->sig[0] = '?';
+        return;
+    }
+    strcpy(out->sig, first);
+    out->exp = (int16_t)(strcmp(first, "0") == 0 ? 0 : -digits);
+}
+
+/* num2dec(const decform *f, double x, decimal *d). decform is {char style;
+   char unused; short digits}; decimal is {char sgn; char unused; short exp;
+   unsigned char length; unsigned char text[36]; unsigned char unused}. x
+   arrives in f1 and takes up r4-r5, so d is in r6. */
+static void h_num2dec(void) {
+    uint32_t f = trap_arg(0), d = trap_arg(3);
+    misc_decimal dec;
+    misc_num2dec((int8_t)gm_r8(f), (int16_t)gm_r16(f + 2), cpu_fpr(1), &dec);
+    gm_w8(d, dec.negative);
+    gm_w8(d + 1, 0);
+    gm_w16(d + 2, (uint16_t)dec.exp);
+    size_t n = strlen(dec.sig);
+    gm_w8(d + 4, (uint8_t)n);
+    memcpy(gm_ptr(d + 5, MISC_SIGDIGLEN), dec.sig, n);
+}
+
 static void h_exit_to_shell(void) {
     log_msg("ExitToShell");
     exit(0);
@@ -301,5 +412,7 @@ void misc_register(void) {
     trap_register("AEInstallEventHandler", h_ae_install_event_handler);
     trap_register("ICStart", h_ic_start);
     trap_register("ICStop", h_ic_stop);
+    trap_register("ICLaunchURL", h_ic_launch_url);
+    trap_register("num2dec", h_num2dec);
     trap_register("ExitToShell", h_exit_to_shell);
 }
diff --git a/src/misc.h b/src/misc.h
index 60532fc..6cf75ec 100644
--- a/src/misc.h
+++ b/src/misc.h
@@ -46,6 +46,26 @@ bool misc_ae_handler(uint32_t event_class, uint32_t event_id, uint32_t *handler,
 typedef void (*misc_idle_fn)(void);
 void misc_set_idle(misc_idle_fn fn);
 
+/* Opens a URL on the host. The default runs /usr/bin/open; tests replace
+   it. Returns false if it couldn't. */
+typedef bool (*misc_url_fn)(const char *url);
+void misc_set_url_opener(misc_url_fn fn);
+
+/* SANE's decimal record (fp.h): the value is sgn, then the digits in sig
+   times 10^exp. sig holds "0" for zero, "I" for infinity, "N" for a NaN and
+   "?" if the digits don't fit. */
+#define MISC_SIGDIGLEN 36
+typedef struct {
+    bool negative;
+    int16_t exp;
+    char sig[MISC_SIGDIGLEN + 1];
+} misc_decimal;
+
+/* num2dec: style 0 (FLOATDECIMAL) gives digits significant digits (1 to
+   MISC_SIGDIGLEN), style 1 (FIXEDDECIMAL) gives digits digits after the
+   decimal point. */
+void misc_num2dec(int style, int digits, double x, misc_decimal *out);
+
 /* Registers Gestalt, time, string, cursor, Apple Event, Internet Config,
-   KeyScript, GetMBarHeight, BlockMoveData and ExitToShell imports. */
+   KeyScript, GetMBarHeight, BlockMoveData, num2dec and ExitToShell imports. */
 void misc_register(void);
diff --git a/src/qd.c b/src/qd.c
index cd5e510..1977e36 100644
--- a/src/qd.c
+++ b/src/qd.c
@@ -576,7 +576,34 @@ static void h_draw_picture(void) {
         Q.dirty = true;
 }
 
+/* Palette (Palettes.h): pmEntries (2), private fields (14), then 16-byte
+   ColorInfos whose first 6 bytes are the RGBColor. The game never makes a
+   palette (it imports no call that does), but it keeps these two calls for
+   one it might have. */
+#define PALETTE_INFO 16
+#define COLOR_INFO_SIZE 16
+
+/* GetEntryColor(PaletteHandle, short entry, RGBColor *rgb) */
+static void h_get_entry_color(void) {
+    uint32_t pal = trap_arg(0), out = trap_arg(2);
+    int16_t i = (int16_t)trap_arg(1);
+    if (!mm_is_handle(pal))
+        trap_crash("GetEntryColor: 0x%08x is not a palette handle", pal);
+    uint32_t p = gm_r32(pal), n = gm_r16(p);
+    if (i < 0 || (uint32_t)i >= n || PALETTE_INFO + COLOR_INFO_SIZE * (uint32_t)(i + 1) > mm_handle_size(pal))
+        trap_crash("GetEntryColor: entry %d is outside the palette (%u entries)", i, n);
+    write_rgb(out, read_rgb(p + PALETTE_INFO + COLOR_INFO_SIZE * (uint32_t)i));
+}
+
+static void h_dispose_palette(void) {
+    uint32_t pal = trap_arg(0);
+    if (pal && mm_dispose_handle(pal) != MM_NO_ERR)
+        trap_crash("DisposePalette: 0x%08x is not a palette handle", pal);
+}
+
 void qd_register(void) {
+    trap_register("GetEntryColor", h_get_entry_color);
+    trap_register("DisposePalette", h_dispose_palette);
     trap_register("SetRect", h_set_rect);
     trap_register("OffsetRect", h_offset_rect);
     trap_register("GetCTable", h_get_ctable);
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build build && ./build/loony_tests misc_ && ./build/loony_tests qd_palette && ./build/loony_tests`
Expected: all pass (the registration test skips without `LOONY_TEST_EMAIL`/`LOONY_TEST_KEY`); no sanitizer reports.

- [ ] **Step 5: Commit**

```bash
git add src/misc.c src/misc.h src/qd.c tests/test_misc.c tests/test_qd.c
git commit -m "ICLaunchURL, num2dec, GetEntryColor and DisposePalette"
```

---

### Task 4: The dialog font

**Files:**
- Create: `src/font.c`
- Create: `src/font.h`
- Create: `tests/test_font.c`

**Interfaces:**
- Produces: `font_init`, `font_glyph`, `font_ascii`, `font_wrap`, `font_draw`, `FONT_W`/`FONT_H`/`FONT_LINE` (`font.h`).

- [ ] **Step 1: Write the failing tests**

```diff
diff --git a/tests/test_font.c b/tests/test_font.c
new file mode 100644
index 0000000..aea0592
--- /dev/null
+++ b/tests/test_font.c
@@ -0,0 +1,73 @@
+#include "test.h"
+
+#include "font.h"
+
+TEST(font_glyphs_come_from_sdl) {
+    font_init();
+    static const uint8_t a[FONT_H] = {0x30, 0x78, 0xCC, 0xCC, 0xFC, 0xCC, 0xCC, 0x00};
+    CHECK(memcmp(font_glyph('A'), a, FONT_H) == 0);
+    static const uint8_t blank[FONT_H] = {0};
+    CHECK(memcmp(font_glyph(' '), blank, FONT_H) == 0);
+    CHECK(font_glyph(0x8E) == font_glyph('?'));
+    font_init(); /* again: harmless */
+    CHECK(memcmp(font_glyph('A'), a, FONT_H) == 0);
+}
+
+TEST(font_mac_roman_text_is_spelled_in_ascii) {
+    char out[64];
+    const uint8_t mac[] = "Caf\x8e \xaa \xd2hi\xd3\r\x01";
+    font_ascii(mac, sizeof mac - 1, out, sizeof out);
+    CHECK_STR(out, "Cafe TM \"hi\"\r?");
+    font_ascii(mac, sizeof mac - 1, out, 5); /* truncated, still terminated */
+    CHECK_STR(out, "Cafe");
+}
+
+static int wrap(const char *text, int chars, char lines[][64]) {
+    int starts[8], lens[8];
+    int n = font_wrap(text, chars * FONT_W, 8, starts, lens);
+    for (int i = 0; i < n && i < 8; i++)
+        snprintf(lines[i], 64, "%.*s", lens[i], text + starts[i]);
+    return n;
+}
+
+TEST(font_wrap_breaks_between_words) {
+    char l[8][64];
+    CHECK_EQ(wrap("hello world foo", 9, l), 2);
+    CHECK_STR(l[0], "hello");
+    CHECK_STR(l[1], "world foo");
+    CHECK_EQ(wrap("hello world", 5, l), 2); /* a space right at the edge */
+    CHECK_STR(l[0], "hello");
+    CHECK_STR(l[1], "world");
+    CHECK_EQ(wrap("abcdefghij", 4, l), 3); /* a word longer than a line */
+    CHECK_STR(l[0], "abcd");
+    CHECK_STR(l[2], "ij");
+    CHECK_EQ(wrap("a\r\rb", 10, l), 3); /* hard breaks, and an empty line */
+    CHECK_STR(l[0], "a");
+    CHECK_STR(l[1], "");
+    CHECK_STR(l[2], "b");
+    CHECK_EQ(wrap("", 10, l), 1);
+    CHECK_STR(l[0], "");
+    CHECK_EQ(wrap("one two", 0, l), 6); /* under one character wide: one per line, no space */
+    int s[2], n[2];
+    CHECK_EQ(font_wrap("a b c d", FONT_W, 2, s, n), 4); /* more lines than room */
+}
+
+TEST(font_draw_sets_glyph_pixels_inside_the_clip) {
+    font_init();
+    uint8_t buf[16 * 16];
+    memset(buf, 0, sizeof buf);
+    qd_palette pal;
+    qd_std_palette(8, &pal);
+    qd_pixels px = {buf, 16, {0, 0, 16, 16}, 8, &pal};
+    qd_rgb white = {0xFFFF, 0xFFFF, 0xFFFF};
+    qd_rect clip = {0, 0, 16, 12};
+    font_draw(&px, 2, 1, "AA", 2, white, clip);
+    uint8_t w = (uint8_t)qd_pixel_for(white, 8, &pal);
+    CHECK_EQ(buf[1 * 16 + 2 + 2], w);      /* row 0 of 'A' is ..##.... */
+    CHECK_EQ(buf[1 * 16 + 2 + 0], 0);
+    CHECK_EQ(buf[5 * 16 + 2 + 0], w);      /* row 4 is ######.. */
+    CHECK_EQ(buf[5 * 16 + 10 + 0], w);     /* the second 'A' */
+    CHECK_EQ(buf[5 * 16 + 10 + 4], 0);     /* x = 14: clipped (and . anyway) */
+    CHECK_EQ(buf[5 * 16 + 10 + 3], 0);     /* x = 13: clipped at right 12 */
+    CHECK_EQ(buf[5 * 16 + 11], w);         /* x = 11: inside */
+}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build`
Expected: `tests/test_font.c` fails to build (no `font.h`).

- [ ] **Step 3: Implement**

```diff
diff --git a/src/font.c b/src/font.c
new file mode 100644
index 0000000..7ab7b1c
--- /dev/null
+++ b/src/font.c
@@ -0,0 +1,126 @@
+#include "font.h"
+
+#include <SDL3/SDL.h>
+#include <string.h>
+
+#include "util.h"
+
+#define FIRST 0x20
+#define LAST 0x7E
+#define COUNT (LAST - FIRST + 1)
+
+static uint8_t glyphs[COUNT][FONT_H];
+static bool ready;
+
+void font_init(void) {
+    if (ready)
+        return;
+    SDL_Surface *s = SDL_CreateSurface(COUNT * FONT_W, FONT_H, SDL_PIXELFORMAT_RGBA32);
+    SDL_Renderer *r = s ? SDL_CreateSoftwareRenderer(s) : NULL;
+    if (!r)
+        fatal("can't rasterize the dialog font: %s", SDL_GetError());
+    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
+    SDL_RenderClear(r);
+    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
+    char all[COUNT + 1];
+    for (int i = 0; i < COUNT; i++)
+        all[i] = (char)(FIRST + i);
+    all[COUNT] = '\0';
+    SDL_RenderDebugText(r, 0, 0, all);
+    SDL_RenderPresent(r);
+    SDL_LockSurface(s);
+    for (int c = 0; c < COUNT; c++)
+        for (int y = 0; y < FONT_H; y++) {
+            uint8_t bits = 0;
+            for (int x = 0; x < FONT_W; x++) {
+                const uint8_t *p = (const uint8_t *)s->pixels + y * s->pitch + (c * FONT_W + x) * 4;
+                if (p[0] > 127)
+                    bits |= (uint8_t)(0x80 >> x);
+            }
+            glyphs[c][y] = bits;
+        }
+    SDL_UnlockSurface(s);
+    SDL_DestroyRenderer(r);
+    SDL_DestroySurface(s);
+    ready = true;
+}
+
+const uint8_t *font_glyph(uint8_t c) {
+    if (c < FIRST || c > LAST)
+        c = '?';
+    return glyphs[c - FIRST];
+}
+
+/* ASCII spellings of Mac Roman 0x80-0xFF. */
+static const char *const high[128] = {
+    "A", "A", "C", "E", "N", "O", "U", "a", "a", "a", "a", "a", "a", "c", "e", "e",     /* 80 */
+    "e", "e", "i", "i", "i", "i", "n", "o", "o", "o", "o", "o", "u", "u", "u", "u",     /* 90 */
+    "+", "o", "c", "L", "S", "*", "P", "ss", "(R)", "(c)", "TM", "'", "\"", "!=", "AE", "O", /* A0 */
+    "?", "+-", "<=", ">=", "Y", "u", "d", "?", "?", "p", "?", "a", "o", "O", "ae", "o", /* B0 */
+    "?", "!", "?", "?", "f", "~", "?", "<<", ">>", "...", " ", "A", "A", "O", "OE", "oe", /* C0 */
+    "-", "-", "\"", "\"", "'", "'", "/", "?", "y", "Y", "/", "E", "<", ">", "fi", "fl", /* D0 */
+    "+", ".", ",", "\"", "%", "A", "E", "A", "E", "E", "I", "I", "I", "I", "O", "O",    /* E0 */
+    "?", "O", "U", "U", "U", "i", "^", "~", "-", "?", ".", "o", ",", "\"", ",", "?",    /* F0 */
+};
+
+void font_ascii(const uint8_t *mac, size_t n, char *out, size_t cap) {
+    size_t o = 0;
+    for (size_t i = 0; i < n && o + 1 < cap; i++) {
+        uint8_t c = mac[i];
+        if (c >= 0x80) {
+            for (const char *s = high[c - 0x80]; *s && o + 1 < cap; s++)
+                out[o++] = *s;
+        } else {
+            out[o++] = c == '\r' || (c >= FIRST && c <= LAST) ? (char)c : '?';
+        }
+    }
+    out[o] = '\0';
+}
+
+int font_wrap(const char *text, int width, int max_lines, int *starts, int *lens) {
+    int per = width / FONT_W > 0 ? width / FONT_W : 1;
+    int n = (int)strlen(text), nlines = 0, i = 0;
+    for (;;) {
+        int start = i, end = i, last_space = -1; /* the line is text[start, end) */
+        while (end < n && text[end] != '\r' && end - start < per) {
+            if (text[end] == ' ')
+                last_space = end;
+            end++;
+        }
+        int next = end;
+        if (end < n && text[end] == '\r') {
+            next = end + 1;
+        } else if (end < n) { /* full, and more follows: break between words */
+            if (text[end] != ' ' && last_space > start)
+                end = next = last_space;
+            while (next < n && text[next] == ' ')
+                next++;
+        }
+        if (nlines < max_lines) {
+            starts[nlines] = start;
+            lens[nlines] = end - start;
+        }
+        nlines++;
+        if (next >= n)
+            return nlines;
+        i = next;
+    }
+}
+
+void font_draw(const qd_pixels *dst, int x, int y, const char *text, int n, qd_rgb color,
+               qd_rect clip) {
+    qd_rect lim = rect_sect(clip, dst->bounds);
+    for (int k = 0; k < n; k++, x += FONT_W) {
+        const uint8_t *g = font_glyph((uint8_t)text[k]);
+        for (int row = 0; row < FONT_H; row++)
+            for (int col = 0; col < FONT_W; col++) {
+                if (!(g[row] & (0x80 >> col)))
+                    continue;
+                int px = x + col, py = y + row;
+                if (px < lim.left || px >= lim.right || py < lim.top || py >= lim.bottom)
+                    continue;
+                qd_rect one = {(int16_t)py, (int16_t)px, (int16_t)(py + 1), (int16_t)(px + 1)};
+                qd_fill(dst, one, lim, color);
+            }
+    }
+}
diff --git a/src/font.h b/src/font.h
new file mode 100644
index 0000000..844c46e
--- /dev/null
+++ b/src/font.h
@@ -0,0 +1,40 @@
+#pragma once
+#include <stdbool.h>
+#include <stddef.h>
+#include <stdint.h>
+
+#include "blit.h"
+
+/* The dialogs' bitmap font: SDL's built-in 8x8 debug font (printable ASCII),
+   rasterized once with SDL's software renderer, so no font data lives in
+   the repo. Mac Roman text is shown through an ASCII spelling: accented
+   letters lose their accents, "\xaa" (the trademark sign) becomes "TM",
+   curly quotes become straight ones, and anything else without one becomes
+   '?'. */
+
+#define FONT_W 8      /* advance per character */
+#define FONT_H 8      /* glyph height */
+#define FONT_LINE 12  /* distance between lines */
+
+/* Rasterizes the glyphs. Safe to call more than once. Needs no SDL_Init. */
+void font_init(void);
+
+/* The 8 rows of character c (0x20-0x7E; others give '?'), most significant
+   bit leftmost. */
+const uint8_t *font_glyph(uint8_t c);
+
+/* Writes the ASCII spelling of Mac Roman text (n bytes) to out, NUL-terminated.
+   Carriage returns stay as '\r'. */
+void font_ascii(const uint8_t *mac, size_t n, char *out, size_t cap);
+
+/* Breaks ASCII text into lines at most width pixels wide: at '\r', and
+   between words where a line would get too long (a word longer than a line
+   is split). Writes up to max_lines (start, length) pairs into starts/lens
+   and returns the number of lines, which may exceed max_lines. */
+int font_wrap(const char *text, int width, int max_lines, int *starts, int *lens);
+
+/* Draws n characters of ASCII text with its top-left at (x, y): set glyph
+   bits become color, the rest is left alone. Only pixels inside clip and
+   dst's bounds are written. */
+void font_draw(const qd_pixels *dst, int x, int y, const char *text, int n, qd_rgb color,
+               qd_rect clip);
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build build && ./build/loony_tests font_ && ./build/loony_tests`
Expected: all pass (the registration test skips without `LOONY_TEST_EMAIL`/`LOONY_TEST_KEY`); no sanitizer reports.

- [ ] **Step 5: Commit**

```bash
git add src/font.c src/font.h tests/test_font.c
git commit -m "Dialog font: SDL's 8x8 glyphs, Mac Roman spelled in ASCII, word wrap"
```

---

### Task 5: Mouse, paste and modal input

**Files:**
- Modify: `src/display.c`
- Modify: `src/display.h`
- Modify: `src/events.c`
- Modify: `src/events.h`
- Modify: `src/main.c`
- Modify: `src/script.c`
- Modify: `src/script.h`
- Modify: `tests/test_display.c`
- Modify: `tests/test_events.c`
- Modify: `tests/test_script.c`

**Interfaces:**
- Produces: `display_input.mouse` and `.paste`, `display_set_cursor` (`display.h`); `events_post_mouse`, `events_post_text`, `ev_modal_sink`/`events_set_modal`, `events_set_cursor` (`events.h`); script actions `click` and `type` (`SCRIPT_CLICK`, `SCRIPT_TYPE`, `script_action.x`/`.y`).

- [ ] **Step 1: Write the failing tests**

```diff
diff --git a/tests/test_display.c b/tests/test_display.c
index 1b1bb2d..f5dedf6 100644
--- a/tests/test_display.c
+++ b/tests/test_display.c
@@ -41,7 +41,8 @@ TEST(display_present_works_with_the_dummy_driver) {
     CHECK_EQ(display_frames(), before + 2);
 }
 
-static int keys, quits, last_scancode;
+static int keys, quits, last_scancode, clicks, mouse_x, mouse_y;
+static char pasted[64];
 static bool last_down;
 static void on_key(int sc, bool down, bool repeat) {
     (void)repeat;
@@ -51,6 +52,12 @@ static void on_key(int sc, bool down, bool repeat) {
 }
 static void on_focus(bool active) { (void)active; }
 static void on_quit(void) { quits++; }
+static void on_mouse(int x, int y, bool down) {
+    clicks += down;
+    mouse_x = x;
+    mouse_y = y;
+}
+static void on_paste(const char *t) { snprintf(pasted, sizeof pasted, "%s", t); }
 
 static void push_key(SDL_Scancode sc, bool down, SDL_Keymod mod) {
     SDL_Event e;
@@ -67,7 +74,7 @@ TEST(display_poll_forwards_keys_but_keeps_cmd_q_and_cmd_f) {
     mm_init();
     qd_init(64, 48, 16);
     display_present(); /* opens the (dummy) window */
-    static const display_input in = {on_key, on_focus, on_quit};
+    static const display_input in = {on_key, on_focus, on_quit, on_mouse, on_paste};
     display_set_input(&in);
     keys = quits = 0;
     push_key(SDL_SCANCODE_Z, true, SDL_KMOD_NONE);
@@ -89,3 +96,48 @@ TEST(display_poll_forwards_keys_but_keeps_cmd_q_and_cmd_f) {
     display_poll();
     CHECK_EQ(quits, 2);
 }
+
+static void push_click(float x, float y, bool down, Uint8 button) {
+    SDL_Event e;
+    memset(&e, 0, sizeof e);
+    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
+    e.button.button = button;
+    e.button.down = down;
+    e.button.x = x;
+    e.button.y = y;
+    SDL_PushEvent(&e);
+}
+
+TEST(display_poll_maps_clicks_to_the_screen_and_pastes) {
+    gm_init();
+    mm_init();
+    qd_init(64, 48, 16);
+    display_present();
+    static const display_input in = {on_key, on_focus, on_quit, on_mouse, on_paste};
+    display_set_input(&in);
+    /* Whichever test opened the window chose its size; its center is the
+       screen's center either way. */
+    int n = 0;
+    SDL_Window **wins = SDL_GetWindows(&n);
+    CHECK(n == 1);
+    int ww, wh;
+    SDL_GetWindowSize(wins[0], &ww, &wh);
+    SDL_free(wins);
+    keys = clicks = 0;
+    push_click((float)ww / 2, (float)wh / 2, true, SDL_BUTTON_LEFT);
+    push_click((float)ww / 2, (float)wh / 2, false, SDL_BUTTON_LEFT);
+    push_click(5, 5, true, SDL_BUTTON_RIGHT); /* ignored */
+    display_poll();
+    CHECK_EQ(clicks, 1);
+    CHECK_EQ(mouse_x, 32);
+    CHECK_EQ(mouse_y, 24);
+    pasted[0] = '\0';
+    CHECK(SDL_SetClipboardText("me@example.com"));
+    push_key(SDL_SCANCODE_V, true, SDL_KMOD_LGUI);
+    push_key(SDL_SCANCODE_V, false, SDL_KMOD_LGUI);
+    display_poll();
+    CHECK_STR(pasted, "me@example.com");
+    CHECK_EQ(keys, 0); /* the game never sees Cmd-V */
+    display_set_cursor(false);
+    display_set_cursor(true);
+}
diff --git a/tests/test_events.c b/tests/test_events.c
index c6dad78..cf219b7 100644
--- a/tests/test_events.c
+++ b/tests/test_events.c
@@ -610,3 +610,87 @@ TEST(events_delay_inside_a_timer_doesnt_fire_other_timers) {
     CHECK_EQ(gm_r32(SEEN), 0);    /* it didn't run during the Delay */
     CHECK_EQ(gm_r32(COUNTER), 1); /* it ran once the waiting proc returned */
 }
+
+/* ---- modal input (dialogs) ---- */
+
+static struct {
+    int keys, downs, texts;
+    uint32_t vkey, mods;
+    uint8_t chr;
+    int x, y;
+    char text[64];
+} sunk;
+
+static void sink_key(uint32_t vkey, uint8_t chr, uint32_t mods) {
+    sunk.keys++;
+    sunk.vkey = vkey;
+    sunk.chr = chr;
+    sunk.mods = mods;
+}
+static void sink_mouse(int x, int y, bool down) {
+    sunk.downs += down;
+    sunk.x = x;
+    sunk.y = y;
+}
+static void sink_text(const char *t) {
+    sunk.texts++;
+    snprintf(sunk.text, sizeof sunk.text, "%s", t);
+}
+static const ev_modal_sink sink = {sink_key, sink_mouse, sink_text};
+
+TEST(events_modal_input_goes_to_the_sink_not_the_game) {
+    setup();
+    next_event(EV_CLASS_APPLICATION, EV_APP_ACTIVATED);
+    memset(&sunk, 0, sizeof sunk);
+    events_post_mouse(1, 2, true); /* not modal: dropped */
+    events_post_text("x");
+    events_set_modal(&sink);
+    events_post_key(SDL_SCANCODE_LSHIFT, true, false);
+    events_post_key(SDL_SCANCODE_A, true, false);
+    events_post_key(SDL_SCANCODE_A, true, true); /* a repeat types again */
+    events_post_key(SDL_SCANCODE_A, false, false);
+    events_post_mouse(30, 40, true);
+    events_post_mouse(30, 40, false);
+    events_post_text("hi");
+    CHECK_EQ(events_queued(), 0);
+    CHECK_EQ(sunk.keys, 2);
+    CHECK_EQ(sunk.vkey, 0x00); /* kVK_ANSI_A */
+    CHECK_EQ(sunk.chr, 'A');
+    CHECK_EQ(sunk.mods, KM_SHIFT);
+    CHECK_EQ(sunk.downs, 1);
+    CHECK_EQ(sunk.x, 30);
+    CHECK_EQ(sunk.texts, 1);
+    CHECK_STR(sunk.text, "hi");
+    events_set_modal(NULL);
+    events_post_key(SDL_SCANCODE_LSHIFT, false, false); /* shift was tracked while modal */
+    uint32_t ev = next_event(EV_CLASS_KEYBOARD, EV_RAW_KEY_MODIFIERS_CHANGED);
+    CHECK(ev != 0);
+    CHECK_EQ(param32(ev, 0x6B6D6F64u), 0);
+    call_import("ReleaseEvent", 1, ev);
+}
+
+static int cursor_calls;
+static bool cursor_shown;
+static void on_cursor(bool visible) {
+    cursor_calls++;
+    cursor_shown = visible;
+}
+
+TEST(events_pump_shows_the_cursor_for_dialogs) {
+    setup();
+    events_set_cursor(on_cursor);
+    char err[256];
+    CHECK(script_parse("0 click 7 8\n0 type abc\n", err, sizeof err));
+    memset(&sunk, 0, sizeof sunk);
+    events_set_modal(&sink);
+    events_pump();
+    CHECK(cursor_shown);
+    CHECK_EQ(sunk.downs, 1);
+    CHECK_EQ(sunk.y, 8);
+    CHECK_STR(sunk.text, "abc");
+    events_set_modal(NULL);
+    events_pump();
+    CHECK(cursor_shown); /* the game hasn't hidden it */
+    events_set_cursor(NULL);
+    CHECK(cursor_calls >= 2);
+}
diff --git a/tests/test_script.c b/tests/test_script.c
index 8e23caa..2d9979c 100644
--- a/tests/test_script.c
+++ b/tests/test_script.c
@@ -46,3 +46,20 @@ TEST(script_load_missing_file) {
     CHECK(!script_load("/nonexistent/loony.script", err, sizeof err));
     CHECK_CONTAINS(err, "can't read /nonexistent/loony.script");
 }
+
+TEST(script_clicks_and_typing) {
+    char err[256] = "";
+    CHECK(script_parse("5 click 320 270\n6 type me@example.com  two words\n7 type\n", err, sizeof err));
+    script_action a;
+    CHECK(script_next(5, &a));
+    CHECK_EQ(a.kind, SCRIPT_CLICK);
+    CHECK_EQ(a.x, 320);
+    CHECK_EQ(a.y, 270);
+    CHECK(script_next(6, &a));
+    CHECK_EQ(a.kind, SCRIPT_TYPE);
+    CHECK_STR(a.path, "me@example.com  two words");
+    CHECK(script_next(7, &a));
+    CHECK_STR(a.path, "");
+    CHECK(!script_parse("5 click 320\n", err, sizeof err));
+    CHECK_CONTAINS(err, "line 1: click needs x and y");
+}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build`
Expected: the new tests fail to build (`events_set_modal`, `SCRIPT_CLICK`, the new `display_input` fields).

- [ ] **Step 3: Implement**

```diff
diff --git a/src/display.c b/src/display.c
index e59d60e..2756f12 100644
--- a/src/display.c
+++ b/src/display.c
@@ -17,6 +17,7 @@ static struct {
     const char *screenshot;
     display_input input;
     bool no_vsync;
+    bool cursor_hidden;
 } D;
 
 static uint8_t *screen_rgba(int *w, int *h) {
@@ -111,6 +112,16 @@ void display_set_vsync(bool on) {
         SDL_SetRenderVSync(D.renderer, on ? 1 : 0);
 }
 
+void display_set_cursor(bool visible) {
+    if (!D.sdl_ok || visible == !D.cursor_hidden)
+        return;
+    D.cursor_hidden = !visible;
+    if (visible)
+        SDL_ShowCursor();
+    else
+        SDL_HideCursor();
+}
+
 void display_poll(void) {
     if (!D.sdl_ok)
         return;
@@ -135,6 +146,15 @@ void display_poll(void) {
                     D.input.quit();
                 break;
             }
+            if ((e.key.mod & SDL_KMOD_GUI) && e.key.scancode == SDL_SCANCODE_V) {
+                if (down && D.input.paste) {
+                    char *text = SDL_GetClipboardText();
+                    if (text && *text)
+                        D.input.paste(text);
+                    SDL_free(text);
+                }
+                break;
+            }
             if ((e.key.mod & SDL_KMOD_GUI) && e.key.scancode == SDL_SCANCODE_F) {
                 if (down && !e.key.repeat) {
                     bool fs = (SDL_GetWindowFlags(D.window) & SDL_WINDOW_FULLSCREEN) != 0;
@@ -146,6 +166,15 @@ void display_poll(void) {
                 D.input.key((int)e.key.scancode, down, e.key.repeat);
             break;
         }
+        case SDL_EVENT_MOUSE_BUTTON_DOWN:
+        case SDL_EVENT_MOUSE_BUTTON_UP: {
+            if (e.button.button != SDL_BUTTON_LEFT || !D.input.mouse)
+                break;
+            float x = e.button.x, y = e.button.y;
+            SDL_RenderCoordinatesFromWindow(D.renderer, e.button.x, e.button.y, &x, &y);
+            D.input.mouse((int)SDL_floorf(x), (int)SDL_floorf(y), e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
+            break;
+        }
         default:
             break;
         }
diff --git a/src/display.h b/src/display.h
index 47d804d..3433721 100644
--- a/src/display.h
+++ b/src/display.h
@@ -22,13 +22,20 @@ typedef struct {
     void (*key)(int scancode, bool down, bool repeat); /* SDL scancode */
     void (*focus)(bool active);
     void (*quit)(void); /* window closed or Cmd-Q */
+    void (*mouse)(int x, int y, bool down); /* left button, emulated screen coordinates */
+    void (*paste)(const char *utf8);        /* Cmd-V: the clipboard's text */
 } display_input;
 void display_set_input(const display_input *in);
 
 /* Handles pending SDL events: keys go to the input callbacks, except Cmd-Q
-   (quit) and Cmd-F (toggle full screen), which the game never sees. */
+   (quit), Cmd-F (toggle full screen) and Cmd-V (paste), which the game never
+   sees. Left-button clicks are mapped from the window to the emulated
+   screen, through the letterboxing and scaling. */
 void display_poll(void);
 
+/* Shows or hides the mouse pointer over the window. */
+void display_set_cursor(bool visible);
+
 /* Writes the current screen as a PNG. */
 bool display_write_png(const char *path);
 
diff --git a/src/events.c b/src/events.c
index 6a88c1c..eccdfe1 100644
--- a/src/events.c
+++ b/src/events.c
@@ -69,6 +69,8 @@ static struct {
     ev_present_fn present;
     ev_poll_fn poll;
     ev_screenshot_fn screenshot;
+    ev_cursor_fn cursor;
+    const ev_modal_sink *modal;
     long exit_after; /* ticks, or 0 */
     double quit_deadline; /* wall-clock seconds, or 0 */
     uint32_t ae_descs; /* guest memory for the quit AppleEvent and its reply */
@@ -163,13 +165,18 @@ void events_post_key(int scancode, bool down, bool repeat) {
         else if (!down && at >= 0)
             E.held[at] = E.held[--E.nheld];
         uint32_t after = current_modifiers();
-        if (after != before) {
+        if (after != before && !E.modal) {
             ev_event *e = post(EV_CLASS_KEYBOARD, EV_RAW_KEY_MODIFIERS_CHANGED);
             if (e)
                 e->modifiers = after;
         }
         return;
     }
+    if (E.modal) {
+        if (down)
+            E.modal->key((uint32_t)k.vkey, keymap_char(&k, current_modifiers()), current_modifiers());
+        return;
+    }
     uint32_t kind = !down ? EV_RAW_KEY_UP : repeat ? EV_RAW_KEY_REPEAT : EV_RAW_KEY_DOWN;
     ev_event *e = post(EV_CLASS_KEYBOARD, kind);
     if (e) {
@@ -179,6 +186,20 @@ void events_post_key(int scancode, bool down, bool repeat) {
     }
 }
 
+void events_post_mouse(int x, int y, bool down) {
+    if (E.modal)
+        E.modal->mouse(x, y, down);
+}
+
+void events_post_text(const char *utf8) {
+    if (E.modal)
+        E.modal->text(utf8);
+}
+
+void events_set_modal(const ev_modal_sink *sink) { E.modal = sink; }
+
+void events_set_cursor(ev_cursor_fn fn) { E.cursor = fn; }
+
 void events_post_activation(bool active) {
     post(EV_CLASS_APPLICATION, active ? EV_APP_ACTIVATED : EV_APP_DEACTIVATED);
 }
@@ -353,6 +374,11 @@ static void run_script(void) {
                 log_msg("script: can't write the screenshot %s", a.path);
             break;
         case SCRIPT_QUIT: events_request_quit(); break;
+        case SCRIPT_CLICK:
+            events_post_mouse(a.x, a.y, true);
+            events_post_mouse(a.x, a.y, false);
+            break;
+        case SCRIPT_TYPE: events_post_text(a.path); break;
         }
     }
 }
@@ -364,6 +390,8 @@ void events_pump(void) {
     sound_pump();
     if (E.loop_depth > 0)
         fire_due_timers();
+    if (E.cursor)
+        E.cursor(misc_cursor_visible() || E.modal);
     if (E.present)
         E.present();
     if (E.exit_after > 0 && misc_ticks() >= (uint32_t)E.exit_after) {
diff --git a/src/events.h b/src/events.h
index b1147ac..d71abc1 100644
--- a/src/events.h
+++ b/src/events.h
@@ -71,6 +71,30 @@ void events_set_screenshot(ev_screenshot_fn fn);
    auto-repeat). Modifier keys become kEventRawKeyModifiersChanged, others
    kEventRawKeyDown, Up or Repeat with 'kcod', 'kchr' and 'kmod'. */
 void events_post_key(int scancode, bool down, bool repeat);
+/* The left mouse button went down or up at (x, y) on the emulated screen.
+   Only a modal sink sees mouse input; the game uses none. */
+void events_post_mouse(int x, int y, bool down);
+
+/* Text to insert (Cmd-V, or a script's "type"), UTF-8. Only a modal sink
+   sees it. */
+void events_post_text(const char *utf8);
+
+/* While a modal dialog runs, it takes the input instead of the game: key
+   downs and repeats (with their Mac key code, character and modifiers),
+   mouse buttons and text go to the sink, and nothing is queued for the
+   game. Modifier and key-up state is still tracked. NULL ends modal input. */
+typedef struct {
+    void (*key)(uint32_t vkey, uint8_t chr, uint32_t modifiers);
+    void (*mouse)(int x, int y, bool down);
+    void (*text)(const char *utf8);
+} ev_modal_sink;
+void events_set_modal(const ev_modal_sink *sink);
+
+/* Called on every pump with whether the mouse pointer should show: when the
+   game hasn't hidden it (HideCursor), or while a modal dialog runs. */
+typedef void (*ev_cursor_fn)(bool visible);
+void events_set_cursor(ev_cursor_fn fn);
+
 /* The window gained or lost focus: kEventAppActivated / Deactivated. */
 void events_post_activation(bool active);
 /* The user asked to quit (window close, Cmd-Q, a script). Queues the quit
diff --git a/src/main.c b/src/main.c
index 26f0ac0..99c18ea 100644
--- a/src/main.c
+++ b/src/main.c
@@ -82,8 +82,9 @@ int main(int argc, char **argv) {
     events_set_present(display_present_if_dirty);
     events_set_poll(display_poll);
     events_set_screenshot(display_write_png);
+    events_set_cursor(display_set_cursor);
     static const display_input input = {events_post_key, events_post_activation,
-                                        events_request_quit};
+                                        events_request_quit, events_post_mouse, events_post_text};
     display_set_input(&input);
     display_set_vsync(!misc_fixed_clock());
     sound_start_output();
diff --git a/src/script.c b/src/script.c
index 2a1f7a2..7c1d772 100644
--- a/src/script.c
+++ b/src/script.c
@@ -69,6 +69,21 @@ bool script_parse(const char *text, char *err, size_t errlen) {
             snprintf(a->path, sizeof a->path, "%s", arg);
         } else if (strcmp(verb, "quit") == 0) {
             a->kind = SCRIPT_QUIT;
+        } else if (strcmp(verb, "click") == 0) {
+            a->kind = SCRIPT_CLICK;
+            if (sscanf(s, "%*u %*s %d %d", &a->x, &a->y) != 2) {
+                snprintf(err, errlen, "line %d: click needs x and y", line_no);
+                return false;
+            }
+        } else if (strcmp(verb, "type") == 0) {
+            a->kind = SCRIPT_TYPE;
+            const char *text = strstr(s, "type") + 4;
+            if (*text == ' ')
+                text++;
+            snprintf(a->path, sizeof a->path, "%s", text);
+            size_t tl = strlen(a->path);
+            if (tl && a->path[tl - 1] == '\r')
+                a->path[tl - 1] = '\0';
         } else {
             snprintf(err, errlen, "line %d: unknown action \"%s\"", line_no, verb);
             return false;
diff --git a/src/script.h b/src/script.h
index 7b30da6..f328f57 100644
--- a/src/script.h
+++ b/src/script.h
@@ -8,16 +8,26 @@
      <tick> up <key>          release it
      <tick> screenshot <file> write the screen as a PNG
      <tick> quit              ask the game to quit (the quit Apple Event)
+     <tick> click <x> <y>     click the left button at (x, y) on the emulated screen
+     <tick> type <text>       type the rest of the line into a dialog, as Cmd-V would
    Blank lines and lines starting with '#' are ignored. Ticks are 1/60 s
    since launch and must not decrease. */
 
-typedef enum { SCRIPT_KEY_DOWN, SCRIPT_KEY_UP, SCRIPT_SCREENSHOT, SCRIPT_QUIT } script_kind;
+typedef enum {
+    SCRIPT_KEY_DOWN,
+    SCRIPT_KEY_UP,
+    SCRIPT_SCREENSHOT,
+    SCRIPT_QUIT,
+    SCRIPT_CLICK,
+    SCRIPT_TYPE,
+} script_kind;
 
 typedef struct {
     uint32_t tick;
     script_kind kind;
     int scancode;       /* key actions */
-    char path[256];     /* screenshot */
+    char path[256];     /* screenshot; the text to type */
+    int x, y;           /* click */
 } script_action;
 
 /* Parses a script file. On failure writes err (with the line number) and
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build build && ./build/loony_tests events_ && ./build/loony_tests display_ && ./build/loony_tests script_ && ./build/loony_tests`
Expected: all pass (the registration test skips without `LOONY_TEST_EMAIL`/`LOONY_TEST_KEY`); no sanitizer reports.

- [ ] **Step 5: Commit**

```bash
git add src/display.c src/display.h src/events.c src/events.h src/main.c src/script.c src/script.h tests/test_display.c tests/test_events.c tests/test_script.c
git commit -m "Mouse clicks, Cmd-V paste and modal input; script click and type; cursor follows HideCursor"
```

---

### Task 6: Dialogs on the screen

**Files:**
- Modify: `src/blit.c`
- Modify: `src/dialogs.c`
- Modify: `src/dialogs.h`
- Modify: `src/memmgr.c`
- Modify: `src/memmgr.h`
- Modify: `src/pict.c`
- Modify: `src/pict.h`
- Modify: `src/qd.c`
- Modify: `src/qd.h`
- Modify: `src/trap.c`
- Modify: `src/trap.h`
- Modify: `tests/test_blit.c`
- Modify: `tests/test_dialogs.c`
- Modify: `tests/test_memmgr.c`
- Modify: `tests/test_pef.c`
- Modify: `tests/test_pict.c`
- Modify: `tests/test_run.c`

**Interfaces:**
- Produces: drawn `Alert`/`StopAlert` with a modal loop; `GetNewDialog`, `ModalDialog`, `GetDialogItem`, `GetDialogItemText`, `DisposeDialog`; `LOONY_AUTO_ALERTS`; `DLG_*` constants (`dialogs.h`); `mm_set_handle_size` (`memmgr.h`); `qd_mark_dirty` (`qd.h`); `trap_has_handler` (`trap.h`); PICT `DirectBitsRect` and QuickTime mattes; direct-to-indexed `qd_blit`.
- Consumes: `font.h` (Task 4) and `events_set_modal` (Task 5).

- [ ] **Step 1: Write the failing tests**

```diff
diff --git a/tests/test_blit.c b/tests/test_blit.c
index 0801d05..737f169 100644
--- a/tests/test_blit.c
+++ b/tests/test_blit.c
@@ -118,17 +118,27 @@ TEST(blit_4bit_pixels_pack_two_per_byte) {
     CHECK_EQ(d[1], 0);
 }
 
-TEST(blit_rejects_unsupported_modes_and_conversions) {
+TEST(blit_rejects_unsupported_modes) {
     qd_palette p;
     qd_std_palette(8, &p);
     uint8_t s[8] = {0}, d[8] = {0};
-    qd_pixels s8 = px(s, 2, 1, 8, &p), d8 = px(d, 2, 1, 8, &p), s16 = px(s, 2, 1, 16, NULL);
+    qd_pixels s8 = px(s, 2, 1, 8, &p), d8 = px(d, 2, 1, 8, &p);
     char err[128] = "";
     CHECK(!qd_blit(&s8, s8.bounds, &d8, d8.bounds, d8.bounds, 36, BLACK, WHITE, err, sizeof err));
     CHECK_CONTAINS(err, "transfer mode 36");
-    CHECK(!qd_blit(&s16, s16.bounds, &d8, d8.bounds, d8.bounds, QD_SRC_COPY, BLACK, WHITE, err,
-                   sizeof err));
-    CHECK_CONTAINS(err, "16-bit pixels to 8 bits");
+}
+
+TEST(blit_direct_to_indexed_picks_the_nearest_color) {
+    qd_palette p;
+    qd_std_palette(8, &p);
+    uint8_t s[8] = {0x00, 0xFF, 0xFF, 0xFF, 0x00, 0xFE, 0x01, 0x02}; /* white, almost red */
+    uint8_t d[2] = {7, 7};
+    qd_pixels s32 = px(s, 2, 1, 32, NULL), d8 = px(d, 2, 1, 8, &p);
+    char err[128] = "";
+    CHECK(qd_blit(&s32, s32.bounds, &d8, d8.bounds, d8.bounds, QD_SRC_COPY, BLACK, WHITE, err,
+                  sizeof err));
+    CHECK_EQ(d[0], 0);  /* the standard palette's white */
+    CHECK_EQ(d[1], 35); /* its pure red (0xFFFF, 0, 0) */
 }
 
 TEST(blit_fill_and_rgba_conversion) {
diff --git a/tests/test_dialogs.c b/tests/test_dialogs.c
index 9db1356..1f68ff3 100644
--- a/tests/test_dialogs.c
+++ b/tests/test_dialogs.c
@@ -3,16 +3,32 @@
 #include <stdlib.h>
 
 #include "dialogs.h"
+#include "events.h"
 #include "harness.h"
+#include "memmgr.h"
+#include "misc.h"
+#include "qd.h"
 #include "rsrc.h"
+#include "script.h"
 
-static const char *const names[] = {"Alert", "StopAlert", "ParamText"};
+static const char *const names[] = {
+    "Alert", "StopAlert", "ParamText", "GetNewDialog", "ModalDialog", "GetDialogItem",
+    "GetDialogItemText", "DisposeDialog",
+};
 static uint8_t *fork_buf;
 
+/* A fixed-clock machine with an 800x600 screen and the game's resources.
+   The modal loop runs the script, so tests drive dialogs with it. */
 static bool setup(void) {
     if (!test_game_present())
         return false;
-    harness_init(names, 3);
+    harness_init(names, sizeof names / sizeof names[0]);
+    mm_init();
+    setenv("LOONY_FIXED_CLOCK", "1", 1);
+    misc_init();
+    unsetenv("LOONY_FIXED_CLOCK");
+    qd_init(800, 600, 8);
+    events_init();
     dialogs_init();
     dialogs_register();
     if (!fork_buf) {
@@ -27,6 +43,26 @@ static bool setup(void) {
     return true;
 }
 
+static void script(const char *text) {
+    char err[256];
+    if (!script_parse(text, err, sizeof err))
+        fatal("bad test script: %s", err);
+}
+
+static uint32_t screen_hash(void) {
+    qd_pixels px;
+    qd_palette pal;
+    qd_screen(&px, &pal);
+    return fnv1a32(px.base, (size_t)px.row_bytes * (size_t)rect_h(px.bounds));
+}
+
+static uint8_t screen_at(int x, int y) {
+    qd_pixels px;
+    qd_palette pal;
+    qd_screen(&px, &pal);
+    return px.base[(size_t)y * px.row_bytes + (size_t)x];
+}
+
 TEST(dialogs_alert_text_lists_buttons_and_text) {
     SKIP_UNLESS_GAME();
     CHECK(setup());
@@ -37,24 +73,153 @@ TEST(dialogs_alert_text_lists_buttons_and_text) {
     CHECK_STR(text, "");
 }
 
-static void child_alert(void *unused) {
+static void child_auto_alert(void *unused) {
     (void)unused;
+    setenv("LOONY_AUTO_ALERTS", "1", 1);
     if (!setup())
         exit(3);
     if (call_import("Alert", 2, 901u, 0u) != 1)
         exit(4);
     if (call_import("StopAlert", 2, 900u, 0u) != 1)
         exit(5);
+    if (misc_ticks() != 0) /* no waiting */
+        exit(6);
 }
 
-TEST(dialogs_alert_answers_the_default_item_and_logs) {
+TEST(dialogs_auto_alerts_answer_the_default_item_at_once) {
     SKIP_UNLESS_GAME();
     char out[16384];
-    CHECK_EQ(test_run_child(child_alert, NULL, out, sizeof out), 0);
+    CHECK_EQ(test_run_child(child_auto_alert, NULL, out, sizeof out), 0);
     CHECK_CONTAINS(out, "loony: Alert 901 (answering item 1): Play Demo");
     CHECK_CONTAINS(out, "loony: Alert 900 (answering item 1): OK");
 }
 
+static void child_alert_waits(void *unused) {
+    (void)unused;
+    if (!setup())
+        exit(3);
+    uint32_t before = screen_hash();
+    script("10 down return\n11 up return\n");
+    if (call_import("Alert", 2, 901u, 0u) != 1)
+        exit(4);
+    if (misc_ticks() < 10) /* it waited for Return */
+        exit(5);
+    if (screen_hash() != before) /* and put the screen back */
+        exit(6);
+}
+
+TEST(dialogs_alert_waits_for_return_and_restores_the_screen) {
+    SKIP_UNLESS_GAME();
+    char out[16384];
+    CHECK_EQ(test_run_child(child_alert_waits, NULL, out, sizeof out), 0);
+    CHECK_CONTAINS(out, "loony: Alert 901: Play Demo | Quit");
+    CHECK_CONTAINS(out, "loony: Alert 901: answered item 1 (Play Demo)");
+}
+
+/* Alert 901 sits at (139, 150) on the 800x600 screen (alert position:
+   centered, a third of the way down); "Enter Key-Code" is item 4 at
+   (110, 260)-(130, 388) inside it, and "Quit" item 2 at (110, 400). */
+static void child_alert_click(void *unused) {
+    (void)unused;
+    if (!setup())
+        exit(3);
+    /* A click outside every button and Esc (901 has no Cancel) do nothing. */
+    script("5 click 9999 9999\n10 down esc\n11 up esc\n20 click 460 270\n");
+    if (call_import("Alert", 2, 901u, 0u) != 4)
+        exit(4);
+}
+
+TEST(dialogs_alert_buttons_answer_clicks) {
+    SKIP_UNLESS_GAME();
+    char out[16384];
+    CHECK_EQ(test_run_child(child_alert_click, NULL, out, sizeof out), 0);
+    CHECK_CONTAINS(out, "loony: Alert 901: answered item 4 (Enter Key-Code)");
+}
+
+static uint32_t item_text(uint32_t dlg, int n, char *out) {
+    uint32_t type = scratch(2), h = scratch(4), box = scratch(8), str = scratch(256);
+    call_import("GetDialogItem", 5, dlg, (uint32_t)n, type, h, box);
+    call_import("GetDialogItemText", 2, gm_r32(h), str);
+    gm_read_pstr(str, out);
+    return gm_r16(type);
+}
+
+/* DLOG 911 (centered at the alert position): e-mail field item 5, key
+   field item 6, Register item 3 (the default), Cancel item 4. */
+static void child_registration_form(void *unused) {
+    (void)unused;
+    if (!setup())
+        exit(3);
+    uint32_t before = screen_hash();
+    uint32_t dlg = call_import("GetNewDialog", 3, 911u, 0u, 0xFFFFFFFFu);
+    if (dlg < DLG_TAG_BASE)
+        exit(4);
+    if (screen_hash() == before) /* drawn at once: the DLOG is visible */
+        exit(5);
+    script("5 type me@example.com\n6 down backspace\n7 up backspace\n8 type m\n9 down tab\n10 up tab\n"
+           "11 type ab\xc3\xa9 cd\n12 down lshift\n13 down tab\n14 up tab\n15 up lshift\n"
+           "16 type !\n20 down return\n21 up return\n");
+    uint32_t hit = scratch(2);
+    call_import("ModalDialog", 2, 0u, hit);
+    if (gm_r16(hit) != 3)
+        exit(6);
+    char t[256];
+    if (item_text(dlg, 5, t) != DLG_ITEM_EDIT_TEXT || strcmp(t, "me@example.com!") != 0)
+        exit(7);
+    if (item_text(dlg, 6, t) != DLG_ITEM_EDIT_TEXT || strcmp(t, "ab cd") != 0) /* only ASCII is typed */
+        exit(8);
+    if (item_text(dlg, 7, t) != (DLG_ITEM_STATIC_TEXT | DLG_ITEM_DISABLED) || strcmp(t, "E-mail address:") != 0)
+        exit(9);
+    uint32_t type = scratch(2), h = scratch(4), box = scratch(8);
+    call_import("GetDialogItem", 5, dlg, 3u, type, h, box);
+    if (gm_r16(type) != DLG_ITEM_BUTTON || gm_r32(h) != 0 || gm_r16(box) != 170 || gm_r16(box + 2) != 320)
+        exit(10);
+    script("30 down esc\n31 up esc\n");
+    call_import("ModalDialog", 2, 0u, hit);
+    if (gm_r16(hit) != 4) /* Esc is Cancel */
+        exit(11);
+    call_import("DisposeDialog", 1, dlg);
+    if (screen_hash() != before)
+        exit(12);
+    call_import("GetDialogItem", 5, dlg, 3u, type, h, box); /* disposed: crashes */
+}
+
+TEST(dialogs_registration_form_takes_typing_and_buttons) {
+    SKIP_UNLESS_GAME();
+    char out[16384];
+    CHECK_EQ(test_run_child(child_registration_form, NULL, out, sizeof out), 2);
+    CHECK_CONTAINS(out, "loony: GetNewDialog 911");
+    CHECK_CONTAINS(out, "GetDialogItem: 0x0b000000 is not a dialog");
+}
+
+static void child_click_fields(void *unused) {
+    (void)unused;
+    if (!setup())
+        exit(3);
+    uint32_t dlg = call_import("GetNewDialog", 3, 911u, 0u, 0xFFFFFFFFu);
+    /* 911 is 440x210, so it sits at (180, 130). The key field (130, 140) ->
+       (260, 320); Register (170, 320) -> (300, 500). Clicking the key field
+       moves the caret there. */
+    script("5 click 340 265\n6 type K1\n7 click 510 305\n");
+    uint32_t hit = scratch(2);
+    call_import("ModalDialog", 2, 0u, hit);
+    char t[256];
+    if (gm_r16(hit) != 3)
+        exit(4);
+    if (item_text(dlg, 5, t), strcmp(t, "") != 0)
+        exit(5);
+    if (item_text(dlg, 6, t), strcmp(t, "K1") != 0)
+        exit(6);
+    if (screen_at(0, 0) != 0) /* the screen outside the dialog is untouched (white) */
+        exit(7);
+}
+
+TEST(dialogs_clicks_focus_fields_and_press_buttons) {
+    SKIP_UNLESS_GAME();
+    char out[16384];
+    CHECK_EQ(test_run_child(child_click_fields, NULL, out, sizeof out), 0);
+}
+
 TEST(dialogs_param_text_substitutes) {
     SKIP_UNLESS_GAME();
     CHECK(setup());
@@ -72,3 +237,34 @@ TEST(dialogs_param_text_substitutes) {
     dialogs_alert_text(9000, text, sizeof text);
     CHECK_STR(text, "OK | ");
 }
+
+static void child_filter(void *unused) {
+    (void)unused;
+    setup();
+    call_import("Alert", 2, 901u, 0x1234u);
+}
+
+static void child_storage(void *unused) {
+    (void)unused;
+    setup();
+    call_import("GetNewDialog", 3, 911u, 0x1234u, 0xFFFFFFFFu);
+}
+
+static void child_auto_modal(void *unused) {
+    (void)unused;
+    setenv("LOONY_AUTO_ALERTS", "1", 1);
+    setup();
+    call_import("GetNewDialog", 3, 911u, 0u, 0xFFFFFFFFu);
+    call_import("ModalDialog", 2, 0u, scratch(2));
+}
+
+TEST(dialogs_unsupported_uses_crash_with_a_report) {
+    SKIP_UNLESS_GAME();
+    char out[16384];
+    CHECK_EQ(test_run_child(child_filter, NULL, out, sizeof out), 2);
+    CHECK_CONTAINS(out, "Alert: filter procs are not supported");
+    CHECK_EQ(test_run_child(child_storage, NULL, out, sizeof out), 2);
+    CHECK_CONTAINS(out, "GetNewDialog: caller-supplied dialog storage is not supported");
+    CHECK_EQ(test_run_child(child_auto_modal, NULL, out, sizeof out), 2);
+    CHECK_CONTAINS(out, "ModalDialog: DLOG 911 can't be answered automatically");
+}
diff --git a/tests/test_memmgr.c b/tests/test_memmgr.c
index b03de4e..a7b8419 100644
--- a/tests/test_memmgr.c
+++ b/tests/test_memmgr.c
@@ -139,3 +139,24 @@ TEST(mm_handle_state_is_recorded) {
     CHECK_EQ(mm_handle_state(h), MM_STATE_LOCKED | MM_STATE_RESOURCE);
     CHECK(mm_is_handle(h));
 }
+
+TEST(mm_set_handle_size_grows_in_place_or_moves) {
+    fresh_heap();
+    uint32_t h = mm_new_handle(5, true);
+    memcpy(gm_ptr(gm_r32(h), 5), "hello", 5);
+    uint32_t d = gm_r32(h);
+    CHECK_EQ(mm_set_handle_size(h, 12), MM_NO_ERR); /* within the 16-byte block */
+    CHECK_EQ(gm_r32(h), d);
+    CHECK_EQ(mm_handle_size(h), 12);
+    uint32_t blocker = mm_new_ptr(16, false); /* the next block is taken */
+    CHECK_EQ(mm_set_handle_size(h, 300), MM_NO_ERR);
+    CHECK(gm_r32(h) != d);
+    CHECK_EQ(mm_handle_size(h), 300);
+    CHECK(memcmp(gm_ptr(gm_r32(h), 5), "hello", 5) == 0);
+    CHECK_EQ(mm_recover_handle(gm_r32(h)), h);
+    CHECK_EQ(mm_set_handle_size(h, 2), MM_NO_ERR);
+    CHECK_EQ(mm_handle_size(h), 2);
+    CHECK_EQ(mm_set_handle_size(blocker, 2), MM_MEM_WZ_ERR);
+    CHECK_EQ(mm_set_handle_size(h, GUEST_HEAP_SIZE), MM_MEM_FULL_ERR);
+    CHECK_EQ(mm_handle_size(h), 2);
+}
diff --git a/tests/test_pef.c b/tests/test_pef.c
index 718c24d..f9c71be 100644
--- a/tests/test_pef.c
+++ b/tests/test_pef.c
@@ -2,7 +2,18 @@
 
 #include <stdlib.h>
 
+#include "cf.h"
+#include "dialogs.h"
+#include "events.h"
+#include "files.h"
+#include "memmgr.h"
+#include "misc.h"
 #include "pef.h"
+#include "ppc.h"
+#include "qd.h"
+#include "rsrc.h"
+#include "sound.h"
+#include "trap.h"
 #include "util.h"
 
 TEST(pef_parses_the_real_executable) {
@@ -82,3 +93,38 @@ TEST(pef_rejects_truncated_executable) {
     }
     free(buf);
 }
+
+/* Every import the game makes has a C implementation. */
+TEST(pef_every_import_has_a_handler) {
+    SKIP_UNLESS_GAME();
+    size_t len;
+    uint8_t *buf = read_file(test_game_exe_path(), &len);
+    CHECK(buf != NULL);
+    pef_file pef;
+    char err[256] = "";
+    CHECK(pef_parse(buf, len, &pef, err, sizeof err));
+    const char **names = calloc(pef.nimports, sizeof *names);
+    for (uint32_t i = 0; i < pef.nimports; i++)
+        names[i] = pef.imports[i].name;
+    fresh_machine();
+    trap_init(pef.nimports, names, GUEST_IMAGE_BASE, 0x10000);
+    mm_register();
+    rsrc_register();
+    misc_register();
+    cf_register();
+    qd_register();
+    dialogs_register();
+    events_register();
+    files_register();
+    sound_register();
+    int missing = 0;
+    for (uint32_t i = 0; i < pef.nimports; i++)
+        if (pef.imports[i].sym_class != PEF_SYM_DATA && !trap_has_handler(i)) {
+            fprintf(stderr, "  no handler: %s\n", names[i]);
+            missing++;
+        }
+    trap_shutdown();
+    free(names);
+    free(buf);
+    CHECK_EQ(missing, 0);
+}
diff --git a/tests/test_pict.c b/tests/test_pict.c
index d5218f3..59e44ca 100644
--- a/tests/test_pict.c
+++ b/tests/test_pict.c
@@ -98,17 +98,29 @@ TEST(pict_scales_to_the_destination_rect) {
     CHECK(mid != 0);
 }
 
-TEST(pict_reports_quicktime_pictures) {
+/* The dialogs' icons: a QuickTime matte to skip, then a 32-bit
+   DirectBitsRect packed as component planes. */
+TEST(pict_draws_the_dialog_icons) {
     SKIP_UNLESS_GAME();
-    rsrc_entry *e = pict(128);
-    CHECK(e != NULL);
-    canvas c;
-    canvas_init(&c, 104, 128);
-    char err[256] = "";
-    bool ok = draw(e, &c, err);
-    free(c.buf);
-    CHECK(!ok);
-    CHECK_CONTAINS(err, "picture opcode 0x8201 is not supported");
+    for (int16_t id = 128; id <= 129; id++) {
+        rsrc_entry *e = pict(id);
+        CHECK(e != NULL);
+        canvas c;
+        canvas_init(&c, 104, 128);
+        char err[256] = "";
+        bool ok = draw(e, &c, err);
+        int distinct = 0;
+        bool seen[256] = {false};
+        for (int i = 0; i < 104 * 128; i++)
+            if (!seen[c.buf[i]]) {
+                seen[c.buf[i]] = true;
+                distinct++;
+            }
+        free(c.buf);
+        CHECK_STR(err, "");
+        CHECK(ok);
+        CHECK(distinct > 8); /* a picture, not a flat fill */
+    }
 }
 
 TEST(pict_rejects_truncated_pictures) {
diff --git a/tests/test_run.c b/tests/test_run.c
index b0a3664..c8b6adf 100644
--- a/tests/test_run.c
+++ b/tests/test_run.c
@@ -10,9 +10,15 @@
    another's frames. Otherwise the runner's own temporary folder is used. */
 static char run_data[1024];
 
+/* The golden frames predate dialogs, so runs answer alerts at once unless a
+   test asks for real ones. */
+static bool real_alerts;
+
 static void run_loony(void *dir) {
     if (run_data[0])
         setenv("LOONY_DATA_DIR", run_data, 1);
+    if (!real_alerts)
+        setenv("LOONY_AUTO_ALERTS", "1", 1);
     execl(LOONY_BIN, "loony", (const char *)dir, (char *)NULL);
     fprintf(stderr, "exec %s failed\n", LOONY_BIN);
     _exit(127);
@@ -247,6 +253,82 @@ TEST(run_preferences_are_saved_at_quit_and_read_at_launch) {
     free(wav);
 }
 
+/* Without LOONY_AUTO_ALERTS the shareware alerts wait for an answer. Alert
+   901 sits at (139, 150); its "Enter Key-Code" button is at (399, 260). The
+   registration form, DLOG 911, sits at (180, 130). The user approved these
+   frames on (pending: shown in the Plan 6 handoff). */
+TEST(run_the_shareware_alerts_wait_for_an_answer) {
+    SKIP_UNLESS_GAME();
+    real_alerts = true;
+    uint32_t ticks[1] = {30}, shots[1];
+    char out[32768];
+    int status = run_script("60 down return\n62 up return\n90 down return\n92 up return\n"
+                            "400 quit\n",
+                            ticks, shots, 1, out, sizeof out);
+    real_alerts = false;
+    CHECK_EQ(status, 0);
+    CHECK_CONTAINS(out, "loony: Alert 901: answered item 1 (Play Demo)");
+    CHECK_CONTAINS(out, "loony: Alert 900: answered item 1 (OK)");
+    CHECK(strstr(out, "Alert 900: answered") < strstr(out, "sending the quit Apple Event"));
+    CHECK_EQ(shots[0], 0x94D533D8u); /* Alert 901 */
+}
+
+TEST(run_a_wrong_key_code_is_refused) {
+    SKIP_UNLESS_GAME();
+    real_alerts = true;
+    uint32_t ticks[2] = {80, 110}, shots[2];
+    char out[32768];
+    int status = run_script("20 click 460 270\n"
+                            "50 type nobody@example.com\n60 down tab\n61 up tab\n"
+                            "70 type ABCD-1234-EFGH\n90 down return\n91 up return\n"
+                            "120 down return\n121 up return\n"   /* 902's OK */
+                            "140 down return\n141 up return\n"   /* 901: Play Demo */
+                            "160 down return\n161 up return\n"   /* 900: OK */
+                            "400 quit\n",
+                            ticks, shots, 2, out, sizeof out);
+    real_alerts = false;
+    CHECK_EQ(status, 0);
+    CHECK_CONTAINS(out, "loony: Alert 901: answered item 4 (Enter Key-Code)");
+    CHECK_CONTAINS(out, "loony: GetNewDialog 911");
+    CHECK_CONTAINS(out, "loony: Alert 902: answered item 1 (OK)");
+    CHECK(!strstr(out, "Alert 903"));
+    CHECK(!strstr(out, "nobody@example.com")); /* typed text is never logged */
+    CHECK_EQ(shots[0], 0x6E85582Au); /* the filled-in form */
+    CHECK_EQ(shots[1], 0x8CF0D4FDu); /* Alert 902 */
+}
+
+/* Registers with a real key code, given at run time (never stored):
+   LOONY_TEST_EMAIL and LOONY_TEST_KEY. The license is kept in the
+   preferences, so the next launch skips the shareware alerts. */
+TEST(run_a_key_code_registers_and_survives_a_relaunch) {
+    SKIP_UNLESS_GAME();
+    const char *email = getenv("LOONY_TEST_EMAIL"), *key = getenv("LOONY_TEST_KEY");
+    if (!email || !*email || !key || !*key) {
+        test_skip("LOONY_TEST_EMAIL and LOONY_TEST_KEY aren't set");
+        return;
+    }
+    test_tmp_dir(run_data, sizeof run_data);
+    real_alerts = true;
+    char actions[1024];
+    snprintf(actions, sizeof actions,
+             "20 click 460 270\n50 type %s\n60 down tab\n61 up tab\n70 type %s\n"
+             "90 down return\n91 up return\n120 down return\n121 up return\n600 quit\n",
+             email, key);
+    char out[32768];
+    int status = run_script(actions, NULL, NULL, 0, out, sizeof out);
+    CHECK_EQ(status, 0);
+    CHECK_CONTAINS(out, "loony: Alert 903: answered item 1 (OK)");
+    CHECK(!strstr(out, "Alert 900"));
+    status = run_script("300 quit\n", NULL, NULL, 0, out, sizeof out);
+    real_alerts = false;
+    test_remove_tree(run_data);
+    run_data[0] = '\0';
+    CHECK_EQ(status, 0);
+    CHECK(!strstr(out, "Alert 90"));
+    CHECK(!strstr(out, email));
+    CHECK(!strstr(out, key));
+}
+
 static void run_loony_bad_script(void *dir) {
     setenv("LOONY_SCRIPT", "/nonexistent/loony.script", 1);
     run_loony(dir);
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build`
Expected: the dialog tests fail to build (`DLG_TAG_BASE`, `mm_set_handle_size`, `trap_has_handler`).

- [ ] **Step 3: Implement**

```diff
diff --git a/src/blit.c b/src/blit.c
index 3ba5bab..c892fe5 100644
--- a/src/blit.c
+++ b/src/blit.c
@@ -167,9 +167,6 @@ bool qd_blit(const qd_pixels *src, qd_rect sr, const qd_pixels *dst, qd_rect dr,
         return fail(err, errlen, "transfer mode %d is not supported", mode);
     if (rect_empty(sr) || rect_empty(dr))
         return true;
-    if (indexed(dst->depth) && !indexed(src->depth))
-        return fail(err, errlen, "copying %d-bit pixels to %d bits is not supported", src->depth,
-                    dst->depth);
     /* Translate source pixel values into destination pixel values once. */
     static uint32_t map[256];
     bool use_map = indexed(src->depth);
@@ -199,8 +196,8 @@ bool qd_blit(const qd_pixels *src, qd_rect sr, const qd_pixels *dst, qd_rect dr,
             uint32_t v = get_px(src, sx, sy);
             if (use_map)
                 v = map[v];
-            else if (dst->depth != src->depth)
-                v = qd_pixel_for(rgb_of(v, src->depth, NULL), dst->depth, NULL);
+            else if (dst->depth != src->depth) /* to indexed: the nearest color */
+                v = qd_pixel_for(rgb_of(v, src->depth, NULL), dst->depth, dst->pal);
             put_px(dst, x, y, v);
         }
     }
diff --git a/src/dialogs.c b/src/dialogs.c
index a49039f..2fce921 100644
--- a/src/dialogs.c
+++ b/src/dialogs.c
@@ -1,19 +1,83 @@
 #include "dialogs.h"
 
 #include <stdio.h>
+#include <stdlib.h>
 #include <string.h>
+#include <strings.h>
 
+#include "events.h"
+#include "font.h"
 #include "guest_mem.h"
+#include "keymap.h"
+#include "memmgr.h"
+#include "misc.h"
+#include "pict.h"
+#include "qd.h"
 #include "rsrc.h"
 #include "trap.h"
 #include "util.h"
 
-#define ITEM_STATIC_TEXT 8
-#define ITEM_BUTTON 4
+#define MAX_ITEMS 16
+#define MAX_TEXT 255
+#define FRAME 4 /* the border drawn outside a dialog's rectangle */
 
-static char param[4][256]; /* ParamText ^0..^3 */
+/* Window positions (the ALRT and DLOG position word). */
+#define POS_CENTER_MAIN 0x280A
+#define POS_ALERT_MAIN 0x300A
 
-void dialogs_init(void) { memset(param, 0, sizeof param); }
+/* Mac virtual key codes. */
+#define VK_RETURN 0x24
+#define VK_ENTER 0x4C
+#define VK_TAB 0x30
+#define VK_DELETE 0x33
+#define VK_ESCAPE 0x35
+#define VK_PERIOD 0x2F
+
+typedef struct {
+    uint8_t type; /* without DLG_ITEM_DISABLED */
+    bool disabled;
+    qd_rect r;    /* local to the dialog */
+    uint8_t text[MAX_TEXT + 1]; /* button title, static text, an edit field's contents */
+    int len;
+    int16_t res_id;  /* pictures and icons */
+    uint32_t handle; /* edit and static text: a guest handle holding the text */
+} item;
+
+typedef struct {
+    bool open;
+    int16_t res_id; /* the ALRT or DLOG */
+    bool is_alert;
+    qd_rect bounds; /* on the screen */
+    item items[MAX_ITEMS];
+    int nitems;
+    int default_item, cancel_item; /* 1-based, 0 = none */
+    int focus;      /* the edit field with the caret (0-based), -1 = none */
+    int pressed;    /* the button under a held mouse button (0-based), -1 = none */
+    int hit;        /* the item ModalDialog or Alert returns (1-based), 0 = none yet */
+    bool shown;
+    uint8_t *saved; /* the screen under the dialog and its frame, at saved_r */
+    qd_rect saved_r, saved_screen;
+    int saved_depth;
+} dialog;
+
+static struct {
+    char param[4][256]; /* ParamText ^0..^3 */
+    bool auto_alerts;
+    dialog d[DLG_MAX];
+    dialog *front; /* the one taking input */
+    qd_palette pal;
+} G;
+
+void dialogs_init(void) {
+    for (int i = 0; i < DLG_MAX; i++)
+        free(G.d[i].saved);
+    memset(&G, 0, sizeof G);
+    const char *a = getenv("LOONY_AUTO_ALERTS");
+    G.auto_alerts = a && strcmp(a, "1") == 0;
+    font_init();
+}
+
+/* ---- text ---- */
 
 /* Appends text to out, replacing ^0..^3 with the ParamText strings and
    carriage returns with spaces. */
@@ -21,7 +85,7 @@ static void append(char *out, size_t cap, const uint8_t *text, size_t n) {
     size_t o = strlen(out);
     for (size_t i = 0; i < n && o + 1 < cap; i++) {
         if (text[i] == '^' && i + 1 < n && text[i + 1] >= '0' && text[i + 1] <= '3') {
-            const char *p = param[text[++i] - '0'];
+            const char *p = G.param[text[++i] - '0'];
             while (*p && o + 1 < cap)
                 out[o++] = *p++;
         } else {
@@ -31,6 +95,21 @@ static void append(char *out, size_t cap, const uint8_t *text, size_t n) {
     out[o] = '\0';
 }
 
+/* Like append, but keeps carriage returns and spells the result in ASCII. */
+static void display_text(const item *it, char *out, size_t cap) {
+    uint8_t mac[1024];
+    size_t o = 0;
+    for (int i = 0; i < it->len && o < sizeof mac; i++) {
+        if (it->text[i] == '^' && i + 1 < it->len && it->text[i + 1] >= '0' && it->text[i + 1] <= '3') {
+            for (const char *p = G.param[it->text[++i] - '0']; *p && o < sizeof mac; p++)
+                mac[o++] = (uint8_t)*p;
+        } else {
+            mac[o++] = it->text[i];
+        }
+    }
+    font_ascii(mac, o, out, cap);
+}
+
 void dialogs_alert_text(int16_t id, char *out, size_t cap) {
     out[0] = '\0';
     rsrc_entry *alrt = rsrc_find(FOURCC('A', 'L', 'R', 'T'), id);
@@ -48,7 +127,7 @@ void dialogs_alert_text(int16_t id, char *out, size_t cap) {
         p += 14;
         if (p + len > ditl->len)
             return;
-        if (type == ITEM_STATIC_TEXT || type == ITEM_BUTTON) {
+        if (type == DLG_ITEM_STATIC_TEXT || type == DLG_ITEM_BUTTON) {
             if (out[0])
                 append(out, cap, (const uint8_t *)" | ", 3);
             append(out, cap, d + p, len);
@@ -57,33 +136,536 @@ void dialogs_alert_text(int16_t id, char *out, size_t cap) {
     }
 }
 
-/* The default item: bit 3 of the first stage's 4 bits in the ALRT's stages
-   word picks item 2, otherwise item 1. */
+/* ---- building a dialog from its resources ---- */
+
+static qd_rect be_rect(const uint8_t *p) {
+    return (qd_rect){(int16_t)rd_be16(p), (int16_t)rd_be16(p + 2), (int16_t)rd_be16(p + 4),
+                     (int16_t)rd_be16(p + 6)};
+}
+
+static qd_rect offset(qd_rect r, int dx, int dy) {
+    return (qd_rect){(int16_t)(r.top + dy), (int16_t)(r.left + dx), (int16_t)(r.bottom + dy),
+                     (int16_t)(r.right + dx)};
+}
+
+static qd_rect outset(qd_rect r, int n) {
+    return (qd_rect){(int16_t)(r.top - n), (int16_t)(r.left - n), (int16_t)(r.bottom + n),
+                     (int16_t)(r.right + n)};
+}
+
+static qd_pixels screen(void) {
+    qd_pixels px;
+    qd_screen(&px, &G.pal);
+    px.pal = &G.pal;
+    return px;
+}
+
+/* Places r on the screen as the position word asks. */
+static qd_rect place(qd_rect r, uint16_t pos) {
+    qd_rect s = screen().bounds;
+    int w = rect_w(r), h = rect_h(r);
+    if (pos == POS_CENTER_MAIN)
+        return offset(r, s.left + (rect_w(s) - w) / 2 - r.left, s.top + (rect_h(s) - h) / 2 - r.top);
+    if (pos == POS_ALERT_MAIN)
+        return offset(r, s.left + (rect_w(s) - w) / 2 - r.left, s.top + (rect_h(s) - h) / 3 - r.top);
+    return r;
+}
+
+static bool is_control(uint8_t type) { return type >= DLG_ITEM_BUTTON && type <= DLG_ITEM_RADIO; }
+static bool has_text_handle(uint8_t type) {
+    return type == DLG_ITEM_EDIT_TEXT || type == DLG_ITEM_STATIC_TEXT;
+}
+
+static void set_handle_text(item *it) {
+    if (mm_set_handle_size(it->handle, (uint32_t)it->len) != MM_NO_ERR)
+        trap_crash("out of guest memory for dialog text");
+    if (it->len)
+        memcpy(gm_ptr(gm_r32(it->handle), (uint32_t)it->len), it->text, (size_t)it->len);
+}
+
+/* Reads DITL id into d. */
+static void load_items(const char *call, dialog *d, int16_t ditl_id) {
+    rsrc_entry *e = rsrc_find(FOURCC('D', 'I', 'T', 'L'), ditl_id);
+    if (!e || e->len < 2)
+        trap_crash("%s: DITL %d doesn't exist", call, ditl_id);
+    const uint8_t *p = rsrc_data(e);
+    uint32_t n = rd_be16(p) + 1u, at = 2;
+    if (n > MAX_ITEMS)
+        trap_crash("%s: DITL %d has %u items (at most %d supported)", call, ditl_id, n, MAX_ITEMS);
+    for (uint32_t i = 0; i < n; i++) {
+        if (at + 14 > e->len)
+            trap_crash("%s: DITL %d is truncated", call, ditl_id);
+        item *it = &d->items[i];
+        it->r = be_rect(p + at + 4);
+        it->type = p[at + 12] & 0x7F;
+        it->disabled = (p[at + 12] & DLG_ITEM_DISABLED) != 0;
+        uint8_t len = p[at + 13];
+        at += 14;
+        if (at + len > e->len)
+            trap_crash("%s: DITL %d is truncated", call, ditl_id);
+        if (it->type == DLG_ITEM_ICON || it->type == DLG_ITEM_PICTURE) {
+            it->res_id = len >= 2 ? (int16_t)rd_be16(p + at) : 0;
+        } else {
+            memcpy(it->text, p + at, len);
+            it->len = len;
+        }
+        if (has_text_handle(it->type)) {
+            it->handle = mm_new_handle(0, false);
+            if (!it->handle)
+                trap_crash("%s: out of guest memory", call);
+            set_handle_text(it);
+        }
+        at += len + (len & 1u);
+    }
+    d->nitems = (int)n;
+    d->focus = -1;
+    d->pressed = -1;
+    for (int i = 0; i < d->nitems && d->focus < 0; i++)
+        if (d->items[i].type == DLG_ITEM_EDIT_TEXT)
+            d->focus = i;
+    for (int i = 0; i < d->nitems; i++)
+        if (d->items[i].type == DLG_ITEM_BUTTON && d->items[i].len == 6 &&
+            strncasecmp((const char *)d->items[i].text, "Cancel", 6) == 0)
+            d->cancel_item = i + 1;
+}
+
+static dialog *new_dialog(const char *call) {
+    for (int i = 0; i < DLG_MAX; i++)
+        if (!G.d[i].open) {
+            memset(&G.d[i], 0, sizeof G.d[i]);
+            G.d[i].open = true;
+            return &G.d[i];
+        }
+    trap_crash("%s: more than %d dialogs open", call, DLG_MAX);
+}
+
+/* ---- drawing ---- */
+
+static const qd_rgb BLACK = {0, 0, 0}, WHITE = {0xFFFF, 0xFFFF, 0xFFFF}, GRAY = {0x8000, 0x8000, 0x8000};
+
+static void fill(qd_rect r, qd_rgb c) {
+    qd_pixels px = screen();
+    qd_fill(&px, r, px.bounds, c);
+}
+
+static void frame(qd_rect r, int thick, qd_rgb c) {
+    fill((qd_rect){r.top, r.left, (int16_t)(r.top + thick), r.right}, c);
+    fill((qd_rect){(int16_t)(r.bottom - thick), r.left, r.bottom, r.right}, c);
+    fill((qd_rect){r.top, r.left, r.bottom, (int16_t)(r.left + thick)}, c);
+    fill((qd_rect){r.top, (int16_t)(r.right - thick), r.bottom, r.right}, c);
+}
+
+static void text_at(int x, int y, const char *s, int n, qd_rgb c, qd_rect clip) {
+    qd_pixels px = screen();
+    font_draw(&px, x, y, s, n, c, clip);
+}
+
+static void draw_picture(qd_rect r, int16_t id) {
+    rsrc_entry *e = rsrc_find(FOURCC('P', 'I', 'C', 'T'), id);
+    if (!e)
+        return;
+    qd_pixels px = screen();
+    char err[128];
+    if (!pict_draw(rsrc_data(e), e->len, r, &px, r, BLACK, WHITE, err, sizeof err))
+        log_msg("dialog: can't draw PICT %d: %s", id, err);
+}
+
+/* A black-and-white 'ICON' (32x32, 1 bit). */
+static void draw_icon(qd_rect r, int16_t id) {
+    rsrc_entry *e = rsrc_find(FOURCC('I', 'C', 'O', 'N'), id);
+    if (!e || e->len < 128)
+        return;
+    const uint8_t *bits = rsrc_data(e);
+    for (int y = 0; y < 32 && r.top + y < r.bottom; y++)
+        for (int x = 0; x < 32 && r.left + x < r.right; x++)
+            if (bits[y * 4 + x / 8] & (0x80 >> (x % 8)))
+                fill((qd_rect){(int16_t)(r.top + y), (int16_t)(r.left + x), (int16_t)(r.top + y + 1),
+                               (int16_t)(r.left + x + 1)},
+                     BLACK);
+}
+
+static void draw_item(dialog *d, int i) {
+    item *it = &d->items[i];
+    qd_rect r = offset(it->r, d->bounds.left, d->bounds.top);
+    char text[1024];
+    display_text(it, text, sizeof text);
+    switch (it->type) {
+    case DLG_ITEM_BUTTON:
+    case DLG_ITEM_CHECKBOX:
+    case DLG_ITEM_RADIO: {
+        bool down = d->pressed == i;
+        if (d->default_item == i + 1)
+            frame(outset(r, 4), 3, BLACK);
+        fill(r, down ? BLACK : WHITE);
+        frame(r, 1, BLACK);
+        int n = (int)strlen(text);
+        if (n * FONT_W > rect_w(r) - 4)
+            n = (rect_w(r) - 4) / FONT_W;
+        int x = r.left + (rect_w(r) - n * FONT_W) / 2, y = r.top + (rect_h(r) - FONT_H) / 2;
+        qd_rgb ink = down ? WHITE : it->disabled ? GRAY : BLACK;
+        text_at(x, y, text, n, ink, r);
+        break;
+    }
+    case DLG_ITEM_STATIC_TEXT: {
+        int starts[32], lens[32];
+        int n = font_wrap(text, rect_w(it->r), 32, starts, lens);
+        for (int k = 0; k < n && k < 32; k++)
+            text_at(r.left, r.top + 2 + k * FONT_LINE, text + starts[k], lens[k], BLACK, d->bounds);
+        break;
+    }
+    case DLG_ITEM_EDIT_TEXT: {
+        qd_rect box = outset(r, 3);
+        fill(box, WHITE);
+        frame(box, 1, BLACK);
+        int room = (rect_w(r) - 2) / FONT_W, n = (int)strlen(text);
+        const char *tail = n > room ? text + (n - room) : text; /* the end, where typing happens */
+        int sn = n > room ? room : n;
+        int y = r.top + (rect_h(r) - FONT_H) / 2;
+        text_at(r.left, y, tail, sn, BLACK, r);
+        if (d->focus == i)
+            fill((qd_rect){(int16_t)(y - 1), (int16_t)(r.left + sn * FONT_W),
+                           (int16_t)(y + FONT_H + 1), (int16_t)(r.left + sn * FONT_W + 1)},
+                 BLACK);
+        break;
+    }
+    case DLG_ITEM_PICTURE: draw_picture(r, it->res_id); break;
+    case DLG_ITEM_ICON: draw_icon(r, it->res_id); break;
+    default: break;
+    }
+    qd_mark_dirty();
+}
+
+/* Saves what's under the dialog, then draws it. */
+static void show(dialog *d) {
+    qd_pixels px = screen();
+    qd_rect r = rect_sect(outset(d->bounds, FRAME), px.bounds);
+    d->shown = true;
+    d->saved_r = r;
+    d->saved_screen = px.bounds;
+    d->saved_depth = px.depth;
+    int bpp = px.depth / 8;
+    if (px.depth < 8 || rect_empty(r)) {
+        d->saved = NULL;
+    } else {
+        size_t row = (size_t)rect_w(r) * (size_t)bpp;
+        d->saved = malloc(row * (size_t)rect_h(r));
+        if (!d->saved)
+            fatal("out of memory");
+        for (int y = r.top; y < r.bottom; y++)
+            memcpy(d->saved + (size_t)(y - r.top) * row,
+                   px.base + (size_t)(y - px.bounds.top) * px.row_bytes + (size_t)(r.left - px.bounds.left) * (size_t)bpp,
+                   row);
+    }
+    fill(outset(d->bounds, FRAME), BLACK);
+    frame(outset(d->bounds, FRAME - 1), 1, WHITE);
+    fill(d->bounds, WHITE);
+    for (int i = 0; i < d->nitems; i++)
+        draw_item(d, i);
+    qd_mark_dirty();
+}
+
+/* Puts the screen back as it was before show(), unless the screen has
+   changed size or depth since. */
+static void hide(dialog *d) {
+    qd_pixels px = screen();
+    qd_rect r = d->saved_r;
+    if (d->saved && px.depth == d->saved_depth && memcmp(&px.bounds, &d->saved_screen, sizeof r) == 0) {
+        int bpp = px.depth / 8;
+        size_t row = (size_t)rect_w(r) * (size_t)bpp;
+        for (int y = r.top; y < r.bottom; y++)
+            memcpy(px.base + (size_t)(y - px.bounds.top) * px.row_bytes + (size_t)(r.left - px.bounds.left) * (size_t)bpp,
+                   d->saved + (size_t)(y - r.top) * row, row);
+    }
+    free(d->saved);
+    d->saved = NULL;
+    d->shown = false;
+    qd_mark_dirty();
+}
+
+static void close_dialog(dialog *d) {
+    hide(d);
+    for (int i = 0; i < d->nitems; i++)
+        if (d->items[i].handle)
+            mm_dispose_handle(d->items[i].handle);
+    if (G.front == d)
+        G.front = NULL;
+    d->open = false;
+}
+
+/* ---- input ---- */
+
+static bool inside(qd_rect r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
+
+static int item_at(dialog *d, int x, int y) {
+    for (int i = d->nitems - 1; i >= 0; i--) {
+        qd_rect r = offset(d->items[i].r, d->bounds.left, d->bounds.top);
+        if (d->items[i].type == DLG_ITEM_EDIT_TEXT)
+            r = outset(r, 3);
+        if (inside(r, x, y))
+            return i;
+    }
+    return -1;
+}
+
+static void press(dialog *d, int item1) {
+    if (item1 > 0 && item1 <= d->nitems && !d->items[item1 - 1].disabled)
+        d->hit = item1;
+}
+
+static void set_focus(dialog *d, int i) {
+    int old = d->focus;
+    d->focus = i;
+    if (old >= 0)
+        draw_item(d, old);
+    if (i >= 0)
+        draw_item(d, i);
+}
+
+static void type_char(dialog *d, uint8_t c) {
+    if (d->focus < 0)
+        return;
+    item *it = &d->items[d->focus];
+    if (c == 0x08) {
+        if (it->len > 0)
+            it->len--;
+    } else if (it->len < MAX_TEXT) {
+        it->text[it->len++] = c;
+    }
+    set_handle_text(it);
+    draw_item(d, d->focus);
+}
+
+static void on_key(uint32_t vkey, uint8_t chr, uint32_t mods) {
+    dialog *d = G.front;
+    if (!d)
+        return;
+    bool cmd = (mods & KM_CMD) != 0;
+    if (vkey == VK_RETURN || vkey == VK_ENTER) {
+        press(d, d->default_item);
+    } else if (vkey == VK_ESCAPE || (cmd && vkey == VK_PERIOD)) {
+        press(d, d->cancel_item);
+    } else if (vkey == VK_TAB) {
+        int n = d->nitems, step = (mods & KM_SHIFT) ? n - 1 : 1;
+        for (int k = 1, i = d->focus; k <= n && d->focus >= 0; k++) {
+            i = (i + step) % n;
+            if (d->items[i].type == DLG_ITEM_EDIT_TEXT) {
+                set_focus(d, i);
+                break;
+            }
+        }
+    } else if (vkey == VK_DELETE) {
+        type_char(d, 0x08);
+    } else if (!cmd && chr >= 0x20 && chr < 0x7F) {
+        type_char(d, chr);
+    }
+}
+
+static void on_mouse(int x, int y, bool down) {
+    dialog *d = G.front;
+    if (!d)
+        return;
+    int i = item_at(d, x, y);
+    if (down) {
+        if (i >= 0 && d->items[i].type == DLG_ITEM_EDIT_TEXT) {
+            set_focus(d, i);
+        } else if (i >= 0 && is_control(d->items[i].type) && !d->items[i].disabled) {
+            d->pressed = i;
+            draw_item(d, i);
+        }
+        return;
+    }
+    int was = d->pressed;
+    if (was < 0)
+        return;
+    d->pressed = -1;
+    draw_item(d, was);
+    if (i == was)
+        press(d, was + 1);
+}
+
+/* Inserts pasted or scripted text: printable ASCII only (the fields hold
+   e-mail addresses and key codes). */
+static void on_text(const char *utf8) {
+    dialog *d = G.front;
+    if (!d)
+        return;
+    for (const uint8_t *p = (const uint8_t *)utf8; *p; p++)
+        if (*p >= 0x20 && *p < 0x7F)
+            type_char(d, *p);
+}
+
+static const ev_modal_sink sink = {on_key, on_mouse, on_text};
+
+/* Takes the input until an item is hit; returns it (1-based). */
+static int run_modal(dialog *d) {
+    dialog *outer = G.front;
+    G.front = d;
+    d->hit = 0;
+    events_set_modal(&sink);
+    while (!d->hit) {
+        events_pump();
+        if (!d->hit)
+            misc_wait(1.0 / 60);
+    }
+    G.front = outer;
+    events_set_modal(outer ? &sink : NULL);
+    return d->hit;
+}
+
+/* ---- Alert ---- */
+
+static const char *button_title(dialog *d, int item1, char *buf, size_t cap) {
+    buf[0] = '\0';
+    if (item1 >= 1 && item1 <= d->nitems)
+        display_text(&d->items[item1 - 1], buf, cap);
+    return buf;
+}
+
+/* Alert(id, filter) and StopAlert. The default item: bit 3 of the first
+   stage's 4 bits in the ALRT's stages word picks item 2, otherwise item 1. */
 static void h_alert(void) {
     int16_t id = (int16_t)trap_arg(0);
     rsrc_entry *alrt = rsrc_find(FOURCC('A', 'L', 'R', 'T'), id);
     if (!alrt || alrt->len < 12)
         trap_crash("Alert: ALRT %d doesn't exist", id);
-    uint16_t stages = rd_be16(rsrc_data(alrt) + 10);
-    int item = (stages & 0x8) ? 2 : 1;
+    if (trap_arg(1))
+        trap_crash("Alert: filter procs are not supported");
+    const uint8_t *a = rsrc_data(alrt);
+    uint16_t stages = rd_be16(a + 10);
+    int item1 = (stages & 0x8) ? 2 : 1;
     char text[1024];
     dialogs_alert_text(id, text, sizeof text);
-    log_msg("Alert %d (answering item %d): %s", id, item, text);
-    trap_return((uint32_t)item);
+    if (G.auto_alerts) {
+        log_msg("Alert %d (answering item %d): %s", id, item1, text);
+        trap_return((uint32_t)item1);
+        return;
+    }
+    log_msg("Alert %d: %s", id, text);
+    dialog *d = new_dialog("Alert");
+    d->res_id = id;
+    d->is_alert = true;
+    d->bounds = place(be_rect(a), alrt->len >= 14 ? rd_be16(a + 12) : 0);
+    load_items("Alert", d, (int16_t)rd_be16(a + 8));
+    d->default_item = item1;
+    show(d);
+    int hit = run_modal(d);
+    char title[256];
+    log_msg("Alert %d: answered item %d (%s)", id, hit, button_title(d, hit, title, sizeof title));
+    close_dialog(d);
+    trap_return((uint32_t)hit);
 }
 
 static void h_param_text(void) {
     for (int i = 0; i < 4; i++) {
         uint32_t s = trap_arg(i);
         if (s)
-            gm_read_pstr(s, param[i]);
+            gm_read_pstr(s, G.param[i]);
         else
-            param[i][0] = '\0';
+            G.param[i][0] = '\0';
     }
 }
 
+/* ---- dialogs ---- */
+
+static uint32_t ref_of(dialog *d) { return DLG_TAG_BASE + 16u * (uint32_t)(d - G.d); }
+
+static dialog *need_dialog(const char *call, uint32_t ref) {
+    uint32_t i = (ref - DLG_TAG_BASE) / 16u;
+    if (ref < DLG_TAG_BASE || (ref - DLG_TAG_BASE) % 16u || i >= DLG_MAX || !G.d[i].open || G.d[i].is_alert)
+        trap_crash("%s: 0x%08x is not a dialog", call, ref);
+    return &G.d[i];
+}
+
+/* GetNewDialog(short id, void *storage, WindowRef behind) -> DialogRef.
+   DLOG: rect (8), procID (2), visible (1), pad (1), goAway (1), pad (1),
+   refCon (4), DITL id (2), title (Str255, padded to even), position (2).
+   The default button is item 1 if it is a button, otherwise the first
+   button (the Dialog Manager would use item 1 regardless). */
+static void h_get_new_dialog(void) {
+    int16_t id = (int16_t)trap_arg(0);
+    if (trap_arg(1))
+        trap_crash("GetNewDialog: caller-supplied dialog storage is not supported");
+    rsrc_entry *e = rsrc_find(FOURCC('D', 'L', 'O', 'G'), id);
+    if (!e || e->len < 21)
+        trap_crash("GetNewDialog: DLOG %d doesn't exist", id);
+    const uint8_t *p = rsrc_data(e);
+    uint32_t pos_at = 21u + p[20];
+    pos_at += pos_at & 1u;
+    dialog *d = new_dialog("GetNewDialog");
+    d->res_id = id;
+    d->bounds = place(be_rect(p), pos_at + 2 <= e->len ? rd_be16(p + pos_at) : 0);
+    load_items("GetNewDialog", d, (int16_t)rd_be16(p + 18));
+    for (int i = 0; i < d->nitems && !d->default_item; i++)
+        if (d->items[i].type == DLG_ITEM_BUTTON && (i == 0 || d->items[0].type != DLG_ITEM_BUTTON))
+            d->default_item = i + 1;
+    if (p[10])
+        show(d);
+    log_msg("GetNewDialog %d", id);
+    trap_return(ref_of(d));
+}
+
+/* ModalDialog(ModalFilterUPP filter, DialogItemIndex *itemHit). Returns
+   when an enabled button is clicked (or chosen with Return or Esc); typing
+   is handled inside. */
+static void h_modal_dialog(void) {
+    if (trap_arg(0))
+        trap_crash("ModalDialog: filter procs are not supported");
+    dialog *d = NULL;
+    for (int i = DLG_MAX - 1; i >= 0 && !d; i--)
+        if (G.d[i].open && !G.d[i].is_alert)
+            d = &G.d[i];
+    if (!d)
+        trap_crash("ModalDialog: no dialog is open");
+    if (G.auto_alerts)
+        trap_crash("ModalDialog: DLOG %d can't be answered automatically (LOONY_AUTO_ALERTS)", d->res_id);
+    if (!d->shown) /* an invisible DLOG appears when it's used */
+        show(d);
+    int hit = run_modal(d);
+    gm_w16(trap_arg(1), (uint16_t)hit);
+}
+
+/* GetDialogItem(DialogRef, DialogItemIndex, DialogItemType *type, Handle
+   *item, Rect *box). Text items' handles hold their current text; other
+   items have none. */
+static void h_get_dialog_item(void) {
+    dialog *d = need_dialog("GetDialogItem", trap_arg(0));
+    int16_t n = (int16_t)trap_arg(1);
+    if (n < 1 || n > d->nitems)
+        trap_crash("GetDialogItem: DLOG %d has no item %d", d->res_id, n);
+    item *it = &d->items[n - 1];
+    if (trap_arg(2))
+        gm_w16(trap_arg(2), (uint16_t)(it->type | (it->disabled ? DLG_ITEM_DISABLED : 0)));
+    if (trap_arg(3))
+        gm_w32(trap_arg(3), it->handle);
+    if (trap_arg(4))
+        qd_write_rect(trap_arg(4), it->r);
+}
+
+/* GetDialogItemText(Handle, Str255 text): the handle's bytes, at most 255. */
+static void h_get_dialog_item_text(void) {
+    uint32_t h = trap_arg(0), out = trap_arg(1);
+    if (!mm_is_handle(h))
+        trap_crash("GetDialogItemText: 0x%08x is not a handle", h);
+    uint32_t n = mm_handle_size(h);
+    if (n > MAX_TEXT)
+        n = MAX_TEXT;
+    gm_w8(out, (uint8_t)n);
+    if (n)
+        memcpy(gm_ptr(out + 1, n), gm_ptr(gm_r32(h), n), n);
+}
+
+static void h_dispose_dialog(void) {
+    dialog *d = need_dialog("DisposeDialog", trap_arg(0));
+    close_dialog(d);
+}
+
 void dialogs_register(void) {
     trap_register("Alert", h_alert);
     trap_register("StopAlert", h_alert);
     trap_register("ParamText", h_param_text);
+    trap_register("GetNewDialog", h_get_new_dialog);
+    trap_register("ModalDialog", h_modal_dialog);
+    trap_register("GetDialogItem", h_get_dialog_item);
+    trap_register("GetDialogItemText", h_get_dialog_item_text);
+    trap_register("DisposeDialog", h_dispose_dialog);
 }
diff --git a/src/dialogs.h b/src/dialogs.h
index b9c5428..552ab44 100644
--- a/src/dialogs.h
+++ b/src/dialogs.h
@@ -1,18 +1,46 @@
 #pragma once
+#include <stdbool.h>
 #include <stddef.h>
 #include <stdint.h>
 
-/* Dialog Manager, for now only what startup needs. Alert and StopAlert don't
-   draw anything yet (that needs the dialog work in milestone 6): they log the
-   alert's text, with ParamText substitutions, and return its default item,
-   as if the user pressed Return. */
+/* Dialog Manager: Alert, StopAlert and ParamText; GetNewDialog,
+   ModalDialog, GetDialogItem, GetDialogItemText and DisposeDialog.
 
-/* Resets ParamText. */
+   Dialogs are drawn straight onto the emulated screen from their ALRT, DLOG
+   and DITL resources, plainly and with the 8x8 font (font.h): a framed white
+   box, buttons, static text with ParamText substitutions, edit fields,
+   pictures and icons. What was under a dialog comes back when it closes.
+   While one is open it takes the input (events_set_modal): clicks on
+   buttons, Return or Enter for the default button, Esc or Cmd-. for a
+   button titled "Cancel", typing, Delete, Tab and Shift-Tab between edit
+   fields, and Cmd-V. No game timers fire meanwhile.
+
+   With LOONY_AUTO_ALERTS=1, Alert and StopAlert draw nothing and answer at
+   once with their default item, as if the user pressed Return (headless
+   runs whose golden frames predate dialogs use this). */
+
+/* DialogRefs are opaque IDs: DLG_TAG_BASE + 16 * slot. */
+#define DLG_TAG_BASE 0x0B000000u
+#define DLG_MAX 4
+
+/* DITL item types (the low 7 bits) and the disabled flag. */
+#define DLG_ITEM_USER 0
+#define DLG_ITEM_BUTTON 4
+#define DLG_ITEM_CHECKBOX 5
+#define DLG_ITEM_RADIO 6
+#define DLG_ITEM_STATIC_TEXT 8
+#define DLG_ITEM_EDIT_TEXT 16
+#define DLG_ITEM_ICON 32
+#define DLG_ITEM_PICTURE 64
+#define DLG_ITEM_DISABLED 128
+
+/* Resets ParamText and closes all dialogs (without restoring the screen).
+   Reads LOONY_AUTO_ALERTS. */
 void dialogs_init(void);
 
 /* The text Alert would show for ALRT id, items joined with " | ". Empty if
    the alert doesn't exist. */
 void dialogs_alert_text(int16_t id, char *out, size_t cap);
 
-/* Registers Alert, StopAlert and ParamText. */
+/* Registers the Dialog Manager imports. */
 void dialogs_register(void);
diff --git a/src/memmgr.c b/src/memmgr.c
index 0d1a95b..43289bb 100644
--- a/src/memmgr.c
+++ b/src/memmgr.c
@@ -190,6 +190,25 @@ int16_t mm_dispose_handle(uint32_t h) {
 
 uint32_t mm_handle_size(uint32_t h) { return logical(gm_r32(h)); }
 
+int16_t mm_set_handle_size(uint32_t h, uint32_t size) {
+    if (!mm_is_handle(h))
+        return MM_MEM_WZ_ERR;
+    uint32_t d = gm_r32(h);
+    if (size <= capacity(d)) {
+        set_header(d, MAGIC_DATA, capacity(d), size, h);
+        return MM_NO_ERR;
+    }
+    uint32_t n = alloc_block(size, MAGIC_DATA, h, false);
+    if (!n)
+        return MM_MEM_FULL_ERR;
+    uint32_t keep = logical(d);
+    if (keep)
+        memcpy(gm_ptr(n, keep), gm_ptr(d, keep), keep);
+    free_block(d);
+    gm_w32(h, n);
+    return MM_NO_ERR;
+}
+
 uint32_t mm_recover_handle(uint32_t p) {
     if (!is_block(p, MAGIC_DATA))
         return 0;
diff --git a/src/memmgr.h b/src/memmgr.h
index c3b4f4e..7bf8ab8 100644
--- a/src/memmgr.h
+++ b/src/memmgr.h
@@ -41,6 +41,11 @@ int16_t mm_dispose_handle(uint32_t h);
 bool mm_is_handle(uint32_t h);
 /* Requires mm_is_handle(h). */
 uint32_t mm_handle_size(uint32_t h);
+
+/* Resizes h's data, in place if it fits, otherwise by moving it (the master
+   pointer follows); the contents are kept up to the smaller size.
+   MM_MEM_FULL_ERR if the heap is full, MM_MEM_WZ_ERR if h is not a handle. */
+int16_t mm_set_handle_size(uint32_t h, uint32_t size);
 /* The handle whose data block starts at p, or 0 if there is none. */
 uint32_t mm_recover_handle(uint32_t p);
 /* Requires mm_is_handle(h). */
diff --git a/src/pict.c b/src/pict.c
index 0c0448c..93c8042 100644
--- a/src/pict.c
+++ b/src/pict.c
@@ -7,11 +7,15 @@
 
 #include "util.h"
 
+#define DITHER_COPY 64 /* srcCopy + ditherCopy */
+
 typedef struct {
     const uint8_t *p, *end;
     int version;
     char *err;
     size_t errlen;
+    uint8_t *matte; /* from a QuickTime opcode, for the next DirectBitsRect: 1 = opaque */
+    int matte_w, matte_h;
 } reader;
 
 static bool fail(reader *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
@@ -224,34 +228,220 @@ static bool bits_rect(reader *r, bool packed, qd_rect frame, qd_rect dst, const
     return ok;
 }
 
-bool pict_draw(const uint8_t *data, size_t len, qd_rect dst, const qd_pixels *target, qd_rect clip,
-               qd_rgb fg, qd_rgb bg, char *err, size_t errlen) {
-    reader r = {data, data + len, 0, err, errlen};
-    qd_rect frame;
-    if (!pict_frame(data, len, &frame))
-        return fail(&r, "picture data is truncated");
-    r.p += 10;
-    if (!need(&r, 2))
+/* Decodes 8-bit QuickTime Animation ('rle ') data into a w x h image of
+   gray levels (0 white, 255 black). Each line starts with a skip byte; then
+   codes: 0 another skip, -1 end of line, n > 0 n groups of 4 literal
+   pixels, n < 0 one group of 4 repeated -n times. */
+static bool qt_rle8(reader *r, const uint8_t *p, size_t n, uint8_t *out, int w, int h) {
+    const uint8_t *end = p + n;
+    if (n < 6)
+        return fail(r, "QuickTime matte is truncated");
+    p += 4; /* chunk size */
+    uint16_t header = rd_be16(p);
+    p += 2;
+    int line = 0, lines = h;
+    if (header & 0x0008) {
+        if (end - p < 8)
+            return fail(r, "QuickTime matte is truncated");
+        line = rd_be16(p);
+        lines = rd_be16(p + 4);
+        p += 8;
+    }
+    for (; lines > 0 && line < h; lines--, line++) {
+        if (p >= end)
+            return fail(r, "QuickTime matte is truncated");
+        int x = 4 * (*p++ - 1);
+        for (;;) {
+            if (p >= end)
+                return fail(r, "QuickTime matte is truncated");
+            int8_t code = (int8_t)*p++;
+            if (code == -1)
+                break;
+            if (code == 0) {
+                if (p >= end)
+                    return fail(r, "QuickTime matte is truncated");
+                x += 4 * (*p++ - 1);
+            } else if (code < 0) {
+                if (end - p < 4)
+                    return fail(r, "QuickTime matte is truncated");
+                for (int k = 0; k < -code; k++, x += 4)
+                    for (int i = 0; i < 4; i++)
+                        if (x + i >= 0 && x + i < w)
+                            out[line * w + x + i] = p[i];
+                p += 4;
+            } else {
+                if (end - p < 4 * code)
+                    return fail(r, "QuickTime matte is truncated");
+                for (int i = 0; i < 4 * code; i++, x++)
+                    if (x >= 0 && x < w)
+                        out[line * w + x] = p[i];
+                p += 4 * code;
+            }
+        }
+    }
+    return true;
+}
+
+/* UncompressedQuickTime (0x8201): version, matrix, then a matte (an image
+   description and its data), which becomes r->matte. The image itself
+   follows as ordinary opcodes. Only 8-bit gray 'rle ' mattes are decoded;
+   other mattes are ignored and the image is drawn whole. */
+static bool quicktime(reader *r) {
+    if (!need(r, 4))
+        return false;
+    uint32_t n = rd_be32(r->p);
+    r->p += 4;
+    if (!need(r, n))
+        return false;
+    const uint8_t *q = r->p, *end = r->p + n;
+    r->p = end;
+    free(r->matte);
+    r->matte = NULL;
+    if (n < 2 + 36 + 4 + 8)
+        return true;
+    uint32_t matte_size = rd_be32(q + 38);
+    q += 50; /* version, matrix, matte size, matte rect */
+    if (matte_size < 86 || (size_t)(end - q) < matte_size)
+        return true;
+    uint32_t id_size = rd_be32(q), data_size = rd_be32(q + 44);
+    int w = rd_be16(q + 32), h = rd_be16(q + 34), depth = rd_be16(q + 82);
+    if (rd_be32(q + 4) != FOURCC('r', 'l', 'e', ' ') || depth != 40 || id_size < 86 ||
+        id_size + data_size > matte_size || w <= 0 || h <= 0 || w > 4096 || h > 4096)
+        return true;
+    uint8_t *gray = calloc((size_t)w * (size_t)h, 1);
+    if (!gray)
+        return fail(r, "out of memory");
+    if (!qt_rle8(r, q + id_size, data_size, gray, w, h)) {
+        free(gray);
         return false;
-    if (r.p[0] == 0x11 && r.p[1] == 0x01) {
-        r.version = 1;
-        r.p += 2;
-    } else if (rd_be16(r.p) == 0x0011 && len >= 14 && rd_be16(r.p + 2) == 0x02FF) {
-        r.version = 2;
-        r.p += 4;
-    } else {
-        return fail(&r, "not a version 1 or 2 picture");
     }
+    for (int i = 0; i < w * h; i++)
+        gray[i] = gray[i] >= 128;
+    r->matte = gray;
+    r->matte_w = w;
+    r->matte_h = h;
+    return true;
+}
+
+/* Reads one packed row of component planes (pack type 4) into a 32-bit
+   xRGB row: the planes come one after another, alpha first if there are 4. */
+static bool unpack_planes(reader *r, size_t packed, int ncmp, uint8_t *row, int w) {
+    uint8_t planes[4 * 4096];
+    if ((size_t)ncmp * (size_t)w > sizeof planes)
+        return fail(r, "direct pixmap row of %d pixels is too wide", w);
+    if (!unpack_row(r, packed, planes, (size_t)ncmp * (size_t)w))
+        return false;
+    const uint8_t *rgb = planes + (size_t)(ncmp - 3) * (size_t)w;
+    for (int x = 0; x < w; x++) {
+        row[4 * x] = 0;
+        row[4 * x + 1] = rgb[x];
+        row[4 * x + 2] = rgb[w + x];
+        row[4 * x + 3] = rgb[2 * w + x];
+    }
+    return true;
+}
+
+/* DirectBitsRect: a base address, a PixMap without a color table, then rows
+   of 32-bit pixels, unpacked (pack type 0 or 1), without their pad byte
+   (2), or as PackBits component planes (4). */
+static bool direct_bits_rect(reader *r, qd_rect frame, qd_rect dst, const qd_pixels *target,
+                             qd_rect clip, qd_rgb fg, qd_rgb bg) {
+    uint16_t row_bytes;
+    qd_rect bounds;
+    if (!skip(r, 4) || !u16(r, &row_bytes) || !rect(r, &bounds) || !need(r, 36))
+        return false;
+    row_bytes &= 0x3FFF;
+    uint16_t pack_type = rd_be16(r->p + 2);
+    int depth = rd_be16(r->p + 18);
+    int ncmp = rd_be16(r->p + 20);
+    r->p += 36;
+    if (depth != 32 || (ncmp != 3 && ncmp != 4) || pack_type == 3 || pack_type > 4)
+        return fail(r, "%d-bit direct pixmaps with %d components and pack type %u are not supported",
+                    depth, ncmp, pack_type);
+    qd_rect src_rect, dst_rect;
+    uint16_t mode;
+    if (!rect(r, &src_rect) || !rect(r, &dst_rect) || !u16(r, &mode))
+        return false;
+    int w = rect_w(bounds), h = rect_h(bounds);
+    if (w < 0 || h < 0 || (size_t)row_bytes < (size_t)w * 4)
+        return fail(r, "pixmap rows are too short for their bounds");
+    uint8_t *pixels = malloc((size_t)w * 4 * (h ? h : 1) + 4);
+    if (!pixels)
+        return fail(r, "out of memory");
+    bool ok = true;
+    for (int y = 0; y < h && ok; y++) {
+        uint8_t *row = pixels + (size_t)y * (size_t)w * 4;
+        if (pack_type == 2) {
+            ok = need(r, (size_t)w * 3);
+            for (int x = 0; ok && x < w; x++) {
+                row[4 * x] = 0;
+                memcpy(row + 4 * x + 1, r->p + 3 * x, 3);
+            }
+            if (ok)
+                r->p += (size_t)w * 3;
+        } else if (pack_type == 4 && row_bytes >= 8) {
+            size_t n = 0;
+            if (row_bytes > 250) {
+                uint16_t c;
+                ok = u16(r, &c);
+                n = c;
+            } else {
+                ok = need(r, 1);
+                if (ok)
+                    n = *r->p++;
+            }
+            ok = ok && unpack_planes(r, n, ncmp, row, w);
+        } else {
+            ok = need(r, row_bytes);
+            if (ok) {
+                memcpy(row, r->p, (size_t)w * 4);
+                r->p += row_bytes;
+            }
+        }
+    }
+    qd_pixels src = {pixels, (uint32_t)w * 4, bounds, 32, NULL};
+    int m = mode == DITHER_COPY ? QD_SRC_COPY : mode; /* drawn without dithering */
+    qd_rect to = map_rect(dst_rect, frame, dst);
+    if (ok && r->matte && r->matte_w == w && r->matte_h == h && rect_w(src_rect) > 0 &&
+        rect_h(src_rect) > 0) {
+        /* Only the matte's opaque runs, each mapped like the whole. */
+        for (int y = src_rect.top; ok && y < src_rect.bottom; y++)
+            for (int x = src_rect.left; ok && x < src_rect.right;) {
+                int mx = x - bounds.left, my = y - bounds.top;
+                if (mx < 0 || mx >= w || my < 0 || my >= h || !r->matte[my * w + mx]) {
+                    x++;
+                    continue;
+                }
+                int x1 = x;
+                while (x1 < src_rect.right && x1 - bounds.left < w && r->matte[my * w + x1 - bounds.left])
+                    x1++;
+                qd_rect run = {(int16_t)y, (int16_t)x, (int16_t)(y + 1), (int16_t)x1};
+                ok = qd_blit(&src, run, target, map_rect(run, src_rect, to), clip, m, fg, bg, r->err,
+                             r->errlen);
+                x = x1;
+            }
+    } else if (ok) {
+        ok = qd_blit(&src, src_rect, target, to, clip, m, fg, bg, r->err, r->errlen);
+    }
+    free(r->matte);
+    r->matte = NULL;
+    free(pixels);
+    return ok;
+}
+
+/* Runs the opcodes after the version. */
+static bool draw_ops(reader *r, const uint8_t *data, qd_rect frame, qd_rect dst,
+                     const qd_pixels *target, qd_rect clip, qd_rgb fg, qd_rgb bg) {
     for (;;) {
         uint16_t op;
-        if (r.version == 1) {
-            if (!need(&r, 1))
+        if (r->version == 1) {
+            if (!need(r, 1))
                 return false;
-            op = *r.p++;
+            op = *r->p++;
         } else {
-            if (((r.p - data) & 1) && !skip(&r, 1))
+            if (((r->p - data) & 1) && !skip(r, 1))
                 return false;
-            if (!u16(&r, &op))
+            if (!u16(r, &op))
                 return false;
         }
         qd_rect bbox;
@@ -259,34 +449,74 @@ bool pict_draw(const uint8_t *data, size_t len, qd_rect dst, const qd_pixels *ta
         case 0x00: /* NOP */
             break;
         case 0x01: /* clip region */
-            if (!region(&r, &bbox))
+            if (!region(r, &bbox))
                 return false;
             break;
         case 0x1E: /* DefHilite */
             break;
         case 0x0C00: /* header */
-            if (!skip(&r, 24))
+            if (!skip(r, 24))
                 return false;
             break;
         case 0x90: /* BitsRect */
         case 0x98: /* PackBitsRect */
-            if (!bits_rect(&r, op == 0x98, frame, dst, target, clip, fg, bg))
+            if (!bits_rect(r, op == 0x98, frame, dst, target, clip, fg, bg))
+                return false;
+            break;
+        case 0x9A: /* DirectBitsRect */
+            if (!direct_bits_rect(r, frame, dst, target, clip, fg, bg))
+                return false;
+            break;
+        case 0x8200: { /* CompressedQuickTime: the image follows as QuickDraw */
+            if (!need(r, 4))
+                return false;
+            uint32_t n = rd_be32(r->p);
+            r->p += 4;
+            if (!skip(r, n))
+                return false;
+            break;
+        }
+        case 0x8201: /* UncompressedQuickTime: a matte for the image that follows */
+            if (!quicktime(r))
                 return false;
             break;
         case 0xA0: /* short comment */
-            if (!skip(&r, 2))
+            if (!skip(r, 2))
                 return false;
             break;
         case 0xA1: { /* long comment */
             uint16_t kind, n;
-            if (!u16(&r, &kind) || !u16(&r, &n) || !skip(&r, n))
+            if (!u16(r, &kind) || !u16(r, &n) || !skip(r, n))
                 return false;
             break;
         }
         case 0xFF: /* end of picture */
             return true;
         default:
-            return fail(&r, "picture opcode 0x%04x is not supported", op);
+            return fail(r, "picture opcode 0x%04x is not supported", op);
         }
     }
 }
+
+bool pict_draw(const uint8_t *data, size_t len, qd_rect dst, const qd_pixels *target, qd_rect clip,
+               qd_rgb fg, qd_rgb bg, char *err, size_t errlen) {
+    reader r = {.p = data, .end = data + len, .err = err, .errlen = errlen};
+    qd_rect frame;
+    if (!pict_frame(data, len, &frame))
+        return fail(&r, "picture data is truncated");
+    r.p += 10;
+    if (!need(&r, 2))
+        return false;
+    if (r.p[0] == 0x11 && r.p[1] == 0x01) {
+        r.version = 1;
+        r.p += 2;
+    } else if (rd_be16(r.p) == 0x0011 && len >= 14 && rd_be16(r.p + 2) == 0x02FF) {
+        r.version = 2;
+        r.p += 4;
+    } else {
+        return fail(&r, "not a version 1 or 2 picture");
+    }
+    bool ok = draw_ops(&r, data, frame, dst, target, clip, fg, bg);
+    free(r.matte);
+    return ok;
+}
diff --git a/src/pict.h b/src/pict.h
index 47e20a0..4cb509a 100644
--- a/src/pict.h
+++ b/src/pict.h
@@ -8,7 +8,10 @@
 /* Draws a PICT (version 1 or 2) into target, mapping the picture's frame onto
    dst and clipping to clip. Supported opcodes: version, header, DefHilite,
    clip (rectangular), BitsRect, PackBitsRect (1, 2, 4 and 8 bits, srcCopy),
-   short and long comments, and end. Anything else writes err and returns false.
+   DirectBitsRect (32 bits), short and long comments, and end. QuickTime
+   opcodes are skipped: their pictures repeat the image as QuickDraw after
+   them (the game's two dialog icons keep only an alpha matte there).
+   Anything else writes err and returns false.
    Reference: Inside Macintosh: Imaging With QuickDraw, appendix A. */
 bool pict_draw(const uint8_t *data, size_t len, qd_rect dst, const qd_pixels *target, qd_rect clip,
                qd_rgb fg, qd_rgb bg, char *err, size_t errlen);
diff --git a/src/qd.c b/src/qd.c
index 1977e36..9d091fa 100644
--- a/src/qd.c
+++ b/src/qd.c
@@ -268,6 +268,8 @@ uint32_t qd_current_port(void) { return Q.cur_port; }
 
 void qd_screen(qd_pixels *out, qd_palette *pal) { qd_bits("screen", gm_r32(Q.screen_pm), out, pal); }
 
+void qd_mark_dirty(void) { Q.dirty = true; }
+
 bool qd_take_dirty(void) {
     bool d = Q.dirty;
     Q.dirty = false;
diff --git a/src/qd.h b/src/qd.h
index a757d38..77d3ad5 100644
--- a/src/qd.h
+++ b/src/qd.h
@@ -80,6 +80,9 @@ void qd_bits(const char *call, uint32_t bits, qd_pixels *out, qd_palette *pal);
 /* The screen's pixels, for display. */
 void qd_screen(qd_pixels *out, qd_palette *pal);
 
+/* Notes that something drew to the screen outside QuickDraw (dialogs). */
+void qd_mark_dirty(void);
+
 /* True if anything drew to the screen since the last call. */
 bool qd_take_dirty(void);
 uint32_t qd_current_port(void);
diff --git a/src/trap.c b/src/trap.c
index 272cc6c..9211dc7 100644
--- a/src/trap.c
+++ b/src/trap.c
@@ -88,6 +88,8 @@ void trap_register(const char *name, trap_handler fn) {
             T.handlers[i] = fn;
 }
 
+bool trap_has_handler(uint32_t index) { return index < T.n && T.handlers[index]; }
+
 const char *trap_import_name(uint32_t index) {
     return index < T.n ? T.names[index] : "(unknown)";
 }
diff --git a/src/trap.h b/src/trap.h
index f5a9717..f60a507 100644
--- a/src/trap.h
+++ b/src/trap.h
@@ -1,4 +1,5 @@
 #pragma once
+#include <stdbool.h>
 #include <stdint.h>
 
 #include "cpu.h"
@@ -25,6 +26,9 @@ uint32_t guest_call(uint32_t tvector, int nargs, const uint32_t *args);
 
 const char *trap_import_name(uint32_t index);
 
+/* True if import index has a handler. */
+bool trap_has_handler(uint32_t index);
+
 /* Prints a crash report (message, registers, depth, recent imports) and exits 2. */
 _Noreturn void trap_crash(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
 
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build build && ./build/loony_tests dialogs_ && ./build/loony_tests pict_ && ./build/loony_tests blit_ && ./build/loony_tests mm_ && ./build/loony_tests pef_every && ./build/loony_tests run_ && ./build/loony_tests`
Expected: all pass (the registration test skips without `LOONY_TEST_EMAIL`/`LOONY_TEST_KEY`); no sanitizer reports.

The three new golden hashes are recorded from this run; they are shown to the user at the handoff for approval, like the earlier ones. `run_a_key_code_registers_and_survives_a_relaunch` is skipped unless `LOONY_TEST_EMAIL` and `LOONY_TEST_KEY` are set.

- [ ] **Step 5: Commit**

```bash
git add src/blit.c src/dialogs.c src/dialogs.h src/memmgr.c src/memmgr.h src/pict.c src/pict.h src/qd.c src/qd.h src/trap.c src/trap.h tests/test_blit.c tests/test_dialogs.c tests/test_memmgr.c tests/test_pef.c tests/test_pict.c tests/test_run.c
git commit -m "Dialogs drawn on the screen: real alerts, the registration form with edit fields; PICT DirectBitsRect and QuickTime mattes"
```

---

### Task 7: README and spec

**Files:**
- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-09-30-loony-shim-design.md`

- [ ] **Step 1: Write the docs**

```diff
diff --git a/README.md b/README.md
index bda1334..9104370 100644
--- a/README.md
+++ b/README.md
@@ -23,7 +23,14 @@ cmake --build build-release
 ./build-release/loony "/path/to/game folder"
 ```
 
-The game starts with its opening and then a self-playing demo. To play:
+Until it is registered, the game first shows two alerts: the shareware screen
+(Play Demo, Buy Now, Enter Key-Code, Quit) and the key list. Click a button, or
+press Return for the outlined one. To register, click **Enter Key-Code**, type
+or paste (Cmd-V) your e-mail address and key code, using Tab to move between
+the fields, and click **Register** (or press Return). The license is saved with
+the preferences, so later launches go straight to the game.
+
+The game then plays its opening and a self-playing demo. To play:
 
 | Key | Does |
 |---|---|
@@ -35,10 +42,13 @@ The game starts with its opening and then a self-playing demo. To play:
 | Cmd-F | Full screen on or off |
 | Cmd-Q or closing the window | Quit |
 
-The keys can be changed from the game's OPTIONS menu. This is the shareware
-version: games are time-limited, and the two startup alerts ("Play Demo" and the
-key list) are answered automatically. Registering, saved preferences and high
-scores come with milestone 6.
+The keys can be changed from the game's OPTIONS menu. Unregistered, games are
+time-limited.
+
+The game's preferences (options, keys, the high-score table and the license)
+are saved when it quits, in `~/Library/Application Support/loony-shim/prefs.plist`.
+Any file the game writes goes to the same folder, never into the game folder.
+Delete the folder to start over.
 
 ## Debugging
 
@@ -52,6 +62,8 @@ SDL_VIDEO_DRIVER=dummy ./build/loony  # no window (with LOONY_SCREENSHOT for hea
 LOONY_WAV=out.wav ./build/loony       # also record the sound (44.1 kHz 16-bit stereo)
 SDL_AUDIO_DRIVER=dummy ./build/loony  # no sound output
 LOONY_FIXED_CLOCK=1 LOONY_SCRIPT=play.txt SDL_VIDEO_DRIVER=dummy ./build/loony
+LOONY_AUTO_ALERTS=1 ./build/loony     # answer alerts with their default button, without showing them
+LOONY_DATA_DIR=/tmp/fresh ./build/loony   # use another folder for preferences and saved files
 ```
 
 `LOONY_FIXED_CLOCK=1` makes time advance only when the game waits, so a run is
@@ -69,6 +81,9 @@ action per line:
 2600 quit
 ```
 
+Scripts can also click (`20 click 460 270`, in emulated-screen pixels) and type
+into a dialog (`50 type me@example.com`, the rest of the line).
+
 Key names are those in `src/keymap.c` (`z`, `slash`, `return`, `space`, `esc`,
 `lshift`, `rshift`, ...).
 
diff --git a/docs/superpowers/specs/2026-09-30-loony-shim-design.md b/docs/superpowers/specs/2026-09-30-loony-shim-design.md
index 48ca9cc..604845f 100644
--- a/docs/superpowers/specs/2026-09-30-loony-shim-design.md
+++ b/docs/superpowers/specs/2026-09-30-loony-shim-design.md
@@ -82,8 +82,11 @@ src/
   mixer.c       command queues and mixing, host-only
   wav.c         .wav recording
   events.c      Carbon Event Manager, event loop, timers, key translation
-  dialogs.c     Alert, GetNewDialog, ModalDialog, item text; draws with a built-in bitmap font
-  files.c       FSSpec file calls, CFPreferences, CFString/CFNumber objects
+  dialogs.c     Alert, GetNewDialog, ModalDialog, item text; draws on the emulated screen
+  font.c        the dialogs' 8x8 bitmap font (SDL's debug font) and word wrap
+  files.c       FSSpec file calls over the game folder and the writable folder
+  cf.c          CFString/CFNumber objects and CFPreferences
+  plist.c       prefs.plist, read and written with the host's CoreFoundation
   misc.c        Gestalt, TickCount, Microseconds, Delay, ICLaunchURL, AE, cursor calls
 tests/          unit and integration tests
 ```
@@ -174,7 +177,7 @@ Used for: the init and main entry points, Carbon event handlers, event loop time
 - **GWorlds:** `NewGWorld` allocates a PixMap and pixel buffer in the guest heap with the requested depth and color table. `LockPixels`, `GetPixBaseAddr`, `GetGWorldPixMap`, `SetGWorld`, `GetGWorld`, `UpdateGWorld` and `DisposeGWorld` are implemented over that structure.
 - **CopyBits:** the `srcCopy` mode at 8, 16 and 32 bits per pixel, same-depth or converting depth. It supports nearest-neighbor scaling when the source and destination rectangles differ, and clips to the destination port's clip rectangle. Any other transfer mode or mask region fails loudly. A copy to a window port marks the screen dirty.
 - **Also implemented:** `PaintRect` with the foreground color, `RGBForeColor`, `ClipRect`, rectangle helpers, `GetCTable`, `GetEntryColor`, `DisposePalette`, `QDFlushPortBuffer` (marks the screen dirty and presents at the next pump), and `GetQDGlobalsScreenBits` / `GetPortBitMapForCopyBits` accessors.
-- **DrawPicture:** a PICT v2 decoder covering the opcodes used by the game's 7 PICTs: header, clip, `PackBitsRect`, `DirectBitsRect`, comments, `OpEndPic`. Any other opcode fails loudly. We can test it offline against all 7.
+- **DrawPicture:** a PICT v2 decoder covering the opcodes used by the game's 7 PICTs: header, clip, `PackBitsRect`, `DirectBitsRect`, comments, `OpEndPic`. Any other opcode fails loudly. We can test it offline against all 7. The two dialog icons (PICT 128 and 129) also carry an `UncompressedQuickTime` opcode holding an alpha matte in QuickTime Animation format; it is decoded and only the matte's opaque pixels are drawn. (Revised during Plan 6.)
 - **SDL window:** resizable. The emulated screen is scaled to the largest size that fits with the correct aspect ratio, using nearest-neighbor sampling, with vsync. Cmd-F toggles fullscreen and Cmd-Q quits (sends the quit Apple Event). The game never sees these two keys.
 
 ### Sound (`sound.c`, `mixer.c`)
@@ -191,26 +194,28 @@ Used for: the init and main entry points, Carbon event handlers, event loop time
 
 - Implements the Carbon event calls on the import list: handler install and remove, event targets, `GetEventKind`, `GetEventParameter` (key code, character code, modifiers, mouse location, direct object), `SendEventToEventTarget`, `ReleaseEvent`, `ReceiveNextEvent`, timers, and the UPP constructors (which return the procedure pointer unchanged).
 - **Keyboard:** SDL scancodes are translated to Mac virtual key codes through a static table covering the full US layout. Modifiers are reported with left and right distinguished (for example `rightShiftKey`), since pinball games often map the flippers to left and right Shift or Command.
-- **`HideCursor` / `InitCursor` / `SetThemeCursor`:** hide or show the SDL cursor.
+- **`HideCursor` / `InitCursor` / `SetThemeCursor`:** hide or show the SDL cursor. It always shows while a dialog is open.
 
 ### Dialogs (`dialogs.c`)
 
-- **`Alert` / `StopAlert`:** build the dialog from its `ALRT` and `DITL` resources, apply `ParamText` substitutions, and draw it on the emulated screen. They run a modal loop until a button is clicked or Return/Esc is pressed, and return the item number.
-- **`GetNewDialog` / `ModalDialog`:** draw `DITL` items (buttons, static text, edit text, icon/PICT items) on the emulated screen with a built-in bitmap font. Edit text supports typing, backspace and Tab between fields. `ModalDialog` calls the game's filter proc, if one was given, for each event. `GetDialogItem`, `GetDialogItemText` and `DisposeDialog` are implemented.
-- These dialogs look plain, not like real Mac OS dialogs, but they work. Likely uses in this game: high-score name entry, preferences and error alerts.
+- **`Alert` / `StopAlert`:** build the dialog from its `ALRT` and `DITL` resources, apply `ParamText` substitutions, and draw it on the emulated screen. They run a modal loop until a button is clicked or Return is pressed (Esc for a button titled "Cancel"), restore the screen under the dialog, and return the item number. `LOONY_AUTO_ALERTS=1` answers with the default item at once, without drawing, for headless runs whose golden frames predate dialogs.
+- **`GetNewDialog` / `ModalDialog`:** draw `DITL` items (buttons, static text, edit text, icon/PICT items) on the emulated screen with a built-in bitmap font: SDL's 8x8 debug font, rasterized at startup, with Mac Roman text spelled in ASCII. Edit text supports typing, Delete, Tab and Shift-Tab between fields, clicking a field, and Cmd-V. `ModalDialog` returns when an enabled button is chosen; the game passes no filter proc, and one would fail loudly. The default button is item 1 if it is a button, otherwise the first button, so Return registers in the key-code form. `GetDialogItem` (text items get a handle holding their text), `GetDialogItemText` and `DisposeDialog` are implemented. (Revised during Plan 6.)
+- **Input while a dialog is open** goes to the dialog, not the game, and no timers fire. Mouse clicks are mapped from the window to the emulated screen.
+- **Measured uses (Plan 6):** the shareware alerts at startup (901, then 900 "Play Demo"), the registration form (DLOG 911: e-mail address and key code, no filter proc), its results (902 refused, 903 certified), the OS checks (800, 801) and the exception report (9000). High-score name entry and the options are drawn by the game itself, not with dialogs.
+- These dialogs look plain, not like real Mac OS dialogs, but they work.
 
 ### Files and preferences (`files.c`)
 
 - **Two folders:** the game folder, read-only, and a writable folder at `~/Library/Application Support/loony-shim/`.
 - **Reading:** looks in the writable folder first, then the game folder.
-- **Writing:** writes to any path inside the game folder go to the matching path in the writable folder, copying the file there first if it exists. This keeps the original files untouched while the game believes it saved in place.
+- **Writing:** writes to any path inside the game folder go to the matching path in the writable folder, copying the file there at its first write (not when it is opened) if it exists. This keeps the original files untouched while the game believes it saved in place. `LOONY_DATA_DIR` names another writable folder (the tests use temporary ones). Names `.` and `..` are refused, and `::` goes up one folder but never above the game folder. (Measured in Plan 6: the game writes no files in normal play; everything it saves is a preference.)
 - **FSSpec calls:** `FSMakeFSSpec` resolves vRefNum/dirID/name to a host path, using a small table of fake volume and directory IDs. `FSpCreate`, `FSpOpenDF`, `PBReadSync`, `FSWrite`, `GetEOF`, `SetEOF`, `GetFPos`, `SetFPos`, `FSClose` and `PBFlushFileSync` map to POSIX calls. Mac-Roman file names are converted to UTF-8, and `:` becomes `/`.
-- **CFPreferences:** stored in `prefs.plist` in the writable folder, written on `CFPreferencesAppSynchronize`. CFString and CFNumber are host objects referred to by tag-space IDs, with reference counts. `kCFPreferencesCurrentApplication` is a pre-made CFString ID.
+- **CFPreferences:** stored in `prefs.plist` in the writable folder, an XML property list of strings and integers written with the host's CoreFoundation, read at startup and written on `CFPreferencesAppSynchronize` (the game calls it as it quits). A file that can't be read is moved to `prefs.plist.bad`. CFString and CFNumber are host objects referred to by tag-space IDs, with reference counts. `kCFPreferencesCurrentApplication` is a pre-made CFString ID. (Measured in Plan 6: the game keeps its options, key assignments, the four-entry high-score table with a checksum, "highscore id", and the license, "user email" and "user id", in the preferences.)
 
 ### Miscellaneous (`misc.c`)
 
 - **`Gestalt`:** a fixed table. It reports OS X 10.2.8 (`sysv` = 0x1028), a G3 CPU with no AltiVec (`cpuf`/`ppcf` AltiVec bit clear), and Carbon present. Unknown selectors return `gestaltUndefSelectorErr` and are logged.
-- **`ICStart` / `ICStop`** are no-ops. **`ICLaunchURL`** opens the URL in the default browser.
+- **`ICStart` / `ICStop`** are no-ops. **`ICLaunchURL`** opens the URL in the default browser (http and https only).
 - **`AEInstallEventHandler`** records the handler. The quit event is sent on window close and Cmd-Q.
 - **`KeyScript`, `GetMBarHeight` (returns 0), `ReadLocation`, `GetDateTime`, `NumToString`, `num2dec`, `p2cstrcpy`, `c2pstrcpy`, `BlockMoveData`, `ExitToShell`:** straightforward.
 
@@ -269,4 +274,4 @@ Milestones 1–3 have the most unknowns. Each later milestone's details may be a
 
 - `~/dev/loony-shim`, a git repo on `main`. C11, `-Wall -Wextra -Werror` in all builds. `Debug` adds the sanitizers, and `Release` is `-O2`.
 - `.gitignore` excludes build output, PNG dumps and anything copied from the game folder.
-- Dependencies come from Homebrew: `unicorn`, `sdl3`, `cmake`, `pkg-config`.
+- Dependencies come from Homebrew: `unicorn`, `sdl3`, `cmake`, `pkg-config`. The preferences file also uses macOS's own CoreFoundation framework.
```

- [ ] **Step 2: Commit**

```bash
git add README.md docs/superpowers/specs/2026-09-30-loony-shim-design.md
git commit -m "README and spec: registering, saved preferences, dialogs, LOONY_AUTO_ALERTS and LOONY_DATA_DIR"
```

---

## What comes next (not part of this plan)

- **Milestone 7 (finishing):** an .app bundle (ad-hoc signed, with the `allow-jit` entitlement), a one-hour run with no crash, and a headless regression test recorded.
- **For the user to judge:**
  - registering with their key code;
  - checking that a high score, a changed option and the license survive a quit and a relaunch (spec success criterion 5);
  - approving the three new dialog frames.
