#include "events.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu.h"
#include "guest_mem.h"
#include "misc.h"
#include "sound.h"
#include "trap.h"
#include "util.h"

#define MAX_STANDARD 8

static struct {
    ev_handler handlers[EV_MAX_HANDLERS];
    int nhandlers;
    uint32_t standard[MAX_STANDARD];
    int nstandard;
    struct {
        bool active;
        uint32_t proc, data;
        double next, interval; /* seconds on the misc clock */
    } timers[EV_MAX_TIMERS];
    bool quit;
    int depth; /* nested RunApplicationEventLoop calls */
    int in_timer; /* > 0 while a timer proc runs */
    ev_present_fn present;
    long exit_after; /* ticks, or 0 */
} E;

void events_init(void) {
    memset(&E, 0, sizeof E);
    const char *s = getenv("LOONY_EXIT_AFTER");
    if (s && *s)
        E.exit_after = strtol(s, NULL, 10);
}

void events_set_present(ev_present_fn fn) { E.present = fn; }

int events_active_timers(void) {
    int n = 0;
    for (int i = 0; i < EV_MAX_TIMERS; i++)
        n += E.timers[i].active;
    return n;
}

static double now_seconds(void) { return misc_seconds(); }

/* Window targets are EV_TAG_BASE + 0x10000 + the window's address / 16,
   which is unique and reversible. */
uint32_t events_window_target(uint32_t window) { return EV_TAG_BASE + 0x10000u + window / 16u; }

int events_handlers(const ev_handler **out) {
    *out = E.handlers;
    return E.nhandlers;
}

bool events_has_standard_handler(uint32_t target) {
    for (int i = 0; i < E.nstandard; i++)
        if (E.standard[i] == target)
            return true;
    return false;
}

static void h_new_event_handler_upp(void) { trap_return(trap_arg(0)); }
static void h_get_application_event_target(void) { trap_return(EV_APPLICATION_TARGET); }
static void h_get_event_dispatcher_target(void) { trap_return(EV_DISPATCHER_TARGET); }
static void h_get_window_event_target(void) { trap_return(events_window_target(trap_arg(0))); }

/* InstallEventHandler(target, handler, numTypes, const EventTypeSpec *list,
   void *userData, EventHandlerRef *outRef) -> OSStatus */
static void h_install_event_handler(void) {
    uint32_t ntypes = trap_arg(2), list = trap_arg(3), out = trap_arg(5);
    if (E.nhandlers == EV_MAX_HANDLERS)
        trap_crash("InstallEventHandler: more than %d handlers", EV_MAX_HANDLERS);
    if (ntypes > 8)
        trap_crash("InstallEventHandler: %u event types (at most 8 supported)", ntypes);
    ev_handler *h = &E.handlers[E.nhandlers];
    h->target = trap_arg(0);
    h->handler = trap_arg(1);
    h->user_data = trap_arg(4);
    h->ntypes = ntypes;
    for (uint32_t i = 0; i < ntypes; i++) {
        h->types[i][0] = gm_r32(list + 8 * i);
        h->types[i][1] = gm_r32(list + 8 * i + 4);
    }
    E.nhandlers++;
    if (out)
        gm_w32(out, EV_TAG_BASE + 0x1000u + (uint32_t)E.nhandlers);
    trap_return(0);
}

static void h_install_standard_event_handler(void) {
    if (E.nstandard < MAX_STANDARD)
        E.standard[E.nstandard++] = trap_arg(0);
    trap_return(0);
}

/* ---- timers and the run loop ---- */

static uint32_t timer_ref(int i) { return EV_TAG_BASE + 0x2000u + (uint32_t)i; }

static void h_new_event_loop_timer_upp(void) { trap_return(trap_arg(0)); }
static void h_get_main_event_loop(void) { trap_return(EV_MAIN_LOOP); }

/* InstallEventLoopTimer(EventLoopRef loop, EventTimerInterval fireDelay,
   EventTimerInterval interval, EventLoopTimerUPP proc, void *data,
   EventLoopTimerRef *outTimer) -> OSStatus. The two intervals are doubles in
   f1 and f2; under the PowerPC calling convention they also take up r4-r7,
   so the remaining arguments arrive in r8-r10. */
