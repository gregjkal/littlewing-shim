#include "test.h"

#include <stdlib.h>

#include "ppc.h"
#include "trap.h"

#define OUTER    GUEST_IMAGE_BASE
#define INNER    (GUEST_IMAGE_BASE + 0x100)
#define TV_IMP0  (GUEST_IMAGE_BASE + 0x8000)
#define TV_IMP1  (GUEST_IMAGE_BASE + 0x8008)
#define TV_OUTER (GUEST_IMAGE_BASE + 0x8010)
#define TV_INNER (GUEST_IMAGE_BASE + 0x8018)

/* A guest function that calls the function whose transition vector is at tv
   (CFM-style: load code and TOC from the TV, bctrl), then adds 1 to r3. */
static void emit_caller(uint32_t at, uint32_t tv) {
    uint32_t code[] = {
        0x7C0802A6,                  /* mflr  r0 */
        0x90010008,                  /* stw   r0,8(r1) */
        0x9421FFC0,                  /* stwu  r1,-64(r1) */
        0x3D800000 | (tv >> 16),     /* lis   r12,hi(tv) */
        0x618C0000 | (tv & 0xFFFF),  /* ori   r12,r12,lo(tv) */
        0x800C0000,                  /* lwz   r0,0(r12) */
        0x804C0004,                  /* lwz   r2,4(r12) */
        0x7C0903A6,                  /* mtctr r0 */
        0x4E800421,                  /* bctrl */
        0x38630001,                  /* addi  r3,r3,1 */
        0x38210040,                  /* addi  r1,r1,64 */
        0x80010008,                  /* lwz   r0,8(r1) */
        0x7C0803A6,                  /* mtlr  r0 */
        0x4E800020,                  /* blr */
    };
    put_words(at, code, (int)(sizeof code / sizeof code[0]));
}

static void set_tv(uint32_t at, uint32_t code) {
    gm_w32(at, code);
    gm_w32(at + 4, 0);
}

static void setup(const char *const *names, uint32_t n) {
    fresh_machine();
    trap_init(n, names, GUEST_IMAGE_BASE, 0x10000);
    set_tv(TV_IMP0, GUEST_TRAP_ADDR(0));
    set_tv(TV_IMP1, GUEST_TRAP_ADDR(1));
    set_tv(TV_OUTER, OUTER);
    set_tv(TV_INNER, INNER);
    emit_caller(OUTER, TV_IMP0);
}

static void h_double(void) {
    trap_return(trap_arg(0) * 2);
}

static void h_call_inner(void) {
    uint32_t a = trap_arg(0);
    trap_return(guest_call(TV_INNER, 1, &a));
}

TEST(trap_calls_handler_and_resumes) {
    static const char *const names[] = {"TestDouble"};
    setup(names, 1);
    trap_register("TestDouble", h_double);
    trap_register("NotImported", h_double); /* ignored */
    uint32_t arg = 5;
    CHECK_EQ(guest_call(TV_OUTER, 1, &arg), 11);
    CHECK_STR(trap_import_name(0), "TestDouble");
}

TEST(trap_nested_guest_call) {
    static const char *const names[] = {"CallInner"};
    setup(names, 1);
    uint32_t inner[] = {0x38630064 /* addi r3,r3,100 */, 0x4E800020 /* blr */};
    put_words(INNER, inner, 2);
    trap_register("CallInner", h_call_inner);
    uint32_t sp = cpu_gpr(1), arg = 5;
    CHECK_EQ(guest_call(TV_OUTER, 1, &arg), 106);
    CHECK_EQ(cpu_gpr(1), sp);
}

static void child_unimplemented(void *unused) {
    (void)unused;
    static const char *const names[] = {"FooBar"};
    setup(names, 1);
    uint32_t arg = 5;
    guest_call(TV_OUTER, 1, &arg);
}

