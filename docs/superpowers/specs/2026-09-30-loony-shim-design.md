# loony-shim: running Loony Labyrinth 3.0.1 natively on Apple Silicon

Status: draft for review, 2026-09-30

## Summary (plain English)

Loony Labyrinth is a 2003 Mac pinball game compiled for PowerPC processors. It doesn't run on this Mac. `loony-shim` is a small native program that:

1. loads the original, unmodified game files,
2. runs the game's PowerPC machine code inside a CPU emulator (Unicorn), and
3. answers the game's requests to the old Mac operating system (for example "draw this", "play this sound", "what key is pressed?") with new C code built on SDL3, a modern library for windows, audio and input.

The game makes 132 kinds of system request, and each one gets a C implementation. The game doesn't know it's no longer on a real Mac.

## Goals

- Play Loony Labyrinth 3.0.1 on this Mac (arm64, macOS 26) with correct graphics, sound, controls, scoring and saved preferences and high scores.
- Use the original files in `/Applications/Loony Labyrinth` read-only. Never modify them.
- Show the game in a resizable window, scaled up sharply, with a fullscreen toggle.

## Non-goals

- Other games, other Carbon apps, or a general compatibility layer. Implement only what this binary uses.
- Other platforms (iOS, Windows, Linux).
- Changing the game itself (widescreen, new tables, bug fixes in game logic).
- Distribution. This is for personal use. The repo never contains game files or assets extracted from them.

## Success criteria (checked by the user as playtester)

1. Double-clicking the built app opens the game, and the title screen looks like the original.
2. A full game can be played: flippers, plunger and nudge respond to the keyboard without noticeable lag.
3. The ball physics feels like the original. The user is the judge; an optional side-by-side against Infinite Mac or SheepShaver is available.
4. Sound effects and music play without crackles or drift.
5. Entering a high score, quitting and relaunching keeps the score. Changed preferences also persist.
6. Running for an hour produces no crash.

## Facts about the binary (measured 2026-09-30)

| Item | Value |
|---|---|
| Executable | `LOONY LABYRINTH 3.0.1`, type `APPL`, creator `fLoO`, PEF container, architecture `pwpc` |
| Carbon | yes (`carb` resource), built for CarbonLib on OS 9 and OS X |
| Sections | 0: code, 280,528 bytes (70,132 instructions). 1: pattern-initialized data, 8,374 bytes packed / 22,892 unpacked. 2: loader |
| Imports | 132 symbols. 107 + 23 from two `CarbonLib` entries, 2 from `Apple;Carbon;Multimedia` (`BeginFullScreen`, `EndFullScreen`). One is a data symbol (`kCFPreferencesCurrentApplication`) |
| Floating point | 79 FP instructions in total, so the physics is almost certainly integer or fixed-point |
| AltiVec | 152 words carry opcode 4. We assume these are fast paths chosen after a `Gestalt` check |
| Memory | `SIZE` asks for 16 MB |
| Resources | 57 types in 3.8 MB. Only 7 `PICT` (largest 512×384). 125 `ESnd` and 6 `snd ` (format 2). Three 256-entry `clut` (8-bit color). 6 `DLOG`, 23 `ALRT`, 3 `WIND`. Table data is in custom types (`Flip`, `Kick`, `Hole`, `Bump`, `Visu`, `PfDb`, and others) that the game's own code parses |
| Data file | `LL Data/effect.bin`, 581,632 bytes, type `TEXT`/`LMAN`, read with plain file calls |
| No menus | MENU/MBAR resources exist, but no Menu Manager calls are imported |

Imported API surface: Memory Manager, Resource Manager, QuickDraw (GWorlds, `CopyBits`, `PaintRect`, `DrawPicture`, color), Sound Manager, Carbon Event Manager (handlers, timers, `ReceiveNextEvent`, `RunApplicationEventLoop`), Dialog Manager (`Alert`, `ModalDialog`, item text), File Manager (FSSpec and PB calls), CFPreferences, CFString/CFNumber, Internet Config (`ICLaunchURL`), Apple Events (one handler install), `Gestalt`, time calls, `BeginFullScreen`/`SetDepth`.

## Architecture

### Decisions

| Decision | Choice | Reason |
|---|---|---|
| Language | C11, clang | Unicorn and SDL are C libraries, and all reference material from the period is C |
| CPU | Unicorn 2 (PPC32 big-endian), behind a small `cpu.h` interface | Fastest route to a running game. The interface keeps a hand-written interpreter possible as a fallback |
| Media | SDL3 | The maintained SDL. It provides window, scaling, audio stream and keyboard |
| Build | CMake, with Homebrew `unicorn` and `sdl3` | Standard; found through pkg-config |
| Threading | All emulated code runs on the main thread | The game was single-threaded, so this avoids races by construction |

