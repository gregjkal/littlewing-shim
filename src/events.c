#include "events.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu.h"
#include "guest_mem.h"
#include "keymap.h"
#include "memmgr.h"
#include "misc.h"
#include "script.h"
#include "sound.h"
#include "trap.h"
#include "util.h"

#define MAX_STANDARD 8
#define MAX_HELD 16
#define QUIT_GRACE_SECONDS 3.0

/* Parameter names and types (Carbon Events.h, AEDataModel.h). */
#define PARAM_KEY_CODE      0x6B636F64u /* 'kcod' */
#define PARAM_KEY_CHAR      0x6B636872u /* 'kchr' */
#define PARAM_KEY_MODIFIERS 0x6B6D6F64u /* 'kmod' */
#define PARAM_DIRECT_OBJECT 0x2D2D2D2Du /* '----' */
#define TYPE_UINT32   0x6D61676Eu /* 'magn' */
#define TYPE_CHAR     0x54455854u /* 'TEXT' */
#define TYPE_WILDCARD 0x2A2A2A2Au /* '****' */
#define AE_CLASS_CORE 0x61657674u /* 'aevt' */
#define AE_ID_QUIT    0x71756974u /* 'quit' */

/* Tag-space sub-ranges. */
#define TAG_HANDLER_REF (EV_TAG_BASE + 0x1000u)
#define TAG_TIMER       (EV_TAG_BASE + 0x2000u)
#define TAG_NEXT_HANDLER (EV_TAG_BASE + 0x3000u)
#define TAG_EVENT       (EV_TAG_BASE + 0x4000u)
#define TAG_WINDOW      (EV_TAG_BASE + 0x10000u)

typedef struct {
    int refs; /* 0 = free slot */
    bool queued;
    bool guest_owned; /* pulled by ReceiveNextEvent; the guest must release it */
    uint32_t cls, kind;
    uint32_t key_code, modifiers;
    uint8_t chr;
} ev_event;

static struct {
    ev_handler handlers[EV_MAX_HANDLERS];
    int nhandlers;
    uint32_t standard[MAX_STANDARD];
    int nstandard;
    uint32_t focus_window; /* the window target keyboard events go to, or 0 */
    bool app_active;       /* the last of kEventAppActivated / Deactivated posted */
    struct {
        bool active, running;
        uint32_t proc, data;
        double next, interval; /* seconds on the misc clock */
    } timers[EV_MAX_TIMERS];
    ev_event events[EV_MAX_EVENTS];
    int queue[EV_MAX_EVENTS];
    int qhead, qcount;
    struct {
        int scancode;
        uint32_t bits;
    } held[MAX_HELD]; /* modifier keys that are down */
    int nheld;
    int game_down[MAX_HELD]; /* other keys whose key-down the game was sent */
    int ngame_down;
    bool quit;
    int loop_depth; /* inside RunApplicationEventLoop or a ReceiveNextEvent wait */
    ev_present_fn present;
    ev_poll_fn poll;
    ev_screenshot_fn screenshot;
    ev_cursor_fn cursor;
    const ev_modal_sink *modal;
    long exit_after; /* ticks, or 0 */
    double quit_deadline; /* wall-clock seconds, or 0 */
    uint32_t ae_descs; /* guest memory for the quit AppleEvent and its reply */
    bool warned_full;
} E;

static double wall_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static double now_seconds(void) { return misc_seconds(); }

/* ---- event objects and the queue ---- */

static uint32_t event_ref(int i) { return TAG_EVENT + (uint32_t)i; }

static ev_event *lookup_event(uint32_t ref) {
    uint32_t i = ref - TAG_EVENT;
    if (i >= EV_MAX_EVENTS || E.events[i].refs == 0)
        return NULL;
    return &E.events[i];
}

static ev_event *need_event(const char *call, uint32_t ref) {
    ev_event *e = lookup_event(ref);
    if (!e)
        trap_crash("%s: 0x%08x is not an event", call, ref);
    return e;
}

/* Queues a new event (the queue holds its reference). Returns it, or NULL
   if the queue is full (logged once). */
