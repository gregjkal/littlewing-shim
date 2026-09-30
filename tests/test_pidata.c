#include "test.h"

#include <stdlib.h>

#include "pef.h"
#include "util.h"

static bool unpack(const uint8_t *src, size_t n, uint8_t *dst, size_t dn, char *err) {
    memset(dst, 0xAA, dn);
    return pef_unpack_pattern(src, n, dst, dn, err, 256);
}

TEST(pidata_zero) {
    uint8_t src[] = {0x03};
    uint8_t dst[3];
    char err[256];
    CHECK(unpack(src, sizeof src, dst, sizeof dst, err));
    CHECK_EQ(dst[0], 0);
    CHECK_EQ(dst[2], 0);
}

TEST(pidata_block_copy) {
    uint8_t src[] = {0x23, 1, 2, 3};
    uint8_t dst[3];
    char err[256];
    CHECK(unpack(src, sizeof src, dst, sizeof dst, err));
    CHECK_EQ(dst[0], 1);
    CHECK_EQ(dst[2], 3);
}

TEST(pidata_count_from_argument) {
    uint8_t src[3 + 128];
    src[0] = 0x20; /* block copy, count in argument */
    src[1] = 0x81; /* argument 128 = 0b1_0000000 */
    src[2] = 0x00;
    for (int i = 0; i < 128; i++)
        src[3 + i] = (uint8_t)i;
    uint8_t dst[128];
    char err[256];
    CHECK(unpack(src, sizeof src, dst, sizeof dst, err));
    CHECK_EQ(dst[0], 0);
    CHECK_EQ(dst[127], 127);
}

TEST(pidata_interleave_with_zero) {
    /* common 2, custom 1, repeat 2, customs 'A' 'B' -> 00 00 A 00 00 B 00 00 */
    uint8_t src[] = {0x82, 0x01, 0x02, 'A', 'B'};
    uint8_t dst[8];
    uint8_t want[8] = {0, 0, 'A', 0, 0, 'B', 0, 0};
    char err[256];
    CHECK(unpack(src, sizeof src, dst, sizeof dst, err));
    CHECK(memcmp(dst, want, 8) == 0);
}

TEST(pidata_sequence_of_instructions) {
    uint8_t src[] = {0x21, 9, 0x02, 0x21, 7};
    uint8_t dst[4];
    uint8_t want[4] = {9, 0, 0, 7};
    char err[256];
    CHECK(unpack(src, sizeof src, dst, sizeof dst, err));
    CHECK(memcmp(dst, want, 4) == 0);
}

TEST(pidata_rejects_overflow) {
    uint8_t src[] = {0x05};
    uint8_t dst[4];
    char err[256];
    CHECK(!unpack(src, sizeof src, dst, sizeof dst, err));
    CHECK_CONTAINS(err, "overflows");
}

TEST(pidata_rejects_short_output) {
    uint8_t src[] = {0x02};
    uint8_t dst[4];
    char err[256];
    CHECK(!unpack(src, sizeof src, dst, sizeof dst, err));
    CHECK_CONTAINS(err, "produced 2 of 4");
}

TEST(pidata_rejects_unsupported_opcode) {
    uint8_t src[] = {0x41, 0x01, 0x00};
    uint8_t dst[4];
    char err[256];
    CHECK(!unpack(src, sizeof src, dst, sizeof dst, err));
    CHECK_CONTAINS(err, "unsupported pattern opcode 2");
}

TEST(pidata_rejects_truncated_input) {
    uint8_t src[] = {0x23, 1};
    uint8_t dst[3];
    char err[256];
    CHECK(!unpack(src, sizeof src, dst, sizeof dst, err));
    CHECK_CONTAINS(err, "truncated");
}

TEST(pidata_unpacks_the_real_data_section) {
    SKIP_UNLESS_GAME();
    size_t len;
    uint8_t *buf = read_file(test_game_exe_path(), &len);
    CHECK(buf != NULL);
    pef_file pef;
    char err[256] = "";
    CHECK(pef_parse(buf, len, &pef, err, sizeof err));
    const pef_section *s = &pef.sections[1];
    uint8_t *dst = malloc(s->unpacked_len);
    bool ok = pef_unpack_pattern(buf + s->container_off, s->container_len, dst, s->unpacked_len,
                                 err, sizeof err);
    uint32_t h = fnv1a32(dst, s->unpacked_len);
    free(dst);
    pef_free(&pef);
    free(buf);
    CHECK(ok);
    CHECK_EQ(h, 0xA2C244EDu);
}
