# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

All nine of LittleWing's PowerPC Mac pinball games (Loony Labyrinth, Crystal Caliburn, Jinni Zeala, MONSTER FAIR, Mad Daedalus, Angel Egg, Golden Logres, FAIRY TOWER, Tristan) run on Apple Silicon: the game's own machine code runs in Unicorn, and every Mac OS call it makes is reimplemented in C on SDL3. The repo holds no game files; the games live in `/Applications` on the developer's Mac, and CI has none. The README is the user manual and the reference for every `LOONY_*` environment variable; read its Debugging section before running the game by hand.

## Build and test

```bash
cmake -S . -B build && cmake --build build -j8     # Debug: ASan + UBSan, -Wall -Wextra -Werror
./build/loony_tests                                # all tests, in parallel, about 40 s
./build/loony_tests memmgr                         # tests whose name contains "memmgr"
./build/loony_tests =misc_gestalt_reports_10_4_11_to_a_macho_game   # exactly one
./build/loony_tests -j 1                           # one process, in order (for lldb)
LOONY_SLOW_TESTS=1 ./build/loony_tests             # also the minutes-long play-throughs
./build/loony_tests --slow three_minutes recorded  # only slow tests, any filter matching
./build/loony_tests --list --slow                  # what a command picks, run none
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release --target app
```

Every warning is an error, and the Debug build has sanitizers, so a change that compiles and passes `loony_tests` is the bar. `tools/make_app.sh` refuses a sanitizer build, so the app always comes from `build-release`.

Tests that need a game skip when it isn't installed (`SKIP_UNLESS_GAME`, `SKIP_UNLESS_CC`, `SKIP_UNLESS_MF`, `SKIP_UNLESS_JZ`, `SKIP_UNLESS_MD`, `SKIP_UNLESS_AE`, `SKIP_UNLESS_GL`, `SKIP_UNLESS_FT`, `SKIP_UNLESS_TR`). The tests that register a game type a made-up address and 24-digit key (`TEST_EMAIL`, `TEST_KEY` in `tests/test_run.c`), which the unlock accepts, so no secrets are needed. A clean run here is `N passed, 0 failed, 0 skipped` with `LOONY_SLOW_TESTS=1` and every game installed.

The slow tests (`SLOW_TEST`) are the only end-to-end check of whole games replayed, seeking and snapshots, and together they take many minutes. Before merging, run the ones for what the change touches: put the filters of every row that applies in one `./build/loony_tests --slow <filters>`. `--slow` picks slow tests alone, and a test that several filters match runs once.

| The change touches | Filters |
| --- | --- |
| Emulation, drawing or sound (anything that can move a frame or a sample) | `three_minutes` |
| Recording or replaying, or a new source of host input | `recorded quit_stops_a_take` |
| Seeking, snapshots, the viewer going back, or a `SNAPSHOT_STATE` block | `watched_game with_snapshots` |
| The game-start snapshot or Share | `starts_at_the_game take_keeps_the_snapshot` |
| MONSTER FAIR's startup or `patch.c` | `monster_fair` |
| Mad Daedalus, or `coreaudio.c` | `mad_daedalus fairy_tower` |
| Angel Egg, or the classic Toolbox calls it makes | `angel_egg` |
| Golden Logres, or the classic Toolbox calls it makes | `golden_logres` |
| FAIRY TOWER | `fairy_tower` |
| Tristan, or the System 7 calls it makes | `tristan` |

