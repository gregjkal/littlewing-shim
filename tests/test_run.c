#include "test.h"

#include <stdlib.h>
#include <unistd.h>

static void run_loony(void *dir) {
    execl(LOONY_BIN, "loony", (const char *)dir, (char *)NULL);
    fprintf(stderr, "exec %s failed\n", LOONY_BIN);
    _exit(127);
}

TEST(run_reaches_the_shareware_alert) {
    SKIP_UNLESS_GAME();
    char out[32768];
    int status = test_run_child(run_loony, (void *)test_game_dir(), out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: loaded ");
    CHECK_CONTAINS(out, "132 imports");
    CHECK_CONTAINS(out, "loony: crash: unimplemented import Alert");
    /* Alert 901 is the shareware dialog. Everything before it is implemented. */
    CHECK_CONTAINS(out, "Alert(0x00000385, ");
    CHECK(!strstr(out, "unknown selector"));
}

/* Review Focus 1: wrong or missing game folder. */
TEST(run_reports_missing_game_folder) {
    char out[4096];
    int status = test_run_child(run_loony, (void *)"/nonexistent/loony", out, sizeof out);
    CHECK_EQ(status, 1);
    CHECK_CONTAINS(out, "can't read /nonexistent/loony/LOONY LABYRINTH 3.0.1");
}
