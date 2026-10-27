#pragma once
/* A resource fork built in memory, for tests that need a few resources
   without a game. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

typedef struct {
    uint32_t type;
    int16_t id;
    const uint8_t *data;
    uint32_t len;
} res;

/* A resource fork holding rs, those of a type next to each other, as
   rsrc_open reads one. The caller keeps it until the test ends. */
static inline uint8_t *build_fork(const res *rs, int n, size_t *len) {
    int ntypes = 0;
    uint32_t data_len = 0;
    for (int i = 0; i < n; i++) {
        data_len += 4 + rs[i].len;
        ntypes += i == 0 || rs[i].type != rs[i - 1].type;
    }
    uint32_t type_list = 28, refs = type_list + 2 + 8u * (uint32_t)ntypes;
    uint32_t map_len = refs + 12u * (uint32_t)n;
    uint32_t data_off = 256, map_off = data_off + data_len;
    *len = map_off + map_len;
    uint8_t *f = calloc(1, *len);
    wr_be32(f, data_off);
    wr_be32(f + 4, map_off);
    wr_be32(f + 8, data_len);
    wr_be32(f + 12, map_len);
    uint8_t *m = f + map_off;
    wr_be16(m + 24, (uint16_t)type_list);
    wr_be16(m + 26, (uint16_t)map_len);
    wr_be16(m + type_list, (uint16_t)(ntypes - 1));
    uint32_t at = 0;
    int t = -1;
    for (int i = 0; i < n; i++) {
        uint8_t *r = m + refs + 12 * i;
        if (i == 0 || rs[i].type != rs[i - 1].type) {
            uint8_t *e = m + type_list + 2 + 8 * ++t;
            int count = 0;
            while (i + count < n && rs[i + count].type == rs[i].type)
                count++;
            wr_be32(e, rs[i].type);
            wr_be16(e + 4, (uint16_t)(count - 1));
            wr_be16(e + 6, (uint16_t)(refs + 12 * (uint32_t)i - type_list));
        }
        wr_be16(r, (uint16_t)rs[i].id);
        wr_be16(r + 2, 0xFFFF);
        wr_be32(r + 4, at);
        wr_be32(f + data_off + at, rs[i].len);
        memcpy(f + data_off + at + 4, rs[i].data, rs[i].len);
        at += 4 + rs[i].len;
    }
    return f;
}
