# Plan 2: Core Services Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the Memory Manager, Resource Manager, Gestalt and miscellaneous calls, and the CFString/CFNumber/CFPreferences calls the game makes at startup, so the real game runs through startup and stops at its first dialog (`Alert`), which needs graphics.

**Architecture:** Each Carbon module is one `src/<module>.c` with a small header and a `<module>_register()` function that installs its import handlers with `trap.c`. State the game can see (heap blocks, handles, resource data, strings it passes) lives in guest memory, big-endian. State it reaches only through calls (CF objects, preferences, Apple Event handlers, the clock) lives in host C structs. Guest-side tests call imports through the real trap dispatch with a small harness (`tests/harness.h`).

**Tech Stack:** C (gnu11), clang, CMake ≥ 3.20, Unicorn 2. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-09-30-loony-shim-design.md` (milestone 2). Plan 1 (`docs/superpowers/plans/2026-09-30-plan1-loader-and-dispatch.md`) built the loader, `trap.c` and `guest_call`.

## Global Constraints

- Repo: `~/dev/loony-shim`, branch `main`. All paths below are relative to it.
- Game files in `/Applications/Loony Labyrinth` are read-only inputs. Never write, move or modify them. Never copy them or anything extracted from them into the repo.
- C11 with GNU extensions (`gnu11`), clang, `-Wall -Wextra -Werror` in every build. Debug builds (the default) add `-fsanitize=address,undefined`. Release is `-O2`.
- Dependencies come only from Homebrew: `unicorn`, `cmake`, `pkg-config`.
- Unimplemented import, guest crash, or an unsupported option in an implemented call: stop with the full crash report (`trap_crash`) and exit **2**. Don't guess and keep going. Exceptions: unknown `Gestalt` selectors (and, later, sound command problems) are logged, not fatal.
- Bad command-line input or unreadable/unloadable game files exit **1** with one `loony: can't ...` line.
- All guest code runs on the main thread.
- Guest addresses inside the code section are printed as `code+0xNNNNN`.
- Tests that need the game files are **skipped**, not failed, if the executable isn't there (`SKIP_UNLESS_GAME()`).
- Each Carbon module registers its handlers with `trap.c` from a `*_register()` function. Modules share state only through `guest_mem`, `memmgr` and small public headers.
- Memory map (unchanged): heap `0x0100_0000`–`0x04FF_FFFF` (64 MB), tag space for opaque host objects from `0x0800_0000` (never dereferenced). CF objects use `0x0800_0000`–`0x08FF_FFFF`.
- Handles never move and are never purged. `HLock`, `HUnlock`, `HPurge`, `HNoPurge`, `MoveHHi`, `HGetState`, `HSetState` only record flags. `SetPtrSize` fails if the block can't grow in place. Freed memory is filled with `0xDEADBEEF`.
- `GetResource` and `GetNamedResource` load a resource into a new guest handle on first use and cache it; repeat calls return the same handle. `ReleaseResource` frees the handle and clears the cache entry. `CurResFile` returns the application file's refNum.
- `Gestalt` is a fixed table: OS X 10.2.8 (`sysv` = 0x1028), a G3 CPU with no AltiVec (`ppcf` bit 4 clear), Carbon present. Unknown selectors return `gestaltUndefSelectorErr` (-5551) and are logged.
- `TickCount` is the host monotonic clock at 60 Hz, starting at 0 on launch. `Microseconds` is the same clock in microseconds.
- CFString and CFNumber are host objects referred to by tag-space IDs, with reference counts. `kCFPreferencesCurrentApplication` is a pre-made CFString ID.

## Facts measured from the real game (the tests assert these)

| Fact | Value |
|---|---|
| Resource fork | `LOONY LABYRINTH 3.0.1/..namedfork/rsrc`, 3,856,669 bytes, data at 256, map at 3,843,455 (13,214 bytes) |
| Resource map | 57 types, 877 resources. `ESnd` 125, `PICT` 7, `snd ` 6, `clut` 3, `DLOG` 6, `ALRT` 23, `WIND` 3, `MENU` 6 |
| Sample resources | `Visu` 128: 36 bytes, FNV-1a32 `0xC4405513`, first byte `0x6C`. `PICT` 128: 18,464 bytes, FNV-1a32 `0xE0CC01FF`. `SIZE` -1: 10 bytes. `ALRT` 901: attributes `0x20` (purgeable). `Flip` 1000 is named `Buttom Left` |
| `STR#` 128 | 25 strings; string 2 is `Untitled`. `STR#` 9100 has 0 strings |
| `ALRT` 901 | The shareware dialog: "Play Demo", "Quit", "Buy Now", "Enter Key-Code" |
| Startup trace | `Gestalt` (`ppcf`, `vm  `, `sysv`, `cbon`), `NewPtrClear`, `NewPtr`, `InitCursor`, about 130 CFPreferences/CFString/CFNumber/CFRelease calls, `Gestalt('mach')`, then `Alert(901)` from `code+0x0dd20` as import #140 |
| After `Alert` (stubbed) | Event handler setup, `GetMainDevice`, `SetDepth(16)`, `BeginFullScreen`, `GetCTable(8)`, GWorld setup, `GetResource('Visu', 128)`. `Gestalt('lram')` and `Gestalt('ram ')` are queried there; the game quits if they're undefined |

## Decisions this plan makes

- **CF objects and preferences go in `src/cf.c`**, not `files.c` as the spec's component list says. `files.c` will hold the FSSpec file calls (milestone 6) and is already the largest module on paper; CF is a separate concern. Preferences are kept in memory only. Saving them to `prefs.plist` on `CFPreferencesAppSynchronize` is milestone 6.
- **Low-memory write logging is opt-in** (`LOONY_TRACE=lowmem`). A Unicorn write hook slows every guest store, and the spec only asks for the writes to be visible. Each address is logged once.
- **Weak imports stay bound to trap addresses.** On a real 10.2.8 system every weak import resolves, so "present" is the faithful answer. No change.
- **Invalid pointers and handles passed by the guest crash** with the call's name (for example `DisposePtr: 0x... is not an allocated pointer`). A real Mac would corrupt its heap; stopping is the spec's fail-loudly rule. `NULL` handles set `MemError` to `nilHandleErr` as on a real Mac.
- **`DisposeHandle` on a resource handle crashes** (use `ReleaseResource`). If the trace shows the game relies on it, a later plan can relax this.
- **`Alert` and everything after it are out of scope.** `Alert` needs dialogs drawn on the screen (milestones 3 and 6). Plan 3 starts from it.

## Review Focus

1. **Guest frees a bad pointer or frees twice.** Expect a crash report naming the call and address, not silent heap corruption. Tests in Tasks 2 and 3.
2. **An allocation larger than the heap.** Expect `NewPtr`/`NewHandleClear` to return 0 with `MemError` = -108, and the heap left intact. Tests in Tasks 2 and 3.
3. **The game asks for a resource that doesn't exist.** Expect `GetResource` to return 0 with `ResError` = -192, not a crash. Test in Task 5.
4. **A corrupt or truncated resource fork.** Expect `rsrc_open` to return an error message with no out-of-bounds read under ASan. Tests in Task 4.
5. **The guest's buffer is too small for a CFString.** Expect `CFStringGetCString` to return false and write nothing. Test in Task 7.

---

### Task 1: Guest string helpers and the import-call test harness

**Files:**
- Modify: `src/guest_mem.h`, `src/guest_mem.c`
- Create: `tests/harness.h`, `tests/test_harness.c`

**Interfaces:**
- Consumes: `gm_r8`, `gm_ptr` (Plan 1); `fresh_machine()` (`tests/ppc.h`); `trap_init`, `guest_call`, `trap_register` (Plan 1).
- Produces:
  - `void gm_read_pstr(uint32_t addr, char out[256])`, `void gm_write_pstr(uint32_t addr, const char *s)`, `bool gm_read_cstr(uint32_t addr, char *out, size_t cap)`, `void gm_write_cstr(uint32_t addr, const char *s)`
  - `tests/harness.h`: `harness_init(const char *const *names, uint32_t n)`, `uint32_t call_import(const char *name, int nargs, ...)` (word arguments, returns r3), `uint32_t scratch(uint32_t n)` (zeroed, 16-byte-aligned guest memory in the image area)

The harness gives each import a transition vector that points straight at its trap address. `guest_call` on that vector stops at the trap immediately, runs the handler, and returns to `RETURN_MAGIC`, so handlers are tested through the real dispatch loop without hand-assembled guest code.

- [ ] **Step 1: Write the failing tests**

`tests/harness.h`:
```c
#pragma once
/* Calls imports through the real trap dispatch, without any guest code: each
   import's transition vector points straight at its trap address, so
   guest_call() stops at the trap, runs the handler, and returns. */
#include <stdarg.h>
#include <string.h>

#include "ppc.h"
#include "trap.h"
#include "util.h"

#define HARNESS_TV_BASE (GUEST_IMAGE_BASE + 0x80000)
#define HARNESS_SCRATCH (GUEST_IMAGE_BASE + 0x100000)

static struct {
    const char *const *names;
    uint32_t n;
    uint32_t scratch_next;
} harness;

/* Fresh machine and trap table with these import names. Register handlers after. */
static inline void harness_init(const char *const *names, uint32_t n) {
    fresh_machine();
    trap_init(n, names, GUEST_IMAGE_BASE, 0x10000);
    for (uint32_t i = 0; i < n; i++) {
        gm_w32(HARNESS_TV_BASE + 8 * i, GUEST_TRAP_ADDR(i));
        gm_w32(HARNESS_TV_BASE + 8 * i + 4, 0);
    }
    harness.names = names;
    harness.n = n;
    harness.scratch_next = HARNESS_SCRATCH;
}

/* Calls the named import with nargs word arguments and returns its r3. */
static inline uint32_t call_import(const char *name, int nargs, ...) {
    uint32_t args[8] = {0};
    va_list ap;
    va_start(ap, nargs);
    for (int i = 0; i < nargs; i++)
        args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    for (uint32_t i = 0; i < harness.n; i++)
        if (strcmp(harness.names[i], name) == 0)
            return guest_call(HARNESS_TV_BASE + 8 * i, nargs, args);
    fatal("harness: %s is not in the import table", name);
}

/* n bytes of zeroed guest memory in the image area, 16-byte aligned. */
static inline uint32_t scratch(uint32_t n) {
    uint32_t a = harness.scratch_next;
    harness.scratch_next = (a + n + 15) & ~15u;
    memset(gm_ptr(a, n ? n : 1), 0, n);
    return a;
}
```

`tests/test_harness.c`:
```c
#include "test.h"

#include "harness.h"

static void h_add(void) {
    trap_return(trap_arg(0) + trap_arg(1));
}

TEST(harness_calls_an_import_through_dispatch) {
    static const char *const names[] = {"First", "Add"};
    harness_init(names, 2);
    trap_register("Add", h_add);
    CHECK_EQ(call_import("Add", 2, 40u, 2u), 42);
}

TEST(harness_scratch_is_aligned_and_zeroed) {
    static const char *const names[] = {"Unused"};
    harness_init(names, 1);
    uint32_t a = scratch(3), b = scratch(1);
    CHECK_EQ(a % 16, 0);
    CHECK_EQ(b, a + 16);
    CHECK_EQ(gm_r8(a), 0);
}

TEST(gm_pascal_strings_round_trip) {
    static const char *const names[] = {"Unused"};
    harness_init(names, 1);
    uint32_t a = scratch(256);
    gm_write_pstr(a, "Hello");
    CHECK_EQ(gm_r8(a), 5);
    CHECK_EQ(gm_r8(a + 1), 'H');
    char s[256];
    gm_read_pstr(a, s);
    CHECK_STR(s, "Hello");
}

TEST(gm_pascal_string_truncates_at_255) {
    static const char *const names[] = {"Unused"};
    harness_init(names, 1);
    char longs[300];
    memset(longs, 'x', 299);
    longs[299] = '\0';
    uint32_t a = scratch(300);
    gm_write_pstr(a, longs);
    CHECK_EQ(gm_r8(a), 255);
    char s[256];
    gm_read_pstr(a, s);
    CHECK_EQ(strlen(s), 255);
}

TEST(gm_c_strings_round_trip_and_report_truncation) {
    static const char *const names[] = {"Unused"};
    harness_init(names, 1);
    uint32_t a = scratch(32);
    gm_write_cstr(a, "abcdef");
    char s[16];
    CHECK(gm_read_cstr(a, s, sizeof s));
    CHECK_STR(s, "abcdef");
    char small[4];
    CHECK(!gm_read_cstr(a, small, sizeof small));
    CHECK_STR(small, "abc");
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake -S . -B build && cmake --build build`
Expected: the build fails with `call to undeclared function 'gm_write_pstr'`.

- [ ] **Step 3: Add the string helpers**

```diff
diff --git a/src/guest_mem.h b/src/guest_mem.h
index c6a93e2..955c946 100644
--- a/src/guest_mem.h
+++ b/src/guest_mem.h
@@ -1,5 +1,6 @@
 #pragma once
 #include <stdbool.h>
+#include <stddef.h>
 #include <stdint.h>
 
 /* The guest's 32-bit address space. See the memory map in the spec. */
@@ -59,3 +60,16 @@ uint32_t gm_r32(uint32_t addr);
 void gm_w8(uint32_t addr, uint8_t v);
 void gm_w16(uint32_t addr, uint16_t v);
 void gm_w32(uint32_t addr, uint32_t v);
+
+/* Guest strings. Pascal strings are a length byte followed by up to 255
+   Mac Roman bytes; C strings are NUL-terminated. */
+
+/* Copies the Pascal string at addr into out as a C string (at most 255 bytes). */
+void gm_read_pstr(uint32_t addr, char out[256]);
+/* Writes s as a Pascal string at addr, truncated to 255 bytes. */
+void gm_write_pstr(uint32_t addr, const char *s);
+/* Copies the C string at addr into out. Returns false (out truncated, still
+   NUL-terminated) if no NUL appears in the first cap - 1 bytes. */
+bool gm_read_cstr(uint32_t addr, char *out, size_t cap);
+/* Writes s and its NUL at addr. */
+void gm_write_cstr(uint32_t addr, const char *s);
```

```diff
diff --git a/src/guest_mem.c b/src/guest_mem.c
index 93f2e97..62aad18 100644
--- a/src/guest_mem.c
+++ b/src/guest_mem.c
@@ -1,6 +1,7 @@
 #include "guest_mem.h"
 
 #include <stdio.h>
+#include <string.h>
 #include <sys/mman.h>
 
 #include "util.h"
@@ -72,3 +73,34 @@ uint32_t gm_r32(uint32_t addr) { return rd_be32(gm_ptr(addr, 4)); }
 void gm_w8(uint32_t addr, uint8_t v) { *gm_ptr(addr, 1) = v; }
 void gm_w16(uint32_t addr, uint16_t v) { wr_be16(gm_ptr(addr, 2), v); }
 void gm_w32(uint32_t addr, uint32_t v) { wr_be32(gm_ptr(addr, 4), v); }
+
+void gm_read_pstr(uint32_t addr, char out[256]) {
+    uint8_t n = gm_r8(addr);
+    memcpy(out, gm_ptr(addr + 1, n), n);
+    out[n] = '\0';
+}
+
+void gm_write_pstr(uint32_t addr, const char *s) {
+    size_t n = strlen(s);
+    if (n > 255)
+        n = 255;
+    uint8_t *p = gm_ptr(addr, (uint32_t)n + 1);
+    p[0] = (uint8_t)n;
+    memcpy(p + 1, s, n);
+}
+
+bool gm_read_cstr(uint32_t addr, char *out, size_t cap) {
+    for (size_t i = 0; i + 1 < cap; i++) {
+        out[i] = (char)gm_r8(addr + (uint32_t)i);
+        if (out[i] == '\0')
+            return true;
+    }
+    if (cap)
+        out[cap - 1] = '\0';
+    return false;
+}
+
+void gm_write_cstr(uint32_t addr, const char *s) {
+    size_t n = strlen(s) + 1;
+    memcpy(gm_ptr(addr, (uint32_t)n), s, n);
+}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ./build/loony_tests harness_ && ./build/loony_tests gm_`
Expected: `2 passed, 0 failed, 0 skipped`, then `9 passed, 0 failed, 0 skipped`. The full suite (`./build/loony_tests`) shows `65 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/guest_mem.h src/guest_mem.c tests/harness.h tests/test_harness.c
git commit -m "Guest string helpers and a harness for calling imports in tests"
```

---

### Task 2: Memory Manager allocator

