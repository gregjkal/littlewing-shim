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
