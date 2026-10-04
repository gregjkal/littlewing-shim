#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Carbon Event Manager: handler installation, event objects and dispatch,
   keyboard input, event loop timers, RunApplicationEventLoop and
   ReceiveNextEvent. Event targets, events, timers and the main event loop are
   opaque IDs in tag space (EV_TAG_BASE and up).

   Dispatch follows Carbon: an event sent to the dispatcher target goes to the
   focus window's handlers, then the application's; one sent to a window goes
   to that window, then the application. On each target the most recently
   installed handler runs first, and dispatch stops at the first handler that
   returns anything other than eventNotHandledErr. */

#define EV_TAG_BASE 0x0A000000u
#define EV_APPLICATION_TARGET (EV_TAG_BASE + 1)
#define EV_DISPATCHER_TARGET  (EV_TAG_BASE + 2)
#define EV_MAIN_LOOP          (EV_TAG_BASE + 3)
#define EV_NOT_HANDLED_ERR        (-9874) /* eventNotHandledErr */
#define EV_LOOP_TIMED_OUT_ERR     (-9875) /* eventLoopTimedOutErr */
#define EV_PARAM_NOT_FOUND_ERR    (-9870) /* eventParameterNotFoundErr */
#define EV_MAX_HANDLERS 32
#define EV_MAX_TIMERS 16
#define EV_MAX_EVENTS 64

/* Event classes and kinds this module creates. */
#define EV_CLASS_KEYBOARD    0x6B657962u /* 'keyb' */
#define EV_CLASS_APPLICATION 0x6170706Cu /* 'appl' */
#define EV_CLASS_APPLE_EVENT 0x65707063u /* 'eppc' */
#define EV_RAW_KEY_DOWN    1
#define EV_RAW_KEY_REPEAT  2
#define EV_RAW_KEY_UP      3
#define EV_RAW_KEY_MODIFIERS_CHANGED 4
#define EV_APP_ACTIVATED   1
#define EV_APP_DEACTIVATED 2
#define EV_APPLE_EVENT     1

typedef struct {
    uint32_t target, handler, user_data;
    uint32_t ntypes;
    uint32_t types[8][2]; /* (class, kind) pairs */
} ev_handler;

/* Resets everything and queues kEventAppActivated, as a Mac does when an
   application starts in front. Reads LOONY_EXIT_AFTER. */
void events_init(void);

/* The event target for a window. */
uint32_t events_window_target(uint32_t window);

/* Installed handlers, in installation order. */
int events_handlers(const ev_handler **out);

/* True if InstallStandardEventHandler was called for target. */
bool events_has_standard_handler(uint32_t target);

/* Called on every pump to show the screen if anything drew to it. */
typedef void (*ev_present_fn)(void);
void events_set_present(ev_present_fn fn);

/* Called on every pump to collect host input (SDL events). */
typedef void (*ev_poll_fn)(void);
void events_set_poll(ev_poll_fn fn);

/* Called for a scripted screenshot. */
typedef bool (*ev_screenshot_fn)(const char *path);
void events_set_screenshot(ev_screenshot_fn fn);

/* Host input. A key with an SDL scancode went down or up (repeat: an
   auto-repeat). Modifier keys become kEventRawKeyModifiersChanged, others
   kEventRawKeyDown, Up or Repeat with 'kcod', 'kchr' and 'kmod'. */
void events_post_key(int scancode, bool down, bool repeat);
/* The left mouse button went down or up at (x, y) on the emulated screen.
   Only a modal sink sees mouse input; the game uses none. */
void events_post_mouse(int x, int y, bool down);

/* Text to insert (Cmd-V, or a script's "type"), UTF-8. Only a modal sink
   sees it. */
void events_post_text(const char *utf8);

/* While a modal dialog runs, it takes the input instead of the game: key
   downs and repeats (with their Mac key code, character and modifiers),
   mouse buttons and text go to the sink. The game is still sent what keeps
   its view of the keyboard right once the dialog closes: modifier changes,
   and the key-up of any key whose key-down it was sent. NULL ends modal
   input. */
typedef struct {
    void (*key)(uint32_t vkey, uint8_t chr, uint32_t modifiers);
    void (*mouse)(int x, int y, bool down);
    void (*text)(const char *utf8);
} ev_modal_sink;
void events_set_modal(const ev_modal_sink *sink);

/* Called on every pump with whether the mouse pointer should show: when the
   game hasn't hidden it (HideCursor), or while a modal dialog runs. */
typedef void (*ev_cursor_fn)(bool visible);
void events_set_cursor(ev_cursor_fn fn);

/* The window gained or lost focus: kEventAppActivated / Deactivated. */
void events_post_activation(bool active);
/* The user asked to quit (window close, Cmd-Q, a script). Queues the quit
   Apple Event for the game's handler; if the game hasn't quit 3 seconds
   (wall clock) later, exits with status 0. */
void events_request_quit(void);

/* True once the host asked the game to quit (Cmd-Q, closing the window, a
   script's quit): the quit Apple Event has been sent. */
bool events_quit_requested(void);

/* Events waiting in the queue. */
int events_queued(void);

/* One pump step: host input, scripted actions, sound, due timers (only
   inside RunApplicationEventLoop or ReceiveNextEvent, never re-entering a
   running timer), presenting the screen, and the LOONY_EXIT_AFTER check. The
   run loop calls it, and so does misc's idle hook while the game waits in
   its own loop. */
void events_pump(void);

/* Number of timers installed and not removed. */
int events_active_timers(void);

/* Registers NewEventHandlerUPP, GetApplicationEventTarget,
   GetEventDispatcherTarget, GetWindowEventTarget, InstallEventHandler,
   InstallStandardEventHandler, NewEventLoopTimerUPP, GetMainEventLoop,
   InstallEventLoopTimer, RemoveEventLoopTimer, RunApplicationEventLoop,
   QuitApplicationEventLoop, ReceiveNextEvent, SendEventToEventTarget,
   GetEventKind, GetEventParameter and ReleaseEvent.

   If LOONY_EXIT_AFTER is set to a tick count (1/60 s), the process exits with
   status 0 once that many ticks have passed. */
void events_register(void);
