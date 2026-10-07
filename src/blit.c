#include "blit.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "util.h"

static bool fail(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static bool fail(char *err, size_t errlen, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return false;
}

qd_rect rect_sect(qd_rect a, qd_rect b) {
    qd_rect r = {
        a.top > b.top ? a.top : b.top,
        a.left > b.left ? a.left : b.left,
        a.bottom < b.bottom ? a.bottom : b.bottom,
        a.right < b.right ? a.right : b.right,
    };
    if (rect_empty(r))
        r = (qd_rect){0, 0, 0, 0};
    return r;
}

void qd_std_palette(int depth, qd_palette *out) {
    memset(out, 0, sizeof *out);
    if (depth == 1) {
        out->n = 2;
        out->c[0] = (qd_rgb){0xFFFF, 0xFFFF, 0xFFFF};
        out->c[1] = (qd_rgb){0, 0, 0};
        return;
    }
    if (depth == 2) {
        static const qd_rgb c2[4] = {
            {0xFFFF, 0xFFFF, 0xFFFF}, {0xACAC, 0xACAC, 0xACAC}, {0x5555, 0x5555, 0x5555}, {0, 0, 0}};
        out->n = 4;
        memcpy(out->c, c2, sizeof c2);
        return;
    }
    if (depth == 4) {
        static const qd_rgb c4[16] = {
            {0xFFFF, 0xFFFF, 0xFFFF}, {0xFC00, 0xF37D, 0x052F}, {0xFFFF, 0x648A, 0x028C},
            {0xDD6B, 0x08C2, 0x06A2}, {0xF2D7, 0x0856, 0x84EC}, {0x46E3, 0x0000, 0xA53E},
            {0x0000, 0x0000, 0xD400}, {0x0241, 0xAB54, 0xEAFF}, {0x1F21, 0xB793, 0x1431},
            {0x0000, 0x64AF, 0x11B0}, {0x5600, 0x2C9D, 0x0524}, {0x90D7, 0x7160, 0x3A34},
            {0xC000, 0xC000, 0xC000}, {0x8000, 0x8000, 0x8000}, {0x4000, 0x4000, 0x4000},
            {0, 0, 0}};
        out->n = 16;
        memcpy(out->c, c4, sizeof c4);
        return;
    }
    /* 8 bits: a 6x6x6 color cube (white first, black omitted), then ten
       shades each of red, green, blue and gray, then black. */
    static const uint16_t cube[6] = {0xFFFF, 0xCCCC, 0x9999, 0x6666, 0x3333, 0x0000};
    static const uint16_t ramp[10] = {0xEEEE, 0xDDDD, 0xBBBB, 0xAAAA, 0x8888,
                                      0x7777, 0x5555, 0x4444, 0x2222, 0x1111};
    int i = 0;
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++)
                if (i < 215)
                    out->c[i++] = (qd_rgb){cube[r], cube[g], cube[b]};
    for (int k = 0; k < 10; k++)
        out->c[i++] = (qd_rgb){ramp[k], 0, 0};
    for (int k = 0; k < 10; k++)
        out->c[i++] = (qd_rgb){0, ramp[k], 0};
    for (int k = 0; k < 10; k++)
        out->c[i++] = (qd_rgb){0, 0, ramp[k]};
    for (int k = 0; k < 10; k++)
        out->c[i++] = (qd_rgb){ramp[k], ramp[k], ramp[k]};
    out->c[i++] = (qd_rgb){0, 0, 0};
    out->n = i;
}