### Components

```
src/
  main.c        startup, argument parsing, top-level run
  cpu.h         run_until(stop_pc), reg get/set, map memory, context save/restore
  cpu_unicorn.c Unicorn implementation of cpu.h
  guest_mem.c   the emulated address space, be16/be32 read/write, guest string helpers
  pef.c         PEF parse, pattern-data unpack, relocation engine, import binding
  trap.c        import table (name → handler), dispatch loop, guest_call()
  rsrc.c        resource fork parser and Resource Manager calls
  memmgr.c      Memory Manager: Ptr and Handle allocation in the guest heap
  qd.c          QuickDraw subset: ports, GWorlds, PixMaps, CopyBits, PaintRect, color
  pict.c        PICT v2 decoder
  display.c     SDL window, the emulated main screen, palette → RGBA, present
  sound.c       Sound Manager: channels, sound headers, audio output, callbacks
  mixer.c       command queues and mixing, host-only
  wav.c         .wav recording
  events.c      Carbon Event Manager, event loop, timers, key translation
  dialogs.c     Alert, GetNewDialog, ModalDialog, item text; draws on the emulated screen
  font.c        the dialogs' 8x8 bitmap font (SDL's debug font) and word wrap
  files.c       FSSpec file calls over the game folder and the writable folder
  cf.c          CFString/CFNumber objects and CFPreferences
  plist.c       prefs.plist, read and written with the host's CoreFoundation
  misc.c        Gestalt, TickCount, Microseconds, Delay, ICLaunchURL, AE, cursor calls
  game.c        the games: id, title, folder or bundle, program path, kind (Plan 8, 9)
  picker.c      the picker shown when more than one game is installed (Plan 8, 9)
  loader.c      loads a PEF or Mach-O program into guest memory (Plan 9 adds Mach-O)
  macho.c       reads the PowerPC slice of a Mach-O file (Plan 9)
  libc.c        the C library calls of a Mach-O game (Plan 9)
  cxxrt.c       the C++ runtime calls of a Mach-O game (Plan 9)
  nib.c         reads the windows of an Interface Builder nib (Plan 9)
  cgimage.c     PNG images through the host's ImageIO (Plan 9)
  cgdisplay.c   the CoreGraphics main display: modes, capture, its memory (Plan 9)
tests/          unit and integration tests
```

Each Carbon module registers its handlers with `trap.c` from a `*_register()` function. Modules share state only through `guest_mem`, `memmgr` and the small public headers of `qd` and `display`.

### Emulated memory map

32-bit, big-endian, backed by one host allocation mapped into Unicorn with `uc_mem_map_ptr`, so C code reads guest memory directly without copying.

| Guest range | Size | Contents |
|---|---|---|
| `0x0000_0000` | 64 KB | Zeroed low-memory page. Reads return 0, writes are logged |
| `0x0010_0000` | code size | Code section (read and execute) |
| next 4 KB boundary | 23 KB | Data section (unpacked), then the import area: one 8-byte slot per import (a transition vector for functions, a data word for data symbols) |
| `0x0100_0000` | 64 MB | Guest heap: Ptrs, Handles and master pointers, resources, pixel buffers, QuickDraw structs |
| `0x0600_0000` | 1 MB | Stack, growing down from `0x0610_0000`, with an unmapped guard page below it |
| `0x0700_0000` | unmapped | Trap addresses: import *i* is `0x0700_0000 + 4i`, and `RETURN_MAGIC` is `0x07FF_FFF0`. Nothing is mapped here, so jumping to one of these addresses stops the CPU with that exact address |
| `0x0800_0000` | unmapped | Tag space for opaque host objects (CFStringRef, window refs, event refs). These are IDs, never dereferenced |

This is the layout for PEF programs. A Mach-O program (Plan 9) loads at its linked addresses from `0x1000`, where the PEF layout keeps low memory, so it gets a second layout (`gm_init_layout(GM_LAYOUT_MACHO)`): `0`-`0x1000` stays unmapped, so a null pointer faults as on Mac OS X; `0x1000`-`0x10_0000` holds the image; and the heap is 256 MB at `0x1000_0000`, with host pages backed only when touched. Stack, trap and tag addresses are the same in both. More tag spaces were added below `0x1000_0000`: events `0x0A00_0000`, dialogs and nib windows `0x0B00_0000` (controls `0x0B04_0000`, nib references `0x0B08_0000`), CG images and data providers `0x0C00_0000`, display modes `0x0C80_0000`.

