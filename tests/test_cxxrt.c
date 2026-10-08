#include "test.h"

#include "cxxrt.h"
#include "harness.h"
#include "loader.h"
#include "memmgr.h"

static const char *const names[] = {
    "_Znwm", "_Znam", "_ZdlPv", "_ZdaPv", "__cxa_guard_acquire", "__cxa_guard_release",
    "__cxa_pure_virtual", "__cxa_allocate_exception", "__cxa_throw", "__cxa_rethrow",
    "_Unwind_Resume", "__cxa_begin_catch", "__cxa_end_catch", "__gxx_personality_v0",
};

static void setup(void) {
    harness_init_direct(names, sizeof names / sizeof names[0]);
    mm_init();
    cxxrt_init();
    cxxrt_register();
}

TEST(cxxrt_new_and_delete_use_the_pointer_heap) {
    setup();
    uint32_t p = call_import("_Znwm", 1, 24), a = call_import("_Znam", 1, 0);
    CHECK(mm_is_ptr(p) && mm_is_ptr(a));
    CHECK_EQ(mm_ptr_size(p), 24);
    call_import("_ZdlPv", 1, p);
    call_import("_ZdaPv", 1, a);
    call_import("_ZdlPv", 1, 0);
    CHECK(!mm_is_ptr(p) && !mm_is_ptr(a));
}

TEST(cxxrt_guard_runs_once) {
    setup();
    uint32_t guard = scratch(8);
    CHECK_EQ(call_import("__cxa_guard_acquire", 1, guard), 1);
    call_import("__cxa_guard_release", 1, guard);
    CHECK_EQ(call_import("__cxa_guard_acquire", 1, guard), 0);
}

TEST(cxxrt_data_symbols) {
    setup();
    uint32_t a = cxxrt_data_symbol("_ZTVN10__cxxabiv117__class_type_infoE");
    uint32_t b = cxxrt_data_symbol("_ZTVN10__cxxabiv120__si_class_type_infoE");
    uint32_t c = cxxrt_data_symbol("_ZTVN10__cxxabiv121__vmi_class_type_infoE");
    CHECK(a && b && c && a != b && b != c);
    CHECK(mm_is_ptr(a));
    CHECK_EQ(cxxrt_data_symbol("__cxa_pure_virtual"), IMAGE_SYMBOL_CODE);
    CHECK_EQ(cxxrt_data_symbol("__gxx_personality_v0"), IMAGE_SYMBOL_CODE);
    CHECK_EQ(cxxrt_data_symbol("_ZTISt9exception"), 0);
}

TEST(cxxrt_reads_type_names) {
    char out[64];
    cxxrt_type_name("N2RT12TOSExceptionE", out, sizeof out);
    CHECK_STR(out, "RT::TOSException");
    cxxrt_type_name("14TUserException", out, sizeof out);
    CHECK_STR(out, "TUserException");
    cxxrt_type_name("PKc", out, sizeof out);
    CHECK_STR(out, "PKc");
    cxxrt_type_name("N2RT99E", out, sizeof out);
    CHECK_STR(out, "N2RT99E");
}

static void child_throw(void *unused) {
    (void)unused;
    setup();
    /* An RT::TOSException: type_info is {vtable, name}. */
    uint32_t name = scratch(32), tinfo = scratch(8);
    gm_write_cstr(name, "N2RT12TOSExceptionE");
    gm_w32(tinfo, cxxrt_data_symbol("_ZTVN10__cxxabiv120__si_class_type_infoE") + 8);
    gm_w32(tinfo + 4, name);
    uint32_t obj = call_import("__cxa_allocate_exception", 1, 16);
    gm_w32(obj + 4, 0xFFFFFFDA); /* -38 */
    gm_w32(obj + 8, 7);
    call_import("__cxa_throw", 3, obj, tinfo, 0);
}

/* Review Focus 3: an exception is never silent. */
TEST(cxxrt_throw_crashes_naming_the_type) {
    char out[8192];
    CHECK_EQ(test_run_child(child_throw, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "loony: crash: the game threw RT::TOSException at ");
    CHECK_CONTAINS(out, "thrown object: 00000000 ffffffda 00000007 00000000");
}

static void child_call(void *name) {
    setup();
    call_import(name, 1, 0);
}

TEST(cxxrt_pure_virtual_crashes) {
    char out[8192];
    CHECK_EQ(test_run_child(child_call, (void *)"__cxa_pure_virtual", out, sizeof out), 2);
    CHECK_CONTAINS(out, "pure virtual function called");
}

TEST(cxxrt_unwinding_crashes) {
    char out[8192];
    CHECK_EQ(test_run_child(child_call, (void *)"_Unwind_Resume", out, sizeof out), 2);
    CHECK_CONTAINS(out, "_Unwind_Resume at ");
    CHECK_EQ(test_run_child(child_call, (void *)"__cxa_rethrow", out, sizeof out), 2);
    CHECK_CONTAINS(out, "the game rethrew an exception");
    CHECK_EQ(test_run_child(child_call, (void *)"__cxa_begin_catch", out, sizeof out), 2);
    CHECK_CONTAINS(out, "nothing was thrown");
    CHECK_EQ(test_run_child(child_call, (void *)"__gxx_personality_v0", out, sizeof out), 2);
    CHECK_CONTAINS(out, "__gxx_personality_v0 called");
}