Before a release, run the whole slow set (`--slow` with no filter). Run slow tests on an idle Mac: they record on the real clock, so under load (another session's tests or build) they fail on timing alone. Rerun a failure on its own before chasing it.

Headless run of a game (never let it open a window or play sound from a session):

```bash
LOONY_FIXED_CLOCK=1 LOONY_AUTO_ALERTS=1 LOONY_DATA_DIR=<empty dir> SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy \
  LOONY_EXIT_AFTER=600 LOONY_SCREENSHOT=shot.png ./build/loony "/Applications/Loony Labyrinth"
```

Add `LOONY_TRACE=imports` to log every OS call, `LOONY_STUB=all` to get past unimplemented ones, and `LOONY_SCRIPT=<file>` for keys, clicks and screenshots at given ticks (format in `src/script.h`). `LOONY_FIXED_CLOCK=1` with an empty `LOONY_DATA_DIR` makes a run reproducible. `timeout` isn't installed on macOS; use `perl -e 'alarm 60; exec @ARGV' <command>`.

## How the shim works

**One loop, one thread.** `main.c` loads the program into guest memory, registers every OS call, then calls the game's `main` with `guest_call`. Nothing else runs: the game's event loop calls back into the shim's event pump (`events_pump`), which polls SDL, and every host service is single-threaded by construction. The one other thread is SDL's audio callback (`feed` in `sound.c`), which pulls rendered samples from the mixer under the stream lock; `mixer.c` is integer-only and touches no guest memory, so a rendered stream is identical in every build.

**Traps.** Guest memory is one host allocation mapped into Unicorn (`guest_mem.h` is the memory map). Import *i* of the program is bound to the unmapped address `GUEST_TRAP_ADDR(i)`, so when the game calls an OS function the CPU stops with that address, `trap.c` runs the C handler registered for the import's name, and `cpu_run` resumes at the link register. A handler reads its arguments with `trap_arg(n)` and answers with `trap_return(v)`; handlers may call back into the game with `guest_call`, which is re-entrant. Each service module exposes `<module>_init()` and `<module>_register()`, the latter a list of `trap_register("Name", h_name)` calls; a name the game doesn't import is ignored, so one module serves every game. Host objects the game holds by reference (CFStrings, windows, events, CG images) are opaque IDs in the tag spaces above `0x08000000`, never dereferenced.

**Two program formats.** The classic games, Jinni Zeala among them, are PEF (Carbon, `pef.c` + `loader.c`, resources from the file's resource fork via `rsrc.c`), laid out at `GM_LAYOUT_PEF` with 8-bit QuickDraw at 800x600. MONSTER FAIR is a Mach-O bundle (`macho.c`, direct code addresses rather than transition vectors, `trap_set_direct_calls`), laid out at `GM_LAYOUT_MACHO`, and needs the C library and C++ runtime (`libc.c`, `cxxrt.c`), nibs (`nib.c`) and CoreGraphics (`cgimage.c`, `cgdisplay.c`) that the Carbon games don't. Each init in the two startup sequences in `main.c` must come where it is: the design doc and `docs/superpowers/plans/2026-10-08-plan9-handoff.md` explain why each init comes where it does. Of MONSTER FAIR, only 1.2.5 runs: `patch.c` edits two instructions of its second license check on every launch and refuses any other version. Mad Daedalus is a Mach-O bundle too, laid out the same way; it plays its sound through CoreAudio (`coreaudio.c`: an AUGraph mixer whose render callbacks are game code, run on the main thread as `sound.c`'s clock-driven source) rather than the Sound Manager, maps its data files with `mmap` (`libc.c`), and gets a 1024x768 display (`game.c`'s screen size), since it plays at the display's size. Angel Egg is a Mach-O bundle built with CodeWarrior on PowerPlant: a classic Toolbox program (Memory Manager, regions, windows, `menumgr.c`, Apple Events in `ae.c`) whose resources are a data-fork file, `Contents/Resources/Angel Egg.rsrc`. Its main is found past MSL's linked-in exit, and its GWorld rows are padded as System 7.1's were (`qd_pad_gworld_rows`), since its table file stores images at those strides. Golden Logres is another CodeWarrior and PowerPlant bundle on the same Toolbox calls, its resources in `Contents/Resources/Golden Logres.rsrc`; it takes the display at 1024x768 and 16 bits. FAIRY TOWER is a Mach-O bundle built as Mad Daedalus is: CoreAudio, whose output it asks for as float samples, a 1024x768 display, and the same menu keys. Tristan is older than Carbon: a PEF that imports InterfaceLib, with a 640x480 display. It reads Toolbox structures directly rather than through accessors, so windows, ports and MenuRefs live in its memory as real records and handles, and InitGraf hands the shim its own QuickDraw globals, which `qd.c` keeps in step (`thePort`, `screenBits`, `randSeed`). It saves into its own resource fork on every launch: `rsrc_app_file` points the application's resources at a copy in the data folder, written there and read from then on. Its sound callback is a classic routine descriptor, which `guest_call` resolves to its PowerPC routine, and it takes no Apple Events, so a host quit reaches it as Cmd-Q, its File menu's Quit (`events.c`).

**Unlock.** No key can be bought any more, so every build drops each game's license verdict: `unlock.c`'s `local_patch_hook` runs after a game's image loads and rewrites the words listed in `games.inc` (one list per game; the file's header comment is the method, the README has each game's path to the verdict). It logs `UNLOCK ACTIVE for <id>`, or `nothing patched` if the code isn't the expected version. Any e-mail and a made-up key of 24 or more digits register; the games' own length tests stay. A new game needs its own list, found with the `unlock-game` skill.

**Determinism and replays.** Every place the host can change what the game sees (a clock read, the event pump's poll, a sound status check) is a *step*, and the emulator is deterministic between steps. That is what `replay.c` records and plays back, with a screen hash every 30 presents to catch a desync. Anything that adds a new source of host input to the game must go through the recorder, or replays of old recordings break. Replays, clips (`clip.c`, `mp4.m`), the viewer (`watch.c`, `export.c`) and the Replays and High Scores menus are Loony Labyrinth only; `play.c` reads that game's own flags at fixed code offsets to know when a game starts and ends. The viewer seeks by putting back snapshots of the whole emulator (`snapshot.c`, `seek.c`), so a module's file-scope state that the game can see must be registered with `SNAPSHOT_STATE` after its definition. If it holds pointers or host-only fields, register it with `SNAPSHOT_STATE_WALK` and a walk that lists what is the game's and runs both ways (`SNAPSHOT_OWNED` for heap objects it owns, `SNAPSHOT_HOST` for fields a restore must keep); otherwise a seek desyncs or hands back freed pointers. `test_snapshot.c` round-trips every plain block on every push, and `LOONY_SNAPSHOT_VERIFY=1` compares each snapshot with the emulator wherever the replay comes to it again, naming the block that differs. A recording also keeps a snapshot of the game's start, written by its first replay and put back by every later one in another process, maybe on another Mac (`gamestart.c`, format 6 in `replay.c`), so a block must hold nothing that means something only in the process that wrote it: a pointer, a path, a temporary folder's name. Make such fields `SNAPSHOT_HOST`, or walk them as an index; the license is kept out by `cf_keep_out_of_snapshots`. Shared recordings carry their snapshot, so a block can come from a file someone else wrote: a walk checks every index, count and path it loads with `snapshot_check` before anything uses it, and a block with such fields needs a walk even if it holds no pointer. That snapshot is keyed to the snapshot layout (every block's name and size, found by itself) and to `GAMESTART_SNAPSHOT_VERSION` in `gamestart.h`, not to the build, so it survives updates: bump the version for a change a snapshot would show that keeps every block's size (a walk saving other fields, a struct a block owns through `SNAPSHOT_OWNED` changing size, a field's meaning, or what the shim writes into guest memory before the game starts). A forgotten bump costs a slow open, since a replay that desyncs, or can't load the module state, after starting from the snapshot drops it and starts again from launch, logging why. The two `docs/littlewing-shim-steal-*.md` reports are the research behind the replay work (snapshot seeking, viewer UI).

**HD mode** (`hd.c`, `blit.c`) keeps an N-times-size RGBA shadow of every pixel buffer and swaps in replacement art from `hd-art/<game id>/<hash>.png` whenever the game draws a picture with a known hash. The game still draws at 1x; HD only follows. The art ships in the repo and the app, and is on whenever the app has art for the game.

**Objective-C** is confined to `menu.m` (menu bar, Sparkle updates, Finder reveals, share sheet) and `mp4.m` (AVFoundation encoding). Everything else is C11.

Each header opens with a prose comment that is the module's specification; read it before the `.c`. The design doc, `docs/superpowers/specs/2026-09-30-loony-shim-design.md`, has the memory map and the decisions behind the layout; the plans beside it are history, useful for the measurements they record.

## Tests

`tests/test.h` is the framework: `TEST(name)` registers through a constructor (`SLOW_TEST(name)` for one of minutes, which skips unless slow tests are on), `CHECK`, `CHECK_EQ`, `CHECK_STR`, `CHECK_CONTAINS` return from the test on failure. Each test runs in a forked child with its own `LOONY_DATA_DIR`, so tests never share preferences and a crash is reported, not fatal. A test that starts a video encoder needs a real exec'd process: open it with `RUN_UNFORKED()`.

Unit tests for OS calls use `tests/harness.h`: `harness_init(names, n)` builds a fresh machine whose imports trap directly, `call_import("Name", nargs, ...)` makes the call through the real dispatch, and `scratch(n)` hands out guest memory for argument structs. `harness_init_direct` is the Mach-O flavour. `tests/asm.h` and `tests/macho_build.h` assemble small PowerPC programs and Mach-O files for tests that need guest code. The integration tests (`test_game.c`, `test_run.c`, `test_replay.c`) run the real game headless with `LOONY_SCRIPT`.

`MAX_TESTS` in `tests/test_main.c` is the registry's cap; raise it when the count nears it.

## Conventions

- No em dashes anywhere: code comments, commit messages, docs, PR text.
- The original game files are only ever read. Anything a game writes goes under `~/Library/Application Support/littlewing-shim/<game id>/` (or `LOONY_DATA_DIR`), never into its folder.
- Environment variables are how the program is configured, in the app and in tests alike. A new one is documented in the README and read in one place.
- Other sessions switch branches in the main checkout. Pass `--head <branch>` to `gh pr create`, and run each `gh` command as its own Bash call.
- Releases go through the `release` skill (`.claude/skills/release`), which wraps `tools/release.sh`; it only runs from `main` in the main checkout. CI (`.github/workflows/ci.yml`) builds Debug with tests and the Release app on every PR, with no games and no slow tests.
- `tools/disasm.py` (Capstone, `tools/requirements-dev.txt` in a `.venv`) disassembles the classic games' functions by name; crash reports and `play.c` give offsets as `code+0xNNNNN`, which it takes directly.
