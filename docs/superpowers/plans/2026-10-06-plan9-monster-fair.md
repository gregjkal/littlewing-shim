# Plan 9: MONSTER FAIR, the first Mac OS X game

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `LittleWing.app` plays *MONSTER FAIR 1.2.5* as well as Loony Labyrinth and Crystal Caliburn.
- The picker shows all the installed games, up to three.
- MONSTER FAIR saves its preferences in its own folder, like the other two.
- Its shareware windows work, and so does registering with a key code.

**Why this is bigger than Plan 8:** Crystal Caliburn ran on the unchanged shim. MONSTER FAIR doesn't. It is a Mac OS X bundle (`/Applications/MONSTER FAIR.app`) whose program is a universal Mach-O binary (PowerPC and i386), built in 2010 with GCC 4 and C++. It has no resource fork. It reads its data from files in `Contents/Resources` and builds its dialogs from an Interface Builder nib. Of its 205 imports, 95 are already implemented. The other 110 are mostly the C and C++ runtime, plus about 55 Carbon calls.

**Architecture:**
- **A second program format.** `src/macho.c` reads the PowerPC slice of a fat Mach-O file. The loader (`src/loader.c`) gains `image_load_macho`. It places `__TEXT` and `__DATA` at their linked addresses (the program isn't position-independent). It points every lazy and non-lazy symbol pointer at a trap address or a data object, and adds symbol addresses at the external relocations. PEF loading doesn't change.
- **A second memory layout.** Mach-O programs load at `0x1000`, where the PEF layout keeps low memory. With `GM_LAYOUT_MACHO`, page zero stays unmapped, `0x1000`-`0x100000` is the image, and the heap is bigger (the game's data files total about 50 MB). The PEF layout is unchanged.
- **Direct calls.** A Mach-O function pointer is a code address, not a transition vector. `trap_set_direct_calls(true)` makes `guest_call` jump straight to the address it's given. Every caller (event handlers, timers, sound callbacks, `qsort`) keeps calling `guest_call` as now.
- **No `crt1`.** The shim runs the `__mod_init_func` entries (C++ static constructors) and then calls `main(argc, argv, envp, apple)`. The loader finds `main` by following the entry point's `bl` into `_start` and taking the `bl` just before the one to the `_exit` stub. Skipping `_start` avoids dyld's private lookup interface, keymgr's dwarf2 registration and the Mach and cthread init hooks.
- **A small C runtime (`src/libc.c`) and C++ runtime (`src/cxxrt.c`)**, as trap handlers. `malloc` and `new` share the Memory Manager's pointer heap. `__cxa_throw` crashes with the exception's type name (see Decisions).
- **Nib windows.** `src/nib.c` reads the subset of `objects.xib` the game uses: windows by name, and their buttons, static texts, edit texts, image views and icons. `dialogs.c` draws them with the code that draws ALRT and DLOG dialogs today. Clicking a button sends `kEventCommandProcess` with the button's command ID to the window's handlers, which is how the game learns which button it was.
- **The game table** gains a kind (`GAME_PEF_FOLDER` or `GAME_MACHO_BUNDLE`) and, for a bundle, the program's path inside it.

**Tech Stack:** C (gnu11), clang, CMake ≥ 3.20, Unicorn 2, SDL3, and on the host ImageIO and CoreGraphics (to decode the one PNG the game shows in its dialogs).

**Spec:** `docs/superpowers/specs/2026-09-30-loony-shim-design.md`, revised in Task 14. The second Mac OS X game, *Mad Daedalus*, is out of scope. It shares the loader and the runtimes from this plan, but needs a different display (it writes to the captured screen's memory), AudioToolbox sound and `mmap`.

## Global Constraints

- Repo: `~/dev/loony-shim`, branch `main`. All paths below are relative to it.
- The game files are read-only inputs. Nothing from them goes into the repo or the app. Nothing is ever written inside `MONSTER FAIR.app`: a write to a file in the bundle goes to the game's save folder, as `files.c` already does for the classic games' folders.
- The user's key codes and e-mail address never appear anywhere. Only the README's public test key (`docs/test_key.txt`, for Loony Labyrinth) may appear in tests.
- C11 with GNU extensions, `-Wall -Wextra -Werror`. Debug builds add the sanitizers.
- No test opens a real window, plays sound, shows a message box, or touches the real `~/Library`. Tests set `HOME`, `LOONY_DATA_DIR` and `LOONY_APPS_DIR` to temporary folders, and the whole test run sets `SDL_VIDEO_DRIVER=dummy`.
- **The classic games don't change.** All of Loony Labyrinth's and Crystal Caliburn's golden frames and recordings stay as they are. The PEF memory layout, `Gestalt` answers, `guest_call` and every PEF code path behave as before. Anything that changes for MONSTER FAIR is chosen by the program kind.
- `loony <folder>` keeps playing the classic game in that folder. `loony "/Applications/MONSTER FAIR.app"` plays MONSTER FAIR, with no picker.
- The app's bundle id doesn't change.

## Facts measured (2026-10-06, by reading the PowerPC slice; nothing has run yet)

| Fact | Value |
|---|---|
| The bundle | `/Applications/MONSTER FAIR.app`, `CFBundleIdentifier` `com.littlewingpinball.monsterfair`, version 1.2.5, signature `LwMf`. Program `Contents/MacOS/MONSTER FAIR`, fat: PowerPC slice (cputype 18, subtype 10 = ppc7400) at file offset `0x1000`, 392,648 bytes; i386 slice at `0x61000` |
| Data files | `Contents/Resources`: `field.bin`, `visual.bin`, `effect.bin`, `placard.bin`, `ball.bin`, `snda.bin`, `sndf.bin`, `pal.bin`, `dctm0001.bin` to `dctm0011.bin` and `dctm0020.bin` to `dctm0022.bin` (named by `%sdctm%04d.bin`), `appl.png` (128×128). About 50 MB in all |
| Resource fork | None. `CurResFile` is imported; no other Resource Manager call is |
| Header | `MH_EXECUTE`, flags `0x10095` (NOUNDEFS, DYLDLINK, PREBOUND, TWOLEVEL, ALLMODSBOUND). Not position-independent |
| Segments | `__PAGEZERO` `0x0`-`0x1000`; `__TEXT` `0x1000`-`0x5b000` (file `0x0`, r-x); `__DATA` `0x5b000`-`0x60000` (file `0x5a000`, `0x3000` bytes in the file, rw-); `__LINKEDIT` `0x60000`-`0x62dc8` |
| Sections that matter | `__text` `0x2430` (`0x40dc0` bytes); `__symbol_stub1` `0x431f0`, 193 stubs of 16 bytes; `__nl_symbol_ptr` `0x5b03c`, 66 slots (7 to imports, 59 local); `__la_symbol_ptr` `0x5b144`, 193 slots (all 0 in the file); `__mod_init_func` `0x5b008`, 13 entries; `__cfstring` `0x5d2c8`, 8 constant CFStrings; `__dyld` `0x5b000` (`0x8fe01000`, `0x8fe01008`); `__eh_frame` `0x4f354`, `__gcc_except_tab` `0x4c810` |
| Entry and `main` | `LC_UNIXTHREAD` srr0 `0x2fdc`. It calls `_start` at `0x3010`, which ends `bl 0x41ca8` then `bl` to the `_exit` stub. `main` is `0x41ca8`. It begins with two `Gestalt` calls |
| Imports | 205: Carbon 150, libSystem 38, libstdc++ 16, libgcc_s 1. 95 have handlers today. The 110 missing are listed under Tasks 4, 5, 7, 8, 9 and 10. All 205 are `N_PBUD \| N_EXT` (type `0xd`, prebound undefined), not `N_UNDF`. The indirect table has 452 entries (193 stubs, 66 non-lazy, 193 lazy) |
| Non-lazy pointers to imports | `_mach_init_routine`, `_errno`, `__cthread_init_routine`, `___keymgr_global`, `___gxx_personality_v0`, `_kCFPreferencesCurrentApplication`, `__DefaultRuneLocale` |
| External relocations | 212, all 32-bit absolute, extern, unprebound: the stored word is the addend. `__ZTVN10__cxxabiv120__si_class_type_infoE` ×81, `___cxa_pure_virtual` ×69, `__ZTVN10__cxxabiv121__vmi_class_type_infoE` ×37, `__ZTVN10__cxxabiv117__class_type_infoE` ×17, `___CFConstantStringClassReference` ×8. `r_address` is the absolute address |
| A constant CFString | At `0x5d2c8`: isa (relocated), flags `0x7c8`, pointer `0x45748` (`"main"`, the nib's name), length 4 |
| Call sites | `__Unwind_Resume` 267, `__Znwm` 157, `__ZdlPv` 157, `__ZdaPv` 63, `___cxa_allocate_exception` 51, `___cxa_throw` 41, `___cxa_rethrow` 3. `_FlushEvents` and `_HSetState` are never called directly |
| Exceptions | Classes `RT::TException`, `TOSException`, `TQDException`, `TUserException`. Messages "Unexpected operating system error occurred [%d:%d]". The throw sites are in the platform layer (`TFileInputStream_MacOS.cpp`, `TVisual_MacOS.cpp` and so on). They look like error paths, but that's unconfirmed until Task 6 runs |
| `dlsym` | Three calls. The strings `sprintf` and `$LDBL128` sit next to `libSystem.` in `__cstring`, so the game looks up `sprintf$LDBL128` at run time |
| Display | Windowed (`CreateNewWindow`, a GWorld, `CopyBits`, `QDFlushPortBuffer`) or full screen (`CGDisplayCapture`, `CGDisplaySwitchToMode`, `CGDisplayBaseAddress`). Preference keys `display width`, `display height`, `display depth`, `mode screen size` |
| Preferences | `CFPreferences` with `kCFPreferencesCurrentApplication`. Keys like the classic games' (`highscore 1`…, `keycode flipper left`…, `user email`, `user id`, `signet`), plus `highscore name/aux/timestamp n` and `switch sound`/`switch music` |
| The nib | `English.lproj/main.nib/objects.xib`, 25,559 bytes, XML. Named windows `Welcome` (Quit `not!`, Buy Now `Ans2`, Enter Key-Code `Ans3`, Play Demo `ok  `; an image view, signature `Appl` id 128), `Register` (two edit texts, signature `User`, ids 0 and 1; Cancel `not!`, Register `ok  `), `Demo` (the key list; OK), `AuthorizeFailed`, `ThankYou`, `MainWindow`, `MenuBar` |
| Gestalt today | `sysv` answers `0x1028` (10.2.8). `main` calls `Gestalt` twice before anything else. Mad Daedalus's nib says "MacOS X 10.3.1 or later required", so MONSTER FAIR probably checks for 10.3 too; Task 6 confirms |
| Startup, measured (Task 6, 2026-10-08, empty save folder, fixed clock) | The 13 initializers each register their destructors the way crt1 does: `_keymgr_get_and_lock_processwide_ptr(14)`, `calloc(20, 1)` the first time, `dlopen("/usr/lib/libSystem.B.dylib")`, `dlsym` of `__cxa_atexit` and `__cxa_finalize` (neither is provided, so the program keeps its own list), `_keymgr_set_and_unlock_processwide_ptr(14, list)`. One initializer calls `_Znam(1)`, another `Gestalt('vm  ')`. Nothing throws |
| `main`, measured | `Gestalt('sysv')` at `0x41d1c` and `Gestalt('cbon')` at `0x41d38`; it goes on with 10.4.11 (`0x104B`) and Carbon 1.6. Then preferences: `CFPreferencesCopyAppValue`, a default written for each of about 20 keys (`CFNumberCreate` with type 9, `kCFNumberIntType`, and `CFPreferencesSetAppValue`), each read back with `CFPreferencesGetAppIntegerValue`. Then `Gestalt('mach')` at `0x396a4`, `CreateNibReference(CFSTR("main"))` at `0x40fd8` (call 252, the first unimplemented import), `CreateWindowFromNib(nib, CFSTR("Welcome"))`, `DisposeNibReference`, `GetWindowEventTarget`, `InstallEventHandler` on the window (one event type, list at `0x4c7f6`), `RepositionWindow(w, NULL, 7)`, `ShowWindow`. No file is opened and no game window is created before the Welcome window, so the display size and `CreateNewWindow`'s arguments wait for Task 10 to be measured |
| Constant CFStrings | `0x5d2c8` "main", `0x5d2d8` "appl.png", `0x5d2e8` "Welcome", `0x5d2f8` "Demo", `0x5d308` "Register", `0x5d318` "AuthorizeFailed", `0x5d328` "ThankYou", `0x5d338` "Monster Fair" |

## Decisions this plan makes

1. **Call `main` directly instead of running `_start`.** dyld runs the main program's `__mod_init_func` entries before jumping to the entry point, so the shim does the same, then calls `main`. `_start` would need `__dyld_func_lookup`, keymgr's dwarf2 registration and the Mach and cthread hooks, and buys nothing. The imports only `_start` uses get trivial handlers anyway, in case something else calls them.
2. **Exceptions crash, loudly.** `__cxa_throw` crashes with the thrown type's name (from its `type_info`) and the throw site. If Task 6 shows a throw during normal play, Task 12 (contingent) adds a host-side unwinder over `__eh_frame` and `__gcc_except_tab`. Until then, 267 `_Unwind_Resume` landing pads and the personality routine are never reached.
3. **One heap.** `malloc`, `calloc`, `free`, `new` and `delete` allocate from the Memory Manager's pointer heap, so `MemError` and the crash reports see one allocator. In the Mach-O layout the heap is 256 MB at `0x10000000`.
4. **`sysv` is 10.4.11 (`0x104B`) for Mach-O games** and stays `0x1028` for PEF games.
5. **Paths the game sees are real host paths.** `CFBundleCopyResourcesDirectoryURL` returns `file:///Applications/MONSTER%20FAIR.app/Contents/Resources/`. `FSPathMakeRef` accepts paths inside the bundle or the save folder, and fails with `fnfErr` for anything else. `files.c` keeps its rule that writes go to the save folder.
6. **The emulated screen takes the size the game asks for.** `qd_init` still runs before `main`, at 800×600×32. The first `CreateNewWindow`, or a `CGDisplaySwitchToMode`, resizes it (`qd_resize_screen`). `CGDisplayCapture` doesn't force the host window full screen. Cmd-F does that, as for the classic games.
7. **The picker gets three cards in a row.** MONSTER FAIR has no title picture in a format the shim reads (its title screen is in `visual.bin`). Its card shows `appl.png`, centered, over the title. A drawn title screen can come later.
8. **No new dependency.** The game's one PNG is decoded with the host's ImageIO, linked as a framework, not with a new library.

## Review Focus

1. **The classic games are untouched:** every existing golden frame and recording matches. PEF loading, the PEF memory layout, `Gestalt`, `guest_call` and `files.c`'s folder handling only change behind the program kind. Covered by the existing `run_*` tests and Task 2's `gm_pef_layout_is_unchanged`.
2. **Binding is complete or the load fails:** every lazy and non-lazy pointer to an import holds a trap address or a data object, every external relocation adds (not stores) the symbol's address, and an import the shim has no slot for fails the load by name, not at first call. Covered by Task 3.
3. **An exception during play is never silent:** `__cxa_throw` and `_Unwind_Resume` crash with the type name and the throw site. Covered by Task 5's `cxxrt_throw_crashes_naming_the_type`.
4. **Nothing is written into the bundle**, and the registration flow works: the Welcome and Register windows take a key code, a wrong one shows AuthorizeFailed, and the license survives a relaunch. Covered by Tasks 8 and 10.
5. **Fixed-clock runs are deterministic:** `time`, `localtime`, `usleep`, `rand` and `Microseconds` all follow `misc`'s clock, so two runs from the same empty save folder produce the same frames and recording. Covered by Task 15's golden run.

---

### Task 1: Reading a Mach-O file

**Files:**
- Create: `src/macho.h`, `src/macho.c`
- Create: `tests/macho_build.h` (builds small Mach-O files in memory for tests)
- Test: `tests/test_macho.c`

**Interfaces:**
- Produces:
  ```c
  #define MACHO_MAX_SEGMENTS 8
  #define MACHO_MAX_SECTIONS 32

  typedef struct {
      char name[17];
      uint32_t vmaddr, vmsize, fileoff, filesize;
      int initprot;            /* VM_PROT_* bits */
  } macho_segment;

  typedef struct {
      char segname[17], sectname[17];
      uint32_t addr, size, offset;
      uint32_t flags;          /* low byte: S_* type */
      uint32_t reserved1;      /* first indirect-symbol index (pointer and stub sections) */
      uint32_t reserved2;      /* stub size (stub sections) */
  } macho_section;

  typedef struct {
      const char *name;        /* without the leading '_' : "CFRelease", "_Znwm" */
      uint8_t type;            /* n_type */
      uint16_t desc;           /* n_desc: library ordinal in the high byte */
      uint32_t value;
  } macho_symbol;

  typedef struct { uint32_t address; uint32_t symbol; } macho_extern_reloc;

  typedef struct {
      const uint8_t *data;     /* the PowerPC slice; points into the caller's buffer */
      size_t len;
      uint32_t entry;          /* LC_UNIXTHREAD srr0 */
      int nsegments, nsections;
      macho_segment segments[MACHO_MAX_SEGMENTS];
      macho_section sections[MACHO_MAX_SECTIONS];
      uint32_t nsyms;
      macho_symbol *syms;
      uint32_t nindirect;
      uint32_t *indirect;      /* INDIRECT_SYMBOL_LOCAL 0x80000000, _ABS 0x40000000 */
      uint32_t nextrel;
      macho_extern_reloc *extrel;
      int ndylibs;
      char dylibs[16][128];
  } macho_file;

  /* Parses a thin PowerPC Mach-O executable, or the PowerPC slice of a fat
     one. On failure writes err and leaves nothing to free. */
  bool macho_parse(const uint8_t *buf, size_t len, macho_file *m, char *err, size_t errlen);
  void macho_free(macho_file *m);
  const macho_section *macho_find_section(const macho_file *m, const char *seg, const char *sect);
  ```
- All multi-byte fields are big-endian, in the fat header and in the slice.

- [x] **Step 1: Write the failing tests**

`tests/macho_build.h` builds a fat file with one PowerPC slice holding `__TEXT` (a `__text` and a `__symbol_stub1` with two stubs), `__DATA` (a `__nl_symbol_ptr` with one slot, a `__la_symbol_ptr` with two, a `__mod_init_func` with one entry), a symbol table with three undefined externals (`_malloc`, `_CFRelease`, `_kCFPreferencesCurrentApplication`), an indirect table, one external relocation, two `LC_LOAD_DYLIB`s and an `LC_UNIXTHREAD`. Tests:

- `macho_parses_a_fat_file_with_a_ppc_slice`: segments, sections, entry, dylib names.
- `macho_parses_a_thin_ppc_file`.
- `macho_refuses_a_file_without_a_ppc_slice`: an i386-only fat file gives "no PowerPC code".
- `macho_refuses_truncated_load_commands`, `macho_refuses_a_section_outside_the_file`, `macho_refuses_an_indirect_index_past_the_symbol_table`.
- `macho_strips_one_leading_underscore`: `_CFRelease` reads as `CFRelease`, `__Znwm` as `_Znwm`, `___cxa_throw` as `__cxa_throw`.
- `macho_reads_external_relocations`.
- `macho_monster_fair_facts` (`SKIP_UNLESS_MF()`, reading the real file): entry `0x2fdc`; `__la_symbol_ptr` at `0x5b144` with 193 slots; `__nl_symbol_ptr` 66; 205 undefined symbols; 212 external relocations; 13 `__mod_init_func` entries.

Add `SKIP_UNLESS_MF()` to `tests/test.h` next to `SKIP_UNLESS_CC()`: it skips unless `/Applications/MONSTER FAIR.app/Contents/MacOS/MONSTER FAIR` is readable.

- [x] **Step 2: Run them to see them fail**

Run: `cmake --build build 2>&1 | tail -5`
Expected: `'macho.h' file not found`.

- [x] **Step 3: Write `macho.c`**

Parse with bounds checks on every offset (the style of `pef.c`). Accept `LC_SEGMENT` (1), `LC_SYMTAB` (2), `LC_DYSYMTAB` (0xb), `LC_LOAD_DYLIB` (0xc), `LC_LOAD_DYLINKER` (0xe), `LC_UNIXTHREAD` (5; PPC_THREAD_STATE, srr0 is the first word), and skip the rest. An `nlist` is 12 bytes: `n_strx` u32, `n_type` u8, `n_sect` u8, `n_desc` u16, `n_value` u32. A relocation is 8 bytes: `r_address` i32, then `r_symbolnum:24 r_pcrel:1 r_length:2 r_extern:1 r_type:4`. Keep only external relocations with `r_extern=1`, `r_length=2`, `r_pcrel=0`, `r_type=0`, and refuse any other kind by name.

- [x] **Step 4: Run the tests**

Run: `cmake --build build && ./build/loony_tests macho_`
Expected: all pass (`macho_monster_fair_facts` skips without the game).

- [x] **Step 5: Commit**

```bash
git add src/macho.h src/macho.c tests/macho_build.h tests/test_macho.c tests/test.h
git commit -m "Read Mach-O executables: the PowerPC slice, its segments, symbols and relocations"
```

---

### Task 2: A memory layout for Mach-O programs

**Files:**
- Modify: `src/guest_mem.h`, `src/guest_mem.c`, `src/memmgr.c` (heap bounds from `gm_heap_base()`/`gm_heap_size()`), `src/trap.c` (the low-memory watch is PEF-only)
- Test: `tests/test_guest_mem.c`, `tests/test_memmgr.c`

**Interfaces:**
- Produces:
  ```c
  typedef enum { GM_LAYOUT_PEF, GM_LAYOUT_MACHO } gm_layout;
  void gm_init_layout(gm_layout layout);   /* gm_init() is gm_init_layout(GM_LAYOUT_PEF) */
  gm_layout gm_current_layout(void);
  uint32_t gm_heap_base(void);
  uint32_t gm_heap_size(void);
  ```
  PEF: unchanged (low memory `0`-`0x10000`, image `0x100000`-`0x1000000`, heap `0x1000000` + 64 MB).
  Mach-O: `0`-`0x1000` unmapped, image `0x1000`-`0x100000`, heap `0x10000000` + 256 MB. Stack, trap and tag addresses are the same in both.

- [x] **Step 1: Failing tests:** `gm_pef_layout_is_unchanged` (every region's base, size and protection as today); `gm_macho_layout_leaves_page_zero_unmapped` (`gm_is_backed(0, 4)` is false, `gm_is_backed(0x1000, 4)` is true); `gm_macho_heap_is_256_mb`; `mm_allocates_in_the_macho_heap`.
- [x] **Step 2: Implement.** Backing memory is allocated per region, so the bigger heap costs only what the game touches (allocate with `mmap(MAP_ANON)` so untouched pages stay unbacked on the host). `memmgr.c` reads the heap bounds from `gm_heap_*` instead of the macros.
- [x] **Step 3: Run** `./build/loony_tests gm_ mm_ run_` and confirm the classic runs still match their goldens.
- [x] **Step 4: Commit** `A memory layout for Mach-O programs: page zero unmapped, a bigger heap`.

---

### Task 3: Loading and binding a Mach-O program

**Files:**
- Modify: `src/loader.h`, `src/loader.c`
- Modify: `src/trap.h`, `src/trap.c` (`trap_set_direct_calls`)
- Test: `tests/test_loader.c`, `tests/test_trap.c`

**Interfaces:**
- Consumes: `macho_parse` (Task 1), `gm_init_layout` (Task 2).
- Produces:
  ```c
  typedef enum { IMAGE_PEF, IMAGE_MACHO } image_kind;
  /* loaded_image gains: */
      image_kind kind;
      uint32_t nnames;          /* imports, then the synthetic ones */
      const char **names;       /* for trap_init */
      uint32_t main_addr;       /* Mach-O: main's code address */
      uint32_t ninit;           /* Mach-O: __mod_init_func entries, in order */
      uint32_t *init_addrs;
      uint32_t text_base, text_len;  /* for code+0xNNNNN in crash reports */

  /* Loads a Mach-O executable at its linked addresses. Requires
     gm_init_layout(GM_LAYOUT_MACHO). */
  bool image_load_macho(const uint8_t *buf, size_t len, loaded_image *img, char *err, size_t errlen);

  /* What a Mach-O import is: called once for each import that a non-lazy
     pointer or an external relocation names (lazy pointers are always
     functions). Returns the guest address of a data object,
     IMAGE_SYMBOL_CODE (1) for a function whose address the program takes,
     or 0 if the shim doesn't know the symbol (the load then fails). */
  #define IMAGE_SYMBOL_CODE 1u
  typedef uint32_t (*image_data_fn)(const char *name);
  void image_set_data_resolver(image_data_fn fn);

  /* trap.h: with direct calls on, guest_call's first argument is a code
     address (Mach-O); off, a transition vector (PEF, the default). */
  void trap_set_direct_calls(bool on);
  ```

Binding rules:
1. Copy each segment's file bytes to its `vmaddr`, zero the rest of `vmsize`. `__PAGEZERO` and `__LINKEDIT` are not copied.
2. Build the name table: every undefined external symbol, then the synthetic imports the runtime needs (`sprintf`, for `dlsym`; see Task 4). Import `i` traps at `GUEST_TRAP_ADDR(i)`.
3. For each `S_LAZY_SYMBOL_POINTERS` and `S_NON_LAZY_SYMBOL_POINTERS` slot: a local or absolute indirect entry is left alone. A function import gets its trap address. A data import gets the resolver's address. An import behind a lazy pointer is a function; for any other, the resolver says: a data address, `IMAGE_SYMBOL_CODE` for a function (`__cxa_pure_virtual`, `__gxx_personality_v0`), or 0, which fails the load by name. (Measured: MONSTER FAIR asks about exactly 12 names: the 7 non-lazy imports in the Facts table, `__CFConstantStringClassReference`, the three type-info vtables, and `__cxa_pure_virtual`.)
4. For each external relocation: `word += address of the symbol` (trap address for a function such as `__cxa_pure_virtual`, resolver address for data). The stored word is the addend (`0x8` for the type-info vtables).
5. The `__dyld` section's two words point at a trap named `dyld_stub_binding_helper` that crashes ("a lazy pointer was not bound"). Nothing should reach it. The loader only binds it; `libc_register` (Task 4) installs the crashing handler.
8. Crash reports print a Mach-O code address as `code+<linked address>` (`code_base` 0, `code_len` the end of `__TEXT`), so they match a disassembly of the file.
6. `main`: from `entry`, decode the first `bl` (opcode 18, LK=1) to reach `_start`; in `_start`, find the first `bl` whose target is the `_exit` stub, and take the target of the `bl` before it. If either step fails, the load fails with "can't find main".
7. `__mod_init_func`: copy its words to `init_addrs`.

- [x] **Step 1: Failing tests**, on `macho_build.h` files: `macho_load_places_segments_at_their_addresses`; `macho_load_binds_lazy_pointers_to_traps`; `macho_load_binds_data_imports_through_the_resolver`; `macho_load_leaves_local_indirect_slots_alone`; `macho_load_adds_at_external_relocations` (addend 8 + address); `macho_load_fails_naming_an_unknown_data_symbol`; `macho_load_finds_main_after_start`; `macho_load_lists_static_initializers`. On the real file (`SKIP_UNLESS_MF`): `macho_load_monster_fair` (main `0x41ca8`, 13 initializers, slot `0x5b144` holds a trap address, `0x5d2c8` holds the class-reference object's address).
- [x] **Step 2: Failing tests for direct calls:** `guest_call_direct_jumps_to_the_address` (a two-instruction function `li r3,42; blr` at a scratch address returns 42 with direct calls on); `guest_call_default_still_reads_a_tvector`.
- [x] **Step 3: Implement.** In `guest_call`, with direct calls on, `code = fn`, and `r12 = fn` (GCC's Darwin code expects the callee address in r12 when called through a pointer); `r2` is left as is.
- [x] **Step 4: Run** `./build/loony_tests macho_load guest_call loader_`.
- [x] **Step 5: Commit** `Load Mach-O programs at their addresses and bind their imports to traps`.

---

### Task 4: The C library

**Files:**
- Create: `src/libc.h`, `src/libc.c`
- Test: `tests/test_libc.c` (through `harness.h`'s `call_import`, with direct calls on)

**Interfaces:**
- Produces: `void libc_init(const char *exe_path); void libc_register(void); uint32_t libc_data_symbol(const char *name);` (the last is Task 3's resolver for libSystem's data). Also `libc_main_args(&argv, &envp, &apple)`, which Task 6 uses to write `main`'s arguments into the heap. `libc_init` allocates the data objects, so it runs after `mm_init` and before `image_load_macho`.
- `misc` gains `misc_unix_time` (the `GetDateTime` calendar in Unix time), `misc_sleep_us` (`Delay`'s loop in microseconds) and `misc_exit` (what `ExitToShell` does, with a status). `trap` gains `trap_find(name)`. `localtime` is UTC on the fixed clock, so runs match in any time zone.

The calls MONSTER FAIR imports from libSystem, and what each does:

| Calls | Behavior |
|---|---|
| `malloc`, `calloc`, `free` | The Memory Manager's pointer heap. `malloc(0)` returns a unique pointer; `free(0)` does nothing |
| `memcpy`, `memmove`, `memset`, `strlen`, `strcmp`, `strcpy`, `strncpy`, `strcat` | On guest memory through `gm_ptr`, which crashes on unbacked ranges |
| `qsort` | A host merge sort calling the guest comparator with `guest_call` (stable, so runs are deterministic) |
| `rand`, `srand` | Darwin 10.4's algorithm, copied from Apple's Libc source (Step 2), so the game's random choices match a real Mac's for the same seed |
| `time`, `localtime`, `strftime` | Seconds from `misc`'s clock (fixed under `LOONY_FIXED_CLOCK`). `localtime` fills a static 44-byte guest `struct tm` (9 ints, `tm_gmtoff`, `tm_zone`). `strftime` formats with the host's in the C locale |
| `usleep` | Advances or waits on `misc`'s clock, and runs the idle hook, as `Delay` does |
| `exit`, `abort` | `exit` goes the way of `ExitToShell` (the exit hook, then quit); `abort` crashes |
| `__maskrune`, `__toupper` | From the rune table below |
| `dlopen`, `dlsym` | `dlopen` returns a fixed handle. `dlsym(h, "sprintf$LDBL128")` and `dlsym(h, "sprintf")` return the synthetic `sprintf` trap; anything else returns 0 and logs |
| `NSIsSymbolNameDefinedWithHint`, `NSLookupAndBindSymbolWithHint`, `NSAddressOfSymbol` | The same table as `dlsym`, through a symbol handle |
| `sprintf` (synthetic) | `%d %i %u %x %X %c %s %%` with flags, width and precision. Arguments from `trap_arg(2)` on. Any other conversion crashes naming the format |
| `_keymgr_get_and_lock_processwide_ptr`, `_keymgr_set_and_unlock_processwide_ptr`, `_init_keymgr`, `__keymgr_dwarf2_register_sections` | A small key→pointer table; the rest do nothing |
| `_dyld_register_func_for_add_image`, `_dyld_register_func_for_remove_image` | Do nothing |

Data symbols from `libc_data_symbol`: `errno` (a word), `_DefaultRuneLocale` (a 32-bit `_RuneLocale`: `__runetype[256]` at offset 52, `__maplower` at 1076, `__mapupper` at 2100, filled from the host's C-locale table), `__keymgr_global`, `mach_init_routine` and `_cthread_init_routine` (words holding 0).

- [x] **Step 1: Failing tests**, one per row, for example: `libc_malloc_and_free_use_the_pointer_heap`, `libc_qsort_calls_the_guest_comparator` (a tiny PPC comparator built with `tests/ppc.h`), `libc_time_follows_the_fixed_clock`, `libc_localtime_fills_a_32_bit_tm`, `libc_strftime_formats_a_high_score_date` (`"%Y-%m-%d %X"`), `libc_maskrune_matches_the_host`, `libc_dlsym_finds_sprintf_ldbl128`, `libc_sprintf_formats_the_games_conversions` (`"%03d,"`, `"%d00"`, `"%6d"`, `"%s"`), `libc_sprintf_refuses_floats`, `libc_rand_is_darwins`.
- [x] **Step 2: Implement.** Check Darwin's 10.4 `rand` in Apple's Libc source before writing it, and note the source in a comment.
- [x] **Step 3: Run** `./build/loony_tests libc_`.
- [x] **Step 4: Commit** `A C library for Mach-O games, on the shim's heap and clock`.

---

### Task 5: The C++ runtime

**Files:**
- Create: `src/cxxrt.h`, `src/cxxrt.c`
- Test: `tests/test_cxxrt.c`

**Interfaces:**
- Produces: `void cxxrt_init(void); void cxxrt_register(void); uint32_t cxxrt_data_symbol(const char *name);` (`cxxrt_init` allocates the vtables after `mm_init`; `cxxrt_data_symbol` also answers `IMAGE_SYMBOL_CODE` for `__cxa_pure_virtual` and `__gxx_personality_v0`). `trap_format_addr` is now public, for the throw site.

| Calls | Behavior |
|---|---|
| `_Znwm`, `_Znam` (`new`, `new[]`) | Pointer heap; crash on failure (the game doesn't catch `bad_alloc`) |
| `_ZdlPv`, `_ZdaPv` | Free; null does nothing |
| `__cxa_guard_acquire`, `__cxa_guard_release` | Single-threaded: acquire returns 1 if the guard's first byte is 0; release sets it to 1 |
| `__cxa_pure_virtual` | Crash: "pure virtual function called" |
| `__cxa_allocate_exception` | Pointer heap, with a header like libstdc++'s in front so `__cxa_begin_catch` would find it |
| `__cxa_throw(obj, tinfo, dest)` | Crash: "the game threw RT::TOSException at code+0x…" (the name is the C string at `tinfo + 4`; also log the object's first 16 bytes, which hold the error codes the game prints) |
| `__cxa_rethrow`, `_Unwind_Resume` | Crash the same way |
| `__cxa_begin_catch`, `__cxa_end_catch` | Crash; nothing can reach them without a throw |
| `__gxx_personality_v0` | A trap address; crash if called |

Data symbols: the three type-info vtables (`_ZTVN10__cxxabiv117__class_type_infoE`, `…120__si_class_type_infoE`, `…121__vmi_class_type_infoE`), 64 zeroed bytes each. The game uses them only as identity (RTTI for exception matching); it imports no `__dynamic_cast`.

- [x] **Step 1: Failing tests:** `cxxrt_new_and_delete_use_the_pointer_heap`, `cxxrt_guard_runs_once`, `cxxrt_throw_crashes_naming_the_type` (run as a child process, like the other crash tests, and check stderr), `cxxrt_pure_virtual_crashes`.
- [x] **Step 2: Implement, run** `./build/loony_tests cxxrt_`.
- [x] **Step 3: Commit** `A C++ runtime for Mach-O games; exceptions crash with their type`.

---

### Task 6: Starting MONSTER FAIR, and running it to its first missing call

This is a measuring task, like Plan 1's Task 9. It ends with facts, not a working game.

**Files:**
- Modify: `src/game.h`, `src/game.c` (kinds and bundle paths)
- Modify: `src/main.c` (the Mach-O startup), `src/misc.c` (`sysv` by kind)
- Modify: `src/rsrc.c` (open with no resources: every lookup fails as if absent)
- Test: `tests/test_game.c`, `tests/test_run.c`

**Interfaces:**
- Produces:
  ```c
  typedef enum { GAME_PEF_FOLDER, GAME_MACHO_BUNDLE } game_kind;
  typedef struct {
      const char *id, *title, *folder_name, *exe;
      game_kind kind;   /* for a bundle, folder_name is "MONSTER FAIR.app" and exe
                           is "Contents/MacOS/MONSTER FAIR" */
  } game_info;
  ```
  Row: `{"monster-fair", "MONSTER FAIR", "MONSTER FAIR.app", "Contents/MacOS/MONSTER FAIR", GAME_MACHO_BUNDLE}`.
  `misc_set_system_version(uint32_t)`, set to `0x104B` for Mach-O games.

Startup for a bundle, in `main.c`: `gm_init_layout(GM_LAYOUT_MACHO)`, `cpu_init`, `image_set_data_resolver` (a function asking `libc_data_symbol`, `cxxrt_data_symbol` and `cf_data_symbol` in turn; Task 7 adds the last), `image_load_macho`, `rsrc_open_empty`, `trap_set_direct_calls(true)`, then the same services as today plus `libc_init`/`libc_register` and `cxxrt_register`. Then `guest_call` each initializer, write `argv`, `envp` and `apple` into the heap, and call `main(1, argv, envp, apple)`. `files_init` gets the bundle as the game folder.

- [x] **Step 1: Failing tests:** `game_table_knows_three_games`; `game_installed_finds_a_bundle`; `game_in_folder_finds_monster_fair_in_its_bundle`; `run_monster_fair_reaches_main` (`SKIP_UNLESS_MF`, `LOONY_TRACE=imports`, expects the log line `loaded … main at 0x41ca8` and a `Gestalt` call).
- [x] **Step 2: Implement.**
- [x] **Step 3: Measure.** Run with `LOONY_TRACE=imports LOONY_FIXED_CLOCK=1 LOONY_DATA_DIR=<empty> SDL_VIDEO_DRIVER=dummy ./build/loony "/Applications/MONSTER FAIR.app"` until the first unimplemented import, then with `LOONY_STUB=all` to see further. Record in this plan's Facts table:
  - the `Gestalt` selectors and what `main` does with the answers;
  - the order of the first 50 imports;
  - the window `CreateNewWindow` asks for (class, attributes, bounds), or whether it captures the display, and the width, height and depth it uses with no preferences;
  - which nib windows it creates and in what order;
  - whether anything throws (the `__cxa_throw` crash), and if so, from where. **If it throws during a normal start, stop and tell the user before going on**, since Task 12 then becomes required;
  - which files it opens, with which permissions.
- [x] **Step 4: Commit** the code and the updated Facts table: `Start MONSTER FAIR: find main, run its initializers, and measure its first calls`.

---

### Task 7: Bundles, URLs and constant CFStrings

**Files:**
- Modify: `src/cf.h`, `src/cf.c`
- Test: `tests/test_cf.c`

Calls: `CFBundleGetMainBundle`, `CFBundleCopyResourcesDirectoryURL`, `CFBundleCopyResourceURL(bundle, name, type, subdir)`, `CFURLCreateCopyAppendingPathComponent`, `CFURLGetFileSystemRepresentation`, and constant CFStrings in `__cfstring`.

- `cf_set_bundle(const char *bundle_path)` from `main.c`.
- CFURLs are a new `cf_obj` kind holding a host path. `CFURLGetFileSystemRepresentation` writes it as UTF-8 and returns false if it doesn't fit.
- `cf_data_symbol("__CFConstantStringClassReference")` returns a 16-byte block. Every `cf.c` entry point that takes a CFString first checks whether the argument's first word is that block's address, and if so reads the string from its pointer and length. `CFRelease` and `CFRetain` on one do nothing.
- `kCFPreferencesCurrentApplication` is in `cf_data_symbol` for a Mach-O game (Task 6). For the classic games `main.c` still writes it into the PEF import's data slot, unchanged, since PEF data imports don't go through the resolver.
- `cf_string_text(call, ref)` reads a CFString object or a constant string, and `cf_url_path(call, ref)` a CFURL's path; Tasks 9 and 10 use both. `CFBundleCopyResourceURL` also looks in `English.lproj`, and returns NULL for a missing resource.

- [x] **Step 1: Failing tests:** `cf_bundle_resources_url_is_the_bundles`, `cf_resource_url_names_a_file_in_resources`, `cf_url_appends_a_component`, `cf_file_system_representation_is_utf8`, `cf_constant_strings_read_from_guest_memory`, `cf_release_ignores_a_constant_string`.
- [x] **Step 2: Implement, run** `./build/loony_tests cf_`.
- [x] **Step 3: Commit** `CFBundle and CFURL for a bundle game, and its constant CFStrings`.

---

### Task 8: Forks and FSRefs

**Files:**
- Modify: `src/files.h`, `src/files.c`
- Test: `tests/test_files.c`

Calls: `FSPathMakeRef`, `FSGetDataForkName`, `FSOpenFork`, `FSGetForkSize`, `FSGetForkPosition`, `FSSetForkPosition`, `FSCloseFork`. Fork refnums are the same refnums `PBReadSync`, `FSWrite`, `SetEOF`, `SetFPos`, `GetFPos`, `GetEOF` and `FSClose` already take, because the game mixes them.

- An `FSRef` (80 opaque bytes) holds an index into a table of host paths. `FSPathMakeRef` resolves a path inside the bundle or the save folder (the save folder first, as `files.c` does now) and returns `fnfErr` for a missing file or a path elsewhere.
- `FSGetDataForkName` writes an empty `HFSUniStr255`.
- `FSOpenFork` with write permission on a file in the bundle copies it to the save folder at the first write, as `FSpOpenDF` already does (done: the game can't tell this from copying at open, and it shares the code). `FSPathMakeRef` also accepts a path in the save folder, which names the same file as the bundle path.

- [x] **Step 1: Failing tests:** `files_path_make_ref_finds_a_resource`, `files_path_make_ref_refuses_outside_paths`, `files_open_fork_reads_with_pbreadsync`, `files_fork_size_and_position`, `files_writing_a_bundle_file_writes_the_save_folders_copy`.
- [x] **Step 2: Implement, run** `./build/loony_tests files_`.
- [x] **Step 3: Commit** `FSRefs and forks, sharing refnums with the File Manager calls`.

---

### Task 9: Windows and the display

**Files:**
- Modify: `src/qd.h`, `src/qd.c` (`qd_resize_screen`, the window calls), `src/display.c` (a screen whose size changes)
- Create: `src/cgdisplay.c`, `src/cgdisplay.h`
- Test: `tests/test_qd.c`, `tests/test_cgdisplay.c`

Calls: `CreateNewWindow`, `ChangeWindowAttributes`, `SetWindowTitleWithCFString` (logged; the host title stays the game's), `DisposeWindow`, `RepositionWindow`, `FlushEvents`, `NewHandle`, `ReallocateHandle`, and the CoreGraphics display calls: `CGMainDisplayID`, `CGDisplayPixelsWide`, `CGDisplayPixelsHigh`, `CGDisplayBytesPerRow`, `CGDisplayBaseAddress`, `CGDisplayCurrentMode`, `CGDisplayBestModeForParameters`, `CGDisplaySwitchToMode`, `CGDisplayCapture`, `CGDisplayIsCaptured`, `CGDisplayRelease`, `CGDisplayHideCursor`, `CGDisplayShowCursor`.

- The emulated screen is the main display. `CGDisplayBaseAddress` and `CGDisplayBytesPerRow` describe `qd_screen`'s pixels, so a game drawing there draws on screen.
- A mode is a CFDictionary (`cf.c` gains read-only dictionaries) with `Width`, `Height`, `BitsPerPixel` and `RefreshRate`. `CGDisplayBestModeForParameters` returns the requested size and sets `*exactMatch` to true.
- `CGDisplaySwitchToMode` and the first `CreateNewWindow` call `qd_resize_screen(w, h, depth)`, which reallocates the screen's pixels and PixMap in place (the GDevice and port addresses stay the same) and marks it dirty. The SDL window keeps its size and letterboxes.
- Use Task 6's measurements for the sizes and depths to test.

- [ ] **Step 1: Failing tests:** `qd_resize_screen_keeps_the_device`, `qd_create_new_window_sizes_the_screen`, `cgdisplay_base_address_is_the_screen`, `cgdisplay_best_mode_is_what_was_asked`, `cgdisplay_capture_and_release_nest`.
- [ ] **Step 2: Implement, run** `./build/loony_tests qd_ cgdisplay_ run_` (the classic goldens must still match).
- [ ] **Step 3: Commit** `Windows and a CoreGraphics main display on the emulated screen`.

---

### Task 10: Nib windows, standard alerts and the dialog image

**Files:**
- Create: `src/nib.h`, `src/nib.c` (reads `objects.xib`)
- Modify: `src/dialogs.h`, `src/dialogs.c` (a dialog from an item list; command IDs), `src/events.c` (`kEventClassCommand` dispatch, the app-modal loop)
- Create: `src/cgimage.c`, `src/cgimage.h` (PNG through ImageIO)
- Modify: `CMakeLists.txt` (link `-framework ImageIO -framework CoreGraphics`)
- Test: `tests/test_nib.c`, `tests/test_dialogs.c`, `tests/test_run.c`

Calls: `CreateNibReference`, `CreateWindowFromNib`, `DisposeNibReference`, `HIViewGetRoot`, `HIViewFindByID`, `HIViewSetVisible`, `GetControlByID`, `GetControlData`, `HIImageViewSetImage`, `HIImageViewSetOpaque`, `HIImageViewSetAlpha`, `HIImageViewSetScaleToFit`, `RunAppModalLoopForWindow`, `QuitAppModalLoopForWindow`, `CreateStandardAlert`, `RunStandardAlert`, `CGDataProviderCreateWithURL`, `CGDataProviderRelease`, `CGImageCreateWithPNGDataProvider`, `CGImageRelease`.

- `nib.c` reads only what the game's nib uses: the `nameTable`, and for each `IBCarbonWindow` its `title`, `windowRect` and root control's children. A child is an `IBCarbonButton` (`title`, `command`, `viewFrame`, `buttonType`), `IBCarbonStaticText` (`title`, with `&#10;` as a line break), `IBCarbonEditText` (`controlSignature`, `controlID`), `IBCarbonImageView` (`controlSignature`, `controlID`) or `IBCarbonIcon`. It's a small hand-written reader for this XML, not a general parser, and it refuses anything else by name. Menus are skipped.
- `dialogs.c` gains `dialogs_open_items(...)`, so nib windows and DLOG dialogs share drawing and input. The default button is the one whose command is `'ok  '`; Esc or Cmd-. press the one whose command is `'not!'`.
- A click (or Return, or Esc) sends `kEventClassCommand`/`kEventCommandProcess` with an `HICommand` (`attributes` 0, `commandID`, and the menu fields zeroed) to the window's target. The game's handler calls `QuitAppModalLoopForWindow`, which ends `RunAppModalLoopForWindow`.
- `GetControlData` on an edit text with `kControlEditTextCFStringTag` (`'cfst'`) returns a new CFString; with `kControlEditTextTextTag` (`'text'`), bytes.
- The image view draws the decoded `appl.png`, scaled to its frame.
- `CreateStandardAlert`/`RunStandardAlert` draw like `Alert`, from the CFStrings given. This is how the game reports an "Unexpected operating system error".
- `LOONY_AUTO_ALERTS=1` answers nib windows with their `'ok  '` button too.

*Progress (2026-10-08):* done and tested: `nib.c` (reads all six of the game's windows), `cgimage.c` (decodes `appl.png`), `events_send_command`/`events_forget_window` (the window-target mapping now also works for Mach-O heap addresses), and `qd_new_window` with a show/hide/dispose/reposition hook, plus `DisposeWindow` and `RepositionWindow`. Left: the nib-window and standard-alert calls in `dialogs.c` (`CreateNibReference` … `RunStandardAlert`, `GetControlByID`, `GetControlData`, the `HIView*`/`HIImageView*` calls), registering `cgimage`, and the two run tests. Task 10 is done before Task 9, because the game window's size can only be measured after the Welcome window.

- [ ] **Step 1: Failing tests:** `nib_reads_the_welcome_window` (from a copy of the window's XML written by the test, not from the game); `nib_refuses_an_unknown_control`; `dialogs_nib_button_sends_its_command`; `dialogs_nib_edit_text_returns_a_cfstring`; `cgimage_decodes_a_png` (a PNG the test writes with `png.c`); and with the game (`SKIP_UNLESS_MF`): `run_monster_fair_shows_the_welcome_window` (screenshot at a tick from Task 6, checked against a golden the user approves); `run_monster_fair_wrong_key_code_shows_authorize_failed`.
- [ ] **Step 2: Implement, run** `./build/loony_tests nib_ dialogs_ cgimage_ run_`.
- [ ] **Step 3: Commit** `Nib windows and standard alerts, drawn like the classic dialogs`.

---

### Task 11: Running to a game

**Files:** whatever the measurements name.

- [ ] **Step 1:** Run from an empty save folder with `LOONY_AUTO_ALERTS=1` and a script: Esc, Esc, Return, Return, then plunger and flippers, as Plan 8 did for Crystal Caliburn. Fix each missing or wrong call as its own small commit, with a unit test for each.
- [ ] **Step 2:** Check the sound: the game uses the Sound Manager with `snda.bin` and `sndf.bin` (8-bit samples). `LOONY_WAV=out.wav` should record its music and effects. Its callbacks now come through direct calls.
- [ ] **Step 3:** Add `run_monster_fair_plays_its_opening_headless` and `run_monster_fair_starts_a_game` (`SKIP_UNLESS_MF`), modeled on Crystal Caliburn's.
- [ ] **Step 4:** Record what was measured in the Facts table, and commit.

---

### Task 12 (contingent): C++ exceptions

Only if Task 6 or 11 shows a throw that a real Mac would catch during normal play. Otherwise, write "not needed" here with the evidence and skip it.

The design, if needed: a host-side two-phase unwinder over the guest's registers. Parse the CIEs and FDEs in `__eh_frame` (PowerPC DWARF register numbers: r0 to r31, f0 to f31 as 32 to 63, LR 65, CR 70), interpret the CFA programs, and use the LSDA in `__gcc_except_tab` for `__gxx_personality_v0` (call sites, actions, type tables compared by `type_info` address). `__cxa_begin_catch`, `__cxa_end_catch`, `__cxa_rethrow` and `_Unwind_Resume` then work for real. This is a plan of its own, written when needed.

---

### Task 13: A picker for three games

**Files:**
- Modify: `src/picker.c`, `src/picker.h`, `tests/test_picker.c`

- Three cards in a row on the 800×600 picker, each about 240 wide. A classic game's PICT 800 is scaled to fit its card. MONSTER FAIR's card shows `appl.png` (through `cgimage.c`) centered above its title.
- Two installed games keep today's two-card layout, so the existing picker golden still matches.
- [ ] **Step 1: Failing tests:** `picker_draws_three_cards`, `picker_left_and_right_wrap_across_three`, `picker_draws_an_icon_card_for_a_bundle_game`.
- [ ] **Step 2: Implement, run** `./build/loony_tests picker_ run_the_picker`.
- [ ] **Step 3: Commit** `The picker shows up to three games`.

---

### Task 14: The app, README and spec

- `tools/make_app.sh`: nothing new is bundled (ImageIO and CoreGraphics are system frameworks); check that `codesign --verify --deep --strict` still passes.
- README: MONSTER FAIR in the Play table (`/Applications/MONSTER FAIR.app`, from LittleWing's download page; drag the app itself to Applications and keep its name), the save folder `monster-fair`, `SKIP_UNLESS_MF` in the test notes.
- Spec: the second program format, the Mach-O memory layout, direct calls, the runtimes, nib windows.
- [ ] **Commit** `README and spec: MONSTER FAIR`.

---

### Task 15: An hour of MONSTER FAIR, the user's playtest, and the recordings

- [ ] **Step 1:** A 216,000-tick fixed-clock soak with `tools/soak_script.py` (adapted for MONSTER FAIR's keys): no crash, no unknown selector, a clean quit, preferences saved.
- [ ] **Step 2:** Run the soak twice from empty save folders and compare frames and `LOONY_WAV` output. They must match byte for byte (Review Focus 5).
- [ ] **Step 3:** Ask the user to play: windowed and full screen, Cmd-F, registering with their own key code (they type it; it is never logged or written anywhere but the preferences), quitting to the picker, Cmd-Q. Fix what they find.
- [ ] **Step 4:** With the user's approval, add MONSTER FAIR's golden frames and recording, and the facts the soak measured.
- [ ] **Step 5:** Commit.

## What comes next (not part of this plan)

*Mad Daedalus 1.1.9.* With this plan done, the loader, memory layout, C and C++ runtimes, bundles, URLs, constant strings and nib windows carry over. It also needs: drawing straight into the captured display (no QuickDraw), AudioToolbox (`AudioFileOpenWithCallbacks`, `AudioConverter` decoding IMA4 AIFC from `sound.dat`, an `AUGraph` whose render callback the shim pulls on the emulation thread), `mmap` of about 78 MB of data files, `pthread_mutex` (no threads, so no-ops), `CFDictionary`/`CFNumberGetValue`, `LSOpenCFURLRef`, and the Text Input Source calls.
