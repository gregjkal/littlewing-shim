#include "test.h"

#include <stdlib.h>

#include "rsrc.h"
#include "util.h"

static uint8_t *read_fork(size_t *len) {
    char path[1100];
    snprintf(path, sizeof path, "%s/..namedfork/rsrc", test_game_exe_path());
    return read_file(path, len);
}

TEST(rsrc_parses_the_real_resource_fork) {
    SKIP_UNLESS_GAME();
    size_t len;
    uint8_t *fork = read_fork(&len);
    CHECK(fork != NULL);
    CHECK_EQ(len, 3856669);
    char err[256] = "";
    CHECK(rsrc_open(fork, len, err, sizeof err));
    CHECK_EQ(rsrc_type_count(), 57);
    CHECK_EQ(rsrc_total(), 877);
    CHECK_EQ(rsrc_count(FOURCC('E', 'S', 'n', 'd')), 125);
    CHECK_EQ(rsrc_count(FOURCC('P', 'I', 'C', 'T')), 7);
    CHECK_EQ(rsrc_count(FOURCC('s', 'n', 'd', ' ')), 6);
    CHECK_EQ(rsrc_count(FOURCC('c', 'l', 'u', 't')), 3);
    CHECK_EQ(rsrc_count(FOURCC('D', 'L', 'O', 'G')), 6);
    CHECK_EQ(rsrc_count(FOURCC('A', 'L', 'R', 'T')), 23);
    CHECK_EQ(rsrc_count(FOURCC('W', 'I', 'N', 'D')), 3);
    CHECK_EQ(rsrc_count(FOURCC('M', 'E', 'N', 'U')), 6);

    rsrc_entry *visu = rsrc_find(FOURCC('V', 'i', 's', 'u'), 128);
    CHECK(visu != NULL);
    CHECK_EQ(visu->len, 36);
    CHECK_EQ(fnv1a32(rsrc_data(visu), visu->len), 0xC4405513u);
    rsrc_entry *pict = rsrc_find(FOURCC('P', 'I', 'C', 'T'), 128);
    CHECK(pict != NULL);
    CHECK_EQ(pict->len, 18464);
    CHECK_EQ(fnv1a32(rsrc_data(pict), pict->len), 0xE0CC01FFu);
    CHECK_EQ(rsrc_find(FOURCC('A', 'L', 'R', 'T'), 901)->attrs, 0x20);

    rsrc_entry *flip = rsrc_find_named(FOURCC('F', 'l', 'i', 'p'), "buttom left");
    CHECK(flip != NULL);
    CHECK_EQ(flip->id, 1000);
    CHECK_STR(flip->name, "Buttom Left");
    CHECK(rsrc_find(FOURCC('V', 'i', 's', 'u'), 129) == NULL);
    CHECK(rsrc_find(FOURCC('Z', 'Z', 'Z', 'Z'), 128) == NULL);
    CHECK(rsrc_find_named(FOURCC('F', 'l', 'i', 'p'), "Nope") == NULL);
    rsrc_close();
    free(fork);
}

/* Review Focus 4: a corrupt or truncated resource fork. */
TEST(rsrc_rejects_junk) {
    uint8_t junk[64];
    memset(junk, 0xFF, sizeof junk);
    char err[256] = "";
    CHECK(!rsrc_open(junk, sizeof junk, err, sizeof err));
    CHECK(err[0] != '\0');
    CHECK(!rsrc_open(junk, 3, err, sizeof err));
    CHECK_CONTAINS(err, "too short");
    CHECK_EQ(rsrc_total(), 0);
}

TEST(rsrc_rejects_truncated_fork) {
    SKIP_UNLESS_GAME();
    size_t len;
    uint8_t *fork = read_fork(&len);
    CHECK(fork != NULL);
    size_t cuts[] = {0, 15, 16, 4096, 3843455, 3843455 + 100, len - 1};
    for (size_t i = 0; i < sizeof cuts / sizeof cuts[0]; i++) {
        /* Copy so ASan catches any read past the cut. */
        uint8_t *part = malloc(cuts[i] ? cuts[i] : 1);
        memcpy(part, fork, cuts[i]);
        char err[256] = "";
        bool ok = rsrc_open(part, cuts[i], err, sizeof err);
        free(part);
        CHECK(!ok);
        CHECK(err[0] != '\0');
    }
    free(fork);
}

TEST(rsrc_rejects_corrupt_map_offsets) {
    SKIP_UNLESS_GAME();
    size_t len;
    uint8_t *fork = read_fork(&len);
    CHECK(fork != NULL);
    uint32_t map_off = rd_be32(fork + 4);
    char err[256] = "";
    /* Type list offset past the end of the map. */
    uint8_t *bad = malloc(len);
    memcpy(bad, fork, len);
    wr_be16(bad + map_off + 24, 0xFFF0);
    CHECK(!rsrc_open(bad, len, err, sizeof err));
    CHECK_CONTAINS(err, "out of range");
    /* A type claiming 65536 resources. */
    memcpy(bad, fork, len);
    uint32_t tl = rd_be16(bad + map_off + 24);
    wr_be16(bad + map_off + tl + 2 + 4, 0xFFFF);
    CHECK(!rsrc_open(bad, len, err, sizeof err));
    CHECK_CONTAINS(err, "truncated");
    free(bad);
    free(fork);
}