static ev_event *post(uint32_t cls, uint32_t kind) {
    int i = 0;
    while (i < EV_MAX_EVENTS && E.events[i].refs)
        i++;
    if (i == EV_MAX_EVENTS || E.qcount == EV_MAX_EVENTS) {
        if (!E.warned_full)
            log_msg("events: the queue is full; dropping input");
        E.warned_full = true;
        return NULL;
    }
    E.events[i] = (ev_event){1, true, false, cls, kind, 0, 0, 0};
    E.queue[(E.qhead + E.qcount) % EV_MAX_EVENTS] = i;
    E.qcount++;
    return &E.events[i];
}

/* Removes the queued event at position k (0 = oldest) and returns its index. */
static int dequeue_at(int k) {
    int idx = E.queue[(E.qhead + k) % EV_MAX_EVENTS];
    for (int j = k; j > 0; j--)
        E.queue[(E.qhead + j) % EV_MAX_EVENTS] = E.queue[(E.qhead + j - 1) % EV_MAX_EVENTS];
    E.qhead = (E.qhead + 1) % EV_MAX_EVENTS;
    E.qcount--;
    E.events[idx].queued = false;
    return idx;
}

static void release(ev_event *e) {
    if (e->refs <= 0)
        trap_crash("event reference count underflow (internal error)");
    if (--e->refs == 0)
        memset(e, 0, sizeof *e);
}

int events_queued(void) { return E.qcount; }

/* ---- host input ---- */

static uint32_t current_modifiers(void) {
    uint32_t m = 0;
    for (int i = 0; i < E.nheld; i++)
        m |= E.held[i].bits;
    return m;
}

void events_post_key(int scancode, bool down, bool repeat) {
    keymap_entry k = keymap_lookup(scancode);
    if (k.vkey < 0)
        return;
    if (k.modifier) {
        uint32_t before = current_modifiers();
        int at = -1;
        for (int i = 0; i < E.nheld; i++)
            if (E.held[i].scancode == scancode)
                at = i;
        if (down && at < 0 && E.nheld < MAX_HELD)
            E.held[E.nheld++] = (typeof(E.held[0])){scancode, k.modifier};
        else if (!down && at >= 0)
            E.held[at] = E.held[--E.nheld];
        uint32_t after = current_modifiers();
        if (after != before) { /* even under a dialog: the game sees them after it */
            ev_event *e = post(EV_CLASS_KEYBOARD, EV_RAW_KEY_MODIFIERS_CHANGED);
            if (e)
                e->modifiers = after;
        }
        return;
    }
    int at = -1;
    for (int i = 0; i < E.ngame_down; i++)
        if (E.game_down[i] == scancode)
            at = i;
    if (E.modal && (down || at < 0)) { /* a key that went down before the dialog still comes up */
        if (down)
            E.modal->key((uint32_t)k.vkey, keymap_char(&k, current_modifiers()), current_modifiers());
        return;
    }
    if (down && at < 0 && E.ngame_down < MAX_HELD)
        E.game_down[E.ngame_down++] = scancode;
    else if (!down && at >= 0)
        E.game_down[at] = E.game_down[--E.ngame_down];
    uint32_t kind = !down ? EV_RAW_KEY_UP : repeat ? EV_RAW_KEY_REPEAT : EV_RAW_KEY_DOWN;
    ev_event *e = post(EV_CLASS_KEYBOARD, kind);
    if (e) {
        e->key_code = (uint32_t)k.vkey;
        e->modifiers = current_modifiers();
        e->chr = keymap_char(&k, e->modifiers);
    }
}

void events_post_mouse(int x, int y, bool down) {
    if (E.modal)
        E.modal->mouse(x, y, down);
}

void events_post_text(const char *utf8) {
    if (E.modal)
        E.modal->text(utf8);
}

void events_set_modal(const ev_modal_sink *sink) { E.modal = sink; }

void events_set_cursor(ev_cursor_fn fn) { E.cursor = fn; }

