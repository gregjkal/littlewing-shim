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
    CHECK(t0 >= MISC_BOOT_TICKS && t0 < MISC_BOOT_TICKS + 60); /* never 0 */
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
    CHECK(ua >= MISC_BOOT_TICKS * 1000000ull / 60); /* the boot minute */
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

static int idle_calls;
static void count_idle(void) { idle_calls++; }

TEST(misc_seconds_has_sub_tick_resolution) {
    setup();
    double a = misc_seconds();
    struct timespec ts = {0, 2000000};
    nanosleep(&ts, NULL);
    double b = misc_seconds();
    CHECK(b - a >= 0.002);
    CHECK(b - a < 0.5);
}

TEST(misc_delay_and_tick_count_run_the_idle_hook) {
    setup();
    idle_calls = 0;
    misc_set_idle(count_idle);
    call_import("Delay", 2, 3u, 0u);
    int after_delay = idle_calls;
    for (int i = 0; i < 1000; i++)
        call_import("TickCount", 0);
    misc_set_idle(NULL);
    CHECK(after_delay >= 3); /* once per tick while waiting */
    CHECK(after_delay <= 5);
    CHECK(idle_calls - after_delay <= 2); /* TickCount only when the tick changes */
}

/* Runs with the virtual clock; restores the real one afterwards. */
static void fixed_setup(void) {
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    setup();
}

static void fixed_teardown(void) {
    unsetenv("LOONY_FIXED_CLOCK");
    misc_init();
}

TEST(misc_fixed_clock_stands_still_until_the_game_waits) {
    fixed_setup();
    bool fixed = misc_fixed_clock();
    double t0 = misc_seconds();
    struct timespec ts = {0, 20000000};
    nanosleep(&ts, NULL);
    double t1 = misc_seconds();
    misc_wait(0.5);
    double t2 = misc_seconds();
    misc_wait(1e-9); /* rounds up to a microsecond, never to nothing */
    double t3 = misc_seconds();
    fixed_teardown();
    CHECK(fixed);
    CHECK(t0 == 0.0);
    CHECK(t1 == 0.0);
    CHECK(t2 == 0.5);
    CHECK(t3 > t2);
}

TEST(misc_fixed_clock_delay_advances_whole_ticks) {
    fixed_setup();
    uint32_t final_ticks = scratch(4);
    call_import("Delay", 2, 0u, final_ticks); /* one tick passes */
    uint32_t a = call_import("TickCount", 0), fa = gm_r32(final_ticks);
    call_import("Delay", 2, 3u, final_ticks);
    uint32_t b = call_import("TickCount", 0);
    fixed_teardown();
    CHECK_EQ(a, MISC_BOOT_TICKS + 1);
    CHECK_EQ(fa, MISC_BOOT_TICKS + 1);
    CHECK_EQ(b, MISC_BOOT_TICKS + 4);
}

TEST(misc_fixed_clock_polling_alone_moves_time) {
    fixed_setup();
    for (int i = 0; i < 199; i++)
        call_import("TickCount", 0);
    uint32_t before = call_import("TickCount", 0); /* the 200th poll */
    uint32_t us = scratch(8);
    for (int i = 0; i < 200; i++)
        call_import("Microseconds", 1, us);
    uint32_t after = call_import("TickCount", 0);
    fixed_teardown();
    CHECK_EQ(before, MISC_BOOT_TICKS + 1);
    CHECK_EQ(after, MISC_BOOT_TICKS + 2);
}

TEST(misc_fixed_clock_date_is_fixed) {
    fixed_setup();
    uint32_t secs = scratch(4);
    call_import("GetDateTime", 1, secs);
    uint32_t d = gm_r32(secs);
    fixed_teardown();
    CHECK_EQ(d, 3124224000u); /* 2003-01-01 00:00:00 */
}

TEST(misc_fixed_clock_location_is_fixed) {
    fixed_setup();
    uint32_t loc = scratch(12);
    gm_w32(loc + 8, 0xFFFFFFFFu);
    call_import("ReadLocation", 1, loc);
    uint32_t delta = gm_r32(loc + 8);
    fixed_teardown();
    CHECK_EQ(delta, 0); /* GMT, no daylight saving */
}
