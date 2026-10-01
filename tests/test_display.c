#include "test.h"

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
