# loony-shim

Runs the 2003 PowerPC Mac game *Loony Labyrinth 3.0.1* natively on Apple Silicon
by emulating its CPU (Unicorn) and reimplementing the Mac OS calls it makes in C.
Personal use only. This repo contains no game files; point it at your own copy.

## Build

```bash
brew install unicorn cmake pkg-config
cmake -S . -B build
cmake --build build
./build/loony_tests          # all tests; ./build/loony_tests <substring> to filter
```

## Run

```bash
./build/loony                         # uses /Applications/Loony Labyrinth
./build/loony "/path/to/game folder"
LOONY_TRACE=imports ./build/loony     # log every OS call
LOONY_TRACE=imports,calls ./build/loony   # also log each call into the game (callbacks)
LOONY_TRACE=lowmem ./build/loony      # log the first write to each low-memory address
LOONY_STUB=all ./build/loony          # unimplemented OS calls return 0 instead of crashing
```

The original game files are only ever read, never modified. Today the game
runs through its startup (memory, resources, Gestalt, preferences) and stops at
its first dialog, `Alert`, which needs graphics (milestone 3).

Design: `docs/superpowers/specs/2026-09-30-loony-shim-design.md`
