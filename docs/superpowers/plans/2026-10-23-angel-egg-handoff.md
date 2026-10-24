# Angel Egg handoff (2026-10-23)

For the session that picks up adding Angel Egg 3.0.1 (`/add-game Angel Egg`). Read this, then CLAUDE.md and `.claude/skills/add-game/SKILL.md`. The add-game skill's steps 1-3 are under way; steps 4-10 haven't started.

## The game

- `/Applications/Angel Egg.app`, version 3.0.1 ("Angel Egg version 3.0.1" in Info.plist). It is a thin PowerPC Mach-O built with CodeWarrior on Metrowerks PowerPlant, not the engine the other games share: a full classic Toolbox app (windows, menus, TextEdit, AppleEvents).
- Program: `Contents/MacOS/Angel Egg`. Its resources are a resource file in a data fork, `Contents/Resources/Angel Egg.rsrc` (PPob, WIND, MENU, PICT, `snd `, ...). Carbon opens `<program>.rsrc` as the application's file, and so does the shim now (#115).
- Table data: `Contents/MacOS/AngelEgg™ files/AngelEgg™ Machine 16bit 3D` and `... 2.5D`, each a 4 MB custom data fork plus 139 `snd ` resources in a resource fork. Type `RtTb`, creator `fAEg`. The game checks the type with FSpGetFInfo and opens the fork with FSpOpenResFile.
- Fonts it uses (from its `Txtr` resources): Geneva 9, Chicago 12, the system and application fonts.
- Shareware: two alerts at launch (901 Play Demo / Quit / Buy Now / Enter Key-Code, then 900 with the keys: ESC pause, ENTER plunger, Z left flipper, SLASH right flipper, X / PERIOD / SPACE nudge). `LOONY_AUTO_ALERTS=1` answers them. `docs/test_key.txt` has no Angel Egg key; tell the user (skill step 10).

## The work is shared with Golden Logres

Golden Logres 3.0.4 is the same kind of program, and another session is adding it. The user agreed a split (memory: `powerplant-games-shared-work`). Every shared piece is its own PR on `main` (some stacked), and both game branches merge them. Peer sessions show up in `ListAgents`; the current Golden Logres one was `golden-logres-ce`. Message it with `SendMessage`.

| Area | Owner | PR | Branch | Base |
| --- | --- | --- | --- | --- |
| CodeWarrior `find_main` | Golden Logres | #111 | codewarrior-main | main |
| CodeWarrior C runtime | Golden Logres | #112 | codewarrior-runtime | #111 |
| Classic Memory Manager | Golden Logres | #113 | classic-memory | #112 |
| Regions, clipping (`each_port_clip`) | Golden Logres | #114 | qd-regions | #113 |
| Classic event loop, GetKeys, oapp event | Golden Logres | #117 | classic-events | #114 |
| Device list, SetDepth(NULL) | Golden Logres | #119 | qd-devices | #117 |
| Menus (`menumgr.c`, tag 0x0E000000), window list | Golden Logres | #121 | menus-windows | #119 |
| CreateCustomWindow and the rest of windows | Golden Logres | #123 | custom-windows | #121 |
| Resource Manager: open files, chain, writes, `.rsrc`, GetPicture/KillPicture | Angel Egg | #115 | rsrc-files | main |
| File Manager calls, default dir = Contents/MacOS | Angel Egg | #116 | file-calls | main |
| Pen, patterns, shapes (`pen.c`, `qd_draw`) | Angel Egg | #118 | qd-pen | #114 |
| Text through CoreText (`text.c`, `qd_fill_mask`) | Angel Egg | #120 | qd-text | #118 |
| AppleEvent descriptors, AESend, OSL setup (`ae.c`, tag 0x09000000) | Angel Egg | #122 | apple-events | main |
| SndPlay, GetSoundHeaderOffset, SndSoundManagerVersion, Fix2X, Long2Fix | Angel Egg | #124 | snd-play | main |

