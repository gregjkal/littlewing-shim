#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The few CoreGraphics image calls MONSTER FAIR makes for its dialogs:
   a data provider for a file URL and a CGImage from a PNG, decoded with the
   host's ImageIO. Providers and images are opaque IDs in tag space
   (CGIMAGE_TAG_BASE and up), reference counted like Core Foundation objects. */

#define CGIMAGE_TAG_BASE 0x0C000000u
#define CGIMAGE_TYPE_ID        32u /* CFGetTypeID's, the shim's own numbers as in cf.h */
#define CGDATAPROVIDER_TYPE_ID 33u

typedef struct {
    int width, height;
    uint8_t *xrgb; /* width * height big-endian 32-bit xRGB pixels, flattened over white */
} cgimage_pixels;

/* Decodes a PNG file. False, with err saying why, if it can't. The caller
   frees out->xrgb. */
bool cgimage_decode_png(const char *path, cgimage_pixels *out, char *err, size_t errlen);

/* The same, flattened over the color (r, g, b) instead of white. */
bool cgimage_decode_png_over(const char *path, uint8_t r, uint8_t g, uint8_t b, cgimage_pixels *out,
                             char *err, size_t errlen);

/* Frees every provider and image. */
void cgimage_init(void);

/* The pixels of a live CGImage, or NULL if ref isn't one. */
const cgimage_pixels *cgimage_get(uint32_t ref);

/* What CFRetain and CFRelease do to a CGImage or data provider. */
void cgimage_retain(uint32_t ref);
void cgimage_release(uint32_t ref);

/* What CFGetTypeID says of ref: CGIMAGE_TYPE_ID, CGDATAPROVIDER_TYPE_ID, or
   0 if ref isn't a live image or provider. */
uint32_t cgimage_type_id(uint32_t ref);

/* Registers CGDataProviderCreateWithURL, CGDataProviderRelease,
   CGImageCreateWithPNGDataProvider and CGImageRelease. */
void cgimage_register(void);
