#include "test.h"

#include <stdlib.h>
#include <unistd.h>

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

static void child_stored_value_over_released(void *unused) {
    (void)unused;
    setup();
    uint32_t k = str("Name"), v = str("Greg");
    call_import("CFPreferencesSetAppValue", 3, k, v, app());
    call_import("CFRelease", 1, v);
    call_import("CFRelease", 1, v); /* one too many: the stored value is freed */
    call_import("CFPreferencesCopyAppValue", 2, k, app());
}

TEST(cf_prefs_over_released_value_crashes_with_a_report) {
    char out[16384];
    int status = test_run_child(child_stored_value_over_released, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: crash: CFPreferencesCopyAppValue: 0x08000");
    CHECK_CONTAINS(out, "is not a live CF object");
}

static void child_stored_number_over_released(void *unused) {
    (void)unused;
    setup();
    uint32_t k = str("Level"), v = num(3);
    call_import("CFPreferencesSetAppValue", 3, k, v, app());
    call_import("CFRelease", 1, v);
    call_import("CFRelease", 1, v);
    call_import("CFPreferencesGetAppIntegerValue", 3, k, app(), 0u);
}

TEST(cf_prefs_over_released_number_crashes_with_a_report) {
    char out[16384];
    int status = test_run_child(child_stored_number_over_released, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: crash: CFPreferencesGetAppIntegerValue: 0x08000");
}

/* ---- the preferences file ---- */

static char prefs_dir[1024], prefs_path[1100];

static void file_setup(void) {
    setup();
    test_tmp_dir(prefs_dir, sizeof prefs_dir);
    snprintf(prefs_path, sizeof prefs_path, "%s/sub/folder/prefs.plist", prefs_dir);
    cf_load_prefs(prefs_path);
}

static void set_str(const char *key, const char *value) {
    uint32_t k = str(key), v = str(value);
    call_import("CFPreferencesSetAppValue", 3, k, v, app());
    call_import("CFRelease", 1, v);
    call_import("CFRelease", 1, k);
}

static void get_str(const char *key, char *out, size_t cap) {
    out[0] = '\0';
    uint32_t v = call_import("CFPreferencesCopyAppValue", 2, str(key), app());
    if (!v)
        return;
    uint32_t buf = scratch((uint32_t)cap);
    call_import("CFStringGetCString", 4, v, buf, (uint32_t)cap, 0u);
    gm_read_cstr(buf, out, cap);
    call_import("CFRelease", 1, v);
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

TEST(cf_prefs_survive_synchronize_and_reload) {
    file_setup();
    set_str("highscore name 1", "CAF\x8e"); /* Mac Roman e-acute */
    uint32_t k = str("highscore 1");
    call_import("CFPreferencesSetAppValue", 3, k, num(123456), app());
    set_str("gone", "x");
    call_import("CFPreferencesSetAppValue", 3, str("gone"), 0u, app());
    CHECK_EQ(call_import("CFPreferencesAppSynchronize", 1, app()), 1);

    size_t len;
    char *xml = (char *)read_file(prefs_path, &len);
    CHECK(xml != NULL);
    xml = realloc(xml, len + 1);
    xml[len] = '\0';
    CHECK_CONTAINS(xml, "<key>highscore name 1</key>");
    CHECK_CONTAINS(xml, "<string>CAF\xc3\xa9</string>"); /* UTF-8 in the file */
    CHECK_CONTAINS(xml, "<integer>123456</integer>");
    CHECK(!strstr(xml, "gone"));
    free(xml);

    cf_init();
    cf_load_prefs(prefs_path);
    char name[64];
    get_str("highscore name 1", name, sizeof name);
    CHECK_STR(name, "CAF\x8e");
    uint32_t valid = scratch(1);
    CHECK_EQ(call_import("CFPreferencesGetAppIntegerValue", 3, str("highscore 1"), app(), valid), 123456);
    CHECK_EQ(gm_r8(valid), 1);
    test_remove_tree(prefs_dir);
}

TEST(cf_prefs_start_empty_without_a_file) {
    file_setup();
    char name[64];
    get_str("highscore name 1", name, sizeof name);
    CHECK_STR(name, "");
    CHECK_EQ(cf_live_objects(), 1 + 1); /* the application ID and the key just made */
    test_remove_tree(prefs_dir);
}

static void child_bad_file(void *unused) {
    (void)unused;
    setup();
    cf_load_prefs(prefs_path);
}

/* Review Focus 1: a damaged preferences file is kept, not overwritten. */
TEST(cf_prefs_set_a_damaged_file_aside) {
    setup();
    test_tmp_dir(prefs_dir, sizeof prefs_dir);
    snprintf(prefs_path, sizeof prefs_path, "%s/sub/folder/prefs.plist", prefs_dir);
    char sub[1100];
    snprintf(sub, sizeof sub, "%s/sub/folder", prefs_dir);
    CHECK(make_dirs(sub));
    write_text(prefs_path, "this is not a plist");
    char out[16384];
    CHECK_EQ(test_run_child(child_bad_file, NULL, out, sizeof out), 0);
    CHECK_CONTAINS(out, "is not a property list dictionary; moved it to");
    char bad[1200];
    snprintf(bad, sizeof bad, "%s.bad", prefs_path);
    size_t len;
    uint8_t *kept = read_file(bad, &len);
    CHECK(kept != NULL);
    CHECK_EQ(len, strlen("this is not a plist"));
    free(kept);
    CHECK(access(prefs_path, F_OK) != 0);
    test_remove_tree(prefs_dir);
}

static void child_odd_values(void *unused) {
    (void)unused;
    setup();
    cf_load_prefs(prefs_path);
    char v[64];
    get_str("name", v, sizeof v);
    if (strcmp(v, "MIO") != 0)
        exit(3);
    if (call_import("CFPreferencesCopyAppValue", 2, str("ratio"), app()) != 0)
        exit(4);
}

TEST(cf_prefs_skip_values_that_arent_strings_or_integers) {
    setup();
    test_tmp_dir(prefs_dir, sizeof prefs_dir);
    snprintf(prefs_path, sizeof prefs_path, "%s/sub/folder/prefs.plist", prefs_dir);
    char sub[1100];
    snprintf(sub, sizeof sub, "%s/sub/folder", prefs_dir);
    CHECK(make_dirs(sub));
    write_text(prefs_path, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>"
                           "<key>name</key><string>MIO</string><key>ratio</key><real>1.5</real>"
                           "<key>on</key><true/></dict></plist>\n");
    char out[16384];
    CHECK_EQ(test_run_child(child_odd_values, NULL, out, sizeof out), 0);
    CHECK_CONTAINS(out, "skipped \"ratio\" (not a string or an integer)");
    CHECK_CONTAINS(out, "skipped \"on\"");
    test_remove_tree(prefs_dir);
}

static void child_unwritable(void *unused) {
    (void)unused;
    setup();
    cf_load_prefs("/dev/null/loony/prefs.plist");
    set_str("a", "b");
    if (call_import("CFPreferencesAppSynchronize", 1, app()) != 0)
        exit(3);
}

TEST(cf_prefs_synchronize_reports_a_write_failure) {
    char out[16384];
    CHECK_EQ(test_run_child(child_unwritable, NULL, out, sizeof out), 0);
    CHECK_CONTAINS(out, "loony: preferences: can't create the folder for /dev/null/loony/prefs.plist");
}
