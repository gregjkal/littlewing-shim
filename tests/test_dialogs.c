#include "test.h"

#include <stdlib.h>

#include "dialogs.h"
#include "events.h"
#include "harness.h"
#include "memmgr.h"
#include "misc.h"
#include "qd.h"
#include "rsrc.h"
#include "script.h"

static const char *const names[] = {
    "Alert", "StopAlert", "ParamText", "GetNewDialog", "ModalDialog", "GetDialogItem",
    "GetDialogItemText", "DisposeDialog",
};
static uint8_t *fork_buf;

/* A fixed-clock machine with an 800x600 screen and the game's resources.
   The modal loop runs the script, so tests drive dialogs with it. */
static bool setup(void) {
    if (!test_game_present())
        return false;
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    misc_init();
    unsetenv("LOONY_FIXED_CLOCK");
    qd_init(800, 600, 8);
    events_init();
    dialogs_init();
    dialogs_register();
    if (!fork_buf) {
        char path[1100];
        size_t len;
        snprintf(path, sizeof path, "%s/..namedfork/rsrc", test_game_exe_path());
        fork_buf = read_file(path, &len);
        char err[256];
        if (!fork_buf || !rsrc_open(fork_buf, len, err, sizeof err))
            fatal("can't open the resource fork");
    }
    return true;
}

static void script(const char *text) {
    char err[256];
    if (!script_parse(text, err, sizeof err))
        fatal("bad test script: %s", err);
}

static uint32_t screen_hash(void) {
    qd_pixels px;
    qd_palette pal;
    qd_screen(&px, &pal);
    return fnv1a32(px.base, (size_t)px.row_bytes * (size_t)rect_h(px.bounds));
}

static uint8_t screen_at(int x, int y) {
    qd_pixels px;
    qd_palette pal;
    qd_screen(&px, &pal);
    return px.base[(size_t)y * px.row_bytes + (size_t)x];
}

TEST(dialogs_alert_text_lists_buttons_and_text) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    char text[1024];
    dialogs_alert_text(901, text, sizeof text);
    CHECK_CONTAINS(text, "Play Demo | Quit | Buy Now | Enter Key-Code | Thank you for trying");
    dialogs_alert_text(4242, text, sizeof text);
    CHECK_STR(text, "");
}

static void child_auto_alert(void *unused) {
    (void)unused;
    setenv("LOONY_AUTO_ALERTS", "1", 1);
    if (!setup())
        exit(3);
    if (call_import("Alert", 2, 901u, 0u) != 1)
        exit(4);
    if (call_import("StopAlert", 2, 900u, 0u) != 1)
        exit(5);
    if (misc_ticks() != 0) /* no waiting */
        exit(6);
}

TEST(dialogs_auto_alerts_answer_the_default_item_at_once) {
    SKIP_UNLESS_GAME();
    char out[16384];
    CHECK_EQ(test_run_child(child_auto_alert, NULL, out, sizeof out), 0);
    CHECK_CONTAINS(out, "loony: Alert 901 (answering item 1): Play Demo");
    CHECK_CONTAINS(out, "loony: Alert 900 (answering item 1): OK");
}

static void child_alert_waits(void *unused) {
    (void)unused;
    if (!setup())
        exit(3);
    uint32_t before = screen_hash();
    script("10 down return\n11 up return\n");
    if (call_import("Alert", 2, 901u, 0u) != 1)
        exit(4);
    if (misc_ticks() < 10) /* it waited for Return */
        exit(5);
    if (screen_hash() != before) /* and put the screen back */
        exit(6);
}

TEST(dialogs_alert_waits_for_return_and_restores_the_screen) {
    SKIP_UNLESS_GAME();
    char out[16384];
    CHECK_EQ(test_run_child(child_alert_waits, NULL, out, sizeof out), 0);
    CHECK_CONTAINS(out, "loony: Alert 901: Play Demo | Quit");
    CHECK_CONTAINS(out, "loony: Alert 901: answered item 1 (Play Demo)");
}

