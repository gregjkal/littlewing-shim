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
