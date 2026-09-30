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
```

The original game files are only ever read, never modified.

Design: `docs/superpowers/specs/2026-09-30-loony-shim-design.md`
