#include "test.h"

#include <stdlib.h>
#include <sys/stat.h>

#include "asm.h"
#include "cf.h"
#include "cgimage.h"
#include "dialogs.h"
#include "events.h"
#include "files.h"
#include "fork_build.h"
#include "harness.h"
#include "memmgr.h"
#include "mixer.h"
#include "misc.h"
#include "ppc.h"
#include "qd.h"
#include "rsrc.h"
#include "script.h"
#include "snapshot.h"

static const char *const names[] = {
    "Alert", "StopAlert", "NoteAlert", "InitDialogs", "ParamText", "GetNewDialog", "ModalDialog", "GetDialogItem",
    "GetDialogItemText", "DisposeDialog", "DrawDialog",
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
    call_import("InitDialogs", 1, 0u); /* changes nothing */
    if (call_import("Alert", 2, 901u, 0u) != 1)
        exit(4);
    if (call_import("StopAlert", 2, 900u, 0u) != 1)
        exit(5);
    if (call_import("NoteAlert", 2, 901u, 0u) != 1)
        exit(7);
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

/* DrawDialog paints a shown dialog again over whatever covered it (Angel
   Egg's Register calls it right after GetNewDialog). */
static void child_draw_dialog(void *unused) {
    (void)unused;
    if (!setup())
        exit(3);
    uint32_t dlg = call_import("GetNewDialog", 3, 911u, 0u, 0xFFFFFFFFu);
    uint32_t drawn = screen_hash();
    qd_pixels px;
    qd_palette pal;
    qd_screen(&px, &pal);
    for (int y = 140; y < 300; y++) /* 911 sits at (180, 130) */
        memset(px.base + (size_t)y * px.row_bytes + 200, 0x23, 400);
    if (screen_hash() == drawn)
        exit(4);
    call_import("DrawDialog", 1, dlg);
    if (screen_hash() != drawn)
        exit(5);
    call_import("DisposeDialog", 1, dlg);
    call_import("DrawDialog", 1, dlg); /* disposed: crashes */
}

TEST(dialogs_draw_dialog_paints_it_again) {
    SKIP_UNLESS_GAME();
    char out[16384];
    CHECK_EQ(test_run_child(child_draw_dialog, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "DrawDialog: 0x0b000000 is not a dialog");
}

/* Tristan's machine: a 640x480 screen, its resources, and the Window
   Manager calls it makes on a DialogRef. */
static const char *const tr_names[] = {
    "GetNewDialog", "ModalDialog", "DisposeDialog", "ShowWindow", "HideWindow", "SelectWindow",
};

static void tr_setup(void) {
    harness_init(tr_names, sizeof tr_names / sizeof tr_names[0]);
    mm_init();
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    misc_init();
    unsetenv("LOONY_FIXED_CLOCK");
    qd_init(640, 480, 8);
    qd_register();
    events_init();
    dialogs_init();
    dialogs_register();
    char path[1100], err[256];
    size_t len;
    snprintf(path, sizeof path, "%s/..namedfork/rsrc", test_tr_exe_path());
    uint8_t *buf = read_file(path, &len);
    if (!buf || !rsrc_open(buf, len, err, sizeof err))
        fatal("can't open Tristan's resource fork");
}

/* Tristan's Register: GetNewDialog(911), then ShowWindow, HideWindow and
   SelectWindow on the DialogRef, which isn't a window port here. DITL 911's
   item 1 is an OK button below the dialog's box (at 230 in a box 142
   high): a Mac clips it away and a click there misses the dialog, but
   Return still answers item 1, the default ModalDialog uses, which
   Tristan's loop passes over. The box is at (86, 112), 468 by 142. */
static void child_tristan_register_window_calls(void *unused) {
    (void)unused;
    tr_setup();
    uint32_t blank = screen_hash();
    uint32_t dlg = call_import("GetNewDialog", 3, 911u, 0u, 0xFFFFFFFFu);
    uint32_t drawn = screen_hash();
    if (drawn == blank) /* the DLOG is visible */
        exit(4);
    call_import("ShowWindow", 1, dlg); /* already shown */
    call_import("SelectWindow", 1, dlg);
    if (screen_hash() != drawn)
        exit(5);
    for (int y = 112 + 142 + 4; y < 480; y++) /* nothing below the box and its frame: no stray OK */
        for (int x = 0; x < 640; x++)
            if (screen_at(x, y) != 0)
                exit(6);
    call_import("HideWindow", 1, dlg);
    if (screen_hash() != blank)
        exit(7);
    call_import("ShowWindow", 1, dlg);
    if (screen_hash() != drawn)
        exit(8);
    /* where the OK would be (116..174, 342..362), then Cancel (356..434, 227..247) */
    script("5 click 145 352\n6 click 395 237\n");
    uint32_t hit = scratch(2);
    call_import("ModalDialog", 2, 0u, hit);
    if (gm_r16(hit) != 3)
        exit(9);
    script("10 down return\n11 up return\n");
    call_import("ModalDialog", 2, 0u, hit);
    if (gm_r16(hit) != 1)
        exit(10);
    call_import("DisposeDialog", 1, dlg);
    if (screen_hash() != blank)
        exit(11);
    call_import("ShowWindow", 1, dlg); /* disposed: crashes */
}

TEST(dialogs_tristan_shows_hides_and_selects_its_register_dialog) {
    SKIP_UNLESS_TR();
    char out[16384];
    CHECK_EQ(test_run_child(child_tristan_register_window_calls, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "ShowWindow: 0x0b000000 is not a port");
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

/* ---- ModalDialog's filter proc (no game needed) ---- */

/* DLOG 200: OK (item 1) and an edit field (item 2), in DITL 200. */
static const uint8_t filter_dlog[] = {
    0, 0, 0, 0, 0, 100, 0, 200, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 200, 0, 0, 0, 0,
};
static const uint8_t filter_ditl[] = {
    0, 1,
    0, 0, 0, 0, 0, 70, 0, 10, 0, 90, 0, 60, DLG_ITEM_BUTTON, 2, 'O', 'K',
    0, 0, 0, 0, 0, 10, 0, 10, 0, 26, 0, 150, DLG_ITEM_EDIT_TEXT, 0,
};

static uint8_t *filter_fork;

static void filter_setup(void) {
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    const res rs[] = {
        {FOURCC('D', 'I', 'T', 'L'), 200, filter_ditl, sizeof filter_ditl},
        {FOURCC('D', 'L', 'O', 'G'), 200, filter_dlog, sizeof filter_dlog},
    };
    size_t len;
    free(filter_fork);
    filter_fork = build_fork(rs, 2, &len);
    char err[256];
    if (!rsrc_open(filter_fork, len, err, sizeof err))
        fatal("rsrc_open: %s", err);
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    misc_init();
    unsetenv("LOONY_FIXED_CLOCK");
    qd_init(800, 600, 8);
    events_init();
    dialogs_init();
    dialogs_register();
}

/* filter(dialog, event, itemHit): counts its calls in calls[0] and keeps
   the event's modifiers in calls[1]; on a key down it answers item 2. */
static uint32_t emit_filter(uint32_t calls) {
    const uint32_t words[] = {
        ppc_lis(8, calls >> 16), ppc_ori(8, 8, calls & 0xFFFF),
        ppc_lwz(9, 0, 8), ppc_addi(9, 9, 1), ppc_stw(9, 0, 8),
        0xA0C40000u, /* lhz r6,0(r4): what */
        0xA0E4000Eu, /* lhz r7,14(r4): modifiers */
        ppc_stw(7, 4, 8),
        ppc_cmpwi(6, 3),
        0x40820014u, /* bne +20 */
        ppc_addi(6, 0, 2),
        0xB0C50000u, /* sth r6,0(r5) */
        ppc_addi(3, 0, 1),
        PPC_BLR,
        ppc_addi(3, 0, 0),
        PPC_BLR,
    };
    uint32_t code = scratch(sizeof words);
    put_words(code, words, (int)(sizeof words / sizeof words[0]));
    uint32_t tv = scratch(8);
    gm_w32(tv, code);
    return tv;
}

TEST(dialogs_modal_dialog_gives_its_filter_the_events_first) {
    filter_setup();
    uint32_t calls = scratch(8), hit = scratch(2);
    uint32_t filter = emit_filter(calls);
    uint32_t dlg = call_import("GetNewDialog", 3, 200u, 0u, 0xFFFFFFFFu);
    script("5 down a\n6 up a\n");
    call_import("ModalDialog", 2, filter, hit);
    CHECK_EQ(gm_r16(hit), 2); /* the filter's answer, not a button */
    CHECK(gm_r32(calls) > 2);  /* null events, then the key */
    CHECK(gm_r32(calls + 4) & 0x80u); /* btnState: the button is up */
    char t[256];
    CHECK_EQ(item_text(dlg, 2, t), DLG_ITEM_EDIT_TEXT);
    CHECK_STR(t, ""); /* the filter took the key: nothing was typed */
    uint32_t n = gm_r32(calls);
    script("10 down return\n11 up return\n");
    call_import("ModalDialog", 2, 0u, hit);
    CHECK_EQ(gm_r16(hit), 1);
    CHECK_EQ(gm_r32(calls), n); /* the filter was for the call that passed it */
}

/* ---- nib windows and standard alerts (a Mac OS X game's) ---- */

static const char *const nib_names[] = {
    "CreateNibReference", "CreateWindowFromNib", "DisposeNibReference", "GetWindowEventTarget",
    "InstallEventHandler", "ShowWindow", "DisposeWindow", "RunAppModalLoopForWindow",
    "QuitAppModalLoopForWindow", "GetEventParameter", "GetControlByID", "GetControlData",
    "HIViewGetRoot", "HIViewFindByID", "HIViewSetVisible", "CreateStandardAlert", "RunStandardAlert",
    "Alert", "ParamText",
};

/* A window like MONSTER FAIR's Register window: a label, an edit text with
   a ControlID, Cancel and OK. 300x100, centered on the 800x600 screen at
   (250, 250): Cancel is at (370, 310)-(440, 330), OK at (460, 310)-(530, 330). */
static const char ask_xib[] =
    "<?xml version=\"1.0\" standalone=\"yes\"?>\n"
    "<object class=\"NSIBObjectData\">\n"
    "  <object name=\"rootObject\" class=\"NSCustomObject\" id=\"1\">\n  </object>\n"
    "  <array count=\"1\" name=\"allObjects\">\n"
    "    <object class=\"IBCarbonWindow\" id=\"200\">\n"
    "      <string name=\"title\">Ask</string>\n"
    "      <object name=\"rootControl\" class=\"IBCarbonRootControl\" id=\"201\">\n"
    "        <array count=\"4\" name=\"subviews\">\n"
    "          <object class=\"IBCarbonStaticText\" id=\"202\">\n"
    "            <string name=\"title\">Name:</string>\n"
    "            <string name=\"bounds\">20 20 32 110 </string>\n"
    "          </object>\n"
    "          <object class=\"IBCarbonEditText\" id=\"203\">\n"
    "            <ostype name=\"controlSignature\">User</ostype>\n"
    "            <int name=\"controlID\">1</int>\n"
    "            <string name=\"bounds\">20 120 36 280 </string>\n"
    "          </object>\n"
    "          <object class=\"IBCarbonButton\" id=\"204\">\n"
    "            <ostype name=\"command\">not!</ostype>\n"
    "            <string name=\"title\">Cancel</string>\n"
    "            <string name=\"bounds\">60 120 80 190 </string>\n"
    "          </object>\n"
    "          <object class=\"IBCarbonButton\" id=\"205\">\n"
    "            <ostype name=\"command\">ok  </ostype>\n"
    "            <string name=\"title\">OK</string>\n"
    "            <string name=\"bounds\">60 210 80 280 </string>\n"
    "          </object>\n"
    "        </array>\n"
    "        <string name=\"bounds\">0 0 100 300 </string>\n"
    "      </object>\n"
    "      <string name=\"windowRect\">100 100 200 400 </string>\n"
    "    </object>\n"
    "  </array>\n"
    "  <dictionary count=\"1\" name=\"nameTable\">\n"
    "    <string>Ask</string>\n"
    "    <reference idRef=\"200\"/>\n"
    "  </dictionary>\n"
    "</object>\n";

#define CMD_HANDLER (GUEST_IMAGE_BASE + 0x200)
#define HICOMMAND   (GUEST_IMAGE_BASE + 0x8300) /* what the handler was sent */
#define THE_WINDOW  (GUEST_IMAGE_BASE + 0x8320)

/* A bundle in a new temporary folder whose main.nib holds xib. */
static void write_bundle_with(char *dir, size_t cap, const char *xib) {
    test_tmp_dir(dir, cap);
    char path[1200];
    const char *parts[] = {"/Contents", "/Resources", "/English.lproj", "/main.nib"};
    snprintf(path, sizeof path, "%s", dir);
    for (int i = 0; i < 4; i++) {
        strncat(path, parts[i], sizeof path - strlen(path) - 1);
        mkdir(path, 0755);
    }
    strncat(path, "/objects.xib", sizeof path - strlen(path) - 1);
    FILE *f = fopen(path, "w");
    if (!f)
        fatal("can't write %s", path);
    fputs(xib, f);
    fclose(f);
}

static void write_bundle(char *dir, size_t cap) { write_bundle_with(dir, cap, ask_xib); }

/* A Mach-O style machine (direct calls) with a 32-bit screen and the
   bundle's nib. Fixed clock: the modal loop runs the script. */
static void nib_setup(const char *bundle) {
    harness_init_direct(nib_names, sizeof nib_names / sizeof nib_names[0]);
    mm_init();
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    misc_init();
    unsetenv("LOONY_FIXED_CLOCK");
    rsrc_open_empty();
    cf_init();
    cf_set_bundle(bundle);
    qd_init(800, 600, 32);
    events_init();
    dialogs_init();
    cf_register();
    qd_register();
    events_register();
    dialogs_register();
}

static int emit_import_call(uint32_t *c, int n, const char *name) {
    uint32_t a = GUEST_TRAP_ADDR((uint32_t)trap_find(name));
    c[n++] = ppc_lis(0, a >> 16);
    c[n++] = ppc_ori(0, 0, a & 0xFFFF);
    c[n++] = PPC_MTCTR_R0;
    c[n++] = PPC_BCTRL;
    return n;
}

/* A kEventCommandProcess handler, handler(nextHandler, event, userData),
   as MONSTER FAIR's: GetEventParameter(event, '----', 'hcmd', NULL, 14,
   NULL, HICOMMAND), then QuitAppModalLoopForWindow(*THE_WINDOW); noErr. */
static void emit_command_handler(void) {
    uint32_t c[48];
    int n = 0;
    c[n++] = PPC_MFLR_R0;
    c[n++] = PPC_SAVE_LR;
    c[n++] = PPC_PUSH64;
    c[n++] = 0x7C832378u; /* mr r3,r4 */
    c[n++] = ppc_lis(4, 0x2D2D);
    c[n++] = ppc_ori(4, 4, 0x2D2D);
    c[n++] = ppc_lis(5, 0x6863);
    c[n++] = ppc_ori(5, 5, 0x6D64);
    c[n++] = ppc_addi(6, 0, 0);
    c[n++] = ppc_addi(7, 0, 14);
    c[n++] = ppc_addi(8, 0, 0);
    c[n++] = ppc_lis(9, HICOMMAND >> 16);
    c[n++] = ppc_ori(9, 9, HICOMMAND & 0xFFFF);
    n = emit_import_call(c, n, "GetEventParameter");
    c[n++] = ppc_lis(3, THE_WINDOW >> 16);
    c[n++] = ppc_ori(3, 3, THE_WINDOW & 0xFFFF);
    c[n++] = ppc_lwz(3, 0, 3);
    n = emit_import_call(c, n, "QuitAppModalLoopForWindow");
    c[n++] = PPC_POP64;
    c[n++] = PPC_LOAD_LR;
    c[n++] = PPC_MTLR_R0;
    c[n++] = ppc_addi(3, 0, 0);
    c[n++] = PPC_BLR;
    put_words(CMD_HANDLER, c, n);
    memset(gm_ptr(HICOMMAND, 16), 0xEE, 16);
}

/* Opens the nib's window called name, with the command handler installed
   on it, and shows it. */
static uint32_t open_window(const char *name) {
    emit_command_handler();
    uint32_t out = scratch(4);
    if (call_import("CreateNibReference", 2, cf_string("main"), out) != 0)
        fatal("CreateNibReference failed");
    uint32_t nib = gm_r32(out);
    if (call_import("CreateWindowFromNib", 3, nib, cf_string(name), out) != 0)
        fatal("CreateWindowFromNib failed");
    uint32_t w = gm_r32(out);
    gm_w32(THE_WINDOW, w);
    call_import("DisposeNibReference", 1, nib);
    uint32_t list = scratch(8);
    gm_w32(list, EV_CLASS_COMMAND);
    gm_w32(list + 4, EV_COMMAND_PROCESS);
    call_import("InstallEventHandler", 6, call_import("GetWindowEventTarget", 1, w), CMD_HANDLER, 1u,
                list, 0u, 0u);
    call_import("ShowWindow", 1, w);
    return w;
}

static uint32_t open_ask_window(void) { return open_window("Ask"); }

static void child_nib_button(void *bundle) {
    nib_setup(bundle);
    uint32_t w = open_ask_window();
    script("10 click 400 320\n");
    call_import("RunAppModalLoopForWindow", 1, w);
    fprintf(stderr, "clicked: %08x %08x\n", gm_r32(HICOMMAND), gm_r32(HICOMMAND + 4));
    script("30 down return\n32 up return\n");
    call_import("RunAppModalLoopForWindow", 1, w);
    fprintf(stderr, "return: %08x\n", gm_r32(HICOMMAND + 4));
    fprintf(stderr, "past the HICommand: %02x %02x\n", gm_r8(HICOMMAND + 14), gm_r8(HICOMMAND + 15));
    call_import("DisposeWindow", 1, w);
}

TEST(dialogs_nib_button_sends_its_command) {
    char bundle[1024], out[16384];
    write_bundle(bundle, sizeof bundle);
    int status = test_run_child(child_nib_button, bundle, out, sizeof out);
    test_remove_tree(bundle);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "nib window Ask: 300x100, 4 controls");
    CHECK_CONTAINS(out, "nib window Ask: shown");
    CHECK_CONTAINS(out, "nib window Ask: command 'not!' (Cancel)");
    CHECK_CONTAINS(out, "clicked: 00000000 6e6f7421");
    CHECK_CONTAINS(out, "nib window Ask: command 'ok  ' (OK)");
    CHECK_CONTAINS(out, "return: 6f6b2020");
    CHECK_CONTAINS(out, "past the HICommand: ee ee"); /* an HICommand is 14 bytes */
}

static void child_nib_edit_text(void *bundle) {
    nib_setup(bundle);
    uint32_t w = open_ask_window();
    script("10 type Alice\n20 down return\n22 up return\n");
    call_import("RunAppModalLoopForWindow", 1, w);
    uint32_t id = scratch(8), out = scratch(4), out2 = scratch(4), buf = scratch(16), actual = scratch(4);
    gm_w32(id, FOURCC('U', 's', 'e', 'r'));
    gm_w32(id + 4, 1);
    fprintf(stderr, "by id: %d\n", (int32_t)call_import("GetControlByID", 3, w, id, out));
    uint32_t root = call_import("HIViewGetRoot", 1, w);
    call_import("HIViewFindByID", 4, root, FOURCC('U', 's', 'e', 'r'), 1u, out2);
    fprintf(stderr, "same view: %d\n", gm_r32(out) == gm_r32(out2) && gm_r32(out) != 0);
    uint32_t edit = gm_r32(out);
    call_import("GetControlData", 6, edit, 0u, FOURCC('c', 'f', 's', 't'), 4u, buf, actual);
    fprintf(stderr, "cfst: %s (%u bytes)\n", cf_string_text("test", gm_r32(buf)), gm_r32(actual));
    call_import("GetControlData", 6, edit, 0u, FOURCC('t', 'e', 'x', 't'), 3u, buf, actual);
    fprintf(stderr, "text: %.3s (%u bytes)\n", (const char *)gm_ptr(buf, 3), gm_r32(actual));
    gm_w32(id + 4, 7);
    fprintf(stderr, "missing: %d\n", (int32_t)call_import("GetControlByID", 3, w, id, out));
}

TEST(dialogs_nib_edit_text_returns_a_cfstring) {
    char bundle[1024], out[16384];
    write_bundle(bundle, sizeof bundle);
    int status = test_run_child(child_nib_edit_text, bundle, out, sizeof out);
    test_remove_tree(bundle);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "by id: 0\n");
    CHECK_CONTAINS(out, "same view: 1");
    CHECK_CONTAINS(out, "cfst: Alice (4 bytes)");
    CHECK_CONTAINS(out, "text: Ali (5 bytes)");
    CHECK_CONTAINS(out, "missing: -30584");
}

static void child_nib_hidden_button(void *bundle) {
    nib_setup(bundle);
    uint32_t w = open_ask_window();
    uint32_t root = call_import("HIViewGetRoot", 1, w), out = scratch(4);
    /* {0, 0} finds the label, the first control without an ID. A window's
       control refs number its controls in order, so Cancel is two on. */
    call_import("HIViewFindByID", 4, root, 0u, 0u, out);
    call_import("HIViewSetVisible", 2, gm_r32(out) + 2, 0u);
    /* A click where Cancel was does nothing, nor does Esc; Return presses OK. */
    script("10 click 400 320\n20 down esc\n22 up esc\n30 down return\n32 up return\n");
    call_import("RunAppModalLoopForWindow", 1, w);
    fprintf(stderr, "answered: %08x\n", gm_r32(HICOMMAND + 4));
}

TEST(dialogs_nib_hidden_controls_take_no_input) {
    char bundle[1024], out[16384];
    write_bundle(bundle, sizeof bundle);
    int status = test_run_child(child_nib_hidden_button, bundle, out, sizeof out);
    test_remove_tree(bundle);
    CHECK_EQ(status, 0);
    CHECK(!strstr(out, "'not!'"));
    CHECK_CONTAINS(out, "answered: 6f6b2020");
}

static void child_auto_nib(void *bundle) {
    setenv("LOONY_AUTO_ALERTS", "1", 1);
    nib_setup(bundle);
    uint32_t w = open_ask_window();
    call_import("RunAppModalLoopForWindow", 1, w);
    fprintf(stderr, "answered: %08x\n", gm_r32(HICOMMAND + 4));
}

TEST(dialogs_auto_alerts_press_a_nib_windows_ok_button) {
    char bundle[1024], out[16384];
    write_bundle(bundle, sizeof bundle);
    int status = test_run_child(child_auto_nib, bundle, out, sizeof out);
    test_remove_tree(bundle);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "nib window Ask: answering with its default button");
    CHECK_CONTAINS(out, "answered: 6f6b2020");
}

static void child_standard_alert(void *bundle) {
    nib_setup(bundle);
    uint32_t out = scratch(4), hit = scratch(2);
    call_import("CreateStandardAlert", 5, 0u, cf_string("Unexpected operating system error occurred [1:2]"),
                cf_string("Try again."), 0u, out);
    script("10 down return\n12 up return\n");
    call_import("RunStandardAlert", 3, gm_r32(out), 0u, hit);
    fprintf(stderr, "hit: %u\n", gm_r16(hit));
}

/* A machine in the given layout with no resources and the Dialog
   Manager. */
static void empty_setup(bool macho) {
    if (macho)
        harness_init_macho(nib_names, sizeof nib_names / sizeof nib_names[0]);
    else
        harness_init(nib_names, sizeof nib_names / sizeof nib_names[0]);
    rsrc_open_empty();
    dialogs_init();
    dialogs_register();
}

/* MONSTER FAIR calls Alert(136) as it quits after rejecting a license, but
   has no resources: as on Mac OS, Alert returns -1 and shows nothing. */
static void child_alert_without_resources(void *unused) {
    (void)unused;
    empty_setup(true);
    fprintf(stderr, "Alert returned %d\n", (int32_t)call_import("Alert", 2, 136u, 0u));
}

TEST(dialogs_alert_without_resources_returns_minus_one) {
    char out[16384];
    CHECK_EQ(test_run_child(child_alert_without_resources, NULL, out, sizeof out), 0);
    CHECK_CONTAINS(out, "Alert 136: the game has no ALRT 136; returning -1, as Mac OS does");
    CHECK_CONTAINS(out, "Alert returned -1");
}

/* A resource fork holding only STR# 128, two bytes long. */
static const uint8_t one_resource_fork[] = {
    0, 0, 0, 16, 0, 0, 0, 22, 0, 0, 0, 6, 0, 0, 0, 50, /* header: data at 16, map at 22 */
    0, 0, 0, 2, 'h', 'i',                               /* the data */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,     /* map: header copy */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 28, 0, 50,               /* handle, ref, attrs, type list, names */
    0, 0, 'S', 'T', 'R', '#', 0, 0, 0, 10,              /* one type, one resource of it */
    0, 128, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0,         /* STR# 128, no name, data at 0 */
};

/* The same with resources, none of them the ALRT: it's the program's kind
   that says, not whether it has resources. */
static void child_alert_with_other_resources(void *unused) {
    (void)unused;
    empty_setup(true);
    char err[256];
    if (!rsrc_open(one_resource_fork, sizeof one_resource_fork, err, sizeof err))
        fatal("can't open the test fork: %s", err);
    fprintf(stderr, "resources: %d\n", (int)rsrc_total());
    fprintf(stderr, "Alert returned %d\n", (int32_t)call_import("Alert", 2, 136u, 0u));
}

TEST(dialogs_alert_in_a_macho_game_with_other_resources_returns_minus_one) {
    char out[16384];
    CHECK_EQ(test_run_child(child_alert_with_other_resources, NULL, out, sizeof out), 0);
    CHECK_CONTAINS(out, "resources: 1");
    CHECK_CONTAINS(out, "Alert returned -1");
}

/* A classic game is a classic game even with an empty resource map: the
   missing ALRT is the shim's bug, not a Mac OS X game's leftover call. */
static void child_classic_alert_without_resources(void *unused) {
    (void)unused;
    empty_setup(false);
    call_import("Alert", 2, 136u, 0u);
}

TEST(dialogs_a_classic_game_without_resources_crashes_in_alert) {
    char out[16384];
    CHECK_EQ(test_run_child(child_classic_alert_without_resources, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "Alert: ALRT 136 doesn't exist");
    CHECK(strstr(out, "returning -1") == NULL);
}

/* A classic game has its ALRTs, so a missing one is the shim's bug. */
static void child_missing_alrt(void *unused) {
    (void)unused;
    setup();
    call_import("Alert", 2, 9999u, 0u);
}

TEST(dialogs_a_classic_games_missing_alrt_crashes) {
    SKIP_UNLESS_GAME();
    char out[16384];
    CHECK_EQ(test_run_child(child_missing_alrt, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "Alert: ALRT 9999 doesn't exist");
}

TEST(dialogs_standard_alert_shows_its_text_and_answers_ok) {
    char bundle[1024], out[16384];
    write_bundle(bundle, sizeof bundle);
    int status = test_run_child(child_standard_alert, bundle, out, sizeof out);
    test_remove_tree(bundle);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "standard alert: Unexpected operating system error occurred [1:2] | Try again.");
    CHECK_CONTAINS(out, "standard alert: answered item 1");
    CHECK_CONTAINS(out, "hit: 1");
}

/* One nib ref, parsed once, makes any number of windows; its parsed tree
   isn't in a snapshot but comes back with it, parsed again from the text. */
static void child_two_windows_one_nib(void *bundle) {
    nib_setup(bundle);
    files_init(bundle, bundle); /* the modules a snapshot walks, set up */
    mix_init();
    cgimage_init();
    uint32_t out = scratch(4), ask = cf_string("Ask"), other = cf_string("Other");
    call_import("CreateNibReference", 2, cf_string("main"), out);
    uint32_t nib = gm_r32(out);
    call_import("CreateWindowFromNib", 3, nib, ask, out);
    uint32_t a = gm_r32(out);
    call_import("CreateWindowFromNib", 3, nib, ask, out);
    fprintf(stderr, "two windows: %d\n", a != 0 && gm_r32(out) != 0 && gm_r32(out) != a);
    size_t len;
    uint8_t *state = snapshot_state_save(&len);
    snapshot_state_load(state, len); /* over a live tree, freed and parsed again */
    call_import("DisposeNibReference", 1, nib);
    snapshot_state_load(state, len);
    free(state);
    call_import("CreateWindowFromNib", 3, nib, ask, out);
    fprintf(stderr, "after the snapshot: %d\n", gm_r32(out) != 0);
    call_import("CreateWindowFromNib", 3, nib, other, out); /* not in the nib: crashes */
}

TEST(dialogs_one_nib_makes_many_windows) {
    char bundle[1024], out[16384];
    write_bundle(bundle, sizeof bundle);
    int status = test_run_child(child_two_windows_one_nib, bundle, out, sizeof out);
    test_remove_tree(bundle);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "two windows: 1");
    CHECK_CONTAINS(out, "after the snapshot: 1");
    CHECK_CONTAINS(out, "CreateWindowFromNib: the nib has no window called Other");
}

/* XML that can't be read opens as a nib, as before, and the window made
   from it crashes saying why. */
static void child_broken_nib(void *bundle) {
    nib_setup(bundle);
    uint32_t out = scratch(4);
    int32_t err = (int32_t)call_import("CreateNibReference", 2, cf_string("main"), out);
    fprintf(stderr, "CreateNibReference: %d\n", err);
    call_import("CreateWindowFromNib", 3, gm_r32(out), cf_string("Ask"), out);
}

TEST(dialogs_a_broken_nib_crashes_making_a_window) {
    char bundle[1024], out[16384];
    write_bundle_with(bundle, sizeof bundle, "<object class=\"NSIBObjectData\">");
    int status = test_run_child(child_broken_nib, bundle, out, sizeof out);
    test_remove_tree(bundle);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "CreateNibReference: 0\n");
    CHECK_CONTAINS(out, "CreateWindowFromNib: <object> is never closed");
}