**Files:**
- Create: `src/memmgr.h`, `src/memmgr.c`
- Create: `tests/test_memmgr.c`

**Interfaces:**
- Consumes: `gm_r32`, `gm_w32`, `gm_ptr`, `GUEST_HEAP_BASE`, `GUEST_HEAP_SIZE` (Plan 1); `wr_be32` (`util.h`).
- Produces (all of `src/memmgr.h`; `mm_register` is implemented in Task 3):
  - Error codes `MM_NO_ERR`, `MM_MEM_FULL_ERR` (-108), `MM_NIL_HANDLE_ERR` (-109), `MM_MEM_WZ_ERR` (-111); state bits `MM_STATE_LOCKED` (0x80), `MM_STATE_PURGEABLE` (0x40), `MM_STATE_RESOURCE` (0x20); `MM_HEADER_SIZE` (16)
  - `void mm_init(void)`
  - `uint32_t mm_new_ptr(uint32_t size, bool clear)`, `int16_t mm_dispose_ptr(uint32_t p)`, `bool mm_is_ptr(uint32_t p)`, `uint32_t mm_ptr_size(uint32_t p)`, `int16_t mm_set_ptr_size(uint32_t p, uint32_t size)`
  - `uint32_t mm_new_handle(uint32_t size, bool clear)`, `int16_t mm_dispose_handle(uint32_t h)`, `bool mm_is_handle(uint32_t h)`, `uint32_t mm_handle_size(uint32_t h)`, `uint32_t mm_recover_handle(uint32_t p)`, `uint8_t mm_handle_state(uint32_t h)`, `void mm_set_handle_state(uint32_t h, uint8_t state)`
  - `uint32_t mm_free_bytes(void)`, `int16_t mm_error(void)`, `void mm_set_error(int16_t err)`

Design: first-fit over the whole heap, with a 16-byte big-endian header before every block (magic, capacity, logical size, owner). Free blocks merge with the free blocks after them when freed and while an allocation walks past them. A handle is a 4-byte master pointer block whose payload points at a separate data block; the data block's header records its handle, so `RecoverHandle` and validity checks are exact.

- [ ] **Step 1: Write the failing test**

`tests/test_memmgr.c`:
```c
#include "test.h"

#include "guest_mem.h"
#include "memmgr.h"

#define FULL (GUEST_HEAP_SIZE - MM_HEADER_SIZE)

static void fresh_heap(void) {
    gm_init();
    mm_init();
}

TEST(mm_new_ptr_is_aligned_inside_the_heap) {
    fresh_heap();
    uint32_t p = mm_new_ptr(10, false);
    CHECK(p >= GUEST_HEAP_BASE && p < GUEST_HEAP_BASE + GUEST_HEAP_SIZE);
    CHECK_EQ(p % 16, 0);
    CHECK(mm_is_ptr(p));
    CHECK_EQ(mm_ptr_size(p), 10);
}

TEST(mm_new_ptr_clear_zeroes_reused_memory) {
    fresh_heap();
    uint32_t p = mm_new_ptr(64, false);
    gm_w32(p, 0x12345678u);
    CHECK_EQ(mm_dispose_ptr(p), MM_NO_ERR);
    uint32_t q = mm_new_ptr(64, true);
    CHECK_EQ(q, p);
    CHECK_EQ(gm_r32(q), 0);
}

TEST(mm_freed_memory_is_filled_with_deadbeef) {
    fresh_heap();
    uint32_t p = mm_new_ptr(64, true);
    mm_new_ptr(16, false); /* keeps p's block from merging into the free tail */
    mm_dispose_ptr(p);
    CHECK_EQ(gm_r32(p), 0xDEADBEEFu);
    CHECK_EQ(gm_r32(p + 60), 0xDEADBEEFu);
}

TEST(mm_blocks_do_not_overlap) {
    fresh_heap();
    uint32_t a = mm_new_ptr(100, true), b = mm_new_ptr(100, true);
    CHECK(b >= a + 100 + MM_HEADER_SIZE || a >= b + 100 + MM_HEADER_SIZE);
}

TEST(mm_freeing_everything_restores_the_whole_heap) {
    fresh_heap();
    CHECK_EQ(mm_free_bytes(), FULL);
    uint32_t a = mm_new_ptr(100, false), b = mm_new_handle(200, false), c = mm_new_ptr(5, true);
    CHECK(mm_free_bytes() < FULL);
    CHECK_EQ(mm_dispose_ptr(a), MM_NO_ERR);
    CHECK_EQ(mm_dispose_handle(b), MM_NO_ERR);
    CHECK_EQ(mm_dispose_ptr(c), MM_NO_ERR);
    CHECK_EQ(mm_free_bytes(), FULL);
}

TEST(mm_first_fit_reuses_a_freed_hole) {
    fresh_heap();
    uint32_t a = mm_new_ptr(256, false);
    mm_new_ptr(16, false);
    mm_dispose_ptr(a);
    CHECK_EQ(mm_new_ptr(100, false), a);
}

/* Review Focus 2: allocation larger than the heap. */
TEST(mm_exhausting_the_heap_returns_zero) {
    fresh_heap();
    CHECK_EQ(mm_new_ptr(GUEST_HEAP_SIZE, false), 0);
    CHECK_EQ(mm_new_ptr(0xFFFFFFF0u, false), 0);
    CHECK_EQ(mm_new_handle(GUEST_HEAP_SIZE, false), 0);
    CHECK_EQ(mm_free_bytes(), FULL);
    CHECK(mm_new_ptr(FULL, false) != 0);
    CHECK_EQ(mm_new_ptr(1, false), 0);
}

/* Review Focus 1: freeing something that isn't an allocated block. */
TEST(mm_dispose_rejects_bad_and_double_frees) {
    fresh_heap();
    uint32_t p = mm_new_ptr(32, false);
    CHECK_EQ(mm_dispose_ptr(p + 4), MM_MEM_WZ_ERR);
    CHECK_EQ(mm_dispose_ptr(GUEST_IMAGE_BASE), MM_MEM_WZ_ERR);
    CHECK_EQ(mm_dispose_ptr(p), MM_NO_ERR);
    CHECK_EQ(mm_dispose_ptr(p), MM_MEM_WZ_ERR);
    uint32_t h = mm_new_handle(8, false);
    CHECK_EQ(mm_dispose_ptr(h), MM_MEM_WZ_ERR);
    CHECK_EQ(mm_dispose_ptr(gm_r32(h)), MM_MEM_WZ_ERR);
    CHECK_EQ(mm_dispose_handle(h), MM_NO_ERR);
    CHECK_EQ(mm_dispose_handle(h), MM_MEM_WZ_ERR);
}

TEST(mm_set_ptr_size_shrinks_and_grows_in_place) {
    fresh_heap();
    uint32_t p = mm_new_ptr(64, true);
    CHECK_EQ(mm_set_ptr_size(p, 16), MM_NO_ERR);
    CHECK_EQ(mm_ptr_size(p), 16);
    CHECK_EQ(mm_set_ptr_size(p, 1000), MM_NO_ERR); /* the free tail follows p */
    CHECK_EQ(mm_ptr_size(p), 1000);
    CHECK(mm_is_ptr(p));
}

TEST(mm_set_ptr_size_fails_when_the_next_block_is_used) {
    fresh_heap();
    uint32_t p = mm_new_ptr(32, false);
    uint32_t q = mm_new_ptr(32, false);
    CHECK_EQ(mm_set_ptr_size(p, 64), MM_MEM_FULL_ERR);
    CHECK_EQ(mm_ptr_size(p), 32);
    CHECK(mm_is_ptr(q));
    CHECK_EQ(mm_set_ptr_size(q + 16, 8), MM_MEM_WZ_ERR);
}

TEST(mm_handles_point_at_their_data) {
    fresh_heap();
    uint32_t h = mm_new_handle(12, true);
    CHECK(mm_is_handle(h));
    CHECK(!mm_is_ptr(h));
    uint32_t d = gm_r32(h);
    CHECK_EQ(d % 16, 0);
    CHECK_EQ(gm_r32(d), 0);
    CHECK_EQ(mm_handle_size(h), 12);
    CHECK_EQ(mm_recover_handle(d), h);
    CHECK_EQ(mm_recover_handle(d + 16), 0);
    CHECK(!mm_is_handle(d));
}

TEST(mm_zero_size_handle_has_a_block) {
    fresh_heap();
    uint32_t h = mm_new_handle(0, false);
    CHECK(mm_is_handle(h));
    CHECK(gm_r32(h) != 0);
    CHECK_EQ(mm_handle_size(h), 0);
}

TEST(mm_handle_state_is_recorded) {
    fresh_heap();
    uint32_t h = mm_new_handle(4, false);
    CHECK_EQ(mm_handle_state(h), 0);
    mm_set_handle_state(h, MM_STATE_LOCKED | MM_STATE_RESOURCE);
    CHECK_EQ(mm_handle_state(h), MM_STATE_LOCKED | MM_STATE_RESOURCE);
    CHECK(mm_is_handle(h));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: the build fails with `'memmgr.h' file not found`.

- [ ] **Step 3: Write the implementation**

`src/memmgr.h`:
```c
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Memory Manager: Ptrs and Handles in the guest heap (GUEST_HEAP_BASE, 64 MB).
   Every block has a 16-byte big-endian header in guest memory. A handle is the
   address of a master pointer block, whose 4-byte payload points at the data
   block. Handles never move and are never purged. */

#define MM_NO_ERR         0
#define MM_MEM_FULL_ERR   (-108)
#define MM_NIL_HANDLE_ERR (-109)
#define MM_MEM_WZ_ERR     (-111)

/* Handle state bits, as returned by HGetState. */
#define MM_STATE_LOCKED    0x80
#define MM_STATE_PURGEABLE 0x40
#define MM_STATE_RESOURCE  0x20

#define MM_HEADER_SIZE 16u

/* Resets the heap to one free block. Requires gm_init(). */
void mm_init(void);

/* Returns a 16-byte-aligned block of size bytes, or 0 if the heap is full.
   clear zeroes the block; otherwise its contents are unspecified. */
uint32_t mm_new_ptr(uint32_t size, bool clear);
/* MM_MEM_WZ_ERR if p is not an allocated pointer block. */
int16_t mm_dispose_ptr(uint32_t p);
bool mm_is_ptr(uint32_t p);
/* Requires mm_is_ptr(p). */
uint32_t mm_ptr_size(uint32_t p);
/* Resizes in place. MM_MEM_FULL_ERR if the block can't grow in place,
   MM_MEM_WZ_ERR if p is not a pointer block. */
int16_t mm_set_ptr_size(uint32_t p, uint32_t size);

/* Returns a handle to size bytes, or 0 if the heap is full. */
uint32_t mm_new_handle(uint32_t size, bool clear);
/* MM_MEM_WZ_ERR if h is not a handle. */
int16_t mm_dispose_handle(uint32_t h);
bool mm_is_handle(uint32_t h);
/* Requires mm_is_handle(h). */
uint32_t mm_handle_size(uint32_t h);
/* The handle whose data block starts at p, or 0 if there is none. */
uint32_t mm_recover_handle(uint32_t p);
/* Requires mm_is_handle(h). */
uint8_t mm_handle_state(uint32_t h);
void mm_set_handle_state(uint32_t h, uint8_t state);

/* Total payload bytes in free blocks. */
uint32_t mm_free_bytes(void);

/* MemError: the result of the last guest Memory Manager call. */
int16_t mm_error(void);
void mm_set_error(int16_t err);

/* Registers the Memory Manager imports with trap.c. */
void mm_register(void);
```

`src/memmgr.c`:
```c
#include "memmgr.h"

#include <string.h>

#include "guest_mem.h"
#include "util.h"

/* Block header, 16 bytes, big-endian:
     +0  magic
     +4  capacity (payload bytes, a multiple of 16)
     +8  logical size (what the caller asked for)
     +12 owner: for a handle's data block, the handle (master pointer address);
         for a master pointer block, the handle state byte; otherwise 0 */
#define MAGIC_FREE   0x46524545u /* 'FREE' */
#define MAGIC_PTR    0x5054524Bu /* 'PTRK' */
#define MAGIC_DATA   0x44415441u /* 'DATA': a handle's data block */
#define MAGIC_MASTER 0x4D415354u /* 'MAST': a master pointer block */

#define HEAP_END (GUEST_HEAP_BASE + GUEST_HEAP_SIZE)
#define MIN_SPLIT 32u /* header + 16 bytes */

static int16_t last_error;

static uint32_t hdr(uint32_t p) { return p - MM_HEADER_SIZE; }
static uint32_t magic(uint32_t p) { return gm_r32(hdr(p)); }
static uint32_t capacity(uint32_t p) { return gm_r32(hdr(p) + 4); }
static uint32_t logical(uint32_t p) { return gm_r32(hdr(p) + 8); }
static uint32_t owner(uint32_t p) { return gm_r32(hdr(p) + 12); }

static void set_header(uint32_t p, uint32_t m, uint32_t cap, uint32_t size, uint32_t own) {
    uint32_t h = hdr(p);
    gm_w32(h, m);
    gm_w32(h + 4, cap);
    gm_w32(h + 8, size);
    gm_w32(h + 12, own);
}

static uint32_t next_block(uint32_t p) {
    return p + capacity(p) + MM_HEADER_SIZE;
}

/* True if p is the payload address of a block whose header has magic m. */
static bool is_block(uint32_t p, uint32_t m) {
    if (p < GUEST_HEAP_BASE + MM_HEADER_SIZE || p >= HEAP_END || (p & 15u) != 0)
        return false;
    return magic(p) == m;
}

static void fill_free(uint32_t p, uint32_t cap) {
    uint8_t *b = gm_ptr(p, cap);
    for (uint32_t i = 0; i + 4 <= cap; i += 4)
        wr_be32(b + i, 0xDEADBEEFu);
}

/* Merges the free block at p with any free blocks that follow it. */
static void coalesce(uint32_t p) {
    for (;;) {
        uint32_t n = next_block(p);
        if (n >= HEAP_END || magic(n) != MAGIC_FREE)
            return;
        gm_w32(hdr(p) + 4, capacity(p) + MM_HEADER_SIZE + capacity(n));
    }
}

void mm_init(void) {
    uint32_t first = GUEST_HEAP_BASE + MM_HEADER_SIZE;
    set_header(first, MAGIC_FREE, GUEST_HEAP_SIZE - MM_HEADER_SIZE, 0, 0);
    last_error = MM_NO_ERR;
}

static uint32_t alloc_block(uint32_t size, uint32_t m, uint32_t own, bool clear) {
    if (size > GUEST_HEAP_SIZE)
        return 0;
    uint32_t need = (size + 15u) & ~15u;
    if (need == 0)
        need = 16;
    for (uint32_t p = GUEST_HEAP_BASE + MM_HEADER_SIZE; p < HEAP_END; p = next_block(p)) {
        if (magic(p) != MAGIC_FREE)
            continue;
        coalesce(p);
        uint32_t cap = capacity(p);
        if (cap < need)
            continue;
        if (cap - need >= MIN_SPLIT) {
            uint32_t rest = p + need + MM_HEADER_SIZE;
            set_header(rest, MAGIC_FREE, cap - need - MM_HEADER_SIZE, 0, 0);
            cap = need;
        }
        set_header(p, m, cap, size, own);
        if (clear)
            memset(gm_ptr(p, cap), 0, cap);
        return p;
    }
    return 0;
}

static void free_block(uint32_t p) {
    uint32_t cap = capacity(p);
    set_header(p, MAGIC_FREE, cap, 0, 0);
    fill_free(p, cap);
    coalesce(p);
}

uint32_t mm_new_ptr(uint32_t size, bool clear) {
    return alloc_block(size, MAGIC_PTR, 0, clear);
}

bool mm_is_ptr(uint32_t p) { return is_block(p, MAGIC_PTR); }

int16_t mm_dispose_ptr(uint32_t p) {
    if (!mm_is_ptr(p))
        return MM_MEM_WZ_ERR;
    free_block(p);
    return MM_NO_ERR;
}

uint32_t mm_ptr_size(uint32_t p) { return logical(p); }

