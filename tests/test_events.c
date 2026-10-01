#include "test.h"

#include <stdlib.h>

#include "asm.h"
#include "events.h"
#include "harness.h"
#include "misc.h"
#include "util.h"

static const char *const names[] = {
    "NewEventHandlerUPP", "GetApplicationEventTarget", "GetEventDispatcherTarget",
    "GetWindowEventTarget", "InstallEventHandler", "InstallStandardEventHandler",
    "NewEventLoopTimerUPP", "GetMainEventLoop", "InstallEventLoopTimer", "RemoveEventLoopTimer",
    "RunApplicationEventLoop", "QuitApplicationEventLoop", "Delay", "ReceiveNextEvent",
};

#define PROC    (GUEST_IMAGE_BASE + 0x200)
#define TV_PROC (GUEST_IMAGE_BASE + 0x8100)
#define COUNTER (GUEST_IMAGE_BASE + 0x8200)

static uint32_t tv_of(const char *name) {
    for (uint32_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(names[i], name) == 0)
            return HARNESS_TV_BASE + 8 * i;
    return 0;
}

/* A timer proc that adds 1 to COUNTER and calls QuitApplicationEventLoop
   once it reaches limit. */
static void emit_counting_proc(int limit) {
    uint32_t tvq = tv_of("QuitApplicationEventLoop");
    uint32_t code[] = {
        PPC_MFLR_R0, PPC_SAVE_LR, PPC_PUSH64,
        ppc_lis(4, COUNTER >> 16), ppc_ori(4, 4, COUNTER & 0xFFFF),
        ppc_lwz(5, 0, 4), ppc_addi(5, 5, 1), ppc_stw(5, 0, 4),
        ppc_cmpwi(5, (int16_t)limit), ppc_blt(7 * 4),
        ppc_lis(12, tvq >> 16), ppc_ori(12, 12, tvq & 0xFFFF),
        ppc_lwz(0, 0, 12), ppc_lwz(2, 4, 12), PPC_MTCTR_R0, PPC_BCTRL,
        PPC_POP64, PPC_LOAD_LR, PPC_MTLR_R0, PPC_BLR,
    };
    put_words(PROC, code, (int)(sizeof code / sizeof code[0]));
    gm_w32(TV_PROC, PROC);
    gm_w32(TV_PROC + 4, 0);
    gm_w32(COUNTER, 0);
}

static void setup(void) {
    harness_init(names, sizeof names / sizeof names[0]);
    misc_init();
    misc_register();
    events_init();
    events_register();
}

static uint32_t install_timer(double delay, double interval) {
    cpu_set_fpr(1, delay);
    cpu_set_fpr(2, interval);
    uint32_t out = scratch(4);
    uint32_t upp = call_import("NewEventLoopTimerUPP", 1, TV_PROC);
    if (call_import("InstallEventLoopTimer", 8, call_import("GetMainEventLoop", 0), 0u, 0u, 0u, 0u,
                    upp, 0x55u, out) != 0)
        fatal("InstallEventLoopTimer failed");
    return gm_r32(out);
}

TEST(events_handlers_are_recorded) {
    setup();
    uint32_t list = scratch(16);
    gm_w32(list, FOURCC('k', 'e', 'y', 'b'));
    gm_w32(list + 4, 1);
    gm_w32(list + 8, FOURCC('k', 'e', 'y', 'b'));
    gm_w32(list + 12, 3);
    uint32_t out = scratch(4);
    uint32_t target = call_import("GetApplicationEventTarget", 0);
    CHECK_EQ(call_import("InstallEventHandler", 6, target, 0x1450u, 2u, list, 0x99u, out), 0);
    CHECK(gm_r32(out) != 0);
    const ev_handler *h;
    CHECK_EQ(events_handlers(&h), 1);
    CHECK_EQ(h[0].target, EV_APPLICATION_TARGET);
    CHECK_EQ(h[0].handler, 0x1450);
    CHECK_EQ(h[0].user_data, 0x99);
    CHECK_EQ(h[0].ntypes, 2);
    CHECK_EQ(h[0].types[1][1], 3);
    CHECK_EQ(call_import("NewEventHandlerUPP", 1, 0x1234u), 0x1234);
    uint32_t w1 = call_import("GetWindowEventTarget", 1, 0x01000100u);
    uint32_t w2 = call_import("GetWindowEventTarget", 1, 0x01000200u);
    CHECK(w1 != w2);
    CHECK(!events_has_standard_handler(w1));
    call_import("InstallStandardEventHandler", 1, w1);
    CHECK(events_has_standard_handler(w1));
    CHECK_EQ(call_import("GetEventDispatcherTarget", 0), EV_DISPATCHER_TARGET);
}