/* A Mac sends these only when the application changes state, so they
   alternate; SDL can report a focus loss twice, and the game removes its
   timer on each deactivation. */
void events_post_activation(bool active) {
    if (active == E.app_active)
        return;
    E.app_active = active;
    post(EV_CLASS_APPLICATION, active ? EV_APP_ACTIVATED : EV_APP_DEACTIVATED);
}

void events_request_quit(void) {
    if (E.quit_deadline == 0) {
        post(EV_CLASS_APPLE_EVENT, EV_APPLE_EVENT);
        E.quit_deadline = wall_seconds() + QUIT_GRACE_SECONDS;
    }
}

bool events_quit_requested(void) { return E.quit_deadline > 0; }

/* ---- setup ---- */

void events_init(void) {
    memset(&E, 0, sizeof E);
    const char *s = getenv("LOONY_EXIT_AFTER");
    if (s && *s)
        E.exit_after = strtol(s, NULL, 10);
    events_post_activation(true);
}

void events_set_present(ev_present_fn fn) { E.present = fn; }
void events_set_poll(ev_poll_fn fn) { E.poll = fn; }
void events_set_screenshot(ev_screenshot_fn fn) { E.screenshot = fn; }

int events_active_timers(void) {
    int n = 0;
    for (int i = 0; i < EV_MAX_TIMERS; i++)
        n += E.timers[i].active;
    return n;
}

/* Window targets are TAG_WINDOW + the window's address / 16, which is
   unique and reversible. */
uint32_t events_window_target(uint32_t window) { return TAG_WINDOW + window / 16u; }

static bool is_window_target(uint32_t t) { return t >= TAG_WINDOW && t < TAG_WINDOW + 0x01000000u; }

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

/* ---- dispatch ---- */

/* Runs guest code that isn't waiting for events: timers may fire only from a
   wait in RunApplicationEventLoop or ReceiveNextEvent, not from a Delay
   inside a callback, so the loop depth is hidden while it runs. */
static uint32_t call_out(uint32_t tvector, int nargs, const uint32_t *args) {
    int depth = E.loop_depth;
    E.loop_depth = 0;
    uint32_t r = guest_call(tvector, nargs, args);
    E.loop_depth = depth;
    return r;
}

/* The quit Apple Event goes to the handler AEInstallEventHandler recorded;
   with none, the application just exits. */
static void handle_apple_event(void) {
    uint32_t handler, refcon;
    if (!misc_ae_handler(AE_CLASS_CORE, AE_ID_QUIT, &handler, &refcon)) {
        log_msg("quit requested; the game has no quit handler");
        exit(0);
    }
    if (!E.ae_descs) {
        E.ae_descs = mm_new_ptr(16, true); /* two AEDescs: {descriptorType, dataHandle} */
        if (!E.ae_descs)
            trap_crash("out of guest memory for an Apple Event");
        gm_w32(E.ae_descs, AE_CLASS_CORE);
        gm_w32(E.ae_descs + 8, 0x6E756C6Cu); /* 'null' */
    }
    log_msg("sending the quit Apple Event");
    uint32_t args[3] = {E.ae_descs, E.ae_descs + 8, refcon};
    call_out(handler, 3, args);
}

static bool handles(const ev_handler *h, const ev_event *e) {
    for (uint32_t k = 0; k < h->ntypes; k++)
        if (h->types[k][0] == e->cls && h->types[k][1] == e->kind)
            return true;
    return false;
}

/* Runs the handlers on target for event idx, newest first. */
static int32_t run_handlers(uint32_t target, int idx) {
    for (int i = E.nhandlers - 1; i >= 0; i--) {
        const ev_handler *h = &E.handlers[i];
        if (h->target != target || !handles(h, &E.events[idx]))
            continue;
        uint32_t args[3] = {TAG_NEXT_HANDLER, event_ref(idx), h->user_data};
        int32_t r = (int32_t)call_out(h->handler, 3, args);
        if (r != EV_NOT_HANDLED_ERR)
            return r;
    }
    return EV_NOT_HANDLED_ERR;
}