int16_t mm_set_ptr_size(uint32_t p, uint32_t size) {
    if (!mm_is_ptr(p))
        return MM_MEM_WZ_ERR;
    uint32_t need = (size + 15u) & ~15u;
    if (need == 0)
        need = 16;
    uint32_t cap = capacity(p);
    if (need > cap) {
        uint32_t n = next_block(p);
        if (n >= HEAP_END || magic(n) != MAGIC_FREE)
            return MM_MEM_FULL_ERR;
        coalesce(n);
        uint32_t avail = cap + MM_HEADER_SIZE + capacity(n);
        if (avail < need)
            return MM_MEM_FULL_ERR;
        cap = avail;
    }
    if (cap - need >= MIN_SPLIT) {
        uint32_t rest = p + need + MM_HEADER_SIZE;
        uint32_t rest_cap = cap - need - MM_HEADER_SIZE;
        set_header(rest, MAGIC_FREE, rest_cap, 0, 0);
        fill_free(rest, rest_cap);
        coalesce(rest);
        cap = need;
    }
    set_header(p, MAGIC_PTR, cap, size, 0);
    return MM_NO_ERR;
}

uint32_t mm_new_handle(uint32_t size, bool clear) {
    uint32_t h = alloc_block(4, MAGIC_MASTER, 0, true);
    if (!h)
        return 0;
    uint32_t d = alloc_block(size, MAGIC_DATA, h, clear);
    if (!d) {
        free_block(h);
        return 0;
    }
    gm_w32(h, d);
    return h;
}

bool mm_is_handle(uint32_t h) {
    if (!is_block(h, MAGIC_MASTER))
        return false;
    uint32_t d = gm_r32(h);
    return is_block(d, MAGIC_DATA) && owner(d) == h;
}

int16_t mm_dispose_handle(uint32_t h) {
    if (!mm_is_handle(h))
        return MM_MEM_WZ_ERR;
    free_block(gm_r32(h));
    free_block(h);
    return MM_NO_ERR;
}

uint32_t mm_handle_size(uint32_t h) { return logical(gm_r32(h)); }

uint32_t mm_recover_handle(uint32_t p) {
    if (!is_block(p, MAGIC_DATA))
        return 0;
    uint32_t h = owner(p);
    return mm_is_handle(h) && gm_r32(h) == p ? h : 0;
}

uint8_t mm_handle_state(uint32_t h) { return (uint8_t)owner(h); }

void mm_set_handle_state(uint32_t h, uint8_t state) { gm_w32(hdr(h) + 12, state); }

uint32_t mm_free_bytes(void) {
    uint32_t total = 0;
    for (uint32_t p = GUEST_HEAP_BASE + MM_HEADER_SIZE; p < HEAP_END; p = next_block(p)) {
        if (magic(p) == MAGIC_FREE) {
            coalesce(p);
            total += capacity(p);
        }
    }
    return total;
}

int16_t mm_error(void) { return last_error; }
void mm_set_error(int16_t err) { last_error = err; }

```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S . -B build && cmake --build build && ./build/loony_tests mm_`
Expected: `13 passed, 0 failed, 0 skipped`. Full suite: `78 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/memmgr.h src/memmgr.c tests/test_memmgr.c
git commit -m "Memory Manager allocator: first-fit guest heap with handles"
```

---

### Task 3: Memory Manager calls

**Files:**
- Modify: `src/memmgr.c` (add an include, append the guest calls)
- Create: `tests/test_memmgr_calls.c`

**Interfaces:**
- Consumes: Task 2's `mm_*` functions; `trap_arg`, `trap_return`, `trap_register`, `trap_crash` (Plan 1); `tests/harness.h` (Task 1).
- Produces: `void mm_register(void)`, which installs `NewPtr`, `NewPtrClear`, `DisposePtr`, `GetPtrSize`, `SetPtrSize`, `NewHandleClear`, `DisposeHandle`, `GetHandleSize`, `RecoverHandle`, `HLock`, `HUnlock`, `HPurge`, `HNoPurge`, `MoveHHi`, `HGetState`, `HSetState`, `MemError`. These are exactly the Memory Manager imports in the game (it imports no `NewHandle` or `SetHandleSize`).

Every call sets `MemError`. `OSErr` and `SInt8` results are sign-extended into r3.

- [ ] **Step 1: Write the failing test**

`tests/test_memmgr_calls.c`:
```c
#include "test.h"

#include "harness.h"
#include "memmgr.h"

static const char *const names[] = {
    "NewPtr", "NewPtrClear", "DisposePtr", "GetPtrSize", "SetPtrSize", "NewHandleClear",
    "DisposeHandle", "GetHandleSize", "RecoverHandle", "HLock", "HUnlock", "HPurge",
    "HNoPurge", "MoveHHi", "HGetState", "HSetState", "MemError",
};

static void setup(void) {
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    mm_register();
}

static int16_t mem_error(void) {
    return (int16_t)call_import("MemError", 0);
}

TEST(mmcall_new_ptr_and_size) {
    setup();
    uint32_t p = call_import("NewPtrClear", 1, 40u);
    CHECK(mm_is_ptr(p));
    CHECK_EQ(mem_error(), 0);
    CHECK_EQ(call_import("GetPtrSize", 1, p), 40);
    call_import("SetPtrSize", 2, p, 8u);
    CHECK_EQ(mem_error(), 0);
    CHECK_EQ(call_import("GetPtrSize", 1, p), 8);
    call_import("DisposePtr", 1, p);
    CHECK(!mm_is_ptr(p));
}

TEST(mmcall_out_of_memory_sets_mem_error) {
    setup();
    CHECK_EQ(call_import("NewPtr", 1, 0x7FFFFFFFu), 0);
    CHECK_EQ(mem_error(), MM_MEM_FULL_ERR);
    CHECK_EQ(call_import("NewHandleClear", 1, 0x7FFFFFFFu), 0);
    CHECK_EQ(mem_error(), MM_MEM_FULL_ERR);
    call_import("NewPtr", 1, 4u);
    CHECK_EQ(mem_error(), 0);
}

TEST(mmcall_set_ptr_size_failure_sets_mem_error) {
    setup();
    uint32_t p = call_import("NewPtr", 1, 16u);
    call_import("NewPtr", 1, 16u);
    call_import("SetPtrSize", 2, p, 4096u);
    CHECK_EQ(mem_error(), MM_MEM_FULL_ERR);
}

TEST(mmcall_handle_lifecycle) {
    setup();
    uint32_t h = call_import("NewHandleClear", 1, 24u);
    CHECK(mm_is_handle(h));
    CHECK_EQ(call_import("GetHandleSize", 1, h), 24);
    uint32_t d = gm_r32(h);
    CHECK_EQ(call_import("RecoverHandle", 1, d), h);
    call_import("HLock", 1, h);
    call_import("HPurge", 1, h);
    CHECK_EQ(call_import("HGetState", 1, h) & 0xFF, MM_STATE_LOCKED | MM_STATE_PURGEABLE);
    call_import("HUnlock", 1, h);
    call_import("HNoPurge", 1, h);
    call_import("MoveHHi", 1, h);
    CHECK_EQ(call_import("HGetState", 1, h), 0);
    call_import("HSetState", 2, h, (uint32_t)MM_STATE_LOCKED);
    CHECK_EQ(call_import("HGetState", 1, h), (uint32_t)(int32_t)(int8_t)MM_STATE_LOCKED);
    CHECK_EQ(gm_r32(h), d);
    call_import("DisposeHandle", 1, h);
    CHECK(!mm_is_handle(h));
    CHECK_EQ(mem_error(), 0);
}

TEST(mmcall_null_handle_sets_nil_handle_error) {
    setup();
    CHECK_EQ(call_import("GetHandleSize", 1, 0u), 0);
    CHECK_EQ(mem_error(), MM_NIL_HANDLE_ERR);
    call_import("HLock", 1, 0u);
    CHECK_EQ(mem_error(), MM_NIL_HANDLE_ERR);
    call_import("DisposePtr", 1, 0u);
    CHECK_EQ(mem_error(), 0);
}

static void child_double_free(void *unused) {
    (void)unused;
    setup();
    uint32_t p = call_import("NewPtr", 1, 16u);
    call_import("DisposePtr", 1, p);
    call_import("DisposePtr", 1, p);
}

/* Review Focus 1: a double free stops with a crash report. */
TEST(mmcall_double_free_crashes) {
    char out[16384];
    int status = test_run_child(child_double_free, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: crash: DisposePtr: 0x");
    CHECK_CONTAINS(out, "is not an allocated pointer");
}

static void child_bad_handle(void *unused) {
    (void)unused;
    setup();
    uint32_t p = call_import("NewPtr", 1, 16u);
    call_import("GetHandleSize", 1, p);
}

TEST(mmcall_pointer_used_as_handle_crashes) {
    char out[16384];
    int status = test_run_child(child_bad_handle, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "GetHandleSize: 0x");
    CHECK_CONTAINS(out, "is not a handle");
}

static void child_dispose_resource(void *unused) {
    (void)unused;
    setup();
    uint32_t h = call_import("NewHandleClear", 1, 16u);
    mm_set_handle_state(h, MM_STATE_RESOURCE);
    call_import("DisposeHandle", 1, h);
}

TEST(mmcall_dispose_handle_on_a_resource_crashes) {
    char out[16384];
    int status = test_run_child(child_dispose_resource, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "is a resource handle (use ReleaseResource)");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: the link fails with `undefined symbol: _mm_register`.

- [ ] **Step 3: Write the implementation**

In `src/memmgr.c`, add `#include "trap.h"` between `#include "guest_mem.h"` and `#include "util.h"`, then append:

```c
/* ---- guest calls ---- */

static void ret_err(int16_t err) {
    last_error = err;
    trap_return((uint32_t)(int32_t)err);
}

static void need_ptr(const char *call, uint32_t p) {
    if (!mm_is_ptr(p))
        trap_crash("%s: 0x%08x is not an allocated pointer", call, p);
}

/* Returns false (MemError = nilHandleErr) for a NULL handle; crashes on a
   non-NULL value that isn't a handle. */
static bool need_handle(const char *call, uint32_t h) {
    if (h == 0) {
        last_error = MM_NIL_HANDLE_ERR;
        return false;
    }
    if (!mm_is_handle(h))
        trap_crash("%s: 0x%08x is not a handle", call, h);
    return true;
}

static void h_new_ptr(void) {
    uint32_t p = mm_new_ptr(trap_arg(0), false);
    last_error = p ? MM_NO_ERR : MM_MEM_FULL_ERR;
    trap_return(p);
}

static void h_new_ptr_clear(void) {
    uint32_t p = mm_new_ptr(trap_arg(0), true);
    last_error = p ? MM_NO_ERR : MM_MEM_FULL_ERR;
    trap_return(p);
}

static void h_dispose_ptr(void) {
    uint32_t p = trap_arg(0);
    if (p) {
        need_ptr("DisposePtr", p);
        mm_dispose_ptr(p);
    }
    last_error = MM_NO_ERR;
}

static void h_get_ptr_size(void) {
    uint32_t p = trap_arg(0);
    need_ptr("GetPtrSize", p);
    last_error = MM_NO_ERR;
    trap_return(mm_ptr_size(p));
}

static void h_set_ptr_size(void) {
    uint32_t p = trap_arg(0);
    need_ptr("SetPtrSize", p);
    last_error = mm_set_ptr_size(p, trap_arg(1));
}

static void h_new_handle_clear(void) {
    uint32_t h = mm_new_handle(trap_arg(0), true);
    last_error = h ? MM_NO_ERR : MM_MEM_FULL_ERR;
    trap_return(h);
}

static void h_dispose_handle(void) {
    uint32_t h = trap_arg(0);
    if (!need_handle("DisposeHandle", h))
        return;
    if (mm_handle_state(h) & MM_STATE_RESOURCE)
        trap_crash("DisposeHandle: 0x%08x is a resource handle (use ReleaseResource)", h);
    mm_dispose_handle(h);
    last_error = MM_NO_ERR;
}

static void h_get_handle_size(void) {
    uint32_t h = trap_arg(0);
    if (!need_handle("GetHandleSize", h)) {
        trap_return(0);
        return;
    }
    last_error = MM_NO_ERR;
    trap_return(mm_handle_size(h));
}

static void h_recover_handle(void) {
    uint32_t p = trap_arg(0);
    uint32_t h = mm_recover_handle(p);
    if (!h)
        trap_crash("RecoverHandle: 0x%08x is not the start of a handle's block", p);
    last_error = MM_NO_ERR;
    trap_return(h);
}

static void change_state(const char *call, uint8_t set, uint8_t clear) {
    uint32_t h = trap_arg(0);
    if (!need_handle(call, h))
        return;
    mm_set_handle_state(h, (uint8_t)((mm_handle_state(h) | set) & ~clear));
    last_error = MM_NO_ERR;
}

static void h_hlock(void) { change_state("HLock", MM_STATE_LOCKED, 0); }
static void h_hunlock(void) { change_state("HUnlock", 0, MM_STATE_LOCKED); }
static void h_hpurge(void) { change_state("HPurge", MM_STATE_PURGEABLE, 0); }
static void h_hnopurge(void) { change_state("HNoPurge", 0, MM_STATE_PURGEABLE); }
static void h_move_hhi(void) { change_state("MoveHHi", 0, 0); }

static void h_hget_state(void) {
    uint32_t h = trap_arg(0);
    if (!need_handle("HGetState", h)) {
        trap_return(0);
        return;
    }
    last_error = MM_NO_ERR;
    trap_return((uint32_t)(int32_t)(int8_t)mm_handle_state(h));
}

static void h_hset_state(void) {
    uint32_t h = trap_arg(0);
    if (!need_handle("HSetState", h))
        return;
    mm_set_handle_state(h, (uint8_t)trap_arg(1));
    last_error = MM_NO_ERR;
}

static void h_mem_error(void) { ret_err(last_error); }

void mm_register(void) {
    trap_register("NewPtr", h_new_ptr);
    trap_register("NewPtrClear", h_new_ptr_clear);
    trap_register("DisposePtr", h_dispose_ptr);
    trap_register("GetPtrSize", h_get_ptr_size);
    trap_register("SetPtrSize", h_set_ptr_size);
    trap_register("NewHandleClear", h_new_handle_clear);
    trap_register("DisposeHandle", h_dispose_handle);
    trap_register("GetHandleSize", h_get_handle_size);
    trap_register("RecoverHandle", h_recover_handle);
    trap_register("HLock", h_hlock);
    trap_register("HUnlock", h_hunlock);
    trap_register("HPurge", h_hpurge);
    trap_register("HNoPurge", h_hnopurge);
    trap_register("MoveHHi", h_move_hhi);
    trap_register("HGetState", h_hget_state);
    trap_register("HSetState", h_hset_state);
    trap_register("MemError", h_mem_error);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ./build/loony_tests mmcall_`
Expected: `8 passed, 0 failed, 0 skipped`. Full suite: `86 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/memmgr.c tests/test_memmgr_calls.c
git commit -m "Memory Manager calls"
```

---

### Task 4: Resource fork parser

**Files:**
- Modify: `src/util.h` (add `FOURCC`)
- Create: `src/rsrc.h`, `src/rsrc.c`
- Create: `tests/test_rsrc.c`

**Interfaces:**
- Consumes: `rd_be16`, `rd_be32`, `read_file`, `fnv1a32` (`util.h`).
- Produces:
  - `FOURCC(a, b, c, d)` in `util.h`
  - All of `src/rsrc.h` (`rsrc_register` is implemented in Task 5): `RSRC_NOT_FOUND_ERR` (-192), `RSRC_APP_REFNUM` (1), `rsrc_entry { type, id, attrs, name, data_off, len, handle }`, `bool rsrc_open(const uint8_t *fork, size_t len, char *err, size_t errlen)`, `void rsrc_close(void)`, `uint32_t rsrc_type_count(void)`, `uint32_t rsrc_total(void)`, `uint32_t rsrc_count(uint32_t type)`, `rsrc_entry *rsrc_find(uint32_t type, int16_t id)`, `rsrc_entry *rsrc_find_named(uint32_t type, const char *name)`, `rsrc_entry *rsrc_find_handle(uint32_t h)`, `const uint8_t *rsrc_data(const rsrc_entry *e)`

Format (Inside Macintosh: More Macintosh Toolbox, 1-121): a 16-byte header (data offset, map offset, data length, map length). In the map, the type list offset is at +24 and the name list offset at +26. The type list starts with the type count minus one (0xFFFF means no types), then 8-byte entries: type, resource count minus one, and the offset of that type's reference list from the start of the type list. Each reference is 12 bytes: ID, name offset (0xFFFF if unnamed, else from the name list), attributes (1 byte), data offset (3 bytes, from the data start), and a reserved handle word. Each resource's data starts with a 4-byte length.

- [ ] **Step 1: Write the failing test**

