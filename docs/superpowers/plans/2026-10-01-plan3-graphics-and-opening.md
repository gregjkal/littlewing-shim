# Plan 3: Graphics and the Opening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show the game. The emulated screen, GWorlds, CopyBits, PICT decoding and the SDL window let the real game play its opening (the LittleWing logo, then the title) and run its attract mode on the table. There is no input or audio yet. The plan also adds the pieces the game needs on the way: answering the two startup alerts, reading its data file, a silent Sound Manager, and event loop timers.

**Architecture:** `blit.c` is a host-side pixel engine: formats, palettes, copy with scaling, and fill. `pict.c` decodes PICTs onto it. `qd.c` keeps the QuickDraw structures the game can see in guest memory (PixMaps in handles, CGrafPorts, the main GDevice, color tables), and turns `CopyBits`, `PaintRect` and `DrawPicture` into `blit.c` calls. `display.c` converts the emulated screen to RGBA for an SDL window and for PNG dumps. The game runs its frame loop inside an event-loop timer callback and waits with `TickCount`/`Delay`. A shared pump (`events_pump`) therefore runs from the run loop, from `Delay`, and from `TickCount` whenever the tick changes. It drives sound callbacks, timers, presenting the screen and the `LOONY_EXIT_AFTER` check.

**Tech Stack:** C (gnu11), clang, CMake ≥ 3.20, Unicorn 2, SDL3 (Homebrew `sdl3`, new in this plan).

**Spec:** `docs/superpowers/specs/2026-09-30-loony-shim-design.md` (milestone 3, plus the parts of milestones 4–6 the opening needs). Plan 2 (`docs/superpowers/plans/2026-10-01-plan2-core-services.md`) built the memory, resource, misc and CF modules.

## Global Constraints

- Repo: `~/dev/loony-shim`, branch `main`. All paths below are relative to it.
- Game files in `/Applications/Loony Labyrinth` are read-only inputs. Never write, move or modify them. Never copy them or anything extracted from them into the repo.
- C11 with GNU extensions (`gnu11`), clang, `-Wall -Wextra -Werror` in every build. Debug builds (the default) add `-fsanitize=address,undefined`. Release is `-O2`.
- Dependencies come only from Homebrew: `unicorn`, `sdl3`, `cmake`, `pkg-config`.
- Unimplemented import, guest crash, or an unsupported option in an implemented call: stop with the full crash report and exit **2**. Exceptions: unknown `Gestalt` selectors and sound command problems are logged, not fatal.
- Bad command-line input or unreadable/unloadable game files exit **1**.
- All guest code runs on the main thread. The SDL audio thread (milestone 5) will never run game code.
- Guest addresses inside the code section are printed as `code+0xNNNNN`.
- Tests that need the game files are **skipped**, not failed, if the executable isn't there. No test opens a real window: the test runner sets `SDL_VIDEO_DRIVER=dummy`.
- Everything the game might inspect directly lives in guest memory in the original big-endian layout: PixMaps, GWorld/CGrafPort structs, the GDevice, color tables, Rects, FSSpecs, sound headers. State it reaches only through calls stays in host C structs: the SDL window, sound channel queues, event handlers, timers.
- **CopyBits:** `srcCopy` at 1, 2, 4, 8, 16 and 32 bits per pixel, same depth or converting. Nearest-neighbor scaling when the rectangles differ, clipped to the destination port's clip rectangle when the destination is the current port. Any other transfer mode or a mask region fails loudly.
- **DrawPicture:** the PICT opcodes the game's pictures use. Anything else fails loudly.
- **SDL window:** resizable, with the emulated screen scaled to fit at the correct aspect ratio using nearest-neighbor sampling, and vsync.
- **Tag space:** opaque host-object IDs start at `0x0800_0000`. CF uses `0x0800_0000`–`0x08FF_FFFF`. Events use `0x0A00_0000` and up.

## Facts measured from the real game (the tests assert these)

| Fact | Value |
|---|---|
| Startup sequence after the alerts | Event handler setup, `GetMainDevice`, `SetDepth(16)`, `BeginFullScreen(&restore, NULL, NULL, NULL, &window, NULL, 2)`, `GetCTable(8)`, GWorld setup, then 222 `DrawPicture` calls into 223 GWorlds |
| Screen size | `InitializeThisGame` creates the game's video output at 800×600×16, and `DoOpening` centers the opening image in 800×600. On a 640×480 screen the right and bottom 80/60 pixels are cut off. `BeginFullScreen` passes NULL sizes, so the screen must already be 800×600 |
| Pictures | `PICT` 800 is the 512×384 title. Most game art is PICT data in custom types (`ViDe`, `ViRt`, `PfDb`), which the game decrypts itself before `DrawPicture`. The opcodes used are: version, header, DefHilite, clip, `PackBitsRect` (4 and 8 bits, with color tables), version-1 `BitsRect`/`PackBitsRect` (1-bit bitmaps), long comments and end. `PICT` 128 and 129 use QuickTime (`0x8201`) and are not drawn at startup |
| Picture checksums | Drawn into an 8-bit standard-palette canvas, FNV-1a32 is `0x4657C203` for `PICT` 800, `0x958A1FF5` for `PICT` 804 (4-bit, 309×80) and `0xA2CE526E` for `PICT` 30000 (version 1, 230×11). An independent Python decoder computed these values |
| Files | `FSMakeFSSpec(0, 0, ":LL Data:effect.bin")`, then `FSpOpenDF` with read permission, `GetEOF`, `PBReadSync`, `GetFPos` |
| Sound | From the first timer callback: `NewSndCallBackUPP`, `SndNewChannel(&chan (NULL), sampledSynth, 0x84, callback)`, `SndDoCommand` |
| Event loop | `NewEventLoopTimerUPP`, `GetMainEventLoop`, `InstallEventLoopTimer` (intervals in f1/f2, proc/data/outTimer in r8–r10), `RunApplicationEventLoop`. The timer callback runs the game's own frame loop and calls `TickCount` about 1.1 million times and `Delay(0)` about 0.7 million times a second. `TSystem::PerformOSTask` calls `ReceiveNextEvent(0, NULL, timeout, true, &event)` about 25 seconds in |
| Opening timeline (ticks since launch, headless, Debug build) | Black until about tick 10, then the LittleWing logo (tick 30), the title (tick 240), then the table in attract mode with the DMD showing "LOONY LABYRINTH" and then a demo game |
| Symbols | The code has traceback tables with C++ names, for example `DoOpening__Fv` and `UpdateFrontBuffer__Q22RT18TVideoDoubleBufferFb`. Useful for symbolizing addresses in crash reports later |

## Decisions this plan makes

