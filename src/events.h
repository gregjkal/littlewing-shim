#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Carbon Event Manager: handler installation, event loop timers and
   RunApplicationEventLoop. Input events (keyboard, mouse) and dispatching
   them to handlers arrive in the next milestone. Event targets, timers and the
   main event loop are opaque IDs in tag space (EV_TAG_BASE and up). */

#define EV_TAG_BASE 0x0A000000u
#define EV_APPLICATION_TARGET (EV_TAG_BASE + 1)
#define EV_DISPATCHER_TARGET  (EV_TAG_BASE + 2)
#define EV_MAIN_LOOP          (EV_TAG_BASE + 3)
#define EV_LOOP_TIMED_OUT_ERR (-9875) /* eventLoopTimedOutErr */
#define EV_MAX_HANDLERS 32
#define EV_MAX_TIMERS 16

typedef struct {
    uint32_t target, handler, user_data;
    uint32_t ntypes;
    uint32_t types[8][2]; /* (class, kind) pairs */
} ev_handler;

void events_init(void);

/* The event target for a window. */
uint32_t events_window_target(uint32_t window);

/* Installed handlers, in installation order. */
int events_handlers(const ev_handler **out);

/* True if InstallStandardEventHandler was called for target. */
bool events_has_standard_handler(uint32_t target);

/* Called on every loop iteration to show the screen if anything drew to it. */
typedef void (*ev_present_fn)(void);
void events_set_present(ev_present_fn fn);

/* One pump step: sound, due timers (unless already inside one), presenting
   the screen, and the LOONY_EXIT_AFTER check. The run loop calls it, and so
   does misc's idle hook while the game waits in its own loop. */
void events_pump(void);

/* Number of timers installed and not removed. */
int events_active_timers(void);

/* Registers NewEventHandlerUPP, GetApplicationEventTarget,
   GetEventDispatcherTarget, GetWindowEventTarget, InstallEventHandler,
   InstallStandardEventHandler, NewEventLoopTimerUPP, GetMainEventLoop,
   InstallEventLoopTimer, RemoveEventLoopTimer, RunApplicationEventLoop,
   QuitApplicationEventLoop and ReceiveNextEvent (which, with no input yet,
   pumps until its timeout and returns EV_LOOP_TIMED_OUT_ERR).

   RunApplicationEventLoop fires due timers (guest_call(proc, timer, data)),
   presents the screen, and sleeps until the next timer. If LOONY_EXIT_AFTER is
   set to a tick count (1/60 s), the process exits with status 0 once that
   many ticks have passed. */
void events_register(void);
