#include "test.h"

#include <stdlib.h>

#include "dialogs.h"
#include "harness.h"
#include "rsrc.h"

static const char *const names[] = {"Alert", "StopAlert", "ParamText"};
static uint8_t *fork_buf;

static bool setup(void) {
    if (!test_game_present())
        return false;
    harness_init(names, 3);
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

TEST(dialogs_alert_text_lists_buttons_and_text) {
    SKIP_UNLESS_GAME();
    CHECK(setup());
    char text[1024];
    dialogs_alert_text(901, text, sizeof text);
    CHECK_CONTAINS(text, "Play Demo | Quit | Buy Now | Enter Key-Code | Thank you for trying");
    dialogs_alert_text(4242, text, sizeof text);
    CHECK_STR(text, "");
}

static void child_alert(void *unused) {
    (void)unused;
    if (!setup())
        exit(3);
    if (call_import("Alert", 2, 901u, 0u) != 1)
        exit(4);
    if (call_import("StopAlert", 2, 900u, 0u) != 1)
        exit(5);
}

TEST(dialogs_alert_answers_the_default_item_and_logs) {
    SKIP_UNLESS_GAME();
    char out[16384];
    CHECK_EQ(test_run_child(child_alert, NULL, out, sizeof out), 0);
    CHECK_CONTAINS(out, "loony: Alert 901 (answering item 1): Play Demo");
    CHECK_CONTAINS(out, "loony: Alert 900 (answering item 1): OK");
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
