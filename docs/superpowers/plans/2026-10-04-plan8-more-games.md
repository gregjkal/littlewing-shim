# Plan 8: LittleWing.app, Two Games and a Picker — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One app, `LittleWing.app`, plays *Loony Labyrinth 3.0.1* and *Crystal Caliburn 3.0.1*.
- At launch it shows a picker with each game's title art. With only one game installed, it goes straight to that game.
- Quitting a game from its own menu returns to the picker. Cmd-Q or closing the window quits the app.
- Each game keeps its own saved preferences.

**Architecture:**
- **The game table** (`src/game.c`): each game's id, title, folder name and program name.
- **Which game runs** (`main.c`): a folder argument decides. Without one, `LOONY_GAME=<id>` decides. Otherwise it depends on how many games are installed in `/Applications` (or `$LOONY_APPS_DIR`): none is an error, one plays directly, and two show the picker.
- **The picker** (`src/picker.c`) draws into an 800×600 32-bit buffer. It shows each game's title picture (PICT 800, read at run time from the user's copy) through `display.c`, in the window the game then keeps using.
- **Returning to the picker:** the emulator's state is global and one game per process, so the game can't be reset in place. When a game that was picked ends by itself (`ExitToShell`, or `main` returning) and the host didn't ask to quit, the shim saves its preferences and `execv`s itself. The new process shows the picker again.
- **Saves:** each game saves in `~/Library/Application Support/loony-shim/<id>/`. Plan 6–7 saves in `loony-shim/` move into `loony-shim/loony-labyrinth/`. The picker remembers the last game in `loony-shim/picker.plist`.

**Tech Stack:** C (gnu11), clang, CMake ≥ 3.20, Unicorn 2, SDL3, macOS `codesign`, `plutil`, `sips`, `iconutil`.

**Spec:** `docs/superpowers/specs/2026-09-30-loony-shim-design.md`, revised in Task 7. This plan carries the design for the second game and the picker; there is no separate spec. Background: the user asked how hard generalizing would be, and Crystal Caliburn turned out to run on the unchanged shim (Facts measured, below). They first approved a plan with one app per game, then asked for one `LittleWing.app` with a picker. Their choices:
- a drawn picker with each game's title art (option preview: two art cards side by side, the selected one marked, "← → to choose, Return to play");
- quitting a game returns to the picker;
- with only one game installed, it goes straight to that game.

## Global Constraints

- Repo: `~/dev/loony-shim`, branch `main`. All paths below are relative to it.
- The game files are read-only inputs. Nothing from them goes into the repo or the bundle. The picker's art is read from the user's copies at run time.
- The user's key codes and e-mail address never appear anywhere.
- C11 with GNU extensions, `-Wall -Wextra -Werror`. Debug builds add the sanitizers.
- No test opens a real window, plays sound, shows a message box, or touches the real `~/Library`. Tests set `HOME`, `LOONY_DATA_DIR` and `LOONY_APPS_DIR` to temporary folders, and `SDL_VIDEO_DRIVER=dummy` is set for the whole test run.
- All of Loony Labyrinth's approved golden frames and recordings stay as they are. No emulation, drawing or sound changes for the games.
- `loony <folder>` behaves as it does today: it plays the game in that folder, with no picker and no return to a picker. Every existing test runs this way.
- The app's bundle id stays `local.loony-shim`.

## Facts measured (2026-10-04, Release build at 9e375d2, unchanged)

| Fact | Value |
|---|---|
| Crystal Caliburn's folder | `/Applications/Crystal Caliburn`, holding `CRYSTAL CALIBURN 3.0.1` (PEF, creator `fXcR`, data fork 224,599 bytes, resource fork 1,708,439) and `CC Data/effect.bin` (different from Loony's) |
| Imports | Crystal Caliburn needs the same 132 as Loony Labyrinth, all implemented. `main` is at code+0x28fd0 |
| Startup alerts | `Alert 901 (answering item 1): Play Demo \| Quit \| Buy Now \| Enter Key-Code \| Thank you for trying LittleWing CRYSTAL CALIBURN Pinball. ...`, then `Alert 900 (answering item 1): OK \| ...` |
| 72,000-tick fixed-clock run (Esc, Esc, Return, Return, then plunger and flippers) | A game started (ball 1, 56,520 points at tick 3000), no crash, no unknown selector, a clean quit |
| The games' preferences | The same key names in both (`highscore 1`, the key bindings, the license). They need separate files |
| The user's current save folder | `~/Library/Application Support/loony-shim/` holds only `prefs.plist` (their Loony license and high scores) |
| Its backup | Copied on 2026-10-04, before any Plan 8 code ran, to `~/Library/Application Support/loony-shim-backup-2026-10-04/prefs.plist` (identical SHA-1; outside `loony-shim/`, so the migration never touches it). Never modify or delete it. If the Loony license or scores go missing after the migration, stop and tell the user before doing anything else, including restoring it |
| Title art | `PICT 800` in both forks: 512×384, the "Solid State Pinball" title screen with the game's logo. `pict_draw` draws it into a 32-bit buffer without error. (Also present in both: 128 and 129 are 104×128 or 128×128 icons; 801, 802 and 804 are dialog art) |
| Quitting from the game's menu | Script `1720 down esc`, `1724 up esc`, `1800 down esc`, `1804 up esc`, `1900 down up`, `1904 up up`, `2000 down return`, `2004 up return` (Esc ends the demo, Esc opens the menu, Up wraps to its quit item, Return). Both games log `ExitToShell` with no quit Apple Event, and save `prefs.plist` |
| Cmd-Q, a window close or a script `quit` | `events_request_quit` sends the quit Apple Event (`sending the quit Apple Event`). The game's handler then calls `ExitToShell` |

## Review Focus

1. **The user's Loony license and high scores survive the upgrade:** `loony-shim/prefs.plist` moves to `loony-shim/loony-labyrinth/prefs.plist` and is never overwritten. `picker.plist` and the game folders stay put. Covered by Task 2 (unit) and Task 5 (`run_the_picker_remembers_and_each_game_saves_apart`).
2. **Cmd-Q or a window close during a game quits the app, while the game's own QUIT returns to the picker.** Mixing these up either traps the user in the picker or drops them out of the app. Covered by Task 5: `run_quitting_from_the_game_menu_returns_to_the_picker` and `run_cmd_q_in_a_picked_game_quits_the_app`.
3. **Returning to the picker keeps the session:** the game's preferences are saved before the `execv`, the log continues rather than rotating away the game's lines, and full screen stays full screen. Covered by Task 5 (the log and the saves) and the Task 8 playtest (full screen).
4. **Zero or one game installed, or a game folder without its title picture:**
   - none: a message naming both folders;
   - one: straight to it;
   - missing art: a plain card rather than a crash.

   Covered by Task 4 (`picker_draw_without_art`) and Task 5 (`run_with_no_game_installed_says_where_to_put_them`, `run_with_one_game_installed_skips_the_picker`).
5. **`loony <folder>` and `LOONY_DATA_DIR` keep their current meaning:** every existing scripted test, plus Task 2's `files_data_dir_honors_loony_data_dir`.

---

### Task 1: The game table

**Files:**
- Create: `src/game.h`, `src/game.c`
- Test: `tests/test_game.c`

**Interfaces:**
- Consumes: `test_tmp_dir`, `test_remove_tree` (`tests/test.h`); `make_dirs` (`src/util.h`).
- Produces:
  ```c
  typedef struct { const char *id, *title, *folder_name, *exe; } game_info;
  size_t game_count(void);
  const game_info *game_at(size_t i);                 /* NULL past the end */
  const game_info *game_by_id(const char *id);        /* NULL if unknown */
  const char *game_apps_dir(void);                    /* $LOONY_APPS_DIR or "/Applications" */
  void game_folder(const game_info *g, char *out, size_t cap);   /* <apps dir>/<folder_name> */
  const game_info *game_in_folder(const char *dir);   /* NULL if none */
  int game_installed(const game_info **out, int cap); /* count, table order */
  ```
  Ids: `"loony-labyrinth"` (index 0), `"crystal-caliburn"` (index 1).

CMake globs `src/*.c` and `tests/*.c` with `CONFIGURE_DEPENDS`, so new files need no CMake edit.

- [ ] **Step 1: Write the failing tests**

`tests/test_game.c`:

```c
#include "test.h"

#include <stdlib.h>
#include <unistd.h>

#include "game.h"
#include "util.h"

static void touch(const char *dir, const char *name) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (f)
        fclose(f);
}

/* Sets LOONY_APPS_DIR for one test; restore_apps_dir puts back the old value. */
static char saved_apps[1024];
static bool had_apps;

static void set_apps_dir(const char *dir) {
    const char *a = getenv("LOONY_APPS_DIR");
    had_apps = a != NULL;
    snprintf(saved_apps, sizeof saved_apps, "%s", a ? a : "");
    setenv("LOONY_APPS_DIR", dir, 1);
}

static void restore_apps_dir(void) {
    if (had_apps)
        setenv("LOONY_APPS_DIR", saved_apps, 1);
    else
        unsetenv("LOONY_APPS_DIR");
}

TEST(game_table_knows_both_games) {
    CHECK_EQ(game_count(), 2);
    CHECK_STR(game_at(0)->id, "loony-labyrinth");
    CHECK_STR(game_at(1)->id, "crystal-caliburn");
    CHECK(game_at(2) == NULL);
    const game_info *cc = game_by_id("crystal-caliburn");
    CHECK(cc != NULL);
    CHECK_STR(cc->title, "Crystal Caliburn");
    CHECK_STR(cc->folder_name, "Crystal Caliburn");
    CHECK_STR(cc->exe, "CRYSTAL CALIBURN 3.0.1");
    CHECK_STR(game_by_id("loony-labyrinth")->exe, "LOONY LABYRINTH 3.0.1");
    CHECK(game_by_id("pacman") == NULL);
}

TEST(game_folders_are_under_the_apps_dir) {
    char def[1024], out[1024];
    set_apps_dir(""); /* empty means unset */
    game_folder(game_at(1), def, sizeof def);
    setenv("LOONY_APPS_DIR", "/tmp/apps", 1);
    game_folder(game_at(0), out, sizeof out);
    restore_apps_dir();
    CHECK_STR(def, "/Applications/Crystal Caliburn");
    CHECK_STR(out, "/tmp/apps/Loony Labyrinth");
}

TEST(game_in_folder_finds_the_program_that_is_there) {
    char dir[1024];
    test_tmp_dir(dir, sizeof dir);
    CHECK(game_in_folder(dir) == NULL);
    touch(dir, "CRYSTAL CALIBURN 3.0.1");
    const game_info *g = game_in_folder(dir);
    touch(dir, "LOONY LABYRINTH 3.0.1");
    const game_info *both = game_in_folder(dir);
    test_remove_tree(dir);
    CHECK(g != NULL);
    CHECK_STR(g->id, "crystal-caliburn");
    CHECK_STR(both->id, "loony-labyrinth"); /* table order */
}

TEST(game_installed_lists_what_is_in_the_apps_dir) {
    char apps[1024], dir[1200];
    test_tmp_dir(apps, sizeof apps);
    set_apps_dir(apps);
    const game_info *g[4];
    int none = game_installed(g, 4);
    snprintf(dir, sizeof dir, "%s/Crystal Caliburn", apps);
    make_dirs(dir);
    touch(dir, "CRYSTAL CALIBURN 3.0.1");
    int one = game_installed(g, 4);
    const game_info *first = g[0];
    snprintf(dir, sizeof dir, "%s/Loony Labyrinth", apps);
    make_dirs(dir);
    touch(dir, "LOONY LABYRINTH 3.0.1");
    int two = game_installed(g, 4);
    restore_apps_dir();
    test_remove_tree(apps);
    CHECK_EQ(none, 0);
    CHECK_EQ(one, 1);
    CHECK_STR(first->id, "crystal-caliburn");
    CHECK_EQ(two, 2);
    CHECK_STR(g[0]->id, "loony-labyrinth");
    CHECK_STR(g[1]->id, "crystal-caliburn");
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build 2>&1 | tail -5`
Expected: `'game.h' file not found`.

