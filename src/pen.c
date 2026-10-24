#include "pen.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "guest_mem.h"
#include "memmgr.h"
#include "qd.h"
#include "region.h"
#include "trap.h"
#include "util.h"

/* PenState: pnLoc, pnSize, pnMode, pnPat. */
#define PENSTATE_SIZE 18

static const uint8_t black[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t white[8] = {0};
static const uint8_t gray[8] = {0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55};
static const uint8_t light_gray[8] = {0x88, 0x22, 0x88, 0x22, 0x88, 0x22, 0x88, 0x22};
static const uint8_t dark_gray[8] = {0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD};

static uint32_t port(void) { return qd_current_port(); }

/* The port's pen or background pattern (field PORT_PN_PIXPAT or PORT_BK_PIXPAT). */
static void pattern(uint32_t p, uint32_t field, uint8_t out[8]) {
    uint32_t h = gm_r32(p + field);
    if (h)
        memcpy(out, gm_ptr(gm_r32(h), 8), 8);
    else
        memcpy(out, field == PORT_PN_PIXPAT ? black : white, 8);
}

static void set_pattern(uint32_t p, uint32_t field, const uint8_t pat[8]) {
    uint32_t h = gm_r32(p + field);
    if (!h) {
        h = mm_new_handle(8, false);
        if (!h)
            trap_crash("out of guest memory for a pattern");
        gm_w32(p + field, h);
    }
    memcpy(gm_ptr(gm_r32(h), 8), pat, 8);
}

static int16_t pen_h(void) { return (int16_t)gm_r16(port() + PORT_PN_LOC + 2); }
static int16_t pen_v(void) { return (int16_t)gm_r16(port() + PORT_PN_LOC); }

static void move_pen(int h, int v) {
    gm_w16(port() + PORT_PN_LOC, (uint16_t)v);
    gm_w16(port() + PORT_PN_LOC + 2, (uint16_t)h);
}

/* ---- drawing a list of rectangles in a pattern ---- */

typedef struct {
    const char *call;
    qd_rect *rects;
    size_t n, cap;
    uint8_t pat[8];
    int mode;
    qd_rgb fg, bg;
} shape;

static void add(shape *s, qd_rect r) {
    if (rect_empty(r))
        return;
    if (s->n == s->cap) {
        s->cap = s->cap ? 2 * s->cap : 16;
        s->rects = realloc(s->rects, s->cap * sizeof *s->rects);
        if (!s->rects)
            fatal("out of memory");
    }
    s->rects[s->n++] = r;
}

static void add_rgn_rect(qd_rect r, void *ctx) { add(ctx, r); }

static void draw_clipped(const qd_pixels *px, qd_rect clip, void *ctx) {
    shape *s = ctx;
    for (size_t i = 0; i < s->n; i++)
        if (!qd_fill_pattern(px, s->rects[i], clip, s->pat, s->mode, s->fg, s->bg))
            trap_crash("%s: transfer mode %d is not supported", s->call, s->mode);
}

/* A shape in the pen's pattern and mode, the background pattern (erasing),
   or a pattern the call gives (filling), both in patCopy. */
static shape pen_shape(const char *call) {
    shape s = {.call = call, .mode = (int16_t)gm_r16(port() + PORT_PN_MODE)};
    pattern(port(), PORT_PN_PIXPAT, s.pat);
    return s;
}

static shape erase_shape(const char *call) {
    shape s = {.call = call, .mode = QD_PAT_COPY};
    pattern(port(), PORT_BK_PIXPAT, s.pat);
    return s;
}

static shape fill_shape(const char *call, uint32_t pat) {
    shape s = {.call = call, .mode = QD_PAT_COPY};
    memcpy(s.pat, gm_ptr(pat, 8), 8);
    return s;
}

/* Draws s and frees it. The pen's shapes draw nothing while it is hidden. */
static void draw(shape *s, bool with_pen) {
    if (!with_pen || (int16_t)gm_r16(port() + PORT_PN_VIS) >= 0) {
        s->fg = qd_read_rgb(port() + PORT_RGB_FG);
        s->bg = qd_read_rgb(port() + PORT_RGB_BK);
        if (s->n)
            qd_draw(s->call, 0, draw_clipped, s);
    }
    free(s->rects);
}

/* ---- the pen ---- */

static void h_move_to(void) { move_pen((int16_t)trap_arg(0), (int16_t)trap_arg(1)); }
static void h_move(void) { move_pen(pen_h() + (int16_t)trap_arg(0), pen_v() + (int16_t)trap_arg(1)); }

static void h_get_pen(void) {
    gm_w16(trap_arg(0), (uint16_t)pen_v());
    gm_w16(trap_arg(0) + 2, (uint16_t)pen_h());
}

/* A line from the pen to (h, v): the pen's rectangle at every point. */
static void line_to(const char *call, int h1, int v1) {
    int h0 = pen_h(), v0 = pen_v();
    int pw = (int16_t)gm_r16(port() + PORT_PN_SIZE + 2), ph = (int16_t)gm_r16(port() + PORT_PN_SIZE);
    move_pen(h1, v1);
    if (pw <= 0 || ph <= 0)
        return;
    shape s = pen_shape(call);
    int dh = abs(h1 - h0), dv = abs(v1 - v0), sh = h0 < h1 ? 1 : -1, sv = v0 < v1 ? 1 : -1;
    int err = dh - dv, h = h0, v = v0;
    for (;;) {
        add(&s, (qd_rect){(int16_t)v, (int16_t)h, (int16_t)(v + ph), (int16_t)(h + pw)});
        if (h == h1 && v == v1)
            break;
        int e2 = 2 * err;
        if (e2 > -dv) {
            err -= dv;
            h += sh;
        }
        if (e2 < dh) {
            err += dh;
            v += sv;
        }
    }
    draw(&s, true);
}

static void h_line_to(void) { line_to("LineTo", (int16_t)trap_arg(0), (int16_t)trap_arg(1)); }
static void h_line(void) {
    line_to("Line", pen_h() + (int16_t)trap_arg(0), pen_v() + (int16_t)trap_arg(1));
}

static void h_pen_size(void) {
    gm_w16(port() + PORT_PN_SIZE, (uint16_t)trap_arg(1));
    gm_w16(port() + PORT_PN_SIZE + 2, (uint16_t)trap_arg(0));
}

static void h_pen_mode(void) { gm_w16(port() + PORT_PN_MODE, (uint16_t)trap_arg(0)); }
static void h_pen_pat(void) { set_pattern(port(), PORT_PN_PIXPAT, gm_ptr(trap_arg(0), 8)); }
static void h_back_pat(void) { set_pattern(port(), PORT_BK_PIXPAT, gm_ptr(trap_arg(0), 8)); }

static void h_pen_normal(void) {
    gm_w16(port() + PORT_PN_SIZE, 1);
    gm_w16(port() + PORT_PN_SIZE + 2, 1);
    gm_w16(port() + PORT_PN_MODE, QD_PAT_COPY);
    set_pattern(port(), PORT_PN_PIXPAT, black);
}

static void h_hide_pen(void) {
    gm_w16(port() + PORT_PN_VIS, (uint16_t)((int16_t)gm_r16(port() + PORT_PN_VIS) - 1));
}

static void h_show_pen(void) {
    gm_w16(port() + PORT_PN_VIS, (uint16_t)((int16_t)gm_r16(port() + PORT_PN_VIS) + 1));
}

static void h_get_pen_state(void) {
    uint32_t out = trap_arg(0);
    memcpy(gm_ptr(out, 10), gm_ptr(port() + PORT_PN_LOC, 10), 10); /* pnLoc, pnSize, pnMode */
    uint8_t pat[8];
    pattern(port(), PORT_PN_PIXPAT, pat);
    memcpy(gm_ptr(out + 10, 8), pat, 8);
}

static void h_set_pen_state(void) {
    uint32_t in = trap_arg(0);
    memcpy(gm_ptr(port() + PORT_PN_LOC, 10), gm_ptr(in, 10), 10);
    set_pattern(port(), PORT_PN_PIXPAT, gm_ptr(in + 10, 8));
}

/* ---- rectangles ---- */

/* r's outline, the pen's size inside it: the whole of r when that leaves
   no hole. */
static void frame(shape *s, qd_rect r) {
    int pw = (int16_t)gm_r16(port() + PORT_PN_SIZE + 2), ph = (int16_t)gm_r16(port() + PORT_PN_SIZE);
    if (pw <= 0 || ph <= 0)
        return;
    if (2 * pw >= rect_w(r) || 2 * ph >= rect_h(r)) {
        add(s, r);
        return;
    }
    add(s, (qd_rect){r.top, r.left, (int16_t)(r.top + ph), r.right});
    add(s, (qd_rect){(int16_t)(r.bottom - ph), r.left, r.bottom, r.right});
    add(s, (qd_rect){(int16_t)(r.top + ph), r.left, (int16_t)(r.bottom - ph), (int16_t)(r.left + pw)});
    add(s, (qd_rect){(int16_t)(r.top + ph), (int16_t)(r.right - pw), (int16_t)(r.bottom - ph), r.right});
}

static void h_frame_rect(void) {
    shape s = pen_shape("FrameRect");
    frame(&s, qd_read_rect(trap_arg(0)));
    draw(&s, true);
}

static void h_erase_rect(void) {
    shape s = erase_shape("EraseRect");
    add(&s, qd_read_rect(trap_arg(0)));
    draw(&s, false);
}

static void h_fill_rect(void) {
    shape s = fill_shape("FillRect", trap_arg(1));
    add(&s, qd_read_rect(trap_arg(0)));
    draw(&s, false);
}

/* How far in from each side row y (from 0) of a rounded rectangle h high
   starts, for corners ow wide and oh high. */
static int corner_inset(int y, int h, int ow, int oh) {
    double rx = ow / 2.0, ry = oh / 2.0;
    if (rx <= 0 || ry <= 0)
        return 0;
    double yc = y + 0.5;
    double dy = yc < ry ? ry - yc : yc > h - ry ? yc - (h - ry) : 0;
    if (dy <= 0)
        return 0;
    double t = dy / ry;
    return (int)lround(rx - rx * sqrt(t < 1 ? 1 - t * t : 0));
}

/* The rows of r's rounded rectangle, ow by oh corners (clamped to r). */
static void round_rows(shape *s, qd_rect r, int ow, int oh) {
    int w = rect_w(r), h = rect_h(r);
    ow = ow < w ? ow : w;
    oh = oh < h ? oh : h;
    for (int y = 0; y < h; y++) {
        int in = corner_inset(y, h, ow, oh);
        add(s, (qd_rect){(int16_t)(r.top + y), (int16_t)(r.left + in), (int16_t)(r.top + y + 1),
                         (int16_t)(r.right - in)});
    }
}

static void h_frame_round_rect(void) {
    qd_rect r = qd_read_rect(trap_arg(0));
    int ow = (int16_t)trap_arg(1), oh = (int16_t)trap_arg(2);
    int pw = (int16_t)gm_r16(port() + PORT_PN_SIZE + 2), ph = (int16_t)gm_r16(port() + PORT_PN_SIZE);
    shape s = pen_shape("FrameRoundRect");
    int w = rect_w(r), h = rect_h(r);
    if (pw > 0 && ph > 0 && w > 0 && h > 0) {
        int cw = ow < w ? ow : w, ch = oh < h ? oh : h;
        int iw = w - 2 * pw, ih = h - 2 * ph, icw = cw - 2 * pw, ich = ch - 2 * ph;
        for (int y = 0; y < h; y++) {
            int out = corner_inset(y, h, cw, ch);
            int16_t top = (int16_t)(r.top + y), bottom = (int16_t)(r.top + y + 1);
            int iy = y - ph;
            if (iw <= 0 || ih <= 0 || iy < 0 || iy >= ih) {
                add(&s, (qd_rect){top, (int16_t)(r.left + out), bottom, (int16_t)(r.right - out)});
                continue;
            }
            int in = pw + corner_inset(iy, ih, icw > 0 ? icw : 0, ich > 0 ? ich : 0);
            add(&s, (qd_rect){top, (int16_t)(r.left + out), bottom, (int16_t)(r.left + in)});
            add(&s, (qd_rect){top, (int16_t)(r.right - in), bottom, (int16_t)(r.right - out)});
        }
    }
    draw(&s, true);
}

static void h_paint_round_rect(void) {
    shape s = pen_shape("PaintRoundRect");
    round_rows(&s, qd_read_rect(trap_arg(0)), (int16_t)trap_arg(1), (int16_t)trap_arg(2));
    draw(&s, true);
}

static void h_erase_round_rect(void) {
    shape s = erase_shape("EraseRoundRect");
    round_rows(&s, qd_read_rect(trap_arg(0)), (int16_t)trap_arg(1), (int16_t)trap_arg(2));
    draw(&s, false);
}

/* ---- regions ---- */

static void add_rgn(shape *s, uint32_t h) {
    rgn r;
    rgn_read_for(s->call, h, &r);
    rgn_each_rect(&r, add_rgn_rect, s);
    rgn_free(&r);
}

static void h_paint_rgn(void) {
    shape s = pen_shape("PaintRgn");
    add_rgn(&s, trap_arg(0));
    draw(&s, true);
}

static void h_erase_rgn(void) {
    shape s = erase_shape("EraseRgn");
    add_rgn(&s, trap_arg(0));
    draw(&s, false);
}

static void h_fill_rgn(void) {
    shape s = fill_shape("FillRgn", trap_arg(1));
    add_rgn(&s, trap_arg(0));
    draw(&s, false);
}

/* ---- colors and the standard patterns ---- */

static void h_rgb_back_color(void) { qd_write_rgb(port() + PORT_RGB_BK, qd_read_rgb(trap_arg(0))); }
static void h_get_fore_color(void) { qd_write_rgb(trap_arg(0), qd_read_rgb(port() + PORT_RGB_FG)); }
static void h_get_back_color(void) { qd_write_rgb(trap_arg(0), qd_read_rgb(port() + PORT_RGB_BK)); }

static void give_pattern(const uint8_t pat[8]) {
    memcpy(gm_ptr(trap_arg(0), 8), pat, 8);
    trap_return(trap_arg(0));
}

static void h_qd_black(void) { give_pattern(black); }
static void h_qd_white(void) { give_pattern(white); }
static void h_qd_gray(void) { give_pattern(gray); }
static void h_qd_light_gray(void) { give_pattern(light_gray); }
static void h_qd_dark_gray(void) { give_pattern(dark_gray); }

/* ---- rectangle and point arithmetic ---- */

static void write_or_empty(uint32_t a, qd_rect r) {
    qd_write_rect(a, rect_empty(r) ? (qd_rect){0, 0, 0, 0} : r);
}

static void h_inset_rect(void) {
    qd_rect r = qd_read_rect(trap_arg(0));
    int16_t dh = (int16_t)trap_arg(1), dv = (int16_t)trap_arg(2);
    write_or_empty(trap_arg(0), (qd_rect){(int16_t)(r.top + dv), (int16_t)(r.left + dh),
                                          (int16_t)(r.bottom - dv), (int16_t)(r.right - dh)});
}

static void h_sect_rect(void) {
    qd_rect r = rect_sect(qd_read_rect(trap_arg(0)), qd_read_rect(trap_arg(1)));
    write_or_empty(trap_arg(2), r);
    trap_return(!rect_empty(r));
}

static void h_union_rect(void) {
    qd_rect a = qd_read_rect(trap_arg(0)), b = qd_read_rect(trap_arg(1));
    qd_write_rect(trap_arg(2), (qd_rect){a.top < b.top ? a.top : b.top, a.left < b.left ? a.left : b.left,
                                         a.bottom > b.bottom ? a.bottom : b.bottom,
                                         a.right > b.right ? a.right : b.right});
}

static void h_equal_rect(void) {
    trap_return(memcmp(gm_ptr(trap_arg(0), RECT_SIZE), gm_ptr(trap_arg(1), RECT_SIZE), RECT_SIZE) == 0);
}

static void h_empty_rect(void) { trap_return(rect_empty(qd_read_rect(trap_arg(0)))); }

/* PtInRect(Point pt, const Rect *r): a Point by value is v in the high half. */
static void h_pt_in_rect(void) {
    int16_t v = (int16_t)(trap_arg(0) >> 16), h = (int16_t)trap_arg(0);
    qd_rect r = qd_read_rect(trap_arg(1));
    trap_return(v >= r.top && v < r.bottom && h >= r.left && h < r.right);
}

static void h_equal_pt(void) { trap_return(trap_arg(0) == trap_arg(1)); }

void pen_register(void) {
    trap_register("MoveTo", h_move_to);
    trap_register("Move", h_move);
    trap_register("GetPen", h_get_pen);
    trap_register("LineTo", h_line_to);
    trap_register("Line", h_line);
    trap_register("PenSize", h_pen_size);
    trap_register("PenMode", h_pen_mode);
    trap_register("PenPat", h_pen_pat);
    trap_register("PenNormal", h_pen_normal);
    trap_register("HidePen", h_hide_pen);
    trap_register("ShowPen", h_show_pen);
    trap_register("GetPenState", h_get_pen_state);
    trap_register("SetPenState", h_set_pen_state);
    trap_register("BackPat", h_back_pat);
    trap_register("FrameRect", h_frame_rect);
    trap_register("EraseRect", h_erase_rect);
    trap_register("FillRect", h_fill_rect);
    trap_register("FrameRoundRect", h_frame_round_rect);
    trap_register("PaintRoundRect", h_paint_round_rect);
    trap_register("EraseRoundRect", h_erase_round_rect);
    trap_register("PaintRgn", h_paint_rgn);
    trap_register("EraseRgn", h_erase_rgn);
    trap_register("FillRgn", h_fill_rgn);
    trap_register("RGBBackColor", h_rgb_back_color);
    trap_register("GetForeColor", h_get_fore_color);
    trap_register("GetBackColor", h_get_back_color);
    trap_register("GetQDGlobalsBlack", h_qd_black);
    trap_register("GetQDGlobalsWhite", h_qd_white);
    trap_register("GetQDGlobalsGray", h_qd_gray);
    trap_register("GetQDGlobalsLightGray", h_qd_light_gray);
    trap_register("GetQDGlobalsDarkGray", h_qd_dark_gray);
    trap_register("InsetRect", h_inset_rect);
    trap_register("SectRect", h_sect_rect);
    trap_register("UnionRect", h_union_rect);
    trap_register("EqualRect", h_equal_rect);
    trap_register("EmptyRect", h_empty_rect);
    trap_register("PtInRect", h_pt_in_rect);
    trap_register("EqualPt", h_equal_pt);
}