static int nearest(qd_rgb c, const qd_palette *pal) {
    int best = 0;
    uint32_t best_d = UINT32_MAX;
    for (int i = 0; i < pal->n; i++) {
        int dr = (c.r >> 8) - (pal->c[i].r >> 8);
        int dg = (c.g >> 8) - (pal->c[i].g >> 8);
        int db = (c.b >> 8) - (pal->c[i].b >> 8);
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

uint32_t qd_pixel_for(qd_rgb c, int depth, const qd_palette *pal) {
    if (depth == 16)
        return ((uint32_t)(c.r >> 11) << 10) | ((uint32_t)(c.g >> 11) << 5) | (c.b >> 11);
    if (depth == 32)
        return ((uint32_t)(c.r >> 8) << 16) | ((uint32_t)(c.g >> 8) << 8) | (c.b >> 8);
    return (uint32_t)nearest(c, pal);
}

static qd_rgb rgb_of(uint32_t v, int depth, const qd_palette *pal) {
    if (depth == 16) {
        uint16_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        return (qd_rgb){(uint16_t)(r << 11 | r << 6 | r << 1 | r >> 4),
                        (uint16_t)(g << 11 | g << 6 | g << 1 | g >> 4),
                        (uint16_t)(b << 11 | b << 6 | b << 1 | b >> 4)};
    }
    if (depth == 32) {
        uint16_t r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
        return (qd_rgb){(uint16_t)(r << 8 | r), (uint16_t)(g << 8 | g), (uint16_t)(b << 8 | b)};
    }
    return v < (uint32_t)pal->n ? pal->c[v] : (qd_rgb){0, 0, 0};
}

static uint32_t get_px(const qd_pixels *p, int x, int y) {
    const uint8_t *row = p->base + (size_t)(y - p->bounds.top) * p->row_bytes;
    int i = x - p->bounds.left;
    switch (p->depth) {
    case 1: return (row[i >> 3] >> (7 - (i & 7))) & 1;
    case 2: return (row[i >> 2] >> (6 - 2 * (i & 3))) & 3;
    case 4: return (row[i >> 1] >> (4 - 4 * (i & 1))) & 15;
    case 8: return row[i];
    case 16: return rd_be16(row + 2 * i);
    default: return rd_be32(row + 4 * i) & 0xFFFFFF;
    }
}

static void put_px(const qd_pixels *p, int x, int y, uint32_t v) {
    uint8_t *row = p->base + (size_t)(y - p->bounds.top) * p->row_bytes;
    int i = x - p->bounds.left;
    switch (p->depth) {
    case 1: {
        int s = 7 - (i & 7);
        row[i >> 3] = (uint8_t)((row[i >> 3] & ~(1 << s)) | ((v & 1) << s));
        break;
    }
    case 2: {
        int s = 6 - 2 * (i & 3);
        row[i >> 2] = (uint8_t)((row[i >> 2] & ~(3 << s)) | ((v & 3) << s));
        break;
    }
    case 4: {
        int s = 4 - 4 * (i & 1);
        row[i >> 1] = (uint8_t)((row[i >> 1] & ~(15 << s)) | ((v & 15) << s));
        break;
    }
    case 8: row[i] = (uint8_t)v; break;
    case 16: wr_be16(row + 2 * i, (uint16_t)v); break;
    default: wr_be32(row + 4 * i, v & 0xFFFFFF); break;
    }
}

uint32_t qd_get_pixel(const qd_pixels *p, int x, int y) { return get_px(p, x, y); }
void qd_set_pixel(const qd_pixels *p, int x, int y, uint32_t v) { put_px(p, x, y, v); }
qd_rgb qd_color_of(uint32_t v, int depth, const qd_palette *pal) { return rgb_of(v, depth, pal); }

static bool indexed(int depth) { return depth <= 8; }

static bool same_palette(const qd_palette *a, const qd_palette *b) {
    return a->n == b->n && memcmp(a->c, b->c, sizeof a->c[0] * (size_t)a->n) == 0;
}

bool qd_blit(const qd_pixels *src, qd_rect sr, const qd_pixels *dst, qd_rect dr, qd_rect clip,
             int mode, qd_rgb fg, qd_rgb bg, char *err, size_t errlen) {
    if (mode != QD_SRC_COPY)
        return fail(err, errlen, "transfer mode %d is not supported", mode);
    if (rect_empty(sr) || rect_empty(dr))
        return true;
    /* Translate source pixel values into destination pixel values once. */
    static uint32_t map[256];
    bool use_map = indexed(src->depth);
    if (use_map) {
        int n = 1 << src->depth;
        bool identity = src->depth != 1 && dst->depth == src->depth && src->pal && dst->pal &&
                        same_palette(src->pal, dst->pal);
        for (int v = 0; v < n; v++) {
            if (identity) {
                map[v] = (uint32_t)v;
                continue;
            }
            qd_rgb c = src->depth == 1 ? (v ? fg : bg) : rgb_of((uint32_t)v, src->depth, src->pal);
            map[v] = qd_pixel_for(c, dst->depth, dst->pal);
        }
    }
    qd_rect area = rect_sect(rect_sect(dr, clip), dst->bounds);
    int sw = rect_w(sr), sh = rect_h(sr), dw = rect_w(dr), dh = rect_h(dr);
    for (int y = area.top; y < area.bottom; y++) {
        int sy = sr.top + (int)((int64_t)(y - dr.top) * sh / dh);
        if (sy < src->bounds.top || sy >= src->bounds.bottom)
            continue;
        for (int x = area.left; x < area.right; x++) {
            int sx = sr.left + (int)((int64_t)(x - dr.left) * sw / dw);
            if (sx < src->bounds.left || sx >= src->bounds.right)
                continue;
            uint32_t v = get_px(src, sx, sy);
            if (use_map)
                v = map[v];
            else if (dst->depth != src->depth) /* to indexed: the nearest color */
                v = qd_pixel_for(rgb_of(v, src->depth, NULL), dst->depth, dst->pal);
            put_px(dst, x, y, v);
        }
    }
    return true;
}

void qd_fill(const qd_pixels *dst, qd_rect r, qd_rect clip, qd_rgb c) {
    uint32_t v = qd_pixel_for(c, dst->depth, dst->pal);
    qd_rect area = rect_sect(rect_sect(r, clip), dst->bounds);
    for (int y = area.top; y < area.bottom; y++)
        for (int x = area.left; x < area.right; x++)
            put_px(dst, x, y, v);
}

void qd_to_rgba(const qd_pixels *src, uint8_t *rgba) {
    for (int y = src->bounds.top; y < src->bounds.bottom; y++) {
        for (int x = src->bounds.left; x < src->bounds.right; x++) {
            qd_rgb c = rgb_of(get_px(src, x, y), src->depth, src->pal);
            *rgba++ = (uint8_t)(c.r >> 8);
            *rgba++ = (uint8_t)(c.g >> 8);
            *rgba++ = (uint8_t)(c.b >> 8);
            *rgba++ = 0xFF;
        }
    }
}
