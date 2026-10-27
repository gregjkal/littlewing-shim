#include "test.h"

#include <stdlib.h>

#include "asm.h"
#include "events.h"
#include "harness.h"
#include <SDL3/SDL_scancode.h>

#include "keymap.h"
#include "memmgr.h"
#include "misc.h"
#include "qd.h"
#include "rsrc.h"
#include "util.h"

/* The classic Event Manager a PowerPlant game (Golden Logres, Angel Egg)
   uses, on the Carbon queue. */

static const char *const names[] = {
    "WaitNextEvent", "GetNextEvent", "EventAvail", "Button", "StillDown", "GetKeys",
    "GetCurrentKeyModifiers", "GetGlobalMouse", "GetCaretTime", "GetDblTime",
    "GetCurrentEventLoop", "AEProcessAppleEvent", "NewEventLoopTimerUPP", "GetMainEventLoop",
    "InstallEventLoopTimer", "GetNewCWindow", "BeginUpdate", "EndUpdate", "ShowWindow",
    "HideWindow", "SelectWindow", "DisposeWindow", "BeginFullScreen", "EndFullScreen", "FlushEvents",
};

#define EVERY 0xFFFFu
#define KEY_DOWN_MASK (1u << 3)
#define PROC    (GUEST_IMAGE_BASE + 0x200)
#define TV_PROC (GUEST_IMAGE_BASE + 0x8100)
#define COUNTER (GUEST_IMAGE_BASE + 0x8200)

static void setup_with(const char *const *imports, int n) {
    unsetenv("LOONY_EXIT_AFTER");
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    harness_init(imports, n);
    mm_init();
    misc_init();
    misc_register();
    qd_init(800, 600, 8);
    qd_register();
    events_init();
    events_register();
    unsetenv("LOONY_FIXED_CLOCK");
}

static void setup(void) { setup_with(names, sizeof names / sizeof names[0]); }

typedef struct {
    uint16_t what, modifiers;
    uint32_t message, when;
    int16_t v, h;
} record;

static record read_record(uint32_t r) {
    return (record){gm_r16(r), gm_r16(r + 14), gm_r32(r + 2), gm_r32(r + 6), (int16_t)gm_r16(r + 10),
                    (int16_t)gm_r16(r + 12)};
}

/* WaitNextEvent with sleep 0: what it returned and the record. */
static bool wne(uint32_t mask, record *out) {
    uint32_t r = scratch(16);
    bool got = call_import("WaitNextEvent", 4, mask, r, 0u, 0u) != 0;
    *out = read_record(r);
    return got;
}

/* What a Mac gives an application first: a resume, then the open
   application Apple Event. */
static void take_launch_events(void) {
    record e;
    wne(EVERY, &e);
    wne(EVERY, &e);
}

TEST(classic_events_start_with_a_resume_then_give_key_downs) {
    setup();
    record e;
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.what, 15);             /* osEvt */
    CHECK_EQ(e.message, 0x01000001u); /* suspendResumeMessage, resumeFlag */
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.what, 23); /* kHighLevelEvent */
    CHECK_EQ(e.message, 0x61657674u); /* 'aevt' */
    CHECK_EQ((uint32_t)(uint16_t)e.v << 16 | (uint16_t)e.h, 0x6F617070u); /* 'oapp' */
    CHECK(!wne(EVERY, &e));
    CHECK_EQ(e.what, 0);

    events_post_key(SDL_SCANCODE_LSHIFT, true, false);
    events_post_key(SDL_SCANCODE_Z, true, false);
    events_post_key(SDL_SCANCODE_Z, true, true);
    events_post_key(SDL_SCANCODE_Z, false, false);
    CHECK(wne(EVERY, &e)); /* the modifier change has no record and is passed over */
    CHECK_EQ(e.what, 3);
    CHECK_EQ(e.message, 0x06u << 8 | 'Z');
    CHECK_EQ(e.modifiers, 0x0200 | 0x0080); /* shiftKey, and the button is up */
    CHECK(e.when >= MISC_BOOT_TICKS);
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.what, 5); /* autoKey */
    CHECK(!wne(EVERY, &e)); /* no keyUp */
    CHECK_EQ(events_queued(), 0);
}

