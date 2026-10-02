# loony-shim

Runs the 2003 PowerPC Mac game *Loony Labyrinth 3.0.1* natively on Apple Silicon
by emulating its CPU (Unicorn) and reimplementing the Mac OS calls it makes in C.
Personal use only. This repo contains no game files; point it at your own copy.

## Build

```bash
brew install unicorn sdl3 cmake pkg-config
cmake -S . -B build                                     # Debug (sanitizers): for development
cmake --build build
./build/loony_tests          # all tests; ./build/loony_tests <substring> to filter

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release  # for playing
cmake --build build-release
cmake --build build-release --target app                # build-release/Loony Labyrinth.app
```

## Play

Copy `build-release/Loony Labyrinth.app` to `/Applications` (or anywhere) and
double-click it. It plays the game in `/Applications/Loony Labyrinth`, and
carries its own copies of Unicorn and SDL3, so Homebrew upgrades don't affect
it. It is signed ad hoc for this Mac only; if macOS refuses to open it after
copying it from elsewhere, right-click it and choose Open. When the app can't
start or the game crashes, it says so in a message box; its log is
`~/Library/Logs/loony-shim/loony.log` (the run before is kept as
`loony.previous.log`).

From a terminal:

```bash
./build-release/loony                       # uses /Applications/Loony Labyrinth
./build-release/loony "/path/to/game folder"
```

Until it is registered, the game first shows two alerts: the shareware screen
(Play Demo, Buy Now, Enter Key-Code, Quit) and the key list. Click a button, or
press Return for the outlined one. To register, click **Enter Key-Code**, type
or paste (Cmd-V) your e-mail address and key code, using Tab to move between
the fields, and click **Register** (or press Return). The license is saved with
the preferences, so later launches go straight to the game.

The game then plays its opening and a self-playing demo. To play:

| Key | Does |
|---|---|
| Esc | Stops the demo; press again for the menu (start a game, options, quit). In a game: pause / menu |
| Return | Choose in the menu; hold and release to pull the plunger |
| Z | Left flipper |
| / | Right flipper |
| Space | Nudge |
| Cmd-F | Full screen on or off |
| Cmd-Q or closing the window | Quit |

The keys can be changed from the game's OPTIONS menu. Unregistered, games are
time-limited.

The game's preferences (options, keys, the high-score table and the license)
are saved when it quits (and at any exit but a crash), in `~/Library/Application Support/loony-shim/prefs.plist`.
Any file the game writes goes to the same folder, never into the game folder.
Delete the folder to start over.

## Debugging

```bash
LOONY_TRACE=imports ./build/loony     # log every OS call
LOONY_TRACE=imports,calls ./build/loony   # also log each call into the game (callbacks)
LOONY_TRACE=lowmem ./build/loony      # log the first write to each low-memory address
LOONY_STUB=all ./build/loony          # unimplemented OS calls return 0 instead of crashing
LOONY_EXIT_AFTER=600 LOONY_SCREENSHOT=shot.png ./build/loony   # run 10 s, save the last frame
SDL_VIDEO_DRIVER=dummy ./build/loony  # no window (with LOONY_SCREENSHOT for headless runs)
LOONY_WAV=out.wav ./build/loony       # also record the sound (44.1 kHz 16-bit stereo)
SDL_AUDIO_DRIVER=dummy ./build/loony  # no sound output
LOONY_FIXED_CLOCK=1 LOONY_SCRIPT=play.txt SDL_VIDEO_DRIVER=dummy ./build/loony
LOONY_AUTO_ALERTS=1 ./build/loony     # answer alerts with their default button, without showing them
LOONY_DATA_DIR=/tmp/fresh ./build/loony   # use another folder for preferences and saved files
```

`LOONY_FIXED_CLOCK=1` makes time advance only when the game waits, so a run is
the same every time and doesn't depend on the host's speed, as long as it
starts from the same preferences: point `LOONY_DATA_DIR` at an empty folder.
It plays no sound, but `LOONY_WAV` still records what would have played, the
same samples every run.

`LOONY_SCRIPT` plays keys and takes screenshots at given ticks (1/60 s since launch), one
action per line:

```
1720 down esc
1724 up esc
1880 screenshot menu.png
2600 quit
```

Scripts can also click (`20 click 460 270`, in emulated-screen pixels) and type
into a dialog (`50 type me@example.com`, the rest of the line), and may be any
length. `tools/soak_script.py` writes one that keeps playing for an hour (game
starts, plunger, flippers, nudges, a screenshot every 5 minutes):

```bash
python3 tools/soak_script.py 216000 /tmp/soak > /tmp/soak.txt
LOONY_DATA_DIR=$(mktemp -d) LOONY_AUTO_ALERTS=1 LOONY_FIXED_CLOCK=1 LOONY_SCRIPT=/tmp/soak.txt \
  SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy ./build-release/loony
```

Key names are those in `src/keymap.c` (`z`, `slash`, `return`, `space`, `esc`,
`lshift`, `rshift`, ...).

The original game files are only ever read, never modified. The emulated screen
is 800x600, the size the game expects.

Design: `docs/superpowers/specs/2026-09-30-loony-shim-design.md`