TEST(trap_unimplemented_import_crashes) {
    char out[16384];
    int status = test_run_child(child_unimplemented, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: crash: unimplemented import FooBar");
    CHECK_CONTAINS(out, "FooBar(0x00000005");
    CHECK_CONTAINS(out, "code+0x");
}

/* Review Focus 5: crash while nested inside a callback. */
static void child_nested_missing(void *unused) {
    (void)unused;
    static const char *const names[] = {"CallInner", "Missing"};
    setup(names, 2);
    emit_caller(INNER, TV_IMP1);
    trap_register("CallInner", h_call_inner);
    uint32_t arg = 5;
    guest_call(TV_OUTER, 1, &arg);
}

TEST(trap_crash_while_nested_reports_depth_and_history) {
    char out[16384];
    int status = test_run_child(child_nested_missing, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "unimplemented import Missing");
    CHECK_CONTAINS(out, "depth 2");
    CHECK_CONTAINS(out, "CallInner(0x00000005");
}

static void child_fault(void *unused) {
    (void)unused;
    static const char *const names[] = {"Unused"};
    setup(names, 1);
    uint32_t code[] = {
        0x3C800580, /* lis r4,0x0580 */
        0x80640000, /* lwz r3,0(r4) */
        0x4E800020, /* blr */
    };
    put_words(INNER, code, 3);
    guest_call(TV_INNER, 0, NULL);
}

TEST(trap_guest_fault_crashes) {
    char out[16384];
    int status = test_run_child(child_fault, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: crash: guest fault");
    CHECK_CONTAINS(out, "0x05800000");
}

static void child_trace(void *unused) {
    (void)unused;
    setenv("LOONY_TRACE", "imports", 1);
    static const char *const names[] = {"TestDouble"};
    setup(names, 1);
    trap_register("TestDouble", h_double);
    uint32_t arg = 5;
    guest_call(TV_OUTER, 1, &arg);
}

TEST(trap_trace_logs_imports) {
    char out[16384];
    int status = test_run_child(child_trace, NULL, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: trace: #0 TestDouble(0x00000005");
    CHECK_CONTAINS(out, "-> 0x0000000a");
}

static void h_read_bad_pointer(void) {
    trap_return(gm_r32(0x05000000u));
}

static void child_handler_bad_pointer(void *unused) {
    (void)unused;
    static const char *const names[] = {"ReadsBadPointer"};
    setup(names, 1);
    trap_register("ReadsBadPointer", h_read_bad_pointer);
    uint32_t arg = 5;
    guest_call(TV_OUTER, 1, &arg);
}

TEST(trap_bad_guest_pointer_in_handler_gives_crash_report) {
    char out[16384];
    int status = test_run_child(child_handler_bad_pointer, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: crash: access to unmapped guest address 0x05000000");
    CHECK_CONTAINS(out, "depth 1");
    CHECK_CONTAINS(out, "ReadsBadPointer(0x00000005, ");
}

static void child_bad_tvector(void *unused) {
    (void)unused;
    static const char *const names[] = {"Unused"};
    setup(names, 1);
    guest_call(0x05000000u, 0, NULL);
}

TEST(trap_bad_tvector_gives_crash_report) {
    char out[16384];
    int status = test_run_child(child_bad_tvector, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: crash: access to unmapped guest address 0x05000000");
    CHECK_CONTAINS(out, "last 0 imports");
}

static void child_sp_above_stack(void *unused) {
    (void)unused;
    static const char *const names[] = {"Unused"};
    setup(names, 1);
    cpu_set_gpr(1, GUEST_STACK_TOP + 0x1000);
    guest_call(TV_INNER, 0, NULL);
}

TEST(trap_stack_pointer_above_stack_crashes) {
    char out[16384];
    int status = test_run_child(child_sp_above_stack, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: crash: guest stack pointer 0x06101000 is outside the stack");
}

static void child_sp_zero(void *unused) {
    (void)unused;
    static const char *const names[] = {"Unused"};
    setup(names, 1);
    cpu_set_gpr(1, 0);
    guest_call(TV_INNER, 0, NULL);
}

TEST(trap_stack_pointer_zero_crashes) {
    char out[16384];
    int status = test_run_child(child_sp_zero, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: crash: guest stack pointer 0x00000000 is outside the stack");
}

static void child_stack_exhausted(void *unused) {
    (void)unused;
    static const char *const names[] = {"Unused"};
    setup(names, 1);
    cpu_set_gpr(1, GUEST_STACK_BASE + 0x800);
    guest_call(TV_INNER, 0, NULL);
}

TEST(trap_stack_exhausted_crashes) {
    char out[16384];
    int status = test_run_child(child_stack_exhausted, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: crash: guest stack exhausted");
}

TEST(trap_args_beyond_r10_come_from_the_parameter_area) {
    static const char *const names[] = {"Unused"};
    setup(names, 1);
    uint32_t sp = GUEST_STACK_TOP - 0x1000;
    cpu_set_gpr(1, sp);
    cpu_set_gpr(6, 0x66);
    gm_w32(sp + 24 + 4 * 8, 0x88);
    gm_w32(sp + 24 + 4 * 9, 0x99);
    CHECK_EQ(trap_arg(3), 0x66);
    CHECK_EQ(trap_arg(8), 0x88);
    CHECK_EQ(trap_arg(9), 0x99);
}

static void child_stub(void *unused) {
    (void)unused;
    setenv("LOONY_STUB", "all", 1);
    static const char *const names[] = {"FooBar"};
    setup(names, 1);
    uint32_t arg = 5;
    /* The stub returns 0 and the caller adds 1. */
    if (guest_call(TV_OUTER, 1, &arg) != 1)
        exit(3);
}

TEST(trap_stub_mode_returns_zero_from_unimplemented_imports) {
    char out[16384];
    int status = test_run_child(child_stub, NULL, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: stub: #0 FooBar(0x00000005, ");
    CHECK_CONTAINS(out, ") from code+0x");
}

static void child_stub_other_value(void *unused) {
    (void)unused;
    setenv("LOONY_STUB", "yes", 1);
    static const char *const names[] = {"FooBar"};
    setup(names, 1);
    uint32_t arg = 5;
    guest_call(TV_OUTER, 1, &arg);
}

TEST(trap_stub_mode_needs_the_value_all) {
    char out[16384];
    int status = test_run_child(child_stub_other_value, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: crash: unimplemented import FooBar");
}

static void child_trace_calls(void *unused) {
    (void)unused;
    setenv("LOONY_TRACE", "calls", 1);
    static const char *const names[] = {"CallInner"};
    setup(names, 1);
    uint32_t inner[] = {0x38630064 /* addi r3,r3,100 */, 0x4E800020 /* blr */};
    put_words(INNER, inner, 2);
    trap_register("CallInner", h_call_inner);
    uint32_t arg = 5;
    guest_call(TV_OUTER, 1, &arg);
}

TEST(trap_trace_calls_logs_nested_guest_calls) {
    char out[16384];
    int status = test_run_child(child_trace_calls, NULL, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: trace: call code+0x00000(0x00000005) depth 1");
    CHECK_CONTAINS(out, "loony: trace: call code+0x00100(0x00000005) depth 2");
    CHECK_CONTAINS(out, "loony: trace: return 0x00000069 from code+0x00100 depth 2");
    CHECK_CONTAINS(out, "loony: trace: return 0x0000006a from code+0x00000 depth 1");
}

static void child_trace_lowmem(void *unused) {
    (void)unused;
    setenv("LOONY_TRACE", "lowmem", 1);
    static const char *const names[] = {"Unused"};
    setup(names, 1);
    uint32_t code[] = {
        0x38800123, /* li  r4,0x123 */
        0x90800910, /* stw r4,0x910(0) */
        0x90800910, /* stw r4,0x910(0) again: logged once */
        0x4E800020, /* blr */
    };
    put_words(INNER, code, 4);
    guest_call(TV_INNER, 0, NULL);
}

TEST(trap_trace_lowmem_logs_first_write_per_address) {
    char out[16384];
    int status = test_run_child(child_trace_lowmem, NULL, out, sizeof out);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "loony: lowmem: write 0x0910 = 0x00000123 (4 bytes) at code+0x00104");
    const char *first = strstr(out, "lowmem: write 0x0910");
    CHECK(first && !strstr(first + 1, "lowmem: write 0x0910"));
}

TEST(guest_call_direct_jumps_to_the_address) {
    static const char *const names[] = {"Unused"};
    setup(names, 1);
    uint32_t code[] = {
        0x3860002A, /* li   r3,42 */
        0x7C6C1A14, /* add  r3,r12,r3: r12 holds the callee's address */
        0x4E800020, /* blr */
    };
    put_words(INNER, code, 3);
    cpu_set_gpr(2, 0x5555);
    trap_set_direct_calls(true);
    CHECK_EQ(guest_call(INNER, 0, NULL), INNER + 42);
    CHECK_EQ(cpu_gpr(2), 0x5555);
    trap_set_direct_calls(false);
}

TEST(guest_call_default_still_reads_a_tvector) {
    static const char *const names[] = {"TestDouble"};
    setup(names, 1);
    trap_register("TestDouble", h_double);
    trap_set_direct_calls(true);
    trap_set_direct_calls(false);
    uint32_t arg = 20;
    CHECK_EQ(guest_call(TV_OUTER, 1, &arg), 41);

    /* trap_init turns direct calls off. */
    trap_set_direct_calls(true);
    setup(names, 1);
    trap_register("TestDouble", h_double);
    CHECK_EQ(guest_call(TV_OUTER, 1, &arg), 41);
}