`tests/test_rsrc.c`:
```c
#include "test.h"

#include <stdlib.h>

#include "rsrc.h"
#include "util.h"

static uint8_t *read_fork(size_t *len) {
    char path[1100];
    snprintf(path, sizeof path, "%s/..namedfork/rsrc", test_game_exe_path());
    return read_file(path, len);
}

TEST(rsrc_parses_the_real_resource_fork) {
    SKIP_UNLESS_GAME();
    size_t len;
    uint8_t *fork = read_fork(&len);
    CHECK(fork != NULL);
    CHECK_EQ(len, 3856669);
    char err[256] = "";
    CHECK(rsrc_open(fork, len, err, sizeof err));
    CHECK_EQ(rsrc_type_count(), 57);
    CHECK_EQ(rsrc_total(), 877);
    CHECK_EQ(rsrc_count(FOURCC('E', 'S', 'n', 'd')), 125);
    CHECK_EQ(rsrc_count(FOURCC('P', 'I', 'C', 'T')), 7);
    CHECK_EQ(rsrc_count(FOURCC('s', 'n', 'd', ' ')), 6);
    CHECK_EQ(rsrc_count(FOURCC('c', 'l', 'u', 't')), 3);
    CHECK_EQ(rsrc_count(FOURCC('D', 'L', 'O', 'G')), 6);
    CHECK_EQ(rsrc_count(FOURCC('A', 'L', 'R', 'T')), 23);
    CHECK_EQ(rsrc_count(FOURCC('W', 'I', 'N', 'D')), 3);
    CHECK_EQ(rsrc_count(FOURCC('M', 'E', 'N', 'U')), 6);

    rsrc_entry *visu = rsrc_find(FOURCC('V', 'i', 's', 'u'), 128);
    CHECK(visu != NULL);
    CHECK_EQ(visu->len, 36);
    CHECK_EQ(fnv1a32(rsrc_data(visu), visu->len), 0xC4405513u);
    rsrc_entry *pict = rsrc_find(FOURCC('P', 'I', 'C', 'T'), 128);
    CHECK(pict != NULL);
    CHECK_EQ(pict->len, 18464);
    CHECK_EQ(fnv1a32(rsrc_data(pict), pict->len), 0xE0CC01FFu);
    CHECK_EQ(rsrc_find(FOURCC('A', 'L', 'R', 'T'), 901)->attrs, 0x20);

    rsrc_entry *flip = rsrc_find_named(FOURCC('F', 'l', 'i', 'p'), "buttom left");
    CHECK(flip != NULL);
    CHECK_EQ(flip->id, 1000);
    CHECK_STR(flip->name, "Buttom Left");
    CHECK(rsrc_find(FOURCC('V', 'i', 's', 'u'), 129) == NULL);
    CHECK(rsrc_find(FOURCC('Z', 'Z', 'Z', 'Z'), 128) == NULL);
    CHECK(rsrc_find_named(FOURCC('F', 'l', 'i', 'p'), "Nope") == NULL);
    rsrc_close();
    free(fork);
}

/* Review Focus 4: a corrupt or truncated resource fork. */
TEST(rsrc_rejects_junk) {
    uint8_t junk[64];
    memset(junk, 0xFF, sizeof junk);
    char err[256] = "";
    CHECK(!rsrc_open(junk, sizeof junk, err, sizeof err));
    CHECK(err[0] != '\0');
    CHECK(!rsrc_open(junk, 3, err, sizeof err));
    CHECK_CONTAINS(err, "too short");
    CHECK_EQ(rsrc_total(), 0);
}

TEST(rsrc_rejects_truncated_fork) {
    SKIP_UNLESS_GAME();
    size_t len;
    uint8_t *fork = read_fork(&len);
    CHECK(fork != NULL);
    size_t cuts[] = {0, 15, 16, 4096, 3843455, 3843455 + 100, len - 1};
    for (size_t i = 0; i < sizeof cuts / sizeof cuts[0]; i++) {
        /* Copy so ASan catches any read past the cut. */
        uint8_t *part = malloc(cuts[i] ? cuts[i] : 1);
        memcpy(part, fork, cuts[i]);
        char err[256] = "";
        bool ok = rsrc_open(part, cuts[i], err, sizeof err);
        free(part);
        CHECK(!ok);
        CHECK(err[0] != '\0');
    }
    free(fork);
}

TEST(rsrc_rejects_corrupt_map_offsets) {
    SKIP_UNLESS_GAME();
    size_t len;
    uint8_t *fork = read_fork(&len);
    CHECK(fork != NULL);
    uint32_t map_off = rd_be32(fork + 4);
    char err[256] = "";
    /* Type list offset past the end of the map. */
    uint8_t *bad = malloc(len);
    memcpy(bad, fork, len);
    wr_be16(bad + map_off + 24, 0xFFF0);
    CHECK(!rsrc_open(bad, len, err, sizeof err));
    CHECK_CONTAINS(err, "out of range");
    /* A type claiming 65536 resources. */
    memcpy(bad, fork, len);
    uint32_t tl = rd_be16(bad + map_off + 24);
    wr_be16(bad + map_off + tl + 2 + 4, 0xFFFF);
    CHECK(!rsrc_open(bad, len, err, sizeof err));
    CHECK_CONTAINS(err, "truncated");
    free(bad);
    free(fork);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: the build fails with `'rsrc.h' file not found`.

- [ ] **Step 3: Write the implementation**

Add `FOURCC` to `src/util.h`:
```diff
diff --git a/src/util.h b/src/util.h
index 451c71a..f8a17a3 100644
--- a/src/util.h
+++ b/src/util.h
@@ -13,6 +13,11 @@ uint8_t *read_file(const char *path, size_t *len_out);
 
 uint32_t fnv1a32(const void *data, size_t len);
 
+/* A Mac four-character code, e.g. FOURCC('P','I','C','T'). */
+#define FOURCC(a, b, c, d) \
+    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | \
+     ((uint32_t)(uint8_t)(c) << 8) | (uint32_t)(uint8_t)(d))
+
 static inline uint16_t rd_be16(const uint8_t *p) {
     return (uint16_t)((p[0] << 8) | p[1]);
 }
```

`src/rsrc.h`:
```c
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Resource Manager over the application's resource fork, the only resource
   file. Reference: "Inside Macintosh: More Macintosh Toolbox", chapter 1. */

#define RSRC_NOT_FOUND_ERR (-192) /* resNotFound */
#define RSRC_APP_REFNUM    1      /* what CurResFile returns */

typedef struct {
    uint32_t type;
    int16_t id;
    uint8_t attrs;
    char *name;        /* NULL if unnamed */
    uint32_t data_off; /* fork offset of the resource's bytes (after the length word) */
    uint32_t len;
    uint32_t handle;   /* guest handle once loaded, else 0 */
} rsrc_entry;

/* Parses a resource fork. fork must outlive every later rsrc_ call. Replaces
   any fork opened before. On failure writes err and returns false. */
bool rsrc_open(const uint8_t *fork, size_t len, char *err, size_t errlen);
void rsrc_close(void);

uint32_t rsrc_type_count(void);
uint32_t rsrc_total(void);
uint32_t rsrc_count(uint32_t type);

/* NULL if absent. */
rsrc_entry *rsrc_find(uint32_t type, int16_t id);
/* Case-insensitive (ASCII) name match. NULL if absent. */
rsrc_entry *rsrc_find_named(uint32_t type, const char *name);
/* The entry whose loaded handle is h, or NULL. */
rsrc_entry *rsrc_find_handle(uint32_t h);
const uint8_t *rsrc_data(const rsrc_entry *e);

/* Registers the Resource Manager imports. Requires rsrc_open() and mm_init(). */
void rsrc_register(void);
```

`src/rsrc.c`:
```c
#include "rsrc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"

static struct {
    const uint8_t *fork;
    rsrc_entry *entries;
    uint32_t n;
    uint32_t ntypes;
} R;

