#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Writes an 8-bit RGBA image (rows top to bottom, 4 bytes per pixel) as an
   uncompressed PNG. Returns false if the file can't be written. */
bool png_write_rgba(const char *path, const uint8_t *rgba, int width, int height);