- [ ] **Step 3: Write the header**

`src/game.h`:

```c
#pragma once
#include <stddef.h>

/* The LittleWing games the shim can play. They share one engine and make
   the same 132 calls, so they differ only in names. Adding a game is a row
   in game.c (the picker is laid out for two; a third needs a new layout). */
typedef struct {
    const char *id;          /* "loony-labyrinth": its save folder's name, LOONY_GAME's value */
    const char *title;       /* "Loony Labyrinth": the window title and the picker */
    const char *folder_name; /* its folder in the applications folder */
    const char *exe;         /* the program's file name in that folder */
} game_info;

size_t game_count(void);

/* The i-th game, or NULL past the end. */
const game_info *game_at(size_t i);

/* The game with this id, or NULL. */
const game_info *game_by_id(const char *id);

/* Where games are installed: $LOONY_APPS_DIR (for tests), or /Applications. */
const char *game_apps_dir(void);

/* The game's folder: <game_apps_dir()>/<folder_name>. */
void game_folder(const game_info *g, char *out, size_t cap);

/* The first game, in table order, whose program is readable in dir, or NULL. */
const game_info *game_in_folder(const char *dir);

/* Writes the games installed in their folders, in table order, to out (at
   most cap) and returns how many there are. */
int game_installed(const game_info **out, int cap);
```

- [ ] **Step 4: Write the implementation**

`src/game.c`:

```c
#include "game.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const game_info games[] = {
    {"loony-labyrinth", "Loony Labyrinth", "Loony Labyrinth", "LOONY LABYRINTH 3.0.1"},
    {"crystal-caliburn", "Crystal Caliburn", "Crystal Caliburn", "CRYSTAL CALIBURN 3.0.1"},
};
#define NGAMES (sizeof games / sizeof games[0])

size_t game_count(void) { return NGAMES; }

const game_info *game_at(size_t i) { return i < NGAMES ? &games[i] : NULL; }

const game_info *game_by_id(const char *id) {
    for (size_t i = 0; i < NGAMES; i++)
        if (strcmp(games[i].id, id) == 0)
            return &games[i];
    return NULL;
}

const char *game_apps_dir(void) {
    const char *d = getenv("LOONY_APPS_DIR");
    return d && *d ? d : "/Applications";
}

void game_folder(const game_info *g, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", game_apps_dir(), g->folder_name);
}

static bool has_program(const char *dir, const game_info *g) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, g->exe);
    return access(path, R_OK) == 0;
}

const game_info *game_in_folder(const char *dir) {
    for (size_t i = 0; i < NGAMES; i++)
        if (has_program(dir, &games[i]))
            return &games[i];
    return NULL;
}

int game_installed(const game_info **out, int cap) {
    int n = 0;
    for (size_t i = 0; i < NGAMES; i++) {
        char dir[PATH_MAX];
        game_folder(&games[i], dir, sizeof dir);
        if (has_program(dir, &games[i]) && n < cap)
            out[n++] = &games[i];
    }
    return n;
}
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build build && ./build/loony_tests game_`
Expected: `4 passed, 0 failed, 0 skipped`.

- [ ] **Step 6: Commit**

```bash
git add src/game.h src/game.c tests/test_game.c
git commit -m "A table of the LittleWing games the shim knows"
```

---

### Task 2: A save folder per game, and moving Loony's old one

**Files:**
- Modify: `src/files.h` (the `files_data_dir` declaration), `src/files.c:89-98`
- Modify: `src/game.h`, `src/game.c` (add `game_move_legacy_data`)
- Modify: `src/main.c` (the `files_data_dir` call)
- Test: `tests/test_files.c`, `tests/test_game.c`

**Interfaces:**
- Consumes: `game_by_id` (Task 1).
- Produces:
  ```c
  bool files_data_root(char *out, size_t cap);   /* false when LOONY_DATA_DIR is set or HOME isn't */
  bool files_data_dir(const char *game_id, char *out, size_t cap);   /* signature changed */
  int game_move_legacy_data(const char *root);
  #define GAME_PICKER_FILE "picker.plist"        /* in the root; Task 5 writes it */
  ```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_files.c`:

```c
/* files_data_root and files_data_dir read HOME and LOONY_DATA_DIR; these
   set them for one test and put them back. */
static char saved_home[1024], saved_data[1024];
static bool had_home, had_data;

static void set_env(const char *home, const char *data) {
    const char *h = getenv("HOME"), *d = getenv("LOONY_DATA_DIR");
    had_home = h != NULL;
    had_data = d != NULL;
    snprintf(saved_home, sizeof saved_home, "%s", h ? h : "");
    snprintf(saved_data, sizeof saved_data, "%s", d ? d : "");
    if (home)
        setenv("HOME", home, 1);
    else
        unsetenv("HOME");
    if (data)
        setenv("LOONY_DATA_DIR", data, 1);
    else
        unsetenv("LOONY_DATA_DIR");
}

static void restore_env(void) {
    if (had_home)
        setenv("HOME", saved_home, 1);
    else
        unsetenv("HOME");
    if (had_data)
        setenv("LOONY_DATA_DIR", saved_data, 1);
    else
        unsetenv("LOONY_DATA_DIR");
}

TEST(files_data_dir_is_per_game_under_home) {
    char root[1024], dir[1024];
    set_env("/Users/someone", NULL);
    bool have_root = files_data_root(root, sizeof root);
    bool have_dir = files_data_dir("crystal-caliburn", dir, sizeof dir);
    restore_env();
    CHECK(have_root);
    CHECK_STR(root, "/Users/someone/Library/Application Support/loony-shim");
    CHECK(have_dir);
    CHECK_STR(dir, "/Users/someone/Library/Application Support/loony-shim/crystal-caliburn");
}

/* Review Focus 5: LOONY_DATA_DIR is the save folder itself, for any game,
   and there is no shared root. */
TEST(files_data_dir_honors_loony_data_dir) {
    char root[1024], dir[1024];
    set_env("/Users/someone", "/tmp/somewhere");
    bool have_root = files_data_root(root, sizeof root);
    bool have_dir = files_data_dir("crystal-caliburn", dir, sizeof dir);
    restore_env();
    CHECK(!have_root);
    CHECK(have_dir);
    CHECK_STR(dir, "/tmp/somewhere");
}

TEST(files_data_dir_without_home_or_loony_data_dir) {
    char root[1024], dir[1024];
    set_env(NULL, NULL);
    bool have_root = files_data_root(root, sizeof root);
    bool have_dir = files_data_dir("loony-labyrinth", dir, sizeof dir);
    restore_env();
    CHECK(!have_root);
    CHECK(!have_dir);
}
```

Append to `tests/test_game.c`:

```c
static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static bool file_says(const char *path, const char *text) {
    size_t len = 0;
    char *got = (char *)read_file(path, &len);
    bool same = got && len == strlen(text) && memcmp(got, text, len) == 0;
    free(got);
    return same;
}

/* Review Focus 1: before Plan 8 Loony Labyrinth saved straight into the root. */
TEST(game_legacy_data_moves_into_loony_labyrinth) {
    char root[1024], path[1300];
    test_tmp_dir(root, sizeof root);
    snprintf(path, sizeof path, "%s/prefs.plist", root);
    write_text(path, "license");
    snprintf(path, sizeof path, "%s/" GAME_PICKER_FILE, root);
    write_text(path, "picker");
    snprintf(path, sizeof path, "%s/crystal-caliburn", root);
    make_dirs(path);
    CHECK_EQ(game_move_legacy_data(root), 1);
    snprintf(path, sizeof path, "%s/loony-labyrinth/prefs.plist", root);
    CHECK(file_says(path, "license"));
    snprintf(path, sizeof path, "%s/prefs.plist", root);
    CHECK(access(path, F_OK) != 0);
    snprintf(path, sizeof path, "%s/" GAME_PICKER_FILE, root);
    CHECK(file_says(path, "picker")); /* the picker's own file stays */
    snprintf(path, sizeof path, "%s/crystal-caliburn", root);
    CHECK(access(path, F_OK) == 0);
    CHECK_EQ(game_move_legacy_data(root), 0); /* nothing left to move */
    test_remove_tree(root);
}

TEST(game_legacy_data_never_overwrites) {
    char root[1024], path[1300];
    test_tmp_dir(root, sizeof root);
    snprintf(path, sizeof path, "%s/loony-labyrinth", root);
    make_dirs(path);
    snprintf(path, sizeof path, "%s/loony-labyrinth/prefs.plist", root);
    write_text(path, "newer");
    snprintf(path, sizeof path, "%s/prefs.plist", root);
    write_text(path, "older");
    CHECK_EQ(game_move_legacy_data(root), 0);
    snprintf(path, sizeof path, "%s/loony-labyrinth/prefs.plist", root);
    CHECK(file_says(path, "newer"));
    snprintf(path, sizeof path, "%s/prefs.plist", root);
    CHECK(file_says(path, "older")); /* left in place, and logged */
    test_remove_tree(root);
}

