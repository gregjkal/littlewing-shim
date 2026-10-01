#include "pict.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

typedef struct {
    const uint8_t *p, *end;
    int version;
    char *err;
    size_t errlen;
} reader;

static bool fail(reader *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static bool fail(reader *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->err, r->errlen, fmt, ap);
    va_end(ap);
    return false;
}

static bool need(reader *r, size_t n) {
    if (r->p > r->end || (size_t)(r->end - r->p) < n)
        return fail(r, "picture data is truncated");
    return true;
}

static bool u16(reader *r, uint16_t *v) {
    if (!need(r, 2))
        return false;
    *v = rd_be16(r->p);
    r->p += 2;
    return true;
}

static bool rect(reader *r, qd_rect *out) {
    if (!need(r, 8))
        return false;
    out->top = (int16_t)rd_be16(r->p);
    out->left = (int16_t)rd_be16(r->p + 2);
    out->bottom = (int16_t)rd_be16(r->p + 4);
    out->right = (int16_t)rd_be16(r->p + 6);
    r->p += 8;
    return true;
}

static bool skip(reader *r, size_t n) {
    if (!need(r, n))
        return false;
    r->p += n;
    return true;
}

bool pict_frame(const uint8_t *data, size_t len, qd_rect *frame) {
    if (len < 10)
        return false;
    frame->top = (int16_t)rd_be16(data + 2);
    frame->left = (int16_t)rd_be16(data + 4);
    frame->bottom = (int16_t)rd_be16(data + 6);
    frame->right = (int16_t)rd_be16(data + 8);
    return true;
}

static int16_t clamp16(int64_t v) {
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

/* Maps coordinate v from [f0, f0 + fn) onto [d0, d0 + dn), in 64 bits. */
static int16_t map(int v, int f0, int fn, int d0, int dn) {
    return clamp16(d0 + (int64_t)(v - f0) * dn / fn);
}

/* Maps a rectangle from picture-frame coordinates to destination coordinates. */
static qd_rect map_rect(qd_rect r, qd_rect frame, qd_rect dst) {
    int fw = rect_w(frame), fh = rect_h(frame), dw = rect_w(dst), dh = rect_h(dst);
    if (fw <= 0 || fh <= 0)
        return dst;
    return (qd_rect){
        map(r.top, frame.top, fh, dst.top, dh),
        map(r.left, frame.left, fw, dst.left, dw),
        map(r.bottom, frame.top, fh, dst.top, dh),
        map(r.right, frame.left, fw, dst.left, dw),
    };
}

/* Expands one PackBits row into out (exactly n bytes). */
static bool unpack_row(reader *r, size_t packed, uint8_t *out, size_t n) {
    if (!need(r, packed))
        return false;
    const uint8_t *p = r->p, *end = r->p + packed;
    size_t o = 0;
    while (p < end && o < n) {
        int8_t c = (int8_t)*p++;
        if (c >= 0) {
            size_t k = (size_t)c + 1;
            if (k > (size_t)(end - p) || k > n - o)
                return fail(r, "PackBits literal run overflows the row");
            memcpy(out + o, p, k);
            p += k;
            o += k;
        } else if (c != -128) {
            size_t k = (size_t)(1 - c);
            if (p >= end || k > n - o)
                return fail(r, "PackBits repeat run overflows the row");
            memset(out + o, *p++, k);
            o += k;
        }
    }
    if (o != n)
        return fail(r, "PackBits row has %zu of %zu bytes", o, n);
    r->p = end;
    return true;
}

static bool color_table(reader *r, qd_palette *pal) {
    if (!need(r, 8))
        return false;
    uint16_t flags = rd_be16(r->p + 4);
    uint32_t n = rd_be16(r->p + 6) + 1u;
    r->p += 8;
    if (n > 256)
        return fail(r, "color table has %u entries", n);
    if (!need(r, 8 * n))
        return false;
    memset(pal, 0, sizeof *pal);
    pal->n = 256;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = r->p + 8 * i;
        /* A device table (flag 0x8000) is indexed by position; otherwise
           each entry names its pixel value. */
        uint32_t v = (flags & 0x8000) ? i : (rd_be16(e) & 0xFFu);
        pal->c[v] = (qd_rgb){rd_be16(e + 2), rd_be16(e + 4), rd_be16(e + 6)};
    }
    r->p += 8 * n;
    return true;
}

static bool region(reader *r, qd_rect *bbox) {
    uint16_t size;
    if (!u16(r, &size))
        return false;
    if (size < 10)
        return fail(r, "region of %u bytes is too short", size);
    if (!rect(r, bbox) || !skip(r, size - 10u))
        return false;
    if (size != 10)
        return fail(r, "non-rectangular clip regions are not supported");
    return true;
}