/* Alert 901 sits at (139, 150) on the 800x600 screen (alert position:
   centered, a third of the way down); "Enter Key-Code" is item 4 at
   (110, 260)-(130, 388) inside it, and "Quit" item 2 at (110, 400). */
static void child_alert_click(void *unused) {
    (void)unused;
    if (!setup())
        exit(3);
    /* A click outside every button and Esc (901 has no Cancel) do nothing. */
    script("5 click 9999 9999\n10 down esc\n11 up esc\n20 click 460 270\n");
    if (call_import("Alert", 2, 901u, 0u) != 4)
        exit(4);
}

TEST(dialogs_alert_buttons_answer_clicks) {
    SKIP_UNLESS_GAME();
    char out[16384];
    CHECK_EQ(test_run_child(child_alert_click, NULL, out, sizeof out), 0);
    CHECK_CONTAINS(out, "loony: Alert 901: answered item 4 (Enter Key-Code)");
}

static uint32_t item_text(uint32_t dlg, int n, char *out) {
    uint32_t type = scratch(2), h = scratch(4), box = scratch(8), str = scratch(256);
    call_import("GetDialogItem", 5, dlg, (uint32_t)n, type, h, box);
    call_import("GetDialogItemText", 2, gm_r32(h), str);
    gm_read_pstr(str, out);
    return gm_r16(type);
}

/* DLOG 911 (centered at the alert position): e-mail field item 5, key
   field item 6, Register item 3 (the default), Cancel item 4. */
static void child_registration_form(void *unused) {
    (void)unused;
    if (!setup())
        exit(3);
    uint32_t before = screen_hash();
    uint32_t dlg = call_import("GetNewDialog", 3, 911u, 0u, 0xFFFFFFFFu);
    if (dlg < DLG_TAG_BASE)
        exit(4);
    if (screen_hash() == before) /* drawn at once: the DLOG is visible */
        exit(5);
    script("5 type me@example.com\n6 down backspace\n7 up backspace\n8 type m\n9 down tab\n10 up tab\n"
           "11 type ab\xc3\xa9 cd\n12 down lshift\n13 down tab\n14 up tab\n15 up lshift\n"
           "16 type !\n20 down return\n21 up return\n");
    uint32_t hit = scratch(2);
    call_import("ModalDialog", 2, 0u, hit);
    if (gm_r16(hit) != 3)
        exit(6);
    char t[256];
    if (item_text(dlg, 5, t) != DLG_ITEM_EDIT_TEXT || strcmp(t, "me@example.com!") != 0)
        exit(7);
    if (item_text(dlg, 6, t) != DLG_ITEM_EDIT_TEXT || strcmp(t, "ab cd") != 0) /* only ASCII is typed */
        exit(8);
    if (item_text(dlg, 7, t) != (DLG_ITEM_STATIC_TEXT | DLG_ITEM_DISABLED) || strcmp(t, "E-mail address:") != 0)
        exit(9);
    uint32_t type = scratch(2), h = scratch(4), box = scratch(8);
    call_import("GetDialogItem", 5, dlg, 3u, type, h, box);
    if (gm_r16(type) != DLG_ITEM_BUTTON || gm_r32(h) != 0 || gm_r16(box) != 170 || gm_r16(box + 2) != 320)
        exit(10);
    script("30 down esc\n31 up esc\n");
    call_import("ModalDialog", 2, 0u, hit);
    if (gm_r16(hit) != 4) /* Esc is Cancel */
        exit(11);
    call_import("DisposeDialog", 1, dlg);
    if (screen_hash() != before)
        exit(12);
    call_import("GetDialogItem", 5, dlg, 3u, type, h, box); /* disposed: crashes */
}

