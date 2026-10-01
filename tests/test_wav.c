#include "test.h"

#include <stdlib.h>
#include <unistd.h>

#include "util.h"
#include "wav.h"

static uint32_t le32_at(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

TEST(wav_writes_16_bit_stereo) {
    const char *t = getenv("TMPDIR");
    char path[1024];
    snprintf(path, sizeof path, "%s/loony-wav-XXXXXX", t && *t ? t : "/tmp");
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
    wav_file w;
    CHECK(wav_open(&w, path, 44100));
    int16_t s[6] = {1, -1, 0x1234, -0x1234, 32767, -32768};
    wav_write(&w, s, 2);
    wav_write(&w, s + 4, 1);
    CHECK(wav_close(&w));
    size_t len = 0;
    uint8_t *f = read_file(path, &len);
    unlink(path);
    CHECK(f != NULL);
    CHECK_EQ(len, 44 + 12);
    CHECK(memcmp(f, "RIFF", 4) == 0 && memcmp(f + 8, "WAVEfmt ", 8) == 0);
    CHECK_EQ(le32_at(f + 4), 36 + 12);
    CHECK_EQ(f[20] | f[21] << 8, 1); /* PCM */
    CHECK_EQ(f[22], 2);
    CHECK_EQ(le32_at(f + 24), 44100);
    CHECK_EQ(le32_at(f + 28), 44100 * 4);
    CHECK_EQ(f[34], 16);
    CHECK(memcmp(f + 36, "data", 4) == 0);
    CHECK_EQ(le32_at(f + 40), 12);
    CHECK_EQ(f[44], 1);
    CHECK_EQ(f[47], 0xFF);
    CHECK_EQ(f[48], 0x34);
    CHECK_EQ(f[49], 0x12);
    CHECK_EQ(f[54], 0x00);
    CHECK_EQ(f[55], 0x80);
    free(f);
}

TEST(wav_open_fails_for_an_unwritable_path) {
    wav_file w;
    CHECK(!wav_open(&w, "/nonexistent/loony.wav", 44100));
}