static bool fail(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static bool fail(char *err, size_t errlen, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return false;
}

void rsrc_close(void) {
    for (uint32_t i = 0; i < R.n; i++)
        free(R.entries[i].name);
    free(R.entries);
    memset(&R, 0, sizeof R);
}

static bool parse(const uint8_t *f, size_t len, char *err, size_t errlen) {
    if (len < 16)
        return fail(err, errlen, "resource fork is too short (%zu bytes)", len);
    uint64_t data_off = rd_be32(f), map_off = rd_be32(f + 4);
    uint64_t data_len = rd_be32(f + 8), map_len = rd_be32(f + 12);
    if (data_off + data_len > len || map_off + map_len > len || map_len < 30)
        return fail(err, errlen, "resource fork header is out of range");
    const uint8_t *m = f + map_off;
    uint32_t tl = rd_be16(m + 24), nl = rd_be16(m + 26);
    if (tl + 2u > map_len || nl > map_len)
        return fail(err, errlen, "resource map offsets are out of range");
    /* Counts are stored minus one; a type count of 0xFFFF means an empty map. */
    uint32_t ntypes = (uint16_t)(rd_be16(m + tl) + 1u);
    if (tl + 2ull + 8ull * ntypes > map_len)
        return fail(err, errlen, "resource type list is truncated");

    uint32_t total = 0;
    for (uint32_t t = 0; t < ntypes; t++)
        total += rd_be16(m + tl + 2 + 8 * t + 4) + 1u;
    R.entries = calloc(total ? total : 1, sizeof *R.entries);
    if (!R.entries)
        return fail(err, errlen, "out of memory");

    for (uint32_t t = 0; t < ntypes; t++) {
        const uint8_t *te = m + tl + 2 + 8 * t;
        uint32_t type = rd_be32(te);
        uint32_t count = rd_be16(te + 4) + 1u;
        uint64_t refs = (uint64_t)tl + rd_be16(te + 6);
        if (refs + 12ull * count > map_len)
            return fail(err, errlen, "reference list for type %u is truncated", t);
        for (uint32_t k = 0; k < count; k++) {
            const uint8_t *re = m + refs + 12 * k;
            rsrc_entry *e = &R.entries[R.n++];
            e->type = type;
            e->id = (int16_t)rd_be16(re);
            uint16_t name_off = rd_be16(re + 2);
            e->attrs = re[4];
            uint64_t off = rd_be32(re + 4) & 0xFFFFFFu;
            if (off + 4 > data_len)
                return fail(err, errlen, "resource %d has a data offset out of range", e->id);
            e->len = rd_be32(f + data_off + off);
            if (off + 4 + e->len > data_len)
                return fail(err, errlen, "resource %d has data out of range", e->id);
            e->data_off = (uint32_t)(data_off + off + 4);
            if (name_off != 0xFFFF) {
                uint64_t np = (uint64_t)nl + name_off;
                if (np + 1 > map_len || np + 1 + m[np] > map_len)
                    return fail(err, errlen, "resource %d has a name out of range", e->id);
                e->name = strndup((const char *)m + np + 1, m[np]);
                if (!e->name)
                    return fail(err, errlen, "out of memory");
            }
        }
    }
    R.ntypes = ntypes;
    R.fork = f;
    return true;
}

bool rsrc_open(const uint8_t *fork, size_t len, char *err, size_t errlen) {
    rsrc_close();
    if (parse(fork, len, err, errlen))
        return true;
    rsrc_close();
    return false;
}

uint32_t rsrc_type_count(void) { return R.ntypes; }
uint32_t rsrc_total(void) { return R.n; }

uint32_t rsrc_count(uint32_t type) {
    uint32_t c = 0;
    for (uint32_t i = 0; i < R.n; i++)
        c += R.entries[i].type == type;
    return c;
}

rsrc_entry *rsrc_find(uint32_t type, int16_t id) {
    for (uint32_t i = 0; i < R.n; i++)
        if (R.entries[i].type == type && R.entries[i].id == id)
            return &R.entries[i];
    return NULL;
}

rsrc_entry *rsrc_find_named(uint32_t type, const char *name) {
    for (uint32_t i = 0; i < R.n; i++)
        if (R.entries[i].type == type && R.entries[i].name &&
            strcasecmp(R.entries[i].name, name) == 0)
            return &R.entries[i];
    return NULL;
}

rsrc_entry *rsrc_find_handle(uint32_t h) {
    if (!h)
        return NULL;
    for (uint32_t i = 0; i < R.n; i++)
        if (R.entries[i].handle == h)
            return &R.entries[i];
    return NULL;
}

const uint8_t *rsrc_data(const rsrc_entry *e) { return R.fork + e->data_off; }

```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S . -B build && cmake --build build && ./build/loony_tests rsrc_`
Expected: `4 passed, 0 failed, 0 skipped`. Full suite: `90 passed`.

A resource count is stored minus one, so 0xFFFF means 65,536 resources, not zero. Only the type count uses 0xFFFF for "none". `rsrc_rejects_corrupt_map_offsets` checks this.

- [ ] **Step 5: Commit**

```bash
git add src/util.h src/rsrc.h src/rsrc.c tests/test_rsrc.c
git commit -m "Parse the application's resource fork"
```

---

### Task 5: Resource Manager calls

**Files:**
- Modify: `src/rsrc.c` (includes, a `ResError` field, append the guest calls)
- Create: `tests/test_rsrc_calls.c`

**Interfaces:**
- Consumes: Task 4's parser; `mm_new_handle`, `mm_dispose_handle`, `mm_set_handle_state`, `MM_*` (Tasks 2–3); `gm_read_pstr`, `gm_ptr`, `gm_r32`, `gm_w8` (Task 1, Plan 1); `trap_*` (Plan 1).
- Produces: `void rsrc_register(void)`, which installs `GetResource`, `GetNamedResource`, `LoadResource`, `ReleaseResource`, `ResError`, `CurResFile`, `GetIndString`.

A loaded resource's handle has state `MM_STATE_RESOURCE`, plus `MM_STATE_PURGEABLE` if the resource's attributes include `resPurgeable` (0x20). Resource IDs arrive as `short` and are sign-extended from the low 16 bits of r4.

- [ ] **Step 1: Write the failing test**

`tests/test_rsrc_calls.c`:
```c
#include "test.h"

#include <stdlib.h>

#include "harness.h"
#include "memmgr.h"
#include "rsrc.h"

static const char *const names[] = {
    "GetResource", "GetNamedResource", "LoadResource", "ReleaseResource",
    "ResError", "CurResFile", "GetIndString",
};

static uint8_t *fork_buf;
static size_t fork_len;

/* Fresh machine and heap, and the fork reopened so no handle is loaded yet.
   Returns false if the game isn't present. */
static bool setup(void) {
    if (!test_game_present())
        return false;
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    if (!fork_buf) {
        char path[1100];
        snprintf(path, sizeof path, "%s/..namedfork/rsrc", test_game_exe_path());
        fork_buf = read_file(path, &fork_len);
    }
    char err[256];
    if (!fork_buf || !rsrc_open(fork_buf, fork_len, err, sizeof err))
        fatal("can't open the resource fork");
    rsrc_register();
    return true;
}

static int16_t res_error(void) { return (int16_t)call_import("ResError", 0); }

TEST(rsrccall_get_resource_loads_a_handle) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    uint32_t h = call_import("GetResource", 2, FOURCC('V', 'i', 's', 'u'), 128u);
    CHECK(mm_is_handle(h));
    CHECK_EQ(res_error(), 0);
    CHECK_EQ(mm_handle_size(h), 36);
    CHECK_EQ(fnv1a32(gm_ptr(gm_r32(h), 36), 36), 0xC4405513u);
    CHECK(mm_handle_state(h) & MM_STATE_RESOURCE);
    CHECK_EQ(call_import("GetResource", 2, FOURCC('V', 'i', 's', 'u'), 128u), h);
    call_import("LoadResource", 1, h);
    CHECK_EQ(res_error(), 0);
}

TEST(rsrccall_purgeable_attribute_sets_handle_state) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    uint32_t h = call_import("GetResource", 2, FOURCC('A', 'L', 'R', 'T'), 901u);
    CHECK_EQ(mm_handle_state(h), MM_STATE_RESOURCE | MM_STATE_PURGEABLE);
}

TEST(rsrccall_negative_ids_are_sign_extended) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    uint32_t h = call_import("GetResource", 2, FOURCC('S', 'I', 'Z', 'E'), 0xFFFFFFFFu);
    CHECK(mm_is_handle(h));
    CHECK_EQ(mm_handle_size(h), 10);
}

/* Review Focus 3: a resource that doesn't exist. */
TEST(rsrccall_missing_resource_returns_null_and_res_error) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    CHECK_EQ(call_import("GetResource", 2, FOURCC('V', 'i', 's', 'u'), 999u), 0);
    CHECK_EQ(res_error(), RSRC_NOT_FOUND_ERR);
    CHECK_EQ(call_import("GetResource", 2, FOURCC('Z', 'Z', 'Z', 'Z'), 128u), 0);
    CHECK_EQ(res_error(), RSRC_NOT_FOUND_ERR);
    call_import("ReleaseResource", 1, 0u);
    CHECK_EQ(res_error(), RSRC_NOT_FOUND_ERR);
}

TEST(rsrccall_release_then_get_gives_a_fresh_copy) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    uint32_t h = call_import("GetResource", 2, FOURCC('V', 'i', 's', 'u'), 128u);
    gm_w8(gm_r32(h), 0xEE);
    call_import("ReleaseResource", 1, h);
    CHECK_EQ(res_error(), 0);
    CHECK(!mm_is_handle(h));
    uint32_t h2 = call_import("GetResource", 2, FOURCC('V', 'i', 's', 'u'), 128u);
    CHECK(mm_is_handle(h2));
    CHECK_EQ(gm_r8(gm_r32(h2)), 0x6C);
}

TEST(rsrccall_get_named_resource) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    uint32_t name = scratch(256);
    gm_write_pstr(name, "Buttom Left");
    uint32_t h = call_import("GetNamedResource", 2, FOURCC('F', 'l', 'i', 'p'), name);
    CHECK(mm_is_handle(h));
    CHECK_EQ(call_import("GetResource", 2, FOURCC('F', 'l', 'i', 'p'), 1000u), h);
    gm_write_pstr(name, "Nope");
    CHECK_EQ(call_import("GetNamedResource", 2, FOURCC('F', 'l', 'i', 'p'), name), 0);
    CHECK_EQ(res_error(), RSRC_NOT_FOUND_ERR);
}

TEST(rsrccall_cur_res_file) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    CHECK_EQ(call_import("CurResFile", 0), RSRC_APP_REFNUM);
}

TEST(rsrccall_get_ind_string) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    uint32_t s = scratch(256);
    char out[256];
    call_import("GetIndString", 3, s, 128u, 2u);
    gm_read_pstr(s, out);
    CHECK_STR(out, "Untitled");
    call_import("GetIndString", 3, s, 128u, 25u);
    CHECK(gm_r8(s) > 0);
    call_import("GetIndString", 3, s, 128u, 26u);
    CHECK_EQ(gm_r8(s), 0);
    call_import("GetIndString", 3, s, 128u, 0u);
    CHECK_EQ(gm_r8(s), 0);
    call_import("GetIndString", 3, s, 9100u, 1u);
    CHECK_EQ(gm_r8(s), 0);
    call_import("GetIndString", 3, s, 4242u, 1u);
    CHECK_EQ(gm_r8(s), 0);
    CHECK_EQ(res_error(), RSRC_NOT_FOUND_ERR);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: the link fails with `undefined symbol: _rsrc_register`.

- [ ] **Step 3: Write the implementation**

In `src/rsrc.c`, replace the single `#include "util.h"` with:

```c
#include "guest_mem.h"
#include "memmgr.h"
#include "trap.h"
#include "util.h"
```

add a `ResError` field as the last member of the `R` struct:

```c
    uint32_t ntypes;
    int16_t error; /* ResError */
} R;
```

and append:

```c
/* ---- guest calls ---- */

/* Returns e's handle, loading it into the guest heap on first use. */
static uint32_t load(rsrc_entry *e) {
    if (!e->handle) {
        uint32_t h = mm_new_handle(e->len, false);
        if (!h) {
            R.error = MM_MEM_FULL_ERR;
            return 0;
        }
        memcpy(gm_ptr(gm_r32(h), e->len), rsrc_data(e), e->len);
        uint8_t state = MM_STATE_RESOURCE;
        if (e->attrs & 0x20) /* resPurgeable */
            state |= MM_STATE_PURGEABLE;
        mm_set_handle_state(h, state);
        e->handle = h;
    }
    R.error = 0;
    return e->handle;
}

static uint32_t get(rsrc_entry *e) {
    if (!e) {
        R.error = RSRC_NOT_FOUND_ERR;
        return 0;
    }
    return load(e);
}

static void h_get_resource(void) {
    trap_return(get(rsrc_find(trap_arg(0), (int16_t)trap_arg(1))));
}

static void h_get_named_resource(void) {
    char name[256];
    gm_read_pstr(trap_arg(1), name);
    trap_return(get(rsrc_find_named(trap_arg(0), name)));
}

static void h_load_resource(void) {
    R.error = rsrc_find_handle(trap_arg(0)) ? 0 : RSRC_NOT_FOUND_ERR;
}

static void h_release_resource(void) {
    rsrc_entry *e = rsrc_find_handle(trap_arg(0));
    if (!e) {
        R.error = RSRC_NOT_FOUND_ERR;
        return;
    }
    mm_dispose_handle(e->handle);
    e->handle = 0;
    R.error = 0;
}

static void h_res_error(void) { trap_return((uint32_t)(int32_t)R.error); }

static void h_cur_res_file(void) { trap_return(RSRC_APP_REFNUM); }

/* GetIndString(Str255 theString, short strListID, short index): index is
   1-based; an absent list or index gives an empty string. */
static void h_get_ind_string(void) {
    uint32_t out = trap_arg(0);
    int16_t id = (int16_t)trap_arg(1), index = (int16_t)trap_arg(2);
    gm_w8(out, 0);
    rsrc_entry *e = rsrc_find(FOURCC('S', 'T', 'R', '#'), id);
    if (!e) {
        R.error = RSRC_NOT_FOUND_ERR;
        return;
    }
    R.error = 0;
    const uint8_t *d = rsrc_data(e);
    if (e->len < 2 || index < 1 || index > rd_be16(d))
        return;
    uint32_t p = 2;
    for (int i = 1; i < index; i++) {
        if (p >= e->len)
            return;
        p += 1u + d[p];
    }
    if (p >= e->len || p + 1u + d[p] > e->len)
        return;
    memcpy(gm_ptr(out, 1u + d[p]), d + p, 1u + d[p]);
}

void rsrc_register(void) {
    trap_register("GetResource", h_get_resource);
    trap_register("GetNamedResource", h_get_named_resource);
    trap_register("LoadResource", h_load_resource);
    trap_register("ReleaseResource", h_release_resource);
    trap_register("ResError", h_res_error);
    trap_register("CurResFile", h_cur_res_file);
    trap_register("GetIndString", h_get_ind_string);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ./build/loony_tests rsrccall_`
Expected: `8 passed, 0 failed, 0 skipped`. Full suite: `98 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/rsrc.c tests/test_rsrc_calls.c
git commit -m "Resource Manager calls"
```

---

### Task 6: Gestalt, time and miscellaneous calls

**Files:**
- Create: `src/misc.h`, `src/misc.c`
- Create: `tests/test_misc.c`

**Interfaces:**
- Consumes: `gm_*` string and word helpers (Task 1, Plan 1); `FOURCC` (Task 4); `trap_*` (Plan 1); `log_msg` (`util.h`).
- Produces:
  - `MISC_GESTALT_UNDEF_SELECTOR_ERR` (-5551), `MISC_IC_INSTANCE` (0x0FFF0001)
  - `void misc_init(void)`, `uint32_t misc_ticks(void)`, `bool misc_cursor_visible(void)`, `bool misc_ae_handler(uint32_t event_class, uint32_t event_id, uint32_t *handler, uint32_t *refcon)`
  - `void misc_register(void)`, which installs `Gestalt`, `TickCount`, `Microseconds`, `Delay`, `GetDateTime`, `ReadLocation`, `NumToString`, `p2cstrcpy`, `c2pstrcpy`, `BlockMoveData`, `InitCursor`, `HideCursor`, `SetThemeCursor`, `KeyScript`, `GetMBarHeight`, `NewAEEventHandlerUPP`, `AEInstallEventHandler`, `ICStart`, `ICStop`, `ExitToShell`

The Gestalt table adds `lram` and `ram ` (256 MB each) and `mach` to the spec's list. The game queries all three during startup and quits if `lram` or `ram ` is undefined. `num2dec` and `ICLaunchURL` are not implemented yet: the trace doesn't reach them, and `ICLaunchURL` only runs from the shareware dialog's "Buy Now" button. `Delay` sleeps on the host. Pumping events during `Delay` arrives with the event loop in milestone 4. The cursor calls only record visibility until the SDL window exists.

- [ ] **Step 1: Write the failing test**

`tests/test_misc.c`:
```c
#include "test.h"

#include <stdlib.h>
#include <time.h>

#include "harness.h"
#include "misc.h"

static const char *const names[] = {
    "Gestalt", "TickCount", "Microseconds", "Delay", "GetDateTime", "ReadLocation",
    "NumToString", "p2cstrcpy", "c2pstrcpy", "BlockMoveData", "InitCursor", "HideCursor",
    "SetThemeCursor", "KeyScript", "GetMBarHeight", "NewAEEventHandlerUPP",
    "AEInstallEventHandler", "ICStart", "ICStop", "ExitToShell",
};

static void setup(void) {
    harness_init(names, sizeof names / sizeof names[0]);
    misc_init();
    misc_register();
}

static uint32_t gestalt(uint32_t sel, uint32_t *value) {
    uint32_t resp = scratch(4);
    gm_w32(resp, 0xAAAAAAAAu);
    uint32_t err = call_import("Gestalt", 2, sel, resp);
    *value = gm_r32(resp);
    return err;
}

TEST(misc_gestalt_reports_os_x_10_2_8_without_altivec) {
    setup();
    uint32_t v;
    CHECK_EQ(gestalt(FOURCC('s', 'y', 's', 'v'), &v), 0);
    CHECK_EQ(v, 0x1028);
    CHECK_EQ(gestalt(FOURCC('c', 'b', 'o', 'n'), &v), 0);
    CHECK(v >= 0x0140);
    CHECK_EQ(gestalt(FOURCC('p', 'p', 'c', 'f'), &v), 0);
    CHECK_EQ(v & 0x10, 0);
    CHECK_EQ(gestalt(FOURCC('l', 'r', 'a', 'm'), &v), 0);
    CHECK(v >= 16u * 1024 * 1024);
    CHECK_EQ(gestalt(FOURCC('r', 'a', 'm', ' '), &v), 0);
    CHECK(v >= 16u * 1024 * 1024);
    CHECK_EQ(gestalt(FOURCC('v', 'm', ' ', ' '), &v), 0);
    CHECK_EQ(gestalt(FOURCC('m', 'a', 'c', 'h'), &v), 0);
}

static void child_unknown_selector(void *unused) {
    (void)unused;
    setup();
    uint32_t v;
    if (gestalt(FOURCC('z', 'z', 'z', 'z'), &v) != (uint32_t)MISC_GESTALT_UNDEF_SELECTOR_ERR)
        exit(3);
    if (v != 0xAAAAAAAAu)
        exit(4);
}

TEST(misc_gestalt_unknown_selector_is_logged_not_fatal) {
    char out[4096];
    int status = test_run_child(child_unknown_selector, NULL, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: Gestalt: unknown selector 'zzzz'");
}

TEST(misc_tick_count_and_delay) {
    setup();
    uint32_t t0 = call_import("TickCount", 0);
    CHECK(t0 < 60);
    uint32_t final_ticks = scratch(4);
    call_import("Delay", 2, 3u, final_ticks);
    uint32_t t1 = call_import("TickCount", 0);
    CHECK(t1 >= t0 + 3);
    CHECK(gm_r32(final_ticks) >= t0 + 3);
    call_import("Delay", 2, 0u, 0u);
}

TEST(misc_microseconds_advances) {
    setup();
    uint32_t a = scratch(8), b = scratch(8);
    call_import("Microseconds", 1, a);
    call_import("Delay", 2, 1u, 0u);
    call_import("Microseconds", 1, b);
    uint64_t ua = ((uint64_t)gm_r32(a) << 32) | gm_r32(a + 4);
    uint64_t ub = ((uint64_t)gm_r32(b) << 32) | gm_r32(b + 4);
    CHECK(ub >= ua + 10000);
}

TEST(misc_get_date_time_uses_the_mac_epoch) {
    setup();
    uint32_t secs = scratch(4);
    call_import("GetDateTime", 1, secs);
    uint32_t mac = gm_r32(secs);
    /* 2026-01-01 is 3,850,000,000-ish seconds after 1904; any sane clock is past 2020. */
    CHECK(mac > 3660000000u);
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    int64_t unix_local = (int64_t)mac - 2082844800;
    CHECK(unix_local - (int64_t)now - lt.tm_gmtoff <= 2);
}

TEST(misc_read_location_reports_the_gmt_offset) {
    setup();
    uint32_t loc = scratch(12);
    call_import("ReadLocation", 1, loc);
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    CHECK_EQ(gm_r32(loc + 8) & 0xFFFFFF, (uint32_t)lt.tm_gmtoff & 0xFFFFFF);
}

TEST(misc_string_conversions) {
    setup();
    uint32_t p = scratch(256), c = scratch(256);
    char s[256];
    call_import("NumToString", 2, (uint32_t)-1234, p);
    gm_read_pstr(p, s);
    CHECK_STR(s, "-1234");
    call_import("p2cstrcpy", 2, c, p);
    CHECK(gm_read_cstr(c, s, sizeof s));
    CHECK_STR(s, "-1234");
    gm_write_cstr(c, "Loony");
    call_import("c2pstrcpy", 2, p, c);
    gm_read_pstr(p, s);
    CHECK_STR(s, "Loony");
}

TEST(misc_block_move_data_handles_overlap) {
    setup();
    uint32_t a = scratch(16);
    gm_write_cstr(a, "abcdef");
    call_import("BlockMoveData", 3, a, a + 2, 4u);
    char s[16];
    gm_read_cstr(a, s, sizeof s);
    CHECK_STR(s, "ababcd");
    call_import("BlockMoveData", 3, a, a + 1, 0u);
}

TEST(misc_cursor_visibility) {
    setup();
    CHECK(misc_cursor_visible());
    call_import("HideCursor", 0);
    call_import("HideCursor", 0);
    CHECK(!misc_cursor_visible());
    call_import("SetThemeCursor", 1, 7u);
    CHECK(!misc_cursor_visible());
    call_import("InitCursor", 0);
    CHECK(misc_cursor_visible());
}

TEST(misc_trivial_calls) {
    setup();
    CHECK_EQ(call_import("GetMBarHeight", 0), 0);
    call_import("KeyScript", 1, 0xFFFFFFFCu);
    uint32_t inst = scratch(4);
    CHECK_EQ(call_import("ICStart", 2, inst, FOURCC('L', 'L', 'P', 'B')), 0);
    CHECK_EQ(gm_r32(inst), MISC_IC_INSTANCE);
    CHECK_EQ(call_import("ICStop", 1, MISC_IC_INSTANCE), 0);
}

TEST(misc_apple_event_handlers_are_recorded) {
    setup();
    uint32_t upp = call_import("NewAEEventHandlerUPP", 1, 0x00145FA8u);
    CHECK_EQ(upp, 0x00145FA8u);
    CHECK_EQ(call_import("AEInstallEventHandler", 5, FOURCC('a', 'e', 'v', 't'),
                         FOURCC('q', 'u', 'i', 't'), upp, 0x55u, 0u), 0);
    uint32_t handler, refcon;
    CHECK(misc_ae_handler(FOURCC('a', 'e', 'v', 't'), FOURCC('q', 'u', 'i', 't'), &handler,
                          &refcon));
    CHECK_EQ(handler, 0x00145FA8u);
    CHECK_EQ(refcon, 0x55);
    CHECK(!misc_ae_handler(FOURCC('a', 'e', 'v', 't'), FOURCC('o', 'a', 'p', 'p'), &handler,
                           &refcon));
    /* Installing again for the same event replaces the handler. */
    call_import("AEInstallEventHandler", 5, FOURCC('a', 'e', 'v', 't'),
                FOURCC('q', 'u', 'i', 't'), 0x1000u, 0u, 0u);
    CHECK(misc_ae_handler(FOURCC('a', 'e', 'v', 't'), FOURCC('q', 'u', 'i', 't'), &handler,
                          &refcon));
    CHECK_EQ(handler, 0x1000u);
}

static void child_exit(void *unused) {
    (void)unused;
    setup();
    call_import("ExitToShell", 0);
    exit(5);
}

TEST(misc_exit_to_shell_exits_cleanly) {
    char out[4096];
    int status = test_run_child(child_exit, NULL, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: ExitToShell");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: the build fails with `'misc.h' file not found`.

- [ ] **Step 3: Write the implementation**

`src/misc.h`:
```c
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MISC_GESTALT_UNDEF_SELECTOR_ERR (-5551)
#define MISC_IC_INSTANCE 0x0FFF0001u /* opaque ICInstance returned by ICStart */

/* Resets the clock (TickCount starts at 0), cursor and Apple Event state. */
void misc_init(void);

/* Ticks (1/60 s) since misc_init(). */
uint32_t misc_ticks(void);

/* False while HideCursor has hidden the cursor (until InitCursor). */
bool misc_cursor_visible(void);

/* The handler AEInstallEventHandler recorded for (event class, event ID).
   Returns false if there is none. */
bool misc_ae_handler(uint32_t event_class, uint32_t event_id, uint32_t *handler,
                     uint32_t *refcon);

/* Registers Gestalt, time, string, cursor, Apple Event, Internet Config,
   KeyScript, GetMBarHeight, BlockMoveData and ExitToShell imports. */
void misc_register(void);
```

`src/misc.c`:
```c
#include "misc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "guest_mem.h"
#include "trap.h"
#include "util.h"

/* Seconds from the Mac epoch (1904-01-01) to the Unix epoch (1970-01-01). */
#define MAC_EPOCH_OFFSET 2082844800u
#define MAX_AE_HANDLERS 16

static struct {
    struct timespec start;
    int cursor_level; /* 0 = visible; HideCursor decrements, InitCursor resets */
    struct {
        uint32_t event_class, event_id, handler, refcon;
    } ae[MAX_AE_HANDLERS];
    int nae;
} M;

void misc_init(void) {
    memset(&M, 0, sizeof M);
    clock_gettime(CLOCK_MONOTONIC, &M.start);
}

static uint64_t elapsed_us(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t us = (int64_t)(now.tv_sec - M.start.tv_sec) * 1000000 +
                 (now.tv_nsec - M.start.tv_nsec) / 1000;
    return us < 0 ? 0 : (uint64_t)us;
}

uint32_t misc_ticks(void) { return (uint32_t)(elapsed_us() * 60 / 1000000); }

bool misc_cursor_visible(void) { return M.cursor_level == 0; }

bool misc_ae_handler(uint32_t event_class, uint32_t event_id, uint32_t *handler,
                     uint32_t *refcon) {
    for (int i = 0; i < M.nae; i++) {
        if (M.ae[i].event_class == event_class && M.ae[i].event_id == event_id) {
            *handler = M.ae[i].handler;
            *refcon = M.ae[i].refcon;
            return true;
        }
    }
    return false;
}

/* ---- Gestalt ---- */

static const struct {
    uint32_t selector, value;
} gestalt_table[] = {
    {FOURCC('s', 'y', 's', 'v'), 0x1028},     /* Mac OS X 10.2.8 */
    {FOURCC('c', 'b', 'o', 'n'), 0x0160},     /* Carbon 1.6 */
    {FOURCC('p', 'p', 'c', 'f'), 0x0003},     /* G3: graphics ops and stfiwx, no AltiVec (bit 4) */
    {FOURCC('v', 'm', ' ', ' '), 0x0001},     /* virtual memory present */
    {FOURCC('m', 'a', 'c', 'h'), 510},        /* gestaltPowerMacG3 */
    {FOURCC('l', 'r', 'a', 'm'), 0x10000000}, /* 256 MB logical RAM */
    {FOURCC('r', 'a', 'm', ' '), 0x10000000}, /* 256 MB physical RAM */
};

static void h_gestalt(void) {
    uint32_t sel = trap_arg(0), resp = trap_arg(1);
    for (size_t i = 0; i < sizeof gestalt_table / sizeof gestalt_table[0]; i++) {
        if (gestalt_table[i].selector == sel) {
            gm_w32(resp, gestalt_table[i].value);
            trap_return(0);
            return;
        }
    }
    char s[5];
    for (int i = 0; i < 4; i++) {
        int c = (int)((sel >> (24 - 8 * i)) & 0xFF);
        s[i] = isprint(c) ? (char)c : '?';
    }
    s[4] = '\0';
    log_msg("Gestalt: unknown selector '%s' (0x%08x)", s, sel);
    trap_return((uint32_t)MISC_GESTALT_UNDEF_SELECTOR_ERR);
}

/* ---- time ---- */

static void h_tick_count(void) { trap_return(misc_ticks()); }

static void h_microseconds(void) {
    uint64_t us = elapsed_us();
    uint32_t out = trap_arg(0);
    gm_w32(out, (uint32_t)(us >> 32));
    gm_w32(out + 4, (uint32_t)us);
}

static void h_delay(void) {
    uint32_t ticks = trap_arg(0), final_ticks = trap_arg(1);
    if ((int32_t)ticks > 0) {
        uint64_t us = (uint64_t)ticks * 1000000 / 60;
        struct timespec ts = {(time_t)(us / 1000000), (long)(us % 1000000) * 1000};
        nanosleep(&ts, NULL);
    }
    if (final_ticks)
        gm_w32(final_ticks, misc_ticks());
}

static void h_get_date_time(void) {
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    gm_w32(trap_arg(0), (uint32_t)((int64_t)now + lt.tm_gmtoff + MAC_EPOCH_OFFSET));
}

/* ReadLocation(MachineLocation *): latitude and longitude 0, then gmtDelta in
   the low 24 bits with the daylight-saving flag (0x80) in the high byte. */
static void h_read_location(void) {
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    uint32_t loc = trap_arg(0);
    gm_w32(loc, 0);
    gm_w32(loc + 4, 0);
    uint32_t dls = lt.tm_isdst > 0 ? 0x80u : 0u;
    gm_w32(loc + 8, (dls << 24) | ((uint32_t)lt.tm_gmtoff & 0x00FFFFFFu));
}

/* ---- strings and memory ---- */

static void h_num_to_string(void) {
    char buf[16];
    snprintf(buf, sizeof buf, "%d", (int32_t)trap_arg(0));
    gm_write_pstr(trap_arg(1), buf);
}

static void h_p2cstrcpy(void) {
    char s[256];
    gm_read_pstr(trap_arg(1), s);
    gm_write_cstr(trap_arg(0), s);
}

static void h_c2pstrcpy(void) {
    char s[256];
    gm_read_cstr(trap_arg(1), s, sizeof s);
    gm_write_pstr(trap_arg(0), s);
}

static void h_block_move_data(void) {
    uint32_t src = trap_arg(0), dst = trap_arg(1);
    int32_t n = (int32_t)trap_arg(2);
    if (n <= 0)
        return;
    const uint8_t *s = gm_ptr(src, (uint32_t)n);
    memmove(gm_ptr(dst, (uint32_t)n), s, (size_t)n);
}

/* ---- cursor, menus, keyboard ---- */

static void h_init_cursor(void) { M.cursor_level = 0; }
static void h_hide_cursor(void) { M.cursor_level--; }
static void h_set_theme_cursor(void) { trap_return(0); }
static void h_key_script(void) {}
static void h_get_mbar_height(void) { trap_return(0); }

/* ---- Apple Events and Internet Config ---- */

static void h_new_ae_event_handler_upp(void) { trap_return(trap_arg(0)); }

static void h_ae_install_event_handler(void) {
    uint32_t cls = trap_arg(0), id = trap_arg(1);
    int slot = M.nae;
    for (int i = 0; i < M.nae; i++)
        if (M.ae[i].event_class == cls && M.ae[i].event_id == id)
            slot = i;
    if (slot == MAX_AE_HANDLERS)
        trap_crash("AEInstallEventHandler: more than %d handlers", MAX_AE_HANDLERS);
    M.ae[slot].event_class = cls;
    M.ae[slot].event_id = id;
    M.ae[slot].handler = trap_arg(2);
    M.ae[slot].refcon = trap_arg(3);
    if (slot == M.nae)
        M.nae++;
    trap_return(0);
}

static void h_ic_start(void) {
    gm_w32(trap_arg(0), MISC_IC_INSTANCE);
    trap_return(0);
}

static void h_ic_stop(void) { trap_return(0); }

static void h_exit_to_shell(void) {
    log_msg("ExitToShell");
    exit(0);
}

void misc_register(void) {
    trap_register("Gestalt", h_gestalt);
    trap_register("TickCount", h_tick_count);
    trap_register("Microseconds", h_microseconds);
    trap_register("Delay", h_delay);
    trap_register("GetDateTime", h_get_date_time);
    trap_register("ReadLocation", h_read_location);
    trap_register("NumToString", h_num_to_string);
    trap_register("p2cstrcpy", h_p2cstrcpy);
    trap_register("c2pstrcpy", h_c2pstrcpy);
    trap_register("BlockMoveData", h_block_move_data);
    trap_register("InitCursor", h_init_cursor);
    trap_register("HideCursor", h_hide_cursor);
    trap_register("SetThemeCursor", h_set_theme_cursor);
    trap_register("KeyScript", h_key_script);
    trap_register("GetMBarHeight", h_get_mbar_height);
    trap_register("NewAEEventHandlerUPP", h_new_ae_event_handler_upp);
    trap_register("AEInstallEventHandler", h_ae_install_event_handler);
    trap_register("ICStart", h_ic_start);
    trap_register("ICStop", h_ic_stop);
    trap_register("ExitToShell", h_exit_to_shell);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S . -B build && cmake --build build && ./build/loony_tests misc_`
Expected: `12 passed, 0 failed, 0 skipped`. Full suite: `110 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/misc.h src/misc.c tests/test_misc.c
git commit -m "Gestalt, time, string, cursor and Apple Event calls"
```

---

### Task 7: CFString, CFNumber and in-memory CFPreferences

**Files:**
- Create: `src/cf.h`, `src/cf.c`
- Create: `tests/test_cf.c`

**Interfaces:**
- Consumes: `gm_read_cstr`, `gm_ptr`, `gm_r8/16/32`, `gm_w8` (Task 1, Plan 1); `trap_*` (Plan 1).
- Produces:
  - `CF_TAG_BASE` (0x08000000), `CF_TAG_LIMIT` (0x09000000), `CF_STRING_TYPE_ID` (7), `CF_NUMBER_TYPE_ID` (22)
  - `void cf_init(void)`, `uint32_t cf_current_app(void)`, `uint32_t cf_string(const char *s)`, `int cf_retain_count(uint32_t ref)`, `uint32_t cf_live_objects(void)`
  - `void cf_register(void)`, which installs `CFStringCreateWithCString`, `CFStringGetCString`, `CFStringGetTypeID`, `CFGetTypeID`, `CFNumberCreate`, `CFRelease`, `CFPreferencesSetAppValue`, `CFPreferencesCopyAppValue`, `CFPreferencesGetAppIntegerValue`, `CFPreferencesAppSynchronize`

The game stores numbers with `CFNumberCreate(kCFNumberIntType = 9)` and reads them back with `CFPreferencesGetAppIntegerValue`. It reads strings with `CFPreferencesCopyAppValue`, checks `CFGetTypeID` against `CFStringGetTypeID`, then calls `CFStringGetCString`. It imports no `CFNumberGetValue`. String bytes are kept as given (Mac Roman), and only Mac Roman, ASCII and UTF-8 encodings are accepted. Preferences for any application other than `kCFPreferencesCurrentApplication` crash, as do floating-point CFNumbers. Neither appears in the trace.

- [ ] **Step 1: Write the failing test**

`tests/test_cf.c`:
```c
#include "test.h"

#include "cf.h"
#include "harness.h"

static const char *const names[] = {
    "CFStringCreateWithCString", "CFStringGetCString", "CFStringGetTypeID", "CFGetTypeID",
    "CFNumberCreate", "CFRelease", "CFPreferencesSetAppValue", "CFPreferencesCopyAppValue",
    "CFPreferencesGetAppIntegerValue", "CFPreferencesAppSynchronize",
};

static void setup(void) {
    harness_init(names, sizeof names / sizeof names[0]);
    cf_init();
    cf_register();
}

static uint32_t str(const char *s) {
    uint32_t c = scratch((uint32_t)strlen(s) + 1);
    gm_write_cstr(c, s);
    return call_import("CFStringCreateWithCString", 3, 0u, c, 0u);
}

static uint32_t num(int32_t v) {
    uint32_t p = scratch(4);
    gm_w32(p, (uint32_t)v);
    return call_import("CFNumberCreate", 3, 0u, 9u, p);
}

static uint32_t app(void) { return cf_current_app(); }

TEST(cf_strings_round_trip) {
    setup();
    uint32_t s = str("HighScore");
    CHECK(s >= CF_TAG_BASE && s < CF_TAG_LIMIT);
    CHECK_EQ(call_import("CFGetTypeID", 1, s), call_import("CFStringGetTypeID", 0));
    uint32_t buf = scratch(32);
    CHECK_EQ(call_import("CFStringGetCString", 4, s, buf, 32u, 0u), 1);
    char out[32];
    gm_read_cstr(buf, out, sizeof out);
    CHECK_STR(out, "HighScore");
}

/* Review Focus 5: the guest's buffer is too small. */
TEST(cf_get_cstring_refuses_a_small_buffer) {
    setup();
    uint32_t s = str("HighScore");
    uint32_t buf = scratch(16);
    gm_w8(buf + 9, 0x77);
    CHECK_EQ(call_import("CFStringGetCString", 4, s, buf, 9u, 0u), 0);
    CHECK_EQ(gm_r8(buf), 0);
    CHECK_EQ(gm_r8(buf + 9), 0x77);
    CHECK_EQ(call_import("CFStringGetCString", 4, s, buf, 10u, 0u), 1);
}

TEST(cf_numbers_have_their_own_type) {
    setup();
    uint32_t n = num(42);
    CHECK_EQ(call_import("CFGetTypeID", 1, n), CF_NUMBER_TYPE_ID);
    CHECK(call_import("CFGetTypeID", 1, n) != call_import("CFStringGetTypeID", 0));
}

TEST(cf_release_frees_at_zero) {
    setup();
    uint32_t before = cf_live_objects();
    uint32_t s = str("x");
    CHECK_EQ(cf_retain_count(s), 1);
    CHECK_EQ(cf_live_objects(), before + 1);
    call_import("CFRelease", 1, s);
    CHECK_EQ(cf_retain_count(s), 0);
    CHECK_EQ(cf_live_objects(), before);
}

TEST(cf_prefs_missing_key) {
    setup();
    uint32_t k = str("Volume");
    CHECK_EQ(call_import("CFPreferencesCopyAppValue", 2, k, app()), 0);
    uint32_t valid = scratch(1);
    gm_w8(valid, 0xFF);
    CHECK_EQ(call_import("CFPreferencesGetAppIntegerValue", 3, k, app(), valid), 0);
    CHECK_EQ(gm_r8(valid), 0);
    CHECK_EQ(call_import("CFPreferencesGetAppIntegerValue", 3, k, app(), 0u), 0);
}

TEST(cf_prefs_store_and_read_integers) {
    setup();
    uint32_t k = str("Volume"), v = num(-7);
    call_import("CFPreferencesSetAppValue", 3, k, v, app());
    CHECK_EQ(cf_retain_count(v), 2);
    call_import("CFRelease", 1, v);
    call_import("CFRelease", 1, k);
    uint32_t k2 = str("Volume");
    uint32_t valid = scratch(1);
    CHECK_EQ(call_import("CFPreferencesGetAppIntegerValue", 3, k2, app(), valid),
             (uint32_t)-7);
    CHECK_EQ(gm_r8(valid), 1);
    CHECK_EQ(call_import("CFPreferencesAppSynchronize", 1, app()), 1);
}

TEST(cf_prefs_copy_returns_a_retained_value) {
    setup();
    uint32_t k = str("Name"), v = str("Greg");
    call_import("CFPreferencesSetAppValue", 3, k, v, app());
    call_import("CFRelease", 1, v);
    uint32_t got = call_import("CFPreferencesCopyAppValue", 2, k, app());
    CHECK_EQ(got, v);
    CHECK_EQ(cf_retain_count(got), 2);
    call_import("CFRelease", 1, got);
    CHECK_EQ(cf_retain_count(v), 1);
}

TEST(cf_prefs_integer_from_a_numeric_string) {
    setup();
    uint32_t k = str("Level");
    call_import("CFPreferencesSetAppValue", 3, k, str("12"), app());
    uint32_t valid = scratch(1);
    CHECK_EQ(call_import("CFPreferencesGetAppIntegerValue", 3, k, app(), valid), 12);
    CHECK_EQ(gm_r8(valid), 1);
    call_import("CFPreferencesSetAppValue", 3, k, str("12abc"), app());
    CHECK_EQ(call_import("CFPreferencesGetAppIntegerValue", 3, k, app(), valid), 0);
    CHECK_EQ(gm_r8(valid), 0);
}

TEST(cf_prefs_replace_and_remove) {
    setup();
    uint32_t k = str("Level"), a = num(1), b = num(2);
    call_import("CFPreferencesSetAppValue", 3, k, a, app());
    call_import("CFPreferencesSetAppValue", 3, k, b, app());
    CHECK_EQ(cf_retain_count(a), 1);
    CHECK_EQ(call_import("CFPreferencesGetAppIntegerValue", 3, k, app(), 0u), 2);
    call_import("CFPreferencesSetAppValue", 3, k, 0u, app());
    CHECK_EQ(cf_retain_count(b), 1);
    CHECK_EQ(call_import("CFPreferencesCopyAppValue", 2, k, app()), 0);
}

static void child_over_release(void *unused) {
    (void)unused;
    setup();
    uint32_t s = str("x");
    call_import("CFRelease", 1, s);
    call_import("CFRelease", 1, s);
}

TEST(cf_over_release_crashes) {
    char out[16384];
    int status = test_run_child(child_over_release, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: crash: CFRelease: 0x08000");
    CHECK_CONTAINS(out, "is not a live CF object");
}

static void child_other_app(void *unused) {
    (void)unused;
    setup();
    uint32_t k = str("x");
    call_import("CFPreferencesCopyAppValue", 2, k, str("com.example.other"));
}

TEST(cf_prefs_for_another_application_crash) {
    char out[16384];
    int status = test_run_child(child_other_app, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "CFPreferencesCopyAppValue: unsupported application ID");
}

static void child_float_number(void *unused) {
    (void)unused;
    setup();
    uint32_t p = scratch(8);
    call_import("CFNumberCreate", 3, 0u, 13u, p);
}

TEST(cf_float_numbers_crash) {
    char out[16384];
    int status = test_run_child(child_float_number, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "CFNumberCreate: unsupported number type 13");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: the build fails with `'cf.h' file not found`.

- [ ] **Step 3: Write the implementation**

`src/cf.h`:
```c
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Core Foundation subset: CFString and CFNumber objects and CFPreferences.
   Objects live in host memory; the guest sees opaque IDs in tag space
   (CF_TAG_BASE + 16 * index), which it never dereferences. Preferences are
   kept in memory only (saved to disk in a later milestone). */

#define CF_TAG_BASE       0x08000000u
#define CF_TAG_LIMIT      0x09000000u
#define CF_STRING_TYPE_ID 7u
#define CF_NUMBER_TYPE_ID 22u

/* Empties the object table and preferences, then creates the string that
   kCFPreferencesCurrentApplication refers to. */
void cf_init(void);

/* The CFStringRef stored in the kCFPreferencesCurrentApplication data import. */
uint32_t cf_current_app(void);

/* Creates a CFString (retain count 1) from a C string. */
uint32_t cf_string(const char *s);

/* Retain count of a live object, or 0 if ref isn't one. */
int cf_retain_count(uint32_t ref);

/* Number of live objects. */
uint32_t cf_live_objects(void);

/* Registers the CFString, CFNumber, CFRelease, CFGetTypeID and CFPreferences imports. */
void cf_register(void);
```

`src/cf.c`:
```c
#include "cf.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "guest_mem.h"
#include "trap.h"
#include "util.h"

#define ENC_MAC_ROMAN 0x00000000u
#define ENC_ASCII     0x00000600u
#define ENC_UTF8      0x08000100u

typedef struct {
    uint32_t type_id; /* 0 = free slot */
    int refs;
    char *str;
    int64_t num;
} cf_obj;

typedef struct {
    char *key;
    uint32_t value;
} pref;

static struct {
    cf_obj *objs;
    uint32_t nobjs, cap;
    pref *prefs;
    uint32_t nprefs, prefs_cap;
    uint32_t current_app;
} C;

static uint32_t ref_of(uint32_t index) { return CF_TAG_BASE + 16u * index; }

static cf_obj *lookup(uint32_t ref) {
    if (ref < CF_TAG_BASE || ref >= CF_TAG_LIMIT || (ref & 15u) != 0)
        return NULL;
    uint32_t i = (ref - CF_TAG_BASE) / 16u;
    if (i >= C.nobjs || C.objs[i].type_id == 0)
        return NULL;
    return &C.objs[i];
}

static cf_obj *need(const char *call, uint32_t ref) {
    cf_obj *o = lookup(ref);
    if (!o)
        trap_crash("%s: 0x%08x is not a live CF object", call, ref);
    return o;
}

static uint32_t new_obj(uint32_t type_id, char *str, int64_t num) {
    uint32_t i = 0;
    while (i < C.nobjs && C.objs[i].type_id != 0)
        i++;
    if (i == C.nobjs) {
        if (ref_of(C.nobjs) >= CF_TAG_LIMIT)
            fatal("too many CF objects");
        if (C.nobjs == C.cap) {
            C.cap = C.cap ? C.cap * 2 : 64;
            C.objs = realloc(C.objs, C.cap * sizeof *C.objs);
            if (!C.objs)
                fatal("out of memory");
        }
        C.nobjs++;
    }
    C.objs[i] = (cf_obj){type_id, 1, str, num};
    return ref_of(i);
}

static void release(cf_obj *o) {
    if (--o->refs > 0)
        return;
    free(o->str);
    memset(o, 0, sizeof *o);
}

void cf_init(void) {
    for (uint32_t i = 0; i < C.nobjs; i++)
        free(C.objs[i].str);
    for (uint32_t i = 0; i < C.nprefs; i++)
        free(C.prefs[i].key);
    free(C.objs);
    free(C.prefs);
    memset(&C, 0, sizeof C);
    C.current_app = cf_string("com.littlewing.loonylabyrinth");
}

uint32_t cf_current_app(void) { return C.current_app; }

uint32_t cf_string(const char *s) {
    char *copy = strdup(s);
    if (!copy)
        fatal("out of memory");
    return new_obj(CF_STRING_TYPE_ID, copy, 0);
}

int cf_retain_count(uint32_t ref) {
    cf_obj *o = lookup(ref);
    return o ? o->refs : 0;
}

uint32_t cf_live_objects(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < C.nobjs; i++)
        n += C.objs[i].type_id != 0;
    return n;
}

/* ---- CFString, CFNumber, CFRelease ---- */

static void need_encoding(const char *call, uint32_t enc) {
    if (enc != ENC_MAC_ROMAN && enc != ENC_ASCII && enc != ENC_UTF8)
        trap_crash("%s: unsupported string encoding 0x%08x", call, enc);
}

/* CFStringCreateWithCString(alloc, cStr, encoding). Bytes are kept as given. */
static void h_string_create_with_cstring(void) {
    uint32_t cstr = trap_arg(1);
    need_encoding("CFStringCreateWithCString", trap_arg(2));
    if (!cstr)
        trap_crash("CFStringCreateWithCString: NULL string");
    char buf[1024];
    if (!gm_read_cstr(cstr, buf, sizeof buf))
        trap_crash("CFStringCreateWithCString: string at 0x%08x is longer than %zu bytes", cstr,
                   sizeof buf - 1);
    trap_return(cf_string(buf));
}

/* CFStringGetCString(str, buffer, bufferSize, encoding) -> Boolean. Writes
   nothing and returns false if the string and its NUL don't fit. */
static void h_string_get_cstring(void) {
    cf_obj *o = need("CFStringGetCString", trap_arg(0));
    if (o->type_id != CF_STRING_TYPE_ID)
        trap_crash("CFStringGetCString: 0x%08x is not a CFString", trap_arg(0));
    need_encoding("CFStringGetCString", trap_arg(3));
    int32_t size = (int32_t)trap_arg(2);
    size_t n = strlen(o->str) + 1;
    if (size < 0 || n > (size_t)size) {
        trap_return(0);
        return;
    }
    memcpy(gm_ptr(trap_arg(1), (uint32_t)n), o->str, n);
    trap_return(1);
}

static void h_string_get_type_id(void) { trap_return(CF_STRING_TYPE_ID); }

static void h_get_type_id(void) { trap_return(need("CFGetTypeID", trap_arg(0))->type_id); }

/* CFNumberCreate(alloc, theType, valuePtr): integer types only. */
static void h_number_create(void) {
    uint32_t type = trap_arg(1), p = trap_arg(2);
    int64_t v;
    switch (type) {
    case 1: case 7: /* SInt8, Char */
        v = (int8_t)gm_r8(p);
        break;
    case 2: case 8: /* SInt16, Short */
        v = (int16_t)gm_r16(p);
        break;
    case 3: case 9: case 10: case 14: /* SInt32, Int, Long, CFIndex */
        v = (int32_t)gm_r32(p);
        break;
    case 4: case 11: /* SInt64, LongLong */
        v = (int64_t)(((uint64_t)gm_r32(p) << 32) | gm_r32(p + 4));
        break;
    default:
        trap_crash("CFNumberCreate: unsupported number type %u", type);
    }
    trap_return(new_obj(CF_NUMBER_TYPE_ID, NULL, v));
}

static void h_release(void) { release(need("CFRelease", trap_arg(0))); }

/* ---- CFPreferences ---- */

static void need_current_app(const char *call, uint32_t app) {
    if (app != C.current_app)
        trap_crash("%s: unsupported application ID 0x%08x", call, app);
}

static const char *key_string(const char *call, uint32_t key) {
    cf_obj *o = need(call, key);
    if (o->type_id != CF_STRING_TYPE_ID)
        trap_crash("%s: key 0x%08x is not a CFString", call, key);
    return o->str;
}

static pref *find_pref(const char *key) {
    for (uint32_t i = 0; i < C.nprefs; i++)
        if (strcmp(C.prefs[i].key, key) == 0)
            return &C.prefs[i];
    return NULL;
}

/* CFPreferencesSetAppValue(key, value, appID). A NULL value removes the key. */
static void h_prefs_set_app_value(void) {
    const char *key = key_string("CFPreferencesSetAppValue", trap_arg(0));
    uint32_t value = trap_arg(1);
    need_current_app("CFPreferencesSetAppValue", trap_arg(2));
    if (value)
        need("CFPreferencesSetAppValue", value)->refs++;
    pref *p = find_pref(key);
    if (p) {
        release(&C.objs[(p->value - CF_TAG_BASE) / 16u]);
        if (value) {
            p->value = value;
        } else {
            free(p->key);
            *p = C.prefs[--C.nprefs];
        }
        return;
    }
    if (!value)
        return;
    if (C.nprefs == C.prefs_cap) {
        C.prefs_cap = C.prefs_cap ? C.prefs_cap * 2 : 32;
        C.prefs = realloc(C.prefs, C.prefs_cap * sizeof *C.prefs);
        if (!C.prefs)
            fatal("out of memory");
    }
    char *copy = strdup(key);
    if (!copy)
        fatal("out of memory");
    C.prefs[C.nprefs++] = (pref){copy, value};
}

/* CFPreferencesCopyAppValue(key, appID): a retained value, or NULL. */
static void h_prefs_copy_app_value(void) {
    const char *key = key_string("CFPreferencesCopyAppValue", trap_arg(0));
    need_current_app("CFPreferencesCopyAppValue", trap_arg(1));
    pref *p = find_pref(key);
    if (!p) {
        trap_return(0);
        return;
    }
    lookup(p->value)->refs++;
    trap_return(p->value);
}

/* CFPreferencesGetAppIntegerValue(key, appID, Boolean *keyExistsAndHasValidFormat).
   Numbers, and strings that are whole decimal integers, are valid. */
static void h_prefs_get_app_integer_value(void) {
    const char *key = key_string("CFPreferencesGetAppIntegerValue", trap_arg(0));
    need_current_app("CFPreferencesGetAppIntegerValue", trap_arg(1));
    uint32_t valid_out = trap_arg(2);
    pref *p = find_pref(key);
    bool valid = false;
    int64_t v = 0;
    if (p) {
        cf_obj *o = lookup(p->value);
        if (o->type_id == CF_NUMBER_TYPE_ID) {
            v = o->num;
            valid = true;
        } else {
            char *end;
            errno = 0;
            long long parsed = strtoll(o->str, &end, 10);
            if (*o->str && !*end && errno == 0) {
                v = parsed;
                valid = true;
            }
        }
    }
    if (valid_out)
        gm_w8(valid_out, valid);
    trap_return(valid ? (uint32_t)(int32_t)v : 0);
}

/* CFPreferencesAppSynchronize(appID) -> Boolean. In memory only, so always true. */
static void h_prefs_app_synchronize(void) {
    need_current_app("CFPreferencesAppSynchronize", trap_arg(0));
    trap_return(1);
}

void cf_register(void) {
    trap_register("CFStringCreateWithCString", h_string_create_with_cstring);
    trap_register("CFStringGetCString", h_string_get_cstring);
    trap_register("CFStringGetTypeID", h_string_get_type_id);
    trap_register("CFGetTypeID", h_get_type_id);
    trap_register("CFNumberCreate", h_number_create);
    trap_register("CFRelease", h_release);
    trap_register("CFPreferencesSetAppValue", h_prefs_set_app_value);
    trap_register("CFPreferencesCopyAppValue", h_prefs_copy_app_value);
    trap_register("CFPreferencesGetAppIntegerValue", h_prefs_get_app_integer_value);
    trap_register("CFPreferencesAppSynchronize", h_prefs_app_synchronize);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S . -B build && cmake --build build && ./build/loony_tests cf_`
Expected: `12 passed, 0 failed, 0 skipped`. Full suite: `122 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/cf.h src/cf.c tests/test_cf.c
git commit -m "CFString, CFNumber and in-memory CFPreferences"
```

---

### Task 8: `LOONY_TRACE=calls` and `LOONY_TRACE=lowmem`

**Files:**
- Modify: `src/cpu.h`, `src/cpu_unicorn.c` (a write watch)
- Modify: `src/trap.c` (the two traces)
- Modify: `tests/test_trap.c` (append two tests)

**Interfaces:**
- Consumes: `cpu_pc` (Plan 1); `trap.c`'s `fmt_addr`, `T`, `h_call_inner` and the test fixtures in `tests/test_trap.c` (Plan 1).
- Produces:
  - `typedef void (*cpu_write_fn)(uint32_t addr, int size, uint64_t value);` and `void cpu_watch_writes(uint32_t begin, uint32_t end, cpu_write_fn fn)` (inclusive range; `NULL` removes; `cpu_init` clears it; registers, including `pc`, are current inside `fn`)
  - With `LOONY_TRACE` containing `calls`: `loony: trace: call <addr>(<args>) depth <n>` on entry to each `guest_call` and `loony: trace: return 0x........ from <addr> depth <n>` on exit
  - With `LOONY_TRACE` containing `lowmem`: `loony: lowmem: write 0x%04x = 0x<value> (<n> bytes) at <addr>`, once per address

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_trap.c`:
```diff
diff --git a/tests/test_trap.c b/tests/test_trap.c
index 448fd09..19d0cd6 100644
--- a/tests/test_trap.c
+++ b/tests/test_trap.c
@@ -284,3 +284,49 @@ TEST(trap_stub_mode_needs_the_value_all) {
     CHECK_EQ(status, 2);
     CHECK_CONTAINS(out, "loony: crash: unimplemented import FooBar");
 }
+
+static void child_trace_calls(void *unused) {
+    (void)unused;
+    setenv("LOONY_TRACE", "calls", 1);
+    static const char *const names[] = {"CallInner"};
+    setup(names, 1);
+    uint32_t inner[] = {0x38630064 /* addi r3,r3,100 */, 0x4E800020 /* blr */};
+    put_words(INNER, inner, 2);
+    trap_register("CallInner", h_call_inner);
+    uint32_t arg = 5;
+    guest_call(TV_OUTER, 1, &arg);
+}
+
+TEST(trap_trace_calls_logs_nested_guest_calls) {
+    char out[16384];
+    int status = test_run_child(child_trace_calls, NULL, out, sizeof out);
+    CHECK_EQ(status, 0);
+    CHECK_CONTAINS(out, "loony: trace: call code+0x00000(0x00000005) depth 1");
+    CHECK_CONTAINS(out, "loony: trace: call code+0x00100(0x00000005) depth 2");
+    CHECK_CONTAINS(out, "loony: trace: return 0x00000069 from code+0x00100 depth 2");
+    CHECK_CONTAINS(out, "loony: trace: return 0x0000006a from code+0x00000 depth 1");
+}
+
+static void child_trace_lowmem(void *unused) {
+    (void)unused;
+    setenv("LOONY_TRACE", "lowmem", 1);
+    static const char *const names[] = {"Unused"};
+    setup(names, 1);
+    uint32_t code[] = {
+        0x38800123, /* li  r4,0x123 */
+        0x90800910, /* stw r4,0x910(0) */
+        0x90800910, /* stw r4,0x910(0) again: logged once */
+        0x4E800020, /* blr */
+    };
+    put_words(INNER, code, 4);
+    guest_call(TV_INNER, 0, NULL);
+}
+
+TEST(trap_trace_lowmem_logs_first_write_per_address) {
+    char out[16384];
+    int status = test_run_child(child_trace_lowmem, NULL, out, sizeof out);
+    CHECK_EQ(status, 0);
+    CHECK_CONTAINS(out, "loony: lowmem: write 0x0910 = 0x00000123 (4 bytes) at code+0x00104");
+    const char *first = strstr(out, "lowmem: write 0x0910");
+    CHECK(first && !strstr(first + 1, "lowmem: write 0x0910"));
+}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build && ./build/loony_tests trap_trace`
Expected: `1 passed, 2 failed`. The new tests fail because their output lacks the trace lines.

- [ ] **Step 3: Write the implementation**

```diff
diff --git a/src/cpu.h b/src/cpu.h
index 30c7f01..3f3d563 100644
--- a/src/cpu.h
+++ b/src/cpu.h
@@ -38,3 +38,9 @@ cpu_stop cpu_run(uint32_t pc);
 /* Snapshot of all registers. cpu_restore() restores and frees it. */
 cpu_context *cpu_save(void);
 void cpu_restore(cpu_context *ctx);
+
+/* Calls fn for every guest write that touches [begin, end] (inclusive), with
+   the CPU's registers (including pc) current. NULL removes the watch.
+   cpu_init() clears it. */
+typedef void (*cpu_write_fn)(uint32_t addr, int size, uint64_t value);
+void cpu_watch_writes(uint32_t begin, uint32_t end, cpu_write_fn fn);
```

```diff
diff --git a/src/cpu_unicorn.c b/src/cpu_unicorn.c
index f2b8de3..688574a 100644
--- a/src/cpu_unicorn.c
+++ b/src/cpu_unicorn.c
@@ -16,7 +16,8 @@ struct cpu_context {
 };
 
 static uc_engine *uc;
-static uc_hook mem_hook, intr_hook;
+static uc_hook mem_hook, intr_hook, write_hook;
+static cpu_write_fn write_fn;
 static bool mem_fault;
 static uc_mem_type mem_fault_type;
 static uint32_t mem_fault_addr;
@@ -99,6 +100,28 @@ void cpu_shutdown(void) {
     if (uc)
         uc_close(uc);
     uc = NULL;
+    write_fn = NULL;
+}
+
+static void on_write(uc_engine *engine, uc_mem_type type, uint64_t address, int size,
+                     int64_t value, void *user) {
+    (void)engine;
+    (void)type;
+    (void)user;
+    if (write_fn)
+        write_fn((uint32_t)address, size, (uint64_t)value);
+}
+
+void cpu_watch_writes(uint32_t begin, uint32_t end, cpu_write_fn fn) {
+    if (write_fn) {
+        check(uc_hook_del(uc, write_hook), "remove write hook");
+        write_fn = NULL;
+    }
+    if (!fn)
+        return;
+    check(uc_hook_add(uc, &write_hook, UC_HOOK_MEM_WRITE, (void *)on_write, NULL, begin, end),
+          "add write hook");
+    write_fn = fn;
 }
 
 uint32_t cpu_gpr(int n) { return (uint32_t)reg_read(UC_PPC_REG_0 + n); }
```

```diff
diff --git a/src/trap.c b/src/trap.c
index 8df3d54..272cc6c 100644
--- a/src/trap.c
+++ b/src/trap.c
@@ -26,7 +26,9 @@ static struct {
     uint32_t hist_count;
     int depth;
     bool trace_imports;
+    bool trace_calls;
     bool stub_all;
+    uint8_t lowmem_seen[GUEST_LOWMEM_SIZE / 8]; /* one bit per address already logged */
 } T;
 
 static const char *fmt_addr(uint32_t a, char buf[static 32]) {
@@ -41,6 +43,17 @@ static _Noreturn void on_guest_fault(const char *msg) {
     trap_crash("%s", msg);
 }
 
+/* Logs the first write to each low-memory address. */
+static void on_lowmem_write(uint32_t addr, int size, uint64_t value) {
+    uint32_t off = addr - GUEST_LOWMEM_BASE;
+    if (T.lowmem_seen[off / 8] & (1u << (off % 8)))
+        return;
+    T.lowmem_seen[off / 8] |= (uint8_t)(1u << (off % 8));
+    char a[32];
+    fprintf(stderr, "loony: lowmem: write 0x%04x = 0x%0*llx (%d bytes) at %s\n", addr, size * 2,
+            (unsigned long long)value, size, fmt_addr(cpu_pc(), a));
+}
+
 void trap_init(uint32_t nimports, const char *const *names, uint32_t code_base,
                uint32_t code_len) {
     trap_shutdown();
@@ -53,6 +66,10 @@ void trap_init(uint32_t nimports, const char *const *names, uint32_t code_base,
     T.code_len = code_len;
     const char *trace = getenv("LOONY_TRACE");
     T.trace_imports = trace && strstr(trace, "imports");
+    T.trace_calls = trace && strstr(trace, "calls");
+    if (trace && strstr(trace, "lowmem"))
+        cpu_watch_writes(GUEST_LOWMEM_BASE, GUEST_LOWMEM_BASE + GUEST_LOWMEM_SIZE - 1,
+                         on_lowmem_write);
     const char *stub = getenv("LOONY_STUB");
     T.stub_all = stub && strcmp(stub, "all") == 0;
     gm_set_fault_handler(on_guest_fault);
@@ -142,6 +159,13 @@ uint32_t guest_call(uint32_t tvector, int nargs, const uint32_t *args) {
     cpu_set_lr(GUEST_RETURN_MAGIC);
 
     T.depth++;
+    if (T.trace_calls) {
+        char a[32];
+        fprintf(stderr, "loony: trace: call %s(", fmt_addr(code, a));
+        for (int i = 0; i < nargs; i++)
+            fprintf(stderr, "%s0x%08x", i ? ", " : "", args[i]);
+        fprintf(stderr, ") depth %d\n", T.depth);
+    }
     uint32_t pc = code;
     for (;;) {
         cpu_stop s = cpu_run(pc);
@@ -178,9 +202,14 @@ uint32_t guest_call(uint32_t tvector, int nargs, const uint32_t *args) {
             fprintf(stderr, "loony: trace:   -> 0x%08x\n", cpu_gpr(3));
         pc = resume;
     }
+    uint32_t result = cpu_gpr(3);
+    if (T.trace_calls) {
+        char a[32];
+        fprintf(stderr, "loony: trace: return 0x%08x from %s depth %d\n", result,
+                fmt_addr(code, a), T.depth);
+    }
     T.depth--;
 
-    uint32_t result = cpu_gpr(3);
     cpu_restore(saved);
     return result;
 }
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ./build/loony_tests trap_trace`
Expected: `3 passed, 0 failed, 0 skipped`. Full suite: `124 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/cpu.h src/cpu_unicorn.c src/trap.c tests/test_trap.c
git commit -m "Trace guest calls and low-memory writes"
```

---

### Task 9: Run the game through startup

**Files:**
- Modify: `src/loader.h`, `src/loader.c` (`image_find_import`)
- Modify: `src/main.c`
- Modify: `tests/test_loader.c`, `tests/test_run.c`, `README.md`

**Interfaces:**
- Consumes: everything above.
- Produces:
  - `int32_t image_find_import(const loaded_image *img, const char *name)` (index, or -1)
  - `loony` reads the resource fork (`<exe>/..namedfork/rsrc`) next to the executable. It exits 1 with `loony: can't read ...` or `loony: can't load the resources of ...` if that fails. It initializes `memmgr`, `misc` and `cf`, registers all four modules, stores `cf_current_app()` in the `kCFPreferencesCurrentApplication` data slot, and runs to `Alert`.

- [ ] **Step 1: Write the failing tests**

```diff
diff --git a/tests/test_loader.c b/tests/test_loader.c
index ecd8529..fe35b83 100644
--- a/tests/test_loader.c
+++ b/tests/test_loader.c
@@ -37,6 +37,9 @@ TEST(loader_loads_the_real_executable) {
     CHECK_EQ(gm_r32(img.main_tvector), 0x001387E0u);
     CHECK_EQ(gm_r32(img.main_tvector + 4), 0x00145000u);
     CHECK_EQ(img.init_tvector, 0);
+    CHECK_EQ(image_find_import(&img, "kCFPreferencesCurrentApplication"), 36);
+    CHECK_EQ(image_find_import(&img, "EndFullScreen"), 131);
+    CHECK_EQ(image_find_import(&img, "NoSuchCall"), -1);
 
     image_free(&img);
     free(buf);
```

```diff
diff --git a/tests/test_run.c b/tests/test_run.c
index 8982dc6..706cd32 100644
--- a/tests/test_run.c
+++ b/tests/test_run.c
@@ -9,14 +9,17 @@ static void run_loony(void *dir) {
     _exit(127);
 }
 
-TEST(run_stops_at_first_unimplemented_import) {
+TEST(run_reaches_the_shareware_alert) {
     SKIP_UNLESS_GAME();
     char out[32768];
     int status = test_run_child(run_loony, (void *)test_game_dir(), out, sizeof out);
     CHECK_EQ(status, 2);
     CHECK_CONTAINS(out, "loony: loaded ");
     CHECK_CONTAINS(out, "132 imports");
-    CHECK_CONTAINS(out, "loony: crash: unimplemented import ");
+    CHECK_CONTAINS(out, "loony: crash: unimplemented import Alert");
+    /* Alert 901 is the shareware dialog. Everything before it is implemented. */
+    CHECK_CONTAINS(out, "Alert(0x00000385, ");
+    CHECK(!strstr(out, "unknown selector"));
 }
 
 /* Review Focus 1: wrong or missing game folder. */
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build`
Expected: the build fails with `call to undeclared function 'image_find_import'`.

- [ ] **Step 3: Write the implementation**

```diff
diff --git a/src/loader.h b/src/loader.h
index 7532bb9..13163b6 100644
--- a/src/loader.h
+++ b/src/loader.h
@@ -20,3 +20,6 @@ typedef struct {
    must outlive img. On failure, writes err and leaves nothing to free. */
 bool image_load(const uint8_t *buf, size_t len, loaded_image *img, char *err, size_t errlen);
 void image_free(loaded_image *img);
+
+/* Index of the import called name, or -1. */
+int32_t image_find_import(const loaded_image *img, const char *name);
```

```diff
diff --git a/src/loader.c b/src/loader.c
index 7f7891d..ab0d609 100644
--- a/src/loader.c
+++ b/src/loader.c
@@ -140,3 +140,10 @@ void image_free(loaded_image *img) {
     free(img->import_addr);
     memset(img, 0, sizeof *img);
 }
+
+int32_t image_find_import(const loaded_image *img, const char *name) {
+    for (uint32_t i = 0; i < img->pef.nimports; i++)
+        if (strcmp(img->pef.imports[i].name, name) == 0)
+            return (int32_t)i;
+    return -1;
+}
```

```diff
diff --git a/src/main.c b/src/main.c
index fa71aee..fb58afc 100644
--- a/src/main.c
+++ b/src/main.c
@@ -4,9 +4,13 @@
 #include <stdlib.h>
 #include <string.h>
 
+#include "cf.h"
 #include "cpu.h"
 #include "guest_mem.h"
 #include "loader.h"
+#include "memmgr.h"
+#include "misc.h"
+#include "rsrc.h"
 #include "trap.h"
 #include "util.h"
 
@@ -29,6 +33,15 @@ int main(int argc, char **argv) {
         return 1;
     }
 
+    char fork_path[PATH_MAX + 32];
+    snprintf(fork_path, sizeof fork_path, "%s/..namedfork/rsrc", path);
+    size_t fork_len = 0;
+    uint8_t *fork = read_file(fork_path, &fork_len);
+    if (!fork) {
+        fprintf(stderr, "loony: can't read %s: %s\n", fork_path, strerror(errno));
+        return 1;
+    }
+
     gm_init();
     cpu_init();
     loaded_image img;
@@ -37,6 +50,13 @@ int main(int argc, char **argv) {
         fprintf(stderr, "loony: can't load %s: %s\n", path, err);
         return 1;
     }
+    if (!rsrc_open(fork, fork_len, err, sizeof err)) {
+        fprintf(stderr, "loony: can't load the resources of %s: %s\n", path, err);
+        return 1;
+    }
+    mm_init();
+    misc_init();
+    cf_init();
 
     const char **names = calloc(img.pef.nimports ? img.pef.nimports : 1, sizeof *names);
     if (!names)
@@ -44,6 +64,13 @@ int main(int argc, char **argv) {
     for (uint32_t i = 0; i < img.pef.nimports; i++)
         names[i] = img.pef.imports[i].name;
     trap_init(img.pef.nimports, names, img.code_base, img.code_len);
+    mm_register();
+    rsrc_register();
+    misc_register();
+    cf_register();
+    int32_t app_id = image_find_import(&img, "kCFPreferencesCurrentApplication");
+    if (app_id >= 0)
+        gm_w32(img.import_addr[app_id], cf_current_app());
 
     if (!img.main_tvector)
         fatal("%s has no main entry point", path);
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ./build/loony_tests run_ && ./build/loony_tests loader_`
Expected: `3 passed, 0 failed, 0 skipped` (the filter also matches `reloc_import_run_advances_import_index`), then `2 passed, 0 failed, 0 skipped`. Full suite: `124 passed, 0 failed, 0 skipped`.

- [ ] **Step 5: Run the game by hand**

Run: `LOONY_TRACE=imports ./build/loony 2>&1 | tail -20; ./build/loony >/dev/null 2>&1; echo "exit status $?"`
Expected: the trace ends with `#140 Alert(0x00000385, ...) from code+0x0dd20`, then `loony: crash: unimplemented import Alert`, and `exit status 2`. No `unknown selector` lines appear. If the import number or address differs, note it in the commit message; Plan 3 starts from whatever this run shows.

- [ ] **Step 6: Update the README**

```diff
diff --git a/README.md b/README.md
index 2f371e5..90784f2 100644
--- a/README.md
+++ b/README.md
@@ -19,9 +19,13 @@ cmake --build build
 ./build/loony                         # uses /Applications/Loony Labyrinth
 ./build/loony "/path/to/game folder"
 LOONY_TRACE=imports ./build/loony     # log every OS call
+LOONY_TRACE=imports,calls ./build/loony   # also log each call into the game (callbacks)
+LOONY_TRACE=lowmem ./build/loony      # log the first write to each low-memory address
 LOONY_STUB=all ./build/loony          # unimplemented OS calls return 0 instead of crashing
 ```
 
-The original game files are only ever read, never modified.
+The original game files are only ever read, never modified. Today the game
+runs through its startup (memory, resources, Gestalt, preferences) and stops at
+its first dialog, `Alert`, which needs graphics (milestone 3).
 
 Design: `docs/superpowers/specs/2026-09-30-loony-shim-design.md`
```

- [ ] **Step 7: Commit**

```bash
git add src/loader.h src/loader.c src/main.c tests/test_loader.c tests/test_run.c README.md
git commit -m "Run the game through startup to its first dialog

The game now stops at Alert(901), the shareware dialog, from code+0x0dd20
(import #140), which needs graphics."
```

---

## What comes next (not part of this plan)

Plan 3 (milestone 3, graphics) starts at `Alert`. It needs the emulated screen and a minimal dialog path first. Run once with `LOONY_STUB=all`, answering `Alert` with item 1 ("Play Demo"), to see the graphics calls that follow: `GetMainDevice`, `SetDepth(16)`, `BeginFullScreen`, `GetCTable(8)`, GWorld setup, then the resource-driven table setup starting with `GetResource('Visu', 128)`. `GetCTable(8)` asks for the system's standard 8-bit color table, which isn't in the game's resource fork, so `qd.c` must build it. The event setup calls in that stretch (`NewEventHandlerUPP`, `InstallEventHandler`, `InstallStandardEventHandler`, `GetApplicationEventTarget`, `GetWindowEventTarget`) can record their handlers ahead of milestone 4.
