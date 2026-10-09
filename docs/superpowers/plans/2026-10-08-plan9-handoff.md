# Plan 9 handoff (2026-10-08)

## Update, later on 2026-10-08: read this first

Tasks 9, 10, 11 and 13 are done, and Task 12 is not needed so far. Their notes in the plan say what was built and measured. **MONSTER FAIR now plays:** the Welcome and Demo windows, the registration flow, the full-screen 1024×768×16 display, the title, the menu, a game and a clean quit. Task 13's picker shows three cards. The status table below is out of date; the plan's checkboxes are current.

Next:
- **Task 14** is done too: the full suite gave 426 passed, 0 failed, 1 skipped.
- **Task 15** is done (2026-10-09): the soak, the determinism check, the user's playtest and the goldens. The full suite gives 429 passed, 0 failed, 2 skipped (both skips need key codes in the environment). Plan 9 is complete.

Waiting on the user:
- Their `docs/test_key.txt` in the main checkout has a MONSTER FAIR block that isn't committed. The game refuses that key in play (see Task 15's notes); a test of a lasting registration needs a key it accepts, in `LOONY_TEST_MF_EMAIL` and `LOONY_TEST_MF_KEY`.

Worktree notes from this session:
- The guard refuses shell loops, heredoc appends and `sh -c`-like constructs. Put helper scripts in the job's tmp dir and call each with plain arguments.
- A PowerPC call-site decoder lives at `<job tmp>/callsites.py` (`--range lo hi` dumps code). It is gone with the job, but it's 100 lines to rewrite if needed.
- Tag spaces added: control refs `0x0B040000` (+256·slot + item, root `0xFF`), nib refs `0x0B080000`, display modes `0x0C800000`.

Where the work on `docs/superpowers/plans/2026-10-06-plan9-monster-fair.md` (MONSTER FAIR) stands. Read the plan first. Its Facts table, the Task 3 to 8 notes and the Task 10 "Progress" note are up to date. This file adds what the plan doesn't say: how to work in this repo, and the detailed design for the rest of Task 10.

## Where the code is

- Branch `origin/plan9-monster-fair`, at the commit that adds this file. Everything is pushed.
- The previous agent worked in a worktree, `.claude/worktrees/plan9`, on a local branch called `plan9-monster-fair-wt`, and pushed with `git push origin HEAD:plan9-monster-fair`. The user's main checkout has `plan9-monster-fair` checked out, so a worktree can't use that branch name. Keep pushing to `plan9-monster-fair`, and never to `main`.
- The user's own instructions: never use em dashes, in code comments, commit messages or anything else. Run each `gh` command as its own Bash call.

## Status

| Task | State |
|---|---|
| 1 Mach-O reader | done (`src/macho.c`, `tests/macho_build.h` builds test files) |
| 2 Mach-O memory layout | done (`gm_init_layout`, heap 256 MB at `0x10000000`) |
| 3 Loader and direct calls | done (`image_load_macho`, `trap_set_direct_calls`) |
| 4 C library | done (`src/libc.c`) |
| 5 C++ runtime | done (`src/cxxrt.c`) |
| 6 Startup and measuring | done. `main.c` has the bundle startup. The run stops at `CreateNibReference`, call 252 |
| 7 CFBundle, CFURL, constant CFStrings | done (`cf_string_text`, `cf_url`, `cf_url_path`, `cf_set_bundle`) |
| 8 FSRefs and forks | done (`src/files.c`) |
| 10 Nib windows | **half done**, see below. Done before Task 9 on purpose |
| 9, 11, 13, 14, 15 | not started |
| 12 C++ exceptions | not needed so far: nothing throws up to the Welcome window |

Tests: `./build/loony_tests` gives 405 passed, 0 failed, 1 skipped (the skip needs `LOONY_TEST_EMAIL` and `LOONY_TEST_KEY`).

## Working in this repo

