#include "test.h"

#include <stdlib.h>
#include <unistd.h>

static void run_loony(void *dir) {
    execl(LOONY_BIN, "loony", (const char *)dir, (char *)NULL);
    fprintf(stderr, "exec %s failed\n", LOONY_BIN);
    _exit(127);
}

TEST(run_stops_at_first_unimplemented_import) {
    SKIP_UNLESS_GAME();
    char out[32768];
    int status = test_run_child(run_loony, (void *)test_game_dir(), out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: loaded ");
    CHECK_CONTAINS(out, "132 imports");
    CHECK_CONTAINS(out, "loony: crash: unimplemented import ");
}

/* Review Focus 1: wrong or missing game folder. */
TEST(run_reports_missing_game_folder) {
    char out[4096];
    int status = test_run_child(run_loony, (void *)"/nonexistent/loony", out, sizeof out);
    CHECK_EQ(status, 1);
    CHECK_CONTAINS(out, "can't read /nonexistent/loony/LOONY LABYRINTH 3.0.1");
}
