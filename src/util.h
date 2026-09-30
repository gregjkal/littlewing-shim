#pragma once
#include <stddef.h>
#include <stdint.h>

/* Prints "loony: fatal: <msg>" to stderr and exits with status 2. */
_Noreturn void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Prints "loony: <msg>" to stderr. */
void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Reads a whole file into a malloc'd buffer. Returns NULL (errno set) on failure. */
uint8_t *read_file(const char *path, size_t *len_out);

uint32_t fnv1a32(const void *data, size_t len);

static inline uint16_t rd_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline void wr_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void wr_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
