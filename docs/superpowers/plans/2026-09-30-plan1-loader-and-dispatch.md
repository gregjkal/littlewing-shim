# Plan 1: Loader and Import Dispatch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Load the real `LOONY LABYRINTH 3.0.1` PowerPC executable into emulated memory, start running its `main`, and stop with a clear report at the first system call (import) that has no implementation.

**Architecture:** One host buffer is the guest's 32-bit address space, mapped into Unicorn (PPC32 big-endian, G3 750 model). A PEF loader places the code and data sections, unpacks the pattern-initialized data, binds each import to a transition vector whose code address is an **unmapped trap address** (`0x0700_0000 + 4i`), and runs the relocation program. `guest_call()` runs guest code until it returns to the unmapped `RETURN_MAGIC` address. Each time the CPU jumps to a trap address, `guest_call()` stops the CPU, runs the C handler outside Unicorn, and resumes at LR. Handlers can call `guest_call()` again, to any depth.

**Tech Stack:** C (gnu11), clang, CMake ≥ 3.20, pkg-config, Unicorn 2 (Homebrew `unicorn`). SDL3 is not needed until Plan 3.

**Spec:** `docs/superpowers/specs/2026-09-30-loony-shim-design.md` (milestones 0 and 1). This plan covers only those two milestones. Plans 2+ are written after this one ships, using the real import trace it produces.

## Global Constraints

- Repo: `~/dev/loony-shim`, branch `main`. All paths below are relative to it.
- Game files in `/Applications/Loony Labyrinth` are read-only inputs. Never write, move or modify them. Never copy them or anything extracted from them into the repo.
- C11 with GNU extensions (`gnu11`), clang, `-Wall -Wextra -Werror` in every build. Debug builds (the default) add `-fsanitize=address,undefined`. Release is `-O2`.
- Dependencies come only from Homebrew: `unicorn`, `cmake`, `pkg-config`.
- Crashes (unimplemented import, guest fault, internal error) print a report to stderr and exit with status **2**. Bad command-line input (for example a missing game folder) exits with status **1**.
- All guest code runs on the main thread.
- Guest addresses inside the code section are printed as `code+0xNNNNN`.
- Tests that need the game files read `LOONY_GAME_DIR` (default `/Applications/Loony Labyrinth`) and are **skipped**, not failed, if the executable isn't there.
- Guest memory map (from the spec, with the revision to unmapped trap addresses):

| Guest range | Mapped | Contents |
|---|---|---|
| `0x0000_0000`–`0x0000_FFFF` | RW | low-memory page (zeroes) |
| `0x0010_0000`–`0x00FF_FFFF` | RWX | image: code, data, import area |
| `0x0100_0000`–`0x04FF_FFFF` | RW | heap (used from Plan 2) |
| `0x0500_0000`–`0x05FF_FFFF` | no | gap; also serves as the stack's guard |
| `0x0600_0000`–`0x060F_FFFF` | RW | stack; `r1` starts at `0x060F_FFC0` |
| `0x0700_0000`–`0x07FE_FFFF` | no | trap addresses, import *i* at `0x0700_0000 + 4i` |
| `0x07FF_FFF0` | no | `RETURN_MAGIC` |
| `0x0800_0000`+ | no | tag space for opaque host objects (from Plan 2) |

## Facts measured from the real binary (the tests assert these)

| Fact | Value |
|---|---|
| Sections | 3: kind 0 (code), kind 2 (pattern data), kind 4 (loader) |
| Code section | file offset `0xCE0`, 280,528 bytes, first word `0x7C0802A6`, FNV-1a32 `0xA50D49B8` |
| Data section | file offset `0x454B0`, container 8,374, unpacked 13,616, total 22,892. Unpacked FNV-1a32 `0xA2C244ED` |
| Pattern opcodes used | 0 (zero), 1 (block copy), 4 (interleave with zero). Opcodes 2 and 3 are not used |
| Loader | main = section 1, offset 4832. No init routine (section -1). 1 relocation header (section 1, 286 instructions) |
| Relocation opcodes used | BySectDWithSkip, Run BySectC (0), Run BySectD (1), Run TVector8 (3), Run ImportRun (5), SmByImport, IncrPosition |
| Imports | 132. Libraries: `CarbonLib` (0–106), `CarbonLib` (107–129, weak), `Apple;Carbon;Multimedia` (130–131, weak) |
| Import samples | 0 `FSClose`, 1 `StopAlert`, 36 `kCFPreferencesCurrentApplication` (data), 131 `EndFullScreen` |
| After loading at code `0x0010_0000` | data at `0x0014_5000`, import area at `0x0014_A970`. `main` TV at `0x0014_62E0` = {`0x0013_87E0`, `0x0014_5000`}. Relocated data FNV-1a32 `0xA7C47401` |

## Review Focus

1. **Wrong or missing game folder.** The user runs `loony` with a bad path. Expect one clear "can't read" message and exit 1, not a crash. Test in Task 9.
2. **Corrupt or wrong executable.** Someone passes a truncated or non-PEF file. Expect `pef_parse` to return an error, with no out-of-bounds read under ASan. Tests in Task 4.
3. **Guest calls a null function pointer**, a classic bug in old Mac code: a jump to address 0. Expect a FAULT, not execution of low memory. Test in Task 3.
4. **Guest stack overflow.** A write below `0x0600_0000` hits the unmapped gap. Expect a FAULT at the exact address. Test in Task 3.
5. **Crash while nested inside a callback.** An unimplemented import is reached at `guest_call` depth 2. Expect a full report (including depth and history) and exit 2, with no hang. Test in Task 8.

---

### Task 1: Toolchain, build skeleton, test runner, utilities

**Files:**
- Create: `CMakeLists.txt`
- Create: `src/util.h`, `src/util.c`
- Create: `src/main.c` (a stub, replaced in Task 9)
- Create: `tests/test.h`, `tests/test_main.c`, `tests/test_util.c`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `_Noreturn void fatal(const char *fmt, ...)` prints `loony: fatal: <msg>` and exits 2
  - `void log_msg(const char *fmt, ...)` prints `loony: <msg>` to stderr
  - `uint8_t *read_file(const char *path, size_t *len_out)` returns a malloc'd buffer, or NULL
  - `uint32_t fnv1a32(const void *data, size_t len)`
  - `static inline uint16_t rd_be16(const uint8_t *p)`, `rd_be32`, `void wr_be16(uint8_t *p, uint16_t v)`, `wr_be32`
  - Test API (`tests/test.h`): `TEST(name)`, `CHECK`, `CHECK_EQ`, `CHECK_STR`, `CHECK_CONTAINS`, `SKIP_UNLESS_GAME()`, `test_game_dir()`, `test_game_exe_path()`, `test_run_child(fn, arg, out, outlen)` (returns the exit status, or -1 if the child died from a signal)

- [ ] **Step 1: Install and check the toolchain**

Run:
```bash
brew install unicorn cmake pkg-config
pkg-config --modversion unicorn
cmake --version | head -1
```
Expected: unicorn version `2.x` (2.0 or newer is required for `uc_ctl_set_cpu_model`), cmake `3.20` or newer.

- [ ] **Step 2: Write the build file**

`CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.20)
project(loony_shim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Werror)
set(CMAKE_C_FLAGS_DEBUG "-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")
set(CMAKE_C_FLAGS_RELEASE "-O2")

find_package(PkgConfig REQUIRED)
pkg_check_modules(UNICORN REQUIRED IMPORTED_TARGET unicorn)

file(GLOB CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.c)
list(REMOVE_ITEM CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/main.c)

add_library(loony_core STATIC ${CORE_SOURCES})
target_include_directories(loony_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(loony_core PUBLIC PkgConfig::UNICORN)

add_executable(loony src/main.c)
target_link_libraries(loony PRIVATE loony_core)

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.c)
add_executable(loony_tests ${TEST_SOURCES})
target_include_directories(loony_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(loony_tests PRIVATE loony_core)
target_compile_definitions(loony_tests PRIVATE LOONY_BIN="$<TARGET_FILE:loony>")
add_dependencies(loony_tests loony)

enable_testing()
add_test(NAME unit COMMAND loony_tests)
```

`src/main.c` (a stub so the build has an executable; Task 9 replaces it):
```c
#include <stdio.h>

int main(void) {
    fputs("loony: not implemented yet\n", stderr);
    return 1;
}
```

- [ ] **Step 3: Write the test runner**

`tests/test.h`:
```c
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void (*test_fn)(void);

void test_register(const char *name, test_fn fn);
void test_fail(const char *file, int line, const char *msg);
void test_skip(const char *reason);

/* Game folder: $LOONY_GAME_DIR, or /Applications/Loony Labyrinth. */
const char *test_game_dir(void);
const char *test_game_exe_path(void);
bool test_game_present(void);

/* Runs fn(arg) in a forked child with its stderr captured into out
   (NUL-terminated, truncated to outlen - 1 bytes). Returns the child's exit
   status, or -1 if it was killed by a signal. The child exits 0 if fn returns. */
int test_run_child(void (*fn)(void *), void *arg, char *out, size_t outlen);

#define TEST(name)                                                              \
    static void name(void);                                                     \
    __attribute__((constructor)) static void name##_register(void) {            \
        test_register(#name, name);                                             \
    }                                                                           \
    static void name(void)

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            test_fail(__FILE__, __LINE__, #cond);                               \
            return;                                                             \
        }                                                                       \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        unsigned long long a_ = (unsigned long long)(a);                        \
        unsigned long long b_ = (unsigned long long)(b);                        \
        if (a_ != b_) {                                                         \
            char m_[256];                                                       \
            snprintf(m_, sizeof m_, "%s == %s (0x%llx != 0x%llx)", #a, #b, a_, b_); \
            test_fail(__FILE__, __LINE__, m_);                                  \
            return;                                                             \
        }                                                                       \
    } while (0)

#define CHECK_STR(a, b)                                                         \
    do {                                                                        \
        const char *a_ = (a), *b_ = (b);                                        \
        if (strcmp(a_, b_) != 0) {                                              \
            char m_[512];                                                       \
            snprintf(m_, sizeof m_, "%s == %s (\"%s\" != \"%s\")", #a, #b, a_, b_); \
            test_fail(__FILE__, __LINE__, m_);                                  \
            return;                                                             \
        }                                                                       \
    } while (0)

#define CHECK_CONTAINS(hay, needle)                                             \
    do {                                                                        \
        const char *h_ = (hay), *n_ = (needle);                                 \
        if (!strstr(h_, n_)) {                                                  \
            char m_[512];                                                       \
            snprintf(m_, sizeof m_, "output does not contain \"%s\"", n_);      \
            test_fail(__FILE__, __LINE__, m_);                                  \
            fprintf(stderr, "---- output ----\n%s\n----------------\n", h_);    \
            return;                                                             \
        }                                                                       \
    } while (0)

#define SKIP_UNLESS_GAME()                                                      \
    do {                                                                        \
        if (!test_game_present()) {                                             \
            test_skip("game files not found");                                  \
            return;                                                             \
        }                                                                       \
    } while (0)
```

`tests/test_main.c`:
```c
#include "test.h"

#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_TESTS 512

static struct {
    const char *name;
    test_fn fn;
} tests[MAX_TESTS];
static int ntests;
static bool cur_failed, cur_skipped;

void test_register(const char *name, test_fn fn) {
    if (ntests == MAX_TESTS) {
        fprintf(stderr, "too many tests\n");
        exit(1);
    }
    tests[ntests].name = name;
    tests[ntests].fn = fn;
    ntests++;
}

void test_fail(const char *file, int line, const char *msg) {
    fprintf(stderr, "  %s:%d: CHECK failed: %s\n", file, line, msg);
    cur_failed = true;
}

void test_skip(const char *reason) {
    fprintf(stderr, "  skipped: %s\n", reason);
    cur_skipped = true;
}

const char *test_game_dir(void) {
    const char *d = getenv("LOONY_GAME_DIR");
    return d && *d ? d : "/Applications/Loony Labyrinth";
}

const char *test_game_exe_path(void) {
    static char path[1024];
    snprintf(path, sizeof path, "%s/LOONY LABYRINTH 3.0.1", test_game_dir());
    return path;
}

bool test_game_present(void) {
    return access(test_game_exe_path(), R_OK) == 0;
}

int test_run_child(void (*fn)(void *), void *arg, char *out, size_t outlen) {
    int fds[2];
    if (pipe(fds) != 0)
        return -2;
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        fn(arg);
        _exit(0);
    }
    close(fds[1]);
    size_t len = 0;
    for (;;) {
        char tmp[4096];
        ssize_t n = read(fds[0], tmp, sizeof tmp);
        if (n <= 0)
            break;
        size_t take = (size_t)n;
        if (take > outlen - 1 - len)
            take = outlen - 1 - len;
        memcpy(out + len, tmp, take);
        len += take;
    }
    out[len] = '\0';
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(int argc, char **argv) {
    const char *filter = argc > 1 ? argv[1] : NULL;
    int passed = 0, failed = 0, skipped = 0;
    for (int i = 0; i < ntests; i++) {
        if (filter && !strstr(tests[i].name, filter))
            continue;
        cur_failed = cur_skipped = false;
        fprintf(stderr, "%s\n", tests[i].name);
        tests[i].fn();
        if (cur_failed)
            failed++;
        else if (cur_skipped)
            skipped++;
        else
            passed++;
    }
    fprintf(stderr, "\n%d passed, %d failed, %d skipped\n", passed, failed, skipped);
    return failed ? 1 : 0;
}
```

