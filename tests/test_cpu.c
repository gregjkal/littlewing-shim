#include "test.h"

#include "ppc.h"

#define CODE GUEST_IMAGE_BASE

TEST(cpu_runs_until_return) {
    fresh_machine();
    uint32_t code[] = {
        0x38600005, /* li   r3,5 */
        0x38630002, /* addi r3,r3,2 */
        0x4E800020, /* blr */
    };
    put_words(CODE, code, 3);
    cpu_stop s = cpu_run(CODE);
    CHECK_EQ(s.kind, CPU_STOP_RETURN);
    CHECK_EQ(cpu_gpr(3), 7);
}

TEST(cpu_stops_at_trap_address) {
    fresh_machine();
    uint32_t code[] = {
        0x3D800700, /* lis   r12,0x0700 */
        0x618C0004, /* ori   r12,r12,4 */
        0x7D8903A6, /* mtctr r12 */
        0x4E800420, /* bctr */
    };
    put_words(CODE, code, 4);
    cpu_stop s = cpu_run(CODE);
    CHECK_EQ(s.kind, CPU_STOP_TRAP);
    CHECK_EQ(s.addr, GUEST_TRAP_ADDR(1));
}

TEST(cpu_resumes_after_trap_at_lr) {
    fresh_machine();
    uint32_t code[] = {
        0x7FE802A6, /* mflr  r31 */
        0x3D800700, /* lis   r12,0x0700 */
        0x7D8903A6, /* mtctr r12 */
        0x4E800421, /* bctrl */
        0x38630001, /* addi  r3,r3,1 */
        0x7FE803A6, /* mtlr  r31 */
        0x4E800020, /* blr */
    };
    put_words(CODE, code, 7);
    cpu_stop s = cpu_run(CODE);
    CHECK_EQ(s.kind, CPU_STOP_TRAP);
    CHECK_EQ(s.addr, GUEST_TRAP_ADDR(0));
    CHECK_EQ(cpu_lr(), CODE + 16);
    cpu_set_gpr(3, 41);
    s = cpu_run(cpu_lr());
    CHECK_EQ(s.kind, CPU_STOP_RETURN);
    CHECK_EQ(cpu_gpr(3), 42);
}

TEST(cpu_reports_unmapped_read_as_fault) {
    fresh_machine();
    uint32_t code[] = {
        0x3C800580, /* lis r4,0x0580 */
        0x80640000, /* lwz r3,0(r4) */
        0x4E800020, /* blr */
    };
    put_words(CODE, code, 3);
    cpu_stop s = cpu_run(CODE);
    CHECK_EQ(s.kind, CPU_STOP_FAULT);
    CHECK_EQ(s.addr, 0x05800000u);
    CHECK_CONTAINS(s.detail, "0x05800000");
}

/* Review Focus 4: stack overflow into the guard gap. */
TEST(cpu_reports_stack_overflow_as_fault) {
    fresh_machine();
    uint32_t code[] = {
        0x3C800600, /* lis r4,0x0600 */
        0x9064FFF0, /* stw r3,-16(r4) */
        0x4E800020, /* blr */
    };
    put_words(CODE, code, 3);
    cpu_stop s = cpu_run(CODE);
    CHECK_EQ(s.kind, CPU_STOP_FAULT);
    CHECK_EQ(s.addr, 0x05FFFFF0u);
}

/* Review Focus 3: calling a null function pointer. */
TEST(cpu_reports_jump_to_zero_as_fault) {
    fresh_machine();
    uint32_t code[] = {
        0x39800000, /* li    r12,0 */
        0x7D8903A6, /* mtctr r12 */
        0x4E800420, /* bctr */
    };
    put_words(CODE, code, 3);
    cpu_stop s = cpu_run(CODE);
    CHECK_EQ(s.kind, CPU_STOP_FAULT);
}

TEST(cpu_reports_illegal_instruction_as_fault) {
    fresh_machine();
    uint32_t code[] = {0x00000000};
    put_words(CODE, code, 1);
    cpu_stop s = cpu_run(CODE);
    CHECK_EQ(s.kind, CPU_STOP_FAULT);
}

TEST(cpu_floating_point_is_enabled) {
    fresh_machine();
    uint32_t code[] = {
        0xFC22182A, /* fadd f1,f2,f3 */
        0x4E800020, /* blr */
    };
    put_words(CODE, code, 2);
    cpu_set_fpr(2, 2.5);
    cpu_set_fpr(3, 4.0);
    cpu_stop s = cpu_run(CODE);
    CHECK_EQ(s.kind, CPU_STOP_RETURN);
    CHECK(cpu_fpr(1) == 6.5);
}

TEST(cpu_save_restore_round_trip) {
    fresh_machine();
    cpu_set_gpr(3, 1);
    cpu_set_lr(0x1234);
    cpu_context *ctx = cpu_save();
    cpu_set_gpr(3, 2);
    cpu_set_lr(0x5678);
    cpu_restore(ctx);
    CHECK_EQ(cpu_gpr(3), 1);
    CHECK_EQ(cpu_lr(), 0x1234);
}