/* BitsRect / PackBitsRect: a BitMap or PixMap, then rows of pixels. */
static bool bits_rect(reader *r, bool packed, qd_rect frame, qd_rect dst, const qd_pixels *target,
                      qd_rect clip, qd_rgb fg, qd_rgb bg) {
    uint16_t row_bytes;
    qd_rect bounds;
    if (!u16(r, &row_bytes) || !rect(r, &bounds))
        return false;
    bool is_pixmap = row_bytes & 0x8000;
    row_bytes &= 0x3FFF;
    int depth = 1;
    qd_palette pal;
    if (is_pixmap) {
        if (!need(r, 36))
            return false;
        uint16_t pack_type = rd_be16(r->p + 2);
        depth = rd_be16(r->p + 18);
        uint16_t cmp_count = rd_be16(r->p + 20);
        r->p += 36;
        if (cmp_count != 1 || (depth != 1 && depth != 2 && depth != 4 && depth != 8))
            return fail(r, "%d-bit pixmaps with %u components are not supported", depth, cmp_count);
        if (pack_type > 1)
            return fail(r, "pack type %u is not supported", pack_type);
        if (!color_table(r, &pal))
            return false;
    } else {
        qd_std_palette(1, &pal);
    }
    qd_rect src_rect, dst_rect;
    uint16_t mode;
    if (!rect(r, &src_rect) || !rect(r, &dst_rect) || !u16(r, &mode))
        return false;
    int h = rect_h(bounds);
    if (h < 0 || rect_w(bounds) < 0 || (size_t)row_bytes * 8 < (size_t)rect_w(bounds) * depth)
        return fail(r, "bitmap rows are too short for their bounds");
    uint8_t *pixels = malloc((size_t)row_bytes * (h ? h : 1));
    if (!pixels)
        return fail(r, "out of memory");
    for (int y = 0; y < h; y++) {
        uint8_t *row = pixels + (size_t)y * row_bytes;
        bool ok;
        if (!packed || row_bytes < 8) {
            ok = need(r, row_bytes);
            if (ok) {
                memcpy(row, r->p, row_bytes);
                r->p += row_bytes;
            }
        } else {
            size_t n = 0;
            if (row_bytes > 250) {
                uint16_t c;
                ok = u16(r, &c);
                n = c;
            } else {
                ok = need(r, 1);
                if (ok)
                    n = *r->p++;
            }
            ok = ok && unpack_row(r, n, row, row_bytes);
        }
        if (!ok) {
            free(pixels);
            return false;
        }
    }
    qd_pixels src = {pixels, row_bytes, bounds, depth, &pal};
    bool ok = qd_blit(&src, src_rect, target, map_rect(dst_rect, frame, dst), clip, mode, fg, bg,
                      r->err, r->errlen);
    free(pixels);
    return ok;
}

bool pict_draw(const uint8_t *data, size_t len, qd_rect dst, const qd_pixels *target, qd_rect clip,
               qd_rgb fg, qd_rgb bg, char *err, size_t errlen) {
    reader r = {data, data + len, 0, err, errlen};
    qd_rect frame;
    if (!pict_frame(data, len, &frame))
        return fail(&r, "picture data is truncated");
    r.p += 10;
    if (!need(&r, 2))
        return false;
    if (r.p[0] == 0x11 && r.p[1] == 0x01) {
        r.version = 1;
        r.p += 2;
    } else if (rd_be16(r.p) == 0x0011 && len >= 14 && rd_be16(r.p + 2) == 0x02FF) {
        r.version = 2;
        r.p += 4;
    } else {
        return fail(&r, "not a version 1 or 2 picture");
    }
    for (;;) {
        uint16_t op;
        if (r.version == 1) {
            if (!need(&r, 1))
                return false;
            op = *r.p++;
        } else {
            if (((r.p - data) & 1) && !skip(&r, 1))
                return false;
            if (!u16(&r, &op))
                return false;
        }
        qd_rect bbox;
        switch (op) {
        case 0x00: /* NOP */
            break;
        case 0x01: /* clip region */
            if (!region(&r, &bbox))
                return false;
            break;
        case 0x1E: /* DefHilite */
            break;
        case 0x0C00: /* header */
            if (!skip(&r, 24))
                return false;
            break;
        case 0x90: /* BitsRect */
        case 0x98: /* PackBitsRect */
            if (!bits_rect(&r, op == 0x98, frame, dst, target, clip, fg, bg))
                return false;
            break;
        case 0xA0: /* short comment */
            if (!skip(&r, 2))
                return false;
            break;
        case 0xA1: { /* long comment */
            uint16_t kind, n;
            if (!u16(&r, &kind) || !u16(&r, &n) || !skip(&r, n))
                return false;
            break;
        }
        case 0xFF: /* end of picture */
            return true;
        default:
            return fail(&r, "picture opcode 0x%04x is not supported", op);
        }
    }
}