- **Build:** `cmake -S . -B build` (the default generator; Ninja isn't installed), then `cmake --build build -j8`. Debug builds have ASan and UBSan, with `-Werror`.
- **Test:** `./build/loony_tests <name-substring>` runs the matching tests. The full suite takes about 4 to 10 minutes and longer when the machine is busy, so run it with a long timeout, or in the background. A slow Crystal Caliburn run is just load (it was CPU-bound under heavy load, not hung).
- **Test limit:** `tests/test_main.c` has `MAX_TESTS 512`, and there are about 406 tests now. Raise it if new tests push past it.
- **Shell restrictions in a worktree session:** the guard refuses compound shell commands that write files (heredoc appends, `&&` chains with `$VAR`, `>>`). Use the Edit and Write tools for file changes, and keep Bash calls simple. `sed -i ''` on a single file is allowed. `timeout` isn't installed; use `perl -e 'alarm 60; exec @ARGV' cmd`.
- **Temporary files:** use `/Users/greg/.claude/jobs/bfdfbdf3/tmp/` or the new job's tmp dir, not `/tmp`.
- **Running the game headless:**
  `LOONY_TRACE=imports LOONY_FIXED_CLOCK=1 LOONY_DATA_DIR=<empty dir> SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy perl -e 'alarm 60; exec @ARGV' ./build/loony "/Applications/MONSTER FAIR.app"`.
  Add `LOONY_STUB=all` to see past missing imports. `LOONY_AUTO_ALERTS=1` answers alerts at once. `LOONY_SCRIPT=<file>` replays input (format in `src/script.h`).
- **Inspecting the binary:** the PowerPC slice is at file offset `0x1000`, 392,648 bytes. `__DATA` is at vmaddr `0x5b000`, file offset `0x5a000` within the slice. Use `python3 -I` with the path as an argument.
- **Commits:** after each task, tick its boxes in the plan (`sed -i '' '/^### Task N:/,/^### Task N+1:/s/^- \[ \]/- [x]/' <plan>`), record any deviation or measurement in the plan, commit with `Co-Authored-By: Claude <noreply@anthropic.com>`, and push.

## Conventions the code now relies on

- **Data resolvers:** `image_set_data_resolver(fn)` returns a data address, `IMAGE_SYMBOL_CODE` (1) for a function whose address is taken, or 0, which fails the load. `main.c`'s `macho_data_symbol` asks `libc_data_symbol`, then `cxxrt_data_symbol`, then `cf_data_symbol`. The game asks about exactly 12 names.
- **Bundle startup order:** `gm_init_layout(MACHO)`, `cpu_init`, `mm_init`, `cf_init`, `cf_set_bundle(dir)`, `libc_init(path)`, `cxxrt_init`, the resolver, `image_load_macho`, `rsrc_open_empty`, `misc_init`, `misc_set_system_version(MACHO)`, then the shared services. `qd_init` runs at 32 bits for Mach-O. After `trap_init`: `trap_set_direct_calls(true)`, `libc_register`, `cxxrt_register`. Then the initializers, then `main(1, argv, envp, apple)` from `libc_main_args`.
- **Crash addresses:** a Mach-O crash report prints `code+<linked address>` (code base 0), so it matches a disassembly of the file.
- **Tag spaces** (opaque IDs, never dereferenced):
  - CF `0x08000000` to `0x09000000`
  - events `0x0A000000` (window targets are `TAG_WINDOW + (addr & 0x0FFFFFFF) / 16`)
  - dialogs `0x0B000000`
  - CG images and data providers `0x0C000000`
  - Everything stays below `0x10000000`, where the Mach-O heap starts.
- **Test harness:** `harness_init_direct(names, n)` (in `tests/harness.h`) makes `call_import` jump straight to trap addresses, as a Mach-O game does.
- **Constant CFStrings** are guest structs (`isa`, flags, bytes, length). Every CF entry point that takes a string must go through `cf_string_text(call, ref)`. `CFRelease` on a constant string does nothing.

## Task 10: what's done, and the design for the rest

Already in place (commit `15c9271`):
- **`nib.c`:** `nib_read_window(xml, len, name, &nib_window, err)`. Windows are looked up through the `nameTable`. Bounds are "top left bottom right", in the window. Commands and signatures are four-character codes, and `"ok  "` keeps its spaces. Tested on all six of the game's windows: Welcome 6 controls, Register 8, Demo 7, AuthorizeFailed 3, ThankYou 3, MainWindow 0. The `Demo` window's title is "Thank you". Use the nameTable name, not the title.
- **`cgimage.c`:**
  - `cgimage_decode_png(path, &px, err)` gives big-endian xRGB flattened over white.
  - Guest calls: `CGDataProviderCreateWithURL`, `CGImageCreateWithPNGDataProvider`, `CGImageRelease`, `CGDataProviderRelease`.
  - `cgimage_get(ref)`, `cgimage_retain` and `cgimage_release` are for the image view.
  - **Not yet called from `main.c`:** add `cgimage_init()` and `cgimage_register()` there.
- **`events.c`:**
  - `events_send_command(window, cmd)` sends `kEventCommandProcess` (`'cmds'`, kind 1) to the window's target, then the application's.
  - `GetEventParameter(ev, '----', 'hcmd', …)` returns a 16-byte HICommand: attributes 0, `commandID`, `menuRef` 0, menu item index 0.
  - `events_forget_window(window)` drops a disposed window's handlers and moves the keyboard focus.
- **`qd.c`:**
  - `qd_new_window(w, h)` makes a hidden window port.
  - `qd_set_window_hook(fn)` reports `QD_WINDOW_SHOWN`, `HIDDEN`, `DISPOSED` and `REPOSITIONED` (with the method as the argument).
  - `DisposeWindow` and `RepositionWindow` are registered.

What the game does at the Welcome window (measured):
- `CreateNibReference(CFSTR("main"), &nib)`
- `CreateWindowFromNib(nib, CFSTR("Welcome"), &w)`
- `DisposeNibReference`
- `GetWindowEventTarget(w)`
- `InstallEventHandler(target, handler, 1 type, list at 0x4c7f6, …)`
- `RepositionWindow(w, NULL, 7)`. 7 is `kWindowAlertPositionOnMainScreen`, the same placement as `POS_ALERT_MAIN` in dialogs.c.
- `ShowWindow(w)`
- then, presumably, `RunAppModalLoopForWindow(w)`

The constant strings the game passes are listed in the plan's Facts table.

To write, in `dialogs.c` (it already has the drawing, the modal input sink and `run_modal`; reuse them):

1. **Nib dialogs.**
   - Give the `dialog` struct `uint32_t window` (the qd port), `bool is_nib`, `bool quit_modal`, and these item fields: `uint32_t command, signature; int32_t id; bool hidden; uint32_t image` (a CGImage ref).
   - Raise `MAX_ITEMS` if a window needs more (Register has 8).
   - Map the nib controls:
     - button → `DLG_ITEM_BUTTON`
     - static text → `DLG_ITEM_STATIC_TEXT`; `\n` in the title should force a line break, so check whether `font_wrap` honors it
     - edit text → `DLG_ITEM_EDIT_TEXT`
     - image view → a new item type that `qd_blit`s the CGImage's xRGB pixels, scaled to the item's rect (depth 32, white under it)
     - icon → draw a plain frame or nothing (AuthorizeFailed's icon has `contentResID` 0)
   - Default item: the button with command `'ok  '`, or else `buttonType` 1. Cancel item: command `'not!'`, or else `buttonType` 2.
   - Item text holds UTF-8 titles. The 8×8 font is ASCII, which is fine for the dialog strings (only the menus have "…", and menus are skipped).
2. **The nib calls.**
   - `CreateNibReference(CFStringRef name, IBNibRef *out)` reads `<bundle>/Contents/Resources/English.lproj/<name>.nib/objects.xib` and keeps the text. Get the bundle path by exporting a getter from `cf.c`; `bundle_path` there is static. Return a tag such as `DLG_TAG_BASE + 0x80000 + slot`.
   - `CreateWindowFromNib(nib, CFStringRef name, WindowRef *out)` creates the dialog and a port with `qd_new_window(width, height)` from the window rect's size. Place the dialog centered for now; `RepositionWindow` moves it. It stays hidden until `ShowWindow`.
   - `DisposeNibReference` frees the text.
   - In `dialogs_init`, register the qd window hook:
     - shown: `show(d)`
     - hidden: `hide(d)`
     - disposed: `close_dialog(d)`, but don't free the port, since qd does that
     - repositioned with method 7: `place(rect, POS_ALERT_MAIN)`; with method 1: center. If it's showing, hide and show it again.
3. **The modal loop.**
   - `RunAppModalLoopForWindow(w)`: set `G.front` and the modal sink, then loop: `events_pump()`. When `d->hit` is set, clear it and call `events_send_command(w, item.command)` from the loop body, not from inside the sink. Wait as `run_modal` does. Stop when `d->quit_modal` is set. Restore the outer front and sink afterwards.
   - `QuitAppModalLoopForWindow(w)` sets `quit_modal`. The game's command handler calls it while handling the command.
   - With `LOONY_AUTO_ALERTS=1`: log, send the default (`'ok  '`) command once, and crash if that doesn't end the loop.
   - Log every window shown and every command sent, for example `nib window Welcome: command 'ok  ' (Play Demo)`. The run tests check these lines.
4. **Controls.** A control ref is a tag that encodes the dialog slot and item index; the root view is item `0xFF`.
   - `HIViewGetRoot(window)`
   - `HIViewFindByID(root, HIViewID by value in r4 (signature) and r5 (id), HIViewRef *out)`
   - `GetControlByID(window, const ControlID *id, ControlRef *out)`; the `ControlID` is a pointer to {signature, id}
   - `HIViewSetVisible(view, Boolean)`: set `hidden`, and redraw if the dialog is showing
   - `GetControlData(control, part, tag, size, buffer, &actual)`: for an edit text, `'cfst'` writes a new `cf_string(text)` ref (4 bytes); `'text'` writes the bytes. Crash on any other tag, naming it.
   - `HIImageViewSetImage(view, CGImageRef)`: retain the new image and release the old one
   - `HIImageViewSetOpaque`, `HIImageViewSetAlpha`, `HIImageViewSetScaleToFit`: store the value or ignore it, and return 0
   - Errors are `errUnknownControl` (-30584) for a control that isn't found, and `paramErr` (-50) otherwise.
5. **Standard alerts.**
   - `CreateStandardAlert(alertType, CFStringRef error, CFStringRef explanation, const AlertStdCFStringAlertParamRec *param, DialogRef *out)`. The param record, 32-bit: `version` (4), `movable` (1), `helpButton` (1), `defaultText` (4, at offset 6? check the alignment: on PowerPC the pointers are 4-aligned, so `defaultText` is at offset 8), `cancelText`, `otherText`, `defaultButton` (2), `cancelButton` (2), `position` (2), `flags` (4). A text of `-1` means the default ("OK", "Cancel"). With a NULL param there's just "OK".
   - Lay it out as an alert: the error text, the explanation, then the buttons.
   - `RunStandardAlert(dialog, filter, DialogItemIndex *hit)` runs it modally, writes the item hit, and disposes of it. Log the text: this is how the game reports "Unexpected operating system error".
6. **Registering:** in `main.c` (both kinds, or only for Mach-O), call `cgimage_init()` with the other init calls and `cgimage_register()` after `trap_init`.
7. **Tests** (the plan lists them):
   - `dialogs_nib_button_sends_its_command`: use the harness. Build the window from the test XML in `tests/test_nib.c`, install a handler that is a tiny PowerPC function (see `tests/test_events.c` for patterns), and check that `events_send_command` reaches it.
   - `dialogs_nib_edit_text_returns_a_cfstring`
   - `run_monster_fair_shows_the_welcome_window`: take a screenshot. The golden image needs the user's approval, so produce the PNG and ask.
   - `run_monster_fair_wrong_key_code_shows_authorize_failed`: click "Enter Key-Code", type a wrong key, press Register. Never use the user's own key.
8. **Then measure again.** Run headless to the next missing import and fill in the Facts table: the game window, `CreateNewWindow` or `CGDisplayCapture`, the display size and depth, and the files the game opens. Then do Task 9 with those numbers, and Task 11.

## Calls still missing after Task 10

Task 9:
- `CreateNewWindow`
- `ChangeWindowAttributes`
- `SetWindowTitleWithCFString`
- `FlushEvents`
- `NewHandle`
- `ReallocateHandle`
- the `CGDisplay*` calls and `CGMainDisplayID`

Everything else the game imports either has a handler, or gets one in Task 10.
