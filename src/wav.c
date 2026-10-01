#include "wav.h"

#include <string.h>

#define HEADER_BYTES 44

static void le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *p, uint32_t v) {
    le16(p, (uint16_t)v);
    le16(p + 2, (uint16_t)(v >> 16));
}

static void header(uint8_t *h, uint32_t rate, uint32_t frames) {
    uint32_t data = frames * 4;
    memcpy(h, "RIFF", 4);
    le32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16);
    le16(h + 20, 1); /* PCM */
    le16(h + 22, 2);
    le32(h + 24, rate);
    le32(h + 28, rate * 4);
    le16(h + 32, 4);
    le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    le32(h + 40, data);
}

bool wav_open(wav_file *w, const char *path, uint32_t rate) {
    memset(w, 0, sizeof *w);
    w->f = fopen(path, "wb");
    if (!w->f)
        return false;
    uint8_t h[HEADER_BYTES];
    header(h, rate, 0);
    w->rate = rate;
    w->failed = fwrite(h, 1, sizeof h, w->f) != sizeof h;
    return true;
}

void wav_write(wav_file *w, const int16_t *samples, uint32_t frames) {
    if (!w->f || w->failed)
        return;
    uint8_t buf[4096];
    while (frames > 0) {
        uint32_t n = frames < sizeof buf / 4 ? frames : sizeof buf / 4;
        for (uint32_t i = 0; i < 2 * n; i++)
            le16(buf + 2 * i, (uint16_t)samples[i]);
        if (fwrite(buf, 4, n, w->f) != n) {
            w->failed = true;
            return;
        }
        w->frames += n;
        samples += 2 * n;
        frames -= n;
    }
}

bool wav_close(wav_file *w) {
    if (!w->f)
        return false;
    uint8_t h[HEADER_BYTES];
    header(h, w->rate, w->frames);
    bool ok = !w->failed && fseek(w->f, 0, SEEK_SET) == 0 && fwrite(h, 1, sizeof h, w->f) == sizeof h;
    ok = fclose(w->f) == 0 && ok;
    w->f = NULL;
    return ok;
}