TEST(classic_events_outside_the_mask_wait_and_event_avail_peeks) {
    setup();
    record e;
    take_launch_events();
    events_post_key(SDL_SCANCODE_A, true, false);
    CHECK(!wne((uint32_t)~KEY_DOWN_MASK & EVERY, &e));
    CHECK_EQ(events_queued(), 1);
    uint32_t r = scratch(16);
    CHECK_EQ(call_import("EventAvail", 2, EVERY, r), 1);
    CHECK_EQ(read_record(r).what, 3);
    CHECK_EQ(events_queued(), 1);
    CHECK_EQ(call_import("GetNextEvent", 2, KEY_DOWN_MASK, r), 1);
    CHECK_EQ(read_record(r).message & 0xFF, 'a');
    CHECK_EQ(events_queued(), 0);
}

TEST(classic_events_follow_the_mouse_once_the_game_asks) {
    setup();
    events_post_mouse(10, 20, true); /* no classic call yet: as for the Carbon games */
    events_post_mouse(10, 20, false);
    CHECK_EQ(events_queued(), 1); /* only the resume */
    record e;
    take_launch_events();

    events_post_mouse(100, 50, true);
    CHECK_EQ(call_import("Button", 0), 1);
    CHECK_EQ(call_import("StillDown", 0), 0); /* its mouseDown is waiting */
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.what, 1);
    CHECK_EQ(e.h, 100);
    CHECK_EQ(e.v, 50);
    CHECK_EQ(e.modifiers & 0x0080, 0); /* the button is down */
    CHECK_EQ(call_import("StillDown", 0), 1);
    uint32_t pt = scratch(4);
    call_import("GetGlobalMouse", 1, pt);
    CHECK_EQ(gm_r16(pt), 50);
    CHECK_EQ(gm_r16(pt + 2), 100);

    events_post_mouse(120, 60, false);
    CHECK_EQ(call_import("Button", 0), 0);
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.what, 2);
    CHECK_EQ(e.h, 120);
    CHECK(e.modifiers & 0x0080);
}

/* Key code k is bit k % 8 of byte k / 8. */
TEST(classic_events_get_keys_maps_key_codes_to_bits) {
    setup();
    events_post_key(SDL_SCANCODE_Z, true, false);      /* key code 0x06 */
    events_post_key(SDL_SCANCODE_LSHIFT, true, false); /* 0x38 */
    uint32_t km = scratch(16);
    call_import("GetKeys", 1, km);
    CHECK_EQ(gm_r8(km), 1u << 6);
    CHECK_EQ(gm_r8(km + 7), 1u << 0);
    for (uint32_t i = 1; i < 16; i++)
        if (i != 7)
            CHECK_EQ(gm_r8(km + i), 0);
    CHECK_EQ(call_import("GetCurrentKeyModifiers", 0), KM_SHIFT);
    events_post_key(SDL_SCANCODE_Z, false, false);
    call_import("GetKeys", 1, km);
    CHECK_EQ(gm_r8(km), 0);
    /* Escape where Golden Logres looks for it, 0x2000 in big-endian word 1 */
    events_post_key(SDL_SCANCODE_LSHIFT, false, false);
    events_post_key(SDL_SCANCODE_ESCAPE, true, false); /* 0x35 */
    call_import("GetKeys", 1, km);
    CHECK_EQ(gm_r32(km + 4), 0x2000u);
}

static int dialog_keys, dialog_clicks;
static void count_key(uint32_t vkey, uint8_t chr, uint32_t mods) {
    (void)vkey, (void)chr, (void)mods;
    dialog_keys++;
}
static void count_click(int x, int y, bool down) {
    (void)x, (void)y;
    dialog_clicks += down;
}
static void no_text(const char *utf8) { (void)utf8; }

/* Under a modal dialog the keys and clicks are the dialog's, but GetKeys
   and Button still see them, as on a Mac: Golden Logres's key-setting
   dialog waits for a key that way. */
TEST(classic_events_get_keys_and_button_see_through_a_dialog) {
    setup();
    take_launch_events();
    static const ev_modal_sink sink = {count_key, count_click, no_text};
    events_set_modal(&sink);
    events_post_key(SDL_SCANCODE_M, true, false); /* key code 0x2E */
    events_post_mouse(10, 20, true);
    CHECK_EQ(dialog_keys, 1);
    CHECK_EQ(dialog_clicks, 1);
    uint32_t km = scratch(16);
    call_import("GetKeys", 1, km);
    CHECK_EQ(gm_r8(km + 5), 1u << 6);
    CHECK_EQ(call_import("Button", 0), 1);
    events_post_key(SDL_SCANCODE_M, false, false);
    events_post_mouse(10, 20, false);
    call_import("GetKeys", 1, km);
    CHECK_EQ(gm_r8(km + 5), 0);
    CHECK_EQ(call_import("Button", 0), 0);
    events_set_modal(NULL);
    record e;
    CHECK(!wne(EVERY, &e)); /* none of it was the game's */
}