Everything the game might inspect directly lives in guest memory, in the original big-endian layout: resource data, handles, PixMap/GWorld/CGrafPort structs, FSSpecs, Rects. State the game can only reach through accessor functions stays in host C structs: SDL objects, sound channels, event handlers, timers and CF objects.

## Control flow

### Startup

1. Parse the arguments: game folder, defaulting to `/Applications/Loony Labyrinth`. Open the data fork and the resource fork (through `/..namedfork/rsrc`).
2. Map the guest memory. Load the PEF sections, unpack section 1's pattern-initialized data, and run the relocation program.
3. Bind imports. Each function import gets a transition vector `{trap_address, 0}` in the import area, and each data import gets a guest word initialized by its handler module. Import names with no registered handler still bind. They only fail when called (see Error handling).
4. Initialize the modules: the display creates the SDL window but shows nothing yet, the resource chain opens the application's resource fork as the current resource file, and sound starts its audio stream.
5. If the loader header names an init routine, call it through `guest_call`. Then call the main entry point the same way. When main returns, or the game calls `ExitToShell`, shut down cleanly.

### Calling an import (guest → host)

- The game calls an import through its transition vector in the usual CFM way: load the code address into CTR and the TOC into r2, then `bctr`. It jumps to that import's trap address.
- The trap addresses are unmapped. Unicorn's unmapped-fetch hook records the address and stops emulation, and `(address - 0x0700_0000) / 4` is the import index. This avoids any doubt about whether the instruction at a stop was executed. (Revised during planning. The first draft used a mapped stub page with a code hook.)
- The dispatch loop in `trap.c`, running outside Unicorn, looks up the handler. Handlers read their arguments from r3–r10 (and f1–f13 for floats) using PowerPC CFM calling-convention helpers, and write their result to r3 (or f1).
- The dispatch loop sets PC to LR and resumes the CPU. It also delivers any pending sound callbacks (see Sound) before resuming.

### Calling into the game (host → guest): `guest_call(tvector, args...)`

1. Save the CPU context.
2. Read the target's code address and TOC from its transition vector. Set r2 = TOC, r12 = tvector, set r3 and up from the arguments, and set LR = `RETURN_MAGIC`.
3. Set SP to a fresh 16-byte-aligned frame below the current SP, with a linkage area.
4. Run the same dispatch loop until PC == `RETURN_MAGIC`. The game's code can call imports during this, and those can call back into the game again, to any depth.
5. Read r3 and restore the context.

Used for: the init and main entry points, Carbon event handlers, event loop timers, sound callbacks, Apple Event handlers and dialog filter procs.

### The event loop

`RunApplicationEventLoop`, `ReceiveNextEvent` and `ModalDialog` all share one pump in `events.c`:

1. Poll SDL events and translate them into Carbon events: raw key down/up/modifiers, mouse, window close → quit Apple Event.
2. Dispatch each event to the handlers installed on the target chain (dispatcher → application → window), innermost first. Stop at the first handler that returns anything other than `eventNotHandledErr`. Run standard handlers where `InstallStandardEventHandler` was used.
3. Fire due `EventLoopTimer`s. We expect one of these drives the game's frame loop.
4. If the screen is dirty, present it.
5. If nothing is due, sleep until the next timer deadline or an SDL event arrives.

`QuitApplicationEventLoop` sets a flag that ends `RunApplicationEventLoop` at the end of the current pump iteration.

### Time

`TickCount` is the host monotonic clock at 60 Hz, starting at 3600 on launch, as on a Mac that booted a minute earlier: the game treats a tick count of 0 as "not scheduled" (`BGMKickOff`), so starting at 0 kept the opening music from playing on the fixed clock. (Revised during Plan 5.) `Microseconds` is the same clock in microseconds. `Delay` sleeps on the host but keeps pumping SDL events and sound callbacks, so the window never freezes.

## Subsystems

### Memory Manager (`memmgr.c`)

- The guest heap uses a simple first-fit allocator with a header before each block recording its size, whether it's a handle, and its flags.
- A handle is a guest address of a master pointer, which points at the block.
- Handles never move and are never purged. `HLock`, `HUnlock`, `HPurge`, `HNoPurge`, `MoveHHi`, `HGetState` and `HSetState` only record flags.
- `SetPtrSize` fails if the block can't grow in place, just as on a real Mac. `SetHandleSize` may relocate, since the master pointer hides the move.
- `MemError` holds the last error. Freed memory is filled with `0xDEADBEEF` in debug builds so use-after-free bugs show up.

