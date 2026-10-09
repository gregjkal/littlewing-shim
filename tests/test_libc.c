#include "test.h"

#include <ctype.h>
#include <runetype.h>
#include <stdlib.h>

#include "harness.h"
#include "libc.h"
#include "memmgr.h"
#include "misc.h"

static const char *const names[] = {
    "malloc", "calloc", "free", "memcpy", "memmove", "memset", "strlen", "strcmp", "strcpy",
    "strncpy", "strcat", "qsort", "rand", "srand", "time", "localtime", "strftime", "usleep",
    "exit", "abort", "__maskrune", "__toupper", "dlopen", "dlsym",
    "NSIsSymbolNameDefinedWithHint", "NSLookupAndBindSymbolWithHint", "NSAddressOfSymbol",
    "_keymgr_get_and_lock_processwide_ptr", "_keymgr_set_and_unlock_processwide_ptr",
    "_init_keymgr", "dyld_stub_binding_helper", "sprintf",
};
#define NNAMES (uint32_t)(sizeof names / sizeof names[0])

static void setup(void) {
    unsetenv("LOONY_FIXED_CLOCK");
    harness_init_direct(names, NNAMES);
    mm_init();
    misc_init();
    libc_init("/Applications/MONSTER FAIR.app/Contents/MacOS/MONSTER FAIR");
    libc_register();
}

static void setup_fixed_clock(void) {
    setup();
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    misc_init();
    unsetenv("LOONY_FIXED_CLOCK");
}

static uint32_t cstr(const char *s) {
    uint32_t a = scratch((uint32_t)strlen(s) + 1);
    gm_write_cstr(a, s);
    return a;
}

static const char *read_str(uint32_t a) {
    static char buf[1024];
    gm_read_cstr(a, buf, sizeof buf);
    return buf;
}

TEST(libc_malloc_and_free_use_the_pointer_heap) {
    setup();
    uint32_t p = call_import("malloc", 1, 100);
    CHECK(mm_is_ptr(p));
    CHECK(p >= gm_heap_base());
    uint32_t a = call_import("malloc", 1, 0), b = call_import("malloc", 1, 0);
    CHECK(a && b && a != b);
    uint32_t c = call_import("calloc", 2, 10, 10);
    CHECK_EQ(mm_ptr_size(c), 100);
    for (uint32_t i = 0; i < 100; i++)
        CHECK_EQ(gm_r8(c + i), 0);
    CHECK_EQ(call_import("calloc", 2, 0x10000, 0x10000), 0);
    call_import("free", 1, p);
    CHECK(!mm_is_ptr(p));
    call_import("free", 1, 0);
}

static void child_bad_free(void *unused) {
    (void)unused;
    setup();
    call_import("free", 1, gm_heap_base() + 0x1234);
}