static int32_t dispatch(int idx, uint32_t target) {
    uint32_t chain[2];
    int n = 0;
    if (target == EV_DISPATCHER_TARGET) {
        if (E.focus_window && E.events[idx].cls == EV_CLASS_KEYBOARD)
            chain[n++] = E.focus_window;
        chain[n++] = EV_APPLICATION_TARGET;
    } else if (is_window_target(target)) {
        chain[n++] = target;
        chain[n++] = EV_APPLICATION_TARGET;
    } else if (target == EV_APPLICATION_TARGET) {
        chain[n++] = EV_APPLICATION_TARGET;
    } else {
        trap_crash("SendEventToEventTarget: 0x%08x is not an event target", target);
    }
    E.events[idx].refs++; /* handlers may release their own references */
    int32_t r = EV_NOT_HANDLED_ERR;
    for (int i = 0; i < n && r == EV_NOT_HANDLED_ERR; i++)
        r = run_handlers(chain[i], idx);
    if (r == EV_NOT_HANDLED_ERR && E.events[idx].cls == EV_CLASS_APPLE_EVENT) {
        handle_apple_event(); /* the standard application handler's job */
        r = 0;
    }
    release(&E.events[idx]);
    return r;
}

/* ---- timers ---- */

static uint32_t timer_ref(int i) { return TAG_TIMER + (uint32_t)i; }

static void fire_due_timers(void) {
    for (int i = 0; i < EV_MAX_TIMERS; i++) {
        if (!E.timers[i].active || E.timers[i].running || now_seconds() < E.timers[i].next)
            continue;
        if (E.timers[i].interval > 0) {
            E.timers[i].next += E.timers[i].interval;
            if (E.timers[i].next < now_seconds()) /* fell behind: don't try to catch up */
                E.timers[i].next = now_seconds() + E.timers[i].interval;
        } else {
            E.timers[i].active = false;
        }
        uint32_t args[2] = {timer_ref(i), E.timers[i].data};
        E.timers[i].running = true;
        call_out(E.timers[i].proc, 2, args);
        E.timers[i].running = false;
    }
}

static void wait_until_next_timer(void) {
    double next = now_seconds() + 0.010;
    for (int i = 0; i < EV_MAX_TIMERS; i++)
        if (E.timers[i].active && !E.timers[i].running && E.timers[i].next < next)
            next = E.timers[i].next;
    misc_wait(next - now_seconds());
}

/* ---- the pump ---- */

static void run_script(void) {
    script_action a;
    while (script_next(misc_ticks(), &a)) {
        switch (a.kind) {
        case SCRIPT_KEY_DOWN: events_post_key(a.scancode, true, false); break;
        case SCRIPT_KEY_UP: events_post_key(a.scancode, false, false); break;
        case SCRIPT_SCREENSHOT:
            if (!E.screenshot || !E.screenshot(a.path))
                log_msg("script: can't write the screenshot %s", a.path);
            break;
        case SCRIPT_QUIT: events_request_quit(); break;
        case SCRIPT_CLICK:
            events_post_mouse(a.x, a.y, true);
            events_post_mouse(a.x, a.y, false);
            break;
        case SCRIPT_TYPE: events_post_text(a.path); break;
        case SCRIPT_BLUR: events_post_activation(false); break;
        case SCRIPT_FOCUS: events_post_activation(true); break;
        }
    }
}

void events_pump(void) {
    if (E.poll)
        E.poll();
    run_script();
    sound_pump();
    if (E.loop_depth > 0)
        fire_due_timers();
    if (E.cursor)
        E.cursor(misc_cursor_visible() || E.modal);
    if (E.present)
        E.present();
    if (E.exit_after > 0 && misc_ticks() >= (uint32_t)E.exit_after) {
        log_msg("exiting after %ld ticks (LOONY_EXIT_AFTER)", E.exit_after);
        exit(0);
    }
    if (E.quit_deadline > 0 && wall_seconds() >= E.quit_deadline) {
        log_msg("the game didn't quit within %.0f seconds; exiting", QUIT_GRACE_SECONDS);
        exit(0);
    }
}