static void h_install_event_loop_timer(void) {
    int i = 0;
    while (i < EV_MAX_TIMERS && E.timers[i].active)
        i++;
    if (i == EV_MAX_TIMERS)
        trap_crash("InstallEventLoopTimer: more than %d timers", EV_MAX_TIMERS);
    double delay = cpu_fpr(1), interval = cpu_fpr(2);
    if (delay < 0 || interval < 0)
        trap_crash("InstallEventLoopTimer: negative interval");
    E.timers[i].active = true;
    E.timers[i].proc = trap_arg(5);
    E.timers[i].data = trap_arg(6);
    E.timers[i].next = now_seconds() + delay;
    E.timers[i].interval = interval;
    if (trap_arg(7))
        gm_w32(trap_arg(7), timer_ref(i));
    trap_return(0);
}

static void h_remove_event_loop_timer(void) {
    uint32_t ref = trap_arg(0);
    uint32_t i = ref - timer_ref(0);
    if (i >= EV_MAX_TIMERS || !E.timers[i].active)
        trap_crash("RemoveEventLoopTimer: 0x%08x is not a timer", ref);
    E.timers[i].active = false;
    trap_return(0);
}

static void fire_due_timers(void) {
    for (int i = 0; i < EV_MAX_TIMERS; i++) {
        if (!E.timers[i].active || now_seconds() < E.timers[i].next)
            continue;
        if (E.timers[i].interval > 0) {
            E.timers[i].next += E.timers[i].interval;
            if (E.timers[i].next < now_seconds()) /* fell behind: don't try to catch up */
                E.timers[i].next = now_seconds() + E.timers[i].interval;
        } else {
            E.timers[i].active = false;
        }
        uint32_t args[2] = {timer_ref(i), E.timers[i].data};
        E.in_timer++;
        guest_call(E.timers[i].proc, 2, args);
        E.in_timer--;
    }
}

static void sleep_until_next_timer(void) {
    double next = now_seconds() + 0.010;
    for (int i = 0; i < EV_MAX_TIMERS; i++)
        if (E.timers[i].active && E.timers[i].next < next)
            next = E.timers[i].next;
    double wait = next - now_seconds();
    if (wait > 0) {
        struct timespec ts = {0, (long)(wait * 1e9)};
        nanosleep(&ts, NULL);
    }
}

void events_pump(void) {
    sound_pump();
    if (!E.in_timer) /* a timer proc waiting in its own loop isn't re-entered */
        fire_due_timers();
    if (E.present)
        E.present();
    if (E.exit_after > 0 && misc_ticks() >= (uint32_t)E.exit_after) {
        log_msg("exiting after %ld ticks (LOONY_EXIT_AFTER)", E.exit_after);
        exit(0);
    }
}

static void h_run_application_event_loop(void) {
    E.quit = false;
    E.depth++;
    while (!E.quit) {
        events_pump();
        sleep_until_next_timer();
    }
    E.depth--;
}

static void h_quit_application_event_loop(void) { E.quit = true; }

/* ReceiveNextEvent(UInt32 numTypes, const EventTypeSpec *list,
   EventTimeout timeout, Boolean pullEvent, EventRef *outEvent) -> OSStatus.
   The timeout is a double in f1 (taking up r5-r6), so pullEvent and
   outEvent arrive in r7 and r8. A negative timeout means wait forever. */
static void h_receive_next_event(void) {
    double timeout = cpu_fpr(1);
    uint32_t out = trap_arg(5);
    double deadline = now_seconds() + timeout;
    for (;;) {
        events_pump();
        if (timeout >= 0 && now_seconds() >= deadline)
            break;
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
    if (out)
        gm_w32(out, 0);
    trap_return((uint32_t)EV_LOOP_TIMED_OUT_ERR);
}

void events_register(void) {
    trap_register("NewEventLoopTimerUPP", h_new_event_loop_timer_upp);
    trap_register("GetMainEventLoop", h_get_main_event_loop);
    trap_register("InstallEventLoopTimer", h_install_event_loop_timer);
    trap_register("RemoveEventLoopTimer", h_remove_event_loop_timer);
    trap_register("RunApplicationEventLoop", h_run_application_event_loop);
    trap_register("QuitApplicationEventLoop", h_quit_application_event_loop);
    trap_register("ReceiveNextEvent", h_receive_next_event);
    trap_register("NewEventHandlerUPP", h_new_event_handler_upp);
    trap_register("GetApplicationEventTarget", h_get_application_event_target);
    trap_register("GetEventDispatcherTarget", h_get_event_dispatcher_target);
    trap_register("GetWindowEventTarget", h_get_window_event_target);
    trap_register("InstallEventHandler", h_install_event_handler);
    trap_register("InstallStandardEventHandler", h_install_standard_event_handler);
}
