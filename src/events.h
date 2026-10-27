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
#define EV_PARAM_ERR              (-50)   /* paramErr */
#define EV_NOT_HANDLED_ERR        (-9874) /* eventNotHandledErr */
#define EV_LOOP_TIMED_OUT_ERR     (-9875) /* eventLoopTimedOutErr */
#define EV_PARAM_NOT_FOUND_ERR    (-9870) /* eventParameterNotFoundErr */
#define EV_MAX_HANDLERS 32
#define EV_MAX_TIMERS 16
#define EV_MAX_EVENTS 64
/* A classic game's keyboard and mouse events waiting at most: the oldest
   goes to make room for a new one, as on a Mac (Inside Macintosh: Macintosh
   Toolbox Essentials, the Operating System event queue). */
#define EV_CLASSIC_QUEUE 20

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
#define EV_CLASS_COMMAND     0x636D6473u /* 'cmds' */
#define EV_COMMAND_PROCESS 1
#define EV_CLASS_MOUSE       0x6D6F7573u /* 'mous' */
#define EV_MOUSE_DOWN 1
#define EV_MOUSE_UP   2

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

/* What clicking a control with a command does: sends kEventCommandProcess,
   whose direct object is an HICommand holding command, to the window's
   handlers, then the application's. Returns the first answer that isn't
   eventNotHandledErr, or eventNotHandledErr. */
int32_t events_send_command(uint32_t window, uint32_t command);

/* A window was disposed of: its handlers go, keyboard focus moves to
   the newest window that still has one (or none), and it is no longer the
   active window, so no deactivate is sent for it. */
void events_forget_window(uint32_t window);

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
   A modal sink takes it. Otherwise only a game that has called the classic
   Event Manager sees it: as kEventMouseDown or Up, and in Button, StillDown
   and GetGlobalMouse. The Carbon games use none. */
void events_post_mouse(int x, int y, bool down);

/* A Command-key down typing chr (in lower case), posted for the game
   alone: no key is held, and none comes up. How a quit or a choice in the
   host's menu bar reaches a classic game's menus. False if a modal dialog
   is up or the queue is full. */
bool events_post_command_key(uint8_t chr);

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

/* The window gained or lost focus: kEventAppActivated / Deactivated, posted
   only when that changes whether the application is active. */
void events_post_activation(bool active);
/* The user asked to quit (window close, Cmd-Q, a script). Queues the quit
   Apple Event for the game's handler; if the game hasn't quit 3 seconds
   (wall clock) later, exits with status 0. */
void events_request_quit(void);

/* True once the host asked the game to quit (Cmd-Q, closing the window, a
   script's quit): the quit Apple Event has been sent. */
bool events_quit_requested(void);

/* Input from the host (the window, a script): logged on a recording,
   dropped on a replay, whose input comes from its file (a quit stops the
   replay), then passed to events_post_key and the others. On a replay,
   keys go to the events_set_replay_key function instead, if there is one. */
void events_set_replay_key(void (*fn)(int scancode, bool down, bool repeat));
void events_host_key(int scancode, bool down, bool repeat);
void events_host_mouse(int x, int y, bool down);
void events_host_text(const char *utf8);
void events_host_activation(bool active);
void events_host_quit(void);

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

/* For snapshots (snapshot.h): the host frames above a guest call the run
   loop (RunApplicationEventLoop) made, a handler it dispatched an event to
   or a timer its own pump fired, which a snapshot taken inside the call
   needs in order to check it is in the same place, or to rebuild them. */
typedef struct {
    int timer;         /* the timer whose callback runs, or -1: a dispatch */
    int slot;          /* a dispatch: the event's slot */
    uint32_t chain[2]; /* the targets dispatch tries, in order */
    int nchain, link;  /* how many, and which one it is on */
    int handler;       /* the handler's index among those installed */
    int loop_depth;    /* the loop depth call_out hides while it runs */
} ev_frame;

/* True, filling *f, while the run loop has a handler or a timer's callback
   running: the guest call one deeper than the run loop. */
bool events_loop_frame(ev_frame *f);

/* The next RunApplicationEventLoop call goes back into what f describes
   instead of starting: the dispatch, or the pump firing timer f->timer. It
   runs call, which stands for the guest call to handler f->handler or to
   the timer's callback and returns what that returned, then finishes the
   dispatch or the pump and runs on as if it had never left. */
void events_resume_loop(const ev_frame *f, uint32_t (*call)(void));

/* After a longjmp out of the run loop: forgets the frame in progress and a
   resume not taken up yet. */
void events_unwind(void);

/* The script's next action (script.h), whatever its tick, for when the
   clock has stopped (the end of a replay, headless): false once there is
   none. */
bool events_script_rest(void);

/* Registers NewEventHandlerUPP, GetApplicationEventTarget,
   GetEventDispatcherTarget, GetWindowEventTarget, InstallEventHandler,
   InstallStandardEventHandler, NewEventLoopTimerUPP, GetMainEventLoop,
   InstallEventLoopTimer, RemoveEventLoopTimer, RunApplicationEventLoop,
   QuitApplicationEventLoop, ReceiveNextEvent, SendEventToEventTarget,
   GetEventKind, GetEventClass, GetEventParameter and ReleaseEvent; and the
   classic Event Manager a PowerPlant game uses: WaitNextEvent,
   GetNextEvent, EventAvail, Button, StillDown, GetKeys,
   GetCurrentKeyModifiers, GetGlobalMouse, GetCaretTime, GetDblTime,
   GetCurrentEventLoop and AEProcessAppleEvent.

   If LOONY_EXIT_AFTER is set to a tick count (1/60 s), the process exits with
   status 0 once that many ticks have passed. */
void events_register(void);