/* A timer proc that adds 1 to COUNTER. */
static void emit_counter(void) {
    uint32_t code[] = {
        ppc_lis(4, COUNTER >> 16), ppc_ori(4, 4, COUNTER & 0xFFFF),
        ppc_lwz(5, 0, 4), ppc_addi(5, 5, 1), ppc_stw(5, 0, 4), PPC_BLR,
    };
    put_words(PROC, code, (int)(sizeof code / sizeof code[0]));
    gm_w32(TV_PROC, PROC);
    gm_w32(TV_PROC + 4, 0);
    gm_w32(COUNTER, 0);
}

TEST(classic_events_wait_next_event_fires_timers_while_it_sleeps) {
    setup();
    emit_counter();
    take_launch_events();
    cpu_set_fpr(1, 0.0);
    cpu_set_fpr(2, 1.0 / 60);
    uint32_t out = scratch(4);
    CHECK_EQ(call_import("InstallEventLoopTimer", 8, call_import("GetMainEventLoop", 0), 0u, 0u, 0u,
                         0u, call_import("NewEventLoopTimerUPP", 1, TV_PROC), 0u, out),
             0);
    uint32_t before = misc_ticks(), r = scratch(16);
    CHECK_EQ(call_import("WaitNextEvent", 4, EVERY, r, 10u, 0u), 0);
    CHECK(misc_ticks() - before >= 10);
    CHECK(gm_r32(COUNTER) >= 9);
    CHECK_EQ(call_import("GetCurrentEventLoop", 0), call_import("GetMainEventLoop", 0));
}

static void child_quit(void *unused) {
    (void)unused;
    setup();
    take_launch_events();
    events_request_quit();
    uint32_t r = scratch(16);
    if (!call_import("WaitNextEvent", 4, EVERY, r, 0u, 0u) || gm_r16(r) != 23 ||
        gm_r32(r + 2) != 0x61657674u || gm_r32(r + 10) != 0x71756974u)
        exit(3);
    call_import("AEProcessAppleEvent", 1, r);
    exit(4); /* the game has no quit handler, so it exits before this */
}

TEST(classic_events_the_quit_comes_as_a_high_level_event) {
    char out[4096];
    CHECK_EQ(test_run_child(child_quit, NULL, out, sizeof out), 0);
    CHECK_CONTAINS(out, "quit requested; the game has no quit handler");
}

/* A classic game that takes no Apple Events (Tristan) gets the quit as
   Cmd-Q, which its menu's key equivalent turns into its Quit. */
TEST(classic_events_the_quit_comes_as_cmd_q_without_apple_events) {
    static const char *const plain[] = {"WaitNextEvent"};
    setup_with(plain, sizeof plain / sizeof plain[0]);
    take_launch_events();
    events_request_quit();
    record e;
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.what, 3); /* keyDown */
    CHECK_EQ(e.message, 0x0C71u);
    CHECK(e.modifiers & 0x0100u); /* cmdKey */
    CHECK(events_quit_requested());
}

/* A quit while a dialog is up waits for it: the game reads its Cmd-Q
   once the dialog closes. */
TEST(classic_events_a_quit_under_a_dialog_comes_once_it_closes) {
    static const char *const plain[] = {"WaitNextEvent"};
    setup_with(plain, sizeof plain / sizeof plain[0]);
    take_launch_events();
    static const ev_modal_sink sink = {count_key, count_click, no_text};
    events_set_modal(&sink);
    events_request_quit();
    CHECK(!events_post_command_key('n')); /* a menu choice isn't held for later */
    events_set_modal(NULL);
    record e;
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.what, 3);
    CHECK_EQ(e.message, 0x0C71u);
    CHECK(!wne(EVERY, &e));
}

/* The same before the game's first event call: Tristan shows its startup
   alerts before it calls WaitNextEvent, and a quit then must still come as
   Cmd-Q, not as an Apple Event it never reads. */
TEST(classic_events_a_quit_before_the_first_event_call_comes_as_cmd_q) {
    static const char *const plain[] = {"WaitNextEvent"};
    setup_with(plain, sizeof plain / sizeof plain[0]);
    events_request_quit();
    record e;
    bool cmd_q = false;
    for (int i = 0; i < 4 && !cmd_q; i++) {
        CHECK(wne(EVERY, &e));
        cmd_q = e.what == 3 && e.message == 0x0C71u && (e.modifiers & 0x0100u);
    }
    CHECK(cmd_q);
}