TEST(game_legacy_data_with_no_root_is_a_no_op) {
    CHECK_EQ(game_move_legacy_data("/nonexistent/loony-shim"), 0);
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build 2>&1 | tail -5`
Expected: compile errors: `files_data_root`, `game_move_legacy_data` and `GAME_PICKER_FILE` are undeclared.

- [ ] **Step 3: Change `files_data_dir` and add `files_data_root`**

In `src/files.h`, replace the `files_data_dir` comment and declaration with:

```c
/* The folder holding each game's save folder (and the picker's file):
   ~/Library/Application Support/loony-shim. False if LOONY_DATA_DIR is
   set (it is then the save folder itself) or HOME isn't. */
bool files_data_root(char *out, size_t cap);

/* The writable folder: $LOONY_DATA_DIR, or <files_data_root>/<game_id>.
   False if neither LOONY_DATA_DIR nor HOME is set. */
bool files_data_dir(const char *game_id, char *out, size_t cap);
```

In `src/files.c`, replace `files_data_dir` with:

```c
bool files_data_root(char *out, size_t cap) {
    const char *d = getenv("LOONY_DATA_DIR"), *home = getenv("HOME");
    if ((d && *d) || !home || !*home)
        return false;
    snprintf(out, cap, "%s/Library/Application Support/loony-shim", home);
    return true;
}

bool files_data_dir(const char *game_id, char *out, size_t cap) {
    const char *d = getenv("LOONY_DATA_DIR");
    if (d && *d) {
        snprintf(out, cap, "%s", d);
        return true;
    }
    char root[PATH_MAX];
    if (!files_data_root(root, sizeof root))
        return false;
    snprintf(out, cap, "%s/%s", root, game_id);
    return true;
}
```

In `src/main.c`, add `#include "game.h"` and change the call to `files_data_dir(game_at(0)->id, data_dir, sizeof data_dir)`. Task 3 passes the chosen game instead.

- [ ] **Step 4: Add `game_move_legacy_data`**

Append to `src/game.h`:

```c
/* The picker's file in the save root (files_data_root): the last game picked. */
#define GAME_PICKER_FILE "picker.plist"

/* Before Plan 8 there was one game, and its files (prefs.plist) were saved
   directly in the save root. Moves every entry of root that is neither a
   known game's folder nor GAME_PICKER_FILE into root/loony-labyrinth. An
   entry already there is left in place and logged, never overwritten.
   Returns the number moved; a missing root moves nothing. */
int game_move_legacy_data(const char *root);
```

Append to `src/game.c`, and add `#include <dirent.h>`, `#include <errno.h>` and `#include "util.h"` at the top:

```c
#define LEGACY_GAME "loony-labyrinth"

int game_move_legacy_data(const char *root) {
    DIR *d = opendir(root);
    if (!d)
        return 0;
    char dest[PATH_MAX];
    snprintf(dest, sizeof dest, "%s/%s", root, LEGACY_GAME);
    bool dest_ok = false;
    int moved = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strcmp(name, GAME_PICKER_FILE) == 0 ||
            game_by_id(name))
            continue;
        if (!dest_ok && !(dest_ok = make_dirs(dest))) {
            log_msg("can't create %s: %s", dest, strerror(errno));
            break;
        }
        char from[PATH_MAX], to[PATH_MAX];
        snprintf(from, sizeof from, "%s/%s", root, name);
        snprintf(to, sizeof to, "%s/%s", dest, name);
        if (access(to, F_OK) == 0) {
            log_msg("left %s where it is: %s already exists", from, to);
            continue;
        }
        if (rename(from, to) != 0) {
            log_msg("can't move %s to %s: %s", from, to, strerror(errno));
            continue;
        }
        log_msg("moved %s to %s", from, to);
        moved++;
    }
    closedir(d);
    return moved;
}
```

(Renaming entries out of a folder while reading it is allowed by POSIX. The destination folder created during the loop is skipped because its name is a game id.)

- [ ] **Step 5: Run the tests**

Run: `cmake --build build && ./build/loony_tests files_data && ./build/loony_tests game_`
Expected: `3 passed, 0 failed, 0 skipped`, then `7 passed, 0 failed, 0 skipped`.

- [ ] **Step 6: Commit**

```bash
git add src/files.h src/files.c src/game.h src/game.c src/main.c tests/test_files.c tests/test_game.c
git commit -m "A save folder per game, and a move for Loony Labyrinth's old one"
```

---

### Task 3: Choosing the game at launch, without the picker yet

**Files:**
- Modify: `src/main.c`, `src/display.h`, `src/display.c`
- Modify: `tests/test.h`, `tests/test_main.c` (Crystal Caliburn's folder helpers)
- Test: `tests/test_run.c`

**Interfaces:**
- Consumes: Task 1's `game_*` functions; `files_data_root`, `files_data_dir`, `game_move_legacy_data` (Task 2).
- Produces:
  - `void display_set_title(const char *title);` sets the title, also on an open window.
  - `main` picks the game:
    1. With a folder argument, the game in it. If none is found there, the first game, which only names the expected program in the "can't read" message.
    2. Otherwise `LOONY_GAME=<id>` picks that game from its folder (an unknown id is a startup error).
    3. Otherwise `game_installed`: none is a startup error naming each game's folder, and one plays directly. With two, it is `games[0]` until Task 5 adds the picker.
  - It logs `playing <title> from <folder>`.
  - Test helpers: `const char *test_cc_dir(void)` (`$LOONY_CC_DIR` or `/Applications/Crystal Caliburn`), `bool test_cc_present(void)`, `SKIP_UNLESS_CC()`.

- [ ] **Step 1: Add the test helpers**

In `tests/test.h`, after `bool test_game_present(void);`:

```c
/* Crystal Caliburn's folder: $LOONY_CC_DIR, or /Applications/Crystal Caliburn. */
const char *test_cc_dir(void);
bool test_cc_present(void);
```

and after `SKIP_UNLESS_GAME`:

```c
#define SKIP_UNLESS_CC()                                                        \
    do {                                                                        \
        if (!test_cc_present()) {                                               \
            test_skip("Crystal Caliburn not found");                            \
            return;                                                             \
        }                                                                       \
    } while (0)
```

In `tests/test_main.c`, after `test_game_present`:

```c
const char *test_cc_dir(void) {
    const char *d = getenv("LOONY_CC_DIR");
    return d && *d ? d : "/Applications/Crystal Caliburn";
}

bool test_cc_present(void) {
    char path[1100];
    snprintf(path, sizeof path, "%s/CRYSTAL CALIBURN 3.0.1", test_cc_dir());
    return access(path, R_OK) == 0;
}
```

- [ ] **Step 2: Write the failing tests**

In `tests/test_run.c`, add after `run_plays_the_opening_headless`:

```c
TEST(run_crystal_caliburn_plays_its_opening_headless) {
    SKIP_UNLESS_CC();
    const char *t = getenv("TMPDIR");
    snprintf(shot, sizeof shot, "%s/loony-run-XXXXXX", t && *t ? t : "/tmp");
    int fd = mkstemp(shot);
    CHECK(fd >= 0);
    close(fd);
    char out[32768];
    test_tmp_dir(run_data, sizeof run_data);
    int status = test_run_child(run_loony_headless, (void *)test_cc_dir(), out, sizeof out);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    size_t len = 0;
    uint8_t *png = read_file(shot, &len);
    unlink(shot);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "playing Crystal Caliburn from");
    CHECK_CONTAINS(out, "CRYSTAL CALIBURN 3.0.1: 132 imports");
    CHECK_CONTAINS(out, "loony: Alert 901 (answering item 1): Play Demo");
    CHECK_CONTAINS(out, "LittleWing CRYSTAL CALIBURN Pinball");
    CHECK_CONTAINS(out, "loony: Alert 900 (answering item 1): OK");
    CHECK_CONTAINS(out, "loony: exiting after 240 ticks (LOONY_EXIT_AFTER)");
    CHECK(!strstr(out, "unknown selector"));
    CHECK(!strstr(out, "not supported"));
    CHECK(png != NULL);
    CHECK(len > 33);
    CHECK_EQ(rd_be32(png + 16), 800);
    CHECK_EQ(rd_be32(png + 20), 600);
    free(png);
}
```

Then add, after `run_reports_missing_game_folder`. These tests make fake application folders: an empty program file is enough for `game_installed`, and a symbolic link to the real folder is used when the game has to run.

```c
/* Fake application folders: apps_dir holds links to the real game folders
   (or empty programs, for runs that never start a game). */
static char apps_dir[1024];

static bool link_game(const char *name, const char *real_dir) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", apps_dir, name);
    return symlink(real_dir, path) == 0;
}

static void run_loony_no_folder(void *unused) {
    (void)unused;
    setenv("LOONY_APPS_DIR", apps_dir, 1);
    if (run_data[0])
        setenv("LOONY_DATA_DIR", run_data, 1);
    setenv("LOONY_AUTO_ALERTS", "1", 1);
    execl(LOONY_BIN, "loony", (char *)NULL);
    _exit(127);
}

/* Review Focus 4. */
TEST(run_with_no_game_installed_says_where_to_put_them) {
    test_tmp_dir(apps_dir, sizeof apps_dir);
    char out[4096];
    int status = test_run_child(run_loony_no_folder, NULL, out, sizeof out);
    char expect[2600];
    snprintf(expect, sizeof expect,
             "no LittleWing game found: put Loony Labyrinth in %s/Loony Labyrinth or Crystal Caliburn in "
             "%s/Crystal Caliburn",
             apps_dir, apps_dir);
    test_remove_tree(apps_dir);
    CHECK_EQ(status, 1);
    CHECK_CONTAINS(out, expect);
}

/* Review Focus 4: one game installed plays at once, with no picker. */
static void run_loony_no_folder_briefly(void *unused) {
    setenv("LOONY_EXIT_AFTER", "60", 1);
    run_loony_no_folder(unused);
}

TEST(run_with_one_game_installed_skips_the_picker) {
    SKIP_UNLESS_CC();
    test_tmp_dir(apps_dir, sizeof apps_dir);
    CHECK(link_game("Crystal Caliburn", test_cc_dir()));
    test_tmp_dir(run_data, sizeof run_data);
    char out[32768];
    int status = test_run_child(run_loony_no_folder_briefly, NULL, out, sizeof out);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    test_remove_tree(apps_dir);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "playing Crystal Caliburn from");
    CHECK(!strstr(out, "picker"));
}

static void run_loony_game_env(void *id) {
    setenv("LOONY_GAME", (const char *)id, 1);
    setenv("LOONY_EXIT_AFTER", "60", 1);
    run_loony_no_folder(NULL);
}

TEST(run_loony_game_picks_without_a_folder) {
    SKIP_UNLESS_CC();
    test_tmp_dir(apps_dir, sizeof apps_dir);
    CHECK(link_game("Crystal Caliburn", test_cc_dir()));
    test_tmp_dir(run_data, sizeof run_data);
    char out[32768], bad_out[4096];
    int status = test_run_child(run_loony_game_env, (void *)"crystal-caliburn", out, sizeof out);
    int bad = test_run_child(run_loony_game_env, (void *)"pacman", bad_out, sizeof bad_out);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    test_remove_tree(apps_dir);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "playing Crystal Caliburn from");
    CHECK_EQ(bad, 1);
    CHECK_CONTAINS(bad_out, "LOONY_GAME names no known game: pacman");
}

/* Review Focus 1 and 5: with no LOONY_DATA_DIR, a game saves in
   ~/Library/Application Support/loony-shim/<id>, and Loony Labyrinth's
   file from before Plan 8 moves into its folder. HOME is temporary. */
static char save_home[1024];

static void run_with_home_saves(void *dir) {
    setenv("HOME", save_home, 1);
    unsetenv("LOONY_DATA_DIR");
    setenv("LOONY_AUTO_ALERTS", "1", 1);
    setenv("LOONY_EXIT_AFTER", "240", 1);
    execl(LOONY_BIN, "loony", (const char *)dir, (char *)NULL);
    _exit(127);
}

TEST(run_each_game_saves_in_its_own_folder) {
    SKIP_UNLESS_CC();
    test_tmp_dir(save_home, sizeof save_home);
    char root[1100], path[1300];
    snprintf(root, sizeof root, "%s/Library/Application Support/loony-shim", save_home);
    CHECK(make_dirs(root));
    snprintf(path, sizeof path, "%s/prefs.plist", root);
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    fputs("Loony's license", f);
    fclose(f);
    char out[32768];
    int status = test_run_child(run_with_home_saves, (void *)test_cc_dir(), out, sizeof out);
    CHECK_EQ(status, 0);
    snprintf(path, sizeof path, "%s/crystal-caliburn/prefs.plist", root);
    CHECK(access(path, R_OK) == 0);
    snprintf(path, sizeof path, "%s/loony-labyrinth/prefs.plist", root);
    size_t len = 0;
    char *moved = (char *)read_file(path, &len);
    CHECK(moved != NULL);
    CHECK(len == 15 && memcmp(moved, "Loony's license", 15) == 0);
    free(moved);
    snprintf(path, sizeof path, "%s/prefs.plist", root);
    CHECK(access(path, F_OK) != 0);
    test_remove_tree(save_home);
}
```

- [ ] **Step 3: Run them to see them fail**

Run: `cmake --build build && ./build/loony_tests run_ 2>&1 | grep -B1 -A3 "CHECK failed" | head -60`
Expected: each of the five new tests fails. There is no `playing ...` line, the no-folder runs read `/Applications/Loony Labyrinth`, and the save lands directly in the data folder.

- [ ] **Step 4: Add `display_set_title`**

In `src/display.h`, after `display_init`:

```c
/* The window's title. Sets it at once on an open window. */
void display_set_title(const char *title);
```

In `src/display.c`, add `const char *title;` to `D`, then add:

```c
void display_set_title(const char *title) {
    D.title = title;
    if (D.window)
        SDL_SetWindowTitle(D.window, title);
}
```

In `open_window`, change the `SDL_CreateWindow` call to use `D.title ? D.title : "LittleWing"`.

- [ ] **Step 5: Choose the game in `main.c`**

In `src/main.c`:

1. Delete `#define DEFAULT_GAME_DIR` and `#define GAME_EXE_NAME`.
2. In `show_failure`, change the message box title `"Loony Labyrinth"` to `"LittleWing"`.
3. Replace the start of `main`, up to and including the `startup_error("can't read ...")` call, with:

```c
/* The games' folders, for the message when none is installed. */
static int no_game_error(void) {
    char msg[2048] = "no LittleWing game found: put ";
    for (size_t i = 0; i < game_count(); i++) {
        char dir[PATH_MAX];
        game_folder(game_at(i), dir, sizeof dir);
        size_t used = strlen(msg);
        snprintf(msg + used, sizeof msg - used, "%s%s in %s", i ? " or " : "", game_at(i)->title, dir);
    }
    return startup_error("%s", msg);
}

int main(int argc, char **argv) {
    log_to_file_if_app(argv[0]);
    const char *dir_arg = NULL;
    int nargs = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-psn_", 5) == 0) /* older macOS adds this when launching an app */
            continue;
        dir_arg = argv[i];
        nargs++;
    }
    if (nargs > 1)
        return startup_error("usage: loony [game-folder]");

    /* The game: the one in the folder given; else LOONY_GAME's; else the
       one installed, or the picker's choice when there are more. */
    const game_info *game;
    char dir[PATH_MAX];
    const char *forced = getenv("LOONY_GAME");
    if (dir_arg) {
        snprintf(dir, sizeof dir, "%s", dir_arg);
        game = game_in_folder(dir);
        if (!game)
            game = game_at(0); /* names the program expected, in the message below */
    } else if (forced && *forced) {
        game = game_by_id(forced);
        if (!game)
            return startup_error("LOONY_GAME names no known game: %s", forced);
        game_folder(game, dir, sizeof dir);
    } else {
        const game_info *installed[8];
        int n = game_installed(installed, 8);
        if (n == 0)
            return no_game_error();
        game = installed[0]; /* Task 5: the picker, when n > 1 */
        game_folder(game, dir, sizeof dir);
    }

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, game->exe);
    size_t len = 0;
    uint8_t *buf = read_file(path, &len);
    if (!buf)
        return startup_error("can't read %s: %s. %s needs the original game in %s.", path,
                             strerror(errno), game->title, dir);
    log_msg("playing %s from %s", game->title, dir);
```

4. Replace the save folder lines (from `char data_dir[PATH_MAX];` through `log_msg("neither ...")`) with:

```c
    char data_root[PATH_MAX], data_dir[PATH_MAX];
    if (files_data_root(data_root, sizeof data_root))
        game_move_legacy_data(data_root);
    bool have_data = files_data_dir(game->id, data_dir, sizeof data_dir);
    if (!have_data)
        log_msg("neither LOONY_DATA_DIR nor HOME is set: nothing will be saved");
```

5. After `display_init();`, add `display_set_title(game->title);`.

The existing `run_reports_missing_game_folder` and `run_as_the_app_logs_to_library_logs` still pass. Their message reads `... Loony Labyrinth needs the original game in /nonexistent/loony.`

- [ ] **Step 6: Run the whole suite**

Run: `cmake --build build && ./build/loony_tests 2>&1 | tail -3`
Expected: `0 failed`. It takes about 10 minutes, and Loony's goldens and recording still match.

- [ ] **Step 7: Commit**

```bash
git add src/main.c src/display.h src/display.c tests/test.h tests/test_main.c tests/test_run.c
git commit -m "Choose the game at launch: the folder, LOONY_GAME, or the one installed"
```

---

### Task 4: Drawing the picker

**Files:**
- Create: `src/picker.h`, `src/picker.c` (drawing, art and input only; Task 5 adds the loop)
- Test: `tests/test_picker.c`

**Interfaces:**
- Consumes: `game_info` (Task 1); `rsrc_open`, `rsrc_find`, `rsrc_data`, `rsrc_close` (`src/rsrc.h`); `pict_frame`, `pict_draw` (`src/pict.h`); `qd_fill`, `qd_pixels`, `qd_rect`, `qd_rgb` (`src/blit.h`); `font_init`, `font_glyph` (`src/font.h`); `read_file`, `fnv1a32` (`src/util.h`).
- Produces (all pixels are QuickDraw 32-bit: big-endian xRGB, 4 bytes per pixel):
  ```c
  #define PICKER_W 800
  #define PICKER_H 600
  #define PICKER_ART_W 384
  #define PICKER_ART_H 288
  #define PICKER_MAX 2
  typedef struct { const game_info *game; uint8_t *art; } picker_entry;  /* art NULL: a plain card */
  bool picker_load_art(const char *exe_path, uint8_t *art, char *err, size_t errlen);
  void picker_draw(const picker_entry *e, int n, int selected, uint8_t *screen);
  int picker_hit(int n, int x, int y);                      /* -1 outside the cards */
  int picker_key(int n, int selected, int scancode, bool *choose);
  int picker_initial(const picker_entry *e, int n, const char *last_id);
  ```

**Layout (800×600, black):**
- **Heading:** `LITTLEWING PINBALL`, the 8×8 font at 3×, gold, centered, top at y=64.
- **Cards:** two 384×288 cards, tops at y=140, lefts at x=10 and x=406.
  - The selected card has a 4-pixel gold border (#FFCC33) just outside it; the other has a dark gray border (#333333).
  - A card without art is filled #222222.
- **Names:** each game's title in capitals, 2×, centered under its card with the top at y=448. White when selected, #888888 otherwise.
- **Hint:** `RETURN TO PLAY - CMD-Q TO QUIT`, 2×, #888888, centered, top at y=540. (The font has no arrows; the arrow keys are mentioned only in the README.)

**The art:** PICT 800 (512×384) is drawn into a 512×384 32-bit buffer, then reduced to 384×288 by averaging. Each output pixel averages the source pixels in `[x*4/3, (x+1)*4/3) × [y*4/3, (y+1)*4/3)`. A missing or different-sized PICT 800 is an error (`err` set), and the card is then drawn plain.

- [ ] **Step 1: Write the failing tests**

`tests/test_picker.c`:

```c
#include "test.h"

#include <SDL3/SDL_scancode.h>
#include <stdlib.h>

#include "game.h"
#include "picker.h"
#include "util.h"

static uint8_t screen[PICKER_W * PICKER_H * 4];

static uint8_t *solid_art(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *a = malloc(PICKER_ART_W * PICKER_ART_H * 4);
    for (int i = 0; i < PICKER_ART_W * PICKER_ART_H; i++) {
        a[i * 4] = 0;
        a[i * 4 + 1] = r;
        a[i * 4 + 2] = g;
        a[i * 4 + 3] = b;
    }
    return a;
}

static const uint8_t *px(int x, int y) { return screen + ((size_t)y * PICKER_W + (size_t)x) * 4; }

TEST(picker_draw_places_the_cards_and_marks_the_selection) {
    picker_entry e[2] = {{game_at(0), solid_art(0xFF, 0, 0)}, {game_at(1), solid_art(0, 0, 0xFF)}};
    picker_draw(e, 2, 1, screen);
    free(e[0].art);
    free(e[1].art);
    CHECK_EQ(px(10 + 100, 140 + 100)[1], 0xFF); /* the first card's art */
    CHECK_EQ(px(406 + 100, 140 + 100)[3], 0xFF); /* the second card's art */
    CHECK_EQ(px(406 - 2, 200)[1], 0xFF);         /* gold border: the second is selected */
    CHECK_EQ(px(406 - 2, 200)[2], 0xCC);
    CHECK_EQ(px(10 - 2, 200)[1], 0x33);          /* gray border on the first */
    CHECK_EQ(px(0, 0)[1], 0);                    /* black background */
}

/* Review Focus 4: a game whose art couldn't be read gets a plain card. */
TEST(picker_draw_without_art) {
    picker_entry e[2] = {{game_at(0), NULL}, {game_at(1), NULL}};
    picker_draw(e, 2, 0, screen);
    CHECK_EQ(px(10 + 100, 140 + 100)[1], 0x22);
    CHECK_EQ(px(406 + 100, 140 + 100)[1], 0x22);
}

/* The whole frame, with solid art, pinned by a hash recorded when this
   task was implemented (picker_draw is deterministic). */
TEST(picker_frame_matches_its_recording) {
    picker_entry e[2] = {{game_at(0), solid_art(0x80, 0x40, 0x20)}, {game_at(1), NULL}};
    picker_draw(e, 2, 0, screen);
    free(e[0].art);
    char got[16];
    snprintf(got, sizeof got, "%08x", fnv1a32(screen, sizeof screen));
    CHECK_STR(got, "");
}

TEST(picker_hit_finds_the_card_under_a_click) {
    CHECK_EQ(picker_hit(2, 10 + 5, 140 + 5), 0);
    CHECK_EQ(picker_hit(2, 406 + 383, 140 + 287), 1);
    CHECK_EQ(picker_hit(2, 400, 300), -1); /* the gap */
    CHECK_EQ(picker_hit(2, 100, 60), -1);  /* the heading */
    CHECK_EQ(picker_hit(2, 100, 460), 0);  /* the name under a card counts */
}

TEST(picker_keys_move_and_choose) {
    bool choose = false;
    CHECK_EQ(picker_key(2, 0, SDL_SCANCODE_RIGHT, &choose), 1);
    CHECK(!choose);
    CHECK_EQ(picker_key(2, 1, SDL_SCANCODE_RIGHT, &choose), 1); /* stops at the end */
    CHECK_EQ(picker_key(2, 1, SDL_SCANCODE_LEFT, &choose), 0);
    CHECK_EQ(picker_key(2, 0, SDL_SCANCODE_LEFT, &choose), 0);
    CHECK_EQ(picker_key(2, 1, SDL_SCANCODE_RETURN, &choose), 1);
    CHECK(choose);
    choose = false;
    CHECK_EQ(picker_key(2, 0, SDL_SCANCODE_KP_ENTER, &choose), 0);
    CHECK(choose);
    choose = false;
    CHECK_EQ(picker_key(2, 0, SDL_SCANCODE_Z, &choose), 0);
    CHECK(!choose);
}

TEST(picker_starts_on_the_last_game) {
    picker_entry e[2] = {{game_at(0), NULL}, {game_at(1), NULL}};
    CHECK_EQ(picker_initial(e, 2, "crystal-caliburn"), 1);
    CHECK_EQ(picker_initial(e, 2, "loony-labyrinth"), 0);
    CHECK_EQ(picker_initial(e, 2, ""), 0);
    CHECK_EQ(picker_initial(e, 2, "pacman"), 0);
}

TEST(picker_art_comes_from_the_games_title_picture) {
    SKIP_UNLESS_GAME();
    uint8_t *art = malloc(PICKER_ART_W * PICKER_ART_H * 4);
    char err[256];
    bool ok = picker_load_art(test_game_exe_path(), art, err, sizeof err);
    bool bad = picker_load_art("/nonexistent/LOONY LABYRINTH 3.0.1", art + 0, err, sizeof err);
    free(art);
    CHECK(ok);
    CHECK(!bad);
    CHECK_CONTAINS(err, "/nonexistent/LOONY LABYRINTH 3.0.1");
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build 2>&1 | tail -5`
Expected: `'picker.h' file not found`.

- [ ] **Step 3: Write the header**

`src/picker.h`:

```c
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "game.h"

/* The game picker LittleWing.app shows when more than one game is
   installed: each game's title picture (PICT 800 in its own resource fork,
   read from the user's copy) on a card, its name under it, and the
   selection marked in gold. This file draws and reads input; picker_run
   (Task 5) shows it. Pixels here are QuickDraw 32-bit: big-endian xRGB. */

#define PICKER_W 800
#define PICKER_H 600
#define PICKER_ART_W 384 /* PICT 800 (512x384) at 3/4 */
#define PICKER_ART_H 288
#define PICKER_MAX 2     /* the layout fits two games */

typedef struct {
    const game_info *game;
    uint8_t *art; /* PICKER_ART_W x PICKER_ART_H, or NULL for a plain card */
} picker_entry;

/* Reads the program's resource fork (exe_path/..namedfork/rsrc), draws its
   PICT 800 and reduces it into art. False, with err naming the file, if
   the fork or the picture can't be read or the picture isn't 512x384.
   Opens and closes the Resource Manager's fork, so call it before the
   game's own is opened. */
bool picker_load_art(const char *exe_path, uint8_t *art, char *err, size_t errlen);

/* Draws the whole picker into screen (PICKER_W x PICKER_H) with entry
   `selected` marked. n is at most PICKER_MAX. */
void picker_draw(const picker_entry *e, int n, int selected, uint8_t *screen);

/* The entry whose card (with its border and name) holds the point, or -1. */
int picker_hit(int n, int x, int y);

/* The selection after an SDL scancode: Left and Right move (stopping at
   the ends), Return and keypad Enter set *choose. Other keys change nothing. */
int picker_key(int n, int selected, int scancode, bool *choose);

/* The entry for last_id (the last game picked), or 0. */
int picker_initial(const picker_entry *e, int n, const char *last_id);
```

- [ ] **Step 4: Write the implementation**

`src/picker.c`:

```c
#include "picker.h"

#include <SDL3/SDL_scancode.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blit.h"
#include "font.h"
#include "pict.h"
#include "rsrc.h"
#include "util.h"

#define ART_SRC_W 512
#define ART_SRC_H 384
#define CARD_TOP 140
#define CARD_GAP 12
#define CARD_LEFT0 10
#define BORDER 4
#define NAME_TOP 448
#define HEADING_TOP 64
#define HINT_TOP 540

static const qd_rgb BLACK = {0, 0, 0}, GOLD = {0xFFFF, 0xCCCC, 0x3333}, DARK = {0x3333, 0x3333, 0x3333},
                    PLAIN = {0x2222, 0x2222, 0x2222}, WHITE = {0xFFFF, 0xFFFF, 0xFFFF},
                    GRAY = {0x8888, 0x8888, 0x8888};

static int card_left(int i) { return CARD_LEFT0 + i * (PICKER_ART_W + CARD_GAP); }

static qd_pixels pixels(uint8_t *base, int w, int h) {
    qd_pixels p = {base, (uint32_t)w * 4, {0, 0, (int16_t)h, (int16_t)w}, 32, NULL};
    return p;
}

static qd_rect rect(int left, int top, int w, int h) {
    qd_rect r = {(int16_t)top, (int16_t)left, (int16_t)(top + h), (int16_t)(left + w)};
    return r;
}

/* ASCII text at `scale`, each glyph bit a scale x scale square, centered on cx. */
static void draw_text(const qd_pixels *s, int cx, int top, const char *text, int scale, qd_rgb c) {
    int n = (int)strlen(text), x0 = cx - n * FONT_W * scale / 2;
    for (int i = 0; i < n; i++) {
        const uint8_t *g = font_glyph((uint8_t)text[i]);
        for (int row = 0; row < FONT_H; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] & (0x80 >> col))
                    qd_fill(s, rect(x0 + (i * FONT_W + col) * scale, top + row * scale, scale, scale), s->bounds, c);
    }
}

void picker_draw(const picker_entry *e, int n, int selected, uint8_t *screen) {
    font_init();
    qd_pixels s = pixels(screen, PICKER_W, PICKER_H);
    qd_fill(&s, s.bounds, s.bounds, BLACK);
    draw_text(&s, PICKER_W / 2, HEADING_TOP, "LITTLEWING PINBALL", 3, GOLD);
    for (int i = 0; i < n && i < PICKER_MAX; i++) {
        int left = card_left(i);
        qd_fill(&s, rect(left - BORDER, CARD_TOP - BORDER, PICKER_ART_W + 2 * BORDER, PICKER_ART_H + 2 * BORDER),
                s.bounds, i == selected ? GOLD : DARK);
        if (e[i].art)
            for (int y = 0; y < PICKER_ART_H; y++)
                memcpy(screen + ((size_t)(CARD_TOP + y) * PICKER_W + (size_t)left) * 4,
                       e[i].art + (size_t)y * PICKER_ART_W * 4, PICKER_ART_W * 4);
        else
            qd_fill(&s, rect(left, CARD_TOP, PICKER_ART_W, PICKER_ART_H), s.bounds, PLAIN);
        char name[64];
        size_t k = 0;
        for (const char *p = e[i].game->title; *p && k + 1 < sizeof name; p++)
            name[k++] = (char)toupper((unsigned char)*p);
        name[k] = '\0';
        draw_text(&s, left + PICKER_ART_W / 2, NAME_TOP, name, 2, i == selected ? WHITE : GRAY);
    }
    draw_text(&s, PICKER_W / 2, HINT_TOP, "RETURN TO PLAY - CMD-Q TO QUIT", 2, GRAY);
}

int picker_hit(int n, int x, int y) {
    for (int i = 0; i < n && i < PICKER_MAX; i++) {
        int left = card_left(i) - BORDER;
        if (x >= left && x < left + PICKER_ART_W + 2 * BORDER && y >= CARD_TOP - BORDER &&
            y < NAME_TOP + 2 * FONT_H)
            return i;
    }
    return -1;
}

int picker_key(int n, int selected, int scancode, bool *choose) {
    switch (scancode) {
    case SDL_SCANCODE_LEFT: return selected > 0 ? selected - 1 : 0;
    case SDL_SCANCODE_RIGHT: return selected < n - 1 ? selected + 1 : n - 1;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: *choose = true; return selected;
    default: return selected;
    }
}

int picker_initial(const picker_entry *e, int n, const char *last_id) {
    for (int i = 0; i < n; i++)
        if (strcmp(e[i].game->id, last_id) == 0)
            return i;
    return 0;
}

/* src (512x384) reduced to 3/4 by averaging the source pixels under each output pixel. */
static void reduce(const uint8_t *src, uint8_t *dst) {
    for (int y = 0; y < PICKER_ART_H; y++) {
        int y0 = y * ART_SRC_H / PICKER_ART_H, y1 = (y + 1) * ART_SRC_H / PICKER_ART_H;
        for (int x = 0; x < PICKER_ART_W; x++) {
            int x0 = x * ART_SRC_W / PICKER_ART_W, x1 = (x + 1) * ART_SRC_W / PICKER_ART_W;
            unsigned sum[3] = {0, 0, 0}, count = 0;
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++, count++)
                    for (int c = 0; c < 3; c++)
                        sum[c] += src[((size_t)sy * ART_SRC_W + (size_t)sx) * 4 + 1 + (size_t)c];
            uint8_t *d = dst + ((size_t)y * PICKER_ART_W + (size_t)x) * 4;
            d[0] = 0;
            for (int c = 0; c < 3; c++)
                d[1 + c] = (uint8_t)(sum[c] / count);
        }
    }
}

bool picker_load_art(const char *exe_path, uint8_t *art, char *err, size_t errlen) {
    char fork_path[PATH_MAX];
    snprintf(fork_path, sizeof fork_path, "%s/..namedfork/rsrc", exe_path);
    size_t len = 0;
    uint8_t *fork = read_file(fork_path, &len);
    if (!fork) {
        snprintf(err, errlen, "can't read %s", fork_path);
        return false;
    }
    char rerr[256];
    bool ok = false;
    if (!rsrc_open(fork, len, rerr, sizeof rerr)) {
        snprintf(err, errlen, "%s: %s", exe_path, rerr);
        free(fork);
        return false;
    }
    rsrc_entry *e = rsrc_find(FOURCC('P', 'I', 'C', 'T'), 800);
    qd_rect frame;
    uint8_t *full = NULL;
    if (!e || !pict_frame(rsrc_data(e), e->len, &frame) || rect_w(frame) != ART_SRC_W ||
        rect_h(frame) != ART_SRC_H) {
        snprintf(err, errlen, "%s has no 512x384 PICT 800", exe_path);
    } else if (!(full = calloc((size_t)ART_SRC_W * ART_SRC_H, 4))) {
        fatal("out of memory");
    } else {
        qd_pixels t = pixels(full, ART_SRC_W, ART_SRC_H);
        ok = pict_draw(rsrc_data(e), e->len, t.bounds, &t, t.bounds, BLACK, WHITE, rerr, sizeof rerr);
        if (ok)
            reduce(full, art);
        else
            snprintf(err, errlen, "%s: PICT 800: %s", exe_path, rerr);
    }
    free(full);
    rsrc_close();
    free(fork);
    return ok;
}
```

(`FOURCC` comes from `util.h`. `font_init` is safe to call more than once. The drawing is all integer arithmetic over fixed inputs, so the frame hash is the same in Debug and Release.)

- [ ] **Step 5: Record the frame hash and run the tests**

Run: `cmake --build build && ./build/loony_tests picker_`
Expected: everything passes except `picker_frame_matches_its_recording`, which fails with `got == "" ("xxxxxxxx" != "")`. Paste that hash in as the expected value, then run again in Debug and in Release (`cmake --build build-release && ./build-release/loony_tests picker_`). Expected: `7 passed` in both.

- [ ] **Step 6: Commit**

```bash
git add src/picker.h src/picker.c tests/test_picker.c
git commit -m "Draw the game picker: each game's title picture, its name, the selection"
```

---

### Task 5: Showing the picker, and returning to it

**Files:**
- Modify: `src/picker.h`, `src/picker.c` (`picker_run`, the last game remembered)
- Modify: `src/display.h`, `src/display.c` (`display_present_rgba`, `display_fullscreen`, `LOONY_FULLSCREEN`)
- Modify: `src/events.h`, `src/events.c` (`events_quit_requested`)
- Modify: `src/misc.h`, `src/misc.c` (`misc_set_exit_hook`)
- Modify: `src/main.c` (the picker, the return, an inherited log)
- Test: `tests/test_run.c`

**Interfaces:**
- Consumes: Tasks 1–4. Also `plist_read`, `plist_write`, `plist_free` (`src/plist.h`), `files_data_root` (Task 2), `png_write_rgba`, and `qd_to_rgba` (`src/blit.h`).
- Produces:
  ```c
  const game_info *picker_run(const game_info *const *games, int n);   /* exits on quit */
  void display_present_rgba(const uint8_t *rgba, int w, int h);
  bool display_fullscreen(void);
  bool events_quit_requested(void);     /* the host asked (Cmd-Q, window close, script quit) */
  void misc_set_exit_hook(void (*fn)(void));   /* ExitToShell calls fn before exiting */
  ```
- Environment:
  - `LOONY_PICK=<id|quit>[,<id|quit>...]`: for tests. Each time the picker appears, it shows one frame, then acts on the first item and passes on the rest.
  - `LOONY_PICKER_SHOT=<png>` writes the picker's first frame.
  - `LOONY_FULLSCREEN=1` opens the window full screen.
  - `LOONY_LOG_INHERITED=1` keeps the log the previous process opened.
  - The last two are set by the shim itself before it restarts.

**How it fits together:**
1. `main` runs the picker when no folder is given, `LOONY_GAME` isn't set, and two or more games are installed. The picker draws through `display_present_rgba`, in the same window the game then uses. A pick sets `return_to_picker` and remembers the game in `<files_data_root>/picker.plist` (key `last game`), unless `LOONY_DATA_DIR` is set.
2. When the game ends by itself (`ExitToShell`, or its `main` returning) with `return_to_picker` set, `events_quit_requested()` false and no failure:
   - it saves the preferences (`cf_save_prefs`);
   - it logs `back to the picker`;
   - it sets `LOONY_FULLSCREEN` and, when logging to a file, `LOONY_LOG_INHERITED`;
   - it `execv`s its own executable (`_NSGetExecutablePath`) with no arguments.
3. Cmd-Q, a window close or a script `quit` goes through the quit Apple Event as now. `events_quit_requested()` is then true, so the process exits.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_run.c`, after `run_each_game_saves_in_its_own_folder`:

```c
/* The game's own QUIT (Facts measured): Esc ends the demo, Esc opens the
   menu, Up wraps to its quit item, Return. */
static const char menu_quit_script[] = "1720 down esc\n1724 up esc\n1800 down esc\n1804 up esc\n"
                                       "1900 down up\n1904 up up\n2000 down return\n2004 up return\n";

static char pick[256], pick_shot[1024];

static void run_picker(void *unused) {
    setenv("LOONY_PICK", pick, 1);
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    if (pick_shot[0])
        setenv("LOONY_PICKER_SHOT", pick_shot, 1);
    if (script_path[0])
        setenv("LOONY_SCRIPT", script_path, 1);
    run_loony_no_folder(unused);
}

static bool both_games(void) {
    return test_game_present() && test_cc_present() && link_game("Loony Labyrinth", test_game_dir()) &&
           link_game("Crystal Caliburn", test_cc_dir());
}

static int run_picker_with(const char *picks, const char *script, char *out, size_t outlen) {
    snprintf(pick, sizeof pick, "%s", picks);
    script_path[0] = '\0';
    if (script) {
        tmp_name(script_path, sizeof script_path, "script");
        FILE *f = fopen(script_path, "w");
        fputs(script, f);
        fclose(f);
    }
    int status = test_run_child(run_picker, NULL, out, outlen);
    if (script)
        unlink(script_path);
    script_path[0] = '\0';
    return status;
}

/* Review Focus 2 and 3: the game's own QUIT goes back to the picker; the
   log carries on across the restart. */
TEST(run_quitting_from_the_game_menu_returns_to_the_picker) {
    SKIP_UNLESS_GAME();
    SKIP_UNLESS_CC();
    test_tmp_dir(apps_dir, sizeof apps_dir);
    CHECK(both_games());
    test_tmp_dir(run_data, sizeof run_data);
    char out[65536];
    int status = run_picker_with("crystal-caliburn,quit", menu_quit_script, out, sizeof out);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    test_remove_tree(apps_dir);
    CHECK_EQ(status, 0);
    const char *a = strstr(out, "picker: crystal-caliburn");
    const char *b = a ? strstr(a, "playing Crystal Caliburn from") : NULL;
    const char *c = b ? strstr(b, "ExitToShell") : NULL;
    const char *d = c ? strstr(c, "back to the picker") : NULL;
    const char *e = d ? strstr(d, "picker: quit") : NULL;
    CHECK(a && b && c && d && e);
    CHECK(!strstr(out, "quit Apple Event"));
}

/* Review Focus 2: Cmd-Q (here a script quit, which takes the same path)
   during a picked game quits the app; the second pick is never used. */
TEST(run_cmd_q_in_a_picked_game_quits_the_app) {
    SKIP_UNLESS_GAME();
    SKIP_UNLESS_CC();
    test_tmp_dir(apps_dir, sizeof apps_dir);
    CHECK(both_games());
    test_tmp_dir(run_data, sizeof run_data);
    char out[65536];
    int status = run_picker_with("loony-labyrinth,crystal-caliburn", "300 quit\n", out, sizeof out);
    test_remove_tree(run_data);
    run_data[0] = '\0';
    test_remove_tree(apps_dir);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "picker: loony-labyrinth");
    CHECK_CONTAINS(out, "sending the quit Apple Event");
    CHECK(!strstr(out, "back to the picker"));
    CHECK(!strstr(out, "picker: crystal-caliburn"));
}

TEST(run_the_picker_frame_shows_both_games) {
    SKIP_UNLESS_GAME();
    SKIP_UNLESS_CC();
    test_tmp_dir(apps_dir, sizeof apps_dir);
    CHECK(both_games());
    test_tmp_dir(run_data, sizeof run_data);
    tmp_name(pick_shot, sizeof pick_shot, "picker");
    char out[16384];
    int status = run_picker_with("quit", NULL, out, sizeof out);
    size_t len = 0;
    uint8_t *png = read_file(pick_shot, &len);
    unlink(pick_shot);
    pick_shot[0] = '\0';
    test_remove_tree(run_data);
    run_data[0] = '\0';
    test_remove_tree(apps_dir);
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "can't read")); /* both pictures loaded */
    CHECK(png != NULL);
    CHECK_EQ(rd_be32(png + 16), 800);
    CHECK_EQ(rd_be32(png + 20), 600);
    char got[16];
    snprintf(got, sizeof got, "%08x", fnv1a32(png, len));
    free(png);
    CHECK_STR(got, ""); /* recorded in Task 8, after the user approves the picker */
}