- **The screen starts at 800×600, 8 bits**, not the spec's 640×480. The game hard-codes an 800×600 video output (see Facts). `SetDepth(16)` then makes it 16-bit.
- **Windows are full-screen ports that share the screen's PixMap.** The only window the game creates is the `BeginFullScreen` one. Dialog windows arrive with milestone 6.
- **`Alert` and `StopAlert` answer their default item without drawing.** They log the alert's text, with `ParamText` substitutions. This gets past "Play Demo / Quit / Buy Now / Enter Key-Code" (default: Play Demo) and the demo's key list (default: OK). Drawn, interactive dialogs are milestone 6.
- **Only the read side of `files.c` is built now.** Writing (and the writable folder) is milestone 6; opening a file with write permission crashes until then.
- **The Sound Manager is silent but keeps time.** Buffers keep their channel busy for frames ÷ sample rate seconds, and callbacks fire when they would on a Mac. Audio output is milestone 5.
- **Event loop timers and `RunApplicationEventLoop` come now**, because the opening runs in a timer callback. Input events, `SendEventToEventTarget`, `GetEventParameter` and keyboard translation are milestone 4. `ReceiveNextEvent` pumps until its timeout and reports no event.
- **`Delay` and `TickCount` pump.** The game never returns from its timer callback while it plays, so presenting the screen, sound callbacks and the exit check run from the idle hook. That hook runs at most once per tick, and `Delay` sleeps in quarter-tick steps. A timer callback that's still running isn't re-entered.
- **`UpdateGWorld` supports only no-op updates** (same size and depth). Changes crash. The game only ever passes the same size.
- **`LOONY_EXIT_AFTER=<ticks>` and `LOONY_SCREENSHOT=<file.png>`** give deterministic-enough headless runs for tests. A fixed tick clock and scripted input (the spec's headless integration test) come with milestone 4.

## Review Focus

1. **A picture with an opcode or pixel format we don't decode.** Expect a crash report naming the opcode, not garbage on screen. Tests in Task 3.
2. **A truncated or corrupt picture.** Expect an error with no out-of-bounds read under ASan. Tests in Task 3.
3. **CopyBits rectangles partly or entirely outside the destination.** Expect clipping, not a write outside the pixel buffer. Test in Task 2.
4. **The game waits in its own loop (`TickCount`/`Delay`) instead of returning to the event loop.** Expect the screen to keep presenting and `LOONY_EXIT_AFTER` to still end the run. Tests in Tasks 8 and 10.
5. **A file the game asks for doesn't exist, or a folder in its path doesn't.** Expect `fnfErr`/`dirNFErr` (with the FSSpec still filled in), not a crash. Test in Task 7.

---

### Task 1: PNG writer

**Files:**
- Create: `src/png.h`, `src/png.c`
- Create: `tests/test_png.c`

**Interfaces:**
- Consumes: `wr_be32`, `read_file`, `rd_be32` (`util.h`).
- Produces: `bool png_write_rgba(const char *path, const uint8_t *rgba, int width, int height)`.

The PNG uses stored (uncompressed) deflate blocks, so no zlib is needed. An 800×600 dump is about 1.9 MB, which is fine for debugging and tests.

- [ ] **Step 1: Write the failing test**

`tests/test_png.c`:
```c
#include "test.h"

#include <stdlib.h>
#include <unistd.h>

#include "png.h"
#include "util.h"

static char *tmp_path(char buf[1024]) {
    const char *t = getenv("TMPDIR");
    snprintf(buf, 1024, "%s/loony-png-XXXXXX", t && *t ? t : "/tmp");
    int fd = mkstemp(buf);
    if (fd >= 0)
        close(fd);
    return buf;
}

TEST(png_writes_a_valid_header_and_size) {
    uint8_t px[2 * 3 * 4];
    for (int i = 0; i < 24; i++)
        px[i] = (uint8_t)(i * 10);
    char path[1024];
    tmp_path(path);
    CHECK(png_write_rgba(path, px, 2, 3));
    size_t len;
    uint8_t *f = read_file(path, &len);
    unlink(path);
    CHECK(f != NULL);
    CHECK(memcmp(f, "\x89PNG\r\n\x1a\n", 8) == 0);
    CHECK(memcmp(f + 12, "IHDR", 4) == 0);
    CHECK_EQ(rd_be32(f + 16), 2);
    CHECK_EQ(rd_be32(f + 20), 3);
    /* signature 8 + IHDR 25 + IDAT (12 + 2 + 5 + 27 + 4) + IEND 12 */
    CHECK_EQ(len, 8 + 25 + 50 + 12);
    CHECK(memcmp(f + len - 8, "IEND", 4) == 0);
    /* the IEND CRC is fixed */
    CHECK_EQ(rd_be32(f + len - 4), 0xAE426082u);
    free(f);
}

TEST(png_splits_large_images_into_stored_blocks) {
    int w = 200, h = 100; /* 80,100 raw bytes: two stored blocks */
    uint8_t *px = calloc((size_t)w * h, 4);
    char path[1024];
    tmp_path(path);
    CHECK(png_write_rgba(path, px, w, h));
    size_t len;
    uint8_t *f = read_file(path, &len);
    unlink(path);
    free(px);
    CHECK(f != NULL);
    size_t raw = (size_t)(w * 4 + 1) * h;
    CHECK_EQ(len, 8 + 25 + 12 + (2 + raw + 5 * 2 + 4) + 12);
    free(f);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: the build fails with `'png.h' file not found`.

- [ ] **Step 3: Write the implementation**

`src/png.h`:
```c
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Writes an 8-bit RGBA image (rows top to bottom, 4 bytes per pixel) as an
   uncompressed PNG. Returns false if the file can't be written. */
bool png_write_rgba(const char *path, const uint8_t *rgba, int width, int height);
```

`src/png.c`:
```c
#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/* Uses stored (uncompressed) deflate blocks, so no zlib is needed. */

static uint32_t crc_table[256];

static void init_crc(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    c = ~c;
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

static bool chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    wr_be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    uint32_t c = crc(0, hdr + 4, 4);
    c = crc(c, data, len);
    uint8_t tail[4];
    wr_be32(tail, c);
    return fwrite(hdr, 1, 8, f) == 8 && fwrite(data, 1, len, f) == len &&
           fwrite(tail, 1, 4, f) == 4;
}

bool png_write_rgba(const char *path, const uint8_t *rgba, int width, int height) {
    if (!crc_table[1])
        init_crc();
    size_t row = (size_t)width * 4 + 1; /* filter byte + pixels */
    size_t raw_len = row * (size_t)height;
    size_t nblocks = raw_len / 65535 + 1;
    size_t z_len = 2 + raw_len + 5 * nblocks + 4;
    uint8_t *z = malloc(z_len);
    if (!z)
        return false;
    size_t o = 0;
    z[o++] = 0x78; /* zlib header: deflate, 32K window */
    z[o++] = 0x01;
    uint32_t a = 1, b = 0; /* Adler-32 */
    size_t done = 0, block_left = 0;
    for (int y = 0; y < height; y++) {
        for (size_t i = 0; i < row; i++) {
            if (block_left == 0) {
                size_t n = raw_len - done < 65535 ? raw_len - done : 65535;
                z[o++] = (uint8_t)(done + n == raw_len); /* BFINAL, stored */
                z[o++] = (uint8_t)n;
                z[o++] = (uint8_t)(n >> 8);
                z[o++] = (uint8_t)~n;
                z[o++] = (uint8_t)(~n >> 8);
                block_left = n;
            }
            uint8_t v = i == 0 ? 0 : rgba[(size_t)y * (size_t)width * 4 + i - 1];
            z[o++] = v;
            a = (a + v) % 65521;
            b = (b + a) % 65521;
            done++;
            block_left--;
        }
    }
    if (raw_len == 0) { /* an empty image still needs one final block */
        z[o++] = 1;
        z[o++] = 0;
        z[o++] = 0;
        z[o++] = 0xFF;
        z[o++] = 0xFF;
    }
    wr_be32(z + o, (b << 16) | a);
    o += 4;

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(z);
        return false;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    uint8_t ihdr[13];
    wr_be32(ihdr, (uint32_t)width);
    wr_be32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 6;  /* RGBA */
    ihdr[10] = 0; /* deflate */
    ihdr[11] = 0; /* adaptive filtering */
    ihdr[12] = 0; /* no interlace */
    bool ok = fwrite(sig, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) &&
              chunk(f, "IDAT", z, (uint32_t)o) && chunk(f, "IEND", NULL, 0);
    ok = (fclose(f) == 0) && ok;
    free(z);
    return ok;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S . -B build && cmake --build build && ./build/loony_tests png_`
Expected: `2 passed, 0 failed, 0 skipped`. Full suite: `130 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/png.h src/png.c tests/test_png.c
git commit -m "Uncompressed PNG writer for screen dumps"
```

---

### Task 2: Pixel engine

**Files:**
- Create: `src/blit.h`, `src/blit.c`
- Create: `tests/test_blit.c`

**Interfaces:**
- Consumes: `rd_be16/32`, `wr_be16/32` (`util.h`).
- Produces (all of `src/blit.h`): `qd_rect`, `qd_rgb`, `qd_palette`, `qd_pixels`, `QD_SRC_COPY`, `rect_w`, `rect_h`, `rect_empty`, `qd_rect rect_sect(qd_rect, qd_rect)`, `void qd_std_palette(int depth, qd_palette *out)`, `uint32_t qd_pixel_for(qd_rgb c, int depth, const qd_palette *pal)`, `bool qd_blit(const qd_pixels *src, qd_rect src_rect, const qd_pixels *dst, qd_rect dst_rect, qd_rect clip, int mode, qd_rgb fg, qd_rgb bg, char *err, size_t errlen)`, `void qd_fill(const qd_pixels *dst, qd_rect r, qd_rect clip, qd_rgb c)`, `void qd_to_rgba(const qd_pixels *src, uint8_t *rgba)`

Pixels are big-endian as on the Mac: 1/2/4/8-bit indexed with the most significant bits first, 16-bit `xRRRRRGGGGGBBBBB`, 32-bit `xRGB`. For an indexed source the engine builds a value-to-value map once per copy: identity when the palettes match, otherwise nearest color. The standard 8-bit palette is a 6×6×6 cube (white first, black omitted), then ten-step ramps of red, green, blue and gray, then black.

- [ ] **Step 1: Write the failing test**

`tests/test_blit.c`:
```c
#include "test.h"

#include "blit.h"
#include "util.h"

static const qd_rgb BLACK = {0, 0, 0}, WHITE = {0xFFFF, 0xFFFF, 0xFFFF};

TEST(blit_standard_8bit_palette) {
    qd_palette p;
    qd_std_palette(8, &p);
    CHECK_EQ(p.n, 256);
    CHECK_EQ(p.c[0].r, 0xFFFF); /* white first */
    CHECK_EQ(p.c[0].b, 0xFFFF);
    CHECK_EQ(p.c[255].r, 0);    /* black last */
    CHECK_EQ(p.c[5].b, 0x0000); /* 0xFFFF, 0xFFFF, 0x0000: yellow */
    CHECK_EQ(p.c[5].r, 0xFFFF);
    CHECK_EQ(p.c[215].r, 0xEEEE); /* the red ramp starts after the cube */
    CHECK_EQ(p.c[215].g, 0);
    CHECK_EQ(p.c[245].r, 0xEEEE); /* the gray ramp */
    CHECK_EQ(p.c[245].g, 0xEEEE);
    qd_std_palette(1, &p);
    CHECK_EQ(p.n, 2);
    CHECK_EQ(p.c[1].r, 0);
}

TEST(blit_pixel_values_for_colors) {
    qd_palette p;
    qd_std_palette(8, &p);
    CHECK_EQ(qd_pixel_for(WHITE, 8, &p), 0);
    CHECK_EQ(qd_pixel_for(BLACK, 8, &p), 255);
    CHECK_EQ(qd_pixel_for((qd_rgb){0xFFFF, 0, 0}, 16, NULL), 0x7C00);
    CHECK_EQ(qd_pixel_for((qd_rgb){0, 0xFFFF, 0}, 16, NULL), 0x03E0);
    CHECK_EQ(qd_pixel_for((qd_rgb){0x1234, 0xABCD, 0xFF00}, 32, NULL), 0x12ABFF);
}

static qd_pixels px(uint8_t *buf, int w, int h, int depth, const qd_palette *pal) {
    return (qd_pixels){buf, (uint32_t)((w * depth + 31) / 32 * 4), {0, 0, (int16_t)h, (int16_t)w},
                       depth, pal};
}

TEST(blit_8bit_to_16bit_converts_through_the_palette) {
    qd_palette p;
    qd_std_palette(8, &p);
    uint8_t s[4 * 2] = {0, 255, 5, 0, 0, 0, 0, 0};
    uint8_t d[8 * 2] = {0};
    qd_pixels src = px(s, 4, 2, 8, &p), dst = px(d, 4, 2, 16, NULL);
    qd_rect r = {0, 0, 1, 3};
    char err[128];
    CHECK(qd_blit(&src, r, &dst, r, dst.bounds, QD_SRC_COPY, BLACK, WHITE, err, sizeof err));
    CHECK_EQ(rd_be16(d), 0x7FFF);     /* white */
    CHECK_EQ(rd_be16(d + 2), 0x0000); /* black */
    CHECK_EQ(rd_be16(d + 4), 0x7FE0); /* yellow */
    CHECK_EQ(rd_be16(d + 6), 0);      /* outside the rect: untouched */
}

TEST(blit_same_palette_copies_indexes) {
    qd_palette p;
    qd_std_palette(8, &p);
    uint8_t s[4] = {17, 42, 99, 200}, d[4] = {0};
    qd_pixels src = px(s, 4, 1, 8, &p), dst = px(d, 4, 1, 8, &p);
    char err[128];
    CHECK(qd_blit(&src, src.bounds, &dst, dst.bounds, dst.bounds, QD_SRC_COPY, BLACK, WHITE, err,
                  sizeof err));
    CHECK(memcmp(s, d, 4) == 0);
}

TEST(blit_scales_with_nearest_neighbor) {
    uint8_t s[4] = {1, 2, 0, 0}; /* 2x1, padded rows */
    uint8_t d[8] = {0};
    qd_palette p;
    qd_std_palette(8, &p);
    qd_pixels src = px(s, 2, 1, 8, &p), dst = px(d, 4, 2, 8, &p);
    char err[128];
    CHECK(qd_blit(&src, (qd_rect){0, 0, 1, 2}, &dst, (qd_rect){0, 0, 2, 4}, dst.bounds, QD_SRC_COPY,
                  BLACK, WHITE, err, sizeof err));
    uint8_t want[8] = {1, 1, 2, 2, 1, 1, 2, 2};
    CHECK(memcmp(d, want, 8) == 0);
}

TEST(blit_respects_clip_and_destination_bounds) {
    qd_palette p;
    qd_std_palette(8, &p);
    uint8_t s[8] = {9, 9, 9, 9, 9, 9, 9, 9}, d[8] = {0};
    qd_pixels src = px(s, 8, 1, 8, &p), dst = px(d, 8, 1, 8, &p);
    char err[128];
    /* Destination rect hangs off the left edge; the clip cuts the right. */
    CHECK(qd_blit(&src, (qd_rect){0, 0, 1, 8}, &dst, (qd_rect){0, -2, 1, 6}, (qd_rect){0, 0, 1, 5},
                  QD_SRC_COPY, BLACK, WHITE, err, sizeof err));
    uint8_t want[8] = {9, 9, 9, 9, 9, 0, 0, 0};
    CHECK(memcmp(d, want, 8) == 0);
}

TEST(blit_1bit_sources_use_foreground_and_background) {
    qd_palette p1, p8;
    qd_std_palette(1, &p1);
    qd_std_palette(8, &p8);
    uint8_t s[4] = {0xA0, 0, 0, 0}; /* 1 0 1 0 */
    uint8_t d[4] = {7, 7, 7, 7};
    qd_pixels src = px(s, 4, 1, 1, &p1), dst = px(d, 4, 1, 8, &p8);
    char err[128];
    CHECK(qd_blit(&src, src.bounds, &dst, dst.bounds, dst.bounds, QD_SRC_COPY, BLACK, WHITE, err,
                  sizeof err));
    uint8_t want[4] = {255, 0, 255, 0};
    CHECK(memcmp(d, want, 4) == 0);
}

TEST(blit_4bit_pixels_pack_two_per_byte) {
    qd_palette p4, p8;
    qd_std_palette(4, &p4);
    qd_std_palette(8, &p8);
    uint8_t s[4] = {0xF0, 0, 0, 0}; /* black, white */
    uint8_t d[4] = {0};
    qd_pixels src = px(s, 2, 1, 4, &p4), dst = px(d, 2, 1, 8, &p8);
    char err[128];
    CHECK(qd_blit(&src, src.bounds, &dst, dst.bounds, dst.bounds, QD_SRC_COPY, BLACK, WHITE, err,
                  sizeof err));
    CHECK_EQ(d[0], 255);
    CHECK_EQ(d[1], 0);
}

TEST(blit_rejects_unsupported_modes_and_conversions) {
    qd_palette p;
    qd_std_palette(8, &p);
    uint8_t s[8] = {0}, d[8] = {0};
    qd_pixels s8 = px(s, 2, 1, 8, &p), d8 = px(d, 2, 1, 8, &p), s16 = px(s, 2, 1, 16, NULL);
    char err[128] = "";
    CHECK(!qd_blit(&s8, s8.bounds, &d8, d8.bounds, d8.bounds, 36, BLACK, WHITE, err, sizeof err));
    CHECK_CONTAINS(err, "transfer mode 36");
    CHECK(!qd_blit(&s16, s16.bounds, &d8, d8.bounds, d8.bounds, QD_SRC_COPY, BLACK, WHITE, err,
                   sizeof err));
    CHECK_CONTAINS(err, "16-bit pixels to 8 bits");
}

TEST(blit_fill_and_rgba_conversion) {
    uint8_t d[4 * 2] = {0};
    qd_pixels dst = px(d, 2, 2, 16, NULL);
    qd_fill(&dst, (qd_rect){0, 1, 2, 2}, dst.bounds, (qd_rgb){0xFFFF, 0, 0});
    uint8_t rgba[16];
    qd_to_rgba(&dst, rgba);
    CHECK_EQ(rgba[0], 0);
    CHECK_EQ(rgba[3], 255);
    CHECK_EQ(rgba[4], 255); /* (1,0) is red */
    CHECK_EQ(rgba[5], 0);
    CHECK_EQ(rgba[12], 255); /* (1,1) is red */
}

TEST(blit_rect_intersection) {
    qd_rect a = {0, 0, 10, 10}, b = {5, 5, 20, 20}, c = {11, 11, 12, 12};
    qd_rect r = rect_sect(a, b);
    CHECK_EQ(r.top, 5);
    CHECK_EQ(r.right, 10);
    CHECK(rect_empty(rect_sect(a, c)));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: the build fails with `'blit.h' file not found`.

- [ ] **Step 3: Write the implementation**

`src/blit.h`:
```c
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pixel rectangles and the copy/fill engine under CopyBits, PaintRect and
   DrawPicture. Everything here works on host pointers; qd.c builds the
   descriptors from guest PixMaps. Pixels are big-endian, as on the Mac:
   1/2/4/8-bit indexed (most significant bits first), 16-bit xRRRRRGGGGGBBBBB
   and 32-bit xRGB. */

typedef struct {
    int16_t top, left, bottom, right;
} qd_rect;

typedef struct {
    uint16_t r, g, b;
} qd_rgb;

/* A color table: entry i is the color of pixel value i. */
typedef struct {
    int n;
    qd_rgb c[256];
} qd_palette;

typedef struct {
    uint8_t *base;    /* the first byte of the row at bounds.top */
    uint32_t row_bytes;
    qd_rect bounds;   /* pixel (bounds.left, bounds.top) is the first pixel */
    int depth;        /* 1, 2, 4, 8, 16 or 32 */
    const qd_palette *pal; /* indexed depths only */
} qd_pixels;

#define QD_SRC_COPY 0

static inline int rect_w(qd_rect r) { return r.right - r.left; }
static inline int rect_h(qd_rect r) { return r.bottom - r.top; }
static inline bool rect_empty(qd_rect r) { return r.right <= r.left || r.bottom <= r.top; }
qd_rect rect_sect(qd_rect a, qd_rect b);

/* The standard Mac color table for 1, 2, 4 or 8 bits (clut IDs 1, 2, 4, 8). */
void qd_std_palette(int depth, qd_palette *out);

/* The pixel value for color c at the given depth (nearest palette entry for
   indexed depths). */
uint32_t qd_pixel_for(qd_rgb c, int depth, const qd_palette *pal);

/* Copies src_rect of src onto dst_rect of dst, scaling with nearest-neighbor
   sampling when the sizes differ. Only pixels inside clip and dst's bounds
   are written. 1-bit sources are colorized: 1 bits become fg, 0 bits bg.
   Writes a message and returns false for an unsupported mode or conversion. */
bool qd_blit(const qd_pixels *src, qd_rect src_rect, const qd_pixels *dst, qd_rect dst_rect,
             qd_rect clip, int mode, qd_rgb fg, qd_rgb bg, char *err, size_t errlen);

/* Fills r (clipped to clip and dst's bounds) with color c. */
void qd_fill(const qd_pixels *dst, qd_rect r, qd_rect clip, qd_rgb c);

/* Converts the whole of src to 8-bit RGBA rows. rgba holds width*height*4 bytes. */
void qd_to_rgba(const qd_pixels *src, uint8_t *rgba);
```

`src/blit.c`:
```c
#include "blit.h"

#include <stdarg.h>
#include <stdio.h>
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

qd_rect rect_sect(qd_rect a, qd_rect b) {
    qd_rect r = {
        a.top > b.top ? a.top : b.top,
        a.left > b.left ? a.left : b.left,
        a.bottom < b.bottom ? a.bottom : b.bottom,
        a.right < b.right ? a.right : b.right,
    };
    if (rect_empty(r))
        r = (qd_rect){0, 0, 0, 0};
    return r;
}

void qd_std_palette(int depth, qd_palette *out) {
    memset(out, 0, sizeof *out);
    if (depth == 1) {
        out->n = 2;
        out->c[0] = (qd_rgb){0xFFFF, 0xFFFF, 0xFFFF};
        out->c[1] = (qd_rgb){0, 0, 0};
        return;
    }
    if (depth == 2) {
        static const qd_rgb c2[4] = {
            {0xFFFF, 0xFFFF, 0xFFFF}, {0xACAC, 0xACAC, 0xACAC}, {0x5555, 0x5555, 0x5555}, {0, 0, 0}};
        out->n = 4;
        memcpy(out->c, c2, sizeof c2);
        return;
    }
    if (depth == 4) {
        static const qd_rgb c4[16] = {
            {0xFFFF, 0xFFFF, 0xFFFF}, {0xFC00, 0xF37D, 0x052F}, {0xFFFF, 0x648A, 0x028C},
            {0xDD6B, 0x08C2, 0x06A2}, {0xF2D7, 0x0856, 0x84EC}, {0x46E3, 0x0000, 0xA53E},
            {0x0000, 0x0000, 0xD400}, {0x0241, 0xAB54, 0xEAFF}, {0x1F21, 0xB793, 0x1431},
            {0x0000, 0x64AF, 0x11B0}, {0x5600, 0x2C9D, 0x0524}, {0x90D7, 0x7160, 0x3A34},
            {0xC000, 0xC000, 0xC000}, {0x8000, 0x8000, 0x8000}, {0x4000, 0x4000, 0x4000},
            {0, 0, 0}};
        out->n = 16;
        memcpy(out->c, c4, sizeof c4);
        return;
    }
    /* 8 bits: a 6x6x6 color cube (white first, black omitted), then ten
       shades each of red, green, blue and gray, then black. */
    static const uint16_t cube[6] = {0xFFFF, 0xCCCC, 0x9999, 0x6666, 0x3333, 0x0000};
    static const uint16_t ramp[10] = {0xEEEE, 0xDDDD, 0xBBBB, 0xAAAA, 0x8888,
                                      0x7777, 0x5555, 0x4444, 0x2222, 0x1111};
    int i = 0;
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++)
                if (i < 215)
                    out->c[i++] = (qd_rgb){cube[r], cube[g], cube[b]};
    for (int k = 0; k < 10; k++)
        out->c[i++] = (qd_rgb){ramp[k], 0, 0};
    for (int k = 0; k < 10; k++)
        out->c[i++] = (qd_rgb){0, ramp[k], 0};
    for (int k = 0; k < 10; k++)
        out->c[i++] = (qd_rgb){0, 0, ramp[k]};
    for (int k = 0; k < 10; k++)
        out->c[i++] = (qd_rgb){ramp[k], ramp[k], ramp[k]};
    out->c[i++] = (qd_rgb){0, 0, 0};
    out->n = i;
}

static int nearest(qd_rgb c, const qd_palette *pal) {
    int best = 0;
    uint32_t best_d = UINT32_MAX;
    for (int i = 0; i < pal->n; i++) {
        int dr = (c.r >> 8) - (pal->c[i].r >> 8);
        int dg = (c.g >> 8) - (pal->c[i].g >> 8);
        int db = (c.b >> 8) - (pal->c[i].b >> 8);
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

uint32_t qd_pixel_for(qd_rgb c, int depth, const qd_palette *pal) {
    if (depth == 16)
        return ((uint32_t)(c.r >> 11) << 10) | ((uint32_t)(c.g >> 11) << 5) | (c.b >> 11);
    if (depth == 32)
        return ((uint32_t)(c.r >> 8) << 16) | ((uint32_t)(c.g >> 8) << 8) | (c.b >> 8);
    return (uint32_t)nearest(c, pal);
}

static qd_rgb rgb_of(uint32_t v, int depth, const qd_palette *pal) {
    if (depth == 16) {
        uint16_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        return (qd_rgb){(uint16_t)(r << 11 | r << 6 | r << 1 | r >> 4),
                        (uint16_t)(g << 11 | g << 6 | g << 1 | g >> 4),
                        (uint16_t)(b << 11 | b << 6 | b << 1 | b >> 4)};
    }
    if (depth == 32) {
        uint16_t r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
        return (qd_rgb){(uint16_t)(r << 8 | r), (uint16_t)(g << 8 | g), (uint16_t)(b << 8 | b)};
    }
    return v < (uint32_t)pal->n ? pal->c[v] : (qd_rgb){0, 0, 0};
}

static uint32_t get_px(const qd_pixels *p, int x, int y) {
    const uint8_t *row = p->base + (size_t)(y - p->bounds.top) * p->row_bytes;
    int i = x - p->bounds.left;
    switch (p->depth) {
    case 1: return (row[i >> 3] >> (7 - (i & 7))) & 1;
    case 2: return (row[i >> 2] >> (6 - 2 * (i & 3))) & 3;
    case 4: return (row[i >> 1] >> (4 - 4 * (i & 1))) & 15;
    case 8: return row[i];
    case 16: return rd_be16(row + 2 * i);
    default: return rd_be32(row + 4 * i) & 0xFFFFFF;
    }
}

static void put_px(const qd_pixels *p, int x, int y, uint32_t v) {
    uint8_t *row = p->base + (size_t)(y - p->bounds.top) * p->row_bytes;
    int i = x - p->bounds.left;
    switch (p->depth) {
    case 1: {
        int s = 7 - (i & 7);
        row[i >> 3] = (uint8_t)((row[i >> 3] & ~(1 << s)) | ((v & 1) << s));
        break;
    }
    case 2: {
        int s = 6 - 2 * (i & 3);
        row[i >> 2] = (uint8_t)((row[i >> 2] & ~(3 << s)) | ((v & 3) << s));
        break;
    }
    case 4: {
        int s = 4 - 4 * (i & 1);
        row[i >> 1] = (uint8_t)((row[i >> 1] & ~(15 << s)) | ((v & 15) << s));
        break;
    }
    case 8: row[i] = (uint8_t)v; break;
    case 16: wr_be16(row + 2 * i, (uint16_t)v); break;
    default: wr_be32(row + 4 * i, v & 0xFFFFFF); break;
    }
}

static bool indexed(int depth) { return depth <= 8; }

static bool same_palette(const qd_palette *a, const qd_palette *b) {
    return a->n == b->n && memcmp(a->c, b->c, sizeof a->c[0] * (size_t)a->n) == 0;
}

bool qd_blit(const qd_pixels *src, qd_rect sr, const qd_pixels *dst, qd_rect dr, qd_rect clip,
             int mode, qd_rgb fg, qd_rgb bg, char *err, size_t errlen) {
    if (mode != QD_SRC_COPY)
        return fail(err, errlen, "transfer mode %d is not supported", mode);
    if (rect_empty(sr) || rect_empty(dr))
        return true;
    if (indexed(dst->depth) && !indexed(src->depth))
        return fail(err, errlen, "copying %d-bit pixels to %d bits is not supported", src->depth,
                    dst->depth);
    /* Translate source pixel values into destination pixel values once. */
    static uint32_t map[256];
    bool use_map = indexed(src->depth);
    if (use_map) {
        int n = 1 << src->depth;
        bool identity = src->depth != 1 && dst->depth == src->depth && src->pal && dst->pal &&
                        same_palette(src->pal, dst->pal);
        for (int v = 0; v < n; v++) {
            if (identity) {
                map[v] = (uint32_t)v;
                continue;
            }
            qd_rgb c = src->depth == 1 ? (v ? fg : bg) : rgb_of((uint32_t)v, src->depth, src->pal);
            map[v] = qd_pixel_for(c, dst->depth, dst->pal);
        }
    }
    qd_rect area = rect_sect(rect_sect(dr, clip), dst->bounds);
    int sw = rect_w(sr), sh = rect_h(sr), dw = rect_w(dr), dh = rect_h(dr);
    for (int y = area.top; y < area.bottom; y++) {
        int sy = sr.top + (int)((int64_t)(y - dr.top) * sh / dh);
        if (sy < src->bounds.top || sy >= src->bounds.bottom)
            continue;
        for (int x = area.left; x < area.right; x++) {
            int sx = sr.left + (int)((int64_t)(x - dr.left) * sw / dw);
            if (sx < src->bounds.left || sx >= src->bounds.right)
                continue;
            uint32_t v = get_px(src, sx, sy);
            if (use_map)
                v = map[v];
            else if (dst->depth != src->depth)
                v = qd_pixel_for(rgb_of(v, src->depth, NULL), dst->depth, NULL);
            put_px(dst, x, y, v);
        }
    }
    return true;
}

void qd_fill(const qd_pixels *dst, qd_rect r, qd_rect clip, qd_rgb c) {
    uint32_t v = qd_pixel_for(c, dst->depth, dst->pal);
    qd_rect area = rect_sect(rect_sect(r, clip), dst->bounds);
    for (int y = area.top; y < area.bottom; y++)
        for (int x = area.left; x < area.right; x++)
            put_px(dst, x, y, v);
}

void qd_to_rgba(const qd_pixels *src, uint8_t *rgba) {
    for (int y = src->bounds.top; y < src->bounds.bottom; y++) {
        for (int x = src->bounds.left; x < src->bounds.right; x++) {
            qd_rgb c = rgb_of(get_px(src, x, y), src->depth, src->pal);
            *rgba++ = (uint8_t)(c.r >> 8);
            *rgba++ = (uint8_t)(c.g >> 8);
            *rgba++ = (uint8_t)(c.b >> 8);
            *rgba++ = 0xFF;
        }
    }
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S . -B build && cmake --build build && ./build/loony_tests blit_`
Expected: `11 passed, 0 failed, 0 skipped`. Full suite: `141 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/blit.h src/blit.c tests/test_blit.c
git commit -m "Pixel engine: formats, palettes, scaled copy and fill"
```

---

### Task 3: PICT decoder

**Files:**
- Create: `src/pict.h`, `src/pict.c`
- Create: `tests/test_pict.c`

**Interfaces:**
- Consumes: `qd_blit`, `qd_std_palette`, `qd_rect`, `qd_pixels` (Task 2); `rsrc_open`, `rsrc_find`, `rsrc_data` (Plan 2) in tests.
- Produces: `bool pict_draw(const uint8_t *data, size_t len, qd_rect dst, const qd_pixels *target, qd_rect clip, qd_rgb fg, qd_rgb bg, char *err, size_t errlen)`, `bool pict_frame(const uint8_t *data, size_t len, qd_rect *frame)`

Reference: *Inside Macintosh: Imaging With QuickDraw*, appendix A. A picture is a 2-byte size, a frame Rect, then opcodes:
- **Version 1:** begins `0x11 0x01`, one-byte opcodes, no alignment.
- **Version 2:** begins `0x0011 0x02FF`, two-byte opcodes aligned to even offsets.
- **Pixel rows:** each `PackBitsRect` row starts with a byte count (two bytes if `rowBytes` > 250). Rows with `rowBytes` < 8 are stored unpacked.
- **Color tables:** with flag `0x8000` the table is indexed by position; otherwise each entry names its pixel value.

- [ ] **Step 1: Write the failing test**

`tests/test_pict.c`:
```c
#include "test.h"

#include <stdlib.h>

#include "pict.h"
#include "rsrc.h"
#include "util.h"

static uint8_t *fork_buf;
static size_t fork_len;

static rsrc_entry *pict(int16_t id) {
    if (!fork_buf) {
        char path[1100];
        snprintf(path, sizeof path, "%s/..namedfork/rsrc", test_game_exe_path());
        fork_buf = read_file(path, &fork_len);
    }
    char err[256];
    if (!fork_buf || !rsrc_open(fork_buf, fork_len, err, sizeof err))
        return NULL;
    return rsrc_find(FOURCC('P', 'I', 'C', 'T'), id);
}

typedef struct {
    uint8_t *buf;
    qd_palette pal;
    qd_pixels px;
} canvas;

static void canvas_init(canvas *c, int w, int h) {
    qd_std_palette(8, &c->pal);
    uint32_t rb = (uint32_t)((w + 3) & ~3);
    c->buf = calloc(rb * (uint32_t)h, 1);
    c->px = (qd_pixels){c->buf, rb, {0, 0, (int16_t)h, (int16_t)w}, 8, &c->pal};
}

static bool draw(rsrc_entry *e, canvas *c, char *err) {
    return pict_draw(rsrc_data(e), e->len, c->px.bounds, &c->px, c->px.bounds, (qd_rgb){0, 0, 0},
                     (qd_rgb){0xFFFF, 0xFFFF, 0xFFFF}, err, 256);
}

/* The expected hashes come from an independent Python decoder (PackBits,
   the picture's color table, nearest standard-palette color). */
TEST(pict_decodes_the_title_picture) {
    SKIP_UNLESS_GAME();
    rsrc_entry *e = pict(800);
    CHECK(e != NULL);
    qd_rect frame;
    CHECK(pict_frame(rsrc_data(e), e->len, &frame));
    CHECK_EQ(rect_w(frame), 512);
    CHECK_EQ(rect_h(frame), 384);
    canvas c;
    canvas_init(&c, 512, 384);
    char err[256] = "";
    bool ok = draw(e, &c, err);
    uint32_t h = fnv1a32(c.buf, 512 * 384);
    free(c.buf);
    CHECK(ok);
    CHECK_EQ(h, 0x4657C203u);
}

TEST(pict_decodes_4bit_and_version_1_pictures) {
    SKIP_UNLESS_GAME();
    char err[256] = "";
    canvas c;
    rsrc_entry *e = pict(804); /* 309x80, 4 bits */
    CHECK(e != NULL);
    canvas_init(&c, 309, 80);
    bool ok = draw(e, &c, err);
    uint32_t h = fnv1a32(c.buf, c.px.row_bytes * 80);
    free(c.buf);
    CHECK(ok);
    CHECK_EQ(h, 0x958A1FF5u);
    e = pict(30000); /* version 1, 230x11 */
    CHECK(e != NULL);
    canvas_init(&c, 230, 11);
    ok = draw(e, &c, err);
    h = fnv1a32(c.buf, c.px.row_bytes * 11);
    free(c.buf);
    CHECK(ok);
    CHECK_EQ(h, 0xA2CE526Eu);
}

TEST(pict_scales_to_the_destination_rect) {
    SKIP_UNLESS_GAME();
    rsrc_entry *e = pict(800);
    CHECK(e != NULL);
    canvas c;
    canvas_init(&c, 256, 192);
    char err[256] = "";
    bool ok = draw(e, &c, err);
    /* Every destination pixel comes from the picture, so no white gaps remain
       where the canvas started out as 0 (white) apart from white pixels in
       the image itself: check a pixel known to be dark sand in the title. */
    uint8_t mid = c.buf[96 * c.px.row_bytes + 128];
    free(c.buf);
    CHECK(ok);
    CHECK(mid != 0);
}

TEST(pict_reports_quicktime_pictures) {
    SKIP_UNLESS_GAME();
    rsrc_entry *e = pict(128);
    CHECK(e != NULL);
    canvas c;
    canvas_init(&c, 104, 128);
    char err[256] = "";
    bool ok = draw(e, &c, err);
    free(c.buf);
    CHECK(!ok);
    CHECK_CONTAINS(err, "picture opcode 0x8201 is not supported");
}

TEST(pict_rejects_truncated_pictures) {
    SKIP_UNLESS_GAME();
    rsrc_entry *e = pict(801);
    CHECK(e != NULL);
    size_t cuts[] = {0, 9, 12, 60, 600, 20000};
    for (size_t i = 0; i < sizeof cuts / sizeof cuts[0]; i++) {
        uint8_t *part = malloc(cuts[i] ? cuts[i] : 1);
        memcpy(part, rsrc_data(e), cuts[i]);
        canvas c;
        canvas_init(&c, 239, 231);
        char err[256] = "";
        bool ok = pict_draw(part, cuts[i], c.px.bounds, &c.px, c.px.bounds, (qd_rgb){0, 0, 0},
                            (qd_rgb){0xFFFF, 0xFFFF, 0xFFFF}, err, sizeof err);
        free(part);
        free(c.buf);
        CHECK(!ok);
        CHECK(err[0] != '\0');
    }
}

TEST(pict_version_1_bitmap_by_hand) {
    /* 4x2 frame; clip; BitsRect of a 1-bit 4x2 bitmap 1001/0110; end. */
    uint8_t pic[] = {
        0, 0, 0, 0, 0, 0, 0, 2, 0, 4,             /* size, frame 0,0,2,4 */
        0x11, 0x01,                               /* version 1 */
        0x01, 0, 10, 0, 0, 0, 0, 0, 2, 0, 4,      /* clip region */
        0x90, 0, 2, 0, 0, 0, 0, 0, 2, 0, 4,       /* BitsRect: rowBytes 2, bounds */
        0, 0, 0, 0, 0, 2, 0, 4,                   /* srcRect */
        0, 0, 0, 0, 0, 2, 0, 4,                   /* dstRect */
        0, 0,                                     /* srcCopy */
        0x90, 0x00, 0x60, 0x00,                   /* rows */
        0xFF,
    };
    canvas c;
    canvas_init(&c, 4, 2);
    char err[256] = "";
    bool ok = pict_draw(pic, sizeof pic, c.px.bounds, &c.px, c.px.bounds, (qd_rgb){0, 0, 0},
                        (qd_rgb){0xFFFF, 0xFFFF, 0xFFFF}, err, sizeof err);
    uint8_t want[8] = {255, 0, 0, 255, 0, 255, 255, 0};
    bool same = memcmp(c.buf, want, 4) == 0 && memcmp(c.buf + c.px.row_bytes, want + 4, 4) == 0;
    free(c.buf);
    CHECK(ok);
    CHECK(same);
}

TEST(pict_rejects_unknown_opcodes) {
    uint8_t pic[] = {0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0x11, 0x01, 0x22, 0, 0};
    canvas c;
    canvas_init(&c, 1, 1);
    char err[256] = "";
    bool ok = pict_draw(pic, sizeof pic, c.px.bounds, &c.px, c.px.bounds, (qd_rgb){0, 0, 0},
                        (qd_rgb){0xFFFF, 0xFFFF, 0xFFFF}, err, sizeof err);
    free(c.buf);
    CHECK(!ok);
    CHECK_CONTAINS(err, "picture opcode 0x0022 is not supported");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: the build fails with `'pict.h' file not found`.

- [ ] **Step 3: Write the implementation**

`src/pict.h`:
```c
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "blit.h"

/* Draws a PICT (version 1 or 2) into target, mapping the picture's frame onto
   dst and clipping to clip. Supported opcodes: version, header, DefHilite,
   clip (rectangular), BitsRect, PackBitsRect (1, 2, 4 and 8 bits, srcCopy),
   short and long comments, and end. Anything else writes err and returns false.
   Reference: Inside Macintosh: Imaging With QuickDraw, appendix A. */
bool pict_draw(const uint8_t *data, size_t len, qd_rect dst, const qd_pixels *target, qd_rect clip,
               qd_rgb fg, qd_rgb bg, char *err, size_t errlen);

/* The picture frame from the header. False if data is too short. */
bool pict_frame(const uint8_t *data, size_t len, qd_rect *frame);
```

`src/pict.c`:
```c
#include "pict.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

typedef struct {
    const uint8_t *p, *end;
    int version;
    char *err;
    size_t errlen;
} reader;

static bool fail(reader *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static bool fail(reader *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->err, r->errlen, fmt, ap);
    va_end(ap);
    return false;
}

static bool need(reader *r, size_t n) {
    if ((size_t)(r->end - r->p) < n)
        return fail(r, "picture data is truncated");
    return true;
}

static bool u16(reader *r, uint16_t *v) {
    if (!need(r, 2))
        return false;
    *v = rd_be16(r->p);
    r->p += 2;
    return true;
}

static bool rect(reader *r, qd_rect *out) {
    if (!need(r, 8))
        return false;
    out->top = (int16_t)rd_be16(r->p);
    out->left = (int16_t)rd_be16(r->p + 2);
    out->bottom = (int16_t)rd_be16(r->p + 4);
    out->right = (int16_t)rd_be16(r->p + 6);
    r->p += 8;
    return true;
}

static bool skip(reader *r, size_t n) {
    if (!need(r, n))
        return false;
    r->p += n;
    return true;
}

bool pict_frame(const uint8_t *data, size_t len, qd_rect *frame) {
    if (len < 10)
        return false;
    frame->top = (int16_t)rd_be16(data + 2);
    frame->left = (int16_t)rd_be16(data + 4);
    frame->bottom = (int16_t)rd_be16(data + 6);
    frame->right = (int16_t)rd_be16(data + 8);
    return true;
}

/* Maps a rectangle from picture-frame coordinates to destination coordinates. */
static qd_rect map_rect(qd_rect r, qd_rect frame, qd_rect dst) {
    int fw = rect_w(frame), fh = rect_h(frame), dw = rect_w(dst), dh = rect_h(dst);
    if (fw <= 0 || fh <= 0)
        return dst;
    return (qd_rect){
        (int16_t)(dst.top + (r.top - frame.top) * dh / fh),
        (int16_t)(dst.left + (r.left - frame.left) * dw / fw),
        (int16_t)(dst.top + (r.bottom - frame.top) * dh / fh),
        (int16_t)(dst.left + (r.right - frame.left) * dw / fw),
    };
}

/* Expands one PackBits row into out (exactly n bytes). */
static bool unpack_row(reader *r, size_t packed, uint8_t *out, size_t n) {
    if (!need(r, packed))
        return false;
    const uint8_t *p = r->p, *end = r->p + packed;
    size_t o = 0;
    while (p < end && o < n) {
        int8_t c = (int8_t)*p++;
        if (c >= 0) {
            size_t k = (size_t)c + 1;
            if (k > (size_t)(end - p) || k > n - o)
                return fail(r, "PackBits literal run overflows the row");
            memcpy(out + o, p, k);
            p += k;
            o += k;
        } else if (c != -128) {
            size_t k = (size_t)(1 - c);
            if (p >= end || k > n - o)
                return fail(r, "PackBits repeat run overflows the row");
            memset(out + o, *p++, k);
            o += k;
        }
    }
    if (o != n)
        return fail(r, "PackBits row has %zu of %zu bytes", o, n);
    r->p = end;
    return true;
}

static bool color_table(reader *r, qd_palette *pal) {
    if (!need(r, 8))
        return false;
    uint16_t flags = rd_be16(r->p + 4);
    uint32_t n = rd_be16(r->p + 6) + 1u;
    r->p += 8;
    if (n > 256)
        return fail(r, "color table has %u entries", n);
    if (!need(r, 8 * n))
        return false;
    memset(pal, 0, sizeof *pal);
    pal->n = 256;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = r->p + 8 * i;
        /* A device table (flag 0x8000) is indexed by position; otherwise
           each entry names its pixel value. */
        uint32_t v = (flags & 0x8000) ? i : (rd_be16(e) & 0xFFu);
        pal->c[v] = (qd_rgb){rd_be16(e + 2), rd_be16(e + 4), rd_be16(e + 6)};
    }
    r->p += 8 * n;
    return true;
}

static bool region(reader *r, qd_rect *bbox) {
    uint16_t size;
    if (!u16(r, &size) || size < 10 || !rect(r, bbox) || !skip(r, size - 10u))
        return false;
    if (size != 10)
        return fail(r, "non-rectangular clip regions are not supported");
    return true;
}

/* BitsRect / PackBitsRect: a BitMap or PixMap, then rows of pixels. */
static bool bits_rect(reader *r, bool packed, qd_rect frame, qd_rect dst, const qd_pixels *target,
                      qd_rect clip, qd_rgb fg, qd_rgb bg) {
    uint16_t row_bytes;
    qd_rect bounds;
    if (!u16(r, &row_bytes) || !rect(r, &bounds))
        return false;
    bool is_pixmap = row_bytes & 0x8000;
    row_bytes &= 0x3FFF;
    int depth = 1;
    qd_palette pal;
    if (is_pixmap) {
        if (!need(r, 36))
            return false;
        uint16_t pack_type = rd_be16(r->p + 2);
        depth = rd_be16(r->p + 18);
        uint16_t cmp_count = rd_be16(r->p + 20);
        r->p += 36;
        if (cmp_count != 1 || (depth != 1 && depth != 2 && depth != 4 && depth != 8))
            return fail(r, "%d-bit pixmaps with %u components are not supported", depth, cmp_count);
        if (pack_type > 1)
            return fail(r, "pack type %u is not supported", pack_type);
        if (!color_table(r, &pal))
            return false;
    } else {
        qd_std_palette(1, &pal);
    }
    qd_rect src_rect, dst_rect;
    uint16_t mode;
    if (!rect(r, &src_rect) || !rect(r, &dst_rect) || !u16(r, &mode))
        return false;
    int h = rect_h(bounds);
    if (h < 0 || rect_w(bounds) < 0 || (size_t)row_bytes * 8 < (size_t)rect_w(bounds) * depth)
        return fail(r, "bitmap rows are too short for their bounds");
    uint8_t *pixels = malloc((size_t)row_bytes * (h ? h : 1));
    if (!pixels)
        return fail(r, "out of memory");
    for (int y = 0; y < h; y++) {
        uint8_t *row = pixels + (size_t)y * row_bytes;
        bool ok;
        if (!packed || row_bytes < 8) {
            ok = need(r, row_bytes);
            if (ok) {
                memcpy(row, r->p, row_bytes);
                r->p += row_bytes;
            }
        } else {
            size_t n = 0;
            if (row_bytes > 250) {
                uint16_t c;
                ok = u16(r, &c);
                n = c;
            } else {
                ok = need(r, 1);
                if (ok)
                    n = *r->p++;
            }
            ok = ok && unpack_row(r, n, row, row_bytes);
        }
        if (!ok) {
            free(pixels);
            return false;
        }
    }
    qd_pixels src = {pixels, row_bytes, bounds, depth, &pal};
    bool ok = qd_blit(&src, src_rect, target, map_rect(dst_rect, frame, dst), clip, mode, fg, bg,
                      r->err, r->errlen);
    free(pixels);
    return ok;
}

bool pict_draw(const uint8_t *data, size_t len, qd_rect dst, const qd_pixels *target, qd_rect clip,
               qd_rgb fg, qd_rgb bg, char *err, size_t errlen) {
    reader r = {data, data + len, 0, err, errlen};
    qd_rect frame;
    if (!pict_frame(data, len, &frame))
        return fail(&r, "picture data is truncated");
    r.p += 10;
    if (!need(&r, 2))
        return false;
    if (r.p[0] == 0x11 && r.p[1] == 0x01) {
        r.version = 1;
        r.p += 2;
    } else if (rd_be16(r.p) == 0x0011 && len >= 14 && rd_be16(r.p + 2) == 0x02FF) {
        r.version = 2;
        r.p += 4;
    } else {
        return fail(&r, "not a version 1 or 2 picture");
    }
    for (;;) {
        uint16_t op;
        if (r.version == 1) {
            if (!need(&r, 1))
                return false;
            op = *r.p++;
        } else {
            if ((r.p - data) & 1)
                r.p++;
            if (!u16(&r, &op))
                return false;
        }
        qd_rect bbox;
        switch (op) {
        case 0x00: /* NOP */
            break;
        case 0x01: /* clip region */
            if (!region(&r, &bbox))
                return false;
            break;
        case 0x1E: /* DefHilite */
            break;
        case 0x0C00: /* header */
            if (!skip(&r, 24))
                return false;
            break;
        case 0x90: /* BitsRect */
        case 0x98: /* PackBitsRect */
            if (!bits_rect(&r, op == 0x98, frame, dst, target, clip, fg, bg))
                return false;
            break;
        case 0xA0: /* short comment */
            if (!skip(&r, 2))
                return false;
            break;
        case 0xA1: { /* long comment */
            uint16_t kind, n;
            if (!u16(&r, &kind) || !u16(&r, &n) || !skip(&r, n))
                return false;
            break;
        }
        case 0xFF: /* end of picture */
            return true;
        default:
            return fail(&r, "picture opcode 0x%04x is not supported", op);
        }
    }
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S . -B build && cmake --build build && ./build/loony_tests pict_`
Expected: `7 passed, 0 failed, 0 skipped`. Full suite: `148 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/pict.h src/pict.c tests/test_pict.c
git commit -m "Decode the PICT opcodes the game's pictures use"
```

---

### Task 4: QuickDraw: ports, GWorlds, the screen and drawing

**Files:**
- Create: `src/qd.h`, `src/qd.c`
- Create: `tests/test_qd.c`

**Interfaces:**
- Consumes: Tasks 2–3; `mm_new_handle`, `mm_new_ptr`, `mm_dispose_*`, `mm_is_handle`, `mm_handle_size` (Plan 2); `rsrc_find`, `rsrc_data` (Plan 2); `gm_*`, `trap_*` (Plans 1–2).
- Produces:
  - Structure offsets `PM_*`, `PIXMAP_SIZE`, `PORT_*`, `CGRAFPORT_SIZE`, `GD_*`, `GDEVICE_SIZE`, `CTAB_HEADER`; `QD_NO_ERR`, `QD_PARAM_ERR`
  - `qd_rect qd_read_rect(uint32_t)`, `void qd_write_rect(uint32_t, qd_rect)`, `void qd_read_ctab(uint32_t ctab, qd_palette *)`, `uint32_t qd_new_ctab(const qd_palette *)`
  - `void qd_init(int width, int height, int depth)`, `uint32_t qd_main_device(void)`, `void qd_bits(const char *call, uint32_t bits, qd_pixels *out, qd_palette *pal)`, `void qd_screen(qd_pixels *out, qd_palette *pal)`, `bool qd_take_dirty(void)`, `typedef void (*qd_present_fn)(void)`, `void qd_set_present(qd_present_fn)`, `uint32_t qd_current_port(void)`
  - `void qd_register(void)`, which installs `SetRect`, `OffsetRect`, `GetCTable`, `GetMainDevice`, `SetDepth`, `NewGWorld`, `UpdateGWorld`, `DisposeGWorld`, `GetGWorldPixMap`, `LockPixels`, `UnlockPixels`, `GetPixBaseAddr`, `SetGWorld`, `GetGWorld`, `SetPortWindowPort`, `GetWindowPort`, `GetPortBounds`, `GetWindowPortBounds`, `GetPortBitMapForCopyBits`, `GetQDGlobalsScreenBits`, `ShowWindow`, `HideWindow`, `InvalWindowRect`, `QDFlushPortBuffer`, `BeginFullScreen`, `EndFullScreen`, `ClipRect`, `RGBForeColor`, `PaintRect`, `CopyBits`, `DrawPicture`

Guest layouts (*Imaging With QuickDraw*):
- **PixMap**, 50 bytes: baseAddr, rowBytes (with `0x8000` set for a PixMap), bounds, version, packType, packSize, hRes, vRes, pixelType, pixelSize, cmpCount, cmpSize, pixelFormat, pmTable, pmExt.
- **CGrafPort**, 108 bytes: portPixMap at +2, portRect at +16, visRgn and clipRgn at +24 and +28, rgbFgColor and rgbBkColor at +36 and +42.
- **GDevice**, 62 bytes: gdType at +4, gdPMap at +22, gdRect at +34.
- **Regions:** a 10-byte rectangular handle. Other regions fail loudly.

A WindowRef is its CGrafPort's address, so `GetWindowPort` returns its argument. Ports are tracked host-side, so passing anything else as a port crashes with the call's name.

- [ ] **Step 1: Write the failing test**

`tests/test_qd.c`:
```c
#include "test.h"

#include <stdlib.h>

#include "harness.h"
#include "memmgr.h"
#include "qd.h"
#include "rsrc.h"

static const char *const names[] = {
    "SetRect", "OffsetRect", "GetCTable", "GetMainDevice", "SetDepth", "NewGWorld",
    "UpdateGWorld", "DisposeGWorld", "GetGWorldPixMap", "LockPixels", "UnlockPixels",
    "GetPixBaseAddr", "SetGWorld", "GetGWorld", "SetPortWindowPort", "GetWindowPort",
    "GetPortBounds", "GetWindowPortBounds", "GetPortBitMapForCopyBits", "GetQDGlobalsScreenBits",
    "ShowWindow", "HideWindow", "InvalWindowRect", "QDFlushPortBuffer", "BeginFullScreen",
    "EndFullScreen", "ClipRect", "RGBForeColor", "PaintRect", "CopyBits", "DrawPicture",
};

static void setup(void) {
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    qd_init(800, 600, 8);
    qd_register();
}

static uint32_t rect(int top, int left, int bottom, int right) {
    uint32_t r = scratch(8);
    qd_write_rect(r, (qd_rect){(int16_t)top, (int16_t)left, (int16_t)bottom, (int16_t)right});
    return r;
}

static uint32_t rgb(uint16_t r, uint16_t g, uint16_t b) {
    uint32_t c = scratch(6);
    gm_w16(c, r);
    gm_w16(c + 2, g);
    gm_w16(c + 4, b);
    return c;
}

static uint32_t new_gworld(int depth, int w, int h) {
    uint32_t out = scratch(4);
    if (call_import("NewGWorld", 6, out, (uint32_t)depth, rect(0, 0, h, w), 0u, 0u, 0u) != 0)
        fatal("NewGWorld failed");
    return gm_r32(out);
}

static uint32_t base_of(uint32_t gw) {
    return call_import("GetPixBaseAddr", 1, call_import("GetGWorldPixMap", 1, gw));
}

TEST(qd_set_rect_and_offset_rect) {
    setup();
    uint32_t r = scratch(8);
    call_import("SetRect", 5, r, 10u, 20u, 30u, 40u); /* left, top, right, bottom */
    qd_rect q = qd_read_rect(r);
    CHECK_EQ(q.top, 20);
    CHECK_EQ(q.left, 10);
    CHECK_EQ(q.bottom, 40);
    CHECK_EQ(q.right, 30);
    call_import("OffsetRect", 3, r, (uint32_t)-5, 7u);
    q = qd_read_rect(r);
    CHECK_EQ(q.top, 27);
    CHECK_EQ(q.left, 5);
}

TEST(qd_new_gworld_builds_a_pixmap) {
    setup();
    uint32_t gw = new_gworld(8, 20, 10);
    uint32_t pm_h = call_import("GetGWorldPixMap", 1, gw);
    CHECK(mm_is_handle(pm_h));
    uint32_t pm = gm_r32(pm_h);
    CHECK_EQ(gm_r16(pm + PM_ROW_BYTES), 0x8000 | 20);
    CHECK_EQ(gm_r16(pm + PM_PIXEL_SIZE), 8);
    qd_rect b = qd_read_rect(pm + PM_BOUNDS);
    CHECK_EQ(b.bottom, 10);
    CHECK_EQ(b.right, 20);
    uint32_t ctab = gm_r32(pm + PM_TABLE);
    CHECK(mm_is_handle(ctab));
    CHECK_EQ(gm_r16(gm_r32(ctab) + 6), 255); /* ctSize: 256 entries */
    CHECK_EQ(base_of(gw), gm_r32(pm + PM_BASE_ADDR));
    CHECK(mm_is_ptr(base_of(gw)));
    CHECK_EQ(call_import("LockPixels", 1, pm_h), 1);
    CHECK_EQ(call_import("GetPortBitMapForCopyBits", 1, gw), pm);
    uint32_t r = scratch(8);
    CHECK_EQ(call_import("GetPortBounds", 2, gw, r), r);
    CHECK_EQ(qd_read_rect(r).right, 20);
}

TEST(qd_new_gworld_depth_0_uses_the_screen_depth) {
    setup();
    uint32_t gw = new_gworld(0, 4, 4);
    CHECK_EQ(gm_r16(gm_r32(call_import("GetGWorldPixMap", 1, gw)) + PM_PIXEL_SIZE), 8);
}

TEST(qd_set_and_get_gworld) {
    setup();
    uint32_t gw = new_gworld(16, 4, 4);
    call_import("SetGWorld", 2, gw, 0u);
    uint32_t p = scratch(4), d = scratch(4);
    call_import("GetGWorld", 2, p, d);
    CHECK_EQ(gm_r32(p), gw);
    CHECK_EQ(gm_r32(d), call_import("GetMainDevice", 0));
    CHECK_EQ(qd_current_port(), gw);
}

TEST(qd_paint_rect_uses_the_fore_color_and_clip) {
    setup();
    uint32_t gw = new_gworld(16, 4, 2);
    call_import("SetGWorld", 2, gw, 0u);
    call_import("RGBForeColor", 1, rgb(0xFFFF, 0, 0));
    call_import("ClipRect", 1, rect(0, 0, 2, 3));
    call_import("PaintRect", 1, rect(0, 1, 1, 4));
    uint32_t b = base_of(gw);
    CHECK_EQ(gm_r16(b), 0);
    CHECK_EQ(gm_r16(b + 2), 0x7C00);
    CHECK_EQ(gm_r16(b + 4), 0x7C00);
    CHECK_EQ(gm_r16(b + 6), 0); /* clipped */
    CHECK_EQ(gm_r16(b + 8), 0); /* row 1 untouched */
}

TEST(qd_copy_bits_converts_8_to_16_bits) {
    setup();
    uint32_t src = new_gworld(8, 4, 1), dst = new_gworld(16, 4, 1);
    gm_w8(base_of(src) + 1, 255); /* black */
    call_import("SetGWorld", 2, dst, 0u);
    uint32_t sb = call_import("GetPortBitMapForCopyBits", 1, src);
    uint32_t db = call_import("GetPortBitMapForCopyBits", 1, dst);
    call_import("CopyBits", 6, sb, db, rect(0, 0, 1, 4), rect(0, 0, 1, 4), 0u, 0u);
    uint32_t b = base_of(dst);
    CHECK_EQ(gm_r16(b), 0x7FFF); /* white */
    CHECK_EQ(gm_r16(b + 2), 0);  /* black */
}

static void child_copy_bits_mask(void *unused) {
    (void)unused;
    setup();
    uint32_t gw = new_gworld(8, 4, 4);
    uint32_t bits = call_import("GetPortBitMapForCopyBits", 1, gw);
    call_import("CopyBits", 6, bits, bits, rect(0, 0, 1, 1), rect(0, 0, 1, 1), 0u, 0x1234u);
}

TEST(qd_copy_bits_with_a_mask_region_crashes) {
    char out[16384];
    CHECK_EQ(test_run_child(child_copy_bits_mask, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "CopyBits: mask regions are not supported");
}

TEST(qd_set_depth_recreates_the_screen) {
    setup();
    uint32_t gd = call_import("GetMainDevice", 0);
    CHECK_EQ(call_import("SetDepth", 4, gd, 16u, 0u, 1u), 0);
    uint32_t pm = gm_r32(gm_r32(gm_r32(gd) + GD_PMAP));
    CHECK_EQ(gm_r16(pm + PM_PIXEL_SIZE), 16);
    CHECK_EQ(gm_r16(gm_r32(gd) + GD_TYPE), 2);
    uint32_t bits = scratch(14);
    call_import("GetQDGlobalsScreenBits", 1, bits);
    CHECK_EQ(gm_r16(bits + 4), 1600);
    qd_rect b = qd_read_rect(bits + 6);
    CHECK_EQ(b.bottom, 600);
    CHECK_EQ(b.right, 800);
}

static int presents;
static void count_present(void) { presents++; }

TEST(qd_begin_full_screen_makes_a_window_on_the_screen) {
    setup();
    call_import("SetDepth", 4, call_import("GetMainDevice", 0), 16u, 0u, 1u);
    uint32_t restore = scratch(4), win_p = scratch(4);
    qd_take_dirty();
    CHECK_EQ(call_import("BeginFullScreen", 7, restore, 0u, 0u, 0u, win_p, rgb(0, 0, 0xFFFF), 2u), 0);
    uint32_t win = gm_r32(win_p);
    CHECK(win != 0);
    CHECK_EQ(call_import("GetWindowPort", 1, win), win);
    uint32_t r = scratch(8);
    call_import("GetWindowPortBounds", 2, win, r);
    CHECK_EQ(qd_read_rect(r).right, 800);
    qd_pixels px;
    qd_palette pal;
    qd_screen(&px, &pal);
    CHECK_EQ(rd_be16(px.base), 0x001F); /* erased to blue */
    CHECK(qd_take_dirty());
    call_import("SetPortWindowPort", 1, win);
    call_import("RGBForeColor", 1, rgb(0xFFFF, 0, 0));
    call_import("PaintRect", 1, rect(0, 0, 1, 1));
    CHECK_EQ(rd_be16(px.base), 0x7C00);
    CHECK(qd_take_dirty());
    presents = 0;
    qd_set_present(count_present);
    call_import("QDFlushPortBuffer", 2, win, 0u);
    CHECK_EQ(presents, 1);
    call_import("HideWindow", 1, win);
    call_import("ShowWindow", 1, win);
    call_import("InvalWindowRect", 2, win, r);
    CHECK_EQ(call_import("EndFullScreen", 2, gm_r32(restore), 0u), 0);
}

TEST(qd_get_ctable_standard_tables) {
    setup();
    uint32_t h = call_import("GetCTable", 1, 8u);
    CHECK(mm_is_handle(h));
    qd_palette pal;
    qd_read_ctab(h, &pal);
    CHECK_EQ(pal.c[0].r, 0xFFFF);
    CHECK_EQ(pal.c[255].r, 0);
    CHECK_EQ(call_import("GetCTable", 1, 99u), 0);
}

TEST(qd_update_gworld_with_the_same_size_is_a_no_op) {
    setup();
    uint32_t gw = new_gworld(8, 16, 8), p = scratch(4);
    gm_w32(p, gw);
    CHECK_EQ(call_import("UpdateGWorld", 6, p, 8u, rect(0, 0, 8, 16), 0u, 0u, 0u), 0);
    CHECK_EQ(call_import("UpdateGWorld", 6, p, 0u, rect(10, 10, 18, 26), 0u, 0u, 0u), 0);
}

static void child_update_gworld_resize(void *unused) {
    (void)unused;
    setup();
    uint32_t gw = new_gworld(8, 16, 8), p = scratch(4);
    gm_w32(p, gw);
    call_import("UpdateGWorld", 6, p, 8u, rect(0, 0, 9, 16), 0u, 0u, 0u);
}

TEST(qd_update_gworld_resize_crashes) {
    char out[16384];
    CHECK_EQ(test_run_child(child_update_gworld_resize, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "UpdateGWorld: changing a 16x8 8-bit GWorld to 16x9 8-bit");
}

TEST(qd_dispose_gworld_frees_its_memory) {
    setup();
    uint32_t before = mm_free_bytes();
    uint32_t gw = new_gworld(16, 64, 64);
    CHECK(mm_free_bytes() < before);
    call_import("DisposeGWorld", 1, gw);
    CHECK_EQ(mm_free_bytes(), before);
}

static void child_bad_port(void *unused) {
    (void)unused;
    setup();
    call_import("SetGWorld", 2, 0x00101234u, 0u);
}

TEST(qd_unknown_port_crashes) {
    char out[16384];
    CHECK_EQ(test_run_child(child_bad_port, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "SetGWorld: 0x00101234 is not a port");
}

TEST(qd_draw_picture_into_a_gworld) {
    SKIP_UNLESS_GAME();
    setup();
    char path[1100];
    size_t len;
    snprintf(path, sizeof path, "%s/..namedfork/rsrc", test_game_exe_path());
    uint8_t *fork = read_file(path, &len);
    CHECK(fork != NULL);
    char err[256];
    CHECK(rsrc_open(fork, len, err, sizeof err));
    rsrc_entry *e = rsrc_find(FOURCC('P', 'I', 'C', 'T'), 800);
    uint32_t pic = mm_new_handle(e->len, false);
    memcpy(gm_ptr(gm_r32(pic), e->len), rsrc_data(e), e->len);
    uint32_t gw = new_gworld(8, 512, 384);
    call_import("SetGWorld", 2, gw, 0u);
    call_import("DrawPicture", 2, pic, rect(0, 0, 384, 512));
    uint32_t h = fnv1a32(gm_ptr(base_of(gw), 512 * 384), 512 * 384);
    rsrc_close();
    free(fork);
    CHECK_EQ(h, 0x4657C203u); /* same as pict_decodes_the_title_picture */
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: the build fails with `'qd.h' file not found`.

- [ ] **Step 3: Write the implementation**

`src/qd.h`:
```c
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "blit.h"

/* QuickDraw subset. The structures the game can see live in guest memory in
   their original big-endian layout (Inside Macintosh: Imaging With
   QuickDraw): Rects, PixMaps (in handles), CGrafPorts, the main GDevice and
   color tables. A WindowRef is the address of the window's CGrafPort. */

/* Rect: top, left, bottom, right (int16 each). */
#define RECT_SIZE 8

/* PixMap, 50 bytes. */
#define PM_BASE_ADDR   0
#define PM_ROW_BYTES   4  /* high bits 0x8000 mark a PixMap rather than a BitMap */
#define PM_BOUNDS      6
#define PM_VERSION     14
#define PM_PACK_TYPE   16
#define PM_PACK_SIZE   18
#define PM_HRES        22
#define PM_VRES        26
#define PM_PIXEL_TYPE  30 /* 0 indexed, 16 (RGBDirect) direct */
#define PM_PIXEL_SIZE  32
#define PM_CMP_COUNT   34
#define PM_CMP_SIZE    36
#define PM_PIXEL_FORMAT 38
#define PM_TABLE       42 /* CTabHandle */
#define PM_EXT         46
#define PIXMAP_SIZE    50

/* CGrafPort, 108 bytes. */
#define PORT_DEVICE    0
#define PORT_PIXMAP    2  /* PixMapHandle */
#define PORT_VERSION   6  /* 0xC000 for a color port */
#define PORT_RECT      16
#define PORT_VIS_RGN   24
#define PORT_CLIP_RGN  28
#define PORT_RGB_FG    36
#define PORT_RGB_BK    42
#define PORT_PN_SIZE   52
#define PORT_PN_MODE   56
#define PORT_PN_VIS    66
#define PORT_FG_COLOR  80
#define PORT_BK_COLOR  84
#define CGRAFPORT_SIZE 108

/* GDevice, 62 bytes. */
#define GD_TYPE  4  /* 0 CLUT, 2 direct */
#define GD_FLAGS 20
#define GD_PMAP  22
#define GD_RECT  34
#define GDEVICE_SIZE 62

/* ColorTable: ctSeed (4), ctFlags (2), ctSize (count - 1, 2), then 8-byte
   ColorSpecs: value, red, green, blue. */
#define CTAB_HEADER 8

#define QD_NO_ERR 0
#define QD_PARAM_ERR (-50)

qd_rect qd_read_rect(uint32_t addr);
void qd_write_rect(uint32_t addr, qd_rect r);

/* Reads a CTabHandle into pal. */
void qd_read_ctab(uint32_t ctab, qd_palette *pal);
/* A new CTabHandle holding pal (pal->n entries, values 0..n-1). */
uint32_t qd_new_ctab(const qd_palette *pal);

/* Creates the main screen (GDevice, PixMap and pixels in the guest heap) at
   width x height and depth (8, 16 or 32), and makes it the current port and
   device. Requires mm_init() and rsrc_open(). */
void qd_init(int width, int height, int depth);

uint32_t qd_main_device(void);
/* Describes a BitMap or PixMap (a pointer, not a handle). pal receives the
   colors for indexed depths. Crashes on an unsupported pixel format. */
void qd_bits(const char *call, uint32_t bits, qd_pixels *out, qd_palette *pal);
/* The screen's pixels, for display. */
void qd_screen(qd_pixels *out, qd_palette *pal);

/* True if anything drew to the screen since the last call. */
bool qd_take_dirty(void);
/* Called when the game flushes a window to the screen (QDFlushPortBuffer). */
typedef void (*qd_present_fn)(void);
void qd_set_present(qd_present_fn fn);

uint32_t qd_current_port(void);

/* Registers the QuickDraw, GWorld and window imports. */
void qd_register(void);
```

`src/qd.c`:
```c
#include "qd.h"

#include <string.h>

#include "guest_mem.h"
#include "memmgr.h"
#include "pict.h"
#include "rsrc.h"
#include "trap.h"
#include "util.h"

#define MAX_PORTS 256
#define WIDE_OPEN ((qd_rect){-32768, -32768, 32767, 32767})

typedef enum { KIND_SCREEN, KIND_GWORLD, KIND_WINDOW } port_kind;

typedef struct {
    uint32_t addr;
    port_kind kind;
    bool visible;
} port_info;

static struct {
    uint32_t main_device;
    uint32_t screen_pm;  /* the screen's PixMapHandle, shared by windows */
    uint32_t screen_port;
    port_info ports[MAX_PORTS];
    int nports;
    uint32_t cur_port, cur_device;
    bool dirty;
    qd_present_fn present;
    int saved_w, saved_h; /* screen size before BeginFullScreen */
} Q;

/* ---- guest structure helpers ---- */

qd_rect qd_read_rect(uint32_t a) {
    return (qd_rect){(int16_t)gm_r16(a), (int16_t)gm_r16(a + 2), (int16_t)gm_r16(a + 4),
                     (int16_t)gm_r16(a + 6)};
}

void qd_write_rect(uint32_t a, qd_rect r) {
    gm_w16(a, (uint16_t)r.top);
    gm_w16(a + 2, (uint16_t)r.left);
    gm_w16(a + 4, (uint16_t)r.bottom);
    gm_w16(a + 6, (uint16_t)r.right);
}

static qd_rgb read_rgb(uint32_t a) { return (qd_rgb){gm_r16(a), gm_r16(a + 2), gm_r16(a + 4)}; }

static void write_rgb(uint32_t a, qd_rgb c) {
    gm_w16(a, c.r);
    gm_w16(a + 2, c.g);
    gm_w16(a + 4, c.b);
}

void qd_read_ctab(uint32_t ctab, qd_palette *pal) {
    memset(pal, 0, sizeof *pal);
    uint32_t t = gm_r32(ctab);
    uint16_t flags = gm_r16(t + 4);
    uint32_t n = (uint16_t)(gm_r16(t + 6) + 1u);
    if (n > 256)
        n = 256;
    pal->n = 256;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = t + CTAB_HEADER + 8 * i;
        uint32_t v = (flags & 0x8000) ? i : (gm_r16(e) & 0xFFu);
        pal->c[v] = read_rgb(e + 2);
    }
}

uint32_t qd_new_ctab(const qd_palette *pal) {
    uint32_t h = mm_new_handle(CTAB_HEADER + 8u * (uint32_t)pal->n, true);
    if (!h)
        trap_crash("out of guest memory for a color table");
    uint32_t t = gm_r32(h);
    gm_w32(t, 1000u + (uint32_t)pal->n); /* ctSeed */
    gm_w16(t + 4, 0);
    gm_w16(t + 6, (uint16_t)(pal->n - 1));
    for (int i = 0; i < pal->n; i++) {
        uint32_t e = t + CTAB_HEADER + 8u * (uint32_t)i;
        gm_w16(e, (uint16_t)i);
        write_rgb(e + 2, pal->c[i]);
    }
    return h;
}

static uint32_t new_rgn(qd_rect r) {
    uint32_t h = mm_new_handle(10, true);
    if (!h)
        trap_crash("out of guest memory for a region");
    gm_w16(gm_r32(h), 10);
    qd_write_rect(gm_r32(h) + 2, r);
    return h;
}

/* The bounding box of a region, which must be rectangular. */
static qd_rect rgn_rect(const char *call, uint32_t rgn) {
    uint32_t p = gm_r32(rgn);
    if (gm_r16(p) != 10)
        trap_crash("%s: non-rectangular regions are not supported", call);
    return qd_read_rect(p + 2);
}

static uint32_t row_bytes_for(int width, int depth) {
    return (uint32_t)((width * depth + 31) / 32 * 4);
}

/* Fills a PixMap for depth and bounds, with new zeroed pixels. */
static void setup_pixmap(uint32_t pm_h, int depth, qd_rect bounds, uint32_t ctab) {
    uint32_t rb = row_bytes_for(rect_w(bounds), depth);
    uint32_t base = mm_new_ptr(rb * (uint32_t)rect_h(bounds), true);
    if (!base)
        trap_crash("out of guest memory for a %dx%d %d-bit pixmap", rect_w(bounds), rect_h(bounds),
                   depth);
    uint32_t pm = gm_r32(pm_h);
    memset(gm_ptr(pm, PIXMAP_SIZE), 0, PIXMAP_SIZE);
    gm_w32(pm + PM_BASE_ADDR, base);
    gm_w16(pm + PM_ROW_BYTES, (uint16_t)(rb | 0x8000));
    qd_write_rect(pm + PM_BOUNDS, bounds);
    gm_w32(pm + PM_HRES, 72u << 16);
    gm_w32(pm + PM_VRES, 72u << 16);
    gm_w16(pm + PM_PIXEL_TYPE, depth > 8 ? 16 : 0);
    gm_w16(pm + PM_PIXEL_SIZE, (uint16_t)depth);
    gm_w16(pm + PM_CMP_COUNT, depth > 8 ? 3 : 1);
    gm_w16(pm + PM_CMP_SIZE, depth == 16 ? 5 : depth == 32 ? 8 : (uint16_t)depth);
    gm_w32(pm + PM_PIXEL_FORMAT, (uint32_t)depth);
    gm_w32(pm + PM_TABLE, ctab);
}

static uint32_t std_ctab(int depth) {
    qd_palette pal;
    if (depth <= 8) {
        qd_std_palette(depth, &pal);
    } else {
        memset(&pal, 0, sizeof pal);
        pal.n = 1; /* direct pixmaps still carry a (trivial) table */
    }
    return qd_new_ctab(&pal);
}

static void free_pixmap_contents(uint32_t pm_h) {
    uint32_t pm = gm_r32(pm_h);
    mm_dispose_ptr(gm_r32(pm + PM_BASE_ADDR));
    mm_dispose_handle(gm_r32(pm + PM_TABLE));
}

void qd_bits(const char *call, uint32_t bits, qd_pixels *out, qd_palette *pal) {
    uint16_t rb = gm_r16(bits + PM_ROW_BYTES);
    qd_rect b = qd_read_rect(bits + PM_BOUNDS);
    int depth = 1;
    if (rb & 0x8000) {
        depth = gm_r16(bits + PM_PIXEL_SIZE);
        if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16 && depth != 32)
            trap_crash("%s: %d-bit pixmaps are not supported", call, depth);
        if (depth <= 8)
            qd_read_ctab(gm_r32(bits + PM_TABLE), pal);
    } else {
        qd_std_palette(1, pal);
    }
    uint32_t row_bytes = rb & 0x3FFFu;
    uint32_t h = rect_h(b) > 0 ? (uint32_t)rect_h(b) : 0;
    uint32_t base = gm_r32(bits + PM_BASE_ADDR);
    out->base = gm_ptr(base, row_bytes * h);
    out->row_bytes = row_bytes;
    out->bounds = b;
    out->depth = depth;
    out->pal = pal;
}

/* ---- ports ---- */

static port_info *find_port(uint32_t addr) {
    for (int i = 0; i < Q.nports; i++)
        if (Q.ports[i].addr == addr)
            return &Q.ports[i];
    return NULL;
}

static port_info *need_port(const char *call, uint32_t addr) {
    port_info *p = find_port(addr);
    if (!p)
        trap_crash("%s: 0x%08x is not a port", call, addr);
    return p;
}

static uint32_t new_port(uint32_t pm_h, qd_rect port_rect, port_kind kind) {
    if (Q.nports == MAX_PORTS)
        trap_crash("more than %d ports", MAX_PORTS);
    uint32_t p = mm_new_ptr(CGRAFPORT_SIZE, true);
    if (!p)
        trap_crash("out of guest memory for a port");
    gm_w32(p + PORT_PIXMAP, pm_h);
    gm_w16(p + PORT_VERSION, 0xC000);
    qd_write_rect(p + PORT_RECT, port_rect);
    gm_w32(p + PORT_VIS_RGN, new_rgn(port_rect));
    gm_w32(p + PORT_CLIP_RGN, new_rgn(WIDE_OPEN));
    write_rgb(p + PORT_RGB_FG, (qd_rgb){0, 0, 0});
    write_rgb(p + PORT_RGB_BK, (qd_rgb){0xFFFF, 0xFFFF, 0xFFFF});
    gm_w16(p + PORT_PN_SIZE, 1);
    gm_w16(p + PORT_PN_SIZE + 2, 1);
    gm_w32(p + PORT_FG_COLOR, 33); /* blackColor */
    gm_w32(p + PORT_BK_COLOR, 30); /* whiteColor */
    Q.ports[Q.nports++] = (port_info){p, kind, kind != KIND_GWORLD};
    return p;
}

static void dispose_port(port_info *info) {
    uint32_t p = info->addr;
    mm_dispose_handle(gm_r32(p + PORT_VIS_RGN));
    mm_dispose_handle(gm_r32(p + PORT_CLIP_RGN));
    mm_dispose_ptr(p);
    *info = Q.ports[--Q.nports];
    if (Q.cur_port == p)
        Q.cur_port = Q.screen_port;
}

static uint32_t port_pixmap(uint32_t port) { return gm_r32(gm_r32(port + PORT_PIXMAP)); }

static bool is_screen_bits(uint32_t bits) {
    return gm_r32(bits + PM_BASE_ADDR) == gm_r32(gm_r32(Q.screen_pm) + PM_BASE_ADDR);
}

/* The current port's clip rectangle intersected with its portRect. */
static qd_rect port_clip(const char *call, uint32_t port) {
    return rect_sect(qd_read_rect(port + PORT_RECT), rgn_rect(call, gm_r32(port + PORT_CLIP_RGN)));
}

/* ---- the screen ---- */

static void make_screen(int w, int h, int depth) {
    qd_rect b = {0, 0, (int16_t)h, (int16_t)w};
    setup_pixmap(Q.screen_pm, depth, b, std_ctab(depth));
    uint32_t gd = gm_r32(Q.main_device);
    gm_w16(gd + GD_TYPE, depth > 8 ? 2 : 0);
    qd_write_rect(gd + GD_RECT, b);
    for (int i = 0; i < Q.nports; i++)
        if (Q.ports[i].kind != KIND_GWORLD) {
            qd_write_rect(Q.ports[i].addr + PORT_RECT, b);
            qd_write_rect(gm_r32(gm_r32(Q.ports[i].addr + PORT_VIS_RGN)) + 2, b);
        }
    Q.dirty = true;
}

void qd_init(int width, int height, int depth) {
    memset(&Q, 0, sizeof Q);
    Q.main_device = mm_new_handle(GDEVICE_SIZE, true);
    Q.screen_pm = mm_new_handle(PIXMAP_SIZE, true);
    if (!Q.main_device || !Q.screen_pm)
        trap_crash("out of guest memory for the screen");
    gm_w32(gm_r32(Q.main_device) + GD_PMAP, Q.screen_pm);
    gm_w16(gm_r32(Q.main_device) + GD_FLAGS, 0x8000 | 0x0001); /* main screen, screen device */
    make_screen(width, height, depth);
    Q.screen_port = new_port(Q.screen_pm, (qd_rect){0, 0, (int16_t)height, (int16_t)width},
                             KIND_SCREEN);
    Q.cur_port = Q.screen_port;
    Q.cur_device = Q.main_device;
}

uint32_t qd_main_device(void) { return Q.main_device; }
uint32_t qd_current_port(void) { return Q.cur_port; }

void qd_screen(qd_pixels *out, qd_palette *pal) { qd_bits("screen", gm_r32(Q.screen_pm), out, pal); }

bool qd_take_dirty(void) {
    bool d = Q.dirty;
    Q.dirty = false;
    return d;
}

void qd_set_present(qd_present_fn fn) { Q.present = fn; }

static void resize_screen(int w, int h, int depth) {
    free_pixmap_contents(Q.screen_pm);
    make_screen(w, h, depth);
}

/* ---- guest calls: rectangles ---- */

static void h_set_rect(void) {
    qd_write_rect(trap_arg(0), (qd_rect){(int16_t)trap_arg(2), (int16_t)trap_arg(1),
                                         (int16_t)trap_arg(4), (int16_t)trap_arg(3)});
}

static void h_offset_rect(void) {
    uint32_t a = trap_arg(0);
    qd_rect r = qd_read_rect(a);
    int16_t dh = (int16_t)trap_arg(1), dv = (int16_t)trap_arg(2);
    qd_write_rect(a, (qd_rect){(int16_t)(r.top + dv), (int16_t)(r.left + dh),
                               (int16_t)(r.bottom + dv), (int16_t)(r.right + dh)});
}

/* ---- guest calls: color tables and devices ---- */

/* GetCTable(id): a new copy of 'clut' resource id, or of the standard table
   for IDs 1, 2, 4 and 8. NULL otherwise. */
static void h_get_ctable(void) {
    int16_t id = (int16_t)trap_arg(0);
    rsrc_entry *e = rsrc_find(FOURCC('c', 'l', 'u', 't'), id);
    if (e) {
        uint32_t h = mm_new_handle(e->len, false);
        if (!h)
            trap_crash("GetCTable: out of guest memory");
        memcpy(gm_ptr(gm_r32(h), e->len), rsrc_data(e), e->len);
        trap_return(h);
        return;
    }
    if (id == 1 || id == 2 || id == 4 || id == 8) {
        trap_return(std_ctab(id));
        return;
    }
    trap_return(0);
}

static void h_get_main_device(void) { trap_return(Q.main_device); }

/* SetDepth(gd, depth, whichFlags, flags) -> OSErr */
static void h_set_depth(void) {
    uint32_t gd = trap_arg(0);
    int depth = (int16_t)trap_arg(1);
    if (gd != Q.main_device)
        trap_crash("SetDepth: 0x%08x is not the main device", gd);
    if (depth != 8 && depth != 16 && depth != 32)
        trap_crash("SetDepth: depth %d is not supported", depth);
    qd_rect b = qd_read_rect(gm_r32(Q.screen_pm) + PM_BOUNDS);
    resize_screen(rect_w(b), rect_h(b), depth);
    trap_return(QD_NO_ERR);
}

/* ---- guest calls: GWorlds ---- */

/* NewGWorld(GWorldPtr *out, short depth, const Rect *bounds, CTabHandle ctab,
   GDHandle device, GWorldFlags flags) -> QDErr */
static void h_new_gworld(void) {
    uint32_t out = trap_arg(0);
    int depth = (int16_t)trap_arg(1);
    qd_rect b = qd_read_rect(trap_arg(2));
    uint32_t ctab = trap_arg(3);
    if (depth == 0)
        depth = gm_r16(gm_r32(Q.screen_pm) + PM_PIXEL_SIZE);
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16 && depth != 32)
        trap_crash("NewGWorld: depth %d is not supported", depth);
    if (rect_empty(b))
        trap_crash("NewGWorld: empty bounds (%d,%d,%d,%d)", b.top, b.left, b.bottom, b.right);
    uint32_t own_ctab;
    if (ctab && depth <= 8) {
        qd_palette pal;
        qd_read_ctab(ctab, &pal);
        pal.n = 1 << depth;
        own_ctab = qd_new_ctab(&pal);
    } else {
        own_ctab = std_ctab(depth);
    }
    uint32_t pm_h = mm_new_handle(PIXMAP_SIZE, true);
    if (!pm_h)
        trap_crash("NewGWorld: out of guest memory");
    setup_pixmap(pm_h, depth, b, own_ctab);
    gm_w32(out, new_port(pm_h, b, KIND_GWORLD));
    trap_return(QD_NO_ERR);
}

/* UpdateGWorld(GWorldPtr *gw, short depth, const Rect *bounds, CTabHandle ctab,
   GDHandle device, GWorldFlags flags) -> GWorldFlags. Only a no-op update
   (same size and depth) is supported. */
static void h_update_gworld(void) {
    uint32_t gw = gm_r32(trap_arg(0));
    port_info *info = need_port("UpdateGWorld", gw);
    if (info->kind != KIND_GWORLD)
        trap_crash("UpdateGWorld: 0x%08x is not a GWorld", gw);
    int depth = (int16_t)trap_arg(1);
    qd_rect b = qd_read_rect(trap_arg(2));
    uint32_t pm = port_pixmap(gw);
    qd_rect cur = qd_read_rect(pm + PM_BOUNDS);
    int cur_depth = gm_r16(pm + PM_PIXEL_SIZE);
    if ((depth != 0 && depth != cur_depth) || rect_w(b) != rect_w(cur) || rect_h(b) != rect_h(cur))
        trap_crash("UpdateGWorld: changing a %dx%d %d-bit GWorld to %dx%d %d-bit is not supported",
                   rect_w(cur), rect_h(cur), cur_depth, rect_w(b), rect_h(b), depth);
    trap_return(0);
}

static void h_dispose_gworld(void) {
    uint32_t gw = trap_arg(0);
    port_info *info = need_port("DisposeGWorld", gw);
    if (info->kind != KIND_GWORLD)
        trap_crash("DisposeGWorld: 0x%08x is not a GWorld", gw);
    uint32_t pm_h = gm_r32(gw + PORT_PIXMAP);
    free_pixmap_contents(pm_h);
    mm_dispose_handle(pm_h);
    dispose_port(info);
}

static void h_get_gworld_pixmap(void) {
    uint32_t gw = trap_arg(0);
    need_port("GetGWorldPixMap", gw);
    trap_return(gm_r32(gw + PORT_PIXMAP));
}

static void h_lock_pixels(void) { trap_return(1); }
static void h_unlock_pixels(void) {}

static void h_get_pix_base_addr(void) { trap_return(gm_r32(gm_r32(trap_arg(0)) + PM_BASE_ADDR)); }

static void h_set_gworld(void) {
    uint32_t port = trap_arg(0), gd = trap_arg(1);
    need_port("SetGWorld", port);
    Q.cur_port = port;
    Q.cur_device = gd ? gd : Q.main_device;
}

static void h_get_gworld(void) {
    gm_w32(trap_arg(0), Q.cur_port);
    if (trap_arg(1))
        gm_w32(trap_arg(1), Q.cur_device);
}

/* ---- guest calls: ports and windows ---- */

static void h_set_port_window_port(void) {
    uint32_t w = trap_arg(0);
    need_port("SetPortWindowPort", w);
    Q.cur_port = w;
}

static void h_get_window_port(void) {
    need_port("GetWindowPort", trap_arg(0));
    trap_return(trap_arg(0));
}

static void h_get_port_bounds(void) {
    uint32_t port = trap_arg(0), r = trap_arg(1);
    need_port("GetPortBounds", port);
    qd_write_rect(r, qd_read_rect(port + PORT_RECT));
    trap_return(r);
}

static void h_get_window_port_bounds(void) {
    uint32_t w = trap_arg(0), r = trap_arg(1);
    need_port("GetWindowPortBounds", w);
    qd_write_rect(r, qd_read_rect(w + PORT_RECT));
    trap_return(r);
}

static void h_get_port_bitmap_for_copy_bits(void) {
    uint32_t port = trap_arg(0);
    need_port("GetPortBitMapForCopyBits", port);
    trap_return(port_pixmap(port));
}

static void h_get_qd_globals_screen_bits(void) {
    uint32_t out = trap_arg(0), pm = gm_r32(Q.screen_pm);
    gm_w32(out, gm_r32(pm + PM_BASE_ADDR));
    gm_w16(out + 4, gm_r16(pm + PM_ROW_BYTES) & 0x3FFF);
    qd_write_rect(out + 6, qd_read_rect(pm + PM_BOUNDS));
    trap_return(out);
}

static void h_show_window(void) { need_port("ShowWindow", trap_arg(0))->visible = true; Q.dirty = true; }
static void h_hide_window(void) { need_port("HideWindow", trap_arg(0))->visible = false; Q.dirty = true; }

static void h_inval_window_rect(void) {
    need_port("InvalWindowRect", trap_arg(0));
    Q.dirty = true;
}

static void h_qd_flush_port_buffer(void) {
    if (Q.present)
        Q.present();
}

/* BeginFullScreen(Ptr *restoreState, GDHandle gd, short *desiredWidth,
   short *desiredHeight, WindowRef *newWindow, RGBColor *eraseColor, long flags) */
static void h_begin_full_screen(void) {
    uint32_t restore = trap_arg(0), wp = trap_arg(2), hp = trap_arg(3), out = trap_arg(4);
    uint32_t erase = trap_arg(5);
    uint32_t pm = gm_r32(Q.screen_pm);
    qd_rect b = qd_read_rect(pm + PM_BOUNDS);
    Q.saved_w = rect_w(b);
    Q.saved_h = rect_h(b);
    int w = wp ? (int16_t)gm_r16(wp) : 0, h = hp ? (int16_t)gm_r16(hp) : 0;
    if (w > 0 && h > 0 && (w != rect_w(b) || h != rect_h(b))) {
        resize_screen(w, h, gm_r16(pm + PM_PIXEL_SIZE));
        b = qd_read_rect(gm_r32(Q.screen_pm) + PM_BOUNDS);
    }
    if (wp)
        gm_w16(wp, (uint16_t)rect_w(b));
    if (hp)
        gm_w16(hp, (uint16_t)rect_h(b));
    uint32_t win = new_port(Q.screen_pm, b, KIND_WINDOW);
    qd_pixels px;
    qd_palette pal;
    qd_screen(&px, &pal);
    qd_fill(&px, b, b, erase ? read_rgb(erase) : (qd_rgb){0, 0, 0});
    Q.dirty = true;
    if (out)
        gm_w32(out, win);
    if (restore)
        gm_w32(restore, win);
    trap_return(QD_NO_ERR);
}

/* EndFullScreen(Ptr restoreState, long flags): disposes of the window and
   restores the screen size. */
static void h_end_full_screen(void) {
    port_info *info = need_port("EndFullScreen", trap_arg(0));
    dispose_port(info);
    uint32_t pm = gm_r32(Q.screen_pm);
    if (Q.saved_w && Q.saved_h)
        resize_screen(Q.saved_w, Q.saved_h, gm_r16(pm + PM_PIXEL_SIZE));
    trap_return(QD_NO_ERR);
}

/* ---- guest calls: drawing ---- */

static void h_clip_rect(void) {
    uint32_t rgn = gm_r32(Q.cur_port + PORT_CLIP_RGN);
    qd_write_rect(gm_r32(rgn) + 2, qd_read_rect(trap_arg(0)));
}

static void h_rgb_fore_color(void) {
    qd_rgb c = read_rgb(trap_arg(0));
    write_rgb(Q.cur_port + PORT_RGB_FG, c);
}

static void h_paint_rect(void) {
    qd_rect r = qd_read_rect(trap_arg(0));
    qd_pixels px;
    qd_palette pal;
    uint32_t bits = port_pixmap(Q.cur_port);
    qd_bits("PaintRect", bits, &px, &pal);
    qd_fill(&px, r, port_clip("PaintRect", Q.cur_port), read_rgb(Q.cur_port + PORT_RGB_FG));
    if (is_screen_bits(bits))
        Q.dirty = true;
}

/* CopyBits(srcBits, dstBits, srcRect, dstRect, mode, maskRgn) */
static void h_copy_bits(void) {
    uint32_t src_bits = trap_arg(0), dst_bits = trap_arg(1);
    int mode = (int16_t)trap_arg(4);
    if (trap_arg(5))
        trap_crash("CopyBits: mask regions are not supported");
    qd_pixels src, dst;
    qd_palette src_pal, dst_pal;
    qd_bits("CopyBits", src_bits, &src, &src_pal);
    qd_bits("CopyBits", dst_bits, &dst, &dst_pal);
    qd_rect clip = dst.bounds;
    if (dst_bits == port_pixmap(Q.cur_port))
        clip = port_clip("CopyBits", Q.cur_port);
    char err[128];
    if (!qd_blit(&src, qd_read_rect(trap_arg(2)), &dst, qd_read_rect(trap_arg(3)), clip, mode,
                 read_rgb(Q.cur_port + PORT_RGB_FG), read_rgb(Q.cur_port + PORT_RGB_BK), err,
                 sizeof err))
        trap_crash("CopyBits: %s", err);
    if (is_screen_bits(dst_bits))
        Q.dirty = true;
}

static void h_draw_picture(void) {
    uint32_t pic = trap_arg(0);
    if (!mm_is_handle(pic))
        trap_crash("DrawPicture: 0x%08x is not a handle", pic);
    uint32_t len = mm_handle_size(pic);
    const uint8_t *data = gm_ptr(gm_r32(pic), len);
    qd_pixels px;
    qd_palette pal;
    uint32_t bits = port_pixmap(Q.cur_port);
    qd_bits("DrawPicture", bits, &px, &pal);
    char err[128];
    if (!pict_draw(data, len, qd_read_rect(trap_arg(1)), &px, port_clip("DrawPicture", Q.cur_port),
                   read_rgb(Q.cur_port + PORT_RGB_FG), read_rgb(Q.cur_port + PORT_RGB_BK), err,
                   sizeof err))
        trap_crash("DrawPicture: %s", err);
    if (is_screen_bits(bits))
        Q.dirty = true;
}

void qd_register(void) {
    trap_register("SetRect", h_set_rect);
    trap_register("OffsetRect", h_offset_rect);
    trap_register("GetCTable", h_get_ctable);
    trap_register("GetMainDevice", h_get_main_device);
    trap_register("SetDepth", h_set_depth);
    trap_register("NewGWorld", h_new_gworld);
    trap_register("UpdateGWorld", h_update_gworld);
    trap_register("DisposeGWorld", h_dispose_gworld);
    trap_register("GetGWorldPixMap", h_get_gworld_pixmap);
    trap_register("LockPixels", h_lock_pixels);
    trap_register("UnlockPixels", h_unlock_pixels);
    trap_register("GetPixBaseAddr", h_get_pix_base_addr);
    trap_register("SetGWorld", h_set_gworld);
    trap_register("GetGWorld", h_get_gworld);
    trap_register("SetPortWindowPort", h_set_port_window_port);
    trap_register("GetWindowPort", h_get_window_port);
    trap_register("GetPortBounds", h_get_port_bounds);
    trap_register("GetWindowPortBounds", h_get_window_port_bounds);
    trap_register("GetPortBitMapForCopyBits", h_get_port_bitmap_for_copy_bits);
    trap_register("GetQDGlobalsScreenBits", h_get_qd_globals_screen_bits);
    trap_register("ShowWindow", h_show_window);
    trap_register("HideWindow", h_hide_window);
    trap_register("InvalWindowRect", h_inval_window_rect);
    trap_register("QDFlushPortBuffer", h_qd_flush_port_buffer);
    trap_register("BeginFullScreen", h_begin_full_screen);
    trap_register("EndFullScreen", h_end_full_screen);
    trap_register("ClipRect", h_clip_rect);
    trap_register("RGBForeColor", h_rgb_fore_color);
    trap_register("PaintRect", h_paint_rect);
    trap_register("CopyBits", h_copy_bits);
    trap_register("DrawPicture", h_draw_picture);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S . -B build && cmake --build build && ./build/loony_tests qd_`
Expected: `15 passed, 0 failed, 0 skipped`. Full suite: `163 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/qd.h src/qd.c tests/test_qd.c
git commit -m "QuickDraw: screen device, GWorlds, ports, CopyBits and DrawPicture"
```

---

### Task 5: The SDL window and screen dumps

**Files:**
- Modify: `CMakeLists.txt` (link SDL3)
- Modify: `tests/test_main.c` (the dummy video driver for every test)
- Create: `src/display.h`, `src/display.c`
- Create: `tests/test_display.c`

**Interfaces:**
- Consumes: `qd_screen`, `qd_take_dirty` (Task 4); `qd_to_rgba` (Task 2); `png_write_rgba` (Task 1).
- Produces: `void display_init(void)` (reads `LOONY_SCREENSHOT`), `void display_present(void)`, `void display_present_if_dirty(void)`, `bool display_write_png(const char *path)`, `unsigned display_frames(void)`

The window is created on the first present, at 2× scale for screens narrower than 800 and 1× otherwise. Closing it exits cleanly for now; milestone 4 turns a close into the quit Apple Event. If SDL can't start, the run continues without a window (a log line says so), and screen dumps still work.

- [ ] **Step 1: Install SDL3 and write the failing test**

Run: `brew install sdl3 && pkg-config --modversion sdl3`
Expected: `3.x` (3.4.16 at the time of writing).

`tests/test_display.c`:
```c
#include "test.h"

#include <stdlib.h>
#include <unistd.h>

#include "display.h"
#include "guest_mem.h"
#include "memmgr.h"
#include "qd.h"
#include "util.h"

TEST(display_writes_the_screen_as_png) {
    gm_init();
    mm_init();
    qd_init(800, 600, 8);
    const char *t = getenv("TMPDIR");
    char path[1024];
    snprintf(path, sizeof path, "%s/loony-screen-XXXXXX", t && *t ? t : "/tmp");
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
    CHECK(display_write_png(path));
    size_t len;
    uint8_t *f = read_file(path, &len);
    unlink(path);
    CHECK(f != NULL);
    CHECK_EQ(rd_be32(f + 16), 800);
    CHECK_EQ(rd_be32(f + 20), 600);
    free(f);
}

TEST(display_present_works_with_the_dummy_driver) {
    gm_init();
    mm_init();
    qd_init(64, 48, 16);
    unsigned before = display_frames();
    display_present();          /* always presents */
    display_present_if_dirty(); /* qd_init left the screen dirty: presents */
    display_present_if_dirty(); /* nothing new: doesn't */
    CHECK_EQ(display_frames(), before + 2);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: the build fails with `'display.h' file not found`.

- [ ] **Step 3: Write the implementation**

```diff
diff --git a/CMakeLists.txt b/CMakeLists.txt
index 1d9d5fa..e409e46 100644
--- a/CMakeLists.txt
+++ b/CMakeLists.txt
@@ -16,13 +16,14 @@ set(CMAKE_C_FLAGS_RELEASE "-O2")
 
 find_package(PkgConfig REQUIRED)
 pkg_check_modules(UNICORN REQUIRED IMPORTED_TARGET unicorn)
+pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
 
 file(GLOB CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.c)
 list(REMOVE_ITEM CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/main.c)
 
 add_library(loony_core STATIC ${CORE_SOURCES})
 target_include_directories(loony_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
-target_link_libraries(loony_core PUBLIC PkgConfig::UNICORN)
+target_link_libraries(loony_core PUBLIC PkgConfig::UNICORN PkgConfig::SDL3)
 
 add_executable(loony src/main.c)
 target_link_libraries(loony PRIVATE loony_core)
```

```diff
diff --git a/tests/test_main.c b/tests/test_main.c
index 0167d87..4498203 100644
--- a/tests/test_main.c
+++ b/tests/test_main.c
@@ -83,6 +83,8 @@ int test_run_child(void (*fn)(void *), void *arg, char *out, size_t outlen) {
 }
 
 int main(int argc, char **argv) {
+    /* No test may open a real window; children inherit this too. */
+    setenv("SDL_VIDEO_DRIVER", "dummy", 1);
     const char *filter = argc > 1 ? argv[1] : NULL;
     int passed = 0, failed = 0, skipped = 0;
     for (int i = 0; i < ntests; i++) {
```

`src/display.h`:
```c
#pragma once
#include <stdbool.h>

/* Shows the emulated screen (qd_screen) in an SDL window, scaled to fit with
   the right aspect ratio and nearest-neighbor sampling. The window is created
   on the first present. With SDL_VIDEO_DRIVER=dummy nothing appears on
   screen, which the tests use. */

/* If LOONY_SCREENSHOT names a file, the screen is written there as a PNG when
   the process exits, including after a crash. */
void display_init(void);

/* Draws the current screen. Also handles window events: closing the window
   exits cleanly. */
void display_present(void);

/* Writes the current screen as a PNG. */
bool display_write_png(const char *path);

/* Presents only if something drew to the screen since the last present. */
void display_present_if_dirty(void);

/* Number of presents so far. */
unsigned display_frames(void);
```

`src/display.c`:
```c
#include "display.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

#include "png.h"
#include "qd.h"
#include "util.h"

static struct {
    bool sdl_ok, tried;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int tex_w, tex_h;
    unsigned frames;
    const char *screenshot;
} D;

static uint8_t *screen_rgba(int *w, int *h) {
    qd_pixels px;
    qd_palette pal;
    qd_screen(&px, &pal);
    *w = rect_w(px.bounds);
    *h = rect_h(px.bounds);
    uint8_t *rgba = malloc((size_t)*w * (size_t)*h * 4);
    if (!rgba)
        fatal("out of memory");
    qd_to_rgba(&px, rgba);
    return rgba;
}

bool display_write_png(const char *path) {
    int w, h;
    uint8_t *rgba = screen_rgba(&w, &h);
    bool ok = png_write_rgba(path, rgba, w, h);
    free(rgba);
    return ok;
}

static void write_screenshot(void) {
    if (D.screenshot && !display_write_png(D.screenshot))
        log_msg("can't write the screenshot %s", D.screenshot);
}

void display_init(void) {
    D.screenshot = getenv("LOONY_SCREENSHOT");
    if (D.screenshot && *D.screenshot)
        atexit(write_screenshot);
    else
        D.screenshot = NULL;
}

static bool open_window(int w, int h) {
    D.tried = true;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log_msg("display: SDL_Init failed: %s (continuing without a window)", SDL_GetError());
        return false;
    }
    int scale = w < 800 ? 2 : 1;
    D.window = SDL_CreateWindow("Loony Labyrinth", w * scale, h * scale, SDL_WINDOW_RESIZABLE);
    D.renderer = D.window ? SDL_CreateRenderer(D.window, NULL) : NULL;
    if (!D.renderer) {
        log_msg("display: can't create a window: %s (continuing without one)", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(D.renderer, 1);
    return true;
}

void display_present(void) {
    D.frames++;
    int w, h;
    uint8_t *rgba = screen_rgba(&w, &h);
    if (!D.tried)
        D.sdl_ok = open_window(w, h);
    if (D.sdl_ok) {
        if (!D.texture || D.tex_w != w || D.tex_h != h) {
            if (D.texture)
                SDL_DestroyTexture(D.texture);
            D.texture = SDL_CreateTexture(D.renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STREAMING, w, h);
            SDL_SetTextureScaleMode(D.texture, SDL_SCALEMODE_NEAREST);
            SDL_SetRenderLogicalPresentation(D.renderer, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
            D.tex_w = w;
            D.tex_h = h;
        }
        SDL_UpdateTexture(D.texture, NULL, rgba, w * 4);
        SDL_SetRenderDrawColor(D.renderer, 0, 0, 0, 255);
        SDL_RenderClear(D.renderer);
        SDL_RenderTexture(D.renderer, D.texture, NULL, NULL);
        SDL_RenderPresent(D.renderer);
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                log_msg("window closed");
                exit(0);
            }
    }
    free(rgba);
}

void display_present_if_dirty(void) {
    if (qd_take_dirty())
        display_present();
}

unsigned display_frames(void) { return D.frames; }
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S . -B build && cmake --build build && ./build/loony_tests display_`
Expected: `2 passed, 0 failed, 0 skipped`. Full suite: `165 passed`.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt tests/test_main.c src/display.h src/display.c tests/test_display.c
git commit -m "Show the emulated screen in an SDL window; PNG screen dumps"
```

---

### Task 6: Startup alerts

**Files:**
- Create: `src/dialogs.h`, `src/dialogs.c`
- Create: `tests/test_dialogs.c`

**Interfaces:**
- Consumes: `rsrc_find`, `rsrc_data` (Plan 2); `gm_read_pstr` (Plan 2); `trap_*`.
- Produces: `void dialogs_init(void)`, `void dialogs_alert_text(int16_t id, char *out, size_t cap)`, `void dialogs_register(void)` (installs `Alert`, `StopAlert` and `ParamText`)

An `ALRT` resource holds a bounds Rect, the `DITL` ID at offset 8 and the stages word at offset 10. Bit 3 of the first stage picks item 2 as the default, otherwise it's item 1. A `DITL` holds a count minus one, then items: 4 reserved bytes, a Rect, a type byte (bit 7 means disabled), a length byte and the data, padded to even length. Button items are type 4, static text type 8.

- [ ] **Step 1: Write the failing test**

`tests/test_dialogs.c`:
```c
#include "test.h"

#include <stdlib.h>

#include "dialogs.h"
#include "harness.h"
#include "rsrc.h"

static const char *const names[] = {"Alert", "StopAlert", "ParamText"};
static uint8_t *fork_buf;

static bool setup(void) {
    if (!test_game_present())
        return false;
    harness_init(names, 3);
    dialogs_init();
    dialogs_register();
    if (!fork_buf) {
        char path[1100];
        size_t len;
        snprintf(path, sizeof path, "%s/..namedfork/rsrc", test_game_exe_path());
        fork_buf = read_file(path, &len);
        char err[256];
        if (!fork_buf || !rsrc_open(fork_buf, len, err, sizeof err))
            fatal("can't open the resource fork");
    }
    return true;
}

TEST(dialogs_alert_text_lists_buttons_and_text) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    char text[1024];
    dialogs_alert_text(901, text, sizeof text);
    CHECK_CONTAINS(text, "Play Demo | Quit | Buy Now | Enter Key-Code | Thank you for trying");
    dialogs_alert_text(4242, text, sizeof text);
    CHECK_STR(text, "");
}

static void child_alert(void *unused) {
    (void)unused;
    if (!setup())
        exit(3);
    if (call_import("Alert", 2, 901u, 0u) != 1)
        exit(4);
    if (call_import("StopAlert", 2, 900u, 0u) != 1)
        exit(5);
}

TEST(dialogs_alert_answers_the_default_item_and_logs) {
    SKIP_UNLESS_GAME();
    char out[16384];
    CHECK_EQ(test_run_child(child_alert, NULL, out, sizeof out), 0);
    CHECK_CONTAINS(out, "loony: Alert 901 (answering item 1): Play Demo");
    CHECK_CONTAINS(out, "loony: Alert 900 (answering item 1): OK");
}

TEST(dialogs_param_text_substitutes) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    uint32_t s[4];
    const char *v[4] = {"A", "BB", "", "D"};
    for (int i = 0; i < 4; i++) {
        s[i] = scratch(16);
        gm_write_pstr(s[i], v[i]);
    }
    call_import("ParamText", 4, s[0], s[1], s[2], s[3]);
    char text[1024];
    dialogs_alert_text(9000, text, sizeof text);
    CHECK_CONTAINS(text, "ABBD");
    call_import("ParamText", 4, 0u, 0u, 0u, 0u);
    dialogs_alert_text(9000, text, sizeof text);
    CHECK_STR(text, "OK | ");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: the build fails with `'dialogs.h' file not found`.

- [ ] **Step 3: Write the implementation**

`src/dialogs.h`:
```c
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Dialog Manager, for now only what startup needs. Alert and StopAlert don't
   draw anything yet (that needs the dialog work in milestone 6): they log the
   alert's text, with ParamText substitutions, and return its default item,
   as if the user pressed Return. */

/* Resets ParamText. */
void dialogs_init(void);

/* The text Alert would show for ALRT id, items joined with " | ". Empty if
   the alert doesn't exist. */
void dialogs_alert_text(int16_t id, char *out, size_t cap);

/* Registers Alert, StopAlert and ParamText. */
void dialogs_register(void);
```

`src/dialogs.c`:
```c
#include "dialogs.h"

#include <stdio.h>
#include <string.h>

#include "guest_mem.h"
#include "rsrc.h"
#include "trap.h"
#include "util.h"

#define ITEM_STATIC_TEXT 8
#define ITEM_BUTTON 4

static char param[4][256]; /* ParamText ^0..^3 */

void dialogs_init(void) { memset(param, 0, sizeof param); }

/* Appends text to out, replacing ^0..^3 with the ParamText strings and
   carriage returns with spaces. */
static void append(char *out, size_t cap, const uint8_t *text, size_t n) {
    size_t o = strlen(out);
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        if (text[i] == '^' && i + 1 < n && text[i + 1] >= '0' && text[i + 1] <= '3') {
            const char *p = param[text[++i] - '0'];
            while (*p && o + 1 < cap)
                out[o++] = *p++;
        } else {
            out[o++] = text[i] == '\r' ? ' ' : (char)text[i];
        }
    }
    out[o] = '\0';
}

void dialogs_alert_text(int16_t id, char *out, size_t cap) {
    out[0] = '\0';
    rsrc_entry *alrt = rsrc_find(FOURCC('A', 'L', 'R', 'T'), id);
    if (!alrt || alrt->len < 12)
        return;
    rsrc_entry *ditl = rsrc_find(FOURCC('D', 'I', 'T', 'L'), (int16_t)rd_be16(rsrc_data(alrt) + 8));
    if (!ditl || ditl->len < 2)
        return;
    const uint8_t *d = rsrc_data(ditl);
    uint32_t n = rd_be16(d) + 1u, p = 2;
    for (uint32_t i = 0; i < n; i++) {
        if (p + 14 > ditl->len)
            return;
        uint8_t type = d[p + 12] & 0x7F, len = d[p + 13];
        p += 14;
        if (p + len > ditl->len)
            return;
        if (type == ITEM_STATIC_TEXT || type == ITEM_BUTTON) {
            if (out[0])
                append(out, cap, (const uint8_t *)" | ", 3);
            append(out, cap, d + p, len);
        }
        p += len + (len & 1u);
    }
}

/* The default item: bit 3 of the first stage's 4 bits in the ALRT's stages
   word picks item 2, otherwise item 1. */
static void h_alert(void) {
    int16_t id = (int16_t)trap_arg(0);
    rsrc_entry *alrt = rsrc_find(FOURCC('A', 'L', 'R', 'T'), id);
    if (!alrt || alrt->len < 12)
        trap_crash("Alert: ALRT %d doesn't exist", id);
    uint16_t stages = rd_be16(rsrc_data(alrt) + 10);
    int item = (stages & 0x8) ? 2 : 1;
    char text[1024];
    dialogs_alert_text(id, text, sizeof text);
    log_msg("Alert %d (answering item %d): %s", id, item, text);
    trap_return((uint32_t)item);
}

static void h_param_text(void) {
    for (int i = 0; i < 4; i++) {
        uint32_t s = trap_arg(i);
        if (s)
            gm_read_pstr(s, param[i]);
        else
            param[i][0] = '\0';
    }
}

void dialogs_register(void) {
    trap_register("Alert", h_alert);
    trap_register("StopAlert", h_alert);
    trap_register("ParamText", h_param_text);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S . -B build && cmake --build build && ./build/loony_tests dialogs_`
Expected: `3 passed, 0 failed, 0 skipped`. Full suite: `168 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/dialogs.h src/dialogs.c tests/test_dialogs.c
git commit -m "Answer startup alerts with their default item"
```

---

### Task 7: Reading the game's data file

**Files:**
- Create: `src/files.h`, `src/files.c`
- Create: `tests/test_files.c`

**Interfaces:**
- Consumes: `gm_read_pstr`, `gm_write_pstr`, `gm_ptr` (Plan 2); `trap_*`.
- Produces:
  - `FILES_VREFNUM` (-1), `FILES_ROOT_DIRID` (2), the error codes `FILES_FNF_ERR` (-43), `FILES_DIR_NF_ERR` (-120), `FILES_EOF_ERR` (-39), `FILES_RF_NUM_ERR` (-51), `FILES_POS_ERR` (-40), `FILES_BD_NAM_ERR` (-37), and `FSSPEC_SIZE` (70)
  - `void files_init(const char *game_dir)`, `void files_mac_to_utf8(const char *mac, char *out, size_t cap)`, `void files_register(void)` (installs `FSMakeFSSpec`, `FSpOpenDF`, `PBReadSync`, `GetEOF`, `SetFPos`, `GetFPos`, `FSClose`)

An FSSpec is a vRefNum (2 bytes), a parID (4) and a Str63 name (64). A ParamBlockRec for `PBReadSync` has ioResult at +16, ioRefNum at +24, ioBuffer at +32, ioReqCount at +36, ioActCount at +40, ioPosMode at +44 and ioPosOffset at +46. Positioning modes are 0 at mark, 1 from start, 2 from end of file and 3 from mark. Only the low two bits are used, and newline mode (0x80) fails loudly.

- [ ] **Step 1: Write the failing test**

`tests/test_files.c`:
```c
#include "test.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "files.h"
#include "harness.h"

static const char *const names[] = {
    "FSMakeFSSpec", "FSpOpenDF", "PBReadSync", "GetEOF", "SetFPos", "GetFPos", "FSClose",
};

static char dir[1024];

static void write_file(const char *rel, const char *text) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    FILE *f = fopen(p, "wb");
    fputs(text, f);
    fclose(f);
}

/* A fresh game folder: "LL Data/effect.bin" and "Café" (Mac Roman "Caf\x8e"). */
static void setup(void) {
    const char *t = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/loony-files-XXXXXX", t && *t ? t : "/tmp");
    if (!mkdtemp(dir))
        fatal("mkdtemp failed");
    char sub[1100];
    snprintf(sub, sizeof sub, "%s/LL Data", dir);
    mkdir(sub, 0755);
    write_file("LL Data/effect.bin", "hello world");
    write_file("Caf\xc3\xa9", "x");
    harness_init(names, sizeof names / sizeof names[0]);
    files_init(dir);
    files_register();
}

static void teardown(void) {
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "can't remove %s\n", dir);
}

static int16_t make_spec(const char *mac, uint32_t spec) {
    uint32_t name = scratch(256);
    gm_write_pstr(name, mac);
    return (int16_t)call_import("FSMakeFSSpec", 4, 0u, 0u, name, spec);
}

TEST(files_mac_roman_names_become_utf8) {
    char out[64];
    files_mac_to_utf8("Caf\x8e", out, sizeof out);
    CHECK_STR(out, "Caf\xc3\xa9");
    files_mac_to_utf8("a/b", out, sizeof out);
    CHECK_STR(out, "a:b");
    files_mac_to_utf8("\xaa", out, sizeof out); /* trademark sign, U+2122 */
    CHECK_STR(out, "\xe2\x84\xa2");
}

TEST(files_make_fsspec_resolves_relative_paths) {
    setup();
    uint32_t spec = scratch(FSSPEC_SIZE);
    CHECK_EQ(make_spec(":LL Data:effect.bin", spec), 0);
    CHECK_EQ((int16_t)gm_r16(spec), FILES_VREFNUM);
    CHECK(gm_r32(spec + 2) != FILES_ROOT_DIRID);
    char name[256];
    gm_read_pstr(spec + 6, name);
    CHECK_STR(name, "effect.bin");
    CHECK_EQ(make_spec("Caf\x8e", spec), 0);
    CHECK_EQ(gm_r32(spec + 2), FILES_ROOT_DIRID);
    CHECK_EQ(make_spec(":LL Data:nope", spec), FILES_FNF_ERR);
    gm_read_pstr(spec + 6, name);
    CHECK_STR(name, "nope"); /* the spec is still filled in */
    CHECK_EQ(make_spec(":Missing:x", spec), FILES_DIR_NF_ERR);
    teardown();
}

TEST(files_read_a_file) {
    setup();
    uint32_t spec = scratch(FSSPEC_SIZE), ref = scratch(2), eof = scratch(4);
    CHECK_EQ(make_spec(":LL Data:effect.bin", spec), 0);
    CHECK_EQ(call_import("FSpOpenDF", 3, spec, 1u, ref), 0);
    uint16_t r = gm_r16(ref);
    CHECK_EQ(call_import("GetEOF", 2, (uint32_t)r, eof), 0);
    CHECK_EQ(gm_r32(eof), 11);
    uint32_t pb = scratch(80), buf = scratch(16);
    gm_w16(pb + 24, r);
    gm_w32(pb + 32, buf);
    gm_w32(pb + 36, 5);
    gm_w16(pb + 44, 1); /* fsFromStart */
    gm_w32(pb + 46, 6);
    CHECK_EQ(call_import("PBReadSync", 1, pb), 0);
    CHECK_EQ(gm_r32(pb + 40), 5);
    CHECK_EQ(gm_r32(pb + 46), 11);
    char s[16];
    memcpy(s, gm_ptr(buf, 5), 5);
    s[5] = '\0';
    CHECK_STR(s, "world");
    gm_w16(pb + 44, 0); /* fsAtMark: nothing left */
    CHECK_EQ((int16_t)call_import("PBReadSync", 1, pb), FILES_EOF_ERR);
    CHECK_EQ(gm_r32(pb + 40), 0);
    CHECK_EQ((int16_t)gm_r16(pb + 16), FILES_EOF_ERR);
    CHECK_EQ(call_import("SetFPos", 3, (uint32_t)r, 1u, 2u), 0);
    uint32_t pos = scratch(4);
    CHECK_EQ(call_import("GetFPos", 2, (uint32_t)r, pos), 0);
    CHECK_EQ(gm_r32(pos), 2);
    CHECK_EQ((int16_t)call_import("SetFPos", 3, (uint32_t)r, 1u, 99u), FILES_EOF_ERR);
    CHECK_EQ(call_import("FSClose", 1, (uint32_t)r), 0);
    CHECK_EQ((int16_t)call_import("FSClose", 1, (uint32_t)r), FILES_RF_NUM_ERR);
    CHECK_EQ((int16_t)call_import("GetEOF", 2, (uint32_t)r, eof), FILES_RF_NUM_ERR);
    teardown();
}

TEST(files_open_missing_file) {
    setup();
    uint32_t spec = scratch(FSSPEC_SIZE), ref = scratch(2);
    make_spec("nope", spec);
    CHECK_EQ((int16_t)call_import("FSpOpenDF", 3, spec, 1u, ref), FILES_FNF_ERR);
    teardown();
}

static void child_open_for_writing(void *unused) {
    (void)unused;
    setup();
    uint32_t spec = scratch(FSSPEC_SIZE), ref = scratch(2);
    make_spec(":LL Data:effect.bin", spec);
    call_import("FSpOpenDF", 3, spec, 3u, ref);
}

TEST(files_writing_is_not_supported_yet) {
    char out[16384];
    CHECK_EQ(test_run_child(child_open_for_writing, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "FSpOpenDF: opening files for writing (permission 3) is not supported yet");
    teardown();
}

static void child_full_path(void *unused) {
    (void)unused;
    setup();
    make_spec("Macintosh HD:x", scratch(FSSPEC_SIZE));
}

TEST(files_full_paths_crash) {
    char out[16384];
    CHECK_EQ(test_run_child(child_full_path, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "full path names");
    teardown();
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: the build fails with `'files.h' file not found`.

- [ ] **Step 3: Write the implementation**

`src/files.h`:
```c
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* File Manager, read side: FSSpecs and data-fork reads from the game folder.
   Writing files arrives with milestone 6; until then calls that would write
   crash with a report.

   There is one fake volume (FILES_VREFNUM). Directory IDs are handed out per
   folder: FILES_ROOT_DIRID is the game folder, which is also the default
   directory (vRefNum 0, dirID 0). Mac paths use ':' separators and Mac Roman
   names; host paths use '/' and UTF-8. */

#define FILES_VREFNUM    (-1)
#define FILES_ROOT_DIRID 2
#define FILES_FNF_ERR    (-43)
#define FILES_DIR_NF_ERR (-120)
#define FILES_EOF_ERR    (-39)
#define FILES_RF_NUM_ERR (-51)
#define FILES_POS_ERR    (-40)
#define FILES_BD_NAM_ERR (-37)

/* FSSpec: vRefNum (2), parID (4), name (Str63, 64 bytes). */
#define FSSPEC_SIZE 70

/* Sets the game folder (read-only). Closes open files and forgets directory IDs. */
void files_init(const char *game_dir);

/* Converts a Mac Roman name to UTF-8, with '/' (legal in Mac names) becoming ':'. */
void files_mac_to_utf8(const char *mac, char *out, size_t cap);

/* Registers FSMakeFSSpec, FSpOpenDF, PBReadSync, GetEOF, SetFPos, GetFPos and FSClose. */
void files_register(void);
```

`src/files.c`:
```c
#include "files.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "guest_mem.h"
#include "trap.h"
#include "util.h"

#define MAX_DIRS 64
#define MAX_FILES 16
#define FIRST_REFNUM 20

/* ParamBlockRec (IOParam) offsets. */
#define PB_RESULT     16
#define PB_REFNUM     24
#define PB_BUFFER     32
#define PB_REQ_COUNT  36
#define PB_ACT_COUNT  40
#define PB_POS_MODE   44
#define PB_POS_OFFSET 46

/* Positioning modes. */
#define FS_AT_MARK    0
#define FS_FROM_START 1
#define FS_FROM_LEOF  2
#define FS_FROM_MARK  3

static struct {
    char game_dir[1024];
    char dirs[MAX_DIRS][512]; /* relative host path of each directory ID - 2 ("" = game folder) */
    int ndirs;
    struct {
        FILE *f;
        long eof;
    } files[MAX_FILES];
} F;

/* Unicode code points for Mac Roman 0x80-0xFF. */
static const uint16_t mac_roman[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3,
    0x00E5, 0x00E7, 0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
    0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3,
    0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA,
    0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
    0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D,
    0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE,
    0x00CF, 0x00CC, 0x00D3, 0x00D4, 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
    0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

void files_mac_to_utf8(const char *mac, char *out, size_t cap) {
    size_t o = 0;
    for (const uint8_t *p = (const uint8_t *)mac; *p && o + 4 < cap; p++) {
        uint32_t c = *p < 0x80 ? *p : mac_roman[*p - 0x80];
        if (c == '/')
            c = ':';
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = '\0';
}

void files_init(const char *game_dir) {
    for (int i = 0; i < MAX_FILES; i++)
        if (F.files[i].f)
            fclose(F.files[i].f);
    memset(&F, 0, sizeof F);
    snprintf(F.game_dir, sizeof F.game_dir, "%s", game_dir);
    F.ndirs = 1; /* ID 2: the game folder */
}

static const char *dir_path(uint32_t id) {
    if (id < FILES_ROOT_DIRID || id - FILES_ROOT_DIRID >= (uint32_t)F.ndirs)
        return NULL;
    return F.dirs[id - FILES_ROOT_DIRID];
}

static uint32_t dir_id(const char *rel) {
    for (int i = 0; i < F.ndirs; i++)
        if (strcmp(F.dirs[i], rel) == 0)
            return FILES_ROOT_DIRID + (uint32_t)i;
    if (F.ndirs == MAX_DIRS)
        trap_crash("more than %d directories", MAX_DIRS);
    snprintf(F.dirs[F.ndirs], sizeof F.dirs[0], "%s", rel);
    return FILES_ROOT_DIRID + (uint32_t)F.ndirs++;
}

static void host_path(const char *rel, char *out, size_t cap) {
    snprintf(out, cap, "%s%s%s", F.game_dir, *rel ? "/" : "", rel);
}

static bool is_dir(const char *rel) {
    char p[1600];
    host_path(rel, p, sizeof p);
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Joins a relative directory and a UTF-8 component. */
static void join(const char *dir, const char *name, char *out, size_t cap) {
    snprintf(out, cap, "%s%s%s", dir, *dir ? "/" : "", name);
}

/* FSMakeFSSpec(vRefNum, dirID, fileName, FSSpec *spec) -> OSErr */
static void h_fs_make_fsspec(void) {
    int16_t vref = (int16_t)trap_arg(0);
    uint32_t dir = trap_arg(1), name_p = trap_arg(2), spec = trap_arg(3);
    if (vref != 0 && vref != FILES_VREFNUM)
        trap_crash("FSMakeFSSpec: unknown volume %d", vref);
    if (dir == 0)
        dir = FILES_ROOT_DIRID;
    const char *base = dir_path(dir);
    if (!base)
        trap_crash("FSMakeFSSpec: unknown directory ID %u", dir);
    char mac[256];
    gm_read_pstr(name_p, mac);
    /* Walk ':'-separated components; a leading ':' means relative. */
    char rel[512], comp[256], utf[512];
    snprintf(rel, sizeof rel, "%s", base);
    const char *p = mac[0] == ':' ? mac + 1 : mac;
    if (strchr(mac, ':') && mac[0] != ':')
        trap_crash("FSMakeFSSpec: full path names (\"%s\") are not supported", mac);
    for (;;) {
        const char *colon = strchr(p, ':');
        size_t n = colon ? (size_t)(colon - p) : strlen(p);
        if (n == 0 || n > 63)
            trap_crash("FSMakeFSSpec: bad path \"%s\"", mac);
        memcpy(comp, p, n);
        comp[n] = '\0';
        if (!colon)
            break;
        files_mac_to_utf8(comp, utf, sizeof utf);
        char next[512];
        join(rel, utf, next, sizeof next);
        if (!is_dir(next)) {
            trap_return((uint32_t)FILES_DIR_NF_ERR);
            return;
        }
        snprintf(rel, sizeof rel, "%s", next);
        p = colon + 1;
    }
    gm_w16(spec, (uint16_t)FILES_VREFNUM);
    gm_w32(spec + 2, dir_id(rel));
    memset(gm_ptr(spec + 6, 64), 0, 64);
    gm_write_pstr(spec + 6, comp);
    files_mac_to_utf8(comp, utf, sizeof utf);
    char file_rel[1024], hp[1600];
    join(rel, utf, file_rel, sizeof file_rel);
    host_path(file_rel, hp, sizeof hp);
    struct stat st;
    trap_return(stat(hp, &st) == 0 ? 0 : (uint32_t)FILES_FNF_ERR);
}

static void spec_host_path(uint32_t spec, char *out, size_t cap) {
    uint32_t dir = gm_r32(spec + 2);
    const char *base = dir_path(dir);
    if (!base)
        trap_crash("FSSpec has an unknown directory ID %u", dir);
    char mac[256], utf[512], rel[1024];
    gm_read_pstr(spec + 6, mac);
    files_mac_to_utf8(mac, utf, sizeof utf);
    join(base, utf, rel, sizeof rel);
    host_path(rel, out, cap);
}

/* FSpOpenDF(const FSSpec *spec, SInt8 permission, short *refNum) -> OSErr */
static void h_fsp_open_df(void) {
    uint32_t spec = trap_arg(0), out = trap_arg(2);
    int perm = (int8_t)trap_arg(1);
    if (perm != 0 && perm != 1)
        trap_crash("FSpOpenDF: opening files for writing (permission %d) is not supported yet", perm);
    char path[1600];
    spec_host_path(spec, path, sizeof path);
    int slot = 0;
    while (slot < MAX_FILES && F.files[slot].f)
        slot++;
    if (slot == MAX_FILES)
        trap_crash("FSpOpenDF: more than %d open files", MAX_FILES);
    FILE *f = fopen(path, "rb");
    if (!f) {
        trap_return((uint32_t)FILES_FNF_ERR);
        return;
    }
    fseek(f, 0, SEEK_END);
    F.files[slot].f = f;
    F.files[slot].eof = ftell(f);
    fseek(f, 0, SEEK_SET);
    gm_w16(out, (uint16_t)(FIRST_REFNUM + slot));
    trap_return(0);
}

static FILE *file_of(int16_t ref, long *eof) {
    int slot = ref - FIRST_REFNUM;
    if (slot < 0 || slot >= MAX_FILES || !F.files[slot].f)
        return NULL;
    *eof = F.files[slot].eof;
    return F.files[slot].f;
}

/* Moves the mark; returns an OSErr. */
static int16_t set_pos(FILE *f, long eof, int mode, int32_t off) {
    long base;
    switch (mode & 3) {
    case FS_AT_MARK: return 0;
    case FS_FROM_START: base = 0; break;
    case FS_FROM_LEOF: base = eof; break;
    default: base = ftell(f); break;
    }
    long pos = base + off;
    if (pos < 0)
        return FILES_POS_ERR;
    if (pos > eof) {
        fseek(f, eof, SEEK_SET);
        return FILES_EOF_ERR;
    }
    fseek(f, pos, SEEK_SET);
    return 0;
}

/* PBReadSync(ParmBlkPtr) -> OSErr */
static void h_pb_read_sync(void) {
    uint32_t pb = trap_arg(0);
    long eof;
    FILE *f = file_of((int16_t)gm_r16(pb + PB_REFNUM), &eof);
    int16_t err = 0;
    uint32_t got = 0;
    if (!f) {
        err = FILES_RF_NUM_ERR;
    } else {
        int mode = (int16_t)gm_r16(pb + PB_POS_MODE);
        if (mode & 0x80)
            trap_crash("PBReadSync: newline mode is not supported");
        err = set_pos(f, eof, mode, (int32_t)gm_r32(pb + PB_POS_OFFSET));
        int32_t want = (int32_t)gm_r32(pb + PB_REQ_COUNT);
        if (!err && want > 0) {
            uint8_t *buf = gm_ptr(gm_r32(pb + PB_BUFFER), (uint32_t)want);
            got = (uint32_t)fread(buf, 1, (size_t)want, f);
            if (got < (uint32_t)want)
                err = FILES_EOF_ERR;
        }
        gm_w32(pb + PB_POS_OFFSET, (uint32_t)ftell(f));
    }
    gm_w32(pb + PB_ACT_COUNT, got);
    gm_w16(pb + PB_RESULT, (uint16_t)err);
    trap_return((uint32_t)(int32_t)err);
}

static void h_get_eof(void) {
    long eof;
    FILE *f = file_of((int16_t)trap_arg(0), &eof);
    if (!f) {
        trap_return((uint32_t)FILES_RF_NUM_ERR);
        return;
    }
    gm_w32(trap_arg(1), (uint32_t)eof);
    trap_return(0);
}

static void h_set_fpos(void) {
    long eof;
    FILE *f = file_of((int16_t)trap_arg(0), &eof);
    if (!f) {
        trap_return((uint32_t)FILES_RF_NUM_ERR);
        return;
    }
    trap_return((uint32_t)(int32_t)set_pos(f, eof, (int16_t)trap_arg(1), (int32_t)trap_arg(2)));
}

static void h_get_fpos(void) {
    long eof;
    FILE *f = file_of((int16_t)trap_arg(0), &eof);
    if (!f) {
        trap_return((uint32_t)FILES_RF_NUM_ERR);
        return;
    }
    gm_w32(trap_arg(1), (uint32_t)ftell(f));
    trap_return(0);
}

static void h_fs_close(void) {
    int16_t ref = (int16_t)trap_arg(0);
    long eof;
    FILE *f = file_of(ref, &eof);
    if (!f) {
        trap_return((uint32_t)FILES_RF_NUM_ERR);
        return;
    }
    fclose(f);
    F.files[ref - FIRST_REFNUM].f = NULL;
    trap_return(0);
}

void files_register(void) {
    trap_register("FSMakeFSSpec", h_fs_make_fsspec);
    trap_register("FSpOpenDF", h_fsp_open_df);
    trap_register("PBReadSync", h_pb_read_sync);
    trap_register("GetEOF", h_get_eof);
    trap_register("SetFPos", h_set_fpos);
    trap_register("GetFPos", h_get_fpos);
    trap_register("FSClose", h_fs_close);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S . -B build && cmake --build build && ./build/loony_tests files_`
Expected: `6 passed, 0 failed, 0 skipped`. Full suite: `174 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/files.h src/files.c tests/test_files.c
git commit -m "FSSpec and data-fork reads from the game folder"
```

---

### Task 8: A finer clock and an idle hook in `Delay` and `TickCount`

**Files:**
- Modify: `src/misc.h`, `src/misc.c`
- Modify: `tests/test_misc.c` (append two tests)

**Interfaces:**
- Consumes: Plan 2's `misc` module.
- Produces: `double misc_seconds(void)`, `typedef void (*misc_idle_fn)(void)`, `void misc_set_idle(misc_idle_fn fn)`. `misc_init` keeps the hook. `Delay` now sleeps in quarter-tick steps and runs the hook once per tick; `TickCount` runs it when the tick has changed.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_misc.c`:
```diff
diff --git a/tests/test_misc.c b/tests/test_misc.c
index 2dd9b4b..0204aad 100644
--- a/tests/test_misc.c
+++ b/tests/test_misc.c
@@ -191,3 +191,30 @@ TEST(misc_exit_to_shell_exits_cleanly) {
     CHECK_EQ(status, 0);
     CHECK_CONTAINS(out, "loony: ExitToShell");
 }
+
+static int idle_calls;
+static void count_idle(void) { idle_calls++; }
+
+TEST(misc_seconds_has_sub_tick_resolution) {
+    setup();
+    double a = misc_seconds();
+    struct timespec ts = {0, 2000000};
+    nanosleep(&ts, NULL);
+    double b = misc_seconds();
+    CHECK(b - a >= 0.002);
+    CHECK(b - a < 0.5);
+}
+
+TEST(misc_delay_and_tick_count_run_the_idle_hook) {
+    setup();
+    idle_calls = 0;
+    misc_set_idle(count_idle);
+    call_import("Delay", 2, 3u, 0u);
+    int after_delay = idle_calls;
+    for (int i = 0; i < 1000; i++)
+        call_import("TickCount", 0);
+    misc_set_idle(NULL);
+    CHECK(after_delay >= 3); /* once per tick while waiting */
+    CHECK(after_delay <= 5);
+    CHECK(idle_calls - after_delay <= 2); /* TickCount only when the tick changes */
+}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build`
Expected: the build fails with `call to undeclared function 'misc_seconds'`.

- [ ] **Step 3: Write the implementation**

```diff
diff --git a/src/misc.h b/src/misc.h
index 1e32e0b..f7dd29e 100644
--- a/src/misc.h
+++ b/src/misc.h
@@ -11,6 +11,9 @@ void misc_init(void);
 /* Ticks (1/60 s) since misc_init(). */
 uint32_t misc_ticks(void);
 
+/* Seconds since misc_init(), with microsecond resolution. */
+double misc_seconds(void);
+
 /* False while HideCursor has hidden the cursor (until InitCursor). */
 bool misc_cursor_visible(void);
 
@@ -19,6 +22,12 @@ bool misc_cursor_visible(void);
 bool misc_ae_handler(uint32_t event_class, uint32_t event_id, uint32_t *handler,
                      uint32_t *refcon);
 
+/* Called by Delay, and by TickCount whenever the tick count has changed,
+   so a game waiting in its own loop still pumps events, sound and the
+   screen. */
+typedef void (*misc_idle_fn)(void);
+void misc_set_idle(misc_idle_fn fn);
+
 /* Registers Gestalt, time, string, cursor, Apple Event, Internet Config,
    KeyScript, GetMBarHeight, BlockMoveData and ExitToShell imports. */
 void misc_register(void);
```

```diff
diff --git a/src/misc.c b/src/misc.c
index 2c09e9a..5ddeaf0 100644
--- a/src/misc.c
+++ b/src/misc.c
@@ -21,10 +21,16 @@ static struct {
         uint32_t event_class, event_id, handler, refcon;
     } ae[MAX_AE_HANDLERS];
     int nae;
+    misc_idle_fn idle;
+    uint32_t last_idle_tick;
 } M;
 
+void misc_set_idle(misc_idle_fn fn) { M.idle = fn; }
+
 void misc_init(void) {
+    misc_idle_fn idle = M.idle;
     memset(&M, 0, sizeof M);
+    M.idle = idle;
     clock_gettime(CLOCK_MONOTONIC, &M.start);
 }
 
@@ -38,6 +44,8 @@ static uint64_t elapsed_us(void) {
 
 uint32_t misc_ticks(void) { return (uint32_t)(elapsed_us() * 60 / 1000000); }
 
+double misc_seconds(void) { return (double)elapsed_us() / 1e6; }
+
 bool misc_cursor_visible(void) { return M.cursor_level == 0; }
 
 bool misc_ae_handler(uint32_t event_class, uint32_t event_id, uint32_t *handler,
@@ -87,7 +95,18 @@ static void h_gestalt(void) {
 
 /* ---- time ---- */
 
-static void h_tick_count(void) { trap_return(misc_ticks()); }
+static void idle_if_new_tick(uint32_t t) {
+    if (M.idle && t != M.last_idle_tick) {
+        M.last_idle_tick = t;
+        M.idle();
+    }
+}
+
+static void h_tick_count(void) {
+    uint32_t t = misc_ticks();
+    idle_if_new_tick(t);
+    trap_return(t);
+}
 
 static void h_microseconds(void) {
     uint64_t us = elapsed_us();
@@ -96,11 +115,17 @@ static void h_microseconds(void) {
     gm_w32(out + 4, (uint32_t)us);
 }
 
+/* Delay(ticks, &finalTicks): sleeps in steps of at most one tick, running
+   the idle hook between steps. */
 static void h_delay(void) {
     uint32_t ticks = trap_arg(0), final_ticks = trap_arg(1);
-    if ((int32_t)ticks > 0) {
-        uint64_t us = (uint64_t)ticks * 1000000 / 60;
-        struct timespec ts = {(time_t)(us / 1000000), (long)(us % 1000000) * 1000};
+    uint32_t end = misc_ticks() + ((int32_t)ticks > 0 ? ticks : 0);
+    for (;;) {
+        uint32_t t = misc_ticks();
+        idle_if_new_tick(t);
+        if (t >= end)
+            break;
+        struct timespec ts = {0, 1000000000L / 60 / 4};
         nanosleep(&ts, NULL);
     }
     if (final_ticks)
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ./build/loony_tests misc_`
Expected: `14 passed, 0 failed, 0 skipped`. Full suite: `176 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/misc.h src/misc.c tests/test_misc.c
git commit -m "Microsecond clock; Delay and TickCount run an idle hook"
```

---

### Task 9: A silent Sound Manager

**Files:**
- Create: `src/sound.h`, `src/sound.c`
- Create: `tests/asm.h` (PowerPC encoders for hand-written guest code in tests)
- Create: `tests/test_sound.c`

**Interfaces:**
- Consumes: `misc_seconds` (Task 8); `mm_new_ptr`, `mm_dispose_ptr` (Plan 2); `guest_call`, `trap_*`.
- Produces:
  - The `SND_*_CMD` numbers, `SND_QUEUE_FULL_ERR`, `SND_BAD_CHANNEL_ERR`
  - `void sound_init(void)`, `void sound_pump(void)`, `double sound_header_seconds(uint32_t header)`, `void sound_register(void)` (installs `NewSndCallBackUPP`, `SndNewChannel`, `SndDisposeChannel`, `SndDoCommand`, `SndDoImmediate`, `SndChannelStatus`)
  - `tests/asm.h`: `ppc_lis`, `ppc_ori`, `ppc_lwz`, `ppc_stw`, `ppc_addi`, `ppc_cmpwi`, `ppc_blt` and the `PPC_*` constants

A SndChannel is allocated by `SndNewChannel` when `*chan` is NULL. Its callBack is at +8 and cmdInProgress at +20. A callback is called as `proc(chan, &cmd)`, with the command copied into cmdInProgress. Sound header frame counts:
- **stdSH** (encode 0): the length field at +4.
- **extSH/cmpSH** (0xFF/0xFE): the field at +22.

The rate is Fixed 16.16 at +8.

- [ ] **Step 1: Write the failing test**

`tests/asm.h`:
```c
#pragma once
/* A few PowerPC instruction encoders for hand-written guest code in tests. */
#include <stdint.h>

static inline uint32_t ppc_lis(int rd, uint32_t imm) { return 0x3C000000u | (uint32_t)rd << 21 | (imm & 0xFFFF); }
static inline uint32_t ppc_ori(int ra, int rs, uint32_t imm) {
    return 0x60000000u | (uint32_t)rs << 21 | (uint32_t)ra << 16 | (imm & 0xFFFF);
}
static inline uint32_t ppc_lwz(int rd, int16_t d, int ra) {
    return 0x80000000u | (uint32_t)rd << 21 | (uint32_t)ra << 16 | (uint16_t)d;
}
static inline uint32_t ppc_stw(int rs, int16_t d, int ra) {
    return 0x90000000u | (uint32_t)rs << 21 | (uint32_t)ra << 16 | (uint16_t)d;
}
static inline uint32_t ppc_addi(int rd, int ra, int16_t imm) {
    return 0x38000000u | (uint32_t)rd << 21 | (uint32_t)ra << 16 | (uint16_t)imm;
}
static inline uint32_t ppc_cmpwi(int ra, int16_t imm) { return 0x2C000000u | (uint32_t)ra << 16 | (uint16_t)imm; }
/* blt with a byte offset relative to this instruction. */
static inline uint32_t ppc_blt(int16_t off) { return 0x41800000u | ((uint16_t)off & 0xFFFC); }

#define PPC_MFLR_R0   0x7C0802A6u
#define PPC_MTLR_R0   0x7C0803A6u
#define PPC_MTCTR_R0  0x7C0903A6u
#define PPC_BCTRL     0x4E800421u
#define PPC_BLR       0x4E800020u
#define PPC_SAVE_LR   0x90010008u /* stw r0,8(r1) */
#define PPC_LOAD_LR   0x80010008u /* lwz r0,8(r1) */
#define PPC_PUSH64    0x9421FFC0u /* stwu r1,-64(r1) */
#define PPC_POP64     0x38210040u /* addi r1,r1,64 */
```

`tests/test_sound.c`:
```c
#include "test.h"

#include <stdlib.h>
#include <time.h>

#include "asm.h"
#include "harness.h"
#include "memmgr.h"
#include "misc.h"
#include "sound.h"

static const char *const names[] = {
    "NewSndCallBackUPP", "SndNewChannel", "SndDisposeChannel", "SndDoCommand", "SndDoImmediate",
    "SndChannelStatus",
};

#define CB      (GUEST_IMAGE_BASE + 0x200)
#define TV_CB   (GUEST_IMAGE_BASE + 0x8100)
#define GOT     (GUEST_IMAGE_BASE + 0x8200)

/* A callback that stores cmd->param2 at GOT. */
static void emit_callback(void) {
    uint32_t code[] = {
        ppc_lwz(5, 4, 4), ppc_lis(6, GOT >> 16), ppc_ori(6, 6, GOT & 0xFFFF), ppc_stw(5, 0, 6),
        PPC_BLR,
    };
    put_words(CB, code, 5);
    gm_w32(TV_CB, CB);
    gm_w32(TV_CB + 4, 0);
    gm_w32(GOT, 0);
}

static void setup(void) {
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    misc_init();
    sound_init();
    sound_register();
    emit_callback();
}

/* A standard 8-bit header (encode 0) of frames at rate Hz. */
static uint32_t std_header(uint32_t frames, uint32_t rate) {
    uint32_t h = scratch(22);
    gm_w32(h + 4, frames);
    gm_w32(h + 8, rate << 16);
    gm_w8(h + 20, 0);
    return h;
}

static uint32_t cmd(uint16_t c, uint16_t p1, uint32_t p2) {
    uint32_t a = scratch(8);
    gm_w16(a, c);
    gm_w16(a + 2, p1);
    gm_w32(a + 4, p2);
    return a;
}

static uint32_t new_channel(void) {
    uint32_t pp = scratch(4);
    uint32_t upp = call_import("NewSndCallBackUPP", 1, TV_CB);
    if (call_import("SndNewChannel", 4, pp, 5u, 0x84u, upp) != 0)
        fatal("SndNewChannel failed");
    return gm_r32(pp);
}

static bool busy(uint32_t chan) {
    uint32_t st = scratch(24);
    call_import("SndChannelStatus", 3, chan, 24u, st);
    return gm_r8(st + 12) != 0;
}

static void sleep_s(double s) {
    struct timespec ts = {0, (long)(s * 1e9)};
    nanosleep(&ts, NULL);
}

TEST(sound_header_durations) {
    setup();
    CHECK(sound_header_seconds(std_header(600, 600)) == 1.0);
    uint32_t ext = scratch(64);
    gm_w32(ext + 8, 22050u << 16);
    gm_w8(ext + 20, 0xFF);
    gm_w32(ext + 22, 44100);
    CHECK(sound_header_seconds(ext) == 2.0);
}

TEST(sound_new_channel_allocates_one) {
    setup();
    uint32_t chan = new_channel();
    CHECK(mm_is_ptr(chan));
    CHECK_EQ(gm_r32(chan + 8), TV_CB);
    CHECK(!busy(chan));
    CHECK_EQ(call_import("SndDisposeChannel", 2, chan, 1u), 0);
    CHECK_EQ((int16_t)call_import("SndDisposeChannel", 2, chan, 1u), SND_BAD_CHANNEL_ERR);
}

TEST(sound_buffer_keeps_the_channel_busy_then_calls_back) {
    setup();
    uint32_t chan = new_channel();
    /* 30 frames at 600 Hz: 50 ms. */
    call_import("SndDoCommand", 3, chan, cmd(SND_BUFFER_CMD, 0, std_header(30, 600)), 0u);
    call_import("SndDoCommand", 3, chan, cmd(SND_CALLBACK_CMD, 0, 0xCAFEu), 0u);
    sound_pump(); /* starts the buffer */
    CHECK(busy(chan));
    sound_pump();
    CHECK_EQ(gm_r32(GOT), 0); /* the buffer is still playing */
    sleep_s(0.08);
    sound_pump();
    CHECK_EQ(gm_r32(GOT), 0xCAFE);
    CHECK(!busy(chan));
}

TEST(sound_immediate_commands) {
    setup();
    uint32_t chan = new_channel();
    call_import("SndDoCommand", 3, chan, cmd(SND_BUFFER_CMD, 0, std_header(600, 600)), 0u);
    call_import("SndDoCommand", 3, chan, cmd(SND_CALLBACK_CMD, 0, 1u), 0u);
    sound_pump();
    CHECK(busy(chan));
    call_import("SndDoImmediate", 2, chan, cmd(SND_FLUSH_CMD, 0, 0));
    call_import("SndDoImmediate", 2, chan, cmd(SND_QUIET_CMD, 0, 0));
    CHECK(!busy(chan));
    sound_pump();
    CHECK_EQ(gm_r32(GOT), 0); /* the flushed callback never runs */
    call_import("SndDoImmediate", 2, chan, cmd(SND_CALLBACK_CMD, 0, 7u));
    CHECK_EQ(gm_r32(GOT), 7);
}

static void child_unknown_command(void *unused) {
    (void)unused;
    setup();
    uint32_t chan = new_channel();
    call_import("SndDoImmediate", 2, chan, cmd(99, 0, 0));
    call_import("SndDoImmediate", 2, chan, cmd(99, 0, 0));
}

TEST(sound_unknown_commands_are_logged_once) {
    char out[4096];
    CHECK_EQ(test_run_child(child_unknown_command, NULL, out, sizeof out), 0);
    const char *msg = "loony: sound: command 99 is not supported (ignored)";
    const char *first = strstr(out, msg);
    CHECK(first != NULL);
    CHECK(strstr(first + strlen(msg), msg) == NULL);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: the build fails with `'sound.h' file not found`.

- [ ] **Step 3: Write the implementation**

`src/sound.h`:
```c
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Sound Manager, silent for now: channels, command queues and callbacks
   behave as on a real Mac (a buffer keeps its channel busy for as long as it
   would play), but nothing is heard. Audio output arrives with milestone 5.
   Sound problems are logged, never fatal. */

/* SndCommand numbers. */
#define SND_NULL_CMD     0
#define SND_QUIET_CMD    3
#define SND_FLUSH_CMD    4
#define SND_WAIT_CMD     10
#define SND_CALLBACK_CMD 13
#define SND_VOLUME_CMD   46
#define SND_SOUND_CMD    80
#define SND_BUFFER_CMD   81

#define SND_QUEUE_FULL_ERR (-203)
#define SND_BAD_CHANNEL_ERR (-205)

void sound_init(void);

/* Runs queued commands whose time has come and delivers due callbacks
   (guest_call). The event loop calls this on every iteration. */
void sound_pump(void);

/* Seconds a sampled sound header plays for: frames / sample rate. 0 for a
   header that can't be parsed (which is logged). */
double sound_header_seconds(uint32_t header);

/* Registers NewSndCallBackUPP, SndNewChannel, SndDisposeChannel,
   SndDoCommand, SndDoImmediate and SndChannelStatus. */
void sound_register(void);
```

`src/sound.c`:
```c
#include "sound.h"

#include <string.h>

#include "guest_mem.h"
#include "memmgr.h"
#include "misc.h"
#include "trap.h"
#include "util.h"

#define MAX_CHANNELS 16
#define QUEUE_LEN 128
/* SndChannel: nextChan, firstMod, callBack (+8), userInfo (+12), wait,
   cmdInProgress (+20, 8 bytes), flags, qLength, qHead, qTail, queue. */
#define CHAN_CALLBACK 8
#define CHAN_CMD_IN_PROGRESS 20
#define SND_CHANNEL_SIZE (36 + 8 * QUEUE_LEN)

typedef struct {
    uint16_t cmd, param1;
    uint32_t param2;
} snd_cmd;

static struct {
    struct {
        uint32_t addr; /* guest SndChannel, 0 = free slot */
        bool ours;     /* allocated by SndNewChannel */
        snd_cmd queue[QUEUE_LEN];
        int head, count;
        double busy_until; /* seconds */
        uint16_t volume_l, volume_r;
    } ch[MAX_CHANNELS];
    bool warned[256];
} SND;

static double now(void) { return misc_seconds(); }

void sound_init(void) { memset(&SND, 0, sizeof SND); }

static int find(uint32_t chan) {
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (chan && SND.ch[i].addr == chan)
            return i;
    return -1;
}

static void warn_once(uint16_t cmd) {
    if (!SND.warned[cmd & 0xFF]) {
        SND.warned[cmd & 0xFF] = true;
        log_msg("sound: command %u is not supported (ignored)", cmd);
    }
}

double sound_header_seconds(uint32_t h) {
    uint32_t rate = gm_r32(h + 8); /* Fixed 16.16 */
    uint8_t encode = gm_r8(h + 20);
    uint32_t frames;
    if (encode == 0x00) /* stdSH: length is the frame count (8-bit mono) */
        frames = gm_r32(h + 4);
    else if (encode == 0xFF || encode == 0xFE) /* extSH / cmpSH */
        frames = gm_r32(h + 22);
    else {
        log_msg("sound: unknown sound header encoding 0x%02x", encode);
        return 0;
    }
    if (rate < 0x10000)
        return 0;
    return frames / (rate / 65536.0);
}

static void deliver_callback(int i, snd_cmd c) {
    uint32_t chan = SND.ch[i].addr, proc = gm_r32(chan + CHAN_CALLBACK);
    if (!proc)
        return;
    uint32_t cmd_p = chan + CHAN_CMD_IN_PROGRESS;
    gm_w16(cmd_p, c.cmd);
    gm_w16(cmd_p + 2, c.param1);
    gm_w32(cmd_p + 4, c.param2);
    uint32_t args[2] = {chan, cmd_p};
    guest_call(proc, 2, args);
}

/* Executes one command now. */
static void run(int i, snd_cmd c) {
    switch (c.cmd) {
    case SND_NULL_CMD:
    case SND_SOUND_CMD: /* installs a voice; doesn't play */
        break;
    case SND_QUIET_CMD:
        SND.ch[i].busy_until = 0;
        break;
    case SND_FLUSH_CMD:
        SND.ch[i].count = 0;
        break;
    case SND_WAIT_CMD: /* param1 in half-milliseconds */
        SND.ch[i].busy_until = now() + c.param1 / 2000.0;
        break;
    case SND_BUFFER_CMD:
        SND.ch[i].busy_until = now() + sound_header_seconds(c.param2);
        break;
    case SND_VOLUME_CMD:
        SND.ch[i].volume_l = (uint16_t)(c.param2 & 0xFFFF);
        SND.ch[i].volume_r = (uint16_t)(c.param2 >> 16);
        break;
    case SND_CALLBACK_CMD:
        deliver_callback(i, c);
        break;
    default:
        warn_once(c.cmd);
        break;
    }
}

void sound_pump(void) {
    for (int i = 0; i < MAX_CHANNELS; i++) {
        while (SND.ch[i].addr && SND.ch[i].count > 0 && now() >= SND.ch[i].busy_until) {
            snd_cmd c = SND.ch[i].queue[SND.ch[i].head];
            SND.ch[i].head = (SND.ch[i].head + 1) % QUEUE_LEN;
            SND.ch[i].count--;
            run(i, c);
        }
    }
}

static snd_cmd read_cmd(uint32_t p) { return (snd_cmd){gm_r16(p), gm_r16(p + 2), gm_r32(p + 4)}; }

static void h_new_snd_callback_upp(void) { trap_return(trap_arg(0)); }

/* SndNewChannel(SndChannelPtr *chan, short synth, long init, SndCallBackUPP userRoutine) */
static void h_snd_new_channel(void) {
    uint32_t pp = trap_arg(0);
    int i;
    for (i = 0; i < MAX_CHANNELS && SND.ch[i].addr; i++)
        ;
    if (i == MAX_CHANNELS)
        trap_crash("SndNewChannel: more than %d channels", MAX_CHANNELS);
    uint32_t chan = gm_r32(pp);
    bool ours = chan == 0;
    if (ours) {
        chan = mm_new_ptr(SND_CHANNEL_SIZE, true);
        if (!chan)
            trap_crash("SndNewChannel: out of guest memory");
        gm_w32(pp, chan);
    }
    gm_w32(chan + CHAN_CALLBACK, trap_arg(3));
    memset(&SND.ch[i], 0, sizeof SND.ch[i]);
    SND.ch[i].addr = chan;
    SND.ch[i].ours = ours;
    SND.ch[i].volume_l = SND.ch[i].volume_r = 0x100;
    trap_return(0);
}

static void h_snd_dispose_channel(void) {
    int i = find(trap_arg(0));
    if (i < 0) {
        trap_return((uint32_t)SND_BAD_CHANNEL_ERR);
        return;
    }
    if (SND.ch[i].ours)
        mm_dispose_ptr(SND.ch[i].addr);
    SND.ch[i].addr = 0;
    trap_return(0);
}

/* SndDoCommand(chan, const SndCommand *cmd, Boolean noWait) */
static void h_snd_do_command(void) {
    int i = find(trap_arg(0));
    if (i < 0) {
        trap_return((uint32_t)SND_BAD_CHANNEL_ERR);
        return;
    }
    if (SND.ch[i].count == QUEUE_LEN) {
        trap_return((uint32_t)SND_QUEUE_FULL_ERR);
        return;
    }
    SND.ch[i].queue[(SND.ch[i].head + SND.ch[i].count) % QUEUE_LEN] = read_cmd(trap_arg(1));
    SND.ch[i].count++;
    trap_return(0);
}

static void h_snd_do_immediate(void) {
    int i = find(trap_arg(0));
    if (i < 0) {
        trap_return((uint32_t)SND_BAD_CHANNEL_ERR);
        return;
    }
    run(i, read_cmd(trap_arg(1)));
    trap_return(0);
}

/* SndChannelStatus(chan, short theLength, SCStatus *status): 24 bytes; the
   busy flag is at +12. */
static void h_snd_channel_status(void) {
    int i = find(trap_arg(0));
    if (i < 0) {
        trap_return((uint32_t)SND_BAD_CHANNEL_ERR);
        return;
    }
    int16_t len = (int16_t)trap_arg(1);
    uint32_t st = trap_arg(2);
    if (len < 24)
        trap_crash("SndChannelStatus: status length %d is too small", len);
    memset(gm_ptr(st, 24), 0, 24);
    gm_w8(st + 12, now() < SND.ch[i].busy_until || SND.ch[i].count > 0);
    trap_return(0);
}

void sound_register(void) {
    trap_register("NewSndCallBackUPP", h_new_snd_callback_upp);
    trap_register("SndNewChannel", h_snd_new_channel);
    trap_register("SndDisposeChannel", h_snd_dispose_channel);
    trap_register("SndDoCommand", h_snd_do_command);
    trap_register("SndDoImmediate", h_snd_do_immediate);
    trap_register("SndChannelStatus", h_snd_channel_status);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S . -B build && cmake --build build && ./build/loony_tests sound_`
Expected: `5 passed, 0 failed, 0 skipped`. Full suite: `181 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/sound.h src/sound.c tests/asm.h tests/test_sound.c
git commit -m "Silent Sound Manager that keeps time and delivers callbacks"
```

---

### Task 10: Event handlers, timers and the run loop

**Files:**
- Create: `src/events.h`, `src/events.c`
- Create: `tests/test_events.c`

**Interfaces:**
- Consumes: `misc_seconds`, `misc_ticks`, `misc_set_idle` (Task 8); `sound_pump` (Task 9); `cpu_fpr` (Plan 1); `guest_call`, `trap_*`; `tests/asm.h` (Task 9).
- Produces:
  - `EV_TAG_BASE`, `EV_APPLICATION_TARGET`, `EV_DISPATCHER_TARGET`, `EV_MAIN_LOOP`, `EV_LOOP_TIMED_OUT_ERR` (-9875), `EV_MAX_HANDLERS`, `EV_MAX_TIMERS`, `ev_handler`
  - `void events_init(void)` (reads `LOONY_EXIT_AFTER`), `uint32_t events_window_target(uint32_t window)`, `int events_handlers(const ev_handler **out)`, `bool events_has_standard_handler(uint32_t target)`, `typedef void (*ev_present_fn)(void)`, `void events_set_present(ev_present_fn)`, `void events_pump(void)`, `int events_active_timers(void)`
  - `void events_register(void)`, which installs `NewEventHandlerUPP`, `GetApplicationEventTarget`, `GetEventDispatcherTarget`, `GetWindowEventTarget`, `InstallEventHandler`, `InstallStandardEventHandler`, `NewEventLoopTimerUPP`, `GetMainEventLoop`, `InstallEventLoopTimer`, `RemoveEventLoopTimer`, `RunApplicationEventLoop`, `QuitApplicationEventLoop`, `ReceiveNextEvent`

Under the PowerPC calling convention a `double` argument goes in the next FPR and also takes up two GPR slots. So `InstallEventLoopTimer(loop, delay, interval, proc, data, &timer)` has its intervals in f1 and f2 and its pointers in r8–r10. `ReceiveNextEvent(numTypes, list, timeout, pull, &event)` has its timeout in f1 and its last two arguments in r7 and r8. Timer procs are called as `proc(timerRef, userData)`. A periodic timer that falls behind skips ahead instead of firing repeatedly to catch up.

- [ ] **Step 1: Write the failing test**

`tests/test_events.c`:
```c
#include "test.h"

#include <stdlib.h>

#include "asm.h"
#include "events.h"
#include "harness.h"
#include "misc.h"
#include "util.h"

static const char *const names[] = {
    "NewEventHandlerUPP", "GetApplicationEventTarget", "GetEventDispatcherTarget",
    "GetWindowEventTarget", "InstallEventHandler", "InstallStandardEventHandler",
    "NewEventLoopTimerUPP", "GetMainEventLoop", "InstallEventLoopTimer", "RemoveEventLoopTimer",
    "RunApplicationEventLoop", "QuitApplicationEventLoop", "Delay", "ReceiveNextEvent",
};

#define PROC    (GUEST_IMAGE_BASE + 0x200)
#define TV_PROC (GUEST_IMAGE_BASE + 0x8100)
#define COUNTER (GUEST_IMAGE_BASE + 0x8200)

static uint32_t tv_of(const char *name) {
    for (uint32_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(names[i], name) == 0)
            return HARNESS_TV_BASE + 8 * i;
    return 0;
}

/* A timer proc that adds 1 to COUNTER and calls QuitApplicationEventLoop
   once it reaches limit. */
static void emit_counting_proc(int limit) {
    uint32_t tvq = tv_of("QuitApplicationEventLoop");
    uint32_t code[] = {
        PPC_MFLR_R0, PPC_SAVE_LR, PPC_PUSH64,
        ppc_lis(4, COUNTER >> 16), ppc_ori(4, 4, COUNTER & 0xFFFF),
        ppc_lwz(5, 0, 4), ppc_addi(5, 5, 1), ppc_stw(5, 0, 4),
        ppc_cmpwi(5, (int16_t)limit), ppc_blt(7 * 4),
        ppc_lis(12, tvq >> 16), ppc_ori(12, 12, tvq & 0xFFFF),
        ppc_lwz(0, 0, 12), ppc_lwz(2, 4, 12), PPC_MTCTR_R0, PPC_BCTRL,
        PPC_POP64, PPC_LOAD_LR, PPC_MTLR_R0, PPC_BLR,
    };
    put_words(PROC, code, (int)(sizeof code / sizeof code[0]));
    gm_w32(TV_PROC, PROC);
    gm_w32(TV_PROC + 4, 0);
    gm_w32(COUNTER, 0);
}

static void setup(void) {
    harness_init(names, sizeof names / sizeof names[0]);
    misc_init();
    misc_register();
    events_init();
    events_register();
}

static uint32_t install_timer(double delay, double interval) {
    cpu_set_fpr(1, delay);
    cpu_set_fpr(2, interval);
    uint32_t out = scratch(4);
    uint32_t upp = call_import("NewEventLoopTimerUPP", 1, TV_PROC);
    if (call_import("InstallEventLoopTimer", 8, call_import("GetMainEventLoop", 0), 0u, 0u, 0u, 0u,
                    upp, 0x55u, out) != 0)
        fatal("InstallEventLoopTimer failed");
    return gm_r32(out);
}

TEST(events_handlers_are_recorded) {
    setup();
    uint32_t list = scratch(16);
    gm_w32(list, FOURCC('k', 'e', 'y', 'b'));
    gm_w32(list + 4, 1);
    gm_w32(list + 8, FOURCC('k', 'e', 'y', 'b'));
    gm_w32(list + 12, 3);
    uint32_t out = scratch(4);
    uint32_t target = call_import("GetApplicationEventTarget", 0);
    CHECK_EQ(call_import("InstallEventHandler", 6, target, 0x1450u, 2u, list, 0x99u, out), 0);
    CHECK(gm_r32(out) != 0);
    const ev_handler *h;
    CHECK_EQ(events_handlers(&h), 1);
    CHECK_EQ(h[0].target, EV_APPLICATION_TARGET);
    CHECK_EQ(h[0].handler, 0x1450);
    CHECK_EQ(h[0].user_data, 0x99);
    CHECK_EQ(h[0].ntypes, 2);
    CHECK_EQ(h[0].types[1][1], 3);
    CHECK_EQ(call_import("NewEventHandlerUPP", 1, 0x1234u), 0x1234);
    uint32_t w1 = call_import("GetWindowEventTarget", 1, 0x01000100u);
    uint32_t w2 = call_import("GetWindowEventTarget", 1, 0x01000200u);
    CHECK(w1 != w2);
    CHECK(!events_has_standard_handler(w1));
    call_import("InstallStandardEventHandler", 1, w1);
    CHECK(events_has_standard_handler(w1));
    CHECK_EQ(call_import("GetEventDispatcherTarget", 0), EV_DISPATCHER_TARGET);
}

TEST(events_run_loop_fires_timers_until_quit) {
    setup();
    emit_counting_proc(3);
    double t0 = misc_seconds();
    install_timer(0.0, 0.02);
    CHECK_EQ(events_active_timers(), 1);
    call_import("RunApplicationEventLoop", 0);
    CHECK_EQ(gm_r32(COUNTER), 3);
    double elapsed = misc_seconds() - t0;
    CHECK(elapsed >= 0.035); /* fires at 0, 0.02 and 0.04 */
    CHECK(elapsed < 0.5);
}

TEST(events_one_shot_timer_fires_once) {
    setup();
    emit_counting_proc(100);
    install_timer(0.0, 0.0);
    events_pump();
    events_pump();
    CHECK_EQ(gm_r32(COUNTER), 1);
    CHECK_EQ(events_active_timers(), 0);
}

TEST(events_remove_timer) {
    setup();
    emit_counting_proc(100);
    uint32_t t = install_timer(10.0, 1.0);
    CHECK_EQ(call_import("RemoveEventLoopTimer", 1, t), 0);
    CHECK_EQ(events_active_timers(), 0);
}

TEST(events_delay_pumps_timers) {
    setup();
    misc_set_idle(events_pump);
    emit_counting_proc(100);
    install_timer(0.0, 0.0);
    call_import("Delay", 2, 2u, 0u);
    misc_set_idle(NULL);
    CHECK_EQ(gm_r32(COUNTER), 1);
}

static void child_exit_after(void *unused) {
    (void)unused;
    setenv("LOONY_EXIT_AFTER", "3", 1);
    setup();
    emit_counting_proc(1000);
    install_timer(0.0, 0.01);
    call_import("RunApplicationEventLoop", 0);
    exit(9);
}

TEST(events_exit_after_ends_the_run_cleanly) {
    char out[4096];
    CHECK_EQ(test_run_child(child_exit_after, NULL, out, sizeof out), 0);
    CHECK_CONTAINS(out, "loony: exiting after 3 ticks (LOONY_EXIT_AFTER)");
}

static void child_bad_timer(void *unused) {
    (void)unused;
    setup();
    call_import("RemoveEventLoopTimer", 1, 0x1234u);
}

TEST(events_removing_an_unknown_timer_crashes) {
    char out[16384];
    CHECK_EQ(test_run_child(child_bad_timer, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "RemoveEventLoopTimer: 0x00001234 is not a timer");
}

TEST(events_receive_next_event_times_out) {
    setup();
    uint32_t out = scratch(4);
    gm_w32(out, 0xFFFFFFFFu);
    cpu_set_fpr(1, 0.03);
    double t0 = misc_seconds();
    CHECK_EQ((int32_t)call_import("ReceiveNextEvent", 6, 0u, 0u, 0u, 0u, 1u, out),
             EV_LOOP_TIMED_OUT_ERR);
    CHECK(misc_seconds() - t0 >= 0.03);
    CHECK_EQ(gm_r32(out), 0);
    cpu_set_fpr(1, 0.0);
    CHECK_EQ((int32_t)call_import("ReceiveNextEvent", 6, 0u, 0u, 0u, 0u, 1u, out),
             EV_LOOP_TIMED_OUT_ERR);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S . -B build && cmake --build build`
Expected: the build fails with `'events.h' file not found`.

- [ ] **Step 3: Write the implementation**

`src/events.h`:
```c
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Carbon Event Manager: handler installation, event loop timers and
   RunApplicationEventLoop. Input events (keyboard, mouse) and dispatching
   them to handlers arrive in the next milestone. Event targets, timers and the
   main event loop are opaque IDs in tag space (EV_TAG_BASE and up). */

#define EV_TAG_BASE 0x0A000000u
#define EV_APPLICATION_TARGET (EV_TAG_BASE + 1)
#define EV_DISPATCHER_TARGET  (EV_TAG_BASE + 2)
#define EV_MAIN_LOOP          (EV_TAG_BASE + 3)
#define EV_LOOP_TIMED_OUT_ERR (-9875) /* eventLoopTimedOutErr */
#define EV_MAX_HANDLERS 32
#define EV_MAX_TIMERS 16

typedef struct {
    uint32_t target, handler, user_data;
    uint32_t ntypes;
    uint32_t types[8][2]; /* (class, kind) pairs */
} ev_handler;

void events_init(void);

/* The event target for a window. */
uint32_t events_window_target(uint32_t window);

/* Installed handlers, in installation order. */
int events_handlers(const ev_handler **out);

/* True if InstallStandardEventHandler was called for target. */
bool events_has_standard_handler(uint32_t target);

/* Called on every loop iteration to show the screen if anything drew to it. */
typedef void (*ev_present_fn)(void);
void events_set_present(ev_present_fn fn);

/* One pump step: sound, due timers (unless already inside one), presenting
   the screen, and the LOONY_EXIT_AFTER check. The run loop calls it, and so
   does misc's idle hook while the game waits in its own loop. */
void events_pump(void);

/* Number of timers installed and not removed. */
int events_active_timers(void);

/* Registers NewEventHandlerUPP, GetApplicationEventTarget,
   GetEventDispatcherTarget, GetWindowEventTarget, InstallEventHandler,
   InstallStandardEventHandler, NewEventLoopTimerUPP, GetMainEventLoop,
   InstallEventLoopTimer, RemoveEventLoopTimer, RunApplicationEventLoop,
   QuitApplicationEventLoop and ReceiveNextEvent (which, with no input yet,
   pumps until its timeout and returns EV_LOOP_TIMED_OUT_ERR).

   RunApplicationEventLoop fires due timers (guest_call(proc, timer, data)),
   presents the screen, and sleeps until the next timer. If LOONY_EXIT_AFTER is
   set to a tick count (1/60 s), the process exits with status 0 once that
   many ticks have passed. */
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
#include "misc.h"
#include "sound.h"
#include "trap.h"
#include "util.h"

#define MAX_STANDARD 8

static struct {
    ev_handler handlers[EV_MAX_HANDLERS];
    int nhandlers;
    uint32_t standard[MAX_STANDARD];
    int nstandard;
    struct {
        bool active;
        uint32_t proc, data;
        double next, interval; /* seconds on the misc clock */
    } timers[EV_MAX_TIMERS];
    bool quit;
    int depth; /* nested RunApplicationEventLoop calls */
    int in_timer; /* > 0 while a timer proc runs */
    ev_present_fn present;
    long exit_after; /* ticks, or 0 */
} E;

void events_init(void) {
    memset(&E, 0, sizeof E);
    const char *s = getenv("LOONY_EXIT_AFTER");
    if (s && *s)
        E.exit_after = strtol(s, NULL, 10);
}

void events_set_present(ev_present_fn fn) { E.present = fn; }

int events_active_timers(void) {
    int n = 0;
    for (int i = 0; i < EV_MAX_TIMERS; i++)
        n += E.timers[i].active;
    return n;
}

static double now_seconds(void) { return misc_seconds(); }

/* Window targets are EV_TAG_BASE + 0x10000 + the window's address / 16,
   which is unique and reversible. */
uint32_t events_window_target(uint32_t window) { return EV_TAG_BASE + 0x10000u + window / 16u; }

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
    if (out)
        gm_w32(out, EV_TAG_BASE + 0x1000u + (uint32_t)E.nhandlers);
    trap_return(0);
}

static void h_install_standard_event_handler(void) {
    if (E.nstandard < MAX_STANDARD)
        E.standard[E.nstandard++] = trap_arg(0);
    trap_return(0);
}

/* ---- timers and the run loop ---- */

static uint32_t timer_ref(int i) { return EV_TAG_BASE + 0x2000u + (uint32_t)i; }

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

static void fire_due_timers(void) {
    for (int i = 0; i < EV_MAX_TIMERS; i++) {
        if (!E.timers[i].active || now_seconds() < E.timers[i].next)
            continue;
        if (E.timers[i].interval > 0) {
            E.timers[i].next += E.timers[i].interval;
            if (E.timers[i].next < now_seconds()) /* fell behind: don't try to catch up */
                E.timers[i].next = now_seconds() + E.timers[i].interval;
        } else {
            E.timers[i].active = false;
        }
        uint32_t args[2] = {timer_ref(i), E.timers[i].data};
        E.in_timer++;
        guest_call(E.timers[i].proc, 2, args);
        E.in_timer--;
    }
}

static void sleep_until_next_timer(void) {
    double next = now_seconds() + 0.010;
    for (int i = 0; i < EV_MAX_TIMERS; i++)
        if (E.timers[i].active && E.timers[i].next < next)
            next = E.timers[i].next;
    double wait = next - now_seconds();
    if (wait > 0) {
        struct timespec ts = {0, (long)(wait * 1e9)};
        nanosleep(&ts, NULL);
    }
}

void events_pump(void) {
    sound_pump();
    if (!E.in_timer) /* a timer proc waiting in its own loop isn't re-entered */
        fire_due_timers();
    if (E.present)
        E.present();
    if (E.exit_after > 0 && misc_ticks() >= (uint32_t)E.exit_after) {
        log_msg("exiting after %ld ticks (LOONY_EXIT_AFTER)", E.exit_after);
        exit(0);
    }
}

static void h_run_application_event_loop(void) {
    E.quit = false;
    E.depth++;
    while (!E.quit) {
        events_pump();
        sleep_until_next_timer();
    }
    E.depth--;
}

static void h_quit_application_event_loop(void) { E.quit = true; }

/* ReceiveNextEvent(UInt32 numTypes, const EventTypeSpec *list,
   EventTimeout timeout, Boolean pullEvent, EventRef *outEvent) -> OSStatus.
   The timeout is a double in f1 (taking up r5-r6), so pullEvent and
   outEvent arrive in r7 and r8. A negative timeout means wait forever. */
static void h_receive_next_event(void) {
    double timeout = cpu_fpr(1);
    uint32_t out = trap_arg(5);
    double deadline = now_seconds() + timeout;
    for (;;) {
        events_pump();
        if (timeout >= 0 && now_seconds() >= deadline)
            break;
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
    if (out)
        gm_w32(out, 0);
    trap_return((uint32_t)EV_LOOP_TIMED_OUT_ERR);
}

void events_register(void) {
    trap_register("NewEventLoopTimerUPP", h_new_event_loop_timer_upp);
    trap_register("GetMainEventLoop", h_get_main_event_loop);
    trap_register("InstallEventLoopTimer", h_install_event_loop_timer);
    trap_register("RemoveEventLoopTimer", h_remove_event_loop_timer);
    trap_register("RunApplicationEventLoop", h_run_application_event_loop);
    trap_register("QuitApplicationEventLoop", h_quit_application_event_loop);
    trap_register("ReceiveNextEvent", h_receive_next_event);
    trap_register("NewEventHandlerUPP", h_new_event_handler_upp);
    trap_register("GetApplicationEventTarget", h_get_application_event_target);
    trap_register("GetEventDispatcherTarget", h_get_event_dispatcher_target);
    trap_register("GetWindowEventTarget", h_get_window_event_target);
    trap_register("InstallEventHandler", h_install_event_handler);
    trap_register("InstallStandardEventHandler", h_install_standard_event_handler);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake -S . -B build && cmake --build build && ./build/loony_tests events_`
Expected: `8 passed, 0 failed, 0 skipped`. Full suite: `189 passed`.

- [ ] **Step 5: Commit**

```bash
git add src/events.h src/events.c tests/test_events.c
git commit -m "Event handler registration, loop timers and the run loop"
```

---

### Task 11: Run the opening

**Files:**
- Modify: `src/main.c`
- Modify: `tests/test_run.c` (replace the shareware-alert test)
- Modify: `README.md`

**Interfaces:**
- Consumes: everything above.
- Produces: `loony` initializes the screen at 800×600×8 and the dialogs, events, files and sound modules. It wires `qd`'s present to `display_present`, the event loop's present to `display_present_if_dirty`, and `misc`'s idle hook to `events_pump`, then registers every module. The game plays its opening and attract mode until the window closes, or until `LOONY_EXIT_AFTER` ticks have passed.

- [ ] **Step 1: Write the failing test**

```diff
diff --git a/tests/test_run.c b/tests/test_run.c
index 706cd32..4066340 100644
--- a/tests/test_run.c
+++ b/tests/test_run.c
@@ -3,23 +3,46 @@
 #include <stdlib.h>
 #include <unistd.h>
 
+#include "util.h"
+
 static void run_loony(void *dir) {
     execl(LOONY_BIN, "loony", (const char *)dir, (char *)NULL);
     fprintf(stderr, "exec %s failed\n", LOONY_BIN);
     _exit(127);
 }
 
-TEST(run_reaches_the_shareware_alert) {
+static char shot[1024];
+
+static void run_loony_headless(void *dir) {
+    setenv("LOONY_EXIT_AFTER", "240", 1);
+    setenv("LOONY_SCREENSHOT", shot, 1);
+    run_loony(dir);
+}
+
+TEST(run_plays_the_opening_headless) {
     SKIP_UNLESS_GAME();
+    const char *t = getenv("TMPDIR");
+    snprintf(shot, sizeof shot, "%s/loony-run-XXXXXX", t && *t ? t : "/tmp");
+    int fd = mkstemp(shot);
+    CHECK(fd >= 0);
+    close(fd);
     char out[32768];
-    int status = test_run_child(run_loony, (void *)test_game_dir(), out, sizeof out);
-    CHECK_EQ(status, 2);
-    CHECK_CONTAINS(out, "loony: loaded ");
+    int status = test_run_child(run_loony_headless, (void *)test_game_dir(), out, sizeof out);
+    size_t len = 0;
+    uint8_t *png = read_file(shot, &len);
+    unlink(shot);
+    CHECK_EQ(status, 0);
     CHECK_CONTAINS(out, "132 imports");
-    CHECK_CONTAINS(out, "loony: crash: unimplemented import Alert");
-    /* Alert 901 is the shareware dialog. Everything before it is implemented. */
-    CHECK_CONTAINS(out, "Alert(0x00000385, ");
+    CHECK_CONTAINS(out, "loony: Alert 901 (answering item 1): Play Demo");
+    CHECK_CONTAINS(out, "loony: Alert 900 (answering item 1): OK");
+    CHECK_CONTAINS(out, "loony: exiting after 240 ticks (LOONY_EXIT_AFTER)");
     CHECK(!strstr(out, "unknown selector"));
+    CHECK(!strstr(out, "not supported"));
+    CHECK(png != NULL);
+    CHECK(len > 33);
+    CHECK_EQ(rd_be32(png + 16), 800);
+    CHECK_EQ(rd_be32(png + 20), 600);
+    free(png);
 }
 
 /* Review Focus 1: wrong or missing game folder. */
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build && ./build/loony_tests run_plays`
Expected: `0 passed, 1 failed`. The run still stops at the unimplemented `Alert` with status 2.

- [ ] **Step 3: Write the implementation**

```diff
diff --git a/src/main.c b/src/main.c
index fb58afc..6036301 100644
--- a/src/main.c
+++ b/src/main.c
@@ -6,11 +6,17 @@
 
 #include "cf.h"
 #include "cpu.h"
+#include "dialogs.h"
+#include "display.h"
+#include "events.h"
+#include "files.h"
 #include "guest_mem.h"
 #include "loader.h"
 #include "memmgr.h"
 #include "misc.h"
+#include "qd.h"
 #include "rsrc.h"
+#include "sound.h"
 #include "trap.h"
 #include "util.h"
 
@@ -57,6 +63,15 @@ int main(int argc, char **argv) {
     mm_init();
     misc_init();
     cf_init();
+    qd_init(800, 600, 8);
+    dialogs_init();
+    events_init();
+    files_init(dir);
+    sound_init();
+    display_init();
+    qd_set_present(display_present);
+    events_set_present(display_present_if_dirty);
+    misc_set_idle(events_pump);
 
     const char **names = calloc(img.pef.nimports ? img.pef.nimports : 1, sizeof *names);
     if (!names)
@@ -68,6 +83,11 @@ int main(int argc, char **argv) {
     rsrc_register();
     misc_register();
     cf_register();
+    qd_register();
+    dialogs_register();
+    events_register();
+    files_register();
+    sound_register();
     int32_t app_id = image_find_import(&img, "kCFPreferencesCurrentApplication");
     if (app_id >= 0)
         gm_w32(img.import_addr[app_id], cf_current_app());
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ./build/loony_tests run_`
Expected: `6 passed, 0 failed, 0 skipped`. The filter also matches four other tests with `run_` in their names. Full suite: `189 passed, 0 failed, 0 skipped`, with no output besides test names.

- [ ] **Step 5: Look at the opening**

Run:
```bash
for t in 30 240 600; do SDL_VIDEO_DRIVER=dummy LOONY_EXIT_AFTER=$t LOONY_SCREENSHOT=/tmp/loony-$t.png ./build/loony; done
```
Expected: three exits with status 0. `/tmp/loony-30.png` shows the LittleWing logo, `/tmp/loony-240.png` the *Loony Labyrinth* title centered on black, and `/tmp/loony-600.png` the table with the backglass and dot-matrix display. Open them and check, since a wrong palette or offset shows up immediately. Then run `./build/loony` and confirm that a window opens and animates, and that closing it exits.

- [ ] **Step 6: Update the README**

```diff
diff --git a/README.md b/README.md
index 90784f2..be76d25 100644
--- a/README.md
+++ b/README.md
@@ -22,10 +22,15 @@ LOONY_TRACE=imports ./build/loony     # log every OS call
 LOONY_TRACE=imports,calls ./build/loony   # also log each call into the game (callbacks)
 LOONY_TRACE=lowmem ./build/loony      # log the first write to each low-memory address
 LOONY_STUB=all ./build/loony          # unimplemented OS calls return 0 instead of crashing
+LOONY_EXIT_AFTER=600 LOONY_SCREENSHOT=shot.png ./build/loony   # run 10 s, save the last frame
+SDL_VIDEO_DRIVER=dummy ./build/loony  # no window (with LOONY_SCREENSHOT for headless runs)
 ```
 
 The original game files are only ever read, never modified. Today the game
-runs through its startup (memory, resources, Gestalt, preferences) and stops at
-its first dialog, `Alert`, which needs graphics (milestone 3).
+plays its opening (the LittleWing logo and the title) and then runs its attract
+mode on the table, silently and without input: keyboard input arrives in
+milestone 4 and sound in milestone 5. The two shareware alerts at startup are
+answered with their default button ("Play Demo", then "OK") until dialogs are
+drawn (milestone 6). The emulated screen is 800x600, the size the game expects.
 
 Design: `docs/superpowers/specs/2026-09-30-loony-shim-design.md`
```

- [ ] **Step 7: Commit**

```bash
git add src/main.c tests/test_run.c README.md
git commit -m "Run the game's opening and attract mode on screen"
```

---

## What comes next (not part of this plan)

Plan 4 (milestone 4, events and input) makes the table playable:
- Translate SDL keys to Carbon raw-key events (Esc, Enter, Z, /, Space, plus left/right Shift and Command) and dispatch them through the installed handlers (`SendEventToEventTarget`, `GetEventKind`, `GetEventParameter`, `ReleaseEvent`).
- Turn a window close or Cmd-Q into the quit Apple Event.
- Add the spec's fixed tick clock and scripted-input headless test.

Watch for:
- frame pacing, since the game busy-waits on `TickCount`;
- CPU use;
- whatever the trace reaches once a game actually starts.
