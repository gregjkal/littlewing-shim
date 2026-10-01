#include "test.h"

#include <unistd.h>

#include "harness.h"
#include "memmgr.h"

static const char *const names[] = {
    "NewPtr", "NewPtrClear", "DisposePtr", "GetPtrSize", "SetPtrSize", "NewHandleClear",
    "DisposeHandle", "GetHandleSize", "RecoverHandle", "HLock", "HUnlock", "HPurge",
    "HNoPurge", "MoveHHi", "HGetState", "HSetState", "MemError",
};

static void setup(void) {
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    mm_register();
}

static int16_t mem_error(void) {
    return (int16_t)call_import("MemError", 0);
}

TEST(mmcall_new_ptr_and_size) {
    setup();
    uint32_t p = call_import("NewPtrClear", 1, 40u);
    CHECK(mm_is_ptr(p));
    CHECK_EQ(mem_error(), 0);
    CHECK_EQ(call_import("GetPtrSize", 1, p), 40);
    call_import("SetPtrSize", 2, p, 8u);
    CHECK_EQ(mem_error(), 0);
    CHECK_EQ(call_import("GetPtrSize", 1, p), 8);
    call_import("DisposePtr", 1, p);
    CHECK(!mm_is_ptr(p));
}

TEST(mmcall_out_of_memory_sets_mem_error) {
    setup();
    CHECK_EQ(call_import("NewPtr", 1, 0x7FFFFFFFu), 0);
    CHECK_EQ(mem_error(), MM_MEM_FULL_ERR);
    CHECK_EQ(call_import("NewHandleClear", 1, 0x7FFFFFFFu), 0);
    CHECK_EQ(mem_error(), MM_MEM_FULL_ERR);
    call_import("NewPtr", 1, 4u);
    CHECK_EQ(mem_error(), 0);
}

TEST(mmcall_set_ptr_size_failure_sets_mem_error) {
    setup();
    uint32_t p = call_import("NewPtr", 1, 16u);
    call_import("NewPtr", 1, 16u);
    call_import("SetPtrSize", 2, p, 4096u);
    CHECK_EQ(mem_error(), MM_MEM_FULL_ERR);
}

TEST(mmcall_handle_lifecycle) {
    setup();
    uint32_t h = call_import("NewHandleClear", 1, 24u);
    CHECK(mm_is_handle(h));
    CHECK_EQ(call_import("GetHandleSize", 1, h), 24);
    uint32_t d = gm_r32(h);
    CHECK_EQ(call_import("RecoverHandle", 1, d), h);
    call_import("HLock", 1, h);
    call_import("HPurge", 1, h);
    CHECK_EQ(call_import("HGetState", 1, h) & 0xFF, MM_STATE_LOCKED | MM_STATE_PURGEABLE);
    call_import("HUnlock", 1, h);
    call_import("HNoPurge", 1, h);
    call_import("MoveHHi", 1, h);
    CHECK_EQ(call_import("HGetState", 1, h), 0);
    call_import("HSetState", 2, h, (uint32_t)MM_STATE_LOCKED);
    CHECK_EQ(call_import("HGetState", 1, h), (uint32_t)(int32_t)(int8_t)MM_STATE_LOCKED);
    CHECK_EQ(gm_r32(h), d);
    call_import("DisposeHandle", 1, h);
    CHECK(!mm_is_handle(h));
    CHECK_EQ(mem_error(), 0);
}

TEST(mmcall_null_handle_sets_nil_handle_error) {
    setup();
    CHECK_EQ(call_import("GetHandleSize", 1, 0u), 0);
    CHECK_EQ(mem_error(), MM_NIL_HANDLE_ERR);
    call_import("HLock", 1, 0u);
    CHECK_EQ(mem_error(), MM_NIL_HANDLE_ERR);
    call_import("DisposePtr", 1, 0u);
    CHECK_EQ(mem_error(), 0);
}

static void child_double_free(void *unused) {
    (void)unused;
    setup();
    uint32_t p = call_import("NewPtr", 1, 16u);
    call_import("DisposePtr", 1, p);
    call_import("DisposePtr", 1, p);
}

/* Review Focus 1: a double free stops with a crash report. */
TEST(mmcall_double_free_crashes) {
    char out[16384];
    int status = test_run_child(child_double_free, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: crash: DisposePtr: 0x");
    CHECK_CONTAINS(out, "is not an allocated pointer");
}

static void child_bad_handle(void *unused) {
    (void)unused;
    setup();
    uint32_t p = call_import("NewPtr", 1, 16u);
    call_import("GetHandleSize", 1, p);
}

TEST(mmcall_pointer_used_as_handle_crashes) {
    char out[16384];
    int status = test_run_child(child_bad_handle, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "GetHandleSize: 0x");
    CHECK_CONTAINS(out, "is not a handle");
}

static void child_dispose_resource(void *unused) {
    (void)unused;
    setup();
    uint32_t h = call_import("NewHandleClear", 1, 16u);
    mm_set_handle_state(h, MM_STATE_RESOURCE);
    call_import("DisposeHandle", 1, h);
}

TEST(mmcall_dispose_handle_on_a_resource_crashes) {
    char out[16384];
    int status = test_run_child(child_dispose_resource, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "is a resource handle (use ReleaseResource)");
}

/* Overwrites the capacity word of the free block that follows a 16-byte
   pointer, as a guest writing past the end of that pointer would. */
static void child_clobbered_header(void *arg) {
    alarm(5); /* a hang becomes a signal, not a stuck test */
    setup();
    uint32_t a = call_import("NewPtr", 1, 16u);
    uint32_t b = call_import("NewPtr", 1, 16u);
    call_import("DisposePtr", 1, b);
    gm_w32(a + 16 + 4, *(uint32_t *)arg);
    call_import("NewPtr", 1, 64u);
}

TEST(mmcall_clobbered_header_with_odd_size_crashes) {
    char out[16384];
    uint32_t cap = 5;
    int status = test_run_child(child_clobbered_header, &cap, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: crash: heap block header at 0x");
    CHECK_CONTAINS(out, "is corrupt");
}

TEST(mmcall_clobbered_header_with_wrapping_size_crashes) {
    char out[16384];
    uint32_t cap = 0xFFFFFFF0u;
    int status = test_run_child(child_clobbered_header, &cap, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "is corrupt");
}
