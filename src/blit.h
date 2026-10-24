#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pixel rectangles and the copy/fill engine under CopyBits, PaintRect and
   DrawPicture. Everything here works on host pointers; qd.c builds the
   descriptors from guest PixMaps. Pixels are big-endian, as on the Mac:
   1/2/4/8-bit indexed (most significant bits first), 16-bit xRRRRRGGGGGBBBBB
   and 32-bit xRGB. */

typedef struct {
    int16_t top, left, bottom, right;
} qd_rect;

typedef struct {
    uint16_t r, g, b;
} qd_rgb;

/* A color table: entry i is the color of pixel value i. */
typedef struct {
    int n;
    qd_rgb c[256];
} qd_palette;

typedef struct {
    uint8_t *base;    /* the first byte of the row at bounds.top */
    uint32_t row_bytes;
    qd_rect bounds;   /* pixel (bounds.left, bounds.top) is the first pixel */
    int depth;        /* 1, 2, 4, 8, 16 or 32 */
    const qd_palette *pal; /* indexed depths only */
} qd_pixels;

#define QD_SRC_COPY 0

static inline int rect_w(qd_rect r) { return r.right - r.left; }
static inline int rect_h(qd_rect r) { return r.bottom - r.top; }
static inline bool rect_empty(qd_rect r) { return r.right <= r.left || r.bottom <= r.top; }
qd_rect rect_sect(qd_rect a, qd_rect b);

/* The standard Mac color table for 1, 2, 4 or 8 bits (clut IDs 1, 2, 4, 8). */
void qd_std_palette(int depth, qd_palette *out);

/* The pixel value for color c at the given depth (nearest palette entry for
   indexed depths). */
uint32_t qd_pixel_for(qd_rgb c, int depth, const qd_palette *pal);

/* Copies src_rect of src onto dst_rect of dst, scaling with nearest-neighbor
   sampling when the sizes differ. Only pixels inside clip and dst's bounds
   are written. 1-bit sources are colorized: 1 bits become fg, 0 bits bg.
   Writes a message and returns false for an unsupported mode or conversion. */
bool qd_blit(const qd_pixels *src, qd_rect src_rect, const qd_pixels *dst, qd_rect dst_rect,
             qd_rect clip, int mode, qd_rgb fg, qd_rgb bg, char *err, size_t errlen);

/* Fills r (clipped to clip and dst's bounds) with color c. */
void qd_fill(const qd_pixels *dst, qd_rect r, qd_rect clip, qd_rgb c);

/* The pattern transfer modes. A source mode (0 to 7) means its pattern mode. */
enum {
    QD_PAT_COPY = 8,
    QD_PAT_OR,
    QD_PAT_XOR,
    QD_PAT_BIC,
    QD_NOT_PAT_COPY,
    QD_NOT_PAT_OR,
    QD_NOT_PAT_XOR,
    QD_NOT_PAT_BIC,
};

/* Fills r (clipped to clip and dst's bounds) with the 8x8 pattern pat, in
   mode: where a pattern bit is 1, patCopy and patOr draw fg, patBic draws
   bg and patXor inverts the pixel; patCopy draws bg where it is 0, and the
   not modes swap the bits. The pattern is aligned to dst's bounds. False
   for a mode that isn't a pattern mode. */
bool qd_fill_pattern(const qd_pixels *dst, qd_rect r, qd_rect clip, const uint8_t pat[8], int mode,
                     qd_rgb fg, qd_rgb bg);

/* The same for a mask, rect_w(at) by rect_h(at) bytes, a row at a time:
   at is where it lies in dst, and a byte that isn't 0 is a 1 bit. For the
   source modes (srcCopy to notSrcBic), which draw as their pattern modes. */
bool qd_fill_mask(const qd_pixels *dst, qd_rect at, const uint8_t *mask, qd_rect clip, int mode,
                  qd_rgb fg, qd_rgb bg);

/* One pixel's value at (x, y), which must be inside p's bounds; storing one;
   and the color of a pixel value. */
uint32_t qd_get_pixel(const qd_pixels *p, int x, int y);
void qd_set_pixel(const qd_pixels *p, int x, int y, uint32_t v);
qd_rgb qd_color_of(uint32_t v, int depth, const qd_palette *pal);

/* Converts the whole of src to 8-bit RGBA rows. rgba holds width*height*4 bytes. */
void qd_to_rgba(const qd_pixels *src, uint8_t *rgba);
