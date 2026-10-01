#include "test.h"

#include <stdlib.h>
#include <unistd.h>

#include "png.h"
#include "util.h"

static char *tmp_path(char buf[1024]) {
    const char *t = getenv("TMPDIR");
    snprintf(buf, 1024, "%s/loony-png-XXXXXX", t && *t ? t : "/tmp");
    int fd = mkstemp(buf);
    if (fd >= 0)
        close(fd);
    return buf;
}

TEST(png_writes_a_valid_header_and_size) {
    uint8_t px[2 * 3 * 4];
    for (int i = 0; i < 24; i++)
        px[i] = (uint8_t)(i * 10);
    char path[1024];
    tmp_path(path);
    CHECK(png_write_rgba(path, px, 2, 3));
    size_t len;
    uint8_t *f = read_file(path, &len);
    unlink(path);
    CHECK(f != NULL);
    CHECK(memcmp(f, "\x89PNG\r\n\x1a\n", 8) == 0);
    CHECK(memcmp(f + 12, "IHDR", 4) == 0);
    CHECK_EQ(rd_be32(f + 16), 2);
    CHECK_EQ(rd_be32(f + 20), 3);
    /* signature 8 + IHDR 25 + IDAT (12 + 2 + 5 + 27 + 4) + IEND 12 */
    CHECK_EQ(len, 8 + 25 + 50 + 12);
    CHECK(memcmp(f + len - 8, "IEND", 4) == 0);
    /* the IEND CRC is fixed */
    CHECK_EQ(rd_be32(f + len - 4), 0xAE426082u);
    free(f);
}

TEST(png_splits_large_images_into_stored_blocks) {
    int w = 200, h = 100; /* 80,100 raw bytes: two stored blocks */
    uint8_t *px = calloc((size_t)w * h, 4);
    char path[1024];
    tmp_path(path);
    CHECK(png_write_rgba(path, px, w, h));
    size_t len;
    uint8_t *f = read_file(path, &len);
    unlink(path);
    free(px);
    CHECK(f != NULL);
    size_t raw = (size_t)(w * 4 + 1) * h;
    CHECK_EQ(len, 8 + 25 + 12 + (2 + raw + 5 * 2 + 4) + 12);
    free(f);
}
