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
