#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/* Uses stored (uncompressed) deflate blocks, so no zlib is needed. */

static uint32_t crc_table[256];

static void init_crc(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    c = ~c;
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

static bool chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    wr_be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    uint32_t c = crc(0, hdr + 4, 4);
    c = crc(c, data, len);
    uint8_t tail[4];
    wr_be32(tail, c);
    return fwrite(hdr, 1, 8, f) == 8 && fwrite(data, 1, len, f) == len &&
           fwrite(tail, 1, 4, f) == 4;
}

bool png_write_rgba(const char *path, const uint8_t *rgba, int width, int height) {
    if (!crc_table[1])
        init_crc();
    size_t row = (size_t)width * 4 + 1; /* filter byte + pixels */
    size_t raw_len = row * (size_t)height;
    size_t nblocks = raw_len / 65535 + 1;
    size_t z_len = 2 + raw_len + 5 * nblocks + 4;
    uint8_t *z = malloc(z_len);
    if (!z)
        return false;
    size_t o = 0;
    z[o++] = 0x78; /* zlib header: deflate, 32K window */
    z[o++] = 0x01;
    uint32_t a = 1, b = 0; /* Adler-32 */
    size_t done = 0, block_left = 0;
    for (int y = 0; y < height; y++) {
        for (size_t i = 0; i < row; i++) {
            if (block_left == 0) {
                size_t n = raw_len - done < 65535 ? raw_len - done : 65535;
                z[o++] = (uint8_t)(done + n == raw_len); /* BFINAL, stored */
                z[o++] = (uint8_t)n;
                z[o++] = (uint8_t)(n >> 8);
                z[o++] = (uint8_t)~n;
                z[o++] = (uint8_t)(~n >> 8);
                block_left = n;
            }
            uint8_t v = i == 0 ? 0 : rgba[(size_t)y * (size_t)width * 4 + i - 1];
            z[o++] = v;
            a = (a + v) % 65521;
            b = (b + a) % 65521;
            done++;
            block_left--;
        }
    }
    if (raw_len == 0) { /* an empty image still needs one final block */
        z[o++] = 1;
        z[o++] = 0;
        z[o++] = 0;
        z[o++] = 0xFF;
        z[o++] = 0xFF;
    }
    wr_be32(z + o, (b << 16) | a);
    o += 4;

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(z);
        return false;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    uint8_t ihdr[13];
    wr_be32(ihdr, (uint32_t)width);
    wr_be32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 6;  /* RGBA */
    ihdr[10] = 0; /* deflate */
    ihdr[11] = 0; /* adaptive filtering */
    ihdr[12] = 0; /* no interlace */
    bool ok = fwrite(sig, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) &&
              chunk(f, "IDAT", z, (uint32_t)o) && chunk(f, "IEND", NULL, 0);
    ok = (fclose(f) == 0) && ok;
    free(z);
    return ok;
}
