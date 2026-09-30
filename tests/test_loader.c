#include "test.h"

#include <stdlib.h>

#include "guest_mem.h"
#include "loader.h"
#include "util.h"

TEST(loader_loads_the_real_executable) {
    SKIP_UNLESS_GAME();
    size_t len;
    uint8_t *buf = read_file(test_game_exe_path(), &len);
    CHECK(buf != NULL);
    gm_init();
    loaded_image img;
    char err[256] = "";
    CHECK(image_load(buf, len, &img, err, sizeof err));

    CHECK_EQ(img.code_base, 0x00100000u);
    CHECK_EQ(img.code_len, 280528);
    CHECK_EQ(gm_r32(img.code_base), 0x7C0802A6u);
    CHECK_EQ(fnv1a32(gm_ptr(img.code_base, img.code_len), img.code_len), 0xA50D49B8u);

    CHECK_EQ(img.data_base, 0x00145000u);
    CHECK_EQ(img.data_len, 22892);
    CHECK_EQ(fnv1a32(gm_ptr(img.data_base, img.data_len), img.data_len), 0xA7C47401u);

    CHECK_EQ(img.import_area, 0x0014A970u);
    CHECK_EQ(img.import_addr[0], 0x0014A970u);
    CHECK_EQ(gm_r32(img.import_addr[0]), GUEST_TRAP_ADDR(0));
    CHECK_EQ(gm_r32(img.import_addr[0] + 4), 0);
    CHECK_EQ(gm_r32(img.import_addr[131]), GUEST_TRAP_ADDR(131));
    CHECK_EQ(img.import_addr[36], 0x0014A970u + 36 * 8);
    CHECK_EQ(gm_r32(img.import_addr[36]), 0);

    CHECK_EQ(img.main_tvector, 0x001462E0u);
    CHECK_EQ(gm_r32(img.main_tvector), 0x001387E0u);
    CHECK_EQ(gm_r32(img.main_tvector + 4), 0x00145000u);
    CHECK_EQ(img.init_tvector, 0);

    image_free(&img);
    free(buf);
}

TEST(loader_reports_parse_errors) {
    uint8_t junk[64] = {0};
    gm_init();
    loaded_image img;
    char err[256] = "";
    CHECK(!image_load(junk, sizeof junk, &img, err, sizeof err));
    CHECK_CONTAINS(err, "not a PowerPC PEF file");
}