### Resource Manager (`rsrc.c`)

- Parses the resource map once at startup: types, IDs, names, attributes.
- `GetResource` and `GetNamedResource` load the resource into a new guest handle on first use and cache it by (type, id). Repeat calls return the same handle.
- `ReleaseResource` frees the handle and clears the cache entry. `LoadResource` is a no-op for resources that are already loaded. `ResError` holds the last error, and `RecoverHandle` works for any handle.
- The only resource file is the application's. `CurResFile` returns its refNum.

### QuickDraw and display (`qd.c`, `pict.c`, `display.c`)

- **The main screen:** a GDevice with a PixMap in guest memory, 8 bits per pixel. It starts at 640×480. When `BeginFullScreen` asks for a size, the screen is recreated at that size, and `SetDepth` changes the depth (8 or 16 bits).
- **Palette:** the screen's color table starts as the standard Mac 8-bit palette. We'll find out how the game installs its own `clut` palette at runtime (the startup trace shows which calls it makes) and implement that path. At present time, the screen is converted to RGBA through the current screen color table.
- **Windows:** the game imports no window-creation call. Its windows come from `BeginFullScreen`, which returns a new full-screen window, and from `GetNewDialog`. Each window is a rectangle on the emulated screen with its own CGrafPort in guest memory. `ShowWindow` and `HideWindow` toggle visibility, and hidden windows aren't drawn. `EndFullScreen` disposes of the full-screen window and restores the previous screen size and depth.
- **GWorlds:** `NewGWorld` allocates a PixMap and pixel buffer in the guest heap with the requested depth and color table. `LockPixels`, `GetPixBaseAddr`, `GetGWorldPixMap`, `SetGWorld`, `GetGWorld`, `UpdateGWorld` and `DisposeGWorld` are implemented over that structure.
- **CopyBits:** the `srcCopy` mode at 8, 16 and 32 bits per pixel, same-depth or converting depth. It supports nearest-neighbor scaling when the source and destination rectangles differ, and clips to the destination port's clip rectangle. Any other transfer mode or mask region fails loudly. A copy to a window port marks the screen dirty.
- **Also implemented:** `PaintRect` with the foreground color, `RGBForeColor`, `ClipRect`, rectangle helpers, `GetCTable`, `GetEntryColor`, `DisposePalette`, `QDFlushPortBuffer` (marks the screen dirty and presents at the next pump), and `GetQDGlobalsScreenBits` / `GetPortBitMapForCopyBits` accessors.
- **DrawPicture:** a PICT v2 decoder covering the opcodes used by the game's 7 PICTs: header, clip, `PackBitsRect`, `DirectBitsRect`, comments, `OpEndPic`. Any other opcode fails loudly. We can test it offline against all 7. The two dialog icons (PICT 128 and 129) also carry an `UncompressedQuickTime` opcode holding an alpha matte in QuickTime Animation format; it is decoded and only the matte's opaque pixels are drawn. (Revised during Plan 6.)
- **SDL window:** resizable. The emulated screen is scaled to the largest size that fits with the correct aspect ratio, using nearest-neighbor sampling, with vsync. Cmd-F toggles fullscreen and Cmd-Q quits (sends the quit Apple Event). The game never sees these two keys.

### Sound (`sound.c`, `mixer.c`)

- **Channels:** `SndNewChannel` creates a host channel with a FIFO command queue. `SndDoCommand` appends a command and `SndDoImmediate` runs it at once. `SndChannelStatus` and `SndDisposeChannel` are implemented.
- **Supported commands:** `bufferCmd` (a sampled sound header: standard, or extended with 8-bit unsigned or 16-bit signed samples, mono or stereo, any sample rate), `soundCmd` (accepted, does nothing), `quietCmd`, `flushCmd`, `waitCmd`, `volumeCmd`, `callBackCmd`, `nullCmd`. Unknown commands and header encodings (compressed headers) are logged once and ignored, not fatal, because sound problems shouldn't stop the game. (Measured: the game uses only standard 8-bit mono headers at 11,127 Hz on two channels, music and effects.)
- **Copy on issue:** `SndDoCommand`/`SndDoImmediate` copy a buffer's samples to the host on the main thread, so the mixer never reads guest memory and the game may reuse a buffer as soon as its command is issued. (Revised during Plan 5; the first draft parsed buffers on the audio thread.)
- **Mixer (`mixer.c`):** integer-only, 16-bit stereo at 44.1 kHz with linear interpolation, channels summed and clamped. It runs each channel's queue: a buffer plays to its end, then the next command runs on the following frame, so chained buffers have no gap. A `callBackCmd` posts to a callback queue (256 entries; a channel stalls while it is full, so none are lost).
- **Two drivers, one mixer:** with an audio device, SDL's audio stream callback renders on the audio thread with the stream's lock held, and the main thread takes that lock for every mixer call. With `LOONY_FIXED_CLOCK=1`, or no device, the main thread renders up to the current time at every pump and before every Sound Manager call, so channels are busy exactly as long as their sounds would play and runs stay deterministic. (Revised during Plan 5; the first draft named a lock-free queue.)
- **Callbacks:** the main thread drains the callback queue at every pump iteration, calling the channel's callback with `guest_call`. The game code never runs on the audio thread.
- **Recording:** `LOONY_WAV=path` writes what the mixer renders to a .wav file, finished at exit. On the fixed clock it is the same file every run, which the end-to-end tests compare against a golden hash.

