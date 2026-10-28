---
name: add-game
description: Add support for another LittleWing game to the shim, from measuring what it needs to its table row, picker card, tests and docs. Use when asked to support, add or try a new game.
---

Each game so far has run on mostly unchanged services, so **measure** before you design: most of the work is wiring the game in, and the measurements tell you whether it is more. Run games headless as CLAUDE.md describes, from a branch, with `-j 4` builds and tests. Keep a scratch folder of screenshots and logs: the frames you look at here become the test's golden hashes.

1. **Identify.** Find its folder or `.app` in `/Applications`: a PEF program starts `Joy!peff` (a classic game: a folder holding the program and its resource fork), a Mach-O one is a bundle like MONSTER FAIR. Note its version from the file name or `vers` resource. Done when you know the kind, the folder, the program's path in it, and the version.

2. **Row.** Append it to the table in `src/game.c` (id, title, folder, program, kind). Appending keeps every other game's index, which tests rely on. Done when `./build/loony "<folder>"` says `playing <title> from`.

3. **Calls.** For a PEF game, diff its imports with a supported game's: `python3 -I .claude/skills/add-game/imports.py "<program>"`, once for each, then `comm -3`. Grep `src/` for `trap_register("<name>"` for each import only the new game has. Then run it headless for 600 ticks with `LOONY_TRACE=imports` and a screenshot. Done when every import is registered, the log has no unimplemented call, and the screenshot is the game's attract screen drawn right. A missing call is real work: implement it in its module, with a unit test through `tests/harness.h`, before going on.

4. **Start.** Find the keys that start a one-player game. Menus differ: Loony Labyrinth and Crystal Caliburn need Esc to end the demo before Esc opens the menu; Jinni Zeala opens it on the first Esc; MONSTER FAIR waits until its table plays. Script a key at a time with a screenshot after each. Done when a screenshot shows ball 1 in play.

5. **Play.** Write a `regression_start` for it in `tests/test_run.c` (beside `classic_start`, `jz_start` and `mf_start`), and run the three-minute script by hand with `LOONY_WAV`. Done when the minute 1 and 2 frames show a game in play, minute 3 shows what the demo's limit or the game's end looks like, and the WAV has sound in every minute.

6. **Quit.** Script the game's own menu to its QUIT: the log must say `ExitToShell` with no quit Apple Event, which is how the app returns to the picker. Then a script `quit` (Cmd-Q's path): `sending the quit Apple Event`. Done when both end with status 0.

7. **Card.** The picker shows a classic game's PICT 800 (512x384); `picker_load_art` falls back to its resource fork's `icns 128` icon, and a bundle game's card is `Contents/Resources/appl.png`. List its `PICT` and `icns` resources to see which applies; a game with neither needs a new card source. If the game count passes `PICKER_MAX`, the picker needs a layout for one more (`card`, `name_top`, `columns` in `src/picker.c`). Done when a run with every game symlinked into a `LOONY_APPS_DIR` folder (`src/game.h`), with `LOONY_PICK=quit` and `LOONY_PICKER_SHOT` (`src/picker.h`), writes a picker frame you have looked at that shows the new card.

8. **Tests.** Mirror the tests of the most similar game: a `test_<x>_dir` and `SKIP_UNLESS_<X>` in `tests/test.h` and `tests/test_main.c` (with a `LOONY_<X>_DIR` override); the table test in `tests/test_game.c`; the opening, the menu quit, the picker with every game, and the slow three-minute run (a `SLOW_TEST`) in `tests/test_run.c`; a card test in `tests/test_picker.c`. Fill in each golden hash from the failing run, after checking the hash matches the frame you looked at. Done when `./build/loony_tests --slow -j 4 <id words>` and the whole fast suite both pass.

9. **Docs.** Update every place that lists the games: the README (intro, the table of folders, the picker paragraph, `LOONY_GAME`'s and `LOONY_DATA_DIR`'s ids, the key table if its keys differ, HD art's ids, the test skip macros and folder variables), CLAUDE.md, and the header comment in `src/game.h`. Find them with `grep -rn -i "<an existing game's title or id>" README.md CLAUDE.md src`. Done when each list names the new game.

10. **Hand off.** Build the Release app and open it for the user to playtest. Tell them which golden frames you recorded, since past frames were approved by the user, and show the picker frame. Replays, HD and High Scores stay Loony Labyrinth only (`play.c` and `main.c` check its id) unless the user asks otherwise. Registering the game needs an unlock: add its `code_patch` list and `GAME(...)` row to `src/games.inc` with the `unlock-game` skill, then register in the tests with `TEST_EMAIL` and `TEST_KEY` (`tests/test_run.c`). Find where the game saved that license (`user email` and `user id` in the data folder's `prefs.plist`, or a file of its own there: note its size and where the key code starts) and fill in the license fields of its `game.c` row, so it starts registered (`src/license.h`); add it to `license_is_saved_in_a_games_own_file` in `tests/test_license.c` if it keeps a file. Done when a run without `LOONY_UNREGISTERED`, with real alerts, logs `license: registered in` and shows no shareware window. Until then, say the game is demo-only.

## Traps

- Script lines must be in tick order: `sort -s -n -k1,1` a script you generated.
- zsh treats a leading `=` as a command lookup, so quote an exact-name test filter: `./build/loony_tests '=name'`.
- Shareware alerts: `LOONY_AUTO_ALERTS=1` answers each with its default button (Play Demo); without it, script clicks on the buttons from a screenshot of the alert.
