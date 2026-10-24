#include "region.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_mem.h"
#include "memmgr.h"
#include "qd.h"
#include "trap.h"
#include "util.h"

#define RGN_END 0x7FFF
#define COORD_MIN (-32768)
#define COORD_MAX 32766

/* ---- the host form ---- */

void rgn_init(rgn *r) { memset(r, 0, sizeof *r); }

void rgn_free(rgn *r) {
    free(r->bands);
    free(r->xs);
    rgn_init(r);
}

static void *grow(void *p, int *cap, int need, size_t size) {
    if (need <= *cap)
        return p;
    int n = *cap ? *cap : 8;
    while (n < need)
        n *= 2;
    void *q = realloc(p, (size_t)n * size);
    if (!q)
        fatal("out of memory for a region");
    *cap = n;
    return q;
}

static void reserve_xs(rgn *r, int more) {
    r->xs = grow(r->xs, &r->cap_xs, r->nxs + more, sizeof *r->xs);
}

static void clear(rgn *r) { r->nbands = r->nxs = 0; }

/* Makes the n x's at r->xs[first], the last ones written, rows
   top..bottom-1: merged into the band above when it touches and is alike,
   dropped when there are none. */
static void end_band(rgn *r, int top, int bottom, int first, int n) {
    if (n == 0 || top >= bottom) {
        r->nxs = first;
        return;
    }
    if (r->nbands) {
        rgn_band *last = &r->bands[r->nbands - 1];
        if (last->bottom == top && last->n == n &&
            memcmp(&r->xs[last->first], &r->xs[first], (size_t)n * sizeof *r->xs) == 0) {
            last->bottom = (int16_t)bottom;
            r->nxs = first;
            return;
        }
    }
    r->bands = grow(r->bands, &r->cap_bands, r->nbands + 1, sizeof *r->bands);
    r->bands[r->nbands++] = (rgn_band){(int16_t)top, (int16_t)bottom, first, n};
}

static int16_t clamp(int v) {
    return (int16_t)(v < COORD_MIN ? COORD_MIN : v > COORD_MAX ? COORD_MAX : v);
}

void rgn_set_rect(rgn *r, qd_rect rect) {
    clear(r);
    rect = (qd_rect){clamp(rect.top), clamp(rect.left), clamp(rect.bottom), clamp(rect.right)};
    if (rect.left >= rect.right || rect.top >= rect.bottom)
        return;
    reserve_xs(r, 2);
    r->xs[0] = rect.left;
    r->xs[1] = rect.right;
    r->nxs = 2;
    end_band(r, rect.top, rect.bottom, 0, 2);
}

void rgn_copy(const rgn *from, rgn *to) {
    if (from == to)
        return;
    clear(to);
    reserve_xs(to, from->nxs);
    to->bands = grow(to->bands, &to->cap_bands, from->nbands, sizeof *to->bands);
    memcpy(to->xs, from->xs, (size_t)from->nxs * sizeof *to->xs);
    memcpy(to->bands, from->bands, (size_t)from->nbands * sizeof *to->bands);
    to->nxs = from->nxs;
    to->nbands = from->nbands;
}

bool rgn_is_empty(const rgn *r) { return r->nbands == 0; }

bool rgn_is_rect(const rgn *r) { return r->nbands == 1 && r->bands[0].n == 2; }

qd_rect rgn_bounds(const rgn *r) {
    if (!r->nbands)
        return (qd_rect){0, 0, 0, 0};
    qd_rect b = {r->bands[0].top, COORD_MAX, r->bands[r->nbands - 1].bottom, COORD_MIN};
    for (int i = 0; i < r->nbands; i++) {
        const rgn_band *band = &r->bands[i];
        if (r->xs[band->first] < b.left)
            b.left = r->xs[band->first];
        if (r->xs[band->first + band->n - 1] > b.right)
            b.right = r->xs[band->first + band->n - 1];
    }
    return b;
}

