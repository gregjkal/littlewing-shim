#include "test.h"

#include "cgdisplay.h"
#include "harness.h"
#include "memmgr.h"
#include "misc.h"
#include "qd.h"

static const char *const names[] = {
    "CGMainDisplayID", "CGDisplayPixelsWide", "CGDisplayPixelsHigh", "CGDisplayBytesPerRow",
    "CGDisplayBaseAddress", "CGDisplayCurrentMode", "CGDisplayBestModeForParameters",
    "CGDisplaySwitchToMode", "CGDisplayCapture", "CGDisplayIsCaptured", "CGDisplayRelease",
    "CGDisplayHideCursor", "CGDisplayShowCursor",
};

static uint32_t display;

static void setup(void) {
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    misc_init();
    qd_init(800, 600, 32);
    cgdisplay_init();
    cgdisplay_register();
    display = call_import("CGMainDisplayID", 0);
}

static uint32_t best_mode(uint32_t depth, uint32_t w, uint32_t h, uint32_t *exact) {
    uint32_t out = scratch(4);
    gm_w32(out, 0);
    uint32_t mode = call_import("CGDisplayBestModeForParameters", 5, display, depth, w, h, out);
    *exact = gm_r32(out);
    return mode;
}

TEST(cgdisplay_base_address_is_the_screen) {
    setup();
    CHECK_EQ(display, CGDISPLAY_MAIN);
    CHECK_EQ(call_import("CGDisplayBaseAddress", 1, display), qd_screen_base());
    CHECK_EQ(call_import("CGDisplayBytesPerRow", 1, display), 3200);
    CHECK_EQ(call_import("CGDisplayPixelsWide", 1, display), 800);
    CHECK_EQ(call_import("CGDisplayPixelsHigh", 1, display), 600);
    uint32_t exact;
    call_import("CGDisplaySwitchToMode", 2, display, best_mode(16, 1024, 768, &exact));
    uint32_t base = call_import("CGDisplayBaseAddress", 1, display);
    CHECK_EQ(base, qd_screen_base());
    CHECK_EQ(call_import("CGDisplayBytesPerRow", 1, display), 2048);
    CHECK_EQ(call_import("CGDisplayPixelsWide", 1, display), 1024);
    CHECK_EQ(call_import("CGDisplayPixelsHigh", 1, display), 768);
    /* What the game writes there is on the screen. */
    gm_w16(base + 2048 + 2, 0x7C00);
    qd_pixels px;
    qd_palette pal;
    qd_screen(&px, &pal);
    CHECK_EQ(qd_get_pixel(&px, 1, 1), 0x7C00);
}

TEST(cgdisplay_best_mode_is_what_was_asked) {
    setup();
    uint32_t start = call_import("CGDisplayCurrentMode", 1, display), exact;
    uint32_t mode = best_mode(16, 1024, 768, &exact);
    CHECK(mode != 0);
    CHECK(mode != start);
    CHECK_EQ(exact, 1);
    CHECK_EQ(best_mode(16, 1024, 768, &exact), mode);
    CHECK_EQ(call_import("CGDisplaySwitchToMode", 2, display, mode), 0);
    CHECK_EQ(call_import("CGDisplayCurrentMode", 1, display), mode);
    qd_pixels px;
    qd_palette pal;
    qd_screen(&px, &pal);
    CHECK_EQ(px.depth, 16);
    CHECK_EQ(rect_w(px.bounds), 1024);
    /* Back to the mode it started in. */
    CHECK_EQ(call_import("CGDisplaySwitchToMode", 2, display, start), 0);
    qd_screen(&px, &pal);
    CHECK_EQ(px.depth, 32);
    CHECK_EQ(rect_w(px.bounds), 800);
    CHECK_EQ(rect_h(px.bounds), 600);
}

TEST(cgdisplay_capture_and_release_nest) {
    setup();
    CHECK_EQ(call_import("CGDisplayIsCaptured", 1, display), 0);
    qd_take_dirty();
    CHECK_EQ(call_import("CGDisplayCapture", 1, display), 0);
    CHECK_EQ(call_import("CGDisplayCapture", 1, display), 0);
    CHECK_EQ(call_import("CGDisplayIsCaptured", 1, display), 1);
    /* Captured, the game draws into the screen itself: it's always presented. */
    CHECK(qd_take_dirty());
    CHECK(qd_take_dirty());
    CHECK_EQ(call_import("CGDisplayRelease", 1, display), 0);
    CHECK_EQ(call_import("CGDisplayIsCaptured", 1, display), 1);
    CHECK_EQ(call_import("CGDisplayRelease", 1, display), 0);
    CHECK_EQ(call_import("CGDisplayIsCaptured", 1, display), 0);
    qd_take_dirty();
    CHECK(!qd_take_dirty());
    CHECK_EQ(call_import("CGDisplayRelease", 1, display), 1001); /* kCGErrorIllegalArgument */
}

TEST(cgdisplay_hides_and_shows_the_cursor) {
    setup();
    CHECK(misc_cursor_visible());
    CHECK_EQ(call_import("CGDisplayHideCursor", 1, display), 0);
    CHECK(!misc_cursor_visible());
    CHECK_EQ(call_import("CGDisplayShowCursor", 1, display), 0);
    CHECK(misc_cursor_visible());
    call_import("CGDisplayShowCursor", 1, display);
    CHECK(misc_cursor_visible());
    call_import("CGDisplayHideCursor", 1, display);
    CHECK(!misc_cursor_visible());
}

static void child_other_display(void *unused) {
    (void)unused;
    setup();
    call_import("CGDisplayPixelsWide", 1, 0x1234u);
}

TEST(cgdisplay_another_display_crashes) {
    char out[8192];
    CHECK_EQ(test_run_child(child_other_display, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "CGDisplayPixelsWide: 0x00001234 is not the main display");
}