Still unassigned: Controls (whoever reaches them first, after telling the other session), TextEdit and Scrap (Angel Egg's side, if Angel Egg turns out to reach them).

Agreements with the other session:
- Before running slow tests, ask it to pause builds, tests and game runs, and wait for it to say it has. Tell it when you're done. It does the same for you. Slow tests record on the real clock and fail under load.
- `events.c`'s quit and oapp events should become real Apple Events with `ae_new_apple_event`, `ae_new_null` and `ae_dispose` (ae.h). That's the Golden Logres session's change to make.
- Tag spaces in use: 0x08 CF, 0x09 AE, 0x0A events, 0x0B dialogs, 0x0C CG images, 0x0D CoreAudio, 0x0E menus, 0x0F000001 pthread_self.

## Where Angel Egg stands

The local branch `gregjkal/angel-egg` (not pushed) holds the game.c row and merges of every branch above. The row is 1024x768: with the windows branch, the game sizes its window to 1024x768 at (0,0).

```
{"angel-egg", "Angel Egg", "Angel Egg.app", "Contents/MacOS/Angel Egg", GAME_MACHO_BUNDLE, 1024, 768},
```

The branch has every branch above merged, custom-windows and snd-play included. With them, Angel Egg runs 1200 ticks headless with `LOONY_STUB=all` and reaches no unregistered call: it answers both shareware alerts, draws its title screen centered and correct at 1024x768, and waits in WaitNextEvent. It calls SndPlay, GetSoundHeaderOffset, SndSoundManagerVersion and Fix2X along the way, and all succeed. Skill step 3 is done.

`game_table_knows_five_games` fails on this branch, as it should with a sixth row: the test, the picker and the docs change in the skill's later steps.

To bring the branch up to date: `git fetch`, then merge `origin/gregjkal/<branch>` for custom-windows, qd-text, apple-events, rsrc-files, file-calls and snd-play into `gregjkal/angel-egg`. Known conflicts are additive: `main.c`'s register list (`pen_register`, `text_register` and `menumgr_register` all stay) and `tests/test_snapshot.c`'s `owners` list (`"menumgr.c N"` and `"ae.c A"` both stay).

Run it headless (CLAUDE.md), with an empty `LOONY_DATA_DIR`:

```bash
LOONY_FIXED_CLOCK=1 LOONY_AUTO_ALERTS=1 LOONY_DATA_DIR=<empty dir> SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy \
  LOONY_EXIT_AFTER=1200 LOONY_SCREENSHOT=shot.png LOONY_TRACE=imports ./build/loony "/Applications/Angel Egg.app"
```

Add `LOONY_STUB=all` to list every unregistered call it reaches:
`grep "^loony: stub" log | sed -E 's/.*#[0-9]+ ([A-Za-z0-9_]+)\(.*/\1/' | awk '!s[$0]++'`.

## Next steps

1. Skill step 4: find the keys that start a game (the alerts list them; the menus may have New Game), one key at a time with screenshots.
2. Steps 5 to 10 as the skill says. Measure before writing: Golden Logres turned out to play through its own Button/GetKeys/CopyBits loop, so windows, menus, TextEdit and controls mattered far less than its import list suggested. Angel Egg may be the same.
3. Sound: the table's sounds are `snd ` resources in the machine file. Check the WAV (`LOONY_WAV`) has sound in every minute (skill step 5).

## In flight when this was written

- Open PRs: #111 to #124 (table above). As each lands, merge `main` into `gregjkal/angel-egg`. The shared memory note `powerplant-games-shared-work` lists them.
- `gregjkal/angel-egg` is local only. It holds the row and the merges; push it only when the game is ready for its own PR.

## Things learned

- `nm` and `otool -L` choke on Angel Egg's old load commands (LC_PREBOUND_DYLIB). A small Python Mach-O reader with Capstone worked (`tools/requirements-dev.txt` in a venv). Set `skipdata` so disassembly doesn't stop at data in `__text`.
- CodeWarrior's `_start` calls MSL's own `exit`, which calls the `_exit` stub, so `find_main` needed #111.
- Resource forks can be written on APFS through `<file>/..namedfork/rsrc`. #115 saves changed resource files that way, in the data folder.
- `FSMakeFSSpec` on a folder returns fnfErr in the shim (it only accepts files) but still fills in the spec.
- Text is drawn without antialiasing, a character at a time at whole-pixel advances, and looks like the classic bitmap fonts. How a glyph looks depends on the macOS version, so golden hashes of frames with text are tied to this Mac's fonts.
- A test of a heap-owning snapshot block must add it to `owners` in `tests/test_snapshot.c`, or the plain-block test fills its pointers with a pattern and crashes.