/* A window whose OK button's title has non-ASCII text, raw UTF-8 and an
   entity, and a ParamText marker; and two edit texts, 'User' 1 and 2. */
static const char two_fields_xib[] =
    "<?xml version=\"1.0\" standalone=\"yes\"?>\n"
    "<object class=\"NSIBObjectData\">\n"
    "  <object name=\"rootObject\" class=\"NSCustomObject\" id=\"1\">\n  </object>\n"
    "  <array count=\"1\" name=\"allObjects\">\n"
    "    <object class=\"IBCarbonWindow\" id=\"200\">\n"
    "      <string name=\"title\">Two</string>\n"
    "      <object name=\"rootControl\" class=\"IBCarbonRootControl\" id=\"201\">\n"
    "        <array count=\"3\" name=\"subviews\">\n"
    "          <object class=\"IBCarbonEditText\" id=\"202\">\n"
    "            <ostype name=\"controlSignature\">User</ostype>\n"
    "            <int name=\"controlID\">1</int>\n"
    "            <string name=\"bounds\">20 120 36 280 </string>\n"
    "          </object>\n"
    "          <object class=\"IBCarbonEditText\" id=\"203\">\n"
    "            <ostype name=\"controlSignature\">User</ostype>\n"
    "            <int name=\"controlID\">2</int>\n"
    "            <string name=\"bounds\">40 120 56 280 </string>\n"
    "          </object>\n"
    "          <object class=\"IBCarbonButton\" id=\"204\">\n"
    "            <ostype name=\"command\">ok  </ostype>\n"
    "            <string name=\"title\">Caf\xC3\xA9 ^0 &#8594;</string>\n"
    "            <string name=\"bounds\">60 120 80 280 </string>\n"
    "          </object>\n"
    "        </array>\n"
    "        <string name=\"bounds\">0 0 100 300 </string>\n"
    "      </object>\n"
    "      <string name=\"windowRect\">100 100 200 400 </string>\n"
    "    </object>\n"
    "  </array>\n"
    "  <dictionary count=\"1\" name=\"nameTable\">\n"
    "    <string>Two</string>\n"
    "    <reference idRef=\"200\"/>\n"
    "  </dictionary>\n"
    "</object>\n";

