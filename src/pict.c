#include "pict.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define DITHER_COPY 64 /* srcCopy + ditherCopy */

typedef struct {
    const uint8_t *p, *end;
    int version;
    char *err;
    size_t errlen;
    uint8_t *matte; /* from a QuickTime opcode, for the next DirectBitsRect: 1 = opaque */
    int matte_w, matte_h;
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

/* Decodes 8-bit QuickTime Animation ('rle ') data into a w x h image of
   gray levels (0 white, 255 black). Each line starts with a skip byte; then
   codes: 0 another skip, -1 end of line, n > 0 n groups of 4 literal
   pixels, n < 0 one group of 4 repeated -n times. */
static bool qt_rle8(reader *r, const uint8_t *p, size_t n, uint8_t *out, int w, int h) {
    const uint8_t *end = p + n;
    if (n < 6)
        return fail(r, "QuickTime matte is truncated");
    p += 4; /* chunk size */
    uint16_t header = rd_be16(p);
    p += 2;
    int line = 0, lines = h;
    if (header & 0x0008) {
        if (end - p < 8)
            return fail(r, "QuickTime matte is truncated");
        line = rd_be16(p);
        lines = rd_be16(p + 4);
        p += 8;
    }
    for (; lines > 0 && line < h; lines--, line++) {
        if (p >= end)
            return fail(r, "QuickTime matte is truncated");
        int x = 4 * (*p++ - 1);
        for (;;) {
            if (p >= end)
                return fail(r, "QuickTime matte is truncated");
            int8_t code = (int8_t)*p++;
            if (code == -1)
                break;
            if (code == 0) {
                if (p >= end)
                    return fail(r, "QuickTime matte is truncated");
                x += 4 * (*p++ - 1);
            } else if (code < 0) {
                if (end - p < 4)
                    return fail(r, "QuickTime matte is truncated");
                for (int k = 0; k < -code; k++, x += 4)
                    for (int i = 0; i < 4; i++)
                        if (x + i >= 0 && x + i < w)
                            out[line * w + x + i] = p[i];
                p += 4;
            } else {
                if (end - p < 4 * code)
                    return fail(r, "QuickTime matte is truncated");
                for (int i = 0; i < 4 * code; i++, x++)
                    if (x >= 0 && x < w)
                        out[line * w + x] = p[i];
                p += 4 * code;
            }
        }
    }
    return true;
}

/* UncompressedQuickTime (0x8201): version, matrix, then a matte (an image
   description and its data), which becomes r->matte. The image itself
   follows as ordinary opcodes. Only 8-bit gray 'rle ' mattes are decoded;
   other mattes are ignored and the image is drawn whole. */
static bool quicktime(reader *r) {
    if (!need(r, 4))
        return false;
    uint32_t n = rd_be32(r->p);
    r->p += 4;
    if (!need(r, n))
        return false;
    const uint8_t *q = r->p, *end = r->p + n;
    r->p = end;
    free(r->matte);
    r->matte = NULL;
    if (n < 2 + 36 + 4 + 8)
        return true;
    uint32_t matte_size = rd_be32(q + 38);
    q += 50; /* version, matrix, matte size, matte rect */
    if (matte_size < 86 || (size_t)(end - q) < matte_size)
        return true;
    uint32_t id_size = rd_be32(q), data_size = rd_be32(q + 44);
    int w = rd_be16(q + 32), h = rd_be16(q + 34), depth = rd_be16(q + 82);
    if (rd_be32(q + 4) != FOURCC('r', 'l', 'e', ' ') || depth != 40 || id_size < 86 ||
        id_size + data_size > matte_size || w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return true;
    uint8_t *gray = calloc((size_t)w * (size_t)h, 1);
    if (!gray)
        return fail(r, "out of memory");
    if (!qt_rle8(r, q + id_size, data_size, gray, w, h)) {
        free(gray);
        return false;
    }
    for (int i = 0; i < w * h; i++)
        gray[i] = gray[i] >= 128;
    r->matte = gray;
    r->matte_w = w;
    r->matte_h = h;
    return true;
}

/* Reads one packed row of component planes (pack type 4) into a 32-bit
   xRGB row: the planes come one after another, alpha first if there are 4. */
static bool unpack_planes(reader *r, size_t packed, int ncmp, uint8_t *row, int w) {
    uint8_t planes[4 * 4096];
    if ((size_t)ncmp * (size_t)w > sizeof planes)
        return fail(r, "direct pixmap row of %d pixels is too wide", w);
    if (!unpack_row(r, packed, planes, (size_t)ncmp * (size_t)w))
        return false;
    const uint8_t *rgb = planes + (size_t)(ncmp - 3) * (size_t)w;
    for (int x = 0; x < w; x++) {
        row[4 * x] = 0;
        row[4 * x + 1] = rgb[x];
        row[4 * x + 2] = rgb[w + x];
        row[4 * x + 3] = rgb[2 * w + x];
    }
    return true;
}

/* DirectBitsRect: a base address, a PixMap without a color table, then rows
   of 32-bit pixels, unpacked (pack type 0 or 1), without their pad byte
   (2), or as PackBits component planes (4). */
static bool direct_bits_rect(reader *r, qd_rect frame, qd_rect dst, const qd_pixels *target,
                             qd_rect clip, qd_rgb fg, qd_rgb bg) {
    uint16_t row_bytes;
    qd_rect bounds;
    if (!skip(r, 4) || !u16(r, &row_bytes) || !rect(r, &bounds) || !need(r, 36))
        return false;
    row_bytes &= 0x3FFF;
    uint16_t pack_type = rd_be16(r->p + 2);
    int depth = rd_be16(r->p + 18);
    int ncmp = rd_be16(r->p + 20);
    r->p += 36;
    if (depth != 32 || (ncmp != 3 && ncmp != 4) || pack_type == 3 || pack_type > 4)
        return fail(r, "%d-bit direct pixmaps with %d components and pack type %u are not supported",
                    depth, ncmp, pack_type);
    qd_rect src_rect, dst_rect;
    uint16_t mode;
    if (!rect(r, &src_rect) || !rect(r, &dst_rect) || !u16(r, &mode))
        return false;
    int w = rect_w(bounds), h = rect_h(bounds);
    if (w < 0 || h < 0 || (size_t)row_bytes < (size_t)w * 4)
        return fail(r, "pixmap rows are too short for their bounds");
    uint8_t *pixels = malloc((size_t)w * 4 * (h ? h : 1) + 4);
    if (!pixels)
        return fail(r, "out of memory");
    bool ok = true;
    for (int y = 0; y < h && ok; y++) {
        uint8_t *row = pixels + (size_t)y * (size_t)w * 4;
        if (pack_type == 2) {
            ok = need(r, (size_t)w * 3);
            for (int x = 0; ok && x < w; x++) {
                row[4 * x] = 0;
                memcpy(row + 4 * x + 1, r->p + 3 * x, 3);
            }
            if (ok)
                r->p += (size_t)w * 3;
        } else if (pack_type == 4 && row_bytes >= 8) {
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
            ok = ok && unpack_planes(r, n, ncmp, row, w);
        } else {
            ok = need(r, row_bytes);
            if (ok) {
                memcpy(row, r->p, (size_t)w * 4);
                r->p += row_bytes;
            }
        }
    }
    qd_pixels src = {pixels, (uint32_t)w * 4, bounds, 32, NULL};
    int m = mode == DITHER_COPY ? QD_SRC_COPY : mode; /* drawn without dithering */
    qd_rect to = map_rect(dst_rect, frame, dst);
    if (ok && r->matte && r->matte_w == w && r->matte_h == h && rect_w(src_rect) > 0 &&
        rect_h(src_rect) > 0) {
        /* Only the matte's opaque runs, each mapped like the whole. */
        for (int y = src_rect.top; ok && y < src_rect.bottom; y++)
            for (int x = src_rect.left; ok && x < src_rect.right;) {
                int mx = x - bounds.left, my = y - bounds.top;
                if (mx < 0 || mx >= w || my < 0 || my >= h || !r->matte[my * w + mx]) {
                    x++;
                    continue;
                }
                int x1 = x;
                while (x1 < src_rect.right && x1 - bounds.left < w && r->matte[my * w + x1 - bounds.left])
                    x1++;
                qd_rect run = {(int16_t)y, (int16_t)x, (int16_t)(y + 1), (int16_t)x1};
                ok = qd_blit(&src, run, target, map_rect(run, src_rect, to), clip, m, fg, bg, r->err,
                             r->errlen);
                x = x1;
            }
    } else if (ok) {
        ok = qd_blit(&src, src_rect, target, to, clip, m, fg, bg, r->err, r->errlen);
    }
    free(r->matte);
    r->matte = NULL;
    free(pixels);
    return ok;
}

/* Runs the opcodes after the version. */
static bool draw_ops(reader *r, const uint8_t *data, qd_rect frame, qd_rect dst,
                     const qd_pixels *target, qd_rect clip, qd_rgb fg, qd_rgb bg) {
    for (;;) {
        uint16_t op;
        if (r->version == 1) {
            if (!need(r, 1))
                return false;
            op = *r->p++;
        } else {
            if (((r->p - data) & 1) && !skip(r, 1))
                return false;
            if (!u16(r, &op))
                return false;
        }
        qd_rect bbox;
        switch (op) {
        case 0x00: /* NOP */
            break;
        case 0x01: /* clip region */
            if (!region(r, &bbox))
                return false;
            break;
        case 0x1E: /* DefHilite */
            break;
        case 0x0C00: /* header */
            if (!skip(r, 24))
                return false;
            break;
        case 0x90: /* BitsRect */
        case 0x98: /* PackBitsRect */
            if (!bits_rect(r, op == 0x98, frame, dst, target, clip, fg, bg))
                return false;
            break;
        case 0x9A: /* DirectBitsRect */
            if (!direct_bits_rect(r, frame, dst, target, clip, fg, bg))
                return false;
            break;
        case 0x8200: { /* CompressedQuickTime: the image follows as QuickDraw */
            if (!need(r, 4))
                return false;
            uint32_t n = rd_be32(r->p);
            r->p += 4;
            if (!skip(r, n))
                return false;
            break;
        }
        case 0x8201: /* UncompressedQuickTime: a matte for the image that follows */
            if (!quicktime(r))
                return false;
            break;
        case 0xA0: /* short comment */
            if (!skip(r, 2))
                return false;
            break;
        case 0xA1: { /* long comment */
            uint16_t kind, n;
            if (!u16(r, &kind) || !u16(r, &n) || !skip(r, n))
                return false;
            break;
        }
        case 0xFF: /* end of picture */
            return true;
        default:
            return fail(r, "picture opcode 0x%04x is not supported", op);
        }
    }
}

bool pict_draw(const uint8_t *data, size_t len, qd_rect dst, const qd_pixels *target, qd_rect clip,
               qd_rgb fg, qd_rgb bg, char *err, size_t errlen) {
    reader r = {.p = data, .end = data + len, .err = err, .errlen = errlen};
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
    bool ok = draw_ops(&r, data, frame, dst, target, clip, fg, bg);
    free(r.matte);
    return ok;
}