/* Review Focus 1: the picker remembers the last game in the save root,
   and each game's preferences land in its own folder. */
static void run_picker_home(void *unused) {
    setenv("HOME", save_home, 1);
    unsetenv("LOONY_DATA_DIR");
    run_data[0] = '\0';
    run_picker(unused);
}

TEST(run_the_picker_remembers_and_each_game_saves_apart) {
    SKIP_UNLESS_GAME();
    SKIP_UNLESS_CC();
    test_tmp_dir(apps_dir, sizeof apps_dir);
    CHECK(both_games());
    test_tmp_dir(save_home, sizeof save_home);
    snprintf(pick, sizeof pick, "crystal-caliburn,quit");
    tmp_name(script_path, sizeof script_path, "script");
    FILE *f = fopen(script_path, "w");
    fputs(menu_quit_script, f);
    fclose(f);
    char out[65536];
    int status = test_run_child(run_picker_home, NULL, out, sizeof out);
    unlink(script_path);
    script_path[0] = '\0';
    char root[1100], path[1300];
    snprintf(root, sizeof root, "%s/Library/Application Support/loony-shim", save_home);
    snprintf(path, sizeof path, "%s/picker.plist", root);
    plist_entry *e = NULL;
    uint32_t n = 0;
    char err[256];
    plist_status ps = plist_read(path, &e, &n, err, sizeof err);
    bool remembered = false;
    for (uint32_t i = 0; ps == PLIST_OK && i < n; i++)
        remembered |= strcmp(e[i].key, "last game") == 0 && !e[i].is_number &&
                      strcmp(e[i].str, "crystal-caliburn") == 0;
    if (ps == PLIST_OK)
        plist_free(e, n);
    snprintf(path, sizeof path, "%s/crystal-caliburn/prefs.plist", root);
    bool cc_saved = access(path, R_OK) == 0;
    snprintf(path, sizeof path, "%s/prefs.plist", root);
    bool none_at_root = access(path, F_OK) != 0;
    test_remove_tree(save_home);
    test_remove_tree(apps_dir);
    CHECK_EQ(status, 0);
    CHECK(remembered);
    CHECK(cc_saved);
    CHECK(none_at_root);
}
```

Add `#include "plist.h"` to `tests/test_run.c`'s includes.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build && ./build/loony_tests picker 2>&1 | grep -B1 -A3 "CHECK failed\|^run_" | head -60`
Expected: the four `run_` tests fail. No `picker:` lines appear, because Task 3 plays the first installed game.