bool rgn_equal(const rgn *a, const rgn *b) {
    if (a->nbands != b->nbands)
        return false;
    for (int i = 0; i < a->nbands; i++) {
        const rgn_band *x = &a->bands[i], *y = &b->bands[i];
        if (x->top != y->top || x->bottom != y->bottom || x->n != y->n ||
            memcmp(&a->xs[x->first], &b->xs[y->first], (size_t)x->n * sizeof *a->xs) != 0)
            return false;
    }
    return true;
}

bool rgn_has_point(const rgn *r, int h, int v) {
    for (int i = 0; i < r->nbands; i++) {
        const rgn_band *band = &r->bands[i];
        if (v < band->top || v >= band->bottom)
            continue;
        for (int k = 0; k < band->n; k += 2)
            if (h >= r->xs[band->first + k] && h < r->xs[band->first + k + 1])
                return true;
        return false;
    }
    return false;
}

bool rgn_meets_rect(const rgn *r, qd_rect rect) {
    for (int i = 0; i < r->nbands; i++) {
        const rgn_band *band = &r->bands[i];
        if (band->bottom <= rect.top || band->top >= rect.bottom)
            continue;
        for (int k = 0; k < band->n; k += 2)
            if (r->xs[band->first + k] < rect.right && r->xs[band->first + k + 1] > rect.left)
                return true;
    }
    return false;
}

void rgn_each_rect(const rgn *r, void (*fn)(qd_rect rect, void *ctx), void *ctx) {
    for (int i = 0; i < r->nbands; i++) {
        const rgn_band *band = &r->bands[i];
        for (int k = 0; k < band->n; k += 2)
            fn((qd_rect){band->top, r->xs[band->first + k], band->bottom,
                         r->xs[band->first + k + 1]},
               ctx);
    }
}

/* ---- combining ---- */

static bool apply(rgn_op op, bool a, bool b) {
    switch (op) {
    case RGN_SECT: return a && b;
    case RGN_UNION: return a || b;
    case RGN_DIFF: return a && !b;
    case RGN_XOR: return a != b;
    }
    return false;
}

/* Appends to out->xs the x's of (xa op xb), each an interval list; returns
   how many. out->xs has room for na + nb more. */
