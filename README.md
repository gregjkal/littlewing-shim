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
```

## Play

```bash
./build-release/loony                       # uses /Applications/Loony Labyrinth
./build-release/loony "/path/to/game folder"
```

The game starts with its opening and then a self-playing demo. To play:

| Key | Does |
|---|---|
| Esc | Stops the demo; press again for the menu (start a game, options, quit). In a game: pause / menu |
| Return | Choose in the menu; hold and release to pull the plunger |
| Z | Left flipper |
| / | Right flipper |
| Space | Nudge |
| Cmd-F | Full screen on or off |
| Cmd-Q or closing the window | Quit |

The keys can be changed from the game's OPTIONS menu. This is the shareware
version: games are time-limited, and the two startup alerts ("Play Demo" and the
key list) are answered automatically. Registering, saved preferences and high
scores come with milestone 6.

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
```

`LOONY_FIXED_CLOCK=1` makes time advance only when the game waits, so a run is
the same every time and doesn't depend on the host's speed. It plays no sound,
but `LOONY_WAV` still records what would have played, the same samples every
run.

`LOONY_SCRIPT` plays keys and takes screenshots at given ticks (1/60 s since launch), one
action per line:

```
1720 down esc
1724 up esc
1880 screenshot menu.png
2600 quit
```

Key names are those in `src/keymap.c` (`z`, `slash`, `return`, `space`, `esc`,
`lshift`, `rshift`, ...).

The original game files are only ever read, never modified. The emulated screen
is 800x600, the size the game expects.

Design: `docs/superpowers/specs/2026-09-30-loony-shim-design.md`