- [ ] **Step 3: `events_quit_requested` and `misc_set_exit_hook`**

In `src/events.h`:

```c
/* True once the host asked the game to quit (Cmd-Q, closing the window, a
   script's quit): the quit Apple Event has been sent. */
bool events_quit_requested(void);
```

In `src/events.c`, after `events_request_quit`:

```c
bool events_quit_requested(void) { return E.quit_deadline > 0; }
```

In `src/misc.h`:

```c
/* Called by ExitToShell before the process exits; it may not return (main
   uses it to go back to the picker). */
void misc_set_exit_hook(void (*fn)(void));
```

In `src/misc.c`, add `void (*exit_hook)(void);` to `M`, and:

```c
void misc_set_exit_hook(void (*fn)(void)) { M.exit_hook = fn; }

static void h_exit_to_shell(void) {
    log_msg("ExitToShell");
    if (M.exit_hook)
        M.exit_hook();
    exit(0);
}
```

Check that `misc_init` doesn't clear `M` after `main` sets the hook. If it does `memset(&M, 0, ...)`, set the hook after `misc_init`, as Step 6 does.

- [ ] **Step 4: Display: presenting any RGBA frame, and full screen**

In `src/display.c`, split `display_present` in two. `display_present_rgba` takes the body that uploads and draws a frame:

```c
void display_present_rgba(const uint8_t *rgba, int w, int h) {
    D.frames++;
    if (!D.tried)
        D.sdl_ok = open_window(w, h);
    if (D.sdl_ok) {
        /* (the existing texture, update, clear, render and present code, unchanged) */
    }
}

void display_present(void) {
    int w, h;
    uint8_t *rgba = screen_rgba(&w, &h);
    display_present_rgba(rgba, w, h);
    free(rgba);
}

bool display_fullscreen(void) {
    return D.window && (SDL_GetWindowFlags(D.window) & SDL_WINDOW_FULLSCREEN) != 0;
}
```

In `open_window`, open full screen when asked:

```c
    const char *fs = getenv("LOONY_FULLSCREEN");
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | (fs && strcmp(fs, "1") == 0 ? SDL_WINDOW_FULLSCREEN : 0);
    D.window = SDL_CreateWindow(D.title ? D.title : "LittleWing", w * scale, h * scale, flags);
```

(Add `#include <string.h>`.) Declare both new functions in `src/display.h`:

```c
/* Draws an RGBA frame (w x h, rows top to bottom), opening the window on the first one. */
void display_present_rgba(const uint8_t *rgba, int w, int h);

/* Whether the window is full screen. LOONY_FULLSCREEN=1 opens it full screen. */
bool display_fullscreen(void);
```