static int combine_row(const int16_t *xa, int na, const int16_t *xb, int nb, rgn_op op, rgn *out) {
    int i = 0, j = 0, n = 0;
    bool in_a = false, in_b = false, in = false;
    while (i < na || j < nb) {
        int x = i < na && (j >= nb || xa[i] <= xb[j]) ? xa[i] : xb[j];
        while (i < na && xa[i] == x) /* a collapsed interval repeats its x */
            in_a = !in_a, i++;
        while (j < nb && xb[j] == x)
            in_b = !in_b, j++;
        bool now = apply(op, in_a, in_b);
        if (now != in) {
            out->xs[out->nxs + n++] = (int16_t)x;
            in = now;
        }
    }
    return n;
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

/* The band of r holding row y, from *at on (rows only go down), or NULL. */
static const rgn_band *band_at(const rgn *r, int y, int *at) {
    while (*at < r->nbands && r->bands[*at].bottom <= y)
        (*at)++;
    return *at < r->nbands && r->bands[*at].top <= y ? &r->bands[*at] : NULL;
}

void rgn_combine(const rgn *a, const rgn *b, rgn_op op, rgn *out) {
    int nys = 2 * (a->nbands + b->nbands);
    int *ys = malloc((size_t)(nys ? nys : 1) * sizeof *ys);
    if (!ys)
        fatal("out of memory for a region");
    int k = 0;
    for (int i = 0; i < a->nbands; i++)
        ys[k++] = a->bands[i].top, ys[k++] = a->bands[i].bottom;
    for (int i = 0; i < b->nbands; i++)
        ys[k++] = b->bands[i].top, ys[k++] = b->bands[i].bottom;
    qsort(ys, (size_t)nys, sizeof *ys, cmp_int);

    rgn res;
    rgn_init(&res);
    int at_a = 0, at_b = 0;
    for (int i = 0; i + 1 < nys; i++) {
        if (ys[i] == ys[i + 1])
            continue;
        const rgn_band *ba = band_at(a, ys[i], &at_a), *bb = band_at(b, ys[i], &at_b);
        int na = ba ? ba->n : 0, nb = bb ? bb->n : 0;
        reserve_xs(&res, na + nb);
        int first = res.nxs;
        int n = combine_row(ba ? &a->xs[ba->first] : NULL, na, bb ? &b->xs[bb->first] : NULL, nb,
                            op, &res);
        res.nxs += n;
        end_band(&res, ys[i], ys[i + 1], first, n);
    }
    free(ys);
    rgn_free(out);
    *out = res;
}

void rgn_offset(rgn *r, int dh, int dv) {
    bool clamped = false;
    for (int i = 0; i < r->nxs; i++) {
        int16_t x = clamp(r->xs[i] + dh);
        clamped |= x != r->xs[i] + dh;
        r->xs[i] = x;
    }
    for (int i = 0; i < r->nbands; i++) {
        int16_t t = clamp(r->bands[i].top + dv), b = clamp(r->bands[i].bottom + dv);
        clamped |= t != r->bands[i].top + dv || b != r->bands[i].bottom + dv;
        r->bands[i].top = t;
        r->bands[i].bottom = b;
    }
    if (clamped) { /* edges that met at the limit: make it canonical again */
        rgn empty;
        rgn_init(&empty);
        rgn_combine(r, &empty, RGN_UNION, r);
    }
}

/* r = op over t in 0..n of (r moved t along one axis), by doubling: once
   r is that for 0..c, r op (r moved s) is it for 0..c + s, while s <= c + 1. */
static void sweep(rgn *r, int n, bool across, rgn_op op) {
    rgn moved;
    rgn_init(&moved);
    for (int c = 0; c < n;) {
        int s = c + 1 < n - c ? c + 1 : n - c;
        rgn_copy(r, &moved);
        rgn_offset(&moved, across ? s : 0, across ? 0 : s);
        rgn_combine(r, &moved, op, r);
        c += s;
    }
    rgn_free(&moved);
}

void rgn_inset(rgn *r, int dh, int dv) {
    int ah = dh < 0 ? -dh : dh, av = dv < 0 ? -dv : dv;
    sweep(r, 2 * ah, true, dh > 0 ? RGN_SECT : RGN_UNION);
    sweep(r, 2 * av, false, dv > 0 ? RGN_SECT : RGN_UNION);
    rgn_offset(r, -ah, -av);
}

/* ---- the packed form ---- */

static bool bad(char *err, size_t errlen, const char *what) {
    snprintf(err, errlen, "%s", what);
    return false;
}

/* to = a xor b, both sorted without repeats; returns the count. */
static int xor_xs(const int16_t *a, int na, const int16_t *b, int nb, int16_t *to) {
    int i = 0, j = 0, n = 0;
    while (i < na || j < nb) {
        if (j >= nb || (i < na && a[i] < b[j]))
            to[n++] = a[i++];
        else if (i >= na || b[j] < a[i])
            to[n++] = b[j++];
        else
            i++, j++;
    }
    return n;
}

bool rgn_read(uint32_t h, rgn *out, char *err, size_t errlen) {
    clear(out);
    if (errlen)
        err[0] = '\0';
    if (!mm_is_handle(h))
        return bad(err, errlen, "not a handle");
    uint32_t p = gm_r32(h), hsize = mm_handle_size(h);
    if (hsize < RGN_HEADER)
        return bad(err, errlen, "smaller than a region's header");
    uint32_t size = gm_r16(p);
    if (size < RGN_HEADER || size > hsize || size % 2)
        return bad(err, errlen, "rgnSize doesn't fit the handle");
    if (size == RGN_HEADER) {
        rgn_set_rect(out, qd_read_rect(p + 2));
        return true;
    }
    int max = (int)(size - RGN_HEADER) / 2; /* no row holds more x's than the region has words */
    int16_t *cur = malloc((size_t)max * sizeof *cur), *next = malloc((size_t)max * sizeof *next);
    int16_t *row = malloc((size_t)max * sizeof *row);
    if (!cur || !next || !row)
        fatal("out of memory for a region");
    int ncur = 0, prev_y = 0;
    bool have_prev = false, ok = false;
    uint32_t at = RGN_HEADER;
    while (at + 2 <= size) {
        int y = (int16_t)gm_r16(p + at);
        at += 2;
        if (y == RGN_END) {
            ok = ncur == 0 ? true : bad(err, errlen, "the last row doesn't close the region");
            break;
        }
        if (have_prev && y <= prev_y) {
            bad(err, errlen, "rows out of order");
            break;
        }
        int nrow = 0;
        bool closed = false;
        while (at + 2 <= size) {
            int x = (int16_t)gm_r16(p + at);
            at += 2;
            if (x == RGN_END) {
                closed = true;
                break;
            }
            if (nrow && x <= row[nrow - 1])
                break;
            row[nrow++] = (int16_t)x;
        }
        if (!closed) {
            bad(err, errlen, "x's out of order, or a row without its end mark");
            break;
        }
        if (have_prev) {
            reserve_xs(out, ncur);
            memcpy(&out->xs[out->nxs], cur, (size_t)ncur * sizeof *cur);
            int first = out->nxs;
            out->nxs += ncur;
            end_band(out, prev_y, y, first, ncur);
        }
        int n = xor_xs(cur, ncur, row, nrow, next);
        if (n % 2) {
            bad(err, errlen, "a row with an odd number of edges");
            break;
        }
        int16_t *t = cur;
        cur = next, next = t, ncur = n;
        prev_y = y, have_prev = true;
    }
    if (!ok && errlen && !err[0])
        bad(err, errlen, "no end mark");
    free(cur);
    free(next);
    free(row);
    if (!ok)
        clear(out);
    return ok;
}

/* Writes (or, with p 0, only counts) the scan lines of r from offset at.
   Returns the offset after the end mark. */
static uint32_t pack(const rgn *r, uint32_t p, int16_t *tmp) {
    uint32_t at = RGN_HEADER;
#define PUT(v) (p ? gm_w16(p + at, (uint16_t)(v)) : (void)0, at += 2)
    const int16_t *prev = NULL;
    int nprev = 0, prev_bottom = 0;
    for (int i = 0; i < r->nbands; i++) {
        const rgn_band *band = &r->bands[i];
        if (nprev && prev_bottom != band->top) {
            PUT(prev_bottom);
            for (int k = 0; k < nprev; k++)
                PUT(prev[k]);
            PUT(RGN_END);
            nprev = 0;
        }
        int n = xor_xs(prev, nprev, &r->xs[band->first], band->n, tmp);
        PUT(band->top);
        for (int k = 0; k < n; k++)
            PUT(tmp[k]);
        PUT(RGN_END);
        prev = &r->xs[band->first];
        nprev = band->n;
        prev_bottom = band->bottom;
    }
    if (nprev) {
        PUT(prev_bottom);
        for (int k = 0; k < nprev; k++)
            PUT(prev[k]);
        PUT(RGN_END);
    }
    PUT(RGN_END);
#undef PUT
    return at;
}

bool rgn_write(uint32_t h, const rgn *r) {
    uint32_t size = RGN_HEADER;
    int16_t *tmp = NULL;
    if (!rgn_is_empty(r) && !rgn_is_rect(r)) {
        int widest = 0;
        for (int i = 0; i < r->nbands; i++)
            widest = r->bands[i].n > widest ? r->bands[i].n : widest;
        tmp = malloc((size_t)(2 * widest) * sizeof *tmp);
        if (!tmp)
            fatal("out of memory for a region");
        size = pack(r, 0, tmp);
    }
    if (size > RGN_MAX_SIZE || mm_set_handle_size(h, size) != MM_NO_ERR) {
        free(tmp);
        return false;
    }
    uint32_t p = gm_r32(h);
    gm_w16(p, (uint16_t)size);
    qd_write_rect(p + 2, rgn_bounds(r));
    if (tmp)
        pack(r, p, tmp);
    free(tmp);
    return true;
}

uint32_t rgn_new_handle(qd_rect rect) {
    uint32_t h = mm_new_handle(RGN_HEADER, true);
    if (!h)
        return 0;
    rgn r;
    rgn_init(&r);
    rgn_set_rect(&r, rect);
    rgn_write(h, &r);
    rgn_free(&r);
    return h;
}

void rgn_read_for(const char *call, uint32_t h, rgn *out) {
    char err[96] = "";
    rgn_init(out);
    if (!rgn_read(h, out, err, sizeof err))
        trap_crash("%s: 0x%08x is not a region (%s)", call, h, err);
}

void rgn_write_for(const char *call, uint32_t h, const rgn *r) {
    if (!rgn_write(h, r))
        trap_crash("%s: the region is too big for its handle 0x%08x", call, h);
}

/* ---- guest calls ---- */

static void h_new_rgn(void) {
    uint32_t h = rgn_new_handle((qd_rect){0, 0, 0, 0});
    if (!h)
        trap_crash("NewRgn: out of guest memory");
    trap_return(h);
}

static void h_dispose_rgn(void) {
    uint32_t h = trap_arg(0);
    if (!mm_is_handle(h))
        trap_crash("DisposeRgn: 0x%08x is not a region", h);
    mm_dispose_handle(h);
}

static void set_to_rect(const char *call, uint32_t h, qd_rect rect) {
    if (!mm_is_handle(h))
        trap_crash("%s: 0x%08x is not a region", call, h);
    rgn r;
    rgn_init(&r);
    rgn_set_rect(&r, rect);
    rgn_write_for(call, h, &r);
    rgn_free(&r);
}

/* RectRgn(RgnHandle, const Rect *) */
static void h_rect_rgn(void) { set_to_rect("RectRgn", trap_arg(0), qd_read_rect(trap_arg(1))); }

/* SetRectRgn(RgnHandle, short left, short top, short right, short bottom) */
static void h_set_rect_rgn(void) {
    qd_rect r = {(int16_t)trap_arg(2), (int16_t)trap_arg(1), (int16_t)trap_arg(4),
                 (int16_t)trap_arg(3)};
    set_to_rect("SetRectRgn", trap_arg(0), r);
}

static void h_set_empty_rgn(void) { set_to_rect("SetEmptyRgn", trap_arg(0), (qd_rect){0, 0, 0, 0}); }

/* CopyRgn(src, dst) */
static void h_copy_rgn(void) {
    rgn r;
    rgn_read_for("CopyRgn", trap_arg(0), &r);
    if (!mm_is_handle(trap_arg(1)))
        trap_crash("CopyRgn: 0x%08x is not a region", trap_arg(1));
    rgn_write_for("CopyRgn", trap_arg(1), &r);
    rgn_free(&r);
}

static void h_empty_rgn(void) {
    rgn r;
    rgn_read_for("EmptyRgn", trap_arg(0), &r);
    trap_return(rgn_is_empty(&r));
    rgn_free(&r);
}

static void h_equal_rgn(void) {
    rgn a, b;
    rgn_read_for("EqualRgn", trap_arg(0), &a);
    rgn_read_for("EqualRgn", trap_arg(1), &b);
    trap_return(rgn_equal(&a, &b));
    rgn_free(&a);
    rgn_free(&b);
}

/* OffsetRgn and InsetRgn (RgnHandle, short dh, short dv) */
static void h_offset_rgn(void) {
    rgn r;
    rgn_read_for("OffsetRgn", trap_arg(0), &r);
    rgn_offset(&r, (int16_t)trap_arg(1), (int16_t)trap_arg(2));
    rgn_write_for("OffsetRgn", trap_arg(0), &r);
    rgn_free(&r);
}

static void h_inset_rgn(void) {
    rgn r;
    rgn_read_for("InsetRgn", trap_arg(0), &r);
    rgn_inset(&r, (int16_t)trap_arg(1), (int16_t)trap_arg(2));
    rgn_write_for("InsetRgn", trap_arg(0), &r);
    rgn_free(&r);
}

/* SectRgn and the others (srcA, srcB, dst); dst may be either source. */
static void combine_call(const char *call, rgn_op op) {
    rgn a, b;
    rgn_read_for(call, trap_arg(0), &a);
    rgn_read_for(call, trap_arg(1), &b);
    if (!mm_is_handle(trap_arg(2)))
        trap_crash("%s: 0x%08x is not a region", call, trap_arg(2));
    rgn_combine(&a, &b, op, &a);
    rgn_write_for(call, trap_arg(2), &a);
    rgn_free(&a);
    rgn_free(&b);
}

static void h_sect_rgn(void) { combine_call("SectRgn", RGN_SECT); }
static void h_union_rgn(void) { combine_call("UnionRgn", RGN_UNION); }
static void h_diff_rgn(void) { combine_call("DiffRgn", RGN_DIFF); }
static void h_xor_rgn(void) { combine_call("XorRgn", RGN_XOR); }

/* PtInRgn(Point, RgnHandle): the Point by value, v in the high half. */
static void h_pt_in_rgn(void) {
    uint32_t pt = trap_arg(0);
    rgn r;
    rgn_read_for("PtInRgn", trap_arg(1), &r);
    trap_return(rgn_has_point(&r, (int16_t)(pt & 0xFFFF), (int16_t)(pt >> 16)));
    rgn_free(&r);
}

/* RectInRgn(const Rect *, RgnHandle) */
static void h_rect_in_rgn(void) {
    rgn r;
    rgn_read_for("RectInRgn", trap_arg(1), &r);
    trap_return(rgn_meets_rect(&r, qd_read_rect(trap_arg(0))));
    rgn_free(&r);
}

/* GetRegionBounds(RgnHandle, Rect *) returns the Rect *. */
static void h_get_region_bounds(void) {
    uint32_t h = trap_arg(0), out = trap_arg(1);
    if (!mm_is_handle(h) || mm_handle_size(h) < RGN_HEADER)
        trap_crash("GetRegionBounds: 0x%08x is not a region", h);
    qd_write_rect(out, qd_read_rect(gm_r32(h) + 2));
    trap_return(out);
}

void region_register(void) {
    trap_register("NewRgn", h_new_rgn);
    trap_register("DisposeRgn", h_dispose_rgn);
    trap_register("RectRgn", h_rect_rgn);
    trap_register("SetRectRgn", h_set_rect_rgn);
    trap_register("SetEmptyRgn", h_set_empty_rgn);
    trap_register("CopyRgn", h_copy_rgn);
    trap_register("EmptyRgn", h_empty_rgn);
    trap_register("EqualRgn", h_equal_rgn);
    trap_register("OffsetRgn", h_offset_rgn);
    trap_register("InsetRgn", h_inset_rgn);
    trap_register("SectRgn", h_sect_rgn);
    trap_register("UnionRgn", h_union_rgn);
    trap_register("DiffRgn", h_diff_rgn);
    trap_register("XorRgn", h_xor_rgn);
    trap_register("PtInRgn", h_pt_in_rgn);
    trap_register("RectInRgn", h_rect_in_rgn);
    trap_register("GetRegionBounds", h_get_region_bounds);
}
