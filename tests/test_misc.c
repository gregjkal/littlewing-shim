#include "test.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>

#include "asm.h"
#include "cf.h"
#include "cpu.h"
#include "harness.h"
#include "misc.h"
#include "ppc.h"
#include "trap.h"

static const char *const names[] = {
    "Gestalt", "TickCount", "Microseconds", "Delay", "GetDateTime", "ReadLocation",
    "NumToString", "p2cstrcpy", "c2pstrcpy", "BlockMoveData", "InitCursor", "HideCursor", "ShowCursor",
    "SetThemeCursor", "KeyScript", "GetMBarHeight", "NewAEEventHandlerUPP",
    "AEInstallEventHandler", "ICStart", "ICStop", "ExitToShell", "ICLaunchURL", "num2dec",
    "LSOpenCFURLRef", "SetTextServiceLanguage", "TISCopyCurrentASCIICapableKeyboardInputSource",
    "TISSelectInputSource", "SetA5", "SetCurrentA5", "Fix2X", "Long2Fix",
    "LMGetCurrentA5", "SysEnvirons", "dec2num", "p2cstr", "c2pstr", "sqrt",
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

TEST(misc_gestalt_reports_10_4_11_to_a_macho_game) {
    setup();
    uint32_t v;
    misc_set_system_version(MISC_SYSTEM_VERSION_MACHO);
    CHECK_EQ(gestalt(FOURCC('s', 'y', 's', 'v'), &v), 0);
    CHECK_EQ(v, 0x104B);
    misc_init(); /* back to the classic games' answer */
    CHECK_EQ(gestalt(FOURCC('s', 'y', 's', 'v'), &v), 0);
    CHECK_EQ(v, 0x1028);
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

TEST(misc_string_conversions_in_place) {
    setup();
    uint32_t p = scratch(256);
    char s[256];
    gm_write_pstr(p, "Tristan");
    CHECK_EQ(call_import("p2cstr", 1, p), p);
    CHECK(gm_read_cstr(p, s, sizeof s));
    CHECK_STR(s, "Tristan");
    CHECK_EQ(call_import("c2pstr", 1, p), p);
    gm_read_pstr(p, s);
    CHECK_STR(s, "Tristan");
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
    call_import("HideCursor", 0);
    call_import("HideCursor", 0);
    call_import("ShowCursor", 0);
    CHECK(!misc_cursor_visible());
    call_import("ShowCursor", 0);
    CHECK(misc_cursor_visible());
    call_import("ShowCursor", 0); /* never past visible */
    call_import("HideCursor", 0);
    CHECK(!misc_cursor_visible());
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

static void exit_hook(void) { fprintf(stderr, "exit hook ran\n"); }

static void child_main_returned(void *unused) {
    (void)unused;
    setup();
    misc_set_exit_hook(exit_hook);
    misc_main_returned(42, unused != NULL);
}

/* As crt1 passes main's return value to exit(): the status is main's, and
   the log says so before the exit hook (which may exec the picker) runs. */
TEST(misc_main_returned_exits_with_mains_status) {
    char out[4096];
    int status = test_run_child(child_main_returned, NULL, out, sizeof out);
    CHECK_EQ(status, 42);
    const char *logged = strstr(out, "loony: main returned 42\n");
    CHECK(logged != NULL);
    CHECK_CONTAINS(logged, "exit hook ran");
}

/* A CodeWarrior program's void main leaves anything in r3: the run exits
   with 0, and the log keeps the value. */
TEST(misc_main_returned_from_a_void_main_exits_with_0) {
    char out[4096];
    int status = test_run_child(child_main_returned, (void *)1, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: main returned 42\n");
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

/* Runs with the virtual clock; restores the real one afterwards. */
static void fixed_setup(void) {
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    setup();
}

static void fixed_teardown(void) {
    unsetenv("LOONY_FIXED_CLOCK");
    misc_init();
}

/* On the virtual clock Delay visits every tick and 200 polls move it a tick,
   so the counts are exact. On the real clock a sleep that overshoots by a
   tick on a busy machine skips one, and the count came out short in CI (#61). */
TEST(misc_delay_and_tick_count_run_the_idle_hook) {
    fixed_setup();
    idle_calls = 0;
    misc_set_idle(count_idle);
    call_import("Delay", 2, 3u, 0u);
    int after_delay = idle_calls;
    uint32_t t0 = misc_ticks();
    for (int i = 0; i < 1000; i++)
        call_import("TickCount", 0);
    uint32_t ticks = misc_ticks() - t0;
    misc_set_idle(NULL);
    fixed_teardown();
    CHECK_EQ(after_delay, 3); /* once per tick while waiting */
    CHECK_EQ(ticks, 5u);
    /* TickCount only when the tick changes, not once per call */
    CHECK_EQ(idle_calls - after_delay, 5);
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

static char opened[256];
static bool fake_open(const char *url) {
    snprintf(opened, sizeof opened, "%s", url);
    return true;
}

static int32_t launch(const char *data, int32_t start, int32_t end) {
    uint32_t d = scratch(256), sp = scratch(4), ep = scratch(4);
    memcpy(gm_ptr(d, 256), data, strlen(data));
    gm_w32(sp, (uint32_t)start);
    gm_w32(ep, (uint32_t)end);
    return (int32_t)call_import("ICLaunchURL", 6, MISC_IC_INSTANCE, scratch(2), d,
                                (uint32_t)strlen(data), sp, ep);
}

TEST(misc_ic_launch_url_opens_the_selection) {
    misc_set_url_opener(fake_open);
    setup();
    opened[0] = '\0';
    const char *u = "http://www.LittleWingPinball.com/";
    CHECK_EQ(launch(u, 0, (int32_t)strlen(u)), 0);
    CHECK_STR(opened, u);
    opened[0] = '\0';
    CHECK_EQ(launch("xx https://a.example/ yy", 3, 21), 0);
    CHECK_STR(opened, "https://a.example/");
    opened[0] = '\0';
    CHECK_EQ(launch("file:///etc/passwd", 0, 18), -50); /* only the web */
    CHECK_EQ(launch("http://a/", 0, 99), -50);           /* past the data */
    CHECK_STR(opened, "");
}

static void check_dec(int style, int digits, double x, bool neg, const char *sig, int exp) {
    misc_decimal d;
    misc_num2dec(style, digits, x, &d);
    if (d.negative != neg || strcmp(d.sig, sig) != 0 || d.exp != exp) {
        char m[200];
        snprintf(m, sizeof m, "num2dec(%d, %d, %g) = %s%se%d, expected %s%se%d", style, digits, x,
                 d.negative ? "-" : "", d.sig, d.exp, neg ? "-" : "", sig, exp);
        test_fail(__FILE__, __LINE__, m);
    }
}

TEST(misc_num2dec_float_and_fixed) {
    check_dec(0, 5, 3.14159265, false, "31416", -4);
    check_dec(0, 3, -1234567.0, true, "123", 4);
    check_dec(0, 2, 9.99, false, "10", 0); /* rounding carries */
    check_dec(0, 4, 0.0, false, "0", 0);
    check_dec(0, 4, -0.0, true, "0", 0);
    check_dec(0, 6, 1e300 * 1e300, false, "I", 0);
    check_dec(0, 6, NAN, false, "N", 0);
    check_dec(0, 32, 0.1, false, "10000000000000000555111512312578", -32);
    check_dec(1, 2, 3.14159, false, "314", -2);
    check_dec(1, 0, 2.5, false, "2", 0); /* round half to even, like printf */
    check_dec(1, 3, 0.0004, false, "0", 0);
    check_dec(1, 2, -0.5, true, "50", -2);
    check_dec(1, 2, 1e40, false, "?", 0);
}

TEST(misc_num2dec_writes_the_decimal_record) {
    setup();
    uint32_t f = scratch(4), d = scratch(42);
    gm_w8(f, 0);
    gm_w16(f + 2, 4);
    cpu_set_fpr(1, -2.5);
    call_import("num2dec", 4, f, 0u, 0u, d);
    CHECK_EQ(gm_r8(d), 1);
    CHECK_EQ((int16_t)gm_r16(d + 2), -3);
    CHECK_EQ(gm_r8(d + 4), 4);
    CHECK(memcmp(gm_ptr(d + 5, 4), "2500", 4) == 0);
}

TEST(misc_ls_open_cfurl_ref_opens_the_web_only) {
    misc_set_url_opener(fake_open);
    setup();
    cf_init();
    opened[0] = '\0';
    uint32_t out = scratch(4);
    gm_w32(out, 0xAAAAAAAAu);
    const char *u = "http://www.littlewingpinball.com/buynow/mad_daedalus_mac.html";
    CHECK_EQ(call_import("LSOpenCFURLRef", 2, cf_url(u), out), 0);
    CHECK_STR(opened, u);
    CHECK_EQ(gm_r32(out), 0);
    opened[0] = '\0';
    CHECK_EQ(call_import("LSOpenCFURLRef", 2, cf_url("/Applications/Calculator.app"), 0), (uint32_t)-50);
    CHECK_STR(opened, "");
}

TEST(misc_text_input_sources_stay_the_hosts) {
    setup();
    CHECK_EQ(call_import("SetTextServiceLanguage", 1, scratch(4)), 0);
    CHECK_EQ(call_import("TISCopyCurrentASCIICapableKeyboardInputSource", 0), 0);
    CHECK_EQ(call_import("TISSelectInputSource", 1, 0), (uint32_t)-50);
}

/* A PowerPC program has no A5 world. */
TEST(misc_set_a5_changes_nothing_and_returns_zero) {
    setup();
    CHECK_EQ(call_import("SetCurrentA5", 0), 0);
    CHECK_EQ(call_import("SetA5", 1, 0x1234u), 0);
    CHECK_EQ(call_import("SetCurrentA5", 0), 0);
    CHECK_EQ(call_import("LMGetCurrentA5", 0), 0);
}

/* Tristan checks the System's version, the processor, the FPU and Color
   QuickDraw before it runs. */
TEST(misc_sys_environs_reports_a_color_68040) {
    setup();
    uint32_t rec = scratch(16);
    memset(gm_ptr(rec, 16), 0xAA, 16);
    CHECK_EQ(call_import("SysEnvirons", 2, 1u, rec), 0);
    CHECK_EQ(gm_r16(rec), 1);
    CHECK_EQ(gm_r16(rec + 4), MISC_SYSTEM_VERSION_PEF);
    CHECK_EQ(gm_r16(rec + 6), 5);
    CHECK_EQ(gm_r8(rec + 8), 1);
    CHECK_EQ(gm_r8(rec + 9), 1);
    CHECK_EQ(gm_r16(rec + 10), 0);
    CHECK_EQ((int16_t)gm_r16(rec + 14), -1);
}

/* A call returning a double, as the game makes it: guest_call puts the
   registers back, so guest code (through a transition vector) stores the
   f1 it returns. */
static double fp_call(const char *name, uint32_t f) {
    uint32_t t = GUEST_TRAP_ADDR(trap_find(name)), out = scratch(8), code = scratch(64);
    const uint32_t words[] = {
        PPC_MFLR_R0, PPC_SAVE_LR, PPC_PUSH64, ppc_lis(12, t >> 16), ppc_ori(12, 12, t & 0xFFFF),
        0x7D8903A6, /* mtctr r12 */
        PPC_BCTRL, ppc_lis(4, out >> 16), ppc_ori(4, 4, out & 0xFFFF),
        0xD8240000, /* stfd f1,0(r4) */
        PPC_POP64, PPC_LOAD_LR, PPC_MTLR_R0, PPC_BLR,
    };
    put_words(code, words, sizeof words / sizeof words[0]);
    uint32_t tv = scratch(8);
    gm_w32(tv, code);
    guest_call(tv, 1, &f);
    uint64_t bits = (uint64_t)gm_r32(out) << 32 | gm_r32(out + 4);
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

static double fix2x(uint32_t f) { return fp_call("Fix2X", f); }

static double dec2num(bool neg, int16_t exp, const char *sig) {
    uint32_t d = scratch(42);
    gm_w8(d, neg);
    gm_w16(d + 2, (uint16_t)exp);
    gm_w8(d + 4, (uint8_t)strlen(sig));
    memcpy(gm_ptr(d + 5, 36), sig, strlen(sig));
    return fp_call("dec2num", d);
}

TEST(misc_dec2num_reads_the_decimal_record) {
    setup();
    CHECK(dec2num(true, -3, "2500") == -2.5);
    CHECK(dec2num(false, 2, "12") == 1200.0);
    CHECK(dec2num(false, 0, "0") == 0.0);
    CHECK(isinf(dec2num(false, 0, "I")));
    CHECK(isnan(dec2num(false, 0, "N")));
}

static double sqrt_of(double x) {
    cpu_set_fpr(1, x);
    return fp_call("sqrt", 0);
}

TEST(misc_sqrt) {
    setup();
    CHECK(sqrt_of(2.0) == sqrt(2.0));
}

TEST(misc_fixed_point) {
    setup();
    CHECK(fix2x(0x00018000u) == 1.5);
    CHECK(fix2x(0xFFFF0000u) == -1.0);
    CHECK_EQ(call_import("Long2Fix", 1, 3u), 0x00030000u);
    CHECK_EQ(call_import("Long2Fix", 1, (uint32_t)-2), 0xFFFE0000u);
    CHECK_EQ(call_import("Long2Fix", 1, 40000u), 0x7FFFFFFFu);
    CHECK_EQ(call_import("Long2Fix", 1, (uint32_t)-40000), 0x80000000u);
}