/* Edit text 'User' id's text, as GetControlData's 'text' tag gives it. */
static void print_field(uint32_t w, int32_t id) {
    uint32_t cid = scratch(8), out = scratch(4), buf = scratch(64), actual = scratch(4);
    gm_w32(cid, FOURCC('U', 's', 'e', 'r'));
    gm_w32(cid + 4, (uint32_t)id);
    call_import("GetControlByID", 3, w, cid, out);
    call_import("GetControlData", 6, gm_r32(out), 0u, FOURCC('t', 'e', 'x', 't'), 64u, buf, actual);
    fprintf(stderr, "field %d: \"%.*s\"\n", id, (int)gm_r32(actual), (const char *)gm_ptr(buf, 64));
}

static void child_nib_text(void *bundle) {
    setenv("LOONY_AUTO_ALERTS", "1", 1);
    nib_setup(bundle);
    uint32_t p = scratch(16);
    gm_write_pstr(p, "X");
    call_import("ParamText", 4, p, p, p, p);
    uint32_t w = open_window("Two");
    call_import("RunAppModalLoopForWindow", 1, w);
}

/* A nib's text is UTF-8, shown as Mac Roman (spelled in ASCII here), and
   ^0 in it is just text. */
TEST(dialogs_nib_text_is_mac_roman_without_param_text) {
    char bundle[1024], out[16384];
    write_bundle_with(bundle, sizeof bundle, two_fields_xib);
    int status = test_run_child(child_nib_text, bundle, out, sizeof out);
    test_remove_tree(bundle);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "nib window Two: command 'ok  ' (Cafe ^0 ?)");
}

