#include "test.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <unistd.h>

#include "display.h"
#include "guest_mem.h"
#include "memmgr.h"
#include "qd.h"
#include "util.h"

TEST(display_writes_the_screen_as_png) {
    gm_init();
    mm_init();
    qd_init(800, 600, 8);
    const char *t = getenv("TMPDIR");
    char path[1024];
    snprintf(path, sizeof path, "%s/loony-screen-XXXXXX", t && *t ? t : "/tmp");
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
    CHECK(display_write_png(path));
    size_t len;
    uint8_t *f = read_file(path, &len);
    unlink(path);
    CHECK(f != NULL);
    CHECK_EQ(rd_be32(f + 16), 800);
    CHECK_EQ(rd_be32(f + 20), 600);
    free(f);
}

TEST(display_present_works_with_the_dummy_driver) {
    gm_init();
    mm_init();
    qd_init(64, 48, 16);
    unsigned before = display_frames();
    display_present();          /* always presents */
    display_present_if_dirty(); /* qd_init left the screen dirty: presents */
    display_present_if_dirty(); /* nothing new: doesn't */
    CHECK_EQ(display_frames(), before + 2);
}

static int keys, quits, last_scancode;
static bool last_down;
static void on_key(int sc, bool down, bool repeat) {
    (void)repeat;
    keys++;
    last_scancode = sc;
    last_down = down;
}
static void on_focus(bool active) { (void)active; }
static void on_quit(void) { quits++; }

static void push_key(SDL_Scancode sc, bool down, SDL_Keymod mod) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = sc;
    e.key.down = down;
    e.key.mod = mod;
    SDL_PushEvent(&e);
}

TEST(display_poll_forwards_keys_but_keeps_cmd_q_and_cmd_f) {
    gm_init();
    mm_init();
    qd_init(64, 48, 16);
    display_present(); /* opens the (dummy) window */
    static const display_input in = {on_key, on_focus, on_quit};
    display_set_input(&in);
    keys = quits = 0;
    push_key(SDL_SCANCODE_Z, true, SDL_KMOD_NONE);
    push_key(SDL_SCANCODE_Z, false, SDL_KMOD_NONE);
    display_poll();
    CHECK_EQ(keys, 2);
    CHECK_EQ(last_scancode, SDL_SCANCODE_Z);
    CHECK(!last_down);
    push_key(SDL_SCANCODE_Q, true, SDL_KMOD_LGUI);
    push_key(SDL_SCANCODE_F, true, SDL_KMOD_LGUI);
    push_key(SDL_SCANCODE_F, false, SDL_KMOD_LGUI);
    display_poll();
    CHECK_EQ(keys, 2);
    CHECK_EQ(quits, 1);
    SDL_Event q;
    memset(&q, 0, sizeof q);
    q.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&q);
    display_poll();
    CHECK_EQ(quits, 2);
}
