#include "test.h"

#include <stdlib.h>

#include "asm.h"
#include "events.h"
#include "harness.h"
#include <SDL3/SDL_scancode.h>

#include "keymap.h"
#include "memmgr.h"
#include "misc.h"

/* The classic Event Manager a PowerPlant game (Golden Logres, Angel Egg)
   uses, on the Carbon queue. */

static const char *const names[] = {
    "WaitNextEvent", "GetNextEvent", "EventAvail", "Button", "StillDown", "GetKeys",
    "GetCurrentKeyModifiers", "GetGlobalMouse", "GetCaretTime", "GetDblTime",
    "GetCurrentEventLoop", "AEProcessAppleEvent", "NewEventLoopTimerUPP", "GetMainEventLoop",
    "InstallEventLoopTimer",
};

#define EVERY 0xFFFFu
#define KEY_DOWN_MASK (1u << 3)
#define PROC    (GUEST_IMAGE_BASE + 0x200)
#define TV_PROC (GUEST_IMAGE_BASE + 0x8100)
#define COUNTER (GUEST_IMAGE_BASE + 0x8200)

static void setup(void) {
    unsetenv("LOONY_EXIT_AFTER");
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    misc_init();
    misc_register();
    events_init();
    events_register();
    unsetenv("LOONY_FIXED_CLOCK");
}

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

TEST(classic_events_caret_and_double_click_times) {
    setup();
    CHECK_EQ(call_import("GetCaretTime", 0), 32);
    CHECK_EQ(call_import("GetDblTime", 0), 30);
}
