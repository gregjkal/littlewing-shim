#include "test.h"

#include <SDL3/SDL_scancode.h>

#include "script.h"

TEST(script_parses_and_orders_actions) {
    char err[256] = "";
    CHECK(script_parse("# a comment\n\n10 down z\n10 up z\n  20 screenshot /tmp/x.png\n30 quit\n",
                       err, sizeof err));
    CHECK_EQ(script_remaining(), 4);
    script_action a;
    CHECK(!script_next(9, &a));
    CHECK(script_next(10, &a));
    CHECK_EQ(a.kind, SCRIPT_KEY_DOWN);
    CHECK_EQ(a.scancode, SDL_SCANCODE_Z);
    CHECK(script_next(10, &a));
    CHECK_EQ(a.kind, SCRIPT_KEY_UP);
    CHECK(!script_next(19, &a));
    CHECK(script_next(25, &a));
    CHECK_EQ(a.kind, SCRIPT_SCREENSHOT);
    CHECK_STR(a.path, "/tmp/x.png");
    CHECK(script_next(1000, &a));
    CHECK_EQ(a.kind, SCRIPT_QUIT);
    CHECK(!script_next(1000, &a));
    CHECK_EQ(script_remaining(), 0);
}

TEST(script_reports_errors_with_line_numbers) {
    char err[256] = "";
    CHECK(!script_parse("1 down z\n2 down nokey\n", err, sizeof err));
    CHECK_CONTAINS(err, "line 2: unknown key \"nokey\"");
    CHECK(!script_parse("5 jump\n", err, sizeof err));
    CHECK_CONTAINS(err, "line 1: unknown action \"jump\"");
    CHECK(!script_parse("5 down z\n4 up z\n", err, sizeof err));
    CHECK_CONTAINS(err, "line 2: tick 4 is before tick 5");
    CHECK(!script_parse("5 screenshot\n", err, sizeof err));
    CHECK_CONTAINS(err, "screenshot needs a file name");
    CHECK(!script_parse("down z\n", err, sizeof err));
    CHECK_CONTAINS(err, "line 1: expected");
    CHECK_EQ(script_remaining(), 0);
}

TEST(script_load_missing_file) {
    char err[256] = "";
    CHECK(!script_load("/nonexistent/loony.script", err, sizeof err));
    CHECK_CONTAINS(err, "can't read /nonexistent/loony.script");
}

TEST(script_clicks_and_typing) {
    char err[256] = "";
    CHECK(script_parse("5 click 320 270\n6 type me@example.com  two words\n7 type\n", err, sizeof err));
    script_action a;
    CHECK(script_next(5, &a));
    CHECK_EQ(a.kind, SCRIPT_CLICK);
    CHECK_EQ(a.x, 320);
    CHECK_EQ(a.y, 270);
    CHECK(script_next(6, &a));
    CHECK_EQ(a.kind, SCRIPT_TYPE);
    CHECK_STR(a.path, "me@example.com  two words");
    CHECK(script_next(7, &a));
    CHECK_STR(a.path, "");
    CHECK(!script_parse("5 click 320\n", err, sizeof err));
    CHECK_CONTAINS(err, "line 1: click needs x and y");
}
