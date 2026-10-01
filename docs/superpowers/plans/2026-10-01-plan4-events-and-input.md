# Plan 4: Events and Input Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the table playable. Keys go from the SDL window to the game's Carbon event handlers, Cmd-Q and window close send the quit Apple Event, and headless runs become deterministic, with a fixed clock and scripted input, so a test can start a real game and check its frames against golden hashes.

**Architecture:**
- **Keys:** `keymap.c` translates SDL scancodes to Mac virtual key codes, characters and modifier bits.
- **Event objects:** `events.c` gains event objects (opaque IDs with reference counts), a queue, and Carbon's dispatch order. That order is focus window, then application, newest handler first, stopping at the first handler that doesn't return `eventNotHandledErr`. Through it, the game's own `ReceiveNextEvent` + `SendEventToEventTarget` polling, and `RunApplicationEventLoop`, deliver keys to its handlers.
- **Polling:** `display_poll` runs on every pump and feeds SDL input in.
- **Virtual clock:** `misc` gets one that moves only when the game waits, plus `script.c` for scripted keys and screenshots. Together they make a headless run reproducible to the pixel.

**Tech Stack:** C (gnu11), clang, CMake ≥ 3.20, Unicorn 2, SDL3.

**Spec:** `docs/superpowers/specs/2026-09-30-loony-shim-design.md` (milestone 4: events and input; the Testing section's headless integration test). Plan 3 (`docs/superpowers/plans/2026-10-01-plan3-graphics-and-opening.md`) built the screen, timers and run loop.

## Global Constraints

- Repo: `~/dev/loony-shim`, branch `main`. All paths below are relative to it.
- Game files in `/Applications/Loony Labyrinth` are read-only inputs. Never write, move or modify them. Never copy them or anything extracted from them into the repo, **including screenshots**: tests compare screenshot hashes, not images.
- C11 with GNU extensions (`gnu11`), clang, `-Wall -Wextra -Werror` in every build. Debug builds (the default) add `-fsanitize=address,undefined`. Release is `-O2`.
- Dependencies come only from Homebrew: `unicorn`, `sdl3`, `cmake`, `pkg-config`.
- Unimplemented import, guest crash, or an unsupported option in an implemented call: stop with the full crash report and exit **2**. Bad command-line input or unreadable inputs (the game folder, a `LOONY_SCRIPT` file) exit **1**.
- All guest code runs on the main thread.
- No test opens a real window (the runner sets `SDL_VIDEO_DRIVER=dummy`).
- **Keyboard:** SDL scancodes are translated to Mac virtual key codes through a static table covering the full US layout. Modifiers are reported with left and right distinguished (for example `rightShiftKey`).
- **Cmd-F** toggles full screen and **Cmd-Q** quits by sending the quit Apple Event. The game never sees these two keys. **Window close** also sends the quit Apple Event.
- **Event loop:** dispatch each event to the handlers installed on the target chain (dispatcher → application → window), innermost first. Stop at the first handler that returns anything other than `eventNotHandledErr`.
- **Headless integration test:** SDL's dummy video driver, a scripted input file (key events at given ticks), and a fixed tick clock, so runs are deterministic. It dumps the emulated screen at chosen ticks and compares against screenshots the user approved. The hashes in Task 6 are provisional until the user approves the frames at handoff.

## Facts measured from the real game (the tests assert these)

| Fact | Value |
|---|---|
| Handlers on the application target | `RawKeyDownHandler` (`keyb`/1), `RawKeyUpHandler` (`keyb`/3), `RawKeyModifiersChangedHandler` (`keyb`/4), `SuspendResumeEventHandler` (`appl`/1, `appl`/2) |
| Handlers on the window target | `WindowDrawContentHandler` (`wind`/2), plus `InstallStandardEventHandler` |
| Parameters read | `'kcod'` as `typeUInt32` (`'magn'`, 4 bytes), `'kchr'` as `typeChar` (`'TEXT'`, 1 byte), `'kmod'` as `typeUInt32` |
| Activation | Every handler returns early unless a game global is set by `SuspendResumeEventHandler` on `kEventAppActivated`. A Mac sends that event at launch, so `events_init` queues it |
| How the game polls | `TSystem::PerformOSTask` calls `ReceiveNextEvent(0, NULL, timeout, true, &event)`, then `SendEventToEventTarget(event, GetEventDispatcherTarget())`, then `ReleaseEvent`, one event per call, about every 2 ticks once the attract mode starts |
| Starting a game | Esc during the self-playing demo stops it, and `CLoony::IsRunning()` becomes false. Esc again calls `Runtime_Enter` → `CLoony::Menu`, which draws MENU / START / OPTIONS / CREDITS / QUIT. Return picks START, then 1 PLAYER, and the game begins: "DEMO VERSION, TIME LEFT 93 SEC", ball 1. `KeyCheckCoinInsert`, `KeyCheckAddPlayer` and `KeyCheckQuit` all return 0 in this version |
| Default keys (ALRT 900) | Esc menu/pause, Enter plunger, Z left flipper, / right flipper, Space nudge. Configurable keys are stored in preferences as `keycode flipper left`, `keycode flipper right`, `keycode plunger` and `keycode nudge left/center/right` |
| Fixed clock | The game waits with `TickCount`/`Delay(0)` loops and polls `Microseconds` and zero-timeout `ReceiveNextEvent`. With one tick per `Delay(0)` and a one-tick backstop after 200 polls, a 2,600-tick scripted run takes about 8 s (Release) and gives byte-identical screenshots every time, in both Debug and Release |
| Vsync | `SDL_RenderPresent` waits for vsync even with the dummy driver. With vsync on, the same fixed-clock run took 53 s |
| Translation cache | Unicorn's default translation cache is 1 GB, and the process's resident size was 1.1 GB (Release). With a 64 MB cache it is 107 MB. The game's code is 280 KB |
| Golden frames | FNV-1a32 of the PNG file: tick 30 (LittleWing logo) `0xABFE3C2B`, tick 240 (title) `0x4C3A7003`, tick 1880 (menu) `0xADE78151`, tick 2500 (game started) `0xA162CB3D` |

## Decisions this plan makes

- **Carbon dispatch order, simplified.** An event sent to the dispatcher goes to the focus window (the last window target with a handler) only if it's a keyboard event, then to the application. A window target goes to the window, then the application. An unhandled quit Apple Event (`'eppc'`) falls to our standard application handler, which calls the handler `AEInstallEventHandler` recorded for `'aevt'`/`'quit'`, or exits if there is none.
- **A quit the game ignores still quits.** Three seconds (wall clock) after the user asks to quit, if the game hasn't exited, `loony` exits with status 0. Before the attract mode the game doesn't poll events at all, and closing the window must still work then.
- **Timers fire only inside `RunApplicationEventLoop` or a `ReceiveNextEvent` wait,** never re-entering a timer whose callback is still running. This replaces Plan 3's "any pump outside a timer" rule; the Plan 3 review flagged that rule as too broad.
- **`QDFlushPortBuffer` only marks the screen dirty.** The next pump presents it, at most once per tick. This fixes the double present the Plan 3 review flagged. `qd_set_present` goes away.
- **SDL events are polled on every pump,** not only when a frame is presented. This fixes another item the Plan 3 review flagged.
- **Key repeats are delivered as `kEventRawKeyRepeat`**, which the game doesn't handle, so they're ignored, as on a Mac. Modifier-only presses become `kEventRawKeyModifiersChanged`, and only when the modifier mask actually changes.
- **A right-side modifier sets both its general bit and its right-side bit** (right Shift = `shiftKey | rightShiftKey`).
- **The fixed clock (`LOONY_FIXED_CLOCK=1`) is a test and debugging tool,** not the default. In it, `GetDateTime` counts from 2003-01-01 00:00:00, so date-seeded behavior is reproducible too.
- **The translation cache is 64 MB.** That's 230 times the game's code, and it saves about 1 GB of memory.

## Review Focus

1. **The user closes the window or presses Cmd-Q while the game isn't polling events** (the opening, a long modal loop). Expect a clean exit within 3 seconds, not a window that won't close. Test in Task 4.
2. **A key goes down and up within the same frame,** or modifiers overlap (both Shifts). Expect each change to arrive as its own event, in order, and the modifier mask to track both sides. Tests in Task 4.
3. **The game releases an event it didn't pull, or asks for a parameter in the wrong type.** Expect a crash report naming the call, not corrupted event state. Tests in Task 4.
4. **A malformed `LOONY_SCRIPT` file** (unknown key, ticks out of order, missing argument). Expect exit 1 with the line number. Tests in Tasks 2 and 6.
5. **The same scripted run twice.** Expect identical frames, so a golden-hash failure always means a real behavior change. Test in Task 6.

---

### Task 1: SDL to Mac keyboard map

**Files:**
- Create: `src/keymap.h`, `src/keymap.c`
- Create: `tests/test_keymap.c`

**Interfaces:**
- Consumes: SDL3's `SDL_Scancode` values (`<SDL3/SDL_scancode.h>`).
- Produces: `KM_CMD`, `KM_SHIFT`, `KM_ALPHA_LOCK`, `KM_OPTION`, `KM_CONTROL`, `KM_RIGHT_SHIFT`, `KM_RIGHT_OPTION`, `KM_RIGHT_CONTROL`; `keymap_entry { vkey, chr, shifted, modifier }`; `keymap_entry keymap_lookup(int scancode)`; `uint8_t keymap_char(const keymap_entry *k, uint32_t modifiers)`; `int keymap_scancode_for_name(const char *name)`.

The virtual key codes come from the US ANSI layout (*Inside Macintosh: Text*): Z is 0x06, / is 0x2C, Return 0x24, Space 0x31, Esc 0x35, left Shift 0x38, right Shift 0x3C, left Command 0x37, right Command 0x36, keypad Enter 0x4C. Keys without a printable character use the classic codes: Return 0x0D, Enter 0x03, Esc 0x1B, Delete 0x08, forward delete 0x7F, arrows 0x1C–0x1F, function keys 0x10.

- [ ] **Step 1: Write the failing test**

`tests/test_keymap.c`:
```c
#include "test.h"

#include <SDL3/SDL_scancode.h>

#include "keymap.h"

TEST(keymap_game_keys) {
    keymap_entry z = keymap_lookup(SDL_SCANCODE_Z);
    CHECK_EQ(z.vkey, 0x06);
    CHECK_EQ(z.chr, 'z');
    CHECK_EQ(z.shifted, 'Z');
    CHECK_EQ(z.modifier, 0);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_SLASH).vkey, 0x2C);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_SLASH).chr, '/');
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_RETURN).vkey, 0x24);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_RETURN).chr, 0x0D);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_KP_ENTER).vkey, 0x4C);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_SPACE).vkey, 0x31);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_ESCAPE).vkey, 0x35);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_ESCAPE).chr, 0x1B);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_LEFT).chr, 0x1C);
}

TEST(keymap_modifiers_distinguish_left_and_right) {
    keymap_entry l = keymap_lookup(SDL_SCANCODE_LSHIFT), r = keymap_lookup(SDL_SCANCODE_RSHIFT);
    CHECK_EQ(l.vkey, 0x38);
    CHECK_EQ(l.modifier, KM_SHIFT);
    CHECK_EQ(r.vkey, 0x3C);
    CHECK_EQ(r.modifier, KM_SHIFT | KM_RIGHT_SHIFT);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_LGUI).modifier, KM_CMD);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_RGUI).vkey, 0x36);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_RCTRL).modifier, KM_CONTROL | KM_RIGHT_CONTROL);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_RALT).modifier, KM_OPTION | KM_RIGHT_OPTION);
}

TEST(keymap_unknown_scancodes_have_no_key) {
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_UNKNOWN).vkey, -1);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_AC_BACK).vkey, -1);
}

TEST(keymap_characters_follow_shift_and_caps_lock) {
    keymap_entry a = keymap_lookup(SDL_SCANCODE_A), one = keymap_lookup(SDL_SCANCODE_1);
    CHECK_EQ(keymap_char(&a, 0), 'a');
    CHECK_EQ(keymap_char(&a, KM_SHIFT), 'A');
    CHECK_EQ(keymap_char(&a, KM_ALPHA_LOCK), 'A');
    CHECK_EQ(keymap_char(&one, KM_ALPHA_LOCK), '1');
    CHECK_EQ(keymap_char(&one, KM_SHIFT | KM_RIGHT_SHIFT), '!');
}

TEST(keymap_script_names) {
    CHECK_EQ(keymap_scancode_for_name("slash"), SDL_SCANCODE_SLASH);
    CHECK_EQ(keymap_scancode_for_name("RETURN"), SDL_SCANCODE_RETURN);
    CHECK_EQ(keymap_scancode_for_name("rshift"), SDL_SCANCODE_RSHIFT);
    CHECK_EQ(keymap_scancode_for_name("nope"), -1);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: the build fails with `'keymap.h' file not found`.

- [ ] **Step 3: Write the implementation**

`src/keymap.h`:
```c
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* SDL scancodes to Mac virtual key codes (US layout) and character codes, and
   Carbon modifier bits. Reference: Inside Macintosh: Text, "Virtual Key
   Codes", and Carbon Events.h. */

/* Carbon modifier bits (kEventParamKeyModifiers). A right-side modifier sets
   both its general bit and its right-side bit. */
#define KM_CMD           0x0100
#define KM_SHIFT         0x0200
#define KM_ALPHA_LOCK    0x0400
#define KM_OPTION        0x0800
#define KM_CONTROL       0x1000
#define KM_RIGHT_SHIFT   0x2000
#define KM_RIGHT_OPTION  0x4000
#define KM_RIGHT_CONTROL 0x8000

typedef struct {
    int vkey;     /* Mac virtual key code, or -1 if the key has none */
    uint8_t chr;  /* Mac Roman character with no modifiers, 0 if none */
    uint8_t shifted; /* character with Shift */
    uint32_t modifier; /* nonzero for a modifier key: its KM_* bits */
} keymap_entry;

/* The mapping for an SDL scancode (SDL_Scancode values). */
keymap_entry keymap_lookup(int scancode);

/* The character a key produces with the given modifier bits. */
uint8_t keymap_char(const keymap_entry *k, uint32_t modifiers);

/* The SDL scancode for a script key name ("z", "slash", "return", "space",
   "esc", "lshift", "rshift", ...), or -1. */
int keymap_scancode_for_name(const char *name);
```

`src/keymap.c`:
```c
#include "keymap.h"

#include <SDL3/SDL_scancode.h>
#include <string.h>
#include <strings.h>

typedef struct {
    int scancode, vkey;
    uint8_t chr, shifted;
    uint32_t modifier;
    const char *name;
} row;

/* Character codes for keys without a printable character: Return 0x0D,
   keypad Enter 0x03, Tab 0x09, Escape 0x1B, Delete 0x08, forward delete
   0x7F, arrows 0x1C-0x1F, function keys 0x10. */
static const row table[] = {
    {SDL_SCANCODE_A, 0x00, 'a', 'A', 0, "a"},
    {SDL_SCANCODE_S, 0x01, 's', 'S', 0, "s"},
    {SDL_SCANCODE_D, 0x02, 'd', 'D', 0, "d"},
    {SDL_SCANCODE_F, 0x03, 'f', 'F', 0, "f"},
    {SDL_SCANCODE_H, 0x04, 'h', 'H', 0, "h"},
    {SDL_SCANCODE_G, 0x05, 'g', 'G', 0, "g"},
    {SDL_SCANCODE_Z, 0x06, 'z', 'Z', 0, "z"},
    {SDL_SCANCODE_X, 0x07, 'x', 'X', 0, "x"},
    {SDL_SCANCODE_C, 0x08, 'c', 'C', 0, "c"},
    {SDL_SCANCODE_V, 0x09, 'v', 'V', 0, "v"},
    {SDL_SCANCODE_B, 0x0B, 'b', 'B', 0, "b"},
    {SDL_SCANCODE_Q, 0x0C, 'q', 'Q', 0, "q"},
    {SDL_SCANCODE_W, 0x0D, 'w', 'W', 0, "w"},
    {SDL_SCANCODE_E, 0x0E, 'e', 'E', 0, "e"},
    {SDL_SCANCODE_R, 0x0F, 'r', 'R', 0, "r"},
    {SDL_SCANCODE_Y, 0x10, 'y', 'Y', 0, "y"},
    {SDL_SCANCODE_T, 0x11, 't', 'T', 0, "t"},
    {SDL_SCANCODE_1, 0x12, '1', '!', 0, "1"},
    {SDL_SCANCODE_2, 0x13, '2', '@', 0, "2"},
    {SDL_SCANCODE_3, 0x14, '3', '#', 0, "3"},
    {SDL_SCANCODE_4, 0x15, '4', '$', 0, "4"},
    {SDL_SCANCODE_6, 0x16, '6', '^', 0, "6"},
    {SDL_SCANCODE_5, 0x17, '5', '%', 0, "5"},
    {SDL_SCANCODE_EQUALS, 0x18, '=', '+', 0, "equals"},
    {SDL_SCANCODE_9, 0x19, '9', '(', 0, "9"},
    {SDL_SCANCODE_7, 0x1A, '7', '&', 0, "7"},
    {SDL_SCANCODE_MINUS, 0x1B, '-', '_', 0, "minus"},
    {SDL_SCANCODE_8, 0x1C, '8', '*', 0, "8"},
    {SDL_SCANCODE_0, 0x1D, '0', ')', 0, "0"},
    {SDL_SCANCODE_RIGHTBRACKET, 0x1E, ']', '}', 0, "rightbracket"},
    {SDL_SCANCODE_O, 0x1F, 'o', 'O', 0, "o"},
    {SDL_SCANCODE_U, 0x20, 'u', 'U', 0, "u"},
    {SDL_SCANCODE_LEFTBRACKET, 0x21, '[', '{', 0, "leftbracket"},
    {SDL_SCANCODE_I, 0x22, 'i', 'I', 0, "i"},
    {SDL_SCANCODE_P, 0x23, 'p', 'P', 0, "p"},
    {SDL_SCANCODE_RETURN, 0x24, 0x0D, 0x0D, 0, "return"},
    {SDL_SCANCODE_L, 0x25, 'l', 'L', 0, "l"},
    {SDL_SCANCODE_J, 0x26, 'j', 'J', 0, "j"},
    {SDL_SCANCODE_APOSTROPHE, 0x27, '\'', '"', 0, "apostrophe"},
    {SDL_SCANCODE_K, 0x28, 'k', 'K', 0, "k"},
    {SDL_SCANCODE_SEMICOLON, 0x29, ';', ':', 0, "semicolon"},
    {SDL_SCANCODE_BACKSLASH, 0x2A, '\\', '|', 0, "backslash"},
    {SDL_SCANCODE_COMMA, 0x2B, ',', '<', 0, "comma"},
    {SDL_SCANCODE_SLASH, 0x2C, '/', '?', 0, "slash"},
    {SDL_SCANCODE_N, 0x2D, 'n', 'N', 0, "n"},
    {SDL_SCANCODE_M, 0x2E, 'm', 'M', 0, "m"},
    {SDL_SCANCODE_PERIOD, 0x2F, '.', '>', 0, "period"},
    {SDL_SCANCODE_TAB, 0x30, 0x09, 0x09, 0, "tab"},
    {SDL_SCANCODE_SPACE, 0x31, ' ', ' ', 0, "space"},
    {SDL_SCANCODE_GRAVE, 0x32, '`', '~', 0, "grave"},
    {SDL_SCANCODE_BACKSPACE, 0x33, 0x08, 0x08, 0, "backspace"},
    {SDL_SCANCODE_ESCAPE, 0x35, 0x1B, 0x1B, 0, "esc"},
    {SDL_SCANCODE_RGUI, 0x36, 0, 0, KM_CMD, "rcmd"},
    {SDL_SCANCODE_LGUI, 0x37, 0, 0, KM_CMD, "lcmd"},
    {SDL_SCANCODE_LSHIFT, 0x38, 0, 0, KM_SHIFT, "lshift"},
    {SDL_SCANCODE_CAPSLOCK, 0x39, 0, 0, KM_ALPHA_LOCK, "capslock"},
    {SDL_SCANCODE_LALT, 0x3A, 0, 0, KM_OPTION, "loption"},
    {SDL_SCANCODE_LCTRL, 0x3B, 0, 0, KM_CONTROL, "lcontrol"},
    {SDL_SCANCODE_RSHIFT, 0x3C, 0, 0, KM_SHIFT | KM_RIGHT_SHIFT, "rshift"},
    {SDL_SCANCODE_RALT, 0x3D, 0, 0, KM_OPTION | KM_RIGHT_OPTION, "roption"},
    {SDL_SCANCODE_RCTRL, 0x3E, 0, 0, KM_CONTROL | KM_RIGHT_CONTROL, "rcontrol"},
    {SDL_SCANCODE_KP_PERIOD, 0x41, '.', '.', 0, "kp_period"},
    {SDL_SCANCODE_KP_MULTIPLY, 0x43, '*', '*', 0, "kp_multiply"},
    {SDL_SCANCODE_KP_PLUS, 0x45, '+', '+', 0, "kp_plus"},
    {SDL_SCANCODE_NUMLOCKCLEAR, 0x47, 0x1B, 0x1B, 0, "kp_clear"},
    {SDL_SCANCODE_KP_DIVIDE, 0x4B, '/', '/', 0, "kp_divide"},
    {SDL_SCANCODE_KP_ENTER, 0x4C, 0x03, 0x03, 0, "enter"},
    {SDL_SCANCODE_KP_MINUS, 0x4E, '-', '-', 0, "kp_minus"},
    {SDL_SCANCODE_KP_EQUALS, 0x51, '=', '=', 0, "kp_equals"},
    {SDL_SCANCODE_KP_0, 0x52, '0', '0', 0, "kp_0"},
    {SDL_SCANCODE_KP_1, 0x53, '1', '1', 0, "kp_1"},
    {SDL_SCANCODE_KP_2, 0x54, '2', '2', 0, "kp_2"},
    {SDL_SCANCODE_KP_3, 0x55, '3', '3', 0, "kp_3"},
    {SDL_SCANCODE_KP_4, 0x56, '4', '4', 0, "kp_4"},
    {SDL_SCANCODE_KP_5, 0x57, '5', '5', 0, "kp_5"},
    {SDL_SCANCODE_KP_6, 0x58, '6', '6', 0, "kp_6"},
    {SDL_SCANCODE_KP_7, 0x59, '7', '7', 0, "kp_7"},
    {SDL_SCANCODE_KP_8, 0x5B, '8', '8', 0, "kp_8"},
    {SDL_SCANCODE_KP_9, 0x5C, '9', '9', 0, "kp_9"},
    {SDL_SCANCODE_F5, 0x60, 0x10, 0x10, 0, "f5"},
    {SDL_SCANCODE_F6, 0x61, 0x10, 0x10, 0, "f6"},
    {SDL_SCANCODE_F7, 0x62, 0x10, 0x10, 0, "f7"},
    {SDL_SCANCODE_F3, 0x63, 0x10, 0x10, 0, "f3"},
    {SDL_SCANCODE_F8, 0x64, 0x10, 0x10, 0, "f8"},
    {SDL_SCANCODE_F9, 0x65, 0x10, 0x10, 0, "f9"},
    {SDL_SCANCODE_F11, 0x67, 0x10, 0x10, 0, "f11"},
    {SDL_SCANCODE_F10, 0x6D, 0x10, 0x10, 0, "f10"},
    {SDL_SCANCODE_F12, 0x6F, 0x10, 0x10, 0, "f12"},
    {SDL_SCANCODE_HOME, 0x73, 0x01, 0x01, 0, "home"},
    {SDL_SCANCODE_PAGEUP, 0x74, 0x0B, 0x0B, 0, "pageup"},
    {SDL_SCANCODE_DELETE, 0x75, 0x7F, 0x7F, 0, "delete"},
    {SDL_SCANCODE_F4, 0x76, 0x10, 0x10, 0, "f4"},
    {SDL_SCANCODE_END, 0x77, 0x04, 0x04, 0, "end"},
    {SDL_SCANCODE_F2, 0x78, 0x10, 0x10, 0, "f2"},
    {SDL_SCANCODE_PAGEDOWN, 0x79, 0x0C, 0x0C, 0, "pagedown"},
    {SDL_SCANCODE_F1, 0x7A, 0x10, 0x10, 0, "f1"},
    {SDL_SCANCODE_LEFT, 0x7B, 0x1C, 0x1C, 0, "left"},
    {SDL_SCANCODE_RIGHT, 0x7C, 0x1D, 0x1D, 0, "right"},
    {SDL_SCANCODE_DOWN, 0x7D, 0x1F, 0x1F, 0, "down"},
    {SDL_SCANCODE_UP, 0x7E, 0x1E, 0x1E, 0, "up"},
};

keymap_entry keymap_lookup(int scancode) {
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (table[i].scancode == scancode)
            return (keymap_entry){table[i].vkey, table[i].chr, table[i].shifted, table[i].modifier};
    return (keymap_entry){-1, 0, 0, 0};
}

uint8_t keymap_char(const keymap_entry *k, uint32_t modifiers) {
    bool shift = (modifiers & KM_SHIFT) != 0;
    bool caps = (modifiers & KM_ALPHA_LOCK) != 0 && k->chr >= 'a' && k->chr <= 'z';
    return (shift || caps) ? k->shifted : k->chr;
}

int keymap_scancode_for_name(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strcasecmp(table[i].name, name) == 0)
            return table[i].scancode;
    return -1;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S . -B build && cmake --build build && ./build/loony_tests keymap_`
Expected: `5 passed, 0 failed, 0 skipped`. Full suite: `201 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/keymap.h src/keymap.c tests/test_keymap.c
git commit -m "Map SDL scancodes to Mac virtual key codes and modifiers"
```

---

### Task 2: Scripted input

**Files:**
- Create: `src/script.h`, `src/script.c`
- Create: `tests/test_script.c`

**Interfaces:**
- Consumes: `keymap_scancode_for_name` (Task 1); `read_file` (`util.h`).
- Produces: `script_kind` (`SCRIPT_KEY_DOWN`, `SCRIPT_KEY_UP`, `SCRIPT_SCREENSHOT`, `SCRIPT_QUIT`), `script_action { tick, kind, scancode, path }`, `bool script_load(const char *path, char *err, size_t errlen)`, `bool script_parse(const char *text, char *err, size_t errlen)`, `bool script_next(uint32_t tick, script_action *out)`, `int script_remaining(void)`.

- [ ] **Step 1: Write the failing test**

`tests/test_script.c`:
```c
#include "test.h"

#include <SDL3/SDL_scancode.h>

#include "script.h"

TEST(script_parses_and_orders_actions) {
    char err[256] = "";
    CHECK(script_parse("# a comment\n\n10 down z\n10 up z\n  20 screenshot /tmp/x.png\n30 quit\n",
                       err, sizeof err));
    CHECK_EQ(script_remaining(), 4);
    script_action a;
    CHECK(!script_next(9, &a));
    CHECK(script_next(10, &a));
    CHECK_EQ(a.kind, SCRIPT_KEY_DOWN);
    CHECK_EQ(a.scancode, SDL_SCANCODE_Z);
    CHECK(script_next(10, &a));
    CHECK_EQ(a.kind, SCRIPT_KEY_UP);
    CHECK(!script_next(19, &a));
    CHECK(script_next(25, &a));
    CHECK_EQ(a.kind, SCRIPT_SCREENSHOT);
    CHECK_STR(a.path, "/tmp/x.png");
    CHECK(script_next(1000, &a));
    CHECK_EQ(a.kind, SCRIPT_QUIT);
    CHECK(!script_next(1000, &a));
    CHECK_EQ(script_remaining(), 0);
}

TEST(script_reports_errors_with_line_numbers) {
    char err[256] = "";
    CHECK(!script_parse("1 down z\n2 down nokey\n", err, sizeof err));
    CHECK_CONTAINS(err, "line 2: unknown key \"nokey\"");
    CHECK(!script_parse("5 jump\n", err, sizeof err));
    CHECK_CONTAINS(err, "line 1: unknown action \"jump\"");
    CHECK(!script_parse("5 down z\n4 up z\n", err, sizeof err));
    CHECK_CONTAINS(err, "line 2: tick 4 is before tick 5");
    CHECK(!script_parse("5 screenshot\n", err, sizeof err));
    CHECK_CONTAINS(err, "screenshot needs a file name");
    CHECK(!script_parse("down z\n", err, sizeof err));
    CHECK_CONTAINS(err, "line 1: expected");
    CHECK_EQ(script_remaining(), 0);
}

TEST(script_load_missing_file) {
    char err[256] = "";
    CHECK(!script_load("/nonexistent/loony.script", err, sizeof err));
    CHECK_CONTAINS(err, "can't read /nonexistent/loony.script");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: the build fails with `'script.h' file not found`.

- [ ] **Step 3: Write the implementation**

`src/script.h`:
```c
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Scripted input for headless runs (LOONY_SCRIPT). One action per line:
     <tick> down <key>        press a key (names from keymap_scancode_for_name)
     <tick> up <key>          release it
     <tick> screenshot <file> write the screen as a PNG
     <tick> quit              ask the game to quit (the quit Apple Event)
   Blank lines and lines starting with '#' are ignored. Ticks are 1/60 s
   since launch and must not decrease. */

typedef enum { SCRIPT_KEY_DOWN, SCRIPT_KEY_UP, SCRIPT_SCREENSHOT, SCRIPT_QUIT } script_kind;

typedef struct {
    uint32_t tick;
    script_kind kind;
    int scancode;       /* key actions */
    char path[256];     /* screenshot */
} script_action;

/* Parses a script file. On failure writes err (with the line number) and
   returns false. Replaces any script loaded before. */
bool script_load(const char *path, char *err, size_t errlen);

/* Parses script text (for tests). */
bool script_parse(const char *text, char *err, size_t errlen);

/* The next action due at or before tick, removed from the script. False if
   none is due. */
bool script_next(uint32_t tick, script_action *out);

/* Actions not yet taken. */
int script_remaining(void);
```

`src/script.c`:
```c
#include "script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keymap.h"
#include "util.h"

#define MAX_ACTIONS 1024

static struct {
    script_action a[MAX_ACTIONS];
    int n, next;
} SC;

bool script_parse(const char *text, char *err, size_t errlen) {
    memset(&SC, 0, sizeof SC);
    int line_no = 0;
    uint32_t last = 0;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        char line[512];
        if (n >= sizeof line) {
            snprintf(err, errlen, "line %d is too long", line_no + 1);
            return false;
        }
        memcpy(line, p, n);
        line[n] = '\0';
        p = eol ? eol + 1 : p + n;
        line_no++;
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s || *s == '#' || *s == '\r')
            continue;
        unsigned long tick;
        char verb[32], arg[256] = "";
        int got = sscanf(s, "%lu %31s %255s", &tick, verb, arg);
        if (got < 2) {
            snprintf(err, errlen, "line %d: expected \"<tick> <action> [argument]\"", line_no);
            return false;
        }
        if (tick < last) {
            snprintf(err, errlen, "line %d: tick %lu is before tick %u", line_no, tick, last);
            return false;
        }
        if (SC.n == MAX_ACTIONS) {
            snprintf(err, errlen, "more than %d actions", MAX_ACTIONS);
            return false;
        }
        script_action *a = &SC.a[SC.n];
        a->tick = (uint32_t)tick;
        if (strcmp(verb, "down") == 0 || strcmp(verb, "up") == 0) {
            a->kind = verb[0] == 'd' ? SCRIPT_KEY_DOWN : SCRIPT_KEY_UP;
            a->scancode = keymap_scancode_for_name(arg);
            if (a->scancode < 0) {
                snprintf(err, errlen, "line %d: unknown key \"%s\"", line_no, arg);
                return false;
            }
        } else if (strcmp(verb, "screenshot") == 0) {
            if (got < 3) {
                snprintf(err, errlen, "line %d: screenshot needs a file name", line_no);
                return false;
            }
            a->kind = SCRIPT_SCREENSHOT;
            snprintf(a->path, sizeof a->path, "%s", arg);
        } else if (strcmp(verb, "quit") == 0) {
            a->kind = SCRIPT_QUIT;
        } else {
            snprintf(err, errlen, "line %d: unknown action \"%s\"", line_no, verb);
            return false;
        }
        last = a->tick;
        SC.n++;
    }
    return true;
}

bool script_load(const char *path, char *err, size_t errlen) {
    size_t len;
    uint8_t *text = read_file(path, &len);
    if (!text) {
        snprintf(err, errlen, "can't read %s", path);
        return false;
    }
    char *z = realloc(text, len + 1);
    if (!z) {
        free(text);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    z[len] = '\0';
    bool ok = script_parse(z, err, errlen);
    free(z);
    return ok;
}

bool script_next(uint32_t tick, script_action *out) {
    if (SC.next >= SC.n || SC.a[SC.next].tick > tick)
        return false;
    *out = SC.a[SC.next++];
    return true;
}

int script_remaining(void) { return SC.n - SC.next; }
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S . -B build && cmake --build build && ./build/loony_tests script_`
Expected: `4 passed, 0 failed, 0 skipped` (the filter also matches `keymap_script_names` from Task 1). Full suite: `204 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/script.h src/script.c tests/test_script.c
git commit -m "Parse scripted input for headless runs"
```

---

### Task 3: A fixed clock

**Files:**
- Modify: `src/misc.h`, `src/misc.c`
- Modify: `tests/test_misc.c` (append four tests)

**Interfaces:**
- Consumes: Plan 3's `misc` module.
- Produces: `bool misc_fixed_clock(void)`, `void misc_wait(double seconds)` (sleeps on the real clock, advances the virtual one), `void misc_poll(void)` (counts a poll toward the 200-poll backstop). With `LOONY_FIXED_CLOCK=1`:
  - `Delay(0)` advances exactly one tick.
  - `Delay(n)` advances n ticks.
  - 200 `TickCount`/`Microseconds`/`misc_poll` calls in a row advance one tick.
  - `GetDateTime` counts from 2003-01-01.

Virtual waits round up to whole microseconds. A wait that rounds to 0 µs would never reach a deadline a fraction of a microsecond away, and the run loop would spin forever; this happened during development.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_misc.c`:
```diff
diff --git a/tests/test_misc.c b/tests/test_misc.c
index 0204aad..76a0ee0 100644
--- a/tests/test_misc.c
+++ b/tests/test_misc.c
@@ -218,3 +218,69 @@ TEST(misc_delay_and_tick_count_run_the_idle_hook) {
     CHECK(after_delay <= 5);
     CHECK(idle_calls - after_delay <= 2); /* TickCount only when the tick changes */
 }
+
+/* Runs with the virtual clock; restores the real one afterwards. */
+static void fixed_setup(void) {
+    setenv("LOONY_FIXED_CLOCK", "1", 1);
+    setup();
+}
+
+static void fixed_teardown(void) {
+    unsetenv("LOONY_FIXED_CLOCK");
+    misc_init();
+}
+
+TEST(misc_fixed_clock_stands_still_until_the_game_waits) {
+    fixed_setup();
+    bool fixed = misc_fixed_clock();
+    double t0 = misc_seconds();
+    struct timespec ts = {0, 20000000};
+    nanosleep(&ts, NULL);
+    double t1 = misc_seconds();
+    misc_wait(0.5);
+    double t2 = misc_seconds();
+    misc_wait(1e-9); /* rounds up to a microsecond, never to nothing */
+    double t3 = misc_seconds();
+    fixed_teardown();
+    CHECK(fixed);
+    CHECK(t0 == 0.0);
+    CHECK(t1 == 0.0);
+    CHECK(t2 == 0.5);
+    CHECK(t3 > t2);
+}
+
+TEST(misc_fixed_clock_delay_advances_whole_ticks) {
+    fixed_setup();
+    uint32_t final_ticks = scratch(4);
+    call_import("Delay", 2, 0u, final_ticks); /* one tick passes */
+    uint32_t a = call_import("TickCount", 0), fa = gm_r32(final_ticks);
+    call_import("Delay", 2, 3u, final_ticks);
+    uint32_t b = call_import("TickCount", 0);
+    fixed_teardown();
+    CHECK_EQ(a, 1);
+    CHECK_EQ(fa, 1);
+    CHECK_EQ(b, 4);
+}
+
+TEST(misc_fixed_clock_polling_alone_moves_time) {
+    fixed_setup();
+    for (int i = 0; i < 199; i++)
+        call_import("TickCount", 0);
+    uint32_t before = call_import("TickCount", 0); /* the 200th poll */
+    uint32_t us = scratch(8);
+    for (int i = 0; i < 200; i++)
+        call_import("Microseconds", 1, us);
+    uint32_t after = call_import("TickCount", 0);
+    fixed_teardown();
+    CHECK_EQ(before, 1);
+    CHECK_EQ(after, 2);
+}
+
+TEST(misc_fixed_clock_date_is_fixed) {
+    fixed_setup();
+    uint32_t secs = scratch(4);
+    call_import("GetDateTime", 1, secs);
+    uint32_t d = gm_r32(secs);
+    fixed_teardown();
+    CHECK_EQ(d, 3124224000u); /* 2003-01-01 00:00:00 */
+}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build`
Expected: the build fails with `call to undeclared function 'misc_fixed_clock'`.

- [ ] **Step 3: Write the implementation**

```diff
diff --git a/src/misc.h b/src/misc.h
index f7dd29e..ccd953c 100644
--- a/src/misc.h
+++ b/src/misc.h
@@ -5,9 +5,23 @@
 #define MISC_GESTALT_UNDEF_SELECTOR_ERR (-5551)
 #define MISC_IC_INSTANCE 0x0FFF0001u /* opaque ICInstance returned by ICStart */
 
-/* Resets the clock (TickCount starts at 0), cursor and Apple Event state. */
+/* Resets the clock (TickCount starts at 0), cursor and Apple Event state.
+   With LOONY_FIXED_CLOCK=1 the clock is virtual: it moves only when the game
+   waits (Delay, misc_wait), one tick per Delay(0), or by one tick after 200
+   time polls in a row (TickCount, Microseconds, misc_poll), so runs don't
+   depend on host speed. */
 void misc_init(void);
 
+/* True if the virtual clock is in use. */
+bool misc_fixed_clock(void);
+
+/* Waits: sleeps on the real clock, or advances the virtual one. */
+void misc_wait(double seconds);
+
+/* Notes that the game polled for time or events without waiting (see
+   LOONY_FIXED_CLOCK). */
+void misc_poll(void);
+
 /* Ticks (1/60 s) since misc_init(). */
 uint32_t misc_ticks(void);
 
```

```diff
diff --git a/src/misc.c b/src/misc.c
index 5ddeaf0..92a05ff 100644
--- a/src/misc.c
+++ b/src/misc.c
@@ -1,6 +1,7 @@
 #include "misc.h"
 
 #include <ctype.h>
+#include <math.h>
 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
@@ -23,6 +24,9 @@ static struct {
     int nae;
     misc_idle_fn idle;
     uint32_t last_idle_tick;
+    bool fixed;
+    uint64_t virtual_us;
+    int polls; /* time polls since the virtual clock last moved */
 } M;
 
 void misc_set_idle(misc_idle_fn fn) { M.idle = fn; }
@@ -32,9 +36,15 @@ void misc_init(void) {
     memset(&M, 0, sizeof M);
     M.idle = idle;
     clock_gettime(CLOCK_MONOTONIC, &M.start);
+    const char *f = getenv("LOONY_FIXED_CLOCK");
+    M.fixed = f && strcmp(f, "1") == 0;
 }
 
+bool misc_fixed_clock(void) { return M.fixed; }
+
 static uint64_t elapsed_us(void) {
+    if (M.fixed)
+        return M.virtual_us;
     struct timespec now;
     clock_gettime(CLOCK_MONOTONIC, &now);
     int64_t us = (int64_t)(now.tv_sec - M.start.tv_sec) * 1000000 +
@@ -46,6 +56,37 @@ uint32_t misc_ticks(void) { return (uint32_t)(elapsed_us() * 60 / 1000000); }
 
 double misc_seconds(void) { return (double)elapsed_us() / 1e6; }
 
+void misc_wait(double seconds) {
+    if (seconds <= 0)
+        return;
+    if (M.fixed) {
+        /* Round up: a wait that rounds to 0 us would never reach its deadline. */
+        uint64_t us = (uint64_t)ceil(seconds * 1e6);
+        M.virtual_us += us ? us : 1;
+        M.polls = 0;
+        return;
+    }
+    struct timespec ts = {(time_t)seconds, (long)((seconds - (double)(time_t)seconds) * 1e9)};
+    nanosleep(&ts, NULL);
+}
+
+/* Virtual clock: the start of the next tick. */
+static void advance_to_next_tick(void) {
+    uint64_t tick_us = 1000000 / 60;
+    uint64_t t = (M.virtual_us / tick_us + 1) * tick_us;
+    /* 1000000/60 isn't whole: step until misc_ticks() really changes */
+    uint32_t before = misc_ticks();
+    M.virtual_us = t;
+    while (misc_ticks() == before)
+        M.virtual_us++;
+    M.polls = 0;
+}
+
+void misc_poll(void) {
+    if (M.fixed && ++M.polls >= 200) /* a loop that polls without ever waiting */
+        advance_to_next_tick();
+}
+
 bool misc_cursor_visible(void) { return M.cursor_level == 0; }
 
 bool misc_ae_handler(uint32_t event_class, uint32_t event_id, uint32_t *handler,
@@ -103,12 +144,14 @@ static void idle_if_new_tick(uint32_t t) {
 }
 
 static void h_tick_count(void) {
+    misc_poll();
     uint32_t t = misc_ticks();
     idle_if_new_tick(t);
     trap_return(t);
 }
 
 static void h_microseconds(void) {
+    misc_poll();
     uint64_t us = elapsed_us();
     uint32_t out = trap_arg(0);
     gm_w32(out, (uint32_t)(us >> 32));
@@ -120,19 +163,31 @@ static void h_microseconds(void) {
 static void h_delay(void) {
     uint32_t ticks = trap_arg(0), final_ticks = trap_arg(1);
     uint32_t end = misc_ticks() + ((int32_t)ticks > 0 ? ticks : 0);
+    if (M.fixed && end == misc_ticks()) /* Delay(0) on the virtual clock: one tick passes */
+        end++;
     for (;;) {
         uint32_t t = misc_ticks();
         idle_if_new_tick(t);
         if (t >= end)
             break;
-        struct timespec ts = {0, 1000000000L / 60 / 4};
-        nanosleep(&ts, NULL);
+        if (M.fixed)
+            advance_to_next_tick();
+        else
+            misc_wait(1.0 / 60 / 4);
     }
     if (final_ticks)
         gm_w32(final_ticks, misc_ticks());
 }
 
+/* The virtual clock's calendar starts at 2003-01-01 00:00:00 (Mac time), so
+   fixed-clock runs see the same date every time. */
+#define FIXED_CLOCK_EPOCH 3124224000u
+
 static void h_get_date_time(void) {
+    if (M.fixed) {
+        gm_w32(trap_arg(0), FIXED_CLOCK_EPOCH + (uint32_t)(M.virtual_us / 1000000));
+        return;
+    }
     time_t now = time(NULL);
     struct tm lt;
     localtime_r(&now, &lt);
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ./build/loony_tests misc_`
Expected: `18 passed, 0 failed, 0 skipped`. Full suite: `208 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/misc.h src/misc.c tests/test_misc.c
git commit -m "Fixed clock: time moves only when the game waits"
```

---

### Task 4: Event objects, dispatch and input

**Files:**
- Replace: `src/events.h`, `src/events.c` (the new versions are given whole)
- Modify: `tests/test_events.c`

**Interfaces:**
- Consumes: `keymap_lookup`, `keymap_char`, `KM_*` (Task 1); `script_next` (Task 2); `misc_seconds`, `misc_ticks`, `misc_wait`, `misc_poll`, `misc_ae_handler` (Task 3, Plan 2); `mm_new_ptr` (Plan 2); `sound_pump` (Plan 3); `cpu_fpr`, `guest_call`, `trap_*`.
- Produces (all of `src/events.h`):
  - New constants `EV_NOT_HANDLED_ERR` (-9874), `EV_PARAM_NOT_FOUND_ERR` (-9870), `EV_MAX_EVENTS`, the event classes `EV_CLASS_KEYBOARD`, `EV_CLASS_APPLICATION` and `EV_CLASS_APPLE_EVENT`, and kinds `EV_RAW_KEY_*`, `EV_APP_*`, `EV_APPLE_EVENT`
  - New functions `events_set_poll(ev_poll_fn)`, `events_set_screenshot(ev_screenshot_fn)`, `events_post_key(int scancode, bool down, bool repeat)`, `events_post_activation(bool active)`, `events_request_quit(void)` and `events_queued(void)`
  - Plan 3's functions, unchanged in signature
  - New imports `SendEventToEventTarget`, `GetEventKind`, `GetEventParameter` and `ReleaseEvent`
  - `ReceiveNextEvent` now returns queued events, honoring its type list and `pullEvent`. `RunApplicationEventLoop` dispatches queued events between timer fires.

Reference counting:
- A queued event belongs to the queue.
- `ReceiveNextEvent` with `pullEvent = true` hands it to the caller, who releases it. With `false` the event stays queued and the caller must not release it; doing so crashes.
- Dispatch takes its own reference, so a handler that releases the event can't free it mid-dispatch.

- [ ] **Step 1: Write the failing tests**

```diff
diff --git a/tests/test_events.c b/tests/test_events.c
index 99d54cb..1fca344 100644
--- a/tests/test_events.c
+++ b/tests/test_events.c
@@ -5,7 +5,12 @@
 #include "asm.h"
 #include "events.h"
 #include "harness.h"
+#include <SDL3/SDL_scancode.h>
+
+#include "keymap.h"
+#include "memmgr.h"
 #include "misc.h"
+#include "script.h"
 #include "util.h"
 
 static const char *const names[] = {
@@ -13,6 +18,8 @@ static const char *const names[] = {
     "GetWindowEventTarget", "InstallEventHandler", "InstallStandardEventHandler",
     "NewEventLoopTimerUPP", "GetMainEventLoop", "InstallEventLoopTimer", "RemoveEventLoopTimer",
     "RunApplicationEventLoop", "QuitApplicationEventLoop", "Delay", "ReceiveNextEvent",
+    "SendEventToEventTarget", "GetEventKind", "GetEventParameter", "ReleaseEvent",
+    "AEInstallEventHandler",
 };
 
 #define PROC    (GUEST_IMAGE_BASE + 0x200)
@@ -47,6 +54,7 @@ static void emit_counting_proc(int limit) {
 
 static void setup(void) {
     harness_init(names, sizeof names / sizeof names[0]);
+    mm_init();
     misc_init();
     misc_register();
     events_init();
@@ -105,14 +113,20 @@ TEST(events_run_loop_fires_timers_until_quit) {
     CHECK(elapsed < 0.5);
 }
 
-TEST(events_one_shot_timer_fires_once) {
+TEST(events_timers_fire_only_inside_the_event_loop) {
     setup();
     emit_counting_proc(100);
     install_timer(0.0, 0.0);
-    events_pump();
-    events_pump();
+    events_pump(); /* outside RunApplicationEventLoop and ReceiveNextEvent */
+    CHECK_EQ(gm_r32(COUNTER), 0);
+    cpu_set_fpr(1, 0.0);
+    uint32_t out = scratch(4);
+    uint32_t list = scratch(8);
+    gm_w32(list, FOURCC('z', 'z', 'z', 'z'));
+    gm_w32(list + 4, 1);
+    call_import("ReceiveNextEvent", 6, 1u, list, 0u, 0u, 1u, out); /* fires due timers */
     CHECK_EQ(gm_r32(COUNTER), 1);
-    CHECK_EQ(events_active_timers(), 0);
+    CHECK_EQ(events_active_timers(), 0); /* one-shot */
 }
 
 TEST(events_remove_timer) {
@@ -123,14 +137,14 @@ TEST(events_remove_timer) {
     CHECK_EQ(events_active_timers(), 0);
 }
 
-TEST(events_delay_pumps_timers) {
+TEST(events_delay_outside_the_loop_doesnt_fire_timers) {
     setup();
     misc_set_idle(events_pump);
     emit_counting_proc(100);
     install_timer(0.0, 0.0);
     call_import("Delay", 2, 2u, 0u);
     misc_set_idle(NULL);
-    CHECK_EQ(gm_r32(COUNTER), 1);
+    CHECK_EQ(gm_r32(COUNTER), 0);
 }
 
 static void child_exit_after(void *unused) {
@@ -163,6 +177,11 @@ TEST(events_removing_an_unknown_timer_crashes) {
 
 TEST(events_receive_next_event_times_out) {
     setup();
+    while (events_queued() > 0) { /* the startup kEventAppActivated */
+        uint32_t ev = scratch(4);
+        cpu_set_fpr(1, 0.0);
+        call_import("ReceiveNextEvent", 6, 0u, 0u, 0u, 0u, 1u, ev);
+    }
     uint32_t out = scratch(4);
     gm_w32(out, 0xFFFFFFFFu);
     cpu_set_fpr(1, 0.03);
@@ -175,3 +194,315 @@ TEST(events_receive_next_event_times_out) {
     CHECK_EQ((int32_t)call_import("ReceiveNextEvent", 6, 0u, 0u, 0u, 0u, 1u, out),
              EV_LOOP_TIMED_OUT_ERR);
 }
+
+/* ---- keyboard events and dispatch ---- */
+
+#define STORE_PROC (GUEST_IMAGE_BASE + 0x400)
+#define NH_PROC    (GUEST_IMAGE_BASE + 0x600)
+#define QUIT_PROC  (GUEST_IMAGE_BASE + 0x700)
+#define TV_STORE   (GUEST_IMAGE_BASE + 0x8110)
+#define TV_NH      (GUEST_IMAGE_BASE + 0x8118)
+#define TV_QUIT    (GUEST_IMAGE_BASE + 0x8120)
+#define STORE      (GUEST_IMAGE_BASE + 0x8300)
+#define NH_FLAG    (GUEST_IMAGE_BASE + 0x8304)
+#define QUIT_FLAG  (GUEST_IMAGE_BASE + 0x8308)
+
+/* Emits code that loads the transition vector at tv and calls it. */
+static int emit_call(uint32_t *code, int n, uint32_t tv) {
+    code[n++] = ppc_lis(12, tv >> 16);
+    code[n++] = ppc_ori(12, 12, tv & 0xFFFF);
+    code[n++] = ppc_lwz(0, 0, 12);
+    code[n++] = ppc_lwz(2, 4, 12);
+    code[n++] = PPC_MTCTR_R0;
+    code[n++] = PPC_BCTRL;
+    return n;
+}
+
+static void set_tv(uint32_t tv, uint32_t code) {
+    gm_w32(tv, code);
+    gm_w32(tv + 4, 0);
+}
+
+/* Handlers, called as handler(nextHandler, event, userData):
+   STORE_PROC: GetEventParameter(event, 'kcod', typeUInt32, NULL, 4, NULL, STORE); returns noErr.
+   NH_PROC:    stores 1 at NH_FLAG and returns eventNotHandledErr.
+   QUIT_PROC:  an Apple Event handler; stores 1 at QUIT_FLAG and calls QuitApplicationEventLoop. */
+static void emit_handlers(void) {
+    uint32_t c[40];
+    int n = 0;
+    c[n++] = PPC_MFLR_R0;
+    c[n++] = PPC_SAVE_LR;
+    c[n++] = PPC_PUSH64;
+    c[n++] = 0x7C832378u; /* mr r3,r4 */
+    c[n++] = ppc_lis(4, 0x6B63);
+    c[n++] = ppc_ori(4, 4, 0x6F64);
+    c[n++] = ppc_lis(5, 0x6D61);
+    c[n++] = ppc_ori(5, 5, 0x676E);
+    c[n++] = ppc_addi(6, 0, 0);
+    c[n++] = ppc_addi(7, 0, 4);
+    c[n++] = ppc_addi(8, 0, 0);
+    c[n++] = ppc_lis(9, STORE >> 16);
+    c[n++] = ppc_ori(9, 9, STORE & 0xFFFF);
+    n = emit_call(c, n, tv_of("GetEventParameter"));
+    c[n++] = PPC_POP64;
+    c[n++] = PPC_LOAD_LR;
+    c[n++] = PPC_MTLR_R0;
+    c[n++] = ppc_addi(3, 0, 0);
+    c[n++] = PPC_BLR;
+    put_words(STORE_PROC, c, n);
+    uint32_t nh[] = {
+        ppc_addi(0, 0, 1), ppc_lis(6, NH_FLAG >> 16), ppc_ori(6, 6, NH_FLAG & 0xFFFF),
+        ppc_stw(0, 0, 6), ppc_addi(3, 0, -9874), PPC_BLR,
+    };
+    put_words(NH_PROC, nh, 6);
+    n = 0;
+    c[n++] = PPC_MFLR_R0;
+    c[n++] = PPC_SAVE_LR;
+    c[n++] = PPC_PUSH64;
+    c[n++] = ppc_addi(0, 0, 1);
+    c[n++] = ppc_lis(6, QUIT_FLAG >> 16);
+    c[n++] = ppc_ori(6, 6, QUIT_FLAG & 0xFFFF);
+    c[n++] = ppc_stw(0, 0, 6);
+    n = emit_call(c, n, tv_of("QuitApplicationEventLoop"));
+    c[n++] = PPC_POP64;
+    c[n++] = PPC_LOAD_LR;
+    c[n++] = PPC_MTLR_R0;
+    c[n++] = ppc_addi(3, 0, 0);
+    c[n++] = PPC_BLR;
+    put_words(QUIT_PROC, c, n);
+    set_tv(TV_STORE, STORE_PROC);
+    set_tv(TV_NH, NH_PROC);
+    set_tv(TV_QUIT, QUIT_PROC);
+    gm_w32(STORE, 0);
+    gm_w32(NH_FLAG, 0);
+    gm_w32(QUIT_FLAG, 0);
+}
+
+static void install(uint32_t target, uint32_t tv, uint32_t cls, uint32_t kind) {
+    uint32_t list = scratch(8);
+    gm_w32(list, cls);
+    gm_w32(list + 4, kind);
+    call_import("InstallEventHandler", 6, target, tv, 1u, list, 0u, 0u);
+}
+
+/* Pulls the next event of (cls, kind), or 0. */
+static uint32_t next_event(uint32_t cls, uint32_t kind) {
+    uint32_t list = scratch(8), out = scratch(4);
+    gm_w32(list, cls);
+    gm_w32(list + 4, kind);
+    cpu_set_fpr(1, 0.0);
+    if (call_import("ReceiveNextEvent", 6, 1u, list, 0u, 0u, 1u, out) != 0)
+        return 0;
+    return gm_r32(out);
+}
+
+static uint32_t param32(uint32_t ev, uint32_t name) {
+    uint32_t out = scratch(4);
+    call_import("GetEventParameter", 7, ev, name, 0x6D61676Eu, 0u, 4u, 0u, out);
+    return gm_r32(out);
+}
+
+TEST(events_startup_queues_app_activated) {
+    setup();
+    CHECK_EQ(events_queued(), 1);
+    uint32_t ev = next_event(EV_CLASS_APPLICATION, EV_APP_ACTIVATED);
+    CHECK(ev != 0);
+    CHECK_EQ(call_import("GetEventKind", 1, ev), EV_APP_ACTIVATED);
+    call_import("ReleaseEvent", 1, ev);
+    CHECK_EQ(events_queued(), 0);
+}
+
+TEST(events_key_down_reaches_the_application_handler) {
+    setup();
+    emit_handlers();
+    install(EV_APPLICATION_TARGET, TV_STORE, EV_CLASS_KEYBOARD, EV_RAW_KEY_DOWN);
+    events_post_key(SDL_SCANCODE_Z, true, false);
+    uint32_t ev = next_event(EV_CLASS_KEYBOARD, EV_RAW_KEY_DOWN);
+    CHECK(ev != 0);
+    CHECK_EQ(call_import("SendEventToEventTarget", 2, ev, EV_DISPATCHER_TARGET), 0);
+    CHECK_EQ(gm_r32(STORE), 0x06);
+    call_import("ReleaseEvent", 1, ev);
+}
+
+TEST(events_key_parameters) {
+    setup();
+    events_post_key(SDL_SCANCODE_LSHIFT, true, false);
+    events_post_key(SDL_SCANCODE_SLASH, true, false);
+    uint32_t ev = next_event(EV_CLASS_KEYBOARD, EV_RAW_KEY_DOWN);
+    CHECK_EQ(param32(ev, 0x6B636F64u), 0x2C); /* 'kcod' */
+    CHECK_EQ(param32(ev, 0x6B6D6F64u), KM_SHIFT); /* 'kmod' */
+    uint32_t c = scratch(1), type = scratch(4), size = scratch(4);
+    CHECK_EQ(call_import("GetEventParameter", 7, ev, 0x6B636872u, 0x54455854u, type, 1u, size, c), 0);
+    CHECK_EQ(gm_r8(c), '?'); /* shifted slash */
+    CHECK_EQ(gm_r32(type), 0x54455854u);
+    CHECK_EQ(gm_r32(size), 1);
+    CHECK_EQ((int32_t)call_import("GetEventParameter", 7, ev, 0x2D2D2D2Du, 0x2A2A2A2Au, 0u, 4u,
+                                  0u, c), EV_PARAM_NOT_FOUND_ERR);
+    call_import("ReleaseEvent", 1, ev);
+}
+
+TEST(events_modifier_changes_track_both_sides) {
+    setup();
+    next_event(EV_CLASS_APPLICATION, EV_APP_ACTIVATED);
+    events_post_key(SDL_SCANCODE_LSHIFT, true, false);
+    events_post_key(SDL_SCANCODE_RSHIFT, true, false);
+    events_post_key(SDL_SCANCODE_LSHIFT, false, false); /* right still down: no change */
+    events_post_key(SDL_SCANCODE_LSHIFT, false, false); /* already up: no change */
+    events_post_key(SDL_SCANCODE_RSHIFT, false, false);
+    CHECK_EQ(events_queued(), 3);
+    uint32_t want[3] = {KM_SHIFT, KM_SHIFT | KM_RIGHT_SHIFT, 0};
+    for (int i = 0; i < 3; i++) {
+        uint32_t ev = next_event(EV_CLASS_KEYBOARD, EV_RAW_KEY_MODIFIERS_CHANGED);
+        CHECK(ev != 0);
+        CHECK_EQ(param32(ev, 0x6B6D6F64u), want[i]);
+        call_import("ReleaseEvent", 1, ev);
+    }
+}
+
+TEST(events_newest_handler_first_and_not_handled_continues) {
+    setup();
+    emit_handlers();
+    install(EV_APPLICATION_TARGET, TV_STORE, EV_CLASS_KEYBOARD, EV_RAW_KEY_UP);
+    install(EV_APPLICATION_TARGET, TV_NH, EV_CLASS_KEYBOARD, EV_RAW_KEY_UP);
+    events_post_key(SDL_SCANCODE_SPACE, false, false);
+    uint32_t ev = next_event(EV_CLASS_KEYBOARD, EV_RAW_KEY_UP);
+    CHECK_EQ(call_import("SendEventToEventTarget", 2, ev, EV_APPLICATION_TARGET), 0);
+    CHECK_EQ(gm_r32(NH_FLAG), 1);
+    CHECK_EQ(gm_r32(STORE), 0x31);
+    call_import("ReleaseEvent", 1, ev);
+}
+
+TEST(events_dispatcher_sends_keys_to_the_window_first) {
+    setup();
+    emit_handlers();
+    uint32_t win = call_import("GetWindowEventTarget", 1, 0x01000100u);
+    install(EV_APPLICATION_TARGET, TV_STORE, EV_CLASS_KEYBOARD, EV_RAW_KEY_DOWN);
+    install(win, TV_NH, EV_CLASS_KEYBOARD, EV_RAW_KEY_DOWN);
+    events_post_key(SDL_SCANCODE_RETURN, true, false);
+    uint32_t ev = next_event(EV_CLASS_KEYBOARD, EV_RAW_KEY_DOWN);
+    CHECK_EQ(call_import("SendEventToEventTarget", 2, ev, EV_DISPATCHER_TARGET), 0);
+    CHECK_EQ(gm_r32(NH_FLAG), 1);
+    CHECK_EQ(gm_r32(STORE), 0x24);
+    call_import("ReleaseEvent", 1, ev);
+}
+
+TEST(events_unhandled_event_returns_not_handled) {
+    setup();
+    events_post_key(SDL_SCANCODE_A, true, false);
+    uint32_t ev = next_event(EV_CLASS_KEYBOARD, EV_RAW_KEY_DOWN);
+    CHECK_EQ((int32_t)call_import("SendEventToEventTarget", 2, ev, EV_DISPATCHER_TARGET),
+             EV_NOT_HANDLED_ERR);
+    call_import("ReleaseEvent", 1, ev);
+}
+
+TEST(events_peek_leaves_the_event_queued) {
+    setup();
+    uint32_t out = scratch(4);
+    cpu_set_fpr(1, 0.0);
+    CHECK_EQ(call_import("ReceiveNextEvent", 6, 0u, 0u, 0u, 0u, 0u, out), 0); /* pull = false */
+    CHECK(gm_r32(out) != 0);
+    CHECK_EQ(events_queued(), 1);
+}
+
+static void child_release_queued(void *unused) {
+    (void)unused;
+    setup();
+    uint32_t out = scratch(4);
+    cpu_set_fpr(1, 0.0);
+    call_import("ReceiveNextEvent", 6, 0u, 0u, 0u, 0u, 0u, out);
+    call_import("ReleaseEvent", 1, gm_r32(out));
+}
+
+TEST(events_releasing_a_queued_event_crashes) {
+    char out[16384];
+    CHECK_EQ(test_run_child(child_release_queued, NULL, out, sizeof out), 2);
+    CHECK_CONTAINS(out, "is still in the event queue");
+}
+
+static void child_wrong_param_type(void *unused) {
+    (void)unused;
+    setup();
+    events_post_key(SDL_SCANCODE_A, true, false);
+    uint32_t ev = next_event(EV_CLASS_KEYBOARD, EV_RAW_KEY_DOWN);
+    call_import("GetEventParameter", 7, ev, 0x6B636F64u, 0x54455854u, 0u, 4u, 0u, scratch(4));
+}
+
+TEST(events_parameter_of_the_wrong_type_crashes) {
+    char out[16384];
+    CHECK_EQ(test_run_child(child_wrong_param_type, NULL, out, sizeof out), 2);
+    CHECK_CONTAINS(out, "GetEventParameter: can't return parameter 0x6b636f64 as type 0x54455854");
+}
+
+TEST(events_run_loop_dispatches_queued_events) {
+    setup();
+    emit_handlers();
+    emit_counting_proc(2);
+    install(EV_APPLICATION_TARGET, TV_STORE, EV_CLASS_KEYBOARD, EV_RAW_KEY_DOWN);
+    events_post_key(SDL_SCANCODE_Z, true, false);
+    install_timer(0.01, 0.01);
+    call_import("RunApplicationEventLoop", 0);
+    CHECK_EQ(gm_r32(STORE), 0x06);
+    CHECK_EQ(events_queued(), 0);
+}
+
+static void child_quit_with_handler(void *unused) {
+    (void)unused;
+    setup();
+    emit_handlers();
+    emit_counting_proc(1000);
+    call_import("AEInstallEventHandler", 5, 0x61657674u, 0x71756974u, TV_QUIT, 0u, 0u);
+    install_timer(0.0, 0.01);
+    events_request_quit();
+    call_import("RunApplicationEventLoop", 0); /* returns once the handler quits the loop */
+    exit(gm_r32(QUIT_FLAG) == 1 ? 0 : 3);
+}
+
+TEST(events_quit_request_calls_the_apple_event_handler) {
+    char out[4096];
+    CHECK_EQ(test_run_child(child_quit_with_handler, NULL, out, sizeof out), 0);
+    CHECK_CONTAINS(out, "loony: sending the quit Apple Event");
+}
+
+static void child_quit_without_handler(void *unused) {
+    (void)unused;
+    setup();
+    emit_counting_proc(1000);
+    install_timer(0.0, 0.01);
+    events_request_quit();
+    call_import("RunApplicationEventLoop", 0);
+    exit(7);
+}
+
+TEST(events_quit_without_a_handler_exits) {
+    char out[4096];
+    CHECK_EQ(test_run_child(child_quit_without_handler, NULL, out, sizeof out), 0);
+    CHECK_CONTAINS(out, "quit requested; the game has no quit handler");
+}
+
+static void child_quit_ignored(void *unused) {
+    (void)unused;
+    setup();
+    events_request_quit();
+    for (int i = 0; i < 500; i++) { /* nobody dispatches the event */
+        events_pump();
+        struct timespec ts = {0, 10000000};
+        nanosleep(&ts, NULL);
+    }
+    exit(7);
+}
+
+TEST(events_quit_ignored_for_3_seconds_exits) {
+    char out[4096];
+    CHECK_EQ(test_run_child(child_quit_ignored, NULL, out, sizeof out), 0);
+    CHECK_CONTAINS(out, "the game didn't quit within 3 seconds; exiting");
+}
+
+TEST(events_pump_runs_due_script_actions) {
+    setup();
+    char err[256];
+    CHECK(script_parse("0 down z\n0 up z\n1000 down z\n", err, sizeof err));
+    events_pump();
+    CHECK_EQ(events_queued(), 3); /* activation, down, up */
+    CHECK_EQ(script_remaining(), 1);
+    CHECK(script_parse("", err, sizeof err));
+}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build`
Expected: the build fails with `call to undeclared function 'events_queued'`.

- [ ] **Step 3: Write the implementation**

`src/events.h`:
```c
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Carbon Event Manager: handler installation, event objects and dispatch,
   keyboard input, event loop timers, RunApplicationEventLoop and
   ReceiveNextEvent. Event targets, events, timers and the main event loop are
   opaque IDs in tag space (EV_TAG_BASE and up).

   Dispatch follows Carbon: an event sent to the dispatcher target goes to the
   focus window's handlers, then the application's; one sent to a window goes
   to that window, then the application. On each target the most recently
   installed handler runs first, and dispatch stops at the first handler that
   returns anything other than eventNotHandledErr. */

#define EV_TAG_BASE 0x0A000000u
#define EV_APPLICATION_TARGET (EV_TAG_BASE + 1)
#define EV_DISPATCHER_TARGET  (EV_TAG_BASE + 2)
#define EV_MAIN_LOOP          (EV_TAG_BASE + 3)
#define EV_NOT_HANDLED_ERR        (-9874) /* eventNotHandledErr */
#define EV_LOOP_TIMED_OUT_ERR     (-9875) /* eventLoopTimedOutErr */
#define EV_PARAM_NOT_FOUND_ERR    (-9870) /* eventParameterNotFoundErr */
#define EV_MAX_HANDLERS 32
#define EV_MAX_TIMERS 16
#define EV_MAX_EVENTS 64

/* Event classes and kinds this module creates. */
#define EV_CLASS_KEYBOARD    0x6B657962u /* 'keyb' */
#define EV_CLASS_APPLICATION 0x6170706Cu /* 'appl' */
#define EV_CLASS_APPLE_EVENT 0x65707063u /* 'eppc' */
#define EV_RAW_KEY_DOWN    1
#define EV_RAW_KEY_REPEAT  2
#define EV_RAW_KEY_UP      3
#define EV_RAW_KEY_MODIFIERS_CHANGED 4
#define EV_APP_ACTIVATED   1
#define EV_APP_DEACTIVATED 2
#define EV_APPLE_EVENT     1

typedef struct {
    uint32_t target, handler, user_data;
    uint32_t ntypes;
    uint32_t types[8][2]; /* (class, kind) pairs */
} ev_handler;

/* Resets everything and queues kEventAppActivated, as a Mac does when an
   application starts in front. Reads LOONY_EXIT_AFTER. */
void events_init(void);

/* The event target for a window. */
uint32_t events_window_target(uint32_t window);

/* Installed handlers, in installation order. */
int events_handlers(const ev_handler **out);

/* True if InstallStandardEventHandler was called for target. */
bool events_has_standard_handler(uint32_t target);

/* Called on every pump to show the screen if anything drew to it. */
typedef void (*ev_present_fn)(void);
void events_set_present(ev_present_fn fn);

/* Called on every pump to collect host input (SDL events). */
typedef void (*ev_poll_fn)(void);
void events_set_poll(ev_poll_fn fn);

/* Called for a scripted screenshot. */
typedef bool (*ev_screenshot_fn)(const char *path);
void events_set_screenshot(ev_screenshot_fn fn);

/* Host input. A key with an SDL scancode went down or up (repeat: an
   auto-repeat). Modifier keys become kEventRawKeyModifiersChanged, others
   kEventRawKeyDown, Up or Repeat with 'kcod', 'kchr' and 'kmod'. */
void events_post_key(int scancode, bool down, bool repeat);
/* The window gained or lost focus: kEventAppActivated / Deactivated. */
void events_post_activation(bool active);
/* The user asked to quit (window close, Cmd-Q, a script). Queues the quit
   Apple Event for the game's handler; if the game hasn't quit 3 seconds
   (wall clock) later, exits with status 0. */
void events_request_quit(void);

/* Events waiting in the queue. */
int events_queued(void);

/* One pump step: host input, scripted actions, sound, due timers (only
   inside RunApplicationEventLoop or ReceiveNextEvent, never re-entering a
   running timer), presenting the screen, and the LOONY_EXIT_AFTER check. The
   run loop calls it, and so does misc's idle hook while the game waits in
   its own loop. */
void events_pump(void);

/* Number of timers installed and not removed. */
int events_active_timers(void);

/* Registers NewEventHandlerUPP, GetApplicationEventTarget,
   GetEventDispatcherTarget, GetWindowEventTarget, InstallEventHandler,
   InstallStandardEventHandler, NewEventLoopTimerUPP, GetMainEventLoop,
   InstallEventLoopTimer, RemoveEventLoopTimer, RunApplicationEventLoop,
   QuitApplicationEventLoop, ReceiveNextEvent, SendEventToEventTarget,
   GetEventKind, GetEventParameter and ReleaseEvent.

   If LOONY_EXIT_AFTER is set to a tick count (1/60 s), the process exits with
   status 0 once that many ticks have passed. */
void events_register(void);
```

`src/events.c`:
```c
#include "events.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu.h"
#include "guest_mem.h"
#include "keymap.h"
#include "memmgr.h"
#include "misc.h"
#include "script.h"
#include "sound.h"
#include "trap.h"
#include "util.h"

#define MAX_STANDARD 8
#define MAX_HELD 16
#define QUIT_GRACE_SECONDS 3.0

/* Parameter names and types (Carbon Events.h, AEDataModel.h). */
#define PARAM_KEY_CODE      0x6B636F64u /* 'kcod' */
#define PARAM_KEY_CHAR      0x6B636872u /* 'kchr' */
#define PARAM_KEY_MODIFIERS 0x6B6D6F64u /* 'kmod' */
#define PARAM_DIRECT_OBJECT 0x2D2D2D2Du /* '----' */
#define TYPE_UINT32   0x6D61676Eu /* 'magn' */
#define TYPE_CHAR     0x54455854u /* 'TEXT' */
#define TYPE_WILDCARD 0x2A2A2A2Au /* '****' */
#define AE_CLASS_CORE 0x61657674u /* 'aevt' */
#define AE_ID_QUIT    0x71756974u /* 'quit' */

/* Tag-space sub-ranges. */
#define TAG_HANDLER_REF (EV_TAG_BASE + 0x1000u)
#define TAG_TIMER       (EV_TAG_BASE + 0x2000u)
#define TAG_NEXT_HANDLER (EV_TAG_BASE + 0x3000u)
#define TAG_EVENT       (EV_TAG_BASE + 0x4000u)
#define TAG_WINDOW      (EV_TAG_BASE + 0x10000u)

typedef struct {
    int refs; /* 0 = free slot */
    bool queued;
    uint32_t cls, kind;
    uint32_t key_code, modifiers;
    uint8_t chr;
} ev_event;

static struct {
    ev_handler handlers[EV_MAX_HANDLERS];
    int nhandlers;
    uint32_t standard[MAX_STANDARD];
    int nstandard;
    uint32_t focus_window; /* the window target keyboard events go to, or 0 */
    struct {
        bool active, running;
        uint32_t proc, data;
        double next, interval; /* seconds on the misc clock */
    } timers[EV_MAX_TIMERS];
    ev_event events[EV_MAX_EVENTS];
    int queue[EV_MAX_EVENTS];
    int qhead, qcount;
    struct {
        int scancode;
        uint32_t bits;
    } held[MAX_HELD]; /* modifier keys that are down */
    int nheld;
    bool quit;
    int loop_depth; /* inside RunApplicationEventLoop or a ReceiveNextEvent wait */
    ev_present_fn present;
    ev_poll_fn poll;
    ev_screenshot_fn screenshot;
    long exit_after; /* ticks, or 0 */
    double quit_deadline; /* wall-clock seconds, or 0 */
    uint32_t ae_descs; /* guest memory for the quit AppleEvent and its reply */
    bool warned_full;
} E;

static double wall_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static double now_seconds(void) { return misc_seconds(); }

/* ---- event objects and the queue ---- */

static uint32_t event_ref(int i) { return TAG_EVENT + (uint32_t)i; }

static ev_event *lookup_event(uint32_t ref) {
    uint32_t i = ref - TAG_EVENT;
    if (i >= EV_MAX_EVENTS || E.events[i].refs == 0)
        return NULL;
    return &E.events[i];
}

static ev_event *need_event(const char *call, uint32_t ref) {
    ev_event *e = lookup_event(ref);
    if (!e)
        trap_crash("%s: 0x%08x is not an event", call, ref);
    return e;
}

/* Queues a new event (the queue holds its reference). Returns it, or NULL
   if the queue is full (logged once). */
static ev_event *post(uint32_t cls, uint32_t kind) {
    int i = 0;
    while (i < EV_MAX_EVENTS && E.events[i].refs)
        i++;
    if (i == EV_MAX_EVENTS || E.qcount == EV_MAX_EVENTS) {
        if (!E.warned_full)
            log_msg("events: the queue is full; dropping input");
        E.warned_full = true;
        return NULL;
    }
    E.events[i] = (ev_event){1, true, cls, kind, 0, 0, 0};
    E.queue[(E.qhead + E.qcount) % EV_MAX_EVENTS] = i;
    E.qcount++;
    return &E.events[i];
}

/* Removes the queued event at position k (0 = oldest) and returns its index. */
static int dequeue_at(int k) {
    int idx = E.queue[(E.qhead + k) % EV_MAX_EVENTS];
    for (int j = k; j > 0; j--)
        E.queue[(E.qhead + j) % EV_MAX_EVENTS] = E.queue[(E.qhead + j - 1) % EV_MAX_EVENTS];
    E.qhead = (E.qhead + 1) % EV_MAX_EVENTS;
    E.qcount--;
    E.events[idx].queued = false;
    return idx;
}

static void release(ev_event *e) {
    if (--e->refs == 0)
        memset(e, 0, sizeof *e);
}

int events_queued(void) { return E.qcount; }

/* ---- host input ---- */

static uint32_t current_modifiers(void) {
    uint32_t m = 0;
    for (int i = 0; i < E.nheld; i++)
        m |= E.held[i].bits;
    return m;
}

void events_post_key(int scancode, bool down, bool repeat) {
    keymap_entry k = keymap_lookup(scancode);
    if (k.vkey < 0)
        return;
    if (k.modifier) {
        uint32_t before = current_modifiers();
        int at = -1;
        for (int i = 0; i < E.nheld; i++)
            if (E.held[i].scancode == scancode)
                at = i;
        if (down && at < 0 && E.nheld < MAX_HELD)
            E.held[E.nheld++] = (typeof(E.held[0])){scancode, k.modifier};
        else if (!down && at >= 0)
            E.held[at] = E.held[--E.nheld];
        uint32_t after = current_modifiers();
        if (after != before) {
            ev_event *e = post(EV_CLASS_KEYBOARD, EV_RAW_KEY_MODIFIERS_CHANGED);
            if (e)
                e->modifiers = after;
        }
        return;
    }
    uint32_t kind = !down ? EV_RAW_KEY_UP : repeat ? EV_RAW_KEY_REPEAT : EV_RAW_KEY_DOWN;
    ev_event *e = post(EV_CLASS_KEYBOARD, kind);
    if (e) {
        e->key_code = (uint32_t)k.vkey;
        e->modifiers = current_modifiers();
        e->chr = keymap_char(&k, e->modifiers);
    }
}

void events_post_activation(bool active) {
    post(EV_CLASS_APPLICATION, active ? EV_APP_ACTIVATED : EV_APP_DEACTIVATED);
}

void events_request_quit(void) {
    if (E.quit_deadline == 0) {
        post(EV_CLASS_APPLE_EVENT, EV_APPLE_EVENT);
        E.quit_deadline = wall_seconds() + QUIT_GRACE_SECONDS;
    }
}

/* ---- setup ---- */

void events_init(void) {
    memset(&E, 0, sizeof E);
    const char *s = getenv("LOONY_EXIT_AFTER");
    if (s && *s)
        E.exit_after = strtol(s, NULL, 10);
    events_post_activation(true);
}

void events_set_present(ev_present_fn fn) { E.present = fn; }
void events_set_poll(ev_poll_fn fn) { E.poll = fn; }
void events_set_screenshot(ev_screenshot_fn fn) { E.screenshot = fn; }

int events_active_timers(void) {
    int n = 0;
    for (int i = 0; i < EV_MAX_TIMERS; i++)
        n += E.timers[i].active;
    return n;
}

/* Window targets are TAG_WINDOW + the window's address / 16, which is
   unique and reversible. */
uint32_t events_window_target(uint32_t window) { return TAG_WINDOW + window / 16u; }

static bool is_window_target(uint32_t t) { return t >= TAG_WINDOW && t < TAG_WINDOW + 0x01000000u; }

int events_handlers(const ev_handler **out) {
    *out = E.handlers;
    return E.nhandlers;
}

bool events_has_standard_handler(uint32_t target) {
    for (int i = 0; i < E.nstandard; i++)
        if (E.standard[i] == target)
            return true;
    return false;
}

/* ---- dispatch ---- */

/* The quit Apple Event goes to the handler AEInstallEventHandler recorded;
   with none, the application just exits. */
static void handle_apple_event(void) {
    uint32_t handler, refcon;
    if (!misc_ae_handler(AE_CLASS_CORE, AE_ID_QUIT, &handler, &refcon)) {
        log_msg("quit requested; the game has no quit handler");
        exit(0);
    }
    if (!E.ae_descs) {
        E.ae_descs = mm_new_ptr(16, true); /* two AEDescs: {descriptorType, dataHandle} */
        if (!E.ae_descs)
            trap_crash("out of guest memory for an Apple Event");
        gm_w32(E.ae_descs, AE_CLASS_CORE);
        gm_w32(E.ae_descs + 8, 0x6E756C6Cu); /* 'null' */
    }
    log_msg("sending the quit Apple Event");
    uint32_t args[3] = {E.ae_descs, E.ae_descs + 8, refcon};
    guest_call(handler, 3, args);
}

static bool handles(const ev_handler *h, const ev_event *e) {
    for (uint32_t k = 0; k < h->ntypes; k++)
        if (h->types[k][0] == e->cls && h->types[k][1] == e->kind)
            return true;
    return false;
}

/* Runs the handlers on target for event idx, newest first. */
static int32_t run_handlers(uint32_t target, int idx) {
    for (int i = E.nhandlers - 1; i >= 0; i--) {
        const ev_handler *h = &E.handlers[i];
        if (h->target != target || !handles(h, &E.events[idx]))
            continue;
        uint32_t args[3] = {TAG_NEXT_HANDLER, event_ref(idx), h->user_data};
        int32_t r = (int32_t)guest_call(h->handler, 3, args);
        if (r != EV_NOT_HANDLED_ERR)
            return r;
    }
    return EV_NOT_HANDLED_ERR;
}

static int32_t dispatch(int idx, uint32_t target) {
    uint32_t chain[2];
    int n = 0;
    if (target == EV_DISPATCHER_TARGET) {
        if (E.focus_window && E.events[idx].cls == EV_CLASS_KEYBOARD)
            chain[n++] = E.focus_window;
        chain[n++] = EV_APPLICATION_TARGET;
    } else if (is_window_target(target)) {
        chain[n++] = target;
        chain[n++] = EV_APPLICATION_TARGET;
    } else if (target == EV_APPLICATION_TARGET) {
        chain[n++] = EV_APPLICATION_TARGET;
    } else {
        trap_crash("SendEventToEventTarget: 0x%08x is not an event target", target);
    }
    E.events[idx].refs++; /* handlers may release their own references */
    int32_t r = EV_NOT_HANDLED_ERR;
    for (int i = 0; i < n && r == EV_NOT_HANDLED_ERR; i++)
        r = run_handlers(chain[i], idx);
    if (r == EV_NOT_HANDLED_ERR && E.events[idx].cls == EV_CLASS_APPLE_EVENT) {
        handle_apple_event(); /* the standard application handler's job */
        r = 0;
    }
    release(&E.events[idx]);
    return r;
}

/* ---- timers ---- */

static uint32_t timer_ref(int i) { return TAG_TIMER + (uint32_t)i; }

static void fire_due_timers(void) {
    for (int i = 0; i < EV_MAX_TIMERS; i++) {
        if (!E.timers[i].active || E.timers[i].running || now_seconds() < E.timers[i].next)
            continue;
        if (E.timers[i].interval > 0) {
            E.timers[i].next += E.timers[i].interval;
            if (E.timers[i].next < now_seconds()) /* fell behind: don't try to catch up */
                E.timers[i].next = now_seconds() + E.timers[i].interval;
        } else {
            E.timers[i].active = false;
        }
        uint32_t args[2] = {timer_ref(i), E.timers[i].data};
        E.timers[i].running = true;
        guest_call(E.timers[i].proc, 2, args);
        E.timers[i].running = false;
    }
}

static void wait_until_next_timer(void) {
    double next = now_seconds() + 0.010;
    for (int i = 0; i < EV_MAX_TIMERS; i++)
        if (E.timers[i].active && !E.timers[i].running && E.timers[i].next < next)
            next = E.timers[i].next;
    misc_wait(next - now_seconds());
}

/* ---- the pump ---- */

static void run_script(void) {
    script_action a;
    while (script_next(misc_ticks(), &a)) {
        switch (a.kind) {
        case SCRIPT_KEY_DOWN: events_post_key(a.scancode, true, false); break;
        case SCRIPT_KEY_UP: events_post_key(a.scancode, false, false); break;
        case SCRIPT_SCREENSHOT:
            if (!E.screenshot || !E.screenshot(a.path))
                log_msg("script: can't write the screenshot %s", a.path);
            break;
        case SCRIPT_QUIT: events_request_quit(); break;
        }
    }
}

void events_pump(void) {
    if (E.poll)
        E.poll();
    run_script();
    sound_pump();
    if (E.loop_depth > 0)
        fire_due_timers();
    if (E.present)
        E.present();
    if (E.exit_after > 0 && misc_ticks() >= (uint32_t)E.exit_after) {
        log_msg("exiting after %ld ticks (LOONY_EXIT_AFTER)", E.exit_after);
        exit(0);
    }
    if (E.quit_deadline > 0 && wall_seconds() >= E.quit_deadline) {
        log_msg("the game didn't quit within %.0f seconds; exiting", QUIT_GRACE_SECONDS);
        exit(0);
    }
}

/* ---- guest calls: handlers ---- */

static void h_new_event_handler_upp(void) { trap_return(trap_arg(0)); }
static void h_get_application_event_target(void) { trap_return(EV_APPLICATION_TARGET); }
static void h_get_event_dispatcher_target(void) { trap_return(EV_DISPATCHER_TARGET); }
static void h_get_window_event_target(void) { trap_return(events_window_target(trap_arg(0))); }

/* InstallEventHandler(target, handler, numTypes, const EventTypeSpec *list,
   void *userData, EventHandlerRef *outRef) -> OSStatus */
static void h_install_event_handler(void) {
    uint32_t ntypes = trap_arg(2), list = trap_arg(3), out = trap_arg(5);
    if (E.nhandlers == EV_MAX_HANDLERS)
        trap_crash("InstallEventHandler: more than %d handlers", EV_MAX_HANDLERS);
    if (ntypes > 8)
        trap_crash("InstallEventHandler: %u event types (at most 8 supported)", ntypes);
    ev_handler *h = &E.handlers[E.nhandlers];
    h->target = trap_arg(0);
    h->handler = trap_arg(1);
    h->user_data = trap_arg(4);
    h->ntypes = ntypes;
    for (uint32_t i = 0; i < ntypes; i++) {
        h->types[i][0] = gm_r32(list + 8 * i);
        h->types[i][1] = gm_r32(list + 8 * i + 4);
    }
    E.nhandlers++;
    if (is_window_target(h->target))
        E.focus_window = h->target;
    if (out)
        gm_w32(out, TAG_HANDLER_REF + (uint32_t)E.nhandlers);
    trap_return(0);
}

static void h_install_standard_event_handler(void) {
    uint32_t t = trap_arg(0);
    if (E.nstandard < MAX_STANDARD)
        E.standard[E.nstandard++] = t;
    if (is_window_target(t))
        E.focus_window = t;
    trap_return(0);
}

/* ---- guest calls: events ---- */

/* SendEventToEventTarget(EventRef, EventTargetRef) -> OSStatus */
static void h_send_event_to_event_target(void) {
    ev_event *e = need_event("SendEventToEventTarget", trap_arg(0));
    trap_return((uint32_t)dispatch((int)(e - E.events), trap_arg(1)));
}

static void h_get_event_kind(void) { trap_return(need_event("GetEventKind", trap_arg(0))->kind); }

static void h_release_event(void) {
    ev_event *e = need_event("ReleaseEvent", trap_arg(0));
    if (e->queued && e->refs == 1)
        trap_crash("ReleaseEvent: 0x%08x is still in the event queue", trap_arg(0));
    release(e);
}

/* GetEventParameter(EventRef, EventParamName, EventParamType desiredType,
   EventParamType *outActualType, UInt32 bufferSize, UInt32 *outActualSize,
   void *outData) -> OSStatus */
static void h_get_event_parameter(void) {
    ev_event *e = need_event("GetEventParameter", trap_arg(0));
    uint32_t name = trap_arg(1), desired = trap_arg(2), out_type = trap_arg(3);
    uint32_t buf_size = trap_arg(4), out_size = trap_arg(5), out = trap_arg(6);
    uint8_t data[4];
    uint32_t type, size;
    if (e->cls == EV_CLASS_KEYBOARD && name == PARAM_KEY_CODE && e->kind != EV_RAW_KEY_MODIFIERS_CHANGED) {
        type = TYPE_UINT32;
        size = 4;
        wr_be32(data, e->key_code);
    } else if (e->cls == EV_CLASS_KEYBOARD && name == PARAM_KEY_CHAR &&
               e->kind != EV_RAW_KEY_MODIFIERS_CHANGED) {
        type = TYPE_CHAR;
        size = 1;
        data[0] = e->chr;
    } else if (e->cls == EV_CLASS_KEYBOARD && name == PARAM_KEY_MODIFIERS) {
        type = TYPE_UINT32;
        size = 4;
        wr_be32(data, e->modifiers);
    } else {
        trap_return((uint32_t)EV_PARAM_NOT_FOUND_ERR);
        return;
    }
    if (desired != type && desired != TYPE_WILDCARD)
        trap_crash("GetEventParameter: can't return parameter 0x%08x as type 0x%08x", name, desired);
    if (out)
        memcpy(gm_ptr(out, buf_size < size ? buf_size : size), data, buf_size < size ? buf_size : size);
    if (out_type)
        gm_w32(out_type, type);
    if (out_size)
        gm_w32(out_size, size);
    trap_return(0);
}

/* ReceiveNextEvent(UInt32 numTypes, const EventTypeSpec *list,
   EventTimeout timeout, Boolean pullEvent, EventRef *outEvent) -> OSStatus.
   The timeout is a double in f1 (taking up r5-r6), so pullEvent and
   outEvent arrive in r7 and r8. A negative timeout means wait forever. A
   pulled event belongs to the caller, who releases it. */
static void h_receive_next_event(void) {
    uint32_t ntypes = trap_arg(0), list = trap_arg(1);
    double timeout = cpu_fpr(1);
    bool pull = (trap_arg(4) & 0xFF) != 0;
    uint32_t out = trap_arg(5);
    double deadline = now_seconds() + timeout;
    E.loop_depth++;
    for (;;) {
        events_pump();
        for (int k = 0; k < E.qcount; k++) {
            int idx = E.queue[(E.qhead + k) % EV_MAX_EVENTS];
            bool match = ntypes == 0;
            for (uint32_t j = 0; j < ntypes && !match; j++)
                match = gm_r32(list + 8 * j) == E.events[idx].cls &&
                        gm_r32(list + 8 * j + 4) == E.events[idx].kind;
            if (!match)
                continue;
            if (pull)
                dequeue_at(k);
            if (out)
                gm_w32(out, event_ref(idx));
            E.loop_depth--;
            trap_return(0);
            return;
        }
        if (timeout >= 0 && now_seconds() >= deadline) {
            misc_poll();
            break;
        }
        misc_wait(0.001);
    }
    E.loop_depth--;
    if (out)
        gm_w32(out, 0);
    trap_return((uint32_t)EV_LOOP_TIMED_OUT_ERR);
}

/* ---- guest calls: timers and the run loop ---- */

static void h_new_event_loop_timer_upp(void) { trap_return(trap_arg(0)); }
static void h_get_main_event_loop(void) { trap_return(EV_MAIN_LOOP); }

/* InstallEventLoopTimer(EventLoopRef loop, EventTimerInterval fireDelay,
   EventTimerInterval interval, EventLoopTimerUPP proc, void *data,
   EventLoopTimerRef *outTimer) -> OSStatus. The two intervals are doubles in
   f1 and f2; under the PowerPC calling convention they also take up r4-r7,
   so the remaining arguments arrive in r8-r10. */
static void h_install_event_loop_timer(void) {
    int i = 0;
    while (i < EV_MAX_TIMERS && E.timers[i].active)
        i++;
    if (i == EV_MAX_TIMERS)
        trap_crash("InstallEventLoopTimer: more than %d timers", EV_MAX_TIMERS);
    double delay = cpu_fpr(1), interval = cpu_fpr(2);
    if (delay < 0 || interval < 0)
        trap_crash("InstallEventLoopTimer: negative interval");
    E.timers[i].active = true;
    E.timers[i].running = false;
    E.timers[i].proc = trap_arg(5);
    E.timers[i].data = trap_arg(6);
    E.timers[i].next = now_seconds() + delay;
    E.timers[i].interval = interval;
    if (trap_arg(7))
        gm_w32(trap_arg(7), timer_ref(i));
    trap_return(0);
}

static void h_remove_event_loop_timer(void) {
    uint32_t ref = trap_arg(0);
    uint32_t i = ref - timer_ref(0);
    if (i >= EV_MAX_TIMERS || !E.timers[i].active)
        trap_crash("RemoveEventLoopTimer: 0x%08x is not a timer", ref);
    E.timers[i].active = false;
    trap_return(0);
}

/* Fires timers and dispatches queued events to the dispatcher target until
   QuitApplicationEventLoop. */
static void h_run_application_event_loop(void) {
    E.quit = false;
    E.loop_depth++;
    while (!E.quit) {
        events_pump();
        while (E.qcount > 0 && !E.quit) {
            int idx = dequeue_at(0);
            dispatch(idx, EV_DISPATCHER_TARGET);
            release(&E.events[idx]);
        }
        if (!E.quit)
            wait_until_next_timer();
    }
    E.loop_depth--;
}

static void h_quit_application_event_loop(void) { E.quit = true; }

void events_register(void) {
    trap_register("NewEventHandlerUPP", h_new_event_handler_upp);
    trap_register("GetApplicationEventTarget", h_get_application_event_target);
    trap_register("GetEventDispatcherTarget", h_get_event_dispatcher_target);
    trap_register("GetWindowEventTarget", h_get_window_event_target);
    trap_register("InstallEventHandler", h_install_event_handler);
    trap_register("InstallStandardEventHandler", h_install_standard_event_handler);
    trap_register("SendEventToEventTarget", h_send_event_to_event_target);
    trap_register("GetEventKind", h_get_event_kind);
    trap_register("GetEventParameter", h_get_event_parameter);
    trap_register("ReleaseEvent", h_release_event);
    trap_register("ReceiveNextEvent", h_receive_next_event);
    trap_register("NewEventLoopTimerUPP", h_new_event_loop_timer_upp);
    trap_register("GetMainEventLoop", h_get_main_event_loop);
    trap_register("InstallEventLoopTimer", h_install_event_loop_timer);
    trap_register("RemoveEventLoopTimer", h_remove_event_loop_timer);
    trap_register("RunApplicationEventLoop", h_run_application_event_loop);
    trap_register("QuitApplicationEventLoop", h_quit_application_event_loop);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ./build/loony_tests events_`
Expected: `23 passed, 0 failed, 0 skipped`. `events_quit_ignored_for_3_seconds_exits` takes 3 seconds. Full suite: `223 passed`, with no output besides test names.

- [ ] **Step 5: Commit**

```bash
git add src/events.h src/events.c tests/test_events.c
git commit -m "Carbon events: event objects, dispatch, keyboard input, quit"
```

---

### Task 5: SDL input, presenting at the pump, and wiring

**Files:**
- Modify: `src/display.h`, `src/display.c` (`display_poll`, input callbacks, Cmd-Q/Cmd-F, vsync switch)
- Modify: `src/qd.h`, `src/qd.c` (`QDFlushPortBuffer` marks the screen dirty; `qd_set_present` removed)
- Modify: `src/main.c`
- Modify: `tests/test_display.c`, `tests/test_qd.c`

**Interfaces:**
- Consumes: Task 4's `events_post_key`, `events_post_activation`, `events_request_quit`, `events_set_poll`, `events_set_screenshot`; `script_load` (Task 2); `misc_fixed_clock` (Task 3).
- Produces: `display_input { key, focus, quit }`, `void display_set_input(const display_input *)`, `void display_poll(void)`, `void display_set_vsync(bool on)`. `loony` reads `LOONY_SCRIPT` (exit 1 if it can't be loaded), polls SDL on every pump, and turns vsync off on the fixed clock.

- [ ] **Step 1: Write the failing tests**

```diff
diff --git a/tests/test_display.c b/tests/test_display.c
index 4292643..1b1bb2d 100644
--- a/tests/test_display.c
+++ b/tests/test_display.c
@@ -1,5 +1,6 @@
 #include "test.h"
 
+#include <SDL3/SDL.h>
 #include <stdlib.h>
 #include <unistd.h>
 
@@ -39,3 +40,52 @@ TEST(display_present_works_with_the_dummy_driver) {
     display_present_if_dirty(); /* nothing new: doesn't */
     CHECK_EQ(display_frames(), before + 2);
 }
+
+static int keys, quits, last_scancode;
+static bool last_down;
+static void on_key(int sc, bool down, bool repeat) {
+    (void)repeat;
+    keys++;
+    last_scancode = sc;
+    last_down = down;
+}
+static void on_focus(bool active) { (void)active; }
+static void on_quit(void) { quits++; }
+
+static void push_key(SDL_Scancode sc, bool down, SDL_Keymod mod) {
+    SDL_Event e;
+    memset(&e, 0, sizeof e);
+    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
+    e.key.scancode = sc;
+    e.key.down = down;
+    e.key.mod = mod;
+    SDL_PushEvent(&e);
+}
+
+TEST(display_poll_forwards_keys_but_keeps_cmd_q_and_cmd_f) {
+    gm_init();
+    mm_init();
+    qd_init(64, 48, 16);
+    display_present(); /* opens the (dummy) window */
+    static const display_input in = {on_key, on_focus, on_quit};
+    display_set_input(&in);
+    keys = quits = 0;
+    push_key(SDL_SCANCODE_Z, true, SDL_KMOD_NONE);
+    push_key(SDL_SCANCODE_Z, false, SDL_KMOD_NONE);
+    display_poll();
+    CHECK_EQ(keys, 2);
+    CHECK_EQ(last_scancode, SDL_SCANCODE_Z);
+    CHECK(!last_down);
+    push_key(SDL_SCANCODE_Q, true, SDL_KMOD_LGUI);
+    push_key(SDL_SCANCODE_F, true, SDL_KMOD_LGUI);
+    push_key(SDL_SCANCODE_F, false, SDL_KMOD_LGUI);
+    display_poll();
+    CHECK_EQ(keys, 2);
+    CHECK_EQ(quits, 1);
+    SDL_Event q;
+    memset(&q, 0, sizeof q);
+    q.type = SDL_EVENT_QUIT;
+    SDL_PushEvent(&q);
+    display_poll();
+    CHECK_EQ(quits, 2);
+}
```

```diff
diff --git a/tests/test_qd.c b/tests/test_qd.c
index 88cc2fc..3f9a73f 100644
--- a/tests/test_qd.c
+++ b/tests/test_qd.c
@@ -160,9 +160,6 @@ TEST(qd_set_depth_recreates_the_screen) {
     CHECK_EQ(b.right, 800);
 }
 
-static int presents;
-static void count_present(void) { presents++; }
-
 TEST(qd_begin_full_screen_makes_a_window_on_the_screen) {
     setup();
     call_import("SetDepth", 4, call_import("GetMainDevice", 0), 16u, 0u, 1u);
@@ -185,10 +182,9 @@ TEST(qd_begin_full_screen_makes_a_window_on_the_screen) {
     call_import("PaintRect", 1, rect(0, 0, 1, 1));
     CHECK_EQ(rd_be16(px.base), 0x7C00);
     CHECK(qd_take_dirty());
-    presents = 0;
-    qd_set_present(count_present);
-    call_import("QDFlushPortBuffer", 2, win, 0u);
-    CHECK_EQ(presents, 1);
+    CHECK(!qd_take_dirty());
+    call_import("QDFlushPortBuffer", 2, win, 0u); /* presented at the next pump */
+    CHECK(qd_take_dirty());
     call_import("HideWindow", 1, win);
     call_import("ShowWindow", 1, win);
     call_import("InvalWindowRect", 2, win, r);
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build`
Expected: the build fails with `unknown type name 'display_input'`.

- [ ] **Step 3: Write the implementation**

```diff
diff --git a/src/display.h b/src/display.h
index edce040..47d804d 100644
--- a/src/display.h
+++ b/src/display.h
@@ -10,10 +10,25 @@
    the process exits, including after a crash. */
 void display_init(void);
 
-/* Draws the current screen. Also handles window events: closing the window
-   exits cleanly. */
+/* Draws the current screen. */
 void display_present(void);
 
+/* Whether presenting waits for the display's refresh (the default). Turned
+   off for fixed-clock runs, where waiting would only slow the run down. */
+void display_set_vsync(bool on);
+
+/* Where display_poll sends host input. */
+typedef struct {
+    void (*key)(int scancode, bool down, bool repeat); /* SDL scancode */
+    void (*focus)(bool active);
+    void (*quit)(void); /* window closed or Cmd-Q */
+} display_input;
+void display_set_input(const display_input *in);
+
+/* Handles pending SDL events: keys go to the input callbacks, except Cmd-Q
+   (quit) and Cmd-F (toggle full screen), which the game never sees. */
+void display_poll(void);
+
 /* Writes the current screen as a PNG. */
 bool display_write_png(const char *path);
 
```

```diff
diff --git a/src/display.c b/src/display.c
index 06d361d..e59d60e 100644
--- a/src/display.c
+++ b/src/display.c
@@ -15,6 +15,8 @@ static struct {
     int tex_w, tex_h;
     unsigned frames;
     const char *screenshot;
+    display_input input;
+    bool no_vsync;
 } D;
 
 static uint8_t *screen_rgba(int *w, int *h) {
@@ -64,7 +66,7 @@ static bool open_window(int w, int h) {
         log_msg("display: can't create a window: %s (continuing without one)", SDL_GetError());
         return false;
     }
-    SDL_SetRenderVSync(D.renderer, 1);
+    SDL_SetRenderVSync(D.renderer, D.no_vsync ? 0 : 1);
     return true;
 }
 
@@ -90,12 +92,6 @@ void display_present(void) {
         SDL_RenderClear(D.renderer);
         SDL_RenderTexture(D.renderer, D.texture, NULL, NULL);
         SDL_RenderPresent(D.renderer);
-        SDL_Event e;
-        while (SDL_PollEvent(&e))
-            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
-                log_msg("window closed");
-                exit(0);
-            }
     }
     free(rgba);
 }
@@ -106,3 +102,52 @@ void display_present_if_dirty(void) {
 }
 
 unsigned display_frames(void) { return D.frames; }
+
+void display_set_input(const display_input *in) { D.input = *in; }
+
+void display_set_vsync(bool on) {
+    D.no_vsync = !on;
+    if (D.renderer)
+        SDL_SetRenderVSync(D.renderer, on ? 1 : 0);
+}
+
+void display_poll(void) {
+    if (!D.sdl_ok)
+        return;
+    SDL_Event e;
+    while (SDL_PollEvent(&e)) {
+        switch (e.type) {
+        case SDL_EVENT_QUIT:
+        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
+            if (D.input.quit)
+                D.input.quit();
+            break;
+        case SDL_EVENT_WINDOW_FOCUS_GAINED:
+        case SDL_EVENT_WINDOW_FOCUS_LOST:
+            if (D.input.focus)
+                D.input.focus(e.type == SDL_EVENT_WINDOW_FOCUS_GAINED);
+            break;
+        case SDL_EVENT_KEY_DOWN:
+        case SDL_EVENT_KEY_UP: {
+            bool down = e.type == SDL_EVENT_KEY_DOWN;
+            if (down && (e.key.mod & SDL_KMOD_GUI) && e.key.scancode == SDL_SCANCODE_Q) {
+                if (D.input.quit)
+                    D.input.quit();
+                break;
+            }
+            if ((e.key.mod & SDL_KMOD_GUI) && e.key.scancode == SDL_SCANCODE_F) {
+                if (down && !e.key.repeat) {
+                    bool fs = (SDL_GetWindowFlags(D.window) & SDL_WINDOW_FULLSCREEN) != 0;
+                    SDL_SetWindowFullscreen(D.window, !fs);
+                }
+                break;
+            }
+            if (D.input.key)
+                D.input.key((int)e.key.scancode, down, e.key.repeat);
+            break;
+        }
+        default:
+            break;
+        }
+    }
+}
```

```diff
diff --git a/src/qd.h b/src/qd.h
index 2d58ca6..a757d38 100644
--- a/src/qd.h
+++ b/src/qd.h
@@ -82,10 +82,6 @@ void qd_screen(qd_pixels *out, qd_palette *pal);
 
 /* True if anything drew to the screen since the last call. */
 bool qd_take_dirty(void);
-/* Called when the game flushes a window to the screen (QDFlushPortBuffer). */
-typedef void (*qd_present_fn)(void);
-void qd_set_present(qd_present_fn fn);
-
 uint32_t qd_current_port(void);
 
 /* Registers the QuickDraw, GWorld and window imports. */
```

```diff
diff --git a/src/qd.c b/src/qd.c
index 5886e24..cd5e510 100644
--- a/src/qd.c
+++ b/src/qd.c
@@ -28,7 +28,6 @@ static struct {
     int nports;
     uint32_t cur_port, cur_device;
     bool dirty;
-    qd_present_fn present;
     int saved_w, saved_h; /* screen size before BeginFullScreen */
 } Q;
 
@@ -275,7 +274,6 @@ bool qd_take_dirty(void) {
     return d;
 }
 
-void qd_set_present(qd_present_fn fn) { Q.present = fn; }
 
 static void resize_screen(int w, int h, int depth) {
     free_pixmap_contents(Q.screen_pm);
@@ -469,10 +467,8 @@ static void h_inval_window_rect(void) {
     Q.dirty = true;
 }
 
-static void h_qd_flush_port_buffer(void) {
-    if (Q.present)
-        Q.present();
-}
+/* QDFlushPortBuffer(port, region): the screen is presented at the next pump. */
+static void h_qd_flush_port_buffer(void) { Q.dirty = true; }
 
 /* BeginFullScreen(Ptr *restoreState, GDHandle gd, short *desiredWidth,
    short *desiredHeight, WindowRef *newWindow, RGBColor *eraseColor, long flags) */
```

```diff
diff --git a/src/main.c b/src/main.c
index 6036301..eadc2d6 100644
--- a/src/main.c
+++ b/src/main.c
@@ -16,6 +16,7 @@
 #include "misc.h"
 #include "qd.h"
 #include "rsrc.h"
+#include "script.h"
 #include "sound.h"
 #include "trap.h"
 #include "util.h"
@@ -69,8 +70,18 @@ int main(int argc, char **argv) {
     files_init(dir);
     sound_init();
     display_init();
-    qd_set_present(display_present);
     events_set_present(display_present_if_dirty);
+    events_set_poll(display_poll);
+    events_set_screenshot(display_write_png);
+    static const display_input input = {events_post_key, events_post_activation,
+                                        events_request_quit};
+    display_set_input(&input);
+    display_set_vsync(!misc_fixed_clock());
+    const char *script = getenv("LOONY_SCRIPT");
+    if (script && *script && !script_load(script, err, sizeof err)) {
+        fprintf(stderr, "loony: can't load the script %s: %s\n", script, err);
+        return 1;
+    }
     misc_set_idle(events_pump);
 
     const char **names = calloc(img.pef.nimports ? img.pef.nimports : 1, sizeof *names);
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ./build/loony_tests display_ && ./build/loony_tests qd_`
Expected: `3 passed, 0 failed, 0 skipped`, then `17 passed, 0 failed, 0 skipped`. Full suite: `224 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/display.h src/display.c src/qd.h src/qd.c src/main.c tests/test_display.c tests/test_qd.c
git commit -m "Feed SDL keys to the game; present at the pump; Cmd-Q and Cmd-F"
```

---

### Task 6: Golden-frame runs, a smaller translation cache, and how to play

**Files:**
- Modify: `src/cpu_unicorn.c` (64 MB translation cache)
- Modify: `tests/test_run.c` (three tests)
- Modify: `README.md`, `.gitignore`

**Interfaces:**
- Consumes: everything above.
- Produces: the deterministic end-to-end tests (the opening frames, and a game started from the menu), and the unreadable-script test.

- [ ] **Step 1: Write the failing tests**

```diff
diff --git a/tests/test_run.c b/tests/test_run.c
index 4066340..8c88125 100644
--- a/tests/test_run.c
+++ b/tests/test_run.c
@@ -45,6 +45,108 @@ TEST(run_plays_the_opening_headless) {
     free(png);
 }
 
+/* Fixed-clock runs are deterministic, so their frames can be compared with
+   golden hashes (FNV-1a32 of the PNG file, which is uncompressed). The
+   goldens were recorded from runs whose frames were checked by eye: the
+   title, and a one-player game started from the menu. */
+
+static char script_path[1024];
+
+static void run_loony_scripted(void *dir) {
+    setenv("LOONY_FIXED_CLOCK", "1", 1);
+    setenv("LOONY_SCRIPT", script_path, 1);
+    run_loony(dir);
+}
+
+static void tmp_name(char *buf, size_t cap, const char *what) {
+    const char *t = getenv("TMPDIR");
+    snprintf(buf, cap, "%s/loony-%s-XXXXXX", t && *t ? t : "/tmp", what);
+    int fd = mkstemp(buf);
+    if (fd >= 0)
+        close(fd);
+}
+
+static int by_tick(const void *a, const void *b) {
+    unsigned long x = strtoul(*(char *const *)a, NULL, 10), y = strtoul(*(char *const *)b, NULL, 10);
+    return x < y ? -1 : x > y;
+}
+
+/* Runs the game with a script (lines in any order, sorted by tick here),
+   returning the exit status; shots[i] gets the hash of the screenshot taken
+   at ticks[i]. */
+static int run_script(const char *actions, const uint32_t *ticks, uint32_t *shots, int nshots,
+                      char *out, size_t outlen) {
+    char pngs[4][1024], lines[32][1100];
+    char *order[32];
+    int n = 0;
+    for (int i = 0; i < nshots; i++) {
+        tmp_name(pngs[i], sizeof pngs[i], "shot");
+        snprintf(lines[n++], sizeof lines[0], "%u screenshot %s", ticks[i], pngs[i]);
+    }
+    for (const char *p = actions; *p && n < 32;) {
+        const char *eol = strchr(p, '\n');
+        size_t len = eol ? (size_t)(eol - p) : strlen(p);
+        snprintf(lines[n++], sizeof lines[0], "%.*s", (int)len, p);
+        p += len + (eol != NULL);
+    }
+    for (int i = 0; i < n; i++)
+        order[i] = lines[i];
+    qsort(order, (size_t)n, sizeof order[0], by_tick); /* qsort isn't stable; ticks here are distinct */
+    tmp_name(script_path, sizeof script_path, "script");
+    FILE *f = fopen(script_path, "w");
+    for (int i = 0; i < n; i++)
+        fprintf(f, "%s\n", order[i]);
+    fclose(f);
+    int status = test_run_child(run_loony_scripted, (void *)test_game_dir(), out, outlen);
+    for (int i = 0; i < nshots; i++) {
+        size_t len = 0;
+        uint8_t *png = read_file(pngs[i], &len);
+        shots[i] = png ? fnv1a32(png, len) : 0;
+        free(png);
+        unlink(pngs[i]);
+    }
+    unlink(script_path);
+    return status;
+}
+
+TEST(run_opening_frames_match_their_goldens) {
+    SKIP_UNLESS_GAME();
+    uint32_t ticks[2] = {30, 240}, shots[2];
+    char out[32768];
+    int status = run_script("300 quit\n", ticks, shots, 2, out, sizeof out);
+    CHECK_EQ(status, 0);
+    CHECK_CONTAINS(out, "loony: sending the quit Apple Event");
+    CHECK_EQ(shots[0], 0xABFE3C2Bu); /* the LittleWing logo */
+    CHECK_EQ(shots[1], 0x4C3A7003u); /* the title */
+}
+
+TEST(run_a_game_starts_from_the_menu) {
+    SKIP_UNLESS_GAME();
+    /* Esc ends the self-playing demo, Esc again opens the menu, Return picks
+       "1 player"; then pull and release the plunger. */
+    const char *actions = "1720 down esc\n1724 up esc\n1800 down esc\n1804 up esc\n"
+                          "1900 down return\n1906 up return\n2100 down return\n"
+                          "2190 up return\n2600 quit\n";
+    uint32_t ticks[2] = {1880, 2500}, shots[2];
+    char out[32768];
+    int status = run_script(actions, ticks, shots, 2, out, sizeof out);
+    CHECK_EQ(status, 0);
+    CHECK_EQ(shots[0], 0xADE78151u); /* the menu */
+    CHECK_EQ(shots[1], 0xA162CB3Du); /* ball 1 in play, "DEMO VERSION TIME LEFT" */
+}
+
+static void run_loony_bad_script(void *dir) {
+    setenv("LOONY_SCRIPT", "/nonexistent/loony.script", 1);
+    run_loony(dir);
+}
+
+TEST(run_reports_an_unreadable_script) {
+    SKIP_UNLESS_GAME();
+    char out[4096];
+    CHECK_EQ(test_run_child(run_loony_bad_script, (void *)test_game_dir(), out, sizeof out), 1);
+    CHECK_CONTAINS(out, "can't load the script /nonexistent/loony.script");
+}
+
 /* Review Focus 1: wrong or missing game folder. */
 TEST(run_reports_missing_game_folder) {
     char out[4096];
```

- [ ] **Step 2: Run the tests to verify they fail, then pass**

Run: `cmake --build build && ./build/loony_tests run_`
Expected: `10 passed, 0 failed, 0 skipped`. These tests pin behavior Tasks 1–5 already built, so they pass immediately. To see that they can fail, change one golden hash by a digit and rerun: the test reports the actual hash. Then put the value back.

- [ ] **Step 3: Cap the translation cache**

```diff
diff --git a/src/cpu_unicorn.c b/src/cpu_unicorn.c
index 688574a..59dd094 100644
--- a/src/cpu_unicorn.c
+++ b/src/cpu_unicorn.c
@@ -10,6 +10,7 @@
 #include "util.h"
 
 #define MSR_FP 0x2000u
+#define TCG_BUFFER_SIZE (64u << 20)
 
 struct cpu_context {
     uc_context *uc_ctx;
@@ -75,6 +76,8 @@ void cpu_init(void) {
         cpu_shutdown();
     check(uc_open(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN, &uc), "uc_open");
     check(uc_ctl_set_cpu_model(uc, UC_CPU_PPC32_750_V3_1), "set CPU model");
+    /* The default translation cache is 1 GB; the game's code is 280 KB. */
+    check(uc_ctl_set_tcg_buffer_size(uc, TCG_BUFFER_SIZE), "set translation cache size");
     const gm_region *regions;
     int n = gm_regions(&regions);
     for (int i = 0; i < n; i++) {
```

- [ ] **Step 4: Check the memory and speed**

Run:
```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release
/usr/bin/time -l env SDL_VIDEO_DRIVER=dummy LOONY_EXIT_AFTER=1200 build-release/loony 2>&1 | grep -E 'exiting|maximum resident'
```
Expected: `exiting after 1200 ticks`, and a maximum resident size around 100 MB (it was about 1.1 GB with Unicorn's default cache). Then `./build/loony_tests`: `227 passed, 0 failed, 0 skipped`.

- [ ] **Step 5: Play it**

Run: `./build-release/loony`. Wait for the table, press Esc twice, then Return twice. A game starts ("DEMO VERSION, TIME LEFT"). Z and / flip, and holding then releasing Return launches the ball. Cmd-F toggles full screen, and Cmd-Q quits.

- [ ] **Step 6: Update the README and .gitignore**

```diff
diff --git a/README.md b/README.md
index be76d25..78bf407 100644
--- a/README.md
+++ b/README.md
@@ -7,30 +7,67 @@ Personal use only. This repo contains no game files; point it at your own copy.
 ## Build
 
 ```bash
-brew install unicorn cmake pkg-config
-cmake -S . -B build
+brew install unicorn sdl3 cmake pkg-config
+cmake -S . -B build                                     # Debug (sanitizers): for development
 cmake --build build
 ./build/loony_tests          # all tests; ./build/loony_tests <substring> to filter
+
+cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release  # for playing
+cmake --build build-release
 ```
 
-## Run
+## Play
+
+```bash
+./build-release/loony                       # uses /Applications/Loony Labyrinth
+./build-release/loony "/path/to/game folder"
+```
+
+The game starts with its opening and then a self-playing demo. To play:
+
+| Key | Does |
+|---|---|
+| Esc | Stops the demo; press again for the menu (start a game, options, quit). In a game: pause / menu |
+| Return | Choose in the menu; hold and release to pull the plunger |
+| Z | Left flipper |
+| / | Right flipper |
+| Space | Nudge |
+| Cmd-F | Full screen on or off |
+| Cmd-Q or closing the window | Quit |
+
+The keys can be changed from the game's OPTIONS menu. This is the shareware
+version: games are time-limited, and the two startup alerts ("Play Demo" and the
+key list) are answered automatically. There is no sound yet (milestone 5), and
+registering, saved preferences and high scores come with milestone 6.
+
+## Debugging
 
 ```bash
-./build/loony                         # uses /Applications/Loony Labyrinth
-./build/loony "/path/to/game folder"
 LOONY_TRACE=imports ./build/loony     # log every OS call
 LOONY_TRACE=imports,calls ./build/loony   # also log each call into the game (callbacks)
 LOONY_TRACE=lowmem ./build/loony      # log the first write to each low-memory address
 LOONY_STUB=all ./build/loony          # unimplemented OS calls return 0 instead of crashing
 LOONY_EXIT_AFTER=600 LOONY_SCREENSHOT=shot.png ./build/loony   # run 10 s, save the last frame
 SDL_VIDEO_DRIVER=dummy ./build/loony  # no window (with LOONY_SCREENSHOT for headless runs)
+LOONY_FIXED_CLOCK=1 LOONY_SCRIPT=play.txt SDL_VIDEO_DRIVER=dummy ./build/loony
+```
+
+`LOONY_FIXED_CLOCK=1` makes time advance only when the game waits, so a run is
+the same every time and doesn't depend on the host's speed. `LOONY_SCRIPT`
+plays keys and takes screenshots at given ticks (1/60 s since launch), one
+action per line:
+
+```
+1720 down esc
+1724 up esc
+1880 screenshot menu.png
+2600 quit
 ```
 
-The original game files are only ever read, never modified. Today the game
-plays its opening (the LittleWing logo and the title) and then runs its attract
-mode on the table, silently and without input: keyboard input arrives in
-milestone 4 and sound in milestone 5. The two shareware alerts at startup are
-answered with their default button ("Play Demo", then "OK") until dialogs are
-drawn (milestone 6). The emulated screen is 800x600, the size the game expects.
+Key names are those in `src/keymap.c` (`z`, `slash`, `return`, `space`, `esc`,
+`lshift`, `rshift`, ...).
+
+The original game files are only ever read, never modified. The emulated screen
+is 800x600, the size the game expects.
 
 Design: `docs/superpowers/specs/2026-09-30-loony-shim-design.md`
```

```diff
diff --git a/.gitignore b/.gitignore
index 7e95214..f1226b7 100644
--- a/.gitignore
+++ b/.gitignore
@@ -1,4 +1,5 @@
 build/
+build-*/
 *.png
 /game/
 .DS_Store
```

- [ ] **Step 7: Commit**

```bash
git add src/cpu_unicorn.c tests/test_run.c README.md .gitignore
git commit -m "Golden-frame scripted runs; 64 MB translation cache; how to play"
```

---

## What comes next (not part of this plan)

- **Milestone 5 (sound):** replace the silent Sound Manager's timing with an SDL audio mixer, playing the `ESnd`/`snd` sample buffers the game passes to `bufferCmd`/`soundCmd`.
- **Milestone 6 (dialogs, files, preferences):**
  - drawn `Alert`/`GetNewDialog`/`ModalDialog`, including the key-code registration dialog;
  - writing files to `~/Library/Application Support/loony-shim/`;
  - saving CFPreferences.

  These are what you need for high scores, settings that persist, and the full game.
- **Still open from earlier reviews:**
  - files.c path handling (`..`, `::`, buffer sizes);
  - the PICT clip bounding box;
  - restoring the depth in `EndFullScreen`;
  - palettes padded to 256 entries.
- **For you to judge:** the playtest (flipper feel, ball physics against the original, frame pacing) and approving the golden frames.
