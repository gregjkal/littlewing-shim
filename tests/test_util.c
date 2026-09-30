#include "test.h"

#include <stdlib.h>
#include <unistd.h>

#include "util.h"

TEST(util_fnv1a32_known_values) {
    CHECK_EQ(fnv1a32("", 0), 0x811C9DC5u);
    CHECK_EQ(fnv1a32("a", 1), 0xE40C292Cu);
}

TEST(util_big_endian_round_trip) {
    uint8_t b[4];
    wr_be32(b, 0x11223344u);
    CHECK_EQ(b[0], 0x11);
    CHECK_EQ(b[3], 0x44);
    CHECK_EQ(rd_be32(b), 0x11223344u);
    wr_be16(b, 0xABCD);
    CHECK_EQ(b[0], 0xAB);
    CHECK_EQ(rd_be16(b), 0xABCD);
}

TEST(util_read_file_round_trip) {
    const char *tmp = getenv("TMPDIR");
    char path[1024];
    snprintf(path, sizeof path, "%s/loony-test-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(write(fd, "hello", 5) == 5);
    close(fd);
    size_t len = 0;
    uint8_t *data = read_file(path, &len);
    unlink(path);
    CHECK(data != NULL);
    CHECK_EQ(len, 5);
    CHECK(memcmp(data, "hello", 5) == 0);
    free(data);
}

TEST(util_read_file_missing_returns_null) {
    size_t len = 0;
    CHECK(read_file("/nonexistent/loony/file", &len) == NULL);
}

static void child_fatal(void *arg) {
    (void)arg;
    fatal("boom %d", 7);
}

TEST(util_fatal_exits_2_with_message) {
    char out[1024];
    int status = test_run_child(child_fatal, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "loony: fatal: boom 7");
}