static void child_hide_focused_field(void *bundle) {
    nib_setup(bundle);
    uint32_t w = open_window("Two");
    uint32_t cid = scratch(8), out = scratch(4);
    gm_w32(cid, FOURCC('U', 's', 'e', 'r'));
    gm_w32(cid + 4, 1);
    call_import("GetControlByID", 3, w, cid, out);
    call_import("HIViewSetVisible", 2, gm_r32(out), 0u); /* the first field, which has the focus */
    script("10 type Bob\n20 down return\n22 up return\n");
    call_import("RunAppModalLoopForWindow", 1, w);
    print_field(w, 1);
    print_field(w, 2);
    /* With the only other field hidden too, typing goes nowhere. */
    gm_w32(cid + 4, 2);
    call_import("GetControlByID", 3, w, cid, out);
    call_import("HIViewSetVisible", 2, gm_r32(out), 0u);
    script("40 type Eve\n50 down return\n52 up return\n");
    call_import("RunAppModalLoopForWindow", 1, w);
    print_field(w, 1);
    print_field(w, 2);
    /* The first shown again: Tab, with no focus, goes to it. */
    gm_w32(cid + 4, 1);
    call_import("GetControlByID", 3, w, cid, out);
    call_import("HIViewSetVisible", 2, gm_r32(out), 1u);
    script("70 down tab\n72 up tab\n80 type Al\n90 down return\n92 up return\n");
    call_import("RunAppModalLoopForWindow", 1, w);
    print_field(w, 1);
}

/* Hiding the focused edit text moves the focus to the next visible one,
   or to none, so typing never changes a hidden field; with none, Tab
   finds a field shown again. */
TEST(dialogs_hiding_the_focused_field_moves_the_focus) {
    char bundle[1024], out[16384];
    write_bundle_with(bundle, sizeof bundle, two_fields_xib);
    int status = test_run_child(child_hide_focused_field, bundle, out, sizeof out);
    test_remove_tree(bundle);
    CHECK_EQ(status, 0);
    const char *a = strstr(out, "field 1: \"\"\nfield 2: \"Bob\"\n");
    CHECK(a != NULL);
    CHECK_CONTAINS(a + 1, "field 1: \"\"\nfield 2: \"Bob\"\n");
    CHECK_CONTAINS(out, "field 1: \"Al\"\n");
}