TEST(dialogs_registration_form_takes_typing_and_buttons) {
    SKIP_UNLESS_GAME();
    char out[16384];
    CHECK_EQ(test_run_child(child_registration_form, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "loony: GetNewDialog 911");
    CHECK_CONTAINS(out, "GetDialogItem: 0x0b000000 is not a dialog");
}

static void child_click_fields(void *unused) {
    (void)unused;
    if (!setup())
        exit(3);
    uint32_t dlg = call_import("GetNewDialog", 3, 911u, 0u, 0xFFFFFFFFu);
    /* 911 is 440x210, so it sits at (180, 130). The key field (130, 140) ->
       (260, 320); Register (170, 320) -> (300, 500). Clicking the key field
       moves the caret there. */
    script("5 click 340 265\n6 type K1\n7 click 510 305\n");
    uint32_t hit = scratch(2);
    call_import("ModalDialog", 2, 0u, hit);
    char t[256];
    if (gm_r16(hit) != 3)
        exit(4);
    if (item_text(dlg, 5, t), strcmp(t, "") != 0)
        exit(5);
    if (item_text(dlg, 6, t), strcmp(t, "K1") != 0)
        exit(6);
    if (screen_at(0, 0) != 0) /* the screen outside the dialog is untouched (white) */
        exit(7);
}

TEST(dialogs_clicks_focus_fields_and_press_buttons) {
    SKIP_UNLESS_GAME();
    char out[16384];
    CHECK_EQ(test_run_child(child_click_fields, NULL, out, sizeof out), 0);
}

/* ModalDialog runs the newest dialog, whatever slot it landed in. */
static void child_newest(void *unused) {
    (void)unused;
    if (!setup())
        exit(3);
    uint32_t a = call_import("GetNewDialog", 3, 911u, 0u, 0xFFFFFFFFu);
    uint32_t b = call_import("GetNewDialog", 3, 911u, 0u, 0xFFFFFFFFu);
    call_import("DisposeDialog", 1, a);
    uint32_t c = call_import("GetNewDialog", 3, 911u, 0u, 0xFFFFFFFFu); /* a's slot */
    if (c != a)
        exit(4);
    script("5 type C\n6 down return\n7 up return\n");
    call_import("ModalDialog", 2, 0u, scratch(2));
    char t[256];
    if (item_text(c, 5, t), strcmp(t, "C") != 0)
        exit(5);
    if (item_text(b, 5, t), strcmp(t, "") != 0)
        exit(6);
}

TEST(dialogs_modal_dialog_runs_the_newest_dialog) {
    SKIP_UNLESS_GAME();
    char out[16384];
    CHECK_EQ(test_run_child(child_newest, NULL, out, sizeof out), 0);
}

TEST(dialogs_param_text_substitutes) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    uint32_t s[4];
    const char *v[4] = {"A", "BB", "", "D"};
    for (int i = 0; i < 4; i++) {
        s[i] = scratch(16);
        gm_write_pstr(s[i], v[i]);
    }
    call_import("ParamText", 4, s[0], s[1], s[2], s[3]);
    char text[1024];
    dialogs_alert_text(9000, text, sizeof text);
    CHECK_CONTAINS(text, "ABBD");
    call_import("ParamText", 4, 0u, 0u, 0u, 0u);
    dialogs_alert_text(9000, text, sizeof text);
    CHECK_STR(text, "OK | ");
}

static void child_filter(void *unused) {
    (void)unused;
    setup();
    call_import("Alert", 2, 901u, 0x1234u);
}

static void child_storage(void *unused) {
    (void)unused;
    setup();
    call_import("GetNewDialog", 3, 911u, 0x1234u, 0xFFFFFFFFu);
}

static void child_auto_modal(void *unused) {
    (void)unused;
    setenv("LOONY_AUTO_ALERTS", "1", 1);
    setup();
    call_import("GetNewDialog", 3, 911u, 0u, 0xFFFFFFFFu);
    call_import("ModalDialog", 2, 0u, scratch(2));
}

TEST(dialogs_unsupported_uses_crash_with_a_report) {
    SKIP_UNLESS_GAME();
    char out[16384];
    CHECK_EQ(test_run_child(child_filter, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "Alert: filter procs are not supported");
    CHECK_EQ(test_run_child(child_storage, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "GetNewDialog: caller-supplied dialog storage is not supported");
    CHECK_EQ(test_run_child(child_auto_modal, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "ModalDialog: DLOG 911 can't be answered automatically");
}