TEST(events_run_loop_fires_timers_until_quit) {
    setup();
    emit_counting_proc(3);
    double t0 = misc_seconds();
    install_timer(0.0, 0.02);
    CHECK_EQ(events_active_timers(), 1);
    call_import("RunApplicationEventLoop", 0);
    CHECK_EQ(gm_r32(COUNTER), 3);
    double elapsed = misc_seconds() - t0;
    CHECK(elapsed >= 0.035); /* fires at 0, 0.02 and 0.04 */
    CHECK(elapsed < 0.5);
}

TEST(events_one_shot_timer_fires_once) {
    setup();
    emit_counting_proc(100);
    install_timer(0.0, 0.0);
    events_pump();
    events_pump();
    CHECK_EQ(gm_r32(COUNTER), 1);
    CHECK_EQ(events_active_timers(), 0);
}

TEST(events_remove_timer) {
    setup();
    emit_counting_proc(100);
    uint32_t t = install_timer(10.0, 1.0);
    CHECK_EQ(call_import("RemoveEventLoopTimer", 1, t), 0);
    CHECK_EQ(events_active_timers(), 0);
}

TEST(events_delay_pumps_timers) {
    setup();
    misc_set_idle(events_pump);
    emit_counting_proc(100);
    install_timer(0.0, 0.0);
    call_import("Delay", 2, 2u, 0u);
    misc_set_idle(NULL);
    CHECK_EQ(gm_r32(COUNTER), 1);
}

static void child_exit_after(void *unused) {
    (void)unused;
    setenv("LOONY_EXIT_AFTER", "3", 1);
    setup();
    emit_counting_proc(1000);
    install_timer(0.0, 0.01);
    call_import("RunApplicationEventLoop", 0);
    exit(9);
}

TEST(events_exit_after_ends_the_run_cleanly) {
    char out[4096];
    CHECK_EQ(test_run_child(child_exit_after, NULL, out, sizeof out), 0);
    CHECK_CONTAINS(out, "loony: exiting after 3 ticks (LOONY_EXIT_AFTER)");
}

static void child_bad_timer(void *unused) {
    (void)unused;
    setup();
    call_import("RemoveEventLoopTimer", 1, 0x1234u);
}

TEST(events_removing_an_unknown_timer_crashes) {
    char out[16384];
    CHECK_EQ(test_run_child(child_bad_timer, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "RemoveEventLoopTimer: 0x00001234 is not a timer");
}

TEST(events_receive_next_event_times_out) {
    setup();
    uint32_t out = scratch(4);
    gm_w32(out, 0xFFFFFFFFu);
    cpu_set_fpr(1, 0.03);
    double t0 = misc_seconds();
    CHECK_EQ((int32_t)call_import("ReceiveNextEvent", 6, 0u, 0u, 0u, 0u, 1u, out),
             EV_LOOP_TIMED_OUT_ERR);
    CHECK(misc_seconds() - t0 >= 0.03);
    CHECK_EQ(gm_r32(out), 0);
    cpu_set_fpr(1, 0.0);
    CHECK_EQ((int32_t)call_import("ReceiveNextEvent", 6, 0u, 0u, 0u, 0u, 1u, out),
             EV_LOOP_TIMED_OUT_ERR);
}
