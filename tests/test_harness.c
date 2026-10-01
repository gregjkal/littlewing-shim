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
