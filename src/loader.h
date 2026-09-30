#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pef.h"

typedef struct {
    pef_file pef;
    uint32_t code_base, code_len;
    uint32_t data_base, data_len;
    uint32_t import_area;   /* 8 bytes per import */
    uint32_t *import_addr;  /* what each import resolved to; pef.nimports entries */
    uint32_t main_tvector;  /* 0 if none */
    uint32_t init_tvector;  /* 0 if none */
} loaded_image;

/* Loads a PEF executable into guest memory: places sections, unpacks data,
   binds imports to trap addresses, and relocates. Requires gm_init(). buf
   must outlive img. On failure, writes err and leaves nothing to free. */
bool image_load(const uint8_t *buf, size_t len, loaded_image *img, char *err, size_t errlen);
void image_free(loaded_image *img);