/* ---- guest calls: handlers ---- */

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
    if (is_window_target(h->target))
        E.focus_window = h->target;
    if (out)
        gm_w32(out, TAG_HANDLER_REF + (uint32_t)E.nhandlers);
    trap_return(0);
}

static void h_install_standard_event_handler(void) {
    uint32_t t = trap_arg(0);
    if (E.nstandard < MAX_STANDARD)
        E.standard[E.nstandard++] = t;
    if (is_window_target(t))
        E.focus_window = t;
    trap_return(0);
}

/* ---- guest calls: events ---- */

/* SendEventToEventTarget(EventRef, EventTargetRef) -> OSStatus */
static void h_send_event_to_event_target(void) {
    ev_event *e = need_event("SendEventToEventTarget", trap_arg(0));
    trap_return((uint32_t)dispatch((int)(e - E.events), trap_arg(1)));
}

static void h_get_event_kind(void) { trap_return(need_event("GetEventKind", trap_arg(0))->kind); }

static void h_release_event(void) {
    ev_event *e = need_event("ReleaseEvent", trap_arg(0));
    if (e->queued)
        trap_crash("ReleaseEvent: 0x%08x is still in the event queue", trap_arg(0));
    if (!e->guest_owned)
        trap_crash("ReleaseEvent: 0x%08x isn't owned by the caller", trap_arg(0));
    e->guest_owned = false;
    release(e);
}

/* GetEventParameter(EventRef, EventParamName, EventParamType desiredType,
   EventParamType *outActualType, UInt32 bufferSize, UInt32 *outActualSize,
   void *outData) -> OSStatus */
static void h_get_event_parameter(void) {
    ev_event *e = need_event("GetEventParameter", trap_arg(0));
    uint32_t name = trap_arg(1), desired = trap_arg(2), out_type = trap_arg(3);
    uint32_t buf_size = trap_arg(4), out_size = trap_arg(5), out = trap_arg(6);
    uint8_t data[4];
    uint32_t type, size;
    if (e->cls == EV_CLASS_KEYBOARD && name == PARAM_KEY_CODE && e->kind != EV_RAW_KEY_MODIFIERS_CHANGED) {
        type = TYPE_UINT32;
        size = 4;
        wr_be32(data, e->key_code);
    } else if (e->cls == EV_CLASS_KEYBOARD && name == PARAM_KEY_CHAR &&
               e->kind != EV_RAW_KEY_MODIFIERS_CHANGED) {
        type = TYPE_CHAR;
        size = 1;
        data[0] = e->chr;
    } else if (e->cls == EV_CLASS_KEYBOARD && name == PARAM_KEY_MODIFIERS) {
        type = TYPE_UINT32;
        size = 4;
        wr_be32(data, e->modifiers);
    } else {
        trap_return((uint32_t)EV_PARAM_NOT_FOUND_ERR);
        return;
    }
    if (desired != type && desired != TYPE_WILDCARD)
        trap_crash("GetEventParameter: can't return parameter 0x%08x as type 0x%08x", name, desired);
    if (out)
        memcpy(gm_ptr(out, buf_size < size ? buf_size : size), data, buf_size < size ? buf_size : size);
    if (out_type)
        gm_w32(out_type, type);
    if (out_size)
        gm_w32(out_size, size);
    trap_return(0);
}

/* ReceiveNextEvent(UInt32 numTypes, const EventTypeSpec *list,
   EventTimeout timeout, Boolean pullEvent, EventRef *outEvent) -> OSStatus.
   The timeout is a double in f1 (taking up r5-r6), so pullEvent and
   outEvent arrive in r7 and r8. A negative timeout means wait forever. A
   pulled event belongs to the caller, who releases it. */