### Events and input (`events.c`)

- Implements the Carbon event calls on the import list: handler install and remove, event targets, `GetEventKind`, `GetEventParameter` (key code, character code, modifiers, mouse location, direct object), `SendEventToEventTarget`, `ReleaseEvent`, `ReceiveNextEvent`, timers, and the UPP constructors (which return the procedure pointer unchanged).
- **Keyboard:** SDL scancodes are translated to Mac virtual key codes through a static table covering the full US layout. Modifiers are reported with left and right distinguished (for example `rightShiftKey`), since pinball games often map the flippers to left and right Shift or Command.
- **`HideCursor` / `InitCursor` / `SetThemeCursor`:** hide or show the SDL cursor. It always shows while a dialog is open.

### Dialogs (`dialogs.c`)

- **`Alert` / `StopAlert`:** build the dialog from its `ALRT` and `DITL` resources, apply `ParamText` substitutions, and draw it on the emulated screen. They run a modal loop until a button is clicked or Return is pressed (Esc for a button titled "Cancel"), restore the screen under the dialog, and return the item number. `LOONY_AUTO_ALERTS=1` answers with the default item at once, without drawing, for headless runs whose golden frames predate dialogs.
- **`GetNewDialog` / `ModalDialog`:** draw `DITL` items (buttons, static text, edit text, icon/PICT items) on the emulated screen with a built-in bitmap font: SDL's 8x8 debug font, rasterized at startup, with Mac Roman text spelled in ASCII. Edit text supports typing, Delete, Tab and Shift-Tab between fields, clicking a field, and Cmd-V. `ModalDialog` returns when an enabled button is chosen; the game passes no filter proc, and one would fail loudly. The default button is item 1 if it is a button, otherwise the first button, so Return registers in the key-code form. `GetDialogItem` (text items get a handle holding their text), `GetDialogItemText` and `DisposeDialog` are implemented. (Revised during Plan 6.)
- **Input while a dialog is open** goes to the dialog, not the game, and no timers fire. The game is still sent modifier changes, and the key-up of any key it saw go down, so no key looks held once the dialog closes. Mouse clicks are mapped from the window to the emulated screen.
- **Measured uses (Plan 6):** the shareware alerts at startup (901, then 900 "Play Demo"), the registration form (DLOG 911: e-mail address and key code, no filter proc), its results (902 refused, 903 certified), the OS checks (800, 801) and the exception report (9000). High-score name entry and the options are drawn by the game itself, not with dialogs.
- These dialogs look plain, not like real Mac OS dialogs, but they work.

### Files and preferences (`files.c`)