- [ ] **Step 4: Write the failing utility tests**

`tests/test_util.c`:
```c
#include "test.h"

#include <stdlib.h>
#include <unistd.h>

#include "util.h"

TEST(util_fnv1a32_known_values) {
    CHECK_EQ(fnv1a32("", 0), 0x811C9DC5u);
    CHECK_EQ(fnv1a32("a", 1), 0xE40C292Cu);
}

TEST(util_big_endian_round_trip) {
    uint8_t b[4];
    wr_be32(b, 0x11223344u);
    CHECK_EQ(b[0], 0x11);
    CHECK_EQ(b[3], 0x44);
    CHECK_EQ(rd_be32(b), 0x11223344u);
    wr_be16(b, 0xABCD);
    CHECK_EQ(b[0], 0xAB);
    CHECK_EQ(rd_be16(b), 0xABCD);
}

TEST(util_read_file_round_trip) {
    const char *tmp = getenv("TMPDIR");
    char path[1024];
    snprintf(path, sizeof path, "%s/loony-test-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(write(fd, "hello", 5) == 5);
    close(fd);
    size_t len = 0;
    uint8_t *data = read_file(path, &len);
    unlink(path);
    CHECK(data != NULL);
    CHECK_EQ(len, 5);
    CHECK(memcmp(data, "hello", 5) == 0);
    free(data);
}

TEST(util_read_file_missing_returns_null) {
    size_t len = 0;
    CHECK(read_file("/nonexistent/loony/file", &len) == NULL);
}

static void child_fatal(void *arg) {
    (void)arg;
    fatal("boom %d", 7);
}

TEST(util_fatal_exits_2_with_message) {
    char out[1024];
    int status = test_run_child(child_fatal, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: fatal: boom 7");
}
```

- [ ] **Step 5: Run the tests to verify they fail**

Run: `cmake -S . -B build && cmake --build build`
Expected: the build fails with `'util.h' file not found`.

- [ ] **Step 6: Write the utilities**

`src/util.h`:
```c
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Prints "loony: fatal: <msg>" to stderr and exits with status 2. */
_Noreturn void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Prints "loony: <msg>" to stderr. */
void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Reads a whole file into a malloc'd buffer. Returns NULL (errno set) on failure. */
uint8_t *read_file(const char *path, size_t *len_out);

uint32_t fnv1a32(const void *data, size_t len);

static inline uint16_t rd_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline void wr_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void wr_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
```

`src/util.c`:
```c
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("loony: fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("loony: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

uint8_t *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 65536;
            uint8_t *grown = realloc(buf, cap);
            if (!grown) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = grown;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0)
            break;
    }
    int failed = ferror(f);
    fclose(f);
    if (failed) {
        free(buf);
        return NULL;
    }
    *len_out = len;
    return buf;
}

uint32_t fnv1a32(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build build && ./build/loony_tests`
Expected: `5 passed, 0 failed, 0 skipped`

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt src/util.h src/util.c src/main.c tests/test.h tests/test_main.c tests/test_util.c
git commit -m "Build skeleton, test runner and utilities"
```

---

### Task 2: Guest memory

**Files:**
- Create: `src/guest_mem.h`, `src/guest_mem.c`
- Create: `tests/test_guest_mem.c`

**Interfaces:**
- Consumes: `fatal`, `rd_be16/32`, `wr_be16/32` (Task 1).
- Produces:
  - Address constants `GUEST_LOWMEM_BASE`, `GUEST_LOWMEM_SIZE`, `GUEST_IMAGE_BASE`, `GUEST_IMAGE_LIMIT`, `GUEST_HEAP_BASE`, `GUEST_HEAP_SIZE`, `GUEST_STACK_BASE`, `GUEST_STACK_SIZE`, `GUEST_STACK_TOP`, `GUEST_TRAP_BASE`, `GUEST_TRAP_LIMIT`, `GUEST_TRAP_ADDR(i)`, `GUEST_RETURN_MAGIC`, `GUEST_TAG_BASE`
  - `typedef struct { uint32_t base, size; int prot; } gm_region;` and `GM_PROT_R/W/X`
  - `void gm_init(void)` gives fresh zeroed memory (call before `cpu_init`)
  - `void gm_shutdown(void)`
  - `uint8_t *gm_host_base(void)`
  - `int gm_regions(const gm_region **out)`
  - `bool gm_is_backed(uint32_t addr, uint32_t len)`
  - `uint8_t *gm_ptr(uint32_t addr, uint32_t len)` (fatal if not backed)
  - `uint8_t gm_r8(uint32_t)`, `uint16_t gm_r16(uint32_t)`, `uint32_t gm_r32(uint32_t)`, `void gm_w8/gm_w16/gm_w32(uint32_t addr, value)`

- [ ] **Step 1: Write the failing test**

`tests/test_guest_mem.c`:
```c
#include "test.h"

#include "guest_mem.h"

TEST(gm_words_are_big_endian_in_host_memory) {
    gm_init();
    gm_w32(GUEST_IMAGE_BASE, 0x11223344u);
    uint8_t *p = gm_host_base() + GUEST_IMAGE_BASE;
    CHECK_EQ(p[0], 0x11);
    CHECK_EQ(p[3], 0x44);
    CHECK_EQ(gm_r32(GUEST_IMAGE_BASE), 0x11223344u);
    CHECK_EQ(gm_r16(GUEST_IMAGE_BASE + 2), 0x3344);
    CHECK_EQ(gm_r8(GUEST_IMAGE_BASE + 1), 0x22);
    gm_w16(GUEST_HEAP_BASE, 0xBEEF);
    gm_w8(GUEST_STACK_BASE, 0x7F);
    CHECK_EQ(gm_r16(GUEST_HEAP_BASE), 0xBEEF);
    CHECK_EQ(gm_r8(GUEST_STACK_BASE), 0x7F);
}

TEST(gm_init_gives_fresh_zeroed_memory) {
    gm_init();
    gm_w32(GUEST_HEAP_BASE, 0xFFFFFFFFu);
    gm_init();
    CHECK_EQ(gm_r32(GUEST_HEAP_BASE), 0);
}

TEST(gm_backed_ranges_match_the_memory_map) {
    gm_init();
    CHECK(gm_is_backed(0, 4));
    CHECK(gm_is_backed(GUEST_IMAGE_BASE, 4));
    CHECK(gm_is_backed(GUEST_HEAP_BASE + GUEST_HEAP_SIZE - 4, 4));
    CHECK(!gm_is_backed(GUEST_HEAP_BASE + GUEST_HEAP_SIZE, 4));
    CHECK(!gm_is_backed(GUEST_STACK_BASE - 16, 4));
    CHECK(gm_is_backed(GUEST_STACK_TOP - 4, 4));
    CHECK(!gm_is_backed(GUEST_STACK_TOP - 2, 4));
    CHECK(!gm_is_backed(GUEST_TRAP_BASE, 4));
    CHECK(!gm_is_backed(0xFFFFFFFCu, 8));
}

TEST(gm_regions_are_page_aligned) {
    const gm_region *r;
    int n = gm_regions(&r);
    CHECK_EQ(n, 4);
    for (int i = 0; i < n; i++) {
        CHECK_EQ(r[i].base % 0x4000, 0);
        CHECK_EQ(r[i].size % 0x4000, 0);
    }
}

static void child_bad_access(void *arg) {
    (void)arg;
    gm_init();
    gm_r32(0x05000000u);
}