- [ ] **Step 5: `picker_run`**

Append to `src/picker.h`:

```c
/* Shows the picker for these installed games (2 to PICKER_MAX) and returns
   the one chosen. Remembers it in <files_data_root>/picker.plist ("last
   game") and starts on the one remembered. Cmd-Q or closing the window
   exits the process. For tests: LOONY_PICK=<id|quit>[,...] acts on its
   first item after the first frame and passes the rest on (as
   LOONY_PICK); LOONY_PICKER_SHOT=<png> writes that first frame. */
const game_info *picker_run(const game_info *const *games, int n);
```

Append to `src/picker.c` (add `#include <SDL3/SDL.h>`, `"display.h"`, `"files.h"`, `"plist.h"`, `"png.h"`):

```c
static struct {
    int n, selected;
    bool choose, quit, dirty;
} P;

static void on_key(int scancode, bool down, bool repeat) {
    (void)repeat;
    if (!down)
        return;
    int before = P.selected;
    P.selected = picker_key(P.n, P.selected, scancode, &P.choose);
    P.dirty |= P.selected != before;
}

static void on_mouse(int x, int y, bool down) {
    int i = picker_hit(P.n, x, y);
    if (!down || i < 0)
        return;
    P.selected = i;
    P.choose = true;
}

static void on_quit(void) { P.quit = true; }

static void last_path(char *out, size_t cap, bool *ok) {
    char root[PATH_MAX];
    *ok = files_data_root(root, sizeof root);
    if (*ok)
        snprintf(out, cap, "%s/" GAME_PICKER_FILE, root);
}

static void load_last(char *id, size_t cap) {
    id[0] = '\0';
    char path[PATH_MAX], err[256];
    bool ok;
    last_path(path, sizeof path, &ok);
    plist_entry *e;
    uint32_t n;
    if (!ok || plist_read(path, &e, &n, err, sizeof err) != PLIST_OK)
        return;
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(e[i].key, "last game") == 0 && !e[i].is_number)
            snprintf(id, cap, "%s", e[i].str);
    plist_free(e, n);
}

static void save_last(const char *id) {
    char path[PATH_MAX], err[256];
    bool ok;
    last_path(path, sizeof path, &ok);
    plist_entry e = {.key = "last game", .str = (char *)id};
    if (ok && !plist_write(path, &e, 1, err, sizeof err))
        log_msg("picker: can't remember the last game: %s", err);
}

/* The next LOONY_PICK item into item ("" if none), passing the rest on. */
static void next_scripted_pick(char *item, size_t cap) {
    item[0] = '\0';
    const char *s = getenv("LOONY_PICK");
    if (!s || !*s)
        return;
    const char *comma = strchr(s, ',');
    snprintf(item, cap, "%.*s", (int)(comma ? (size_t)(comma - s) : strlen(s)), s);
    if (comma)
        setenv("LOONY_PICK", comma + 1, 1);
    else
        unsetenv("LOONY_PICK");
}

const game_info *picker_run(const game_info *const *games, int n) {
    if (n > PICKER_MAX)
        n = PICKER_MAX;
    picker_entry e[PICKER_MAX];
    for (int i = 0; i < n; i++) {
        e[i].game = games[i];
        e[i].art = malloc(PICKER_ART_W * PICKER_ART_H * 4);
        char dir[PATH_MAX], exe[PATH_MAX + 64], err[512];
        game_folder(games[i], dir, sizeof dir);
        snprintf(exe, sizeof exe, "%s/%s", dir, games[i]->exe);
        if (!e[i].art || !picker_load_art(exe, e[i].art, err, sizeof err)) {
            log_msg("picker: %s", e[i].art ? err : "out of memory");
            free(e[i].art);
            e[i].art = NULL;
        }
    }
    char last[64];
    load_last(last, sizeof last);
    memset(&P, 0, sizeof P);
    P.n = n;
    P.selected = picker_initial(e, n, last);
    P.dirty = true;
    static const display_input input = {on_key, NULL, on_quit, on_mouse, NULL};
    display_set_input(&input);
    display_set_title("LittleWing");
    static uint8_t screen[PICKER_W * PICKER_H * 4], rgba[PICKER_W * PICKER_H * 4];
    char scripted[64] = "";
    bool first = true;
    while (!P.choose && !P.quit) {
        if (P.dirty) {
            picker_draw(e, n, P.selected, screen);
            qd_pixels s = {screen, PICKER_W * 4, {0, 0, PICKER_H, PICKER_W}, 32, NULL};
            qd_to_rgba(&s, rgba);
            P.dirty = false;
        }
        display_present_rgba(rgba, PICKER_W, PICKER_H); /* waits for the display's refresh */
        if (first) {
            first = false;
            const char *shot = getenv("LOONY_PICKER_SHOT");
            if (shot && *shot && !png_write_rgba(shot, rgba, PICKER_W, PICKER_H))
                log_msg("picker: can't write %s", shot);
            next_scripted_pick(scripted, sizeof scripted);
            if (strcmp(scripted, "quit") == 0)
                P.quit = true;
            for (int i = 0; i < n; i++)
                if (strcmp(scripted, e[i].game->id) == 0) {
                    P.selected = i;
                    P.choose = true;
                }
        }
        display_poll();
        SDL_Delay(1); /* without vsync (the dummy driver), don't spin */
    }
    for (int i = 0; i < n; i++)
        free(e[i].art);
    if (P.quit) {
        log_msg("picker: quit");
        exit(0);
    }
    log_msg("picker: %s", e[P.selected].game->id);
    save_last(e[P.selected].game->id);
    return e[P.selected].game;
}
```