static void h_receive_next_event(void) {
    uint32_t ntypes = trap_arg(0), list = trap_arg(1);
    double timeout = cpu_fpr(1);
    bool pull = (trap_arg(4) & 0xFF) != 0;
    uint32_t out = trap_arg(5);
    double deadline = now_seconds() + timeout;
    E.loop_depth++;
    for (;;) {
        events_pump();
        for (int k = 0; k < E.qcount; k++) {
            int idx = E.queue[(E.qhead + k) % EV_MAX_EVENTS];
            bool match = ntypes == 0;
            for (uint32_t j = 0; j < ntypes && !match; j++)
                match = gm_r32(list + 8 * j) == E.events[idx].cls &&
                        gm_r32(list + 8 * j + 4) == E.events[idx].kind;
            if (!match)
                continue;
            if (pull) {
                dequeue_at(k);
                E.events[idx].guest_owned = true;
            }
            if (out)
                gm_w32(out, event_ref(idx));
            E.loop_depth--;
            trap_return(0);
            return;
        }
        if (timeout >= 0 && now_seconds() >= deadline) {
            misc_poll();
            break;
        }
        misc_wait(0.001);
    }
    E.loop_depth--;
    if (out)
        gm_w32(out, 0);
    trap_return((uint32_t)EV_LOOP_TIMED_OUT_ERR);
}

/* ---- guest calls: timers and the run loop ---- */

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
    E.timers[i].running = false;
    E.timers[i].proc = trap_arg(5);
    E.timers[i].data = trap_arg(6);
    E.timers[i].next = now_seconds() + delay;
    E.timers[i].interval = interval;
    if (trap_arg(7))
        gm_w32(trap_arg(7), timer_ref(i));
    trap_return(0);
}

/* RemoveEventLoopTimer(EventLoopTimerRef) -> OSStatus. Carbon answers NULL
   with paramErr. The game's kEventAppDeactivated handler removes its timer
   and then sets it to NULL, so a second deactivation without an activation
   between them passes NULL. */
static void h_remove_event_loop_timer(void) {
    uint32_t ref = trap_arg(0);
    if (ref == 0) {
        trap_return((uint32_t)EV_PARAM_ERR);
        return;
    }
    uint32_t i = ref - timer_ref(0);
    if (i >= EV_MAX_TIMERS || !E.timers[i].active)
        trap_crash("RemoveEventLoopTimer: 0x%08x is not a timer", ref);
    E.timers[i].active = false;
    trap_return(0);
}

/* Fires timers and dispatches queued events to the dispatcher target until
   QuitApplicationEventLoop. */
static void h_run_application_event_loop(void) {
    E.quit = false;
    E.loop_depth++;
    while (!E.quit) {
        events_pump();
        while (E.qcount > 0 && !E.quit) {
            int idx = dequeue_at(0);
            dispatch(idx, EV_DISPATCHER_TARGET);
            release(&E.events[idx]);
        }
        if (!E.quit)
            wait_until_next_timer();
    }
    E.loop_depth--;
}

static void h_quit_application_event_loop(void) { E.quit = true; }

void events_register(void) {
    trap_register("NewEventHandlerUPP", h_new_event_handler_upp);
    trap_register("GetApplicationEventTarget", h_get_application_event_target);
    trap_register("GetEventDispatcherTarget", h_get_event_dispatcher_target);
    trap_register("GetWindowEventTarget", h_get_window_event_target);
    trap_register("InstallEventHandler", h_install_event_handler);
    trap_register("InstallStandardEventHandler", h_install_standard_event_handler);
    trap_register("SendEventToEventTarget", h_send_event_to_event_target);
    trap_register("GetEventKind", h_get_event_kind);
    trap_register("GetEventParameter", h_get_event_parameter);
    trap_register("ReleaseEvent", h_release_event);
    trap_register("ReceiveNextEvent", h_receive_next_event);
    trap_register("NewEventLoopTimerUPP", h_new_event_loop_timer_upp);
    trap_register("GetMainEventLoop", h_get_main_event_loop);
    trap_register("InstallEventLoopTimer", h_install_event_loop_timer);
    trap_register("RemoveEventLoopTimer", h_remove_event_loop_timer);
    trap_register("RunApplicationEventLoop", h_run_application_event_loop);
    trap_register("QuitApplicationEventLoop", h_quit_application_event_loop);
}