TEST(classic_events_caret_and_double_click_times) {
    setup();
    CHECK_EQ(call_import("GetCaretTime", 0), 32);
    CHECK_EQ(call_import("GetDblTime", 0), 30);
}

/* A window GetNewCWindow shows is activated first, then gets an updateEvt
   after the queue's events, every time, until BeginUpdate takes it. */
TEST(classic_events_update_a_new_window_until_begin_update) {
    setup();
    static const uint8_t wind[] = {0, 0, 0, 0, 0, 100, 0, 200, 0, 3, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    rsrc_entry res = {.type = FOURCC('W', 'I', 'N', 'D'), .id = 128, .len = sizeof wind};
    const uint8_t *d = wind;
    size_t len;
    uint8_t *fork = rsrc_build(&res, &d, 1, &len);
    char err[256];
    CHECK(rsrc_open(fork, len, err, sizeof err));
    qd_init(640, 480, 8);
    qd_register();
    record e;
    CHECK(wne(EVERY, &e)); /* the resume */
    uint32_t w = call_import("GetNewCWindow", 3, 128u, 0u, (uint32_t)-1);
    events_post_key(SDL_SCANCODE_Z, true, false);
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.what, 8); /* activateEvt, ahead of everything */
    CHECK_EQ(e.message, w);
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.what, 23); /* the open application Apple Event, queued first */
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.what, 3); /* then the key */
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.what, 6); /* updateEvt */
    CHECK_EQ(e.message, w);
    CHECK(!wne(KEY_DOWN_MASK, &e)); /* not in the mask */
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.what, 6); /* again */
    call_import("BeginUpdate", 1, w);
    call_import("EndUpdate", 1, w);
    CHECK(!wne(EVERY, &e));
    CHECK_EQ(e.what, 0);
    rsrc_close();
    free(fork);
}

/* A window coming to the front is activated, ahead of anything queued, and
   the one it covers is deactivated first. PowerPlant starts a window's work
   on its activate, so a window shown with the application already in front
   (after the picker) waits on this. */
TEST(classic_events_activate_the_front_window) {
    setup();
    take_launch_events();
    record e;
    uint32_t a = qd_new_window(800, 600), b = qd_new_window(400, 300);
    CHECK(!wne(EVERY, &e)); /* both hidden */

    call_import("ShowWindow", 1, a);
    events_post_key(SDL_SCANCODE_A, true, false);
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.what, 8); /* activateEvt */
    CHECK_EQ(e.message, a);
    CHECK_EQ(e.modifiers & 1, 1); /* activeFlag */
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.what, 3); /* then the key */
    CHECK(!wne(EVERY, &e));

    call_import("ShowWindow", 1, b);
    uint32_t r = scratch(16);
    CHECK_EQ(call_import("EventAvail", 2, EVERY, r), 1); /* a peek changes nothing */
    CHECK_EQ(read_record(r).message, a);
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.what, 8);
    CHECK_EQ(e.message, a);
    CHECK_EQ(e.modifiers & 1, 0); /* a deactivate */
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.message, b);
    CHECK_EQ(e.modifiers & 1, 1);
    CHECK(!wne((uint32_t)~(1u << 8) & EVERY, &e)); /* outside the mask, nothing */

    call_import("DisposeWindow", 1, b); /* gone: only a is activated */
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.message, a);
    CHECK_EQ(e.modifiers & 1, 1);
    CHECK(!wne(EVERY, &e));

    call_import("HideWindow", 1, a); /* hidden: deactivated, nothing in front */
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.message, a);
    CHECK_EQ(e.modifiers & 1, 0);
    CHECK(!wne(EVERY, &e));
}

/* A window made after the active one is disposed of can get its address;
   it is still a new window, and is activated. */
TEST(classic_events_activate_a_window_at_a_disposed_ones_address) {
    setup();
    take_launch_events();
    record e;
    uint32_t a = qd_new_window(800, 600);
    call_import("ShowWindow", 1, a);
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.message, a);
    call_import("DisposeWindow", 1, a);
    uint32_t b = qd_new_window(800, 600);
    CHECK_EQ(b, a); /* the case this test is for */
    call_import("ShowWindow", 1, b);
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.what, 8);
    CHECK_EQ(e.message, b);
    CHECK_EQ(e.modifiers & 1, 1);
}