TEST(libc_free_of_a_bad_pointer_crashes) {
    char out[8192];
    CHECK_EQ(test_run_child(child_bad_free, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "not a block malloc returned");
}

TEST(libc_strings_and_bytes) {
    setup();
    uint32_t a = cstr("pinball"), buf = scratch(64);
    CHECK_EQ(call_import("strlen", 1, a), 7);
    CHECK_EQ(call_import("strcpy", 2, buf, a), buf);
    CHECK_STR(read_str(buf), "pinball");
    call_import("strcat", 2, buf, cstr(" fair"));
    CHECK_STR(read_str(buf), "pinball fair");
    CHECK_EQ(call_import("strcmp", 2, a, cstr("pinball")), 0);
    CHECK((int32_t)call_import("strcmp", 2, a, cstr("pinbalm")) < 0);
    CHECK((int32_t)call_import("strcmp", 2, cstr("\xE9"), cstr("a")) > 0); /* unsigned bytes */
    call_import("memset", 3, buf, 'x', 16);
    call_import("strncpy", 3, buf, cstr("ab"), 5);
    CHECK_EQ(gm_r8(buf + 1), 'b');
    CHECK_EQ(gm_r8(buf + 4), 0);
    CHECK_EQ(gm_r8(buf + 5), 'x');
    call_import("memcpy", 3, buf, a, 3);
    CHECK_EQ(gm_r8(buf + 2), 'n');
    call_import("memmove", 3, buf + 1, buf, 4); /* overlapping */
    CHECK_EQ(gm_r8(buf + 3), 'n');
    CHECK_EQ(call_import("memset", 3, 0, 0, 0), 0); /* nothing to touch */
}

TEST(libc_qsort_calls_the_guest_comparator) {
    setup();
    /* int cmp(const pair *a, const pair *b) { return a->key - b->key; } */
    uint32_t cmp = scratch(16);
    const uint32_t code[] = {
        0x80630000, /* lwz  r3,0(r3) */
        0x80840000, /* lwz  r4,0(r4) */
        0x7C641850, /* subf r3,r4,r3 */
        0x4E800020, /* blr */
    };
    put_words(cmp, code, 4);
    const uint32_t keys[] = {5, 1, 4, 1, 5, 9, 2, 6, 5, 3};
    uint32_t arr = scratch(8 * 10);
    for (uint32_t i = 0; i < 10; i++) {
        gm_w32(arr + 8 * i, keys[i]);
        gm_w32(arr + 8 * i + 4, i); /* original position */
    }
    call_import("qsort", 4, arr, 10, 8, cmp);
    const uint32_t want_key[] = {1, 1, 2, 3, 4, 5, 5, 5, 6, 9};
    const uint32_t want_pos[] = {1, 3, 6, 9, 2, 0, 4, 8, 7, 5}; /* stable */
    for (uint32_t i = 0; i < 10; i++) {
        CHECK_EQ(gm_r32(arr + 8 * i), want_key[i]);
        CHECK_EQ(gm_r32(arr + 8 * i + 4), want_pos[i]);
    }
}

TEST(libc_rand_is_darwins) {
    setup();
    /* The first values of Park and Miller's minimal standard generator. */
    CHECK_EQ(call_import("rand", 0), 16807);
    CHECK_EQ(call_import("rand", 0), 282475249);
    CHECK_EQ(call_import("rand", 0), 1622650073);
    call_import("srand", 1, 1);
    CHECK_EQ(call_import("rand", 0), 16807);
    call_import("srand", 1, 0); /* 0 is replaced by 123459876 */
    CHECK_EQ(call_import("rand", 0), 520932930);
}

TEST(libc_time_follows_the_fixed_clock) {
    setup_fixed_clock();
    uint32_t out = scratch(4);
    CHECK_EQ(call_import("time", 1, out), 1041379200); /* 2003-01-01 00:00:00 UTC */
    CHECK_EQ(gm_r32(out), 1041379200);
    misc_wait(5);
    CHECK_EQ(call_import("time", 1, 0), 1041379205);
}

TEST(libc_localtime_fills_a_32_bit_tm) {
    setup_fixed_clock();
    uint32_t t = scratch(4);
    gm_w32(t, 1041379200 + 3661);
    uint32_t tm = call_import("localtime", 1, t);
    const uint32_t want[] = {1, 1, 1, 1, 0, 103, 3, 0, 0, 0}; /* a Wednesday, UTC */
    for (uint32_t i = 0; i < 10; i++)
        CHECK_EQ(gm_r32(tm + 4 * i), want[i]);
    CHECK_STR(read_str(gm_r32(tm + 40)), "UTC");
}

TEST(libc_strftime_formats_a_high_score_date) {
    setup_fixed_clock();
    uint32_t t = scratch(4), buf = scratch(64);
    gm_w32(t, 1041379200 + 86400 * 40 + 3600 * 13 + 60 * 7 + 9);
    uint32_t tm = call_import("localtime", 1, t);
    CHECK_EQ(call_import("strftime", 4, buf, 64, cstr("%Y-%m-%d %X"), tm), 19);
    CHECK_STR(read_str(buf), "2003-02-10 13:07:09");
    CHECK_EQ(call_import("strftime", 4, buf, 10, cstr("%Y-%m-%d %X"), tm), 0); /* too long */
}

TEST(libc_usleep_advances_the_fixed_clock) {
    setup_fixed_clock();
    call_import("usleep", 1, 500000);
    CHECK_EQ(misc_ticks(), 30);
    call_import("usleep", 1, 1);
    CHECK(misc_seconds() > 0.5 && misc_seconds() < 0.5001);
}

TEST(libc_maskrune_matches_the_host) {
    setup();
    uint32_t rune = libc_data_symbol("_DefaultRuneLocale");
    CHECK(rune != 0);
    CHECK_EQ(memcmp(gm_ptr(rune, 8), "RuneMagA", 8), 0);
    for (uint32_t c = 0; c < 256; c++) {
        CHECK_EQ(call_import("__maskrune", 2, c, _CTYPE_A), (uint32_t)(isalpha((int)c) ? _CTYPE_A : 0));
        CHECK_EQ(gm_r32(rune + 52 + 4 * c), _DefaultRuneLocale.__runetype[c]);
        CHECK_EQ(gm_r32(rune + 2100 + 4 * c), (uint32_t)_DefaultRuneLocale.__mapupper[c]);
    }
    CHECK_EQ(call_import("__toupper", 1, 'q'), 'Q');
    CHECK_EQ(call_import("__toupper", 1, 0x1234), 0x1234);
    CHECK_EQ(call_import("__maskrune", 2, 0x1234, 0xFFFFFFFFu), 0);
}

TEST(libc_dlsym_finds_sprintf_ldbl128) {
    setup();
    uint32_t sprintf_trap = GUEST_TRAP_ADDR(NNAMES - 1);
    uint32_t h = call_import("dlopen", 2, cstr("/usr/lib/libSystem.B.dylib"), 1);
    CHECK(h != 0);
    CHECK_EQ(call_import("dlsym", 2, h, cstr("sprintf$LDBL128")), sprintf_trap);
    CHECK_EQ(call_import("dlsym", 2, h, cstr("sprintf")), sprintf_trap);
    CHECK_EQ(call_import("dlsym", 2, h, cstr("printf")), 0);
    CHECK_EQ(call_import("NSIsSymbolNameDefinedWithHint", 2, cstr("_sprintf$LDBL128"), 0), 1);
    CHECK_EQ(call_import("NSIsSymbolNameDefinedWithHint", 2, cstr("_fprintf"), 0), 0);
    uint32_t sym = call_import("NSLookupAndBindSymbolWithHint", 2, cstr("_sprintf$LDBL128"), 0);
    CHECK_EQ(call_import("NSAddressOfSymbol", 1, sym), sprintf_trap);

    /* Calling what dlsym returned formats. */
    uint32_t buf = scratch(32);
    uint32_t args[3] = {buf, cstr("%d points"), 500};
    CHECK_EQ(guest_call(sprintf_trap, 3, args), 10);
    CHECK_STR(read_str(buf), "500 points");
}

TEST(libc_sprintf_formats_the_games_conversions) {
    setup();
    uint32_t buf = scratch(256);
    call_import("sprintf", 3, buf, cstr("%03d,"), 7);
    CHECK_STR(read_str(buf), "007,");
    call_import("sprintf", 3, buf, cstr("%d00"), 12);
    CHECK_STR(read_str(buf), "1200");
    call_import("sprintf", 3, buf, cstr("[%6d]"), (uint32_t)-42);
    CHECK_STR(read_str(buf), "[   -42]");
    call_import("sprintf", 4, buf, cstr("%s and %-5s|"), cstr("LOONY"), cstr("MF"));
    CHECK_STR(read_str(buf), "LOONY and MF   |");
    call_import("sprintf", 6, buf, cstr("%x %X %c %u %%"), 0xBEEF, 0xBEEF, 'z', 0xFFFFFFFFu);
    CHECK_STR(read_str(buf), "beef BEEF z 4294967295 %");
    call_import("sprintf", 6, buf, cstr("%*d|%.2s|%ld"), 4, 7, cstr("abc"), 99);
    CHECK_STR(read_str(buf), "   7|ab|99");
    call_import("sprintf", 3, buf, cstr("%hd"), 0x12345);
    CHECK_STR(read_str(buf), "9029");
    /* Six arguments fill r5 to r10. */
    call_import("sprintf", 8, buf, cstr("%d%d%d%d%d%d"), 1, 2, 3, 4, 5, 6);
    CHECK_STR(read_str(buf), "123456");
}

static void child_sprintf_float(void *unused) {
    (void)unused;
    setup();
    call_import("sprintf", 3, scratch(64), cstr("%.2f"), 0);
}

TEST(libc_sprintf_refuses_floats) {
    char out[8192];
    CHECK_EQ(test_run_child(child_sprintf_float, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "sprintf: unsupported conversion %f in \"%.2f\"");
}

TEST(libc_keymgr_keeps_a_pointer_per_key) {
    setup();
    CHECK_EQ(call_import("_keymgr_get_and_lock_processwide_ptr", 1, 5), 0);
    call_import("_keymgr_set_and_unlock_processwide_ptr", 2, 5, 0x1234);
    call_import("_keymgr_set_and_unlock_processwide_ptr", 2, 6, 0x5678);
    CHECK_EQ(call_import("_keymgr_get_and_lock_processwide_ptr", 1, 5), 0x1234);
    CHECK_EQ(call_import("_keymgr_get_and_lock_processwide_ptr", 1, 6), 0x5678);
    call_import("_init_keymgr", 0);
}

TEST(libc_data_symbols) {
    setup();
    const char *data[] = {"errno", "_DefaultRuneLocale", "__keymgr_global", "mach_init_routine",
                          "_cthread_init_routine"};
    for (int i = 0; i < 5; i++) {
        uint32_t a = libc_data_symbol(data[i]);
        CHECK(a >= gm_heap_base());
        CHECK_EQ(gm_r32(a), i == 1 ? 0x52756E65u /* 'Rune' */ : 0);
    }
    CHECK_EQ(libc_data_symbol("environ"), 0);
}

TEST(libc_writes_mains_arguments) {
    setup();
    uint32_t argv, envp, apple;
    libc_main_args(&argv, &envp, &apple);
    CHECK_STR(read_str(gm_r32(argv)), "/Applications/MONSTER FAIR.app/Contents/MacOS/MONSTER FAIR");
    CHECK_EQ(gm_r32(argv + 4), 0);
    CHECK_EQ(gm_r32(envp), 0);
    CHECK_EQ(gm_r32(apple), gm_r32(argv));
    CHECK_EQ(gm_r32(apple + 4), 0);
}

static void exit_hook(void) {
    fprintf(stderr, "exit hook ran\n");
}

static void child_exit(void *unused) {
    (void)unused;
    setup();
    misc_set_exit_hook(exit_hook);
    call_import("exit", 1, 3);
}

TEST(libc_exit_runs_the_exit_hook) {
    char out[8192];
    CHECK_EQ(test_run_child(child_exit, NULL, out, sizeof out), 3);
    CHECK_CONTAINS(out, "loony: exit(3)");
    CHECK_CONTAINS(out, "exit hook ran");
}

static void child_abort(void *unused) {
    (void)unused;
    setup();
    call_import("abort", 0);
}

static void child_unbound(void *unused) {
    (void)unused;
    setup();
    call_import("dyld_stub_binding_helper", 0);
}

TEST(libc_abort_and_the_binding_helper_crash) {
    char out[8192];
    CHECK_EQ(test_run_child(child_abort, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "the game called abort()");
    CHECK_EQ(test_run_child(child_unbound, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "a lazy pointer was not bound");
}