- **Two folders:** the game folder, read-only, and a writable folder at `~/Library/Application Support/loony-shim/<game id>/` (`loony-labyrinth` or `crystal-caliburn`). Loony Labyrinth's files from before Plan 8, saved directly in `loony-shim/`, move into its folder at launch; one already there is never overwritten.
- **Reading:** looks in the writable folder first, then the game folder.
- **Writing:** writes to any path inside the game folder go to the matching path in the writable folder, copying the file there at its first write (not when it is opened) if it exists. This keeps the original files untouched while the game believes it saved in place. `LOONY_DATA_DIR` names another writable folder (the tests use temporary ones); one that is, contains or is inside the game folder is refused, and nothing is saved. A copy is made under a temporary name and renamed when complete, so a failed copy never hides the original. Names `.` and `..` are refused, and `::` goes up one folder but never above the game folder. (Measured in Plan 6: the game writes no files in normal play; everything it saves is a preference.)
- **FSSpec calls:** `FSMakeFSSpec` resolves vRefNum/dirID/name to a host path, using a small table of fake volume and directory IDs. `FSpCreate`, `FSpOpenDF`, `PBReadSync`, `FSWrite`, `GetEOF`, `SetEOF`, `GetFPos`, `SetFPos`, `FSClose` and `PBFlushFileSync` map to POSIX calls. Mac-Roman file names are converted to UTF-8, and `:` becomes `/`.
- **CFPreferences:** stored in `prefs.plist` in the writable folder, an XML property list of strings and integers written with the host's CoreFoundation, read at startup and written on `CFPreferencesAppSynchronize` (the game calls it as it quits) and at any other exit but a crash, as macOS keeps values an application set without synchronizing. (Revised during Plan 7: a quit during a sequence that outlasted the 3-second quit grace lost the game's unsaved changes.) A file that can't be read is moved to `prefs.plist.bad`. CFString and CFNumber are host objects referred to by tag-space IDs, with reference counts. `kCFPreferencesCurrentApplication` is a pre-made CFString ID. (Measured in Plan 6: the game keeps its options, key assignments, the four-entry high-score table with a checksum, "highscore id", and the license, "user email" and "user id", in the preferences.)

### Miscellaneous (`misc.c`)

- **`Gestalt`:** a fixed table. It reports OS X 10.2.8 (`sysv` = 0x1028), a G3 CPU with no AltiVec (`cpuf`/`ppcf` AltiVec bit clear), and Carbon present. Unknown selectors return `gestaltUndefSelectorErr` and are logged.
- **`ICStart` / `ICStop`** are no-ops. **`ICLaunchURL`** opens the URL in the default browser (http and https only).
- **`AEInstallEventHandler`** records the handler. The quit event is sent on window close and Cmd-Q.
- **`KeyScript`, `GetMBarHeight` (returns 0), `ReadLocation`, `GetDateTime`, `NumToString`, `num2dec`, `p2cstrcpy`, `c2pstrcpy`, `BlockMoveData`, `ExitToShell`:** straightforward.

## Mac OS X games (Plan 9)

*MONSTER FAIR 1.2.5* (2010) is a Mac OS X application, not a Carbon program in a folder: `/Applications/MONSTER FAIR.app`, whose program `Contents/MacOS/MONSTER FAIR` is a universal Mach-O binary (PowerPC and i386) built with GCC 4 and C++. It has no resource fork; it reads its data from files in `Contents/Resources` and builds its dialogs from an Interface Builder nib. Of its 205 imports, 95 were already implemented. The game table gives each game a kind, `GAME_PEF_FOLDER` or `GAME_MACHO_BUNDLE`, and everything that differs for MONSTER FAIR is chosen by that kind, so the classic games run exactly as before.