(`picker_run` doesn't free its art before exiting on quit; the process ends right away.)

- [ ] **Step 6: `main`: the picker, the return, the inherited log**

In `src/main.c` (add `#include <mach-o/dyld.h>`, `#include <unistd.h>` if missing, `"picker.h"`, `"events.h"`, `"misc.h"`, `"display.h"`):

1. Make `log_to_file_if_app` keep a log handed down by `back_to_picker`. Right after the `snprintf(prev, ...)` line, insert:

```c
    util_set_failure_hook(show_failure);
    const char *inherited = getenv("LOONY_LOG_INHERITED");
    if (inherited && strcmp(inherited, "1") == 0) /* restarted for the picker: stderr is already the log */
        return;
```

and remove the later `util_set_failure_hook(show_failure);` line, which this replaces. Update the comment above `log_path`: "...after a restart for the picker, the same log carries on."

2. Add, above `main`:

```c
/* Set when the picker chose the game: its own quit goes back to the picker. */
static bool return_to_picker;

/* The game ended by itself (ExitToShell, or its main returned). After a
   pick, and unless the host asked to quit or something failed, the
   preferences are saved and the process restarts itself to show the picker
   again: the emulator's state can't be reset in place. */
static void back_to_picker(void) {
    if (!return_to_picker || events_quit_requested() || util_failed())
        return;
    cf_save_prefs();
    char self[PATH_MAX];
    uint32_t size = sizeof self;
    if (_NSGetExecutablePath(self, &size) != 0) {
        log_msg("can't find this program to go back to the picker");
        return;
    }
    setenv("LOONY_FULLSCREEN", display_fullscreen() ? "1" : "0", 1);
    if (log_path[0])
        setenv("LOONY_LOG_INHERITED", "1", 1);
    log_msg("back to the picker");
    fflush(stderr);
    execv(self, (char *const[]){self, NULL});
    log_msg("can't restart for the picker: %s", strerror(errno));
}
```

3. In the no-folder branch, replace `game = installed[0]; /* Task 5: ... */` with:

```c
        if (n == 1) {
            game = installed[0];
        } else {
            game = picker_run(installed, n);
            return_to_picker = true;
        }
```

4. After `misc_init();` add `misc_set_exit_hook(back_to_picker);`.
5. After `guest_call(img.main_tvector, 0, NULL);` and before `log_msg("main returned");`, add `back_to_picker();`.
6. `display_set_input(&input)` already comes after the picker in `main`, so the game takes the input back from the picker.

- [ ] **Step 7: Run the tests**

Run: `cmake --build build && ./build/loony_tests picker`
Expected: everything passes except `run_the_picker_frame_shows_both_games`, which fails only at its final `CHECK_STR(got, "")`. That hash is recorded in Task 8, after the user approves the picker. Until then, leave the expected string empty: the test is red only on that line.

Write the frame out to look at it:
`LOONY_APPS_DIR=/Applications LOONY_PICK=quit LOONY_PICKER_SHOT=/tmp/picker.png ./build-release/loony` (with `LOONY_DATA_DIR` set to a temporary folder). Open `/tmp/picker.png`. It should match the Task 4 layout and show both title pictures.

Then the whole suite: `./build/loony_tests 2>&1 | tail -1`
Expected: `1 failed` (only the picker frame awaiting its hash).

- [ ] **Step 8: Commit**

```bash
git add src/picker.h src/picker.c src/display.h src/display.c src/events.h src/events.c src/misc.h src/misc.c src/main.c tests/test_run.c
git commit -m "Show the picker with two games installed; a game's own quit goes back to it"
```

---

### Task 6: LittleWing.app

**Files:**
- Modify: `tools/make_app.sh`, `CMakeLists.txt` (the `app` target's comment)
- Test: `tests/test_run.c` (`run_the_app_bundle_is_self_contained_and_plays`)

**Interfaces:**
- Consumes: Task 5's `main` (no folder means the picker).
- Produces: `build-release/LittleWing.app`, with the bundle id `local.loony-shim` (unchanged), `CFBundleName` and `CFBundleDisplayName` `LittleWing`, and the executable `Contents/MacOS/loony`.

The icon stays `tools/AppIcon.png`, Loony's art, until the user supplies LittleWing art. Task 8 asks them.

- [ ] **Step 1: Update the bundle test**

In `run_the_app_bundle_is_self_contained_and_plays`, replace every `Loony Labyrinth.app` with `LittleWing.app` (the `bundle_bin` path, the `codesign` command and the icon path). After the icon check, add:

```c
    snprintf(cmd, sizeof cmd, "plutil -extract CFBundleName raw '%s/LittleWing.app/Contents/Info.plist'", out_dir);
    p = popen(cmd, "r");
    CHECK(p != NULL);
    n = fread(text, 1, sizeof text - 1, p);
    text[n] = '\0';
    pclose(p);
    CHECK_STR(text, "LittleWing\n");
```

The run still passes `test_game_dir()` as the folder, so it plays Loony and checks the approved menu frame `0xADE78151`.

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build build-release && ./build-release/loony_tests self_contained`
Expected: FAIL: `LittleWing.app` doesn't exist.

- [ ] **Step 3: Rename the bundle**

In `tools/make_app.sh`:
- the first comment line becomes `# Builds LittleWing.app around a loony binary: it plays the LittleWing games installed in /Applications, with a picker when there are two.` The rest of the comment stays.
- `app="$out/Loony Labyrinth.app"` becomes `app="$out/LittleWing.app"`.
- In the Info.plist, `CFBundleName` and `CFBundleDisplayName` become `LittleWing`. `CFBundleIdentifier` stays `local.loony-shim`.

In `CMakeLists.txt`, the `app` target's comment becomes `"Building LittleWing.app"` and the comment above it `# The double-clickable app: cmake --build build-release --target app`.

- [ ] **Step 4: Run the tests and build the app**

Run: `cmake --build build-release && ./build-release/loony_tests app && cmake --build build-release --target app`
Expected: the `app` tests pass, and it prints `built .../build-release/LittleWing.app`. Also run `./build/loony_tests app` (Debug, the sanitizer branch). Expected: it passes.

- [ ] **Step 5: Commit**

```bash
git add tools/make_app.sh CMakeLists.txt tests/test_run.c
git commit -m "The app is LittleWing.app"
```

---

### Task 7: README and spec

**Files:**
- Modify: `README.md`, `docs/superpowers/specs/2026-09-30-loony-shim-design.md` (around line 209, "Two folders", and line 278, "The app")

- [ ] **Step 1: README**

Make these edits in the README's current voice:
- **Opening:** "Runs two 2003 PowerPC Mac pinball games by LittleWing, *Loony Labyrinth 3.0.1* and *Crystal Caliburn 3.0.1*, natively on Apple Silicon...". Keep "This repo contains no game files".
- **Build:** `# build-release/LittleWing.app`.
- **Play:**
  - The games go in `/Applications/Loony Labyrinth` (holding `LOONY LABYRINTH 3.0.1`) and `/Applications/Crystal Caliburn` (holding `CRYSTAL CALIBURN 3.0.1`), each from LittleWing's download page. Name Crystal Caliburn's disk image only once it is confirmed on the page.
  - With both installed, the app opens on a picker: Left and Right (or the mouse) choose, Return plays, and it starts on the last game played.
  - With one installed, it plays that game.
  - Choosing QUIT in a game's menu returns to the picker. Cmd-Q or closing the window quits.
- **From a terminal:** `./build-release/loony` behaves like the app. `./build-release/loony <folder>` plays the game in that folder (no picker), and `LOONY_GAME=crystal-caliburn ./build-release/loony` plays it from its usual folder.
- **Keys:** both games use the same defaults.
- **Preferences:** each game's are in `~/Library/Application Support/loony-shim/<game>/prefs.plist` (`loony-labyrinth` or `crystal-caliburn`). Saves from before (`loony-shim/prefs.plist`) move into `loony-labyrinth/` on the next launch. The picker's last choice is `loony-shim/picker.plist`.
- **Giving it to someone:** the app is `LittleWing.app`.
- **`docs/test_key.txt`:** is for Loony Labyrinth.

- [ ] **Step 2: Spec**

- **"Two folders" line:** the writable folder is `~/Library/Application Support/loony-shim/<game id>/`; the pre-Plan 8 files move into it for Loony Labyrinth.
- **"The app" paragraph:** the bundle is `LittleWing.app` (bundle id unchanged). It shows the picker (`src/picker.c`) when two games are installed, and returns to it by restarting itself when a game quits from its own menu. End the paragraph with "(Revised during Plan 8.)"
- **New paragraph after the milestone table:** "Plan 8 added Crystal Caliburn 3.0.1, which runs on the same engine with the same 132 imports (`src/game.c` lists the games), and a picker showing each game's title picture (PICT 800) from the user's copy."

- [ ] **Step 3: Check**

Run each command block in the README's Build and Play sections, apart from signing and notarizing.

Run: `grep -n "Loony Labyrinth.app\|loony-shim/prefs.plist" README.md docs/superpowers/specs/2026-09-30-loony-shim-design.md`
Expected: only lines that describe the old save location.

- [ ] **Step 4: Commit**

```bash
git add README.md docs/superpowers/specs/2026-09-30-loony-shim-design.md
git commit -m "README and spec: LittleWing.app, two games and the picker"
```

---

### Task 8: An hour of Crystal Caliburn, the user's playtest, and the recordings

**Files:**
- Test: `tests/test_run.c`:
  - `run_three_minutes_of_play_match_the_recording` is refactored, and a Crystal Caliburn twin is added;
  - `run_the_picker_frame_shows_both_games` gets its hash.

- [ ] **Step 1: The fixed-clock hour**

```bash
S=$(mktemp -d)
python3 tools/soak_script.py 216000 "$S/cc" > "$S/soak.txt"
LOONY_FIXED_CLOCK=1 LOONY_SCRIPT="$S/soak.txt" LOONY_DATA_DIR="$S/data" LOONY_AUTO_ALERTS=1 \
  SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy \
  /usr/bin/time -l ./build-release/loony "/Applications/Crystal Caliburn" 2>&1 | tail -25
```

Expected:
- exit 0, with `ExitToShell` at the end;
- no `unknown selector`, `not supported` or `fatal`;
- a maximum resident set size near Loony's (about 118 MB).

Open early, middle and late screenshots (`$S/cc*.png`); each should show play or the menu, without garbage. If anything crashes, use superpowers:systematic-debugging. The fix goes into the shim with a unit test, and then the whole suite must pass.

- [ ] **Step 2: The user's playtest (blocking)**

Ask the user to:
1. Copy `build-release/LittleWing.app` to `/Applications` and open it.
2. Check the picker:
   - both title pictures;
   - Left and Right, clicking, and Return;
   - Cmd-F full screen.
3. Play **Loony Labyrinth** and confirm it's still registered and its high scores are there (their real `prefs.plist` moved). If they aren't, stop and tell the user before doing anything else. Don't restore the backup (Facts measured) or debug the migration until they say how to proceed.
4. Choose QUIT from its menu. That should return to the picker, still full screen if it was, and start on Loony.
5. Play **Crystal Caliburn**: sound, flippers (Z, /), plunger (Return), nudge (Space), and high scores kept across a return to the picker. Register it if they have a key code.
6. Quit from the picker with Cmd-Q, and from inside a game by closing the window.
7. Say whether they want LittleWing art for the icon; it uses Loony's for now.

Record their words for the commit in Step 5. Record no hashes before they approve.

- [ ] **Step 3: Record the picker frame**

Run: `./build-release/loony_tests picker_frame_shows`
Expected: fails with `got == "" ("xxxxxxxx" != "")`. Paste in the hash, and replace the comment with `/* approved by the user on <date> */`. Then run it in Debug too; it must match. The PICT decoder and the reduction are integer-only.

- [ ] **Step 4: Share the regression run and add Crystal Caliburn's**

In `tests/test_run.c`, move the body of `run_three_minutes_of_play_match_the_recording` (from setting up `shots` through the WAV hash) into:

```c
/* Plays the regression script on the fixed clock in the game folder dir.
   h gets the three frames' hashes, *wav_hash and *wav_len the recording's.
   Returns the exit status; out gets stderr. */
static int play_regression(const char *dir, uint32_t h[3], uint32_t *wav_hash, size_t *wav_len, char *out,
                           size_t outlen) {
    char shots[3][1024];
    for (int i = 0; i < 3; i++)
        tmp_name(shots[i], sizeof shots[i], "shot");
    tmp_name(script_path, sizeof script_path, "script");
    write_regression_script(script_path, shots);
    tmp_name(wav_path, sizeof wav_path, "wav");
    test_tmp_dir(run_data, sizeof run_data);
    char ticks[16];
    snprintf(ticks, sizeof ticks, "%d", REGRESSION_TICKS);
    setenv("LOONY_EXIT_AFTER", ticks, 1);
    int status = test_run_child(run_loony_scripted, (void *)dir, out, outlen);
    unsetenv("LOONY_EXIT_AFTER");
    test_remove_tree(run_data);
    run_data[0] = '\0';
    unlink(script_path);
    for (int i = 0; i < 3; i++) {
        size_t len = 0;
        uint8_t *png = read_file(shots[i], &len);
        h[i] = png ? fnv1a32(png, len) : 0;
        free(png);
        unlink(shots[i]);
    }
    uint8_t *wav = read_file(wav_path, wav_len);
    *wav_hash = wav ? fnv1a32(wav, *wav_len) : 0;
    if (!wav)
        *wav_len = 0;
    free(wav);
    unlink(wav_path);
    wav_path[0] = '\0';
    return status;
}

TEST(run_three_minutes_of_play_match_the_recording) {
    SKIP_UNLESS_GAME();
    uint32_t h[3], wh;
    size_t len;
    char out[32768];
    int status = play_regression(test_game_dir(), h, &wh, &len, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "runtime error"));
    CHECK_EQ(len, 44 + (size_t)REGRESSION_TICKS * 44100 / 60 * 4);
    CHECK_EQ(h[0], 0xAAD1E97Fu); /* minute 1: ball 1 in play */
    CHECK_EQ(h[1], 0x015482C8u); /* minute 2: ball 3, 13 seconds of demo time left */
    CHECK_EQ(h[2], 0x66E6FBF1u); /* minute 3: the time ran out; a new game, ball 1 */
    CHECK_EQ(wh, 0x3663C0FEu);
}

/* The same three minutes in Crystal Caliburn (the same keys). Recorded
   after the user's Plan 8 playtest. */
TEST(run_three_minutes_of_crystal_caliburn_match_the_recording) {
    SKIP_UNLESS_CC();
    uint32_t h[3], wh;
    size_t len;
    char out[32768];
    int status = play_regression(test_cc_dir(), h, &wh, &len, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "runtime error"));
    CHECK_EQ(len, 44 + (size_t)REGRESSION_TICKS * 44100 / 60 * 4);
    char got[64];
    snprintf(got, sizeof got, "%08x %08x %08x %08x", h[0], h[1], h[2], wh);
    CHECK_STR(got, ""); /* frames at minutes 1, 2, 3, then the recording */
}
```

Run: `cmake --build build-release && ./build-release/loony_tests three_minutes_of_crystal`
Expected: `got == "" ("aaaaaaaa bbbbbbbb cccccccc dddddddd" != "")`. Paste that in, then run it in Release and Debug; it must pass in both. To describe the frames, temporarily comment out `unlink(shots[i])` and run once. Open the PNGs and write one comment per minute, as the Loony test has. Then restore the line.

- [ ] **Step 5: The whole suite, then commit**

Run: `cmake --build build && ./build/loony_tests 2>&1 | tail -1 && ./build-release/loony_tests 2>&1 | tail -1`
Expected: `0 failed` in both.

```bash
git add tests/test_run.c
git commit -m "Plan 8: Crystal Caliburn's hour, the user's playtest, the picker frame and Crystal Caliburn's regression run

<the user's playtest words>"
```

- [ ] **Step 6: Final review**

Run a whole-branch review of Plan 8's commits (superpowers:requesting-code-review, on opus). Fix what it finds, run the suite again, and push to `main`.
