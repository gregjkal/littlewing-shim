#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "blit.h"

/* QuickDraw regions. A RgnHandle is a handle in the guest heap holding a
   region in QuickDraw's packed format: rgnSize (int16, bytes, the header
   included), rgnBBox (a Rect), and, unless the region is a rectangle (or
   empty) and rgnSize is 10, scan lines of inversion points. Each scan line
   is a y, then the x's (ascending) where a point's insideness flips from
   the line above, then 0x7FFF; a last 0x7FFF ends the region. A point is
   inside when an odd number of inversion points lie above and to the left
   of it, on or before its own row and column.

   On the host a region is a list of bands: rows top..bottom-1 that share
   one list of x intervals [x0, x1) [x2, x3) .... Every operation leaves a
   region canonical (no empty band, no two touching bands alike, no empty or
   touching interval), so equal regions have equal bands and the same
   packed bytes, and rgn_write of what rgn_read read gives back the bytes a
   QuickDraw region of that shape has. Coordinates stay in -32768..32766
   (a wide-open rectangle's 32767 becomes 32766): 0x7FFF is the format's
   end mark. */

typedef struct {
    int16_t top, bottom; /* rows top..bottom-1 */
    int first, n;        /* n x's (even) at xs[first] */
} rgn_band;

typedef struct {
    rgn_band *bands;
    int16_t *xs;
    int nbands, nxs, cap_bands, cap_xs;
} rgn;

#define RGN_HEADER 10     /* rgnSize and rgnBBox */
#define RGN_MAX_SIZE 32767 /* rgnSize is an int16 */

void rgn_init(rgn *r);
void rgn_free(rgn *r);

/* r becomes the rectangle (empty if the rectangle is). */
void rgn_set_rect(rgn *r, qd_rect rect);
void rgn_copy(const rgn *from, rgn *to);

bool rgn_is_empty(const rgn *r);
/* One band with one interval. */
bool rgn_is_rect(const rgn *r);
/* The bounding box; (0, 0, 0, 0) for an empty region. */
qd_rect rgn_bounds(const rgn *r);
bool rgn_equal(const rgn *a, const rgn *b);
bool rgn_has_point(const rgn *r, int h, int v);
/* True if any of rect's pixels is in r. */
bool rgn_meets_rect(const rgn *r, qd_rect rect);

typedef enum { RGN_SECT, RGN_UNION, RGN_DIFF, RGN_XOR } rgn_op;

/* out = a op b. out may be a or b. */
void rgn_combine(const rgn *a, const rgn *b, rgn_op op, rgn *out);

void rgn_offset(rgn *r, int dh, int dv);

/* InsetRgn: each edge moves in by dh (left and right) and dv (top and
   bottom), out for a negative one, as InsetRect does to a rectangle. A
   point stays when the (2dh + 1) by (2dv + 1) box around it is inside. */
void rgn_inset(rgn *r, int dh, int dv);

/* Calls fn with each rectangle of r, top to bottom, left to right. They
   don't overlap, and together they are r. */
void rgn_each_rect(const rgn *r, void (*fn)(qd_rect rect, void *ctx), void *ctx);

/* Reads the region in handle h. False, with err written, if h's bytes
   aren't a region (a size that doesn't fit, rows or x's out of order, an
   odd row, no end mark). */
bool rgn_read(uint32_t h, rgn *out, char *err, size_t errlen);

/* Writes r into handle h, resizing it. False if the packed region would be
   bigger than RGN_MAX_SIZE or the heap is full; h is unchanged then. */
bool rgn_write(uint32_t h, const rgn *r);

/* A new RgnHandle holding rect, or 0 if the heap is full. */
uint32_t rgn_new_handle(qd_rect rect);

/* Reads handle h as call's region, crashing with call's name if it isn't
   one. The caller frees out. */
void rgn_read_for(const char *call, uint32_t h, rgn *out);

/* Writes r into handle h for call, crashing if it can't. */
void rgn_write_for(const char *call, uint32_t h, const rgn *r);

/* Registers the region calls: NewRgn, DisposeRgn, RectRgn, SetRectRgn,
   SetEmptyRgn, CopyRgn, EmptyRgn, EqualRgn, OffsetRgn, InsetRgn, SectRgn,
   UnionRgn, DiffRgn, XorRgn, PtInRgn, RectInRgn and GetRegionBounds.
   Requires trap_init(). */
void region_register(void);
