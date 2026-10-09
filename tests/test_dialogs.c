#include "test.h"

#include <stdlib.h>
#include <sys/stat.h>

#include "asm.h"
#include "cf.h"
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

/* ---- nib windows and standard alerts (a Mac OS X game's) ---- */

static const char *const nib_names[] = {
    "CreateNibReference", "CreateWindowFromNib", "DisposeNibReference", "GetWindowEventTarget",
    "InstallEventHandler", "ShowWindow", "DisposeWindow", "RunAppModalLoopForWindow",
    "QuitAppModalLoopForWindow", "GetEventParameter", "GetControlByID", "GetControlData",
    "HIViewGetRoot", "HIViewFindByID", "HIViewSetVisible", "CreateStandardAlert", "RunStandardAlert",
    "Alert",
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

/* A bundle in a new temporary folder whose main.nib holds ask_xib. */
static void write_bundle(char *dir, size_t cap) {
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
    fputs(ask_xib, f);
    fclose(f);
}

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

/* Opens the Ask window, with the command handler installed on it, and
   shows it. */
static uint32_t open_ask_window(void) {
    emit_command_handler();
    uint32_t out = scratch(4);
    if (call_import("CreateNibReference", 2, cf_string("main"), out) != 0)
        fatal("CreateNibReference failed");
    uint32_t nib = gm_r32(out);
    if (call_import("CreateWindowFromNib", 3, nib, cf_string("Ask"), out) != 0)
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

/* MONSTER FAIR calls Alert(136) as it quits after rejecting a license, but
   has no resources: as on Mac OS, Alert returns -1 and shows nothing. */
static void child_alert_without_resources(void *bundle) {
    nib_setup(bundle);
    fprintf(stderr, "Alert returned %d\n", (int32_t)call_import("Alert", 2, 136u, 0u));
}

TEST(dialogs_alert_without_resources_returns_minus_one) {
    char bundle[1024], out[16384];
    write_bundle(bundle, sizeof bundle);
    int status = test_run_child(child_alert_without_resources, bundle, out, sizeof out);
    test_remove_tree(bundle);
    CHECK_EQ(status, 0);
    CHECK_CONTAINS(out, "Alert 136: the game has no ALRT 136; returning -1, as Mac OS does");
    CHECK_CONTAINS(out, "Alert returned -1");
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