- **A second program format.** `macho.c` reads the PowerPC slice of a fat or thin Mach-O executable: segments, sections, symbols, the indirect symbol table and external relocations, with a bounds check on every offset. `image_load_macho` copies `__TEXT` and `__DATA` to their linked addresses (the program isn't position-independent), points every lazy and non-lazy symbol pointer at a trap address or a data object, and adds a symbol's address at each external relocation (the stored word is the addend). Data symbols come from a resolver chain (`libc`, then `cxxrt`, then `cf`); an import the shim can't bind fails the load by name, not at its first call. Crash reports print `code+<linked address>`, matching a disassembly of the file.
- **No `crt1`.** The shim runs the `__mod_init_func` entries (the C++ static constructors), as dyld would, then calls `main(argc, argv, envp, apple)` directly. The loader finds `main` by following the entry point's `bl` into `_start` and taking the `bl` just before the one to the `_exit` stub. This skips dyld's private lookup interface, keymgr's dwarf2 registration and the Mach and cthread init hooks.
- **Direct calls.** A Mach-O function pointer is a code address, not a transition vector. `trap_set_direct_calls(true)` makes `guest_call` jump straight to the address it is given, with `r12` set to it as GCC's Darwin code expects. Every caller (event handlers, timers, sound callbacks, `qsort`) still goes through `guest_call`.
- **The C library (`libc.c`)** covers what the game imports from libSystem: `malloc` and friends on the Memory Manager's pointer heap (one allocator, so `MemError` and crash reports see everything), string and memory calls through `gm_ptr`, a stable `qsort` calling the guest comparator, Darwin 10.4's `rand`, `time`, `localtime` (UTC on the fixed clock) and `usleep` on `misc`'s clock, `dlsym` for the one symbol the game looks up (`sprintf$LDBL128`), and the `_DefaultRuneLocale` table.
- **The C++ runtime (`cxxrt.c`):** `new` and `delete` on the same heap, static-local guards, and the type-info vtables (identity only). Exceptions are not implemented: `__cxa_throw`, `__cxa_rethrow` and `_Unwind_Resume` crash, naming the thrown type and the throw site. Nothing throws in normal play (measured through the registration flow, the menus, games and an hour-long soak), so an unwinder over `__eh_frame` was never needed.
- **Bundles, URLs and files.** `CFBundleGetMainBundle`, `CFBundleCopyResourceURL` (also looking in `English.lproj`) and CFURLs give the game real host paths inside the bundle. Constant CFStrings in `__cfstring` are read from guest memory by every CF call that takes a string. `FSPathMakeRef` accepts paths in the bundle or the save folder; the fork calls (`FSOpenFork`, `FSGetForkSize`, ...) share refnums with the File Manager calls, since the game mixes them, and a write to a bundle file goes to the save folder's copy, as for the classic games. `sysv` is 10.4.11 (`0x104B`) for a Mach-O game.
- **Nib windows.** `nib.c` reads the part of `objects.xib` the game uses: windows by name from the `nameTable`, with their buttons (and command IDs), static texts, edit texts, image views and icons. `dialogs.c` draws them with the DLOG code. A click, Return or Esc sends `kEventCommandProcess` with a 14-byte `HICommand` to the window's handlers; the game calls `QuitAppModalLoopForWindow`, which ends `RunAppModalLoopForWindow`. `GetControlData` returns an edit text's text, and the image view draws `appl.png`, decoded with the host's ImageIO (`cgimage.c`) and scaled to its frame. `CreateStandardAlert` and `RunStandardAlert` draw like `Alert`. `LOONY_AUTO_ALERTS=1` answers nib windows with their `'ok  '` button.
- **The display.** MONSTER FAIR plays full screen: it asks CoreGraphics for a 1024×768, 16-bit mode, captures the display, and draws straight into the display's memory. `cgdisplay.c` makes the emulated screen the main display: `CGDisplaySwitchToMode` resizes it in place (`qd_resize_screen`: the GDevice and port addresses stay the same), `CGDisplayBaseAddress` and `CGDisplayBytesPerRow` describe its pixels, and while it is captured the screen is presented at every pump, because the game's drawing bypasses QuickDraw. A mode is an opaque ID, not a CFDictionary, since the game reads none. Capturing doesn't make the host window full screen; Cmd-F still does. On quitting, the game switches back to 800×600, 32 bits. The windowed path (`CreateNewWindow` and a GWorld) is implemented, but only runs when no mode is found, which never happens here.
- **The picker** shows up to three cards in a row; MONSTER FAIR's shows `appl.png` over its name, since its title screen is in its own data format.

*Mad Daedalus 1.1.9*, LittleWing's other Mac OS X game, is out of scope: it shares the loader and runtimes, but needs AudioToolbox sound, `mmap` and drawing without QuickDraw at all.

## Error handling and diagnostics

- **Unimplemented import, guest crash (unmapped memory access, illegal instruction, stack guard hit), or an unsupported option in an implemented call:** stop and print the reason, the import name or fault address, the guest PC and LR, all registers, and the last 64 imports called with their arguments. Exit with code 2. Don't guess and keep going.
- **Exception:** sound command problems and unknown `Gestalt` selectors are logged, not fatal.
- **`LOONY_TRACE=imports`:** logs every import call with its decoded arguments and return value.
- **`LOONY_TRACE=calls`:** also logs each `guest_call` entry and exit.
- **Symbolication:** guest addresses are printed as `code+0xNNNN` so they match a Ghidra project loaded from the same binary.
- **Debug build:** AddressSanitizer and UBSan on the host code, and heap poisoning in the guest.

## Testing

- **Unit tests** (a small built-in runner, `ctest`):
  - big-endian helpers
  - heap allocator and handle semantics
  - pattern-data unpacking (hand-built vectors for each opcode)
  - relocation opcodes (hand-built vectors)
  - PEF loading of the real binary: 3 sections, 132 imports with the names above, one data import
  - resource parsing of the real resource fork: counts per type match the Facts table
  - PICT decoding of all 7 PICTs to PNG, compared against stored checksums once the user approves the images
  - `snd` header parsing
  - a `guest_call` round trip using a small hand-assembled PPC function that calls an import and returns
- **Game-file tests** read the game folder from `LOONY_GAME_DIR`, defaulting to `/Applications/Loony Labyrinth`, and are skipped if it's missing.
- **Headless integration test:** SDL's dummy video and audio drivers, a scripted input file (key events at given ticks), and a fixed tick clock, so runs are deterministic. It dumps the emulated screen to PNG at chosen ticks and compares against screenshots the user approved during playtesting.
- **Playtest:** the user works through the success criteria at the end of milestones 4, 5 and 6.

## Milestones

| # | Milestone | Done when |
|---|---|---|
| 0 | Toolchain and skeleton | `brew install unicorn sdl3 cmake pkg-config` done. CMake builds, unit test runner passes on an empty test |
| 1 | Loader and dispatch | Real binary loads and relocates. `guest_call(main)` runs until the first unimplemented import and prints its name. `guest_call` round-trip test passes |
| 2 | Core services | Memory, Resource, misc and Gestalt calls implemented. The game runs until it needs graphics |
| 3 | Graphics | Screen, GWorlds, CopyBits, PICT, palettes. The title screen appears and matches the original (user checks) |
| 4 | Events and input | Event loop, timers, keyboard. The table plays silently (user playtest) |
| 5 | Sound | Sound Manager and mixer. Effects and music are correct (user playtest) |
| 6 | Dialogs, files, prefs | High scores and preferences persist across launches (user playtest) |
| 7 | Finish | One-hour run with no crash. `.app` bundle, ad-hoc signed, with the `allow-jit` entitlement. Headless regression test recorded (three minutes of scripted play, `run_three_minutes_of_play_match_the_recording`) |

Plan 8 added Crystal Caliburn 3.0.1, which runs on the same engine with the same 132 imports (`src/game.c` lists the games), and a picker showing each game's title picture (PICT 800) from the user's copy.

Plan 9 added MONSTER FAIR 1.2.5, the first Mac OS X game (see Mac OS X games): a Mach-O loader, a second memory layout, C and C++ runtimes, nib windows and the CoreGraphics display.

Milestones 1–3 have the most unknowns. Each later milestone's details may be adjusted based on what the import trace shows the game actually does.

## Risks and fallbacks

| Risk | Fallback |
|---|---|
| Unicorn PPC32 misexecutes something, or nested runs misbehave | Write our own interpreter behind `cpu.h`, about a week of work. The Carbon layer stays the same |
| The game ignores `Gestalt` and runs AltiVec code | Pick a Unicorn CPU model with AltiVec (7400), or implement the few instructions used in the interpreter fallback |
| Sound callback timing makes music stutter | Queue more buffers ahead, deliver callbacks from a tighter pump, or run callbacks at exact sample positions |
| The game relies on undocumented OS behavior (lowmem globals, struct fields) | The fail-loud logging shows where. Implement the specific field or global it needs |
| Palette mechanism unclear | Trace at milestone 3 and implement the path the game uses |
| Physics timing depends on the frame rate | Match the original timer interval exactly. Present on the timer, not the display refresh rate |

## Build and repository

- `~/dev/loony-shim`, a git repo on `main`. C11, `-Wall -Wextra -Werror` in all builds. `Debug` adds the sanitizers, and `Release` is `-O2`.
- `.gitignore` excludes build output, PNG dumps and anything copied from the game folder.
- Dependencies come from Homebrew: `unicorn`, `sdl3`, `cmake`, `pkg-config`. The preferences file also uses macOS's own CoreFoundation framework.
- **The app** (`cmake --build build-release --target app`, `tools/make_app.sh`): `LittleWing.app` (bundle id `local.loony-shim`, unchanged), with `libunicorn` and `libSDL3` copied into `Contents/Frameworks` and referenced through `@rpath`, an icon built from `tools/AppIcon.png` (original art, not from the game; drawn as a superellipse so macOS 26 shows it unframed), signed ad hoc with the hardened runtime and the entitlements `allow-jit` (Unicorn's translated code) and `disable-library-validation` (ad-hoc signatures carry no team ID, so the hardened runtime would otherwise refuse the bundled libraries). With `LOONY_SIGN_ID` set to a Developer ID Application identity, the bundle is signed with it and a secure timestamp instead, and `tools/notarize.sh` notarizes and staples it and zips it for giving to others. The bundle requires macOS 26 (`LSMinimumSystemVersion`), the deployment target of the binary and of the Homebrew libraries. Launched as the app, with no terminal, the log goes to `~/Library/Logs/loony-shim/loony.log` and startup failures and crashes appear in a message box. (Revised during Plan 7.) When two games are installed, the app shows the picker (`src/picker.c`); when a picked game quits from its own menu, the app saves its preferences and restarts itself to show the picker again, while Cmd-Q or closing the window quits. (Revised during Plan 8.)