/* The same after full screen ends and begins again (EndFullScreen disposes
   of its window). */
static uint32_t begin_full_screen(void) {
    uint32_t restore = scratch(4), win = scratch(4);
    call_import("BeginFullScreen", 7, restore, 0u, 0u, 0u, win, 0u, 0u);
    return gm_r32(win);
}

TEST(classic_events_activate_full_screen_begun_again) {
    setup();
    take_launch_events();
    record e;
    uint32_t a = begin_full_screen();
    CHECK(a != 0);
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.what, 8);
    CHECK_EQ(e.message, a);
    call_import("EndFullScreen", 2, a, 0u);
    uint32_t b = begin_full_screen();
    CHECK_EQ(b, a);
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.what, 8);
    CHECK_EQ(e.message, b);
    CHECK_EQ(e.modifiers & 1, 1);
}

/* A classic game that reads the keyboard with GetKeys and leaves the queue
   alone (Tristan, while a ball is in play) loses its oldest input, as a Mac
   does when the Operating System event queue is full, never the newest:
   30 presses of Z and one of X leave 20 events, and X is the last. */
TEST(classic_events_a_full_queue_drops_its_oldest_input) {
    setup();
    take_launch_events();
    uint32_t km = scratch(16);
    for (int i = 0; i < 30; i++) {
        events_post_key(SDL_SCANCODE_Z, true, false);
        events_post_key(SDL_SCANCODE_Z, false, false);
    }
    events_post_key(SDL_SCANCODE_X, true, false); /* key code 0x07 */
    CHECK_EQ(events_queued(), EV_CLASSIC_QUEUE);
    call_import("GetKeys", 1, km); /* play input is the keyboard's state, not the queue's */
    CHECK_EQ(gm_r8(km), 1u << 7);
    record e;
    int downs = 0;
    while (wne(EVERY, &e)) {
        CHECK_EQ(e.what, 3);
        downs++;
    }
    CHECK_EQ(downs, EV_CLASSIC_QUEUE); /* the last 19 Zs and X: their ups take no place */
    CHECK_EQ(e.what, 0);
    events_post_key(SDL_SCANCODE_X, false, false);
    events_post_key(SDL_SCANCODE_C, true, false);
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.message >> 8, 0x08u); /* C */
}

/* FlushEvents takes the keyboard and mouse events in its mask, oldest
   first, up to the first in its stop mask; the rest stay in order. */
TEST(classic_events_flush_events_stops_at_the_stop_mask) {
    setup();
    take_launch_events();
    events_post_key(SDL_SCANCODE_Z, true, false);
    events_post_mouse(10, 20, true);
    events_post_key(SDL_SCANCODE_X, true, false);
    events_post_mouse(10, 20, false);
    events_post_key(SDL_SCANCODE_C, true, false);
    call_import("FlushEvents", 2, KEY_DOWN_MASK | 1u << 1, 1u << 2); /* keyDown and mouseDown, stop at mouseUp */
    CHECK_EQ(events_queued(), 2);
    record e;
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.what, 2);
    CHECK(wne(EVERY, &e));
    CHECK_EQ(e.what, 3);
    CHECK_EQ(e.message >> 8, 0x08u); /* C */
    events_post_key(SDL_SCANCODE_Z, true, false);
    events_post_mouse(10, 20, true);
    call_import("FlushEvents", 2, EVERY, 0u);
    CHECK_EQ(events_queued(), 0);
}

/* A Carbon game's FlushEvents leaves its queue alone. */
TEST(classic_events_a_carbon_games_flush_events_does_nothing) {
    setup();
    events_post_key(SDL_SCANCODE_Z, true, false);
    events_post_key(SDL_SCANCODE_Z, false, false);
    int queued = events_queued();
    CHECK(queued >= 2);
    call_import("FlushEvents", 2, EVERY, 0u);
    CHECK_EQ(events_queued(), queued);
}

/* A Carbon game's queue (Loony Labyrinth's) is left as it was: it holds
   64 events, and what comes once it is full is dropped. */
TEST(classic_events_a_carbon_games_queue_keeps_its_size) {
    setup();
    for (int i = 0; i < 40; i++) {
        events_post_key(SDL_SCANCODE_Z, true, false);
        events_post_key(SDL_SCANCODE_Z, false, false);
    }
    CHECK_EQ(events_queued(), EV_MAX_EVENTS);
}