TEST(gm_unbacked_access_is_fatal) {
    char out[1024];
    int status = test_run_child(child_bad_access, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "unmapped guest address 0x05000000");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build`
Expected: the build fails with `'guest_mem.h' file not found`.

- [ ] **Step 3: Write the implementation**

`src/guest_mem.h`:
```c
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The guest's 32-bit address space. See the memory map in the spec. */
#define GUEST_LOWMEM_BASE  0x00000000u
#define GUEST_LOWMEM_SIZE  0x00010000u
#define GUEST_IMAGE_BASE   0x00100000u
#define GUEST_IMAGE_LIMIT  0x01000000u
#define GUEST_HEAP_BASE    0x01000000u
#define GUEST_HEAP_SIZE    0x04000000u
#define GUEST_STACK_BASE   0x06000000u
#define GUEST_STACK_SIZE   0x00100000u
#define GUEST_STACK_TOP    (GUEST_STACK_BASE + GUEST_STACK_SIZE)
/* Unmapped. Jumping here stops the CPU; see cpu_run(). */
#define GUEST_TRAP_BASE    0x07000000u
#define GUEST_TRAP_LIMIT   0x07FF0000u
#define GUEST_TRAP_ADDR(i) (GUEST_TRAP_BASE + 4u * (uint32_t)(i))
#define GUEST_RETURN_MAGIC 0x07FFFFF0u
/* Opaque host-object IDs (Plan 2+). Never dereferenced. */
#define GUEST_TAG_BASE     0x08000000u

#define GM_PROT_R 1
#define GM_PROT_W 2
#define GM_PROT_X 4

typedef struct {
    uint32_t base, size;
    int prot;
} gm_region;

/* Allocates fresh zeroed guest memory, replacing any previous allocation.
   Call before cpu_init(), which maps this memory into the CPU. */
void gm_init(void);
void gm_shutdown(void);

/* Host address of guest address 0. Only backed regions may be touched. */
uint8_t *gm_host_base(void);

/* The regions the CPU maps. Everything else is unmapped. */
int gm_regions(const gm_region **out);

/* True if [addr, addr+len) lies inside a single backed region. */
bool gm_is_backed(uint32_t addr, uint32_t len);

/* Host pointer for a guest range. Fatal if the range is not backed. */
uint8_t *gm_ptr(uint32_t addr, uint32_t len);

uint8_t gm_r8(uint32_t addr);
uint16_t gm_r16(uint32_t addr);
uint32_t gm_r32(uint32_t addr);
void gm_w8(uint32_t addr, uint8_t v);
void gm_w16(uint32_t addr, uint16_t v);
void gm_w32(uint32_t addr, uint32_t v);
```

`src/guest_mem.c`:
```c
#include "guest_mem.h"

#include <sys/mman.h>

#include "util.h"

#define GUEST_MEM_SIZE GUEST_STACK_TOP

static uint8_t *mem;

static const gm_region regions[] = {
    {GUEST_LOWMEM_BASE, GUEST_LOWMEM_SIZE, GM_PROT_R | GM_PROT_W},
    {GUEST_IMAGE_BASE, GUEST_IMAGE_LIMIT - GUEST_IMAGE_BASE, GM_PROT_R | GM_PROT_W | GM_PROT_X},
    {GUEST_HEAP_BASE, GUEST_HEAP_SIZE, GM_PROT_R | GM_PROT_W},
    {GUEST_STACK_BASE, GUEST_STACK_SIZE, GM_PROT_R | GM_PROT_W},
};

void gm_init(void) {
    gm_shutdown();
    void *p = mmap(NULL, GUEST_MEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        fatal("can't allocate %u bytes of guest memory", GUEST_MEM_SIZE);
    mem = p;
}

void gm_shutdown(void) {
    if (mem)
        munmap(mem, GUEST_MEM_SIZE);
    mem = NULL;
}

uint8_t *gm_host_base(void) {
    return mem;
}

int gm_regions(const gm_region **out) {
    *out = regions;
    return (int)(sizeof regions / sizeof regions[0]);
}

bool gm_is_backed(uint32_t addr, uint32_t len) {
    for (size_t i = 0; i < sizeof regions / sizeof regions[0]; i++) {
        uint64_t start = regions[i].base, end = start + regions[i].size;
        if (addr >= start && (uint64_t)addr + len <= end)
            return true;
    }
    return false;
}

uint8_t *gm_ptr(uint32_t addr, uint32_t len) {
    if (!mem)
        fatal("guest memory used before gm_init()");
    if (!gm_is_backed(addr, len))
        fatal("access to unmapped guest address 0x%08x (%u bytes)", addr, len);
    return mem + addr;
}

uint8_t gm_r8(uint32_t addr) { return *gm_ptr(addr, 1); }
uint16_t gm_r16(uint32_t addr) { return rd_be16(gm_ptr(addr, 2)); }
uint32_t gm_r32(uint32_t addr) { return rd_be32(gm_ptr(addr, 4)); }
void gm_w8(uint32_t addr, uint8_t v) { *gm_ptr(addr, 1) = v; }
void gm_w16(uint32_t addr, uint16_t v) { wr_be16(gm_ptr(addr, 2), v); }
void gm_w32(uint32_t addr, uint32_t v) { wr_be32(gm_ptr(addr, 4), v); }
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ./build/loony_tests gm_`
Expected: `5 passed, 0 failed, 0 skipped`

- [ ] **Step 5: Commit**

```bash
git add src/guest_mem.h src/guest_mem.c tests/test_guest_mem.c
git commit -m "Guest address space with big-endian accessors"
```

---

### Task 3: CPU wrapper over Unicorn

**Files:**
- Create: `src/cpu.h`, `src/cpu_unicorn.c`
- Create: `tests/ppc.h` (test helpers), `tests/test_cpu.c`

**Interfaces:**
- Consumes: `gm_regions`, `gm_host_base`, the address constants (Task 2); `fatal` (Task 1).
- Produces:
  - `typedef enum { CPU_STOP_RETURN, CPU_STOP_TRAP, CPU_STOP_FAULT } cpu_stop_kind;`
  - `typedef struct { cpu_stop_kind kind; uint32_t addr; uint32_t pc; char detail[96]; } cpu_stop;` (for TRAP, `addr` is the trap address; for FAULT, it's the faulting address)
  - `void cpu_init(void)`, `void cpu_shutdown(void)`
  - `uint32_t cpu_gpr(int n)`, `void cpu_set_gpr(int n, uint32_t v)`, `double cpu_fpr(int n)`, `void cpu_set_fpr(int n, double v)`
  - `uint32_t cpu_lr(void)`, `void cpu_set_lr(uint32_t v)`, `uint32_t cpu_ctr(void)`, `uint32_t cpu_pc(void)`
  - `cpu_stop cpu_run(uint32_t pc)` runs from `pc` until a return, trap or fault
  - `typedef struct cpu_context cpu_context;`, `cpu_context *cpu_save(void)`, `void cpu_restore(cpu_context *ctx)` (restores and frees)
  - Test helpers in `tests/ppc.h`: `put_words(addr, words, n)` and `fresh_machine()` (gm_init + cpu_init + r1 at the stack top + LR = RETURN_MAGIC)

Notes for the implementer:
- `uc_ctl_set_cpu_model` must be the first call after `uc_open`.
- Unicorn PPC32 registers are 32-bit. Read and write them through a zeroed `uint64_t` so the code works whether Unicorn copies 4 or 8 bytes (the host is little-endian).
- Floating-point instructions trap unless `MSR[FP]` (`0x2000`) is set, so `cpu_init` sets it. The `fadd` test proves this.
- Unicorn caches translated code. Tests always call `fresh_machine()` before writing code, so stale translations can't occur.

- [ ] **Step 1: Write the test helpers and failing tests**

`tests/ppc.h`:
```c
#pragma once
#include "cpu.h"
#include "guest_mem.h"

static inline void put_words(uint32_t addr, const uint32_t *words, int n) {
    for (int i = 0; i < n; i++)
        gm_w32(addr + 4u * (uint32_t)i, words[i]);
}

/* Fresh memory and CPU, stack pointer near the stack top, LR = RETURN_MAGIC. */
static inline void fresh_machine(void) {
    gm_init();
    cpu_init();
    cpu_set_gpr(1, GUEST_STACK_TOP - 64);
    cpu_set_lr(GUEST_RETURN_MAGIC);
}
```

`tests/test_cpu.c`:
```c
#include "test.h"

#include "ppc.h"

#define CODE GUEST_IMAGE_BASE

TEST(cpu_runs_until_return) {
    fresh_machine();
    uint32_t code[] = {
        0x38600005, /* li   r3,5 */
        0x38630002, /* addi r3,r3,2 */
        0x4E800020, /* blr */
    };
    put_words(CODE, code, 3);
    cpu_stop s = cpu_run(CODE);
    CHECK_EQ(s.kind, CPU_STOP_RETURN);
    CHECK_EQ(cpu_gpr(3), 7);
}

TEST(cpu_stops_at_trap_address) {
    fresh_machine();
    uint32_t code[] = {
        0x3D800700, /* lis   r12,0x0700 */
        0x618C0004, /* ori   r12,r12,4 */
        0x7D8903A6, /* mtctr r12 */
        0x4E800420, /* bctr */
    };
    put_words(CODE, code, 4);
    cpu_stop s = cpu_run(CODE);
    CHECK_EQ(s.kind, CPU_STOP_TRAP);
    CHECK_EQ(s.addr, GUEST_TRAP_ADDR(1));
}

TEST(cpu_resumes_after_trap_at_lr) {
    fresh_machine();
    uint32_t code[] = {
        0x7FE802A6, /* mflr  r31 */
        0x3D800700, /* lis   r12,0x0700 */
        0x7D8903A6, /* mtctr r12 */
        0x4E800421, /* bctrl */
        0x38630001, /* addi  r3,r3,1 */
        0x7FE803A6, /* mtlr  r31 */
        0x4E800020, /* blr */
    };
    put_words(CODE, code, 7);
    cpu_stop s = cpu_run(CODE);
    CHECK_EQ(s.kind, CPU_STOP_TRAP);
    CHECK_EQ(s.addr, GUEST_TRAP_ADDR(0));
    CHECK_EQ(cpu_lr(), CODE + 16);
    cpu_set_gpr(3, 41);
    s = cpu_run(cpu_lr());
    CHECK_EQ(s.kind, CPU_STOP_RETURN);
    CHECK_EQ(cpu_gpr(3), 42);
}

TEST(cpu_reports_unmapped_read_as_fault) {
    fresh_machine();
    uint32_t code[] = {
        0x3C800580, /* lis r4,0x0580 */
        0x80640000, /* lwz r3,0(r4) */
        0x4E800020, /* blr */
    };
    put_words(CODE, code, 3);
    cpu_stop s = cpu_run(CODE);
    CHECK_EQ(s.kind, CPU_STOP_FAULT);
    CHECK_EQ(s.addr, 0x05800000u);
    CHECK_CONTAINS(s.detail, "0x05800000");
}

/* Review Focus 4: stack overflow into the guard gap. */
TEST(cpu_reports_stack_overflow_as_fault) {
    fresh_machine();
    uint32_t code[] = {
        0x3C800600, /* lis r4,0x0600 */
        0x9064FFF0, /* stw r3,-16(r4) */
        0x4E800020, /* blr */
    };
    put_words(CODE, code, 3);
    cpu_stop s = cpu_run(CODE);
    CHECK_EQ(s.kind, CPU_STOP_FAULT);
    CHECK_EQ(s.addr, 0x05FFFFF0u);
}

/* Review Focus 3: calling a null function pointer. */
TEST(cpu_reports_jump_to_zero_as_fault) {
    fresh_machine();
    uint32_t code[] = {
        0x39800000, /* li    r12,0 */
        0x7D8903A6, /* mtctr r12 */
        0x4E800420, /* bctr */
    };
    put_words(CODE, code, 3);
    cpu_stop s = cpu_run(CODE);
    CHECK_EQ(s.kind, CPU_STOP_FAULT);
}

TEST(cpu_reports_illegal_instruction_as_fault) {
    fresh_machine();
    uint32_t code[] = {0x00000000};
    put_words(CODE, code, 1);
    cpu_stop s = cpu_run(CODE);
    CHECK_EQ(s.kind, CPU_STOP_FAULT);
}

TEST(cpu_floating_point_is_enabled) {
    fresh_machine();
    uint32_t code[] = {
        0xFC22182A, /* fadd f1,f2,f3 */
        0x4E800020, /* blr */
    };
    put_words(CODE, code, 2);
    cpu_set_fpr(2, 2.5);
    cpu_set_fpr(3, 4.0);
    cpu_stop s = cpu_run(CODE);
    CHECK_EQ(s.kind, CPU_STOP_RETURN);
    CHECK(cpu_fpr(1) == 6.5);
}

TEST(cpu_save_restore_round_trip) {
    fresh_machine();
    cpu_set_gpr(3, 1);
    cpu_set_lr(0x1234);
    cpu_context *ctx = cpu_save();
    cpu_set_gpr(3, 2);
    cpu_set_lr(0x5678);
    cpu_restore(ctx);
    CHECK_EQ(cpu_gpr(3), 1);
    CHECK_EQ(cpu_lr(), 0x1234);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build`
Expected: the build fails with `'cpu.h' file not found`.

- [ ] **Step 3: Write the implementation**

`src/cpu.h`:
```c
#pragma once
#include <stdint.h>

/* Why cpu_run() stopped. */
typedef enum {
    CPU_STOP_RETURN, /* reached GUEST_RETURN_MAGIC */
    CPU_STOP_TRAP,   /* jumped to a trap address; addr = that address */
    CPU_STOP_FAULT,  /* anything else; addr = faulting address, detail = why */
} cpu_stop_kind;

typedef struct {
    cpu_stop_kind kind;
    uint32_t addr;
    uint32_t pc;
    char detail[96];
} cpu_stop;

typedef struct cpu_context cpu_context;

/* Creates the CPU and maps guest memory. Requires gm_init(). Replaces any
   previous CPU. */
void cpu_init(void);
void cpu_shutdown(void);

uint32_t cpu_gpr(int n);
void cpu_set_gpr(int n, uint32_t v);
double cpu_fpr(int n);
void cpu_set_fpr(int n, double v);
uint32_t cpu_lr(void);
void cpu_set_lr(uint32_t v);
uint32_t cpu_ctr(void);
uint32_t cpu_pc(void);

/* Runs guest code from pc until it returns to GUEST_RETURN_MAGIC, jumps to a
   trap address, or faults. Resume after a trap with cpu_run(cpu_lr()). */
cpu_stop cpu_run(uint32_t pc);

/* Snapshot of all registers. cpu_restore() restores and frees it. */
cpu_context *cpu_save(void);
void cpu_restore(cpu_context *ctx);
```

`src/cpu_unicorn.c`:
```c
#include "cpu.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>

#include "guest_mem.h"
#include "util.h"

#define MSR_FP 0x2000u

struct cpu_context {
    uc_context *uc_ctx;
};

static uc_engine *uc;
static uc_hook mem_hook, intr_hook;
static bool mem_fault;
static uc_mem_type mem_fault_type;
static uint32_t mem_fault_addr;
static bool intr_seen;
static uint32_t intr_no;

static void check(uc_err err, const char *what) {
    if (err != UC_ERR_OK)
        fatal("unicorn: %s failed: %s", what, uc_strerror(err));
}

static bool on_mem_invalid(uc_engine *engine, uc_mem_type type, uint64_t address, int size,
                           int64_t value, void *user) {
    (void)engine;
    (void)size;
    (void)value;
    (void)user;
    mem_fault = true;
    mem_fault_type = type;
    mem_fault_addr = (uint32_t)address;
    return false;
}

static void on_intr(uc_engine *engine, uint32_t intno, void *user) {
    (void)user;
    intr_seen = true;
    intr_no = intno;
    uc_emu_stop(engine);
}

static const char *mem_fault_desc(uc_mem_type type) {
    switch (type) {
    case UC_MEM_READ_UNMAPPED: return "read of unmapped address";
    case UC_MEM_WRITE_UNMAPPED: return "write to unmapped address";
    case UC_MEM_FETCH_UNMAPPED: return "jump to unmapped address";
    case UC_MEM_WRITE_PROT: return "write to read-only address";
    case UC_MEM_READ_PROT: return "read of unreadable address";
    case UC_MEM_FETCH_PROT: return "jump to non-executable address";
    default: return "memory fault at";
    }
}

static uint64_t reg_read(int reg) {
    uint64_t v = 0;
    check(uc_reg_read(uc, reg, &v), "register read");
    return v;
}

static void reg_write(int reg, uint64_t v) {
    check(uc_reg_write(uc, reg, &v), "register write");
}

void cpu_init(void) {
    if (uc)
        cpu_shutdown();
    check(uc_open(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN, &uc), "uc_open");
    check(uc_ctl_set_cpu_model(uc, UC_CPU_PPC32_750_V3_1), "set CPU model");
    const gm_region *regions;
    int n = gm_regions(&regions);
    for (int i = 0; i < n; i++) {
        uint32_t prot = 0;
        if (regions[i].prot & GM_PROT_R)
            prot |= UC_PROT_READ;
        if (regions[i].prot & GM_PROT_W)
            prot |= UC_PROT_WRITE;
        if (regions[i].prot & GM_PROT_X)
            prot |= UC_PROT_EXEC;
        check(uc_mem_map_ptr(uc, regions[i].base, regions[i].size, prot,
                             gm_host_base() + regions[i].base),
              "map guest memory");
    }
    check(uc_hook_add(uc, &mem_hook, UC_HOOK_MEM_INVALID, (void *)on_mem_invalid, NULL, 1, 0),
          "add memory hook");
    check(uc_hook_add(uc, &intr_hook, UC_HOOK_INTR, (void *)on_intr, NULL, 1, 0),
          "add interrupt hook");
    reg_write(UC_PPC_REG_MSR, reg_read(UC_PPC_REG_MSR) | MSR_FP);
}

void cpu_shutdown(void) {
    if (uc)
        uc_close(uc);
    uc = NULL;
}

uint32_t cpu_gpr(int n) { return (uint32_t)reg_read(UC_PPC_REG_0 + n); }
void cpu_set_gpr(int n, uint32_t v) { reg_write(UC_PPC_REG_0 + n, v); }
uint32_t cpu_lr(void) { return (uint32_t)reg_read(UC_PPC_REG_LR); }
void cpu_set_lr(uint32_t v) { reg_write(UC_PPC_REG_LR, v); }
uint32_t cpu_ctr(void) { return (uint32_t)reg_read(UC_PPC_REG_CTR); }
uint32_t cpu_pc(void) { return (uint32_t)reg_read(UC_PPC_REG_PC); }

double cpu_fpr(int n) {
    uint64_t bits = reg_read(UC_PPC_REG_FPR0 + n);
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

void cpu_set_fpr(int n, double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    reg_write(UC_PPC_REG_FPR0 + n, bits);
}

cpu_stop cpu_run(uint32_t pc) {
    cpu_stop s;
    memset(&s, 0, sizeof s);
    mem_fault = false;
    intr_seen = false;
    uc_err err = uc_emu_start(uc, pc, GUEST_RETURN_MAGIC, 0, 0);
    s.pc = cpu_pc();

    if (mem_fault && mem_fault_type == UC_MEM_FETCH_UNMAPPED) {
        uint32_t a = mem_fault_addr;
        if (a == GUEST_RETURN_MAGIC) {
            s.kind = CPU_STOP_RETURN;
            s.addr = a;
            return s;
        }
        if (a >= GUEST_TRAP_BASE && a < GUEST_TRAP_LIMIT && (a & 3u) == 0) {
            s.kind = CPU_STOP_TRAP;
            s.addr = a;
            return s;
        }
    }
    if (err == UC_ERR_OK && !mem_fault && !intr_seen && s.pc == GUEST_RETURN_MAGIC) {
        s.kind = CPU_STOP_RETURN;
        s.addr = s.pc;
        return s;
    }

    s.kind = CPU_STOP_FAULT;
    if (mem_fault) {
        s.addr = mem_fault_addr;
        snprintf(s.detail, sizeof s.detail, "%s 0x%08x", mem_fault_desc(mem_fault_type),
                 mem_fault_addr);
    } else if (intr_seen) {
        s.addr = s.pc;
        snprintf(s.detail, sizeof s.detail, "cpu exception %u", intr_no);
    } else {
        s.addr = s.pc;
        snprintf(s.detail, sizeof s.detail, "%s",
                 err == UC_ERR_OK ? "emulation stopped unexpectedly" : uc_strerror(err));
    }
    return s;
}

cpu_context *cpu_save(void) {
    cpu_context *ctx = malloc(sizeof *ctx);
    if (!ctx)
        fatal("out of memory");
    check(uc_context_alloc(uc, &ctx->uc_ctx), "context alloc");
    check(uc_context_save(uc, ctx->uc_ctx), "context save");
    return ctx;
}

void cpu_restore(cpu_context *ctx) {
    check(uc_context_restore(uc, ctx->uc_ctx), "context restore");
    uc_context_free(ctx->uc_ctx);
    free(ctx);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ./build/loony_tests cpu_`
Expected: `9 passed, 0 failed, 0 skipped`

If `cpu_floating_point_is_enabled` fails with a FAULT, Unicorn ignored the MSR write. Print `reg_read(UC_PPC_REG_MSR)` before and after the write to confirm, and check Unicorn's PPC `reg_write` for MSR handling. If `cpu_reports_jump_to_zero_as_fault` fails because it returns instead, Unicorn isn't enforcing the missing exec permission on low memory. Fix that by unmapping page 0 (make the first region start at `0x1000`) and updating the `gm_` tests to match. Don't weaken either test.

- [ ] **Step 5: Commit**

```bash
git add src/cpu.h src/cpu_unicorn.c tests/ppc.h tests/test_cpu.c
git commit -m "CPU wrapper over Unicorn with trap/return/fault stops"
```

---

### Task 4: PEF container parsing

**Files:**
- Create: `src/pef.h`, `src/pef.c`
- Create: `tests/test_pef.c`

**Interfaces:**
- Consumes: `rd_be16/32` (Task 1).
- Produces (the whole of `src/pef.h`, including declarations that Tasks 5 and 6 implement in `pef.c`):
  - Enums `PEF_KIND_*`, `PEF_SYM_*`, `PEF_MAX_SECTIONS`
  - `pef_section { total_len, unpacked_len, container_len, container_off, kind }`
  - `pef_import { char *name; char *library; uint8_t sym_class; bool weak; }`
  - `pef_file { file, file_len, nsections, sections[], main_section, main_offset, init_section, init_offset, nimports, imports, nreloc_sections, reloc_headers_off, reloc_instr_off }`
  - `bool pef_parse(const uint8_t *buf, size_t len, pef_file *out, char *err, size_t errlen)`: on failure it frees whatever it allocated. `buf` must outlive `out`.
  - `void pef_free(pef_file *pef)`: safe to call twice.

- [ ] **Step 1: Write the failing test**

`tests/test_pef.c`:
```c
#include "test.h"

#include <stdlib.h>

#include "pef.h"
#include "util.h"

TEST(pef_parses_the_real_executable) {
    SKIP_UNLESS_GAME();
    size_t len;
    uint8_t *buf = read_file(test_game_exe_path(), &len);
    CHECK(buf != NULL);
    pef_file pef;
    char err[256] = "";
    CHECK(pef_parse(buf, len, &pef, err, sizeof err));

    CHECK_EQ(pef.nsections, 3);
    CHECK_EQ(pef.sections[0].kind, PEF_KIND_CODE);
    CHECK_EQ(pef.sections[0].container_off, 0xCE0);
    CHECK_EQ(pef.sections[0].total_len, 280528);
    CHECK_EQ(pef.sections[1].kind, PEF_KIND_PATTERN_DATA);
    CHECK_EQ(pef.sections[1].total_len, 22892);
    CHECK_EQ(pef.sections[1].unpacked_len, 13616);
    CHECK_EQ(pef.sections[1].container_len, 8374);
    CHECK_EQ(pef.sections[2].kind, PEF_KIND_LOADER);

    CHECK_EQ(pef.main_section, 1);
    CHECK_EQ(pef.main_offset, 4832);
    CHECK_EQ(pef.init_section, -1);
    CHECK_EQ(pef.nreloc_sections, 1);

    CHECK_EQ(pef.nimports, 132);
    CHECK_STR(pef.imports[0].name, "FSClose");
    CHECK_STR(pef.imports[0].library, "CarbonLib");
    CHECK_EQ(pef.imports[0].sym_class, PEF_SYM_TVECTOR);
    CHECK(!pef.imports[0].weak);
    CHECK_STR(pef.imports[1].name, "StopAlert");
    CHECK_STR(pef.imports[36].name, "kCFPreferencesCurrentApplication");
    CHECK_EQ(pef.imports[36].sym_class, PEF_SYM_DATA);
    CHECK(pef.imports[107].weak);
    CHECK_STR(pef.imports[107].library, "CarbonLib");
    CHECK_STR(pef.imports[131].name, "EndFullScreen");
    CHECK_STR(pef.imports[131].library, "Apple;Carbon;Multimedia");
    CHECK(pef.imports[131].weak);

    int weak = 0;
    for (uint32_t i = 0; i < pef.nimports; i++)
        weak += pef.imports[i].weak;
    CHECK_EQ(weak, 25);

    pef_free(&pef);
    pef_free(&pef);
    free(buf);
}

/* Review Focus 2: wrong or corrupt executable. */
TEST(pef_rejects_non_pef_data) {
    uint8_t junk[64] = "this is not a PEF file at all";
    pef_file pef;
    char err[256] = "";
    CHECK(!pef_parse(junk, sizeof junk, &pef, err, sizeof err));
    CHECK_CONTAINS(err, "not a PowerPC PEF file");
    CHECK(!pef_parse(junk, 0, &pef, err, sizeof err));
}

TEST(pef_rejects_truncated_executable) {
    SKIP_UNLESS_GAME();
    size_t len;
    uint8_t *buf = read_file(test_game_exe_path(), &len);
    CHECK(buf != NULL);
    size_t cuts[] = {39, 60, 0x100, 0x400, 0xCE0 + 1000};
    for (size_t i = 0; i < sizeof cuts / sizeof cuts[0]; i++) {
        /* Copy so ASan catches any read past the cut. */
        uint8_t *part = malloc(cuts[i]);
        memcpy(part, buf, cuts[i]);
        pef_file pef;
        char err[256] = "";
        bool ok = pef_parse(part, cuts[i], &pef, err, sizeof err);
        free(part);
        CHECK(!ok);
        CHECK(err[0] != '\0');
    }
    free(buf);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build`
Expected: the build fails with `'pef.h' file not found`.

- [ ] **Step 3: Write the implementation**

`src/pef.h`:
```c
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* PEF (Preferred Executable Format), the classic Mac OS PowerPC container.
   Reference: "Mac OS Runtime Architectures", chapter 8. */

enum {
    PEF_KIND_CODE = 0,
    PEF_KIND_UNPACKED_DATA = 1,
    PEF_KIND_PATTERN_DATA = 2,
    PEF_KIND_CONSTANT = 3,
    PEF_KIND_LOADER = 4,
};

enum {
    PEF_SYM_CODE = 0,
    PEF_SYM_DATA = 1,
    PEF_SYM_TVECTOR = 2,
    PEF_SYM_TOC = 3,
    PEF_SYM_GLUE = 4,
};

#define PEF_MAX_SECTIONS 16

typedef struct {
    uint32_t total_len;     /* bytes in memory, including trailing zero fill */
    uint32_t unpacked_len;  /* initialized bytes */
    uint32_t container_len; /* bytes in the file */
    uint32_t container_off; /* file offset */
    uint8_t kind;           /* PEF_KIND_* */
} pef_section;

typedef struct {
    char *name;
    char *library;
    uint8_t sym_class; /* PEF_SYM_* */
    bool weak;
} pef_import;

typedef struct {
    const uint8_t *file;
    size_t file_len;
    int nsections;
    pef_section sections[PEF_MAX_SECTIONS];
    int32_t main_section; /* -1 if none */
    uint32_t main_offset;
    int32_t init_section; /* -1 if none */
    uint32_t init_offset;
    uint32_t nimports;
    pef_import *imports;
    uint32_t nreloc_sections;
    uint32_t reloc_headers_off; /* file offset of the first 12-byte relocation header */
    uint32_t reloc_instr_off;   /* file offset of the relocation instructions */
} pef_file;

/* Parses buf (which must outlive out). On failure writes a message to err,
   frees anything allocated, and returns false. */
bool pef_parse(const uint8_t *buf, size_t len, pef_file *out, char *err, size_t errlen);

/* Frees the import table. Safe to call more than once. */
void pef_free(pef_file *pef);

/* Task 5: unpacks pattern-initialized data. Must produce exactly dstlen bytes. */
bool pef_unpack_pattern(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen,
                        char *err, size_t errlen);

/* Task 6: one section's relocation state. */
typedef struct {
    uint8_t *host;     /* the instantiated section's bytes */
    uint32_t len;      /* bytes available at host */
    uint32_t section_c; /* initial sectionC: address of section 0 */
    uint32_t section_d; /* initial sectionD: address of section 1 */
    const uint32_t *import_addr;
    uint32_t nimports;
} pef_reloc_target;

/* Task 6: runs ninstrs 16-bit big-endian relocation instructions against t. */
bool pef_reloc_run(const uint8_t *instrs, uint32_t ninstrs, const pef_reloc_target *t,
                   char *err, size_t errlen);

/* Task 6: runs every relocation header in the file. Arrays are indexed by
   section; section_host[i] is NULL for sections that were not instantiated. */
bool pef_relocate(const pef_file *pef, uint8_t *const section_host[],
                  const uint32_t section_addr[], const uint32_t section_len[],
                  const uint32_t import_addr[], char *err, size_t errlen);
```

`src/pef.c`:
```c
#include "pef.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static bool fail(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static bool fail(char *err, size_t errlen, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return false;
}

static bool in_file(size_t file_len, uint64_t off, uint64_t len) {
    return off <= file_len && len <= file_len - off;
}

/* strdup of the NUL-terminated string at start, which must end before limit. */
static char *dup_string(const pef_file *pef, uint64_t start, uint64_t limit) {
    for (uint64_t i = start; i < limit; i++)
        if (pef->file[i] == '\0')
            return strdup((const char *)pef->file + start);
    return NULL;
}

static bool parse(const uint8_t *buf, size_t len, pef_file *out, char *err, size_t errlen) {
    out->file = buf;
    out->file_len = len;
    if (len < 40 || memcmp(buf, "Joy!peff", 8) != 0 || memcmp(buf + 8, "pwpc", 4) != 0)
        return fail(err, errlen, "not a PowerPC PEF file");

    uint16_t nsec = rd_be16(buf + 32);
    if (nsec > PEF_MAX_SECTIONS)
        return fail(err, errlen, "too many sections (%u)", nsec);
    if (!in_file(len, 40, 28ull * nsec))
        return fail(err, errlen, "section headers are truncated");

    int loader = -1;
    for (int i = 0; i < nsec; i++) {
        const uint8_t *h = buf + 40 + 28 * i;
        pef_section *s = &out->sections[i];
        s->total_len = rd_be32(h + 8);
        s->unpacked_len = rd_be32(h + 12);
        s->container_len = rd_be32(h + 16);
        s->container_off = rd_be32(h + 20);
        s->kind = h[24];
        if (!in_file(len, s->container_off, s->container_len))
            return fail(err, errlen, "section %d lies outside the file", i);
        if (s->kind == PEF_KIND_LOADER && loader < 0)
            loader = i;
    }
    out->nsections = nsec;
    if (loader < 0)
        return fail(err, errlen, "no loader section");

    uint64_t L = out->sections[loader].container_off;
    uint64_t Lend = L + out->sections[loader].container_len;
    if (Lend - L < 56)
        return fail(err, errlen, "loader section is truncated");
    const uint8_t *lh = buf + L;
    out->main_section = (int32_t)rd_be32(lh + 0);
    out->main_offset = rd_be32(lh + 4);
    out->init_section = (int32_t)rd_be32(lh + 8);
    out->init_offset = rd_be32(lh + 12);
    uint32_t nlibs = rd_be32(lh + 24);
    uint32_t nsyms = rd_be32(lh + 28);
    out->nreloc_sections = rd_be32(lh + 32);
    uint32_t reloc_instr = rd_be32(lh + 36);
    uint32_t strings = rd_be32(lh + 40);

    uint64_t libs_off = L + 56;
    uint64_t syms_off = libs_off + 24ull * nlibs;
    uint64_t rel_off = syms_off + 4ull * nsyms;
    if (rel_off + 12ull * out->nreloc_sections > Lend)
        return fail(err, errlen, "loader tables are truncated");
    if (L + reloc_instr > Lend || L + strings > Lend)
        return fail(err, errlen, "loader offsets are out of range");
    out->reloc_headers_off = (uint32_t)rel_off;
    out->reloc_instr_off = (uint32_t)(L + reloc_instr);

    out->nimports = nsyms;
    out->imports = calloc(nsyms ? nsyms : 1, sizeof *out->imports);
    if (!out->imports)
        return fail(err, errlen, "out of memory");

    uint64_t str_base = L + strings;
    for (uint32_t li = 0; li < nlibs; li++) {
        const uint8_t *e = buf + libs_off + 24ull * li;
        uint32_t name_off = rd_be32(e);
        uint32_t count = rd_be32(e + 12);
        uint32_t first = rd_be32(e + 16);
        if ((uint64_t)first + count > nsyms)
            return fail(err, errlen, "library %u has an out-of-range symbol list", li);
        for (uint32_t k = first; k < first + count; k++) {
            if (out->imports[k].library)
                return fail(err, errlen, "import %u belongs to two libraries", k);
            out->imports[k].library = dup_string(out, str_base + name_off, Lend);
            if (!out->imports[k].library)
                return fail(err, errlen, "library %u has a bad name", li);
        }
    }
    for (uint32_t i = 0; i < nsyms; i++) {
        if (!out->imports[i].library)
            return fail(err, errlen, "import %u has no library", i);
        uint32_t w = rd_be32(buf + syms_off + 4ull * i);
        out->imports[i].sym_class = (uint8_t)((w >> 24) & 0x0F);
        out->imports[i].weak = ((w >> 24) & 0x80) != 0;
        out->imports[i].name = dup_string(out, str_base + (w & 0xFFFFFF), Lend);
        if (!out->imports[i].name)
            return fail(err, errlen, "import %u has a bad name", i);
    }
    return true;
}

bool pef_parse(const uint8_t *buf, size_t len, pef_file *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    if (parse(buf, len, out, err, errlen))
        return true;
    pef_free(out);
    return false;
}

void pef_free(pef_file *pef) {
    if (pef->imports) {
        for (uint32_t i = 0; i < pef->nimports; i++) {
            free(pef->imports[i].name);
            free(pef->imports[i].library);
        }
        free(pef->imports);
    }
    pef->imports = NULL;
    pef->nimports = 0;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ./build/loony_tests pef_`
Expected: `3 passed, 0 failed, 0 skipped`. (`pef.h` already declares the Task 5 and 6 functions. Nothing calls them yet, so the link succeeds.)

- [ ] **Step 5: Commit**

```bash
git add src/pef.h src/pef.c tests/test_pef.c
git commit -m "PEF container and import table parsing"
```

---

### Task 5: Pattern-initialized data unpacking

**Files:**
- Modify: `src/pef.c` (append `pef_unpack_pattern` and its helpers)
- Create: `tests/test_pidata.c`

**Interfaces:**
- Consumes: `pef_parse`, `pef_section` (Task 4).
- Produces: `bool pef_unpack_pattern(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen, char *err, size_t errlen)`, already declared in `pef.h`.

Format: each instruction starts with one byte, opcode in the high 3 bits and count in the low 5. A count of 0 means the real count follows as a variable-length argument: 7 bits per byte, most significant group first, high bit set on every byte except the last. Opcodes:
- 0 Zero: write `count` zero bytes.
- 1 BlockCopy: copy the next `count` source bytes.
- 4 InterleaveRepeatBlockWithZero: `count` is the common size. It's followed by arguments `customSize` and `repeatCount`. Write `commonSize` zeros, then `repeatCount` times: copy `customSize` source bytes, write `commonSize` zeros.

Opcodes 2 and 3 aren't used by this game. They fail with "unsupported pattern opcode", in line with the spec's fail-loudly rule.

- [ ] **Step 1: Write the failing test**

`tests/test_pidata.c`:
```c
#include "test.h"

#include <stdlib.h>

#include "pef.h"
#include "util.h"

static bool unpack(const uint8_t *src, size_t n, uint8_t *dst, size_t dn, char *err) {
    memset(dst, 0xAA, dn);
    return pef_unpack_pattern(src, n, dst, dn, err, 256);
}

TEST(pidata_zero) {
    uint8_t src[] = {0x03};
    uint8_t dst[3];
    char err[256];
    CHECK(unpack(src, sizeof src, dst, sizeof dst, err));
    CHECK_EQ(dst[0], 0);
    CHECK_EQ(dst[2], 0);
}

TEST(pidata_block_copy) {
    uint8_t src[] = {0x23, 1, 2, 3};
    uint8_t dst[3];
    char err[256];
    CHECK(unpack(src, sizeof src, dst, sizeof dst, err));
    CHECK_EQ(dst[0], 1);
    CHECK_EQ(dst[2], 3);
}

TEST(pidata_count_from_argument) {
    uint8_t src[3 + 128];
    src[0] = 0x20; /* block copy, count in argument */
    src[1] = 0x81; /* argument 128 = 0b1_0000000 */
    src[2] = 0x00;
    for (int i = 0; i < 128; i++)
        src[3 + i] = (uint8_t)i;
    uint8_t dst[128];
    char err[256];
    CHECK(unpack(src, sizeof src, dst, sizeof dst, err));
    CHECK_EQ(dst[0], 0);
    CHECK_EQ(dst[127], 127);
}

TEST(pidata_interleave_with_zero) {
    /* common 2, custom 1, repeat 2, customs 'A' 'B' -> 00 00 A 00 00 B 00 00 */
    uint8_t src[] = {0x82, 0x01, 0x02, 'A', 'B'};
    uint8_t dst[8];
    uint8_t want[8] = {0, 0, 'A', 0, 0, 'B', 0, 0};
    char err[256];
    CHECK(unpack(src, sizeof src, dst, sizeof dst, err));
    CHECK(memcmp(dst, want, 8) == 0);
}

TEST(pidata_sequence_of_instructions) {
    uint8_t src[] = {0x21, 9, 0x02, 0x21, 7};
    uint8_t dst[4];
    uint8_t want[4] = {9, 0, 0, 7};
    char err[256];
    CHECK(unpack(src, sizeof src, dst, sizeof dst, err));
    CHECK(memcmp(dst, want, 4) == 0);
}

TEST(pidata_rejects_overflow) {
    uint8_t src[] = {0x05};
    uint8_t dst[4];
    char err[256];
    CHECK(!unpack(src, sizeof src, dst, sizeof dst, err));
    CHECK_CONTAINS(err, "overflows");
}

TEST(pidata_rejects_short_output) {
    uint8_t src[] = {0x02};
    uint8_t dst[4];
    char err[256];
    CHECK(!unpack(src, sizeof src, dst, sizeof dst, err));
    CHECK_CONTAINS(err, "produced 2 of 4");
}

TEST(pidata_rejects_unsupported_opcode) {
    uint8_t src[] = {0x41, 0x01, 0x00};
    uint8_t dst[4];
    char err[256];
    CHECK(!unpack(src, sizeof src, dst, sizeof dst, err));
    CHECK_CONTAINS(err, "unsupported pattern opcode 2");
}

TEST(pidata_rejects_truncated_input) {
    uint8_t src[] = {0x23, 1};
    uint8_t dst[3];
    char err[256];
    CHECK(!unpack(src, sizeof src, dst, sizeof dst, err));
    CHECK_CONTAINS(err, "truncated");
}

TEST(pidata_unpacks_the_real_data_section) {
    SKIP_UNLESS_GAME();
    size_t len;
    uint8_t *buf = read_file(test_game_exe_path(), &len);
    CHECK(buf != NULL);
    pef_file pef;
    char err[256] = "";
    CHECK(pef_parse(buf, len, &pef, err, sizeof err));
    const pef_section *s = &pef.sections[1];
    uint8_t *dst = malloc(s->unpacked_len);
    bool ok = pef_unpack_pattern(buf + s->container_off, s->container_len, dst, s->unpacked_len,
                                 err, sizeof err);
    uint32_t h = fnv1a32(dst, s->unpacked_len);
    free(dst);
    pef_free(&pef);
    free(buf);
    CHECK(ok);
    CHECK_EQ(h, 0xA2C244EDu);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build`
Expected: link fails with `undefined symbol: _pef_unpack_pattern`.

- [ ] **Step 3: Write the implementation (append to `src/pef.c`)**

```c
/* ---- pattern-initialized data ---- */

typedef struct {
    const uint8_t *p, *end;
    uint8_t *dst;
    size_t out, cap;
    char *err;
    size_t errlen;
} unpacker;

static bool u_arg(unpacker *u, uint32_t *v) {
    *v = 0;
    for (int i = 0; i < 5; i++) {
        if (u->p >= u->end)
            return fail(u->err, u->errlen, "truncated pattern data");
        uint8_t b = *u->p++;
        *v = (*v << 7) | (b & 0x7F);
        if (!(b & 0x80))
            return true;
    }
    return fail(u->err, u->errlen, "bad pattern-data argument");
}

static bool u_zero(unpacker *u, uint32_t n) {
    if (n > u->cap - u->out)
        return fail(u->err, u->errlen, "pattern data overflows the section");
    memset(u->dst + u->out, 0, n);
    u->out += n;
    return true;
}

static bool u_copy(unpacker *u, uint32_t n) {
    if (n > (size_t)(u->end - u->p))
        return fail(u->err, u->errlen, "truncated pattern data");
    if (n > u->cap - u->out)
        return fail(u->err, u->errlen, "pattern data overflows the section");
    memcpy(u->dst + u->out, u->p, n);
    u->p += n;
    u->out += n;
    return true;
}

bool pef_unpack_pattern(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen,
                        char *err, size_t errlen) {
    unpacker u = {src, src + srclen, dst, 0, dstlen, err, errlen};
    while (u.p < u.end) {
        uint8_t b = *u.p++;
        uint32_t op = b >> 5, count = b & 0x1F;
        if (count == 0 && !u_arg(&u, &count))
            return false;
        switch (op) {
        case 0: /* Zero */
            if (!u_zero(&u, count))
                return false;
            break;
        case 1: /* BlockCopy */
            if (!u_copy(&u, count))
                return false;
            break;
        case 4: { /* InterleaveRepeatBlockWithZero */
            uint32_t custom, repeat;
            if (!u_arg(&u, &custom) || !u_arg(&u, &repeat))
                return false;
            if (!u_zero(&u, count))
                return false;
            for (uint32_t i = 0; i < repeat; i++)
                if (!u_copy(&u, custom) || !u_zero(&u, count))
                    return false;
            break;
        }
        default:
            return fail(err, errlen, "unsupported pattern opcode %u", op);
        }
    }
    if (u.out != dstlen)
        return fail(err, errlen, "pattern data produced %zu of %zu bytes", u.out, dstlen);
    return true;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ./build/loony_tests pidata_`
Expected: `10 passed, 0 failed, 0 skipped`

- [ ] **Step 5: Commit**

```bash
git add src/pef.c tests/test_pidata.c
git commit -m "Unpack PEF pattern-initialized data"
```

---

### Task 6: Relocation engine

**Files:**
- Modify: `src/pef.c` (append `pef_reloc_run`, `pef_relocate`)
- Create: `tests/test_reloc.c`

**Interfaces:**
- Consumes: `pef_file`, `pef_reloc_target` (Task 4).
- Produces: `pef_reloc_run` and `pef_relocate`, as declared in `pef.h`.

Relocation instructions are 16-bit big-endian words. State: `pos` (byte offset in the section, starts at 0), `imp` (next import index, starts at 0), `sectionC` and `sectionD` (from the target). "Add X to the word at pos" means the big-endian 32-bit word at `host + pos` has X added to it. Supported opcodes, which are exactly the ones this game uses:

| Bits | Name | Action |
|---|---|---|
| `00 skip:8 count:6` | RelocBySectDWithSkip | `pos += skip*4`, then `count` times: add sectionD, `pos += 4` |
| `010 0000 n-1:9` | RelocBySectC | n times: add sectionC, `pos += 4` |
| `010 0001 n-1:9` | RelocBySectD | n times: add sectionD, `pos += 4` |
| `010 0011 n-1:9` | RelocTVector8 | n times: add sectionC at pos, add sectionD at pos+4, `pos += 8` |
| `010 0101 n-1:9` | RelocImportRun | n times: add `import_addr[imp++]`, `pos += 4` |
| `011 0000 idx:9` | RelocSmByImport | add `import_addr[idx]`, `imp = idx + 1`, `pos += 4` |
| `1000 off-1:12` | RelocIncrPosition | `pos += off` |

Everything else fails with `unsupported relocation opcode 0x%04x`.

- [ ] **Step 1: Write the failing test**

`tests/test_reloc.c`:
```c
#include "test.h"

#include "pef.h"
#include "util.h"

#define C 0x1000u
#define D 0x2000u

static const uint32_t imports[] = {0xA000, 0xB000, 0xC000};

typedef struct {
    uint8_t mem[32]; /* 8 big-endian words, each starting as 0x10 */
    pef_reloc_target t;
} fixture;

static void setup(fixture *f, uint32_t len) {
    for (int i = 0; i < 8; i++)
        wr_be32(f->mem + 4 * i, 0x10);
    f->t = (pef_reloc_target){f->mem, len, C, D, imports, 3};
}

static uint32_t word(const fixture *f, int i) { return rd_be32(f->mem + 4 * i); }

static bool run(fixture *f, const uint16_t *ops, uint32_t n, char *err) {
    uint8_t bytes[64];
    for (uint32_t i = 0; i < n; i++)
        wr_be16(bytes + 2 * i, ops[i]);
    return pef_reloc_run(bytes, n, &f->t, err, 256);
}

TEST(reloc_by_sect_d_with_skip) {
    fixture f;
    setup(&f, 32);
    uint16_t ops[] = {(1 << 6) | 2};
    char err[256];
    CHECK(run(&f, ops, 1, err));
    CHECK_EQ(word(&f, 0), 0x10);
    CHECK_EQ(word(&f, 1), D + 0x10);
    CHECK_EQ(word(&f, 2), D + 0x10);
    CHECK_EQ(word(&f, 3), 0x10);
}

TEST(reloc_runs_by_section) {
    fixture f;
    setup(&f, 32);
    uint16_t ops[] = {0x4001 /* BySectC x2 */, 0x4200 /* BySectD x1 */};
    char err[256];
    CHECK(run(&f, ops, 2, err));
    CHECK_EQ(word(&f, 0), C + 0x10);
    CHECK_EQ(word(&f, 1), C + 0x10);
    CHECK_EQ(word(&f, 2), D + 0x10);
}

TEST(reloc_tvector8) {
    fixture f;
    setup(&f, 32);
    uint16_t ops[] = {0x4601 /* TVector8 x2 */};
    char err[256];
    CHECK(run(&f, ops, 1, err));
    CHECK_EQ(word(&f, 0), C + 0x10);
    CHECK_EQ(word(&f, 1), D + 0x10);
    CHECK_EQ(word(&f, 2), C + 0x10);
    CHECK_EQ(word(&f, 3), D + 0x10);
}

TEST(reloc_import_run_advances_import_index) {
    fixture f;
    setup(&f, 32);
    uint16_t ops[] = {0x4A00 /* ImportRun x1 */, 0x4A00 /* ImportRun x1 */};
    char err[256];
    CHECK(run(&f, ops, 2, err));
    CHECK_EQ(word(&f, 0), 0xA000 + 0x10);
    CHECK_EQ(word(&f, 1), 0xB000 + 0x10);
}

TEST(reloc_small_by_import_sets_import_index) {
    fixture f;
    setup(&f, 32);
    uint16_t ops[] = {0x6001 /* SmByImport 1 */, 0x4A00 /* ImportRun x1 */};
    char err[256];
    CHECK(run(&f, ops, 2, err));
    CHECK_EQ(word(&f, 0), 0xB000 + 0x10);
    CHECK_EQ(word(&f, 1), 0xC000 + 0x10);
}

TEST(reloc_incr_position) {
    fixture f;
    setup(&f, 32);
    uint16_t ops[] = {0x8007 /* IncrPosition 8 */, 0x4200 /* BySectD x1 */};
    char err[256];
    CHECK(run(&f, ops, 2, err));
    CHECK_EQ(word(&f, 1), 0x10);
    CHECK_EQ(word(&f, 2), D + 0x10);
}

TEST(reloc_rejects_unsupported_opcode) {
    fixture f;
    setup(&f, 32);
    uint16_t ops[] = {0x9000 /* RelocSmRepeat */};
    char err[256];
    CHECK(!run(&f, ops, 1, err));
    CHECK_CONTAINS(err, "unsupported relocation opcode 0x9000");
}

TEST(reloc_rejects_write_past_section_end) {
    fixture f;
    setup(&f, 8);
    uint16_t ops[] = {0x4002 /* BySectC x3 over a 2-word section */};
    char err[256];
    CHECK(!run(&f, ops, 1, err));
    CHECK_CONTAINS(err, "past the end");
}

TEST(reloc_rejects_import_out_of_range) {
    fixture f;
    setup(&f, 32);
    uint16_t ops[] = {0x6005 /* SmByImport 5 */};
    char err[256];
    CHECK(!run(&f, ops, 1, err));
    CHECK_CONTAINS(err, "import index 5");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build`
Expected: link fails with `undefined symbol: _pef_reloc_run`.

- [ ] **Step 3: Write the implementation (append to `src/pef.c`)**

```c
/* ---- relocations ---- */

static bool add_word(const pef_reloc_target *t, uint32_t pos, uint32_t value, char *err,
                     size_t errlen) {
    if ((uint64_t)pos + 4 > t->len)
        return fail(err, errlen, "relocation at offset 0x%x is past the end of the section", pos);
    wr_be32(t->host + pos, rd_be32(t->host + pos) + value);
    return true;
}

static bool add_import(const pef_reloc_target *t, uint32_t pos, uint32_t idx, char *err,
                       size_t errlen) {
    if (idx >= t->nimports)
        return fail(err, errlen, "import index %u out of range", idx);
    return add_word(t, pos, t->import_addr[idx], err, errlen);
}

bool pef_reloc_run(const uint8_t *instrs, uint32_t ninstrs, const pef_reloc_target *t,
                   char *err, size_t errlen) {
    uint32_t pos = 0, imp = 0;
    const uint32_t sc = t->section_c, sd = t->section_d;
    for (uint32_t k = 0; k < ninstrs; k++) {
        uint16_t w = rd_be16(instrs + 2 * k);
        if ((w >> 14) == 0) { /* RelocBySectDWithSkip */
            pos += ((w >> 6) & 0xFFu) * 4u;
            for (uint32_t n = w & 0x3Fu; n > 0; n--, pos += 4)
                if (!add_word(t, pos, sd, err, errlen))
                    return false;
        } else if ((w >> 13) == 2) { /* run group */
            uint32_t sub = (w >> 9) & 0xFu, run = (w & 0x1FFu) + 1u;
            for (uint32_t n = 0; n < run; n++) {
                switch (sub) {
                case 0: /* RelocBySectC */
                    if (!add_word(t, pos, sc, err, errlen))
                        return false;
                    pos += 4;
                    break;
                case 1: /* RelocBySectD */
                    if (!add_word(t, pos, sd, err, errlen))
                        return false;
                    pos += 4;
                    break;
                case 3: /* RelocTVector8 */
                    if (!add_word(t, pos, sc, err, errlen) || !add_word(t, pos + 4, sd, err, errlen))
                        return false;
                    pos += 8;
                    break;
                case 5: /* RelocImportRun */
                    if (!add_import(t, pos, imp++, err, errlen))
                        return false;
                    pos += 4;
                    break;
                default:
                    return fail(err, errlen, "unsupported relocation opcode 0x%04x", w);
                }
            }
        } else if ((w >> 13) == 3) { /* small-index group */
            uint32_t sub = (w >> 9) & 0xFu, idx = w & 0x1FFu;
            if (sub != 0)
                return fail(err, errlen, "unsupported relocation opcode 0x%04x", w);
            if (!add_import(t, pos, idx, err, errlen)) /* RelocSmByImport */
                return false;
            imp = idx + 1;
            pos += 4;
        } else if ((w >> 12) == 8) { /* RelocIncrPosition */
            pos += (w & 0xFFFu) + 1u;
        } else {
            return fail(err, errlen, "unsupported relocation opcode 0x%04x", w);
        }
    }
    return true;
}

bool pef_relocate(const pef_file *pef, uint8_t *const section_host[],
                  const uint32_t section_addr[], const uint32_t section_len[],
                  const uint32_t import_addr[], char *err, size_t errlen) {
    for (uint32_t r = 0; r < pef->nreloc_sections; r++) {
        const uint8_t *h = pef->file + pef->reloc_headers_off + 12ull * r;
        uint16_t si = rd_be16(h);
        uint32_t count = rd_be32(h + 4);
        uint32_t first = rd_be32(h + 8);
        if (si >= pef->nsections || !section_host[si])
            return fail(err, errlen, "relocations target section %u, which is not loaded", si);
        uint64_t off = (uint64_t)pef->reloc_instr_off + first;
        if (!in_file(pef->file_len, off, 2ull * count))
            return fail(err, errlen, "relocation instructions lie outside the file");
        pef_reloc_target t = {
            section_host[si],
            section_len[si],
            section_addr[0],
            pef->nsections > 1 ? section_addr[1] : 0,
            import_addr,
            pef->nimports,
        };
        if (!pef_reloc_run(pef->file + off, count, &t, err, errlen))
            return false;
    }
    return true;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ./build/loony_tests reloc_`
Expected: `9 passed, 0 failed, 0 skipped`

- [ ] **Step 5: Commit**

```bash
git add src/pef.c tests/test_reloc.c
git commit -m "PEF relocation engine for the opcodes this game uses"
```

---

### Task 7: Image loader

**Files:**
- Create: `src/loader.h`, `src/loader.c`
- Create: `tests/test_loader.c`

**Interfaces:**
- Consumes: `pef_parse`, `pef_free`, `pef_unpack_pattern`, `pef_relocate` (Tasks 4–6); `gm_ptr`, `gm_w32`, `GUEST_TRAP_ADDR`, image constants (Task 2).
- Produces:
  - `loaded_image { pef_file pef; uint32_t code_base, code_len, data_base, data_len, import_area; uint32_t *import_addr; uint32_t main_tvector, init_tvector; }`. `main_tvector` and `init_tvector` are 0 when absent.
  - `bool image_load(const uint8_t *buf, size_t len, loaded_image *img, char *err, size_t errlen)`: requires `gm_init()`; `buf` must outlive `img`.
  - `void image_free(loaded_image *img)`
  - Import binding rule: TVECTOR imports get an 8-byte slot `{GUEST_TRAP_ADDR(i), 0}` and `import_addr[i]` = the slot. DATA imports get a zeroed slot and `import_addr[i]` = the slot (Plan 2 fills in `kCFPreferencesCurrentApplication`). CODE imports get `import_addr[i] = GUEST_TRAP_ADDR(i)`. TOC and GLUE are errors.

Layout: instantiate sections of kind code, unpacked data, pattern data and constant, in file order. Each starts on a 4 KB boundary from `GUEST_IMAGE_BASE`. The import area starts at the next 16-byte boundary after the last section.

- [ ] **Step 1: Write the failing test**

`tests/test_loader.c`:
```c
#include "test.h"

#include <stdlib.h>

#include "guest_mem.h"
#include "loader.h"
#include "util.h"

TEST(loader_loads_the_real_executable) {
    SKIP_UNLESS_GAME();
    size_t len;
    uint8_t *buf = read_file(test_game_exe_path(), &len);
    CHECK(buf != NULL);
    gm_init();
    loaded_image img;
    char err[256] = "";
    CHECK(image_load(buf, len, &img, err, sizeof err));

    CHECK_EQ(img.code_base, 0x00100000u);
    CHECK_EQ(img.code_len, 280528);
    CHECK_EQ(gm_r32(img.code_base), 0x7C0802A6u);
    CHECK_EQ(fnv1a32(gm_ptr(img.code_base, img.code_len), img.code_len), 0xA50D49B8u);

    CHECK_EQ(img.data_base, 0x00145000u);
    CHECK_EQ(img.data_len, 22892);
    CHECK_EQ(fnv1a32(gm_ptr(img.data_base, img.data_len), img.data_len), 0xA7C47401u);

    CHECK_EQ(img.import_area, 0x0014A970u);
    CHECK_EQ(img.import_addr[0], 0x0014A970u);
    CHECK_EQ(gm_r32(img.import_addr[0]), GUEST_TRAP_ADDR(0));
    CHECK_EQ(gm_r32(img.import_addr[0] + 4), 0);
    CHECK_EQ(gm_r32(img.import_addr[131]), GUEST_TRAP_ADDR(131));
    CHECK_EQ(img.import_addr[36], 0x0014A970u + 36 * 8);
    CHECK_EQ(gm_r32(img.import_addr[36]), 0);

    CHECK_EQ(img.main_tvector, 0x001462E0u);
    CHECK_EQ(gm_r32(img.main_tvector), 0x001387E0u);
    CHECK_EQ(gm_r32(img.main_tvector + 4), 0x00145000u);
    CHECK_EQ(img.init_tvector, 0);

    image_free(&img);
    free(buf);
}

TEST(loader_reports_parse_errors) {
    uint8_t junk[64] = {0};
    gm_init();
    loaded_image img;
    char err[256] = "";
    CHECK(!image_load(junk, sizeof junk, &img, err, sizeof err));
    CHECK_CONTAINS(err, "not a PowerPC PEF file");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build`
Expected: the build fails with `'loader.h' file not found`.

- [ ] **Step 3: Write the implementation**

`src/loader.h`:
```c
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pef.h"

typedef struct {
    pef_file pef;
    uint32_t code_base, code_len;
    uint32_t data_base, data_len;
    uint32_t import_area;   /* 8 bytes per import */
    uint32_t *import_addr;  /* what each import resolved to; pef.nimports entries */
    uint32_t main_tvector;  /* 0 if none */
    uint32_t init_tvector;  /* 0 if none */
} loaded_image;

/* Loads a PEF executable into guest memory: places sections, unpacks data,
   binds imports to trap addresses, and relocates. Requires gm_init(). buf
   must outlive img. On failure, writes err and leaves nothing to free. */
bool image_load(const uint8_t *buf, size_t len, loaded_image *img, char *err, size_t errlen);
void image_free(loaded_image *img);
```

`src/loader.c`:
```c
#include "loader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_mem.h"

static bool fail(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static bool fail(char *err, size_t errlen, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return false;
}

static uint32_t align_up(uint32_t v, uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}

static bool instantiable(uint8_t kind) {
    return kind == PEF_KIND_CODE || kind == PEF_KIND_UNPACKED_DATA ||
           kind == PEF_KIND_PATTERN_DATA || kind == PEF_KIND_CONSTANT;
}

static bool entry_point(const pef_file *pef, uint8_t *const host[], const uint32_t addr[],
                        const uint32_t size[], int32_t section, uint32_t offset, uint32_t *out,
                        const char *what, char *err, size_t errlen) {
    *out = 0;
    if (section < 0)
        return true;
    if (section >= pef->nsections || !host[section] || (uint64_t)offset + 8 > size[section])
        return fail(err, errlen, "%s entry point is not inside a loaded section", what);
    *out = addr[section] + offset;
    return true;
}

static bool load(const uint8_t *buf, size_t len, loaded_image *img, char *err, size_t errlen) {
    if (!pef_parse(buf, len, &img->pef, err, errlen))
        return false;
    const pef_file *pef = &img->pef;

    uint8_t *host[PEF_MAX_SECTIONS] = {0};
    uint32_t addr[PEF_MAX_SECTIONS] = {0};
    uint32_t size[PEF_MAX_SECTIONS] = {0};
    uint32_t cursor = GUEST_IMAGE_BASE;

    for (int i = 0; i < pef->nsections; i++) {
        const pef_section *s = &pef->sections[i];
        if (!instantiable(s->kind) || s->total_len == 0)
            continue;
        if (s->unpacked_len > s->total_len)
            return fail(err, errlen, "section %d is larger unpacked than in memory", i);
        uint32_t base = align_up(cursor, 0x1000);
        if ((uint64_t)base + s->total_len > GUEST_IMAGE_LIMIT)
            return fail(err, errlen, "section %d does not fit in the image area", i);
        uint8_t *dst = gm_ptr(base, s->total_len);
        memset(dst, 0, s->total_len);
        const uint8_t *src = buf + s->container_off;
        if (s->kind == PEF_KIND_PATTERN_DATA) {
            if (!pef_unpack_pattern(src, s->container_len, dst, s->unpacked_len, err, errlen))
                return false;
        } else {
            if (s->container_len < s->unpacked_len)
                return fail(err, errlen, "section %d is shorter in the file than unpacked", i);
            memcpy(dst, src, s->unpacked_len);
        }
        host[i] = dst;
        addr[i] = base;
        size[i] = s->total_len;
        cursor = base + s->total_len;
        if (s->kind == PEF_KIND_CODE && !img->code_base) {
            img->code_base = base;
            img->code_len = s->total_len;
        }
        if ((s->kind == PEF_KIND_UNPACKED_DATA || s->kind == PEF_KIND_PATTERN_DATA) &&
            !img->data_base) {
            img->data_base = base;
            img->data_len = s->total_len;
        }
    }

    if (pef->nimports > (GUEST_TRAP_LIMIT - GUEST_TRAP_BASE) / 4)
        return fail(err, errlen, "too many imports (%u)", pef->nimports);
    img->import_area = align_up(cursor, 16);
    if ((uint64_t)img->import_area + 8ull * pef->nimports > GUEST_IMAGE_LIMIT)
        return fail(err, errlen, "imports do not fit in the image area");
    img->import_addr = calloc(pef->nimports ? pef->nimports : 1, sizeof *img->import_addr);
    if (!img->import_addr)
        return fail(err, errlen, "out of memory");

    for (uint32_t i = 0; i < pef->nimports; i++) {
        const pef_import *im = &pef->imports[i];
        uint32_t slot = img->import_area + 8 * i;
        switch (im->sym_class) {
        case PEF_SYM_TVECTOR:
            gm_w32(slot, GUEST_TRAP_ADDR(i));
            gm_w32(slot + 4, 0);
            img->import_addr[i] = slot;
            break;
        case PEF_SYM_DATA:
            gm_w32(slot, 0);
            gm_w32(slot + 4, 0);
            img->import_addr[i] = slot;
            break;
        case PEF_SYM_CODE:
            img->import_addr[i] = GUEST_TRAP_ADDR(i);
            break;
        default:
            return fail(err, errlen, "import %s has unsupported symbol class %u", im->name,
                        im->sym_class);
        }
    }

    if (!pef_relocate(pef, host, addr, size, img->import_addr, err, errlen))
        return false;
    if (!entry_point(pef, host, addr, size, pef->main_section, pef->main_offset,
                     &img->main_tvector, "main", err, errlen))
        return false;
    if (!entry_point(pef, host, addr, size, pef->init_section, pef->init_offset,
                     &img->init_tvector, "init", err, errlen))
        return false;
    return true;
}

bool image_load(const uint8_t *buf, size_t len, loaded_image *img, char *err, size_t errlen) {
    memset(img, 0, sizeof *img);
    if (load(buf, len, img, err, errlen))
        return true;
    image_free(img);
    return false;
}

void image_free(loaded_image *img) {
    pef_free(&img->pef);
    free(img->import_addr);
    memset(img, 0, sizeof *img);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ./build/loony_tests loader_`
Expected: `2 passed, 0 failed, 0 skipped`

If the relocated-data checksum doesn't match but everything else does, compare `gm_r32(img.main_tvector)` first. The reference values came from a Python model of the same rules, so a mismatch points to a difference in opcode handling in Task 6.

- [ ] **Step 5: Commit**

```bash
git add src/loader.h src/loader.c tests/test_loader.c
git commit -m "Load PEF image into guest memory and bind imports to traps"
```

---

### Task 8: Import dispatch, guest_call and crash reports

**Files:**
- Create: `src/trap.h`, `src/trap.c`
- Create: `tests/test_trap.c`

**Interfaces:**
- Consumes: `cpu_*` (Task 3); `gm_r32`, `gm_w32`, `GUEST_*` (Task 2); `fatal` (Task 1).
- Produces:
  - `typedef void (*trap_handler)(void);` Handlers read arguments with `trap_arg(n)` (n = 0..7 is r3..r10) and return with `trap_return(v)` (sets r3).
  - `void trap_init(uint32_t nimports, const char *const *names, uint32_t code_base, uint32_t code_len)`: requires `cpu_init()`; resets handlers and history; sets r1 to `GUEST_STACK_TOP - 64`; reads `LOONY_TRACE`. `names` must outlive trap use.
  - `void trap_shutdown(void)`
  - `void trap_register(const char *name, trap_handler fn)`: ignored if the game doesn't import `name`.
  - `uint32_t guest_call(uint32_t tvector, int nargs, const uint32_t *args)`: re-entrant; returns the guest's r3.
  - `const char *trap_import_name(uint32_t index)`
  - `_Noreturn void trap_crash(const char *fmt, ...)`: prints `loony: crash: <msg>`, registers, depth and the last 64 imports, then exits 2.
  - Trace (`LOONY_TRACE` contains `imports`): `loony: trace: #<n> <Name>(0x........, 0x........, 0x........, 0x........) from <addr>`, then `loony: trace:   -> 0x........`

Notes for the implementer:
- The Mac OS PowerPC ABI lets leaf functions use a 224-byte "red zone" below r1. So `guest_call` builds its frame 512 bytes below the current r1, 16-byte aligned, and writes the back-chain word (the old r1) at the new r1.
- Capture LR before running the handler and resume there. A handler that calls `guest_call` gets all registers restored when that call returns, so this is equivalent, but being explicit makes it obvious.
- Record each import in the history **before** checking whether it has a handler, so the unimplemented one appears in its own crash report.

- [ ] **Step 1: Write the failing test**

`tests/test_trap.c`:
```c
#include "test.h"

#include <stdlib.h>

#include "ppc.h"
#include "trap.h"

#define OUTER    GUEST_IMAGE_BASE
#define INNER    (GUEST_IMAGE_BASE + 0x100)
#define TV_IMP0  (GUEST_IMAGE_BASE + 0x8000)
#define TV_IMP1  (GUEST_IMAGE_BASE + 0x8008)
#define TV_OUTER (GUEST_IMAGE_BASE + 0x8010)
#define TV_INNER (GUEST_IMAGE_BASE + 0x8018)

/* A guest function that calls the function whose transition vector is at tv
   (CFM-style: load code and TOC from the TV, bctrl), then adds 1 to r3. */
static void emit_caller(uint32_t at, uint32_t tv) {
    uint32_t code[] = {
        0x7C0802A6,                  /* mflr  r0 */
        0x90010008,                  /* stw   r0,8(r1) */
        0x9421FFC0,                  /* stwu  r1,-64(r1) */
        0x3D800000 | (tv >> 16),     /* lis   r12,hi(tv) */
        0x618C0000 | (tv & 0xFFFF),  /* ori   r12,r12,lo(tv) */
        0x800C0000,                  /* lwz   r0,0(r12) */
        0x804C0004,                  /* lwz   r2,4(r12) */
        0x7C0903A6,                  /* mtctr r0 */
        0x4E800421,                  /* bctrl */
        0x38630001,                  /* addi  r3,r3,1 */
        0x38210040,                  /* addi  r1,r1,64 */
        0x80010008,                  /* lwz   r0,8(r1) */
        0x7C0803A6,                  /* mtlr  r0 */
        0x4E800020,                  /* blr */
    };
    put_words(at, code, (int)(sizeof code / sizeof code[0]));
}

static void set_tv(uint32_t at, uint32_t code) {
    gm_w32(at, code);
    gm_w32(at + 4, 0);
}

static void setup(const char *const *names, uint32_t n) {
    fresh_machine();
    trap_init(n, names, GUEST_IMAGE_BASE, 0x10000);
    set_tv(TV_IMP0, GUEST_TRAP_ADDR(0));
    set_tv(TV_IMP1, GUEST_TRAP_ADDR(1));
    set_tv(TV_OUTER, OUTER);
    set_tv(TV_INNER, INNER);
    emit_caller(OUTER, TV_IMP0);
}

static void h_double(void) {
    trap_return(trap_arg(0) * 2);
}

static void h_call_inner(void) {
    uint32_t a = trap_arg(0);
    trap_return(guest_call(TV_INNER, 1, &a));
}

TEST(trap_calls_handler_and_resumes) {
    static const char *const names[] = {"TestDouble"};
    setup(names, 1);
    trap_register("TestDouble", h_double);
    trap_register("NotImported", h_double); /* ignored */
    uint32_t arg = 5;
    CHECK_EQ(guest_call(TV_OUTER, 1, &arg), 11);
    CHECK_STR(trap_import_name(0), "TestDouble");
}

TEST(trap_nested_guest_call) {
    static const char *const names[] = {"CallInner"};
    setup(names, 1);
    uint32_t inner[] = {0x38630064 /* addi r3,r3,100 */, 0x4E800020 /* blr */};
    put_words(INNER, inner, 2);
    trap_register("CallInner", h_call_inner);
    uint32_t sp = cpu_gpr(1), arg = 5;
    CHECK_EQ(guest_call(TV_OUTER, 1, &arg), 106);
    CHECK_EQ(cpu_gpr(1), sp);
}

static void child_unimplemented(void *unused) {
    (void)unused;
    static const char *const names[] = {"FooBar"};
    setup(names, 1);
    uint32_t arg = 5;
    guest_call(TV_OUTER, 1, &arg);
}

TEST(trap_unimplemented_import_crashes) {
    char out[16384];
    int status = test_run_child(child_unimplemented, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: crash: unimplemented import FooBar");
    CHECK_CONTAINS(out, "FooBar(0x00000005");
    CHECK_CONTAINS(out, "code+0x");
}

/* Review Focus 5: crash while nested inside a callback. */
static void child_nested_missing(void *unused) {
    (void)unused;
    static const char *const names[] = {"CallInner", "Missing"};
    setup(names, 2);
    emit_caller(INNER, TV_IMP1);
    trap_register("CallInner", h_call_inner);
    uint32_t arg = 5;
    guest_call(TV_OUTER, 1, &arg);
}

TEST(trap_crash_while_nested_reports_depth_and_history) {
    char out[16384];
    int status = test_run_child(child_nested_missing, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "unimplemented import Missing");
    CHECK_CONTAINS(out, "depth 2");
    CHECK_CONTAINS(out, "CallInner(0x00000005");
}

static void child_fault(void *unused) {
    (void)unused;
    static const char *const names[] = {"Unused"};
    setup(names, 1);
    uint32_t code[] = {
        0x3C800580, /* lis r4,0x0580 */
        0x80640000, /* lwz r3,0(r4) */
        0x4E800020, /* blr */
    };
    put_words(INNER, code, 3);
    guest_call(TV_INNER, 0, NULL);
}

TEST(trap_guest_fault_crashes) {
    char out[16384];
    int status = test_run_child(child_fault, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: crash: guest fault");
    CHECK_CONTAINS(out, "0x05800000");
}

static void child_trace(void *unused) {
    (void)unused;
    setenv("LOONY_TRACE", "imports", 1);
    static const char *const names[] = {"TestDouble"};
    setup(names, 1);
    trap_register("TestDouble", h_double);
    uint32_t arg = 5;
    guest_call(TV_OUTER, 1, &arg);
}

TEST(trap_trace_logs_imports) {
    char out[16384];
    int status = test_run_child(child_trace, NULL, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: trace: #0 TestDouble(0x00000005");
    CHECK_CONTAINS(out, "-> 0x0000000a");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build`
Expected: the build fails with `'trap.h' file not found`.

- [ ] **Step 3: Write the implementation**

`src/trap.h`:
```c
#pragma once
#include <stdint.h>

#include "cpu.h"

/* A C implementation of one imported function. Reads arguments with
   trap_arg() and sets the result with trap_return(). */
typedef void (*trap_handler)(void);

/* Requires cpu_init(). names[i] is import i's name and must outlive trap use.
   code_base/code_len are used to print code addresses as code+0xNNNNN. */
void trap_init(uint32_t nimports, const char *const *names, uint32_t code_base,
               uint32_t code_len);
void trap_shutdown(void);

/* Installs fn for the import called name. Ignored if the game doesn't import it. */
void trap_register(const char *name, trap_handler fn);

/* Calls the guest function whose transition vector is at tvector, with up to
   8 word arguments in r3..r10. Returns the guest's r3. Re-entrant: handlers
   may call it again. All registers are restored before it returns. */
uint32_t guest_call(uint32_t tvector, int nargs, const uint32_t *args);

const char *trap_import_name(uint32_t index);

/* Prints a crash report (message, registers, depth, recent imports) and exits 2. */
_Noreturn void trap_crash(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static inline uint32_t trap_arg(int n) { return cpu_gpr(3 + n); }
static inline void trap_return(uint32_t v) { cpu_set_gpr(3, v); }
```

`src/trap.c`:
```c
#include "trap.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_mem.h"
#include "util.h"

#define HISTORY 64
/* Below the caller's r1: 224-byte red zone plus room for a linkage area. */
#define CALL_FRAME_GAP 512u

typedef struct {
    uint32_t index, lr, a[4];
} hist_entry;

static struct {
    uint32_t n;
    const char *const *names;
    trap_handler *handlers;
    uint32_t code_base, code_len;
    hist_entry hist[HISTORY];
    uint32_t hist_count;
    int depth;
    bool trace_imports;
} T;

static const char *fmt_addr(uint32_t a, char buf[static 32]) {
    if (T.code_len && a >= T.code_base && a - T.code_base < T.code_len)
        snprintf(buf, 32, "code+0x%05x", a - T.code_base);
    else
        snprintf(buf, 32, "0x%08x", a);
    return buf;
}

void trap_init(uint32_t nimports, const char *const *names, uint32_t code_base,
               uint32_t code_len) {
    trap_shutdown();
    T.n = nimports;
    T.names = names;
    T.handlers = calloc(nimports ? nimports : 1, sizeof *T.handlers);
    if (!T.handlers)
        fatal("out of memory");
    T.code_base = code_base;
    T.code_len = code_len;
    const char *trace = getenv("LOONY_TRACE");
    T.trace_imports = trace && strstr(trace, "imports");
    cpu_set_gpr(1, GUEST_STACK_TOP - 64);
}

void trap_shutdown(void) {
    free(T.handlers);
    memset(&T, 0, sizeof T);
}

void trap_register(const char *name, trap_handler fn) {
    for (uint32_t i = 0; i < T.n; i++)
        if (strcmp(T.names[i], name) == 0)
            T.handlers[i] = fn;
}

const char *trap_import_name(uint32_t index) {
    return index < T.n ? T.names[index] : "(unknown)";
}

static void report_state(void) {
    char a[32], b[32];
    fprintf(stderr, "  pc %s  lr %s  ctr 0x%08x  depth %d\n", fmt_addr(cpu_pc(), a),
            fmt_addr(cpu_lr(), b), cpu_ctr(), T.depth);
    for (int r = 0; r < 32; r += 4)
        fprintf(stderr, "  r%-2d 0x%08x  r%-2d 0x%08x  r%-2d 0x%08x  r%-2d 0x%08x\n", r,
                cpu_gpr(r), r + 1, cpu_gpr(r + 1), r + 2, cpu_gpr(r + 2), r + 3, cpu_gpr(r + 3));
    uint32_t n = T.hist_count < HISTORY ? T.hist_count : HISTORY;
    fprintf(stderr, "  last %u imports (oldest first):\n", n);
    for (uint32_t k = T.hist_count - n; k < T.hist_count; k++) {
        const hist_entry *h = &T.hist[k % HISTORY];
        fprintf(stderr, "    #%u %s(0x%08x, 0x%08x, 0x%08x, 0x%08x) from %s\n", k,
                trap_import_name(h->index), h->a[0], h->a[1], h->a[2], h->a[3],
                fmt_addr(h->lr, a));
    }
}

void trap_crash(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("loony: crash: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    report_state();
    exit(2);
}

static const hist_entry *record(uint32_t index) {
    hist_entry *h = &T.hist[T.hist_count % HISTORY];
    h->index = index;
    h->lr = cpu_lr();
    for (int i = 0; i < 4; i++)
        h->a[i] = cpu_gpr(3 + i);
    T.hist_count++;
    return h;
}

uint32_t guest_call(uint32_t tvector, int nargs, const uint32_t *args) {
    if (nargs < 0 || nargs > 8)
        fatal("guest_call: bad argument count %d", nargs);
    uint32_t code = gm_r32(tvector), toc = gm_r32(tvector + 4);
    cpu_context *saved = cpu_save();

    uint32_t old_sp = cpu_gpr(1);
    uint32_t sp = (old_sp - CALL_FRAME_GAP) & ~15u;
    if (sp < GUEST_STACK_BASE + 4096)
        trap_crash("guest stack exhausted");
    gm_w32(sp, old_sp); /* back chain */
    cpu_set_gpr(1, sp);
    cpu_set_gpr(2, toc);
    cpu_set_gpr(12, tvector);
    for (int i = 0; i < nargs; i++)
        cpu_set_gpr(3 + i, args[i]);
    cpu_set_lr(GUEST_RETURN_MAGIC);

    T.depth++;
    uint32_t pc = code;
    for (;;) {
        cpu_stop s = cpu_run(pc);
        if (s.kind == CPU_STOP_RETURN)
            break;
        if (s.kind == CPU_STOP_FAULT) {
            char a[32];
            trap_crash("guest fault: %s (pc %s)", s.detail, fmt_addr(s.pc, a));
        }
        uint32_t index = (s.addr - GUEST_TRAP_BASE) / 4;
        if (index >= T.n)
            trap_crash("jump to unused trap address 0x%08x", s.addr);
        const hist_entry *h = record(index);
        if (!T.handlers[index])
            trap_crash("unimplemented import %s", T.names[index]);
        uint32_t resume = cpu_lr();
        if (T.trace_imports) {
            char a[32];
            fprintf(stderr, "loony: trace: #%u %s(0x%08x, 0x%08x, 0x%08x, 0x%08x) from %s\n",
                    T.hist_count - 1, T.names[index], h->a[0], h->a[1], h->a[2], h->a[3],
                    fmt_addr(h->lr, a));
        }
        T.handlers[index]();
        if (T.trace_imports)
            fprintf(stderr, "loony: trace:   -> 0x%08x\n", cpu_gpr(3));
        pc = resume;
    }
    T.depth--;

    uint32_t result = cpu_gpr(3);
    cpu_restore(saved);
    return result;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ./build/loony_tests trap_`
Expected: `6 passed, 0 failed, 0 skipped`

- [ ] **Step 5: Run the whole suite**

Run: `./build/loony_tests`
Expected: `0 failed`. There should be no skips if the game folder is present.

- [ ] **Step 6: Commit**

```bash
git add src/trap.h src/trap.c tests/test_trap.c
git commit -m "Import dispatch, re-entrant guest_call and crash reports"
```

---

### Task 9: Run the real game to its first unimplemented import

**Files:**
- Modify: `src/main.c` (replace the stub from Task 1)
- Create: `tests/test_run.c`
- Create: `README.md`

**Interfaces:**
- Consumes: everything above.
- Produces: the `loony [game-folder]` executable. It exits 1 on bad input, 2 on a crash, and 0 if `main` returns. It logs `loaded <path>: <n> imports, main at <addr>` before running.

- [ ] **Step 1: Write the failing test**

`tests/test_run.c`:
```c
#include "test.h"

#include <stdlib.h>
#include <unistd.h>

static void run_loony(void *dir) {
    execl(LOONY_BIN, "loony", (const char *)dir, (char *)NULL);
    fprintf(stderr, "exec %s failed\n", LOONY_BIN);
    _exit(127);
}

TEST(run_stops_at_first_unimplemented_import) {
    SKIP_UNLESS_GAME();
    char out[32768];
    int status = test_run_child(run_loony, (void *)test_game_dir(), out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: loaded ");
    CHECK_CONTAINS(out, "132 imports");
    CHECK_CONTAINS(out, "loony: crash: unimplemented import ");
}

/* Review Focus 1: wrong or missing game folder. */
TEST(run_reports_missing_game_folder) {
    char out[4096];
    int status = test_run_child(run_loony, (void *)"/nonexistent/loony", out, sizeof out);
    CHECK_EQ(status, 1);
    CHECK_CONTAINS(out, "can't read /nonexistent/loony/LOONY LABYRINTH 3.0.1");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build && ./build/loony_tests run_`
Expected: both FAIL. The stub `main` exits 1 and prints "not implemented yet", so the first test's status check fails and the second test's message check fails.

- [ ] **Step 3: Write `src/main.c`**

```c
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "guest_mem.h"
#include "loader.h"
#include "trap.h"
#include "util.h"

#define DEFAULT_GAME_DIR "/Applications/Loony Labyrinth"
#define GAME_EXE_NAME "LOONY LABYRINTH 3.0.1"

int main(int argc, char **argv) {
    if (argc > 2) {
        fprintf(stderr, "usage: loony [game-folder]\n");
        return 1;
    }
    const char *dir = argc == 2 ? argv[1] : DEFAULT_GAME_DIR;

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, GAME_EXE_NAME);
    size_t len = 0;
    uint8_t *buf = read_file(path, &len);
    if (!buf) {
        fprintf(stderr, "loony: can't read %s: %s\n", path, strerror(errno));
        return 1;
    }

    gm_init();
    cpu_init();
    loaded_image img;
    char err[256];
    if (!image_load(buf, len, &img, err, sizeof err)) {
        fprintf(stderr, "loony: can't load %s: %s\n", path, err);
        return 1;
    }

    const char **names = calloc(img.pef.nimports ? img.pef.nimports : 1, sizeof *names);
    if (!names)
        fatal("out of memory");
    for (uint32_t i = 0; i < img.pef.nimports; i++)
        names[i] = img.pef.imports[i].name;
    trap_init(img.pef.nimports, names, img.code_base, img.code_len);

    if (!img.main_tvector)
        fatal("%s has no main entry point", path);
    log_msg("loaded %s: %u imports, main at code+0x%05x", path, img.pef.nimports,
            gm_r32(img.main_tvector) - img.code_base);

    if (img.init_tvector)
        guest_call(img.init_tvector, 0, NULL);
    guest_call(img.main_tvector, 0, NULL);
    log_msg("main returned");
    return 0;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ./build/loony_tests run_`
Expected: `2 passed, 0 failed, 0 skipped`

- [ ] **Step 5: Run the game by hand and record the first import**

Run: `LOONY_TRACE=imports ./build/loony; echo "exit status $?"`
Expected: `loony: loaded /Applications/Loony Labyrinth/LOONY LABYRINTH 3.0.1: 132 imports, main at code+0x387e0`, then a crash report beginning `loony: crash: unimplemented import <Name>`, and `exit status 2`. With no handlers registered yet, the first import the game calls is the one reported. Record that name and the report's `from code+0x.....` address in the commit message below. Plan 2 starts there.

- [ ] **Step 6: Write the README**

`README.md`:
````markdown
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
````

- [ ] **Step 7: Run the whole suite, then commit**

Run: `./build/loony_tests`
Expected: `0 failed`.

```bash
git add src/main.c tests/test_run.c README.md
git commit -m "Run the game's main until the first unimplemented import

First import reached: <Name> from code+0x<addr> (from Step 5)."
```

---

## What comes next (not part of this plan)

Plan 2 (milestone 2, core services) starts from the import recorded in Task 9 Step 5. It will add a `*_register()` function per module and implement imports in the order the trace reaches them: memory, resources, Gestalt and time calls. It will also add three things the spec lists but this plan defers because nothing needs them yet: filling in the `kCFPreferencesCurrentApplication` data slot, logging writes to low memory, and `LOONY_TRACE=calls` (logging each `guest_call` entry and exit, which matters once the game's callbacks run). SDL3 is installed in Plan 3, when the display arrives.
