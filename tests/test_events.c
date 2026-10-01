#include "test.h"

#include <stdlib.h>

#include "asm.h"
#include "events.h"
#include "harness.h"
#include <SDL3/SDL_scancode.h>

#include "keymap.h"
#include "memmgr.h"
#include "misc.h"
#include "script.h"
#include "util.h"

static const char *const names[] = {
    "NewEventHandlerUPP", "GetApplicationEventTarget", "GetEventDispatcherTarget",
    "GetWindowEventTarget", "InstallEventHandler", "InstallStandardEventHandler",
    "NewEventLoopTimerUPP", "GetMainEventLoop", "InstallEventLoopTimer", "RemoveEventLoopTimer",
    "RunApplicationEventLoop", "QuitApplicationEventLoop", "Delay", "ReceiveNextEvent",
    "SendEventToEventTarget", "GetEventKind", "GetEventParameter", "ReleaseEvent",
    "AEInstallEventHandler",
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
    mm_init();
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

TEST(events_timers_fire_only_inside_the_event_loop) {
    setup();
    emit_counting_proc(100);
    install_timer(0.0, 0.0);
    events_pump(); /* outside RunApplicationEventLoop and ReceiveNextEvent */
    CHECK_EQ(gm_r32(COUNTER), 0);
    cpu_set_fpr(1, 0.0);
    uint32_t out = scratch(4);
    uint32_t list = scratch(8);
    gm_w32(list, FOURCC('z', 'z', 'z', 'z'));
    gm_w32(list + 4, 1);
    call_import("ReceiveNextEvent", 6, 1u, list, 0u, 0u, 1u, out); /* fires due timers */
    CHECK_EQ(gm_r32(COUNTER), 1);
    CHECK_EQ(events_active_timers(), 0); /* one-shot */
}

TEST(events_remove_timer) {
    setup();
    emit_counting_proc(100);
    uint32_t t = install_timer(10.0, 1.0);
    CHECK_EQ(call_import("RemoveEventLoopTimer", 1, t), 0);
    CHECK_EQ(events_active_timers(), 0);
}

TEST(events_delay_outside_the_loop_doesnt_fire_timers) {
    setup();
    misc_set_idle(events_pump);
    emit_counting_proc(100);
    install_timer(0.0, 0.0);
    call_import("Delay", 2, 2u, 0u);
    misc_set_idle(NULL);
    CHECK_EQ(gm_r32(COUNTER), 0);
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
    while (events_queued() > 0) { /* the startup kEventAppActivated */
        uint32_t ev = scratch(4);
        cpu_set_fpr(1, 0.0);
        call_import("ReceiveNextEvent", 6, 0u, 0u, 0u, 0u, 1u, ev);
    }
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

/* ---- keyboard events and dispatch ---- */

#define STORE_PROC (GUEST_IMAGE_BASE + 0x400)
#define NH_PROC    (GUEST_IMAGE_BASE + 0x600)
#define QUIT_PROC  (GUEST_IMAGE_BASE + 0x700)
#define TV_STORE   (GUEST_IMAGE_BASE + 0x8110)
#define TV_NH      (GUEST_IMAGE_BASE + 0x8118)
#define TV_QUIT    (GUEST_IMAGE_BASE + 0x8120)
#define STORE      (GUEST_IMAGE_BASE + 0x8300)
#define NH_FLAG    (GUEST_IMAGE_BASE + 0x8304)
#define QUIT_FLAG  (GUEST_IMAGE_BASE + 0x8308)

/* Emits code that loads the transition vector at tv and calls it. */
static int emit_call(uint32_t *code, int n, uint32_t tv) {
    code[n++] = ppc_lis(12, tv >> 16);
    code[n++] = ppc_ori(12, 12, tv & 0xFFFF);
    code[n++] = ppc_lwz(0, 0, 12);
    code[n++] = ppc_lwz(2, 4, 12);
    code[n++] = PPC_MTCTR_R0;
    code[n++] = PPC_BCTRL;
    return n;
}

static void set_tv(uint32_t tv, uint32_t code) {
    gm_w32(tv, code);
    gm_w32(tv + 4, 0);
}

/* Handlers, called as handler(nextHandler, event, userData):
   STORE_PROC: GetEventParameter(event, 'kcod', typeUInt32, NULL, 4, NULL, STORE); returns noErr.
   NH_PROC:    stores 1 at NH_FLAG and returns eventNotHandledErr.
   QUIT_PROC:  an Apple Event handler; stores 1 at QUIT_FLAG and calls QuitApplicationEventLoop. */
static void emit_handlers(void) {
    uint32_t c[40];
    int n = 0;
    c[n++] = PPC_MFLR_R0;
    c[n++] = PPC_SAVE_LR;
    c[n++] = PPC_PUSH64;
    c[n++] = 0x7C832378u; /* mr r3,r4 */
    c[n++] = ppc_lis(4, 0x6B63);
    c[n++] = ppc_ori(4, 4, 0x6F64);
    c[n++] = ppc_lis(5, 0x6D61);
    c[n++] = ppc_ori(5, 5, 0x676E);
    c[n++] = ppc_addi(6, 0, 0);
    c[n++] = ppc_addi(7, 0, 4);
    c[n++] = ppc_addi(8, 0, 0);
    c[n++] = ppc_lis(9, STORE >> 16);
    c[n++] = ppc_ori(9, 9, STORE & 0xFFFF);
    n = emit_call(c, n, tv_of("GetEventParameter"));
    c[n++] = PPC_POP64;
    c[n++] = PPC_LOAD_LR;
    c[n++] = PPC_MTLR_R0;
    c[n++] = ppc_addi(3, 0, 0);
    c[n++] = PPC_BLR;
    put_words(STORE_PROC, c, n);
    uint32_t nh[] = {
        ppc_addi(0, 0, 1), ppc_lis(6, NH_FLAG >> 16), ppc_ori(6, 6, NH_FLAG & 0xFFFF),
        ppc_stw(0, 0, 6), ppc_addi(3, 0, -9874), PPC_BLR,
    };
    put_words(NH_PROC, nh, 6);
    n = 0;
    c[n++] = PPC_MFLR_R0;
    c[n++] = PPC_SAVE_LR;
    c[n++] = PPC_PUSH64;
    c[n++] = ppc_addi(0, 0, 1);
    c[n++] = ppc_lis(6, QUIT_FLAG >> 16);
    c[n++] = ppc_ori(6, 6, QUIT_FLAG & 0xFFFF);
    c[n++] = ppc_stw(0, 0, 6);
    n = emit_call(c, n, tv_of("QuitApplicationEventLoop"));
    c[n++] = PPC_POP64;
    c[n++] = PPC_LOAD_LR;
    c[n++] = PPC_MTLR_R0;
    c[n++] = ppc_addi(3, 0, 0);
    c[n++] = PPC_BLR;
    put_words(QUIT_PROC, c, n);
    set_tv(TV_STORE, STORE_PROC);
    set_tv(TV_NH, NH_PROC);
    set_tv(TV_QUIT, QUIT_PROC);
    gm_w32(STORE, 0);
    gm_w32(NH_FLAG, 0);
    gm_w32(QUIT_FLAG, 0);
}

static void install(uint32_t target, uint32_t tv, uint32_t cls, uint32_t kind) {
    uint32_t list = scratch(8);
    gm_w32(list, cls);
    gm_w32(list + 4, kind);
    call_import("InstallEventHandler", 6, target, tv, 1u, list, 0u, 0u);
}

/* Pulls the next event of (cls, kind), or 0. */
static uint32_t next_event(uint32_t cls, uint32_t kind) {
    uint32_t list = scratch(8), out = scratch(4);
    gm_w32(list, cls);
    gm_w32(list + 4, kind);
    cpu_set_fpr(1, 0.0);
    if (call_import("ReceiveNextEvent", 6, 1u, list, 0u, 0u, 1u, out) != 0)
        return 0;
    return gm_r32(out);
}

static uint32_t param32(uint32_t ev, uint32_t name) {
    uint32_t out = scratch(4);
    call_import("GetEventParameter", 7, ev, name, 0x6D61676Eu, 0u, 4u, 0u, out);
    return gm_r32(out);
}

TEST(events_startup_queues_app_activated) {
    setup();
    CHECK_EQ(events_queued(), 1);
    uint32_t ev = next_event(EV_CLASS_APPLICATION, EV_APP_ACTIVATED);
    CHECK(ev != 0);
    CHECK_EQ(call_import("GetEventKind", 1, ev), EV_APP_ACTIVATED);
    call_import("ReleaseEvent", 1, ev);
    CHECK_EQ(events_queued(), 0);
}

TEST(events_key_down_reaches_the_application_handler) {
    setup();
    emit_handlers();
    install(EV_APPLICATION_TARGET, TV_STORE, EV_CLASS_KEYBOARD, EV_RAW_KEY_DOWN);
    events_post_key(SDL_SCANCODE_Z, true, false);
    uint32_t ev = next_event(EV_CLASS_KEYBOARD, EV_RAW_KEY_DOWN);
    CHECK(ev != 0);
    CHECK_EQ(call_import("SendEventToEventTarget", 2, ev, EV_DISPATCHER_TARGET), 0);
    CHECK_EQ(gm_r32(STORE), 0x06);
    call_import("ReleaseEvent", 1, ev);
}

TEST(events_key_parameters) {
    setup();
    events_post_key(SDL_SCANCODE_LSHIFT, true, false);
    events_post_key(SDL_SCANCODE_SLASH, true, false);
    uint32_t ev = next_event(EV_CLASS_KEYBOARD, EV_RAW_KEY_DOWN);
    CHECK_EQ(param32(ev, 0x6B636F64u), 0x2C); /* 'kcod' */
    CHECK_EQ(param32(ev, 0x6B6D6F64u), KM_SHIFT); /* 'kmod' */
    uint32_t c = scratch(1), type = scratch(4), size = scratch(4);
    CHECK_EQ(call_import("GetEventParameter", 7, ev, 0x6B636872u, 0x54455854u, type, 1u, size, c), 0);
    CHECK_EQ(gm_r8(c), '?'); /* shifted slash */
    CHECK_EQ(gm_r32(type), 0x54455854u);
    CHECK_EQ(gm_r32(size), 1);
    CHECK_EQ((int32_t)call_import("GetEventParameter", 7, ev, 0x2D2D2D2Du, 0x2A2A2A2Au, 0u, 4u,
                                  0u, c), EV_PARAM_NOT_FOUND_ERR);
    call_import("ReleaseEvent", 1, ev);
}

TEST(events_modifier_changes_track_both_sides) {
    setup();
    next_event(EV_CLASS_APPLICATION, EV_APP_ACTIVATED);
    events_post_key(SDL_SCANCODE_LSHIFT, true, false);
    events_post_key(SDL_SCANCODE_RSHIFT, true, false);
    events_post_key(SDL_SCANCODE_LSHIFT, false, false); /* right still down: no change */
    events_post_key(SDL_SCANCODE_LSHIFT, false, false); /* already up: no change */
    events_post_key(SDL_SCANCODE_RSHIFT, false, false);
    CHECK_EQ(events_queued(), 3);
    uint32_t want[3] = {KM_SHIFT, KM_SHIFT | KM_RIGHT_SHIFT, 0};
    for (int i = 0; i < 3; i++) {
        uint32_t ev = next_event(EV_CLASS_KEYBOARD, EV_RAW_KEY_MODIFIERS_CHANGED);
        CHECK(ev != 0);
        CHECK_EQ(param32(ev, 0x6B6D6F64u), want[i]);
        call_import("ReleaseEvent", 1, ev);
    }
}

TEST(events_newest_handler_first_and_not_handled_continues) {
    setup();
    emit_handlers();
    install(EV_APPLICATION_TARGET, TV_STORE, EV_CLASS_KEYBOARD, EV_RAW_KEY_UP);
    install(EV_APPLICATION_TARGET, TV_NH, EV_CLASS_KEYBOARD, EV_RAW_KEY_UP);
    events_post_key(SDL_SCANCODE_SPACE, false, false);
    uint32_t ev = next_event(EV_CLASS_KEYBOARD, EV_RAW_KEY_UP);
    CHECK_EQ(call_import("SendEventToEventTarget", 2, ev, EV_APPLICATION_TARGET), 0);
    CHECK_EQ(gm_r32(NH_FLAG), 1);
    CHECK_EQ(gm_r32(STORE), 0x31);
    call_import("ReleaseEvent", 1, ev);
}

TEST(events_dispatcher_sends_keys_to_the_window_first) {
    setup();
    emit_handlers();
    uint32_t win = call_import("GetWindowEventTarget", 1, 0x01000100u);
    install(EV_APPLICATION_TARGET, TV_STORE, EV_CLASS_KEYBOARD, EV_RAW_KEY_DOWN);
    install(win, TV_NH, EV_CLASS_KEYBOARD, EV_RAW_KEY_DOWN);
    events_post_key(SDL_SCANCODE_RETURN, true, false);
    uint32_t ev = next_event(EV_CLASS_KEYBOARD, EV_RAW_KEY_DOWN);
    CHECK_EQ(call_import("SendEventToEventTarget", 2, ev, EV_DISPATCHER_TARGET), 0);
    CHECK_EQ(gm_r32(NH_FLAG), 1);
    CHECK_EQ(gm_r32(STORE), 0x24);
    call_import("ReleaseEvent", 1, ev);
}

TEST(events_unhandled_event_returns_not_handled) {
    setup();
    events_post_key(SDL_SCANCODE_A, true, false);
    uint32_t ev = next_event(EV_CLASS_KEYBOARD, EV_RAW_KEY_DOWN);
    CHECK_EQ((int32_t)call_import("SendEventToEventTarget", 2, ev, EV_DISPATCHER_TARGET),
             EV_NOT_HANDLED_ERR);
    call_import("ReleaseEvent", 1, ev);
}

TEST(events_peek_leaves_the_event_queued) {
    setup();
    uint32_t out = scratch(4);
    cpu_set_fpr(1, 0.0);
    CHECK_EQ(call_import("ReceiveNextEvent", 6, 0u, 0u, 0u, 0u, 0u, out), 0); /* pull = false */
    CHECK(gm_r32(out) != 0);
    CHECK_EQ(events_queued(), 1);
}

static void child_release_queued(void *unused) {
    (void)unused;
    setup();
    uint32_t out = scratch(4);
    cpu_set_fpr(1, 0.0);
    call_import("ReceiveNextEvent", 6, 0u, 0u, 0u, 0u, 0u, out);
    call_import("ReleaseEvent", 1, gm_r32(out));
}

TEST(events_releasing_a_queued_event_crashes) {
    char out[16384];
    CHECK_EQ(test_run_child(child_release_queued, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "is still in the event queue");
}

static void child_wrong_param_type(void *unused) {
    (void)unused;
    setup();
    events_post_key(SDL_SCANCODE_A, true, false);
    uint32_t ev = next_event(EV_CLASS_KEYBOARD, EV_RAW_KEY_DOWN);
    call_import("GetEventParameter", 7, ev, 0x6B636F64u, 0x54455854u, 0u, 4u, 0u, scratch(4));
}

TEST(events_parameter_of_the_wrong_type_crashes) {
    char out[16384];
    CHECK_EQ(test_run_child(child_wrong_param_type, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "GetEventParameter: can't return parameter 0x6b636f64 as type 0x54455854");
}

TEST(events_run_loop_dispatches_queued_events) {
    setup();
    emit_handlers();
    emit_counting_proc(2);
    install(EV_APPLICATION_TARGET, TV_STORE, EV_CLASS_KEYBOARD, EV_RAW_KEY_DOWN);
    events_post_key(SDL_SCANCODE_Z, true, false);
    install_timer(0.01, 0.01);
    call_import("RunApplicationEventLoop", 0);
    CHECK_EQ(gm_r32(STORE), 0x06);
    CHECK_EQ(events_queued(), 0);
}

static void child_quit_with_handler(void *unused) {
    (void)unused;
    setup();
    emit_handlers();
    emit_counting_proc(1000);
    call_import("AEInstallEventHandler", 5, 0x61657674u, 0x71756974u, TV_QUIT, 0u, 0u);
    install_timer(0.0, 0.01);
    events_request_quit();
    call_import("RunApplicationEventLoop", 0); /* returns once the handler quits the loop */
    exit(gm_r32(QUIT_FLAG) == 1 ? 0 : 3);
}

TEST(events_quit_request_calls_the_apple_event_handler) {
    char out[4096];
    CHECK_EQ(test_run_child(child_quit_with_handler, NULL, out, sizeof out), 0);
    CHECK_CONTAINS(out, "loony: sending the quit Apple Event");
}

static void child_quit_without_handler(void *unused) {
    (void)unused;
    setup();
    emit_counting_proc(1000);
    install_timer(0.0, 0.01);
    events_request_quit();
    call_import("RunApplicationEventLoop", 0);
    exit(7);
}

TEST(events_quit_without_a_handler_exits) {
    char out[4096];
    CHECK_EQ(test_run_child(child_quit_without_handler, NULL, out, sizeof out), 0);
    CHECK_CONTAINS(out, "quit requested; the game has no quit handler");
}

static void child_quit_ignored(void *unused) {
    (void)unused;
    setup();
    events_request_quit();
    for (int i = 0; i < 500; i++) { /* nobody dispatches the event */
        events_pump();
        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);
    }
    exit(7);
}

TEST(events_quit_ignored_for_3_seconds_exits) {
    char out[4096];
    CHECK_EQ(test_run_child(child_quit_ignored, NULL, out, sizeof out), 0);
    CHECK_CONTAINS(out, "the game didn't quit within 3 seconds; exiting");
}

TEST(events_pump_runs_due_script_actions) {
    setup();
    char err[256];
    CHECK(script_parse("0 down z\n0 up z\n1000 down z\n", err, sizeof err));
    events_pump();
    CHECK_EQ(events_queued(), 3); /* activation, down, up */
    CHECK_EQ(script_remaining(), 1);
    CHECK(script_parse("", err, sizeof err));
}

/* ---- review fixes ---- */

#define REL_PROC (GUEST_IMAGE_BASE + 0x800)
#define TV_REL   (GUEST_IMAGE_BASE + 0x8128)

/* A handler that releases the event it was given (an over-release). */
static void emit_releasing_handler(void) {
    uint32_t c[24];
    int n = 0;
    c[n++] = PPC_MFLR_R0;
    c[n++] = PPC_SAVE_LR;
    c[n++] = PPC_PUSH64;
    c[n++] = 0x7C832378u; /* mr r3,r4 */
    n = emit_call(c, n, tv_of("ReleaseEvent"));
    c[n++] = PPC_POP64;
    c[n++] = PPC_LOAD_LR;
    c[n++] = PPC_MTLR_R0;
    c[n++] = ppc_addi(3, 0, 0);
    c[n++] = PPC_BLR;
    put_words(REL_PROC, c, n);
    set_tv(TV_REL, REL_PROC);
}

static void child_release_in_run_loop(void *unused) {
    (void)unused;
    setup();
    emit_handlers();
    emit_releasing_handler();
    emit_counting_proc(2);
    install(EV_APPLICATION_TARGET, TV_REL, EV_CLASS_KEYBOARD, EV_RAW_KEY_DOWN);
    events_post_key(SDL_SCANCODE_Z, true, false);
    install_timer(0.01, 0.01);
    call_import("RunApplicationEventLoop", 0);
}

TEST(events_handler_releasing_its_event_in_the_run_loop_crashes) {
    char out[16384];
    CHECK_EQ(test_run_child(child_release_in_run_loop, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "isn't owned by the caller");
}

static void child_release_after_peek(void *unused) {
    (void)unused;
    setup();
    emit_releasing_handler();
    install(EV_APPLICATION_TARGET, TV_REL, EV_CLASS_APPLICATION, EV_APP_ACTIVATED);
    uint32_t out = scratch(4);
    cpu_set_fpr(1, 0.0);
    call_import("ReceiveNextEvent", 6, 0u, 0u, 0u, 0u, 0u, out); /* peek */
    call_import("SendEventToEventTarget", 2, gm_r32(out), EV_APPLICATION_TARGET);
}

TEST(events_handler_releasing_a_peeked_event_crashes) {
    char out[16384];
    CHECK_EQ(test_run_child(child_release_after_peek, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "ReleaseEvent: 0x0a00");
}

#define WAIT_PROC (GUEST_IMAGE_BASE + 0xA00)
#define TV_WAIT   (GUEST_IMAGE_BASE + 0x8130)
#define SEEN      (GUEST_IMAGE_BASE + 0x8310)

/* A timer proc that calls Delay(2), then records COUNTER in SEEN and quits. */
static void emit_waiting_proc(void) {
    uint32_t c[40];
    int n = 0;
    c[n++] = PPC_MFLR_R0;
    c[n++] = PPC_SAVE_LR;
    c[n++] = PPC_PUSH64;
    c[n++] = ppc_addi(3, 0, 2);
    c[n++] = ppc_addi(4, 0, 0);
    n = emit_call(c, n, tv_of("Delay"));
    c[n++] = ppc_lis(4, COUNTER >> 16);
    c[n++] = ppc_ori(4, 4, COUNTER & 0xFFFF);
    c[n++] = ppc_lwz(5, 0, 4);
    c[n++] = ppc_lis(6, SEEN >> 16);
    c[n++] = ppc_ori(6, 6, SEEN & 0xFFFF);
    c[n++] = ppc_stw(5, 0, 6);
    n = emit_call(c, n, tv_of("QuitApplicationEventLoop"));
    c[n++] = PPC_POP64;
    c[n++] = PPC_LOAD_LR;
    c[n++] = PPC_MTLR_R0;
    c[n++] = PPC_BLR;
    put_words(WAIT_PROC, c, n);
    set_tv(TV_WAIT, WAIT_PROC);
    gm_w32(SEEN, 0xFFFFFFFFu);
}

TEST(events_delay_inside_a_timer_doesnt_fire_other_timers) {
    setup();
    misc_set_idle(events_pump);
    emit_counting_proc(100);
    emit_waiting_proc();
    uint32_t out = scratch(4);
    cpu_set_fpr(1, 0.0);
    cpu_set_fpr(2, 0.0);
    call_import("InstallEventLoopTimer", 8, EV_MAIN_LOOP, 0u, 0u, 0u, 0u, TV_WAIT, 0u, out);
    install_timer(0.0, 0.0); /* the counting timer, due at the same time */
    call_import("RunApplicationEventLoop", 0);
    misc_set_idle(NULL);
    CHECK_EQ(gm_r32(SEEN), 0);    /* it didn't run during the Delay */
    CHECK_EQ(gm_r32(COUNTER), 1); /* it ran once the waiting proc returned */
}
