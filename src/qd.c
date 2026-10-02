#include "qd.h"

#include <string.h>

#include "guest_mem.h"
#include "memmgr.h"
#include "pict.h"
#include "rsrc.h"
#include "trap.h"
#include "util.h"

#define MAX_PORTS 256
#define WIDE_OPEN ((qd_rect){-32768, -32768, 32767, 32767})

typedef enum { KIND_SCREEN, KIND_GWORLD, KIND_WINDOW } port_kind;

typedef struct {
    uint32_t addr;
    port_kind kind;
    bool visible;
} port_info;

static struct {
    uint32_t main_device;
    uint32_t screen_pm;  /* the screen's PixMapHandle, shared by windows */
    uint32_t screen_port;
    port_info ports[MAX_PORTS];
    int nports;
    uint32_t cur_port, cur_device;
    bool dirty;
    int saved_w, saved_h; /* screen size before BeginFullScreen */
} Q;

/* ---- guest structure helpers ---- */

qd_rect qd_read_rect(uint32_t a) {
    return (qd_rect){(int16_t)gm_r16(a), (int16_t)gm_r16(a + 2), (int16_t)gm_r16(a + 4),
                     (int16_t)gm_r16(a + 6)};
}

void qd_write_rect(uint32_t a, qd_rect r) {
    gm_w16(a, (uint16_t)r.top);
    gm_w16(a + 2, (uint16_t)r.left);
    gm_w16(a + 4, (uint16_t)r.bottom);
    gm_w16(a + 6, (uint16_t)r.right);
}

static qd_rgb read_rgb(uint32_t a) { return (qd_rgb){gm_r16(a), gm_r16(a + 2), gm_r16(a + 4)}; }

static void write_rgb(uint32_t a, qd_rgb c) {
    gm_w16(a, c.r);
    gm_w16(a + 2, c.g);
    gm_w16(a + 4, c.b);
}

void qd_read_ctab(uint32_t ctab, qd_palette *pal) {
    memset(pal, 0, sizeof *pal);
    uint32_t t = gm_r32(ctab);
    uint16_t flags = gm_r16(t + 4);
    uint32_t n = (uint16_t)(gm_r16(t + 6) + 1u);
    if (n > 256)
        n = 256;
    pal->n = 256;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = t + CTAB_HEADER + 8 * i;
        uint32_t v = (flags & 0x8000) ? i : (gm_r16(e) & 0xFFu);
        pal->c[v] = read_rgb(e + 2);
    }
}

uint32_t qd_new_ctab(const qd_palette *pal) {
    uint32_t h = mm_new_handle(CTAB_HEADER + 8u * (uint32_t)pal->n, true);
    if (!h)
        trap_crash("out of guest memory for a color table");
    uint32_t t = gm_r32(h);
    gm_w32(t, 1000u + (uint32_t)pal->n); /* ctSeed */
    gm_w16(t + 4, 0);
    gm_w16(t + 6, (uint16_t)(pal->n - 1));
    for (int i = 0; i < pal->n; i++) {
        uint32_t e = t + CTAB_HEADER + 8u * (uint32_t)i;
        gm_w16(e, (uint16_t)i);
        write_rgb(e + 2, pal->c[i]);
    }
    return h;
}

static uint32_t new_rgn(qd_rect r) {
    uint32_t h = mm_new_handle(10, true);
    if (!h)
        trap_crash("out of guest memory for a region");
    gm_w16(gm_r32(h), 10);
    qd_write_rect(gm_r32(h) + 2, r);
    return h;
}

/* The bounding box of a region, which must be rectangular. */
static qd_rect rgn_rect(const char *call, uint32_t rgn) {
    uint32_t p = gm_r32(rgn);
    if (gm_r16(p) != 10)
        trap_crash("%s: non-rectangular regions are not supported", call);
    return qd_read_rect(p + 2);
}

static uint32_t row_bytes_for(int width, int depth) {
    return (uint32_t)((width * depth + 31) / 32 * 4);
}

/* Fills a PixMap for depth and bounds, with new zeroed pixels. */
static void setup_pixmap(uint32_t pm_h, int depth, qd_rect bounds, uint32_t ctab) {
    uint32_t rb = row_bytes_for(rect_w(bounds), depth);
    uint32_t base = mm_new_ptr(rb * (uint32_t)rect_h(bounds), true);
    if (!base)
        trap_crash("out of guest memory for a %dx%d %d-bit pixmap", rect_w(bounds), rect_h(bounds),
                   depth);
    uint32_t pm = gm_r32(pm_h);
    memset(gm_ptr(pm, PIXMAP_SIZE), 0, PIXMAP_SIZE);
    gm_w32(pm + PM_BASE_ADDR, base);
    gm_w16(pm + PM_ROW_BYTES, (uint16_t)(rb | 0x8000));
    qd_write_rect(pm + PM_BOUNDS, bounds);
    gm_w32(pm + PM_HRES, 72u << 16);
    gm_w32(pm + PM_VRES, 72u << 16);
    gm_w16(pm + PM_PIXEL_TYPE, depth > 8 ? 16 : 0);
    gm_w16(pm + PM_PIXEL_SIZE, (uint16_t)depth);
    gm_w16(pm + PM_CMP_COUNT, depth > 8 ? 3 : 1);
    gm_w16(pm + PM_CMP_SIZE, depth == 16 ? 5 : depth == 32 ? 8 : (uint16_t)depth);
    gm_w32(pm + PM_PIXEL_FORMAT, (uint32_t)depth);
    gm_w32(pm + PM_TABLE, ctab);
}

static uint32_t std_ctab(int depth) {
    qd_palette pal;
    if (depth <= 8) {
        qd_std_palette(depth, &pal);
    } else {
        memset(&pal, 0, sizeof pal);
        pal.n = 1; /* direct pixmaps still carry a (trivial) table */
    }
    return qd_new_ctab(&pal);
}

static void free_pixmap_contents(uint32_t pm_h) {
    uint32_t pm = gm_r32(pm_h);
    mm_dispose_ptr(gm_r32(pm + PM_BASE_ADDR));
    mm_dispose_handle(gm_r32(pm + PM_TABLE));
}

void qd_bits(const char *call, uint32_t bits, qd_pixels *out, qd_palette *pal) {
    uint16_t rb = gm_r16(bits + PM_ROW_BYTES);
    qd_rect b = qd_read_rect(bits + PM_BOUNDS);
    int depth = 1;
    if (rb & 0x8000) {
        depth = gm_r16(bits + PM_PIXEL_SIZE);
        if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16 && depth != 32)
            trap_crash("%s: %d-bit pixmaps are not supported", call, depth);
        if (depth <= 8) {
            if (!gm_r32(bits + PM_TABLE))
                trap_crash("%s: %d-bit pixmap has no color table", call, depth);
            qd_read_ctab(gm_r32(bits + PM_TABLE), pal);
        }
    } else {
        qd_std_palette(1, pal);
    }
    uint32_t row_bytes = rb & 0x3FFFu;
    uint32_t w = rect_w(b) > 0 ? (uint32_t)rect_w(b) : 0;
    uint32_t h = rect_h(b) > 0 ? (uint32_t)rect_h(b) : 0;
    if ((uint64_t)row_bytes * 8 < (uint64_t)w * (uint32_t)depth)
        trap_crash("%s: pixmap rowBytes %u is too small for %u %d-bit pixels", call, row_bytes, w,
                   depth);
    uint32_t base = gm_r32(bits + PM_BASE_ADDR);
    out->base = gm_ptr(base, row_bytes * h);
    out->row_bytes = row_bytes;
    out->bounds = b;
    out->depth = depth;
    out->pal = pal;
}

/* ---- ports ---- */

static port_info *find_port(uint32_t addr) {
    for (int i = 0; i < Q.nports; i++)
        if (Q.ports[i].addr == addr)
            return &Q.ports[i];
    return NULL;
}

static port_info *need_port(const char *call, uint32_t addr) {
    port_info *p = find_port(addr);
    if (!p)
        trap_crash("%s: 0x%08x is not a port", call, addr);
    return p;
}

static uint32_t new_port(uint32_t pm_h, qd_rect port_rect, port_kind kind) {
    if (Q.nports == MAX_PORTS)
        trap_crash("more than %d ports", MAX_PORTS);
    uint32_t p = mm_new_ptr(CGRAFPORT_SIZE, true);
    if (!p)
        trap_crash("out of guest memory for a port");
    gm_w32(p + PORT_PIXMAP, pm_h);
    gm_w16(p + PORT_VERSION, 0xC000);
    qd_write_rect(p + PORT_RECT, port_rect);
    gm_w32(p + PORT_VIS_RGN, new_rgn(port_rect));
    gm_w32(p + PORT_CLIP_RGN, new_rgn(WIDE_OPEN));
    write_rgb(p + PORT_RGB_FG, (qd_rgb){0, 0, 0});
    write_rgb(p + PORT_RGB_BK, (qd_rgb){0xFFFF, 0xFFFF, 0xFFFF});
    gm_w16(p + PORT_PN_SIZE, 1);
    gm_w16(p + PORT_PN_SIZE + 2, 1);
    gm_w32(p + PORT_FG_COLOR, 33); /* blackColor */
    gm_w32(p + PORT_BK_COLOR, 30); /* whiteColor */
    Q.ports[Q.nports++] = (port_info){p, kind, kind != KIND_GWORLD};
    return p;
}

static void dispose_port(port_info *info) {
    uint32_t p = info->addr;
    mm_dispose_handle(gm_r32(p + PORT_VIS_RGN));
    mm_dispose_handle(gm_r32(p + PORT_CLIP_RGN));
    mm_dispose_ptr(p);
    *info = Q.ports[--Q.nports];
    if (Q.cur_port == p)
        Q.cur_port = Q.screen_port;
}

static uint32_t port_pixmap(uint32_t port) { return gm_r32(gm_r32(port + PORT_PIXMAP)); }

static bool is_screen_bits(uint32_t bits) {
    return gm_r32(bits + PM_BASE_ADDR) == gm_r32(gm_r32(Q.screen_pm) + PM_BASE_ADDR);
}

/* The current port's clip rectangle intersected with its portRect. */
static qd_rect port_clip(const char *call, uint32_t port) {
    return rect_sect(qd_read_rect(port + PORT_RECT), rgn_rect(call, gm_r32(port + PORT_CLIP_RGN)));
}

/* ---- the screen ---- */

static void make_screen(int w, int h, int depth) {
    qd_rect b = {0, 0, (int16_t)h, (int16_t)w};
    setup_pixmap(Q.screen_pm, depth, b, std_ctab(depth));
    uint32_t gd = gm_r32(Q.main_device);
    gm_w16(gd + GD_TYPE, depth > 8 ? 2 : 0);
    qd_write_rect(gd + GD_RECT, b);
    for (int i = 0; i < Q.nports; i++)
        if (Q.ports[i].kind != KIND_GWORLD) {
            qd_write_rect(Q.ports[i].addr + PORT_RECT, b);
            qd_write_rect(gm_r32(gm_r32(Q.ports[i].addr + PORT_VIS_RGN)) + 2, b);
        }
    Q.dirty = true;
}

void qd_init(int width, int height, int depth) {
    memset(&Q, 0, sizeof Q);
    Q.main_device = mm_new_handle(GDEVICE_SIZE, true);
    Q.screen_pm = mm_new_handle(PIXMAP_SIZE, true);
    if (!Q.main_device || !Q.screen_pm)
        trap_crash("out of guest memory for the screen");
    gm_w32(gm_r32(Q.main_device) + GD_PMAP, Q.screen_pm);
    gm_w16(gm_r32(Q.main_device) + GD_FLAGS, 0x8000 | 0x0001); /* main screen, screen device */
    make_screen(width, height, depth);
    Q.screen_port = new_port(Q.screen_pm, (qd_rect){0, 0, (int16_t)height, (int16_t)width},
                             KIND_SCREEN);
    Q.cur_port = Q.screen_port;
    Q.cur_device = Q.main_device;
}

uint32_t qd_main_device(void) { return Q.main_device; }
uint32_t qd_current_port(void) { return Q.cur_port; }

void qd_screen(qd_pixels *out, qd_palette *pal) { qd_bits("screen", gm_r32(Q.screen_pm), out, pal); }

bool qd_take_dirty(void) {
    bool d = Q.dirty;
    Q.dirty = false;
    return d;
}


static void resize_screen(int w, int h, int depth) {
    free_pixmap_contents(Q.screen_pm);
    make_screen(w, h, depth);
}

/* ---- guest calls: rectangles ---- */

static void h_set_rect(void) {
    qd_write_rect(trap_arg(0), (qd_rect){(int16_t)trap_arg(2), (int16_t)trap_arg(1),
                                         (int16_t)trap_arg(4), (int16_t)trap_arg(3)});
}

static void h_offset_rect(void) {
    uint32_t a = trap_arg(0);
    qd_rect r = qd_read_rect(a);
    int16_t dh = (int16_t)trap_arg(1), dv = (int16_t)trap_arg(2);
    qd_write_rect(a, (qd_rect){(int16_t)(r.top + dv), (int16_t)(r.left + dh),
                               (int16_t)(r.bottom + dv), (int16_t)(r.right + dh)});
}

/* ---- guest calls: color tables and devices ---- */

/* GetCTable(id): a new copy of 'clut' resource id, or of the standard table
   for IDs 1, 2, 4 and 8. NULL otherwise. */
static void h_get_ctable(void) {
    int16_t id = (int16_t)trap_arg(0);
    rsrc_entry *e = rsrc_find(FOURCC('c', 'l', 'u', 't'), id);
    if (e) {
        uint32_t h = mm_new_handle(e->len, false);
        if (!h)
            trap_crash("GetCTable: out of guest memory");
        memcpy(gm_ptr(gm_r32(h), e->len), rsrc_data(e), e->len);
        trap_return(h);
        return;
    }
    if (id == 1 || id == 2 || id == 4 || id == 8) {
        trap_return(std_ctab(id));
        return;
    }
    trap_return(0);
}

static void h_get_main_device(void) { trap_return(Q.main_device); }

/* SetDepth(gd, depth, whichFlags, flags) -> OSErr */
static void h_set_depth(void) {
    uint32_t gd = trap_arg(0);
    int depth = (int16_t)trap_arg(1);
    if (gd != Q.main_device)
        trap_crash("SetDepth: 0x%08x is not the main device", gd);
    if (depth != 8 && depth != 16 && depth != 32)
        trap_crash("SetDepth: depth %d is not supported", depth);
    qd_rect b = qd_read_rect(gm_r32(Q.screen_pm) + PM_BOUNDS);
    resize_screen(rect_w(b), rect_h(b), depth);
    trap_return(QD_NO_ERR);
}

/* ---- guest calls: GWorlds ---- */

/* NewGWorld(GWorldPtr *out, short depth, const Rect *bounds, CTabHandle ctab,
   GDHandle device, GWorldFlags flags) -> QDErr */
static void h_new_gworld(void) {
    uint32_t out = trap_arg(0);
    int depth = (int16_t)trap_arg(1);
    qd_rect b = qd_read_rect(trap_arg(2));
    uint32_t ctab = trap_arg(3);
    if (depth == 0)
        depth = gm_r16(gm_r32(Q.screen_pm) + PM_PIXEL_SIZE);
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16 && depth != 32)
        trap_crash("NewGWorld: depth %d is not supported", depth);
    if (rect_empty(b))
        trap_crash("NewGWorld: empty bounds (%d,%d,%d,%d)", b.top, b.left, b.bottom, b.right);
    uint32_t own_ctab;
    if (ctab && depth <= 8) {
        qd_palette pal;
        qd_read_ctab(ctab, &pal);
        pal.n = 1 << depth;
        own_ctab = qd_new_ctab(&pal);
    } else {
        own_ctab = std_ctab(depth);
    }
    uint32_t pm_h = mm_new_handle(PIXMAP_SIZE, true);
    if (!pm_h)
        trap_crash("NewGWorld: out of guest memory");
    setup_pixmap(pm_h, depth, b, own_ctab);
    gm_w32(out, new_port(pm_h, b, KIND_GWORLD));
    trap_return(QD_NO_ERR);
}

/* UpdateGWorld(GWorldPtr *gw, short depth, const Rect *bounds, CTabHandle ctab,
   GDHandle device, GWorldFlags flags) -> GWorldFlags. Only a no-op update
   (same size and depth) is supported. */
static void h_update_gworld(void) {
    uint32_t gw = gm_r32(trap_arg(0));
    port_info *info = need_port("UpdateGWorld", gw);
    if (info->kind != KIND_GWORLD)
        trap_crash("UpdateGWorld: 0x%08x is not a GWorld", gw);
    int depth = (int16_t)trap_arg(1);
    qd_rect b = qd_read_rect(trap_arg(2));
    uint32_t pm = port_pixmap(gw);
    qd_rect cur = qd_read_rect(pm + PM_BOUNDS);
    int cur_depth = gm_r16(pm + PM_PIXEL_SIZE);
    if ((depth != 0 && depth != cur_depth) || rect_w(b) != rect_w(cur) || rect_h(b) != rect_h(cur))
        trap_crash("UpdateGWorld: changing a %dx%d %d-bit GWorld to %dx%d %d-bit is not supported",
                   rect_w(cur), rect_h(cur), cur_depth, rect_w(b), rect_h(b), depth);
    trap_return(0);
}

static void h_dispose_gworld(void) {
    uint32_t gw = trap_arg(0);
    port_info *info = need_port("DisposeGWorld", gw);
    if (info->kind != KIND_GWORLD)
        trap_crash("DisposeGWorld: 0x%08x is not a GWorld", gw);
    uint32_t pm_h = gm_r32(gw + PORT_PIXMAP);
    free_pixmap_contents(pm_h);
    mm_dispose_handle(pm_h);
    dispose_port(info);
}

static void h_get_gworld_pixmap(void) {
    uint32_t gw = trap_arg(0);
    need_port("GetGWorldPixMap", gw);
    trap_return(gm_r32(gw + PORT_PIXMAP));
}

static void h_lock_pixels(void) { trap_return(1); }
static void h_unlock_pixels(void) {}

static void h_get_pix_base_addr(void) { trap_return(gm_r32(gm_r32(trap_arg(0)) + PM_BASE_ADDR)); }

static void h_set_gworld(void) {
    uint32_t port = trap_arg(0), gd = trap_arg(1);
    need_port("SetGWorld", port);
    Q.cur_port = port;
    Q.cur_device = gd ? gd : Q.main_device;
}

static void h_get_gworld(void) {
    gm_w32(trap_arg(0), Q.cur_port);
    if (trap_arg(1))
        gm_w32(trap_arg(1), Q.cur_device);
}

/* ---- guest calls: ports and windows ---- */

static void h_set_port_window_port(void) {
    uint32_t w = trap_arg(0);
    need_port("SetPortWindowPort", w);
    Q.cur_port = w;
}

static void h_get_window_port(void) {
    need_port("GetWindowPort", trap_arg(0));
    trap_return(trap_arg(0));
}

static void h_get_port_bounds(void) {
    uint32_t port = trap_arg(0), r = trap_arg(1);
    need_port("GetPortBounds", port);
    qd_write_rect(r, qd_read_rect(port + PORT_RECT));
    trap_return(r);
}

static void h_get_window_port_bounds(void) {
    uint32_t w = trap_arg(0), r = trap_arg(1);
    need_port("GetWindowPortBounds", w);
    qd_write_rect(r, qd_read_rect(w + PORT_RECT));
    trap_return(r);
}

static void h_get_port_bitmap_for_copy_bits(void) {
    uint32_t port = trap_arg(0);
    need_port("GetPortBitMapForCopyBits", port);
    trap_return(port_pixmap(port));
}

static void h_get_qd_globals_screen_bits(void) {
    uint32_t out = trap_arg(0), pm = gm_r32(Q.screen_pm);
    gm_w32(out, gm_r32(pm + PM_BASE_ADDR));
    gm_w16(out + 4, gm_r16(pm + PM_ROW_BYTES) & 0x3FFF);
    qd_write_rect(out + 6, qd_read_rect(pm + PM_BOUNDS));
    trap_return(out);
}

static void h_show_window(void) { need_port("ShowWindow", trap_arg(0))->visible = true; Q.dirty = true; }
static void h_hide_window(void) { need_port("HideWindow", trap_arg(0))->visible = false; Q.dirty = true; }

static void h_inval_window_rect(void) {
    need_port("InvalWindowRect", trap_arg(0));
    Q.dirty = true;
}

/* QDFlushPortBuffer(port, region): the screen is presented at the next pump. */
static void h_qd_flush_port_buffer(void) { Q.dirty = true; }

/* BeginFullScreen(Ptr *restoreState, GDHandle gd, short *desiredWidth,
   short *desiredHeight, WindowRef *newWindow, RGBColor *eraseColor, long flags) */
static void h_begin_full_screen(void) {
    uint32_t restore = trap_arg(0), wp = trap_arg(2), hp = trap_arg(3), out = trap_arg(4);
    uint32_t erase = trap_arg(5);
    uint32_t pm = gm_r32(Q.screen_pm);
    qd_rect b = qd_read_rect(pm + PM_BOUNDS);
    Q.saved_w = rect_w(b);
    Q.saved_h = rect_h(b);
    int w = wp ? (int16_t)gm_r16(wp) : 0, h = hp ? (int16_t)gm_r16(hp) : 0;
    if (w > 0 && h > 0 && (w != rect_w(b) || h != rect_h(b))) {
        resize_screen(w, h, gm_r16(pm + PM_PIXEL_SIZE));
        b = qd_read_rect(gm_r32(Q.screen_pm) + PM_BOUNDS);
    }
    if (wp)
        gm_w16(wp, (uint16_t)rect_w(b));
    if (hp)
        gm_w16(hp, (uint16_t)rect_h(b));
    uint32_t win = new_port(Q.screen_pm, b, KIND_WINDOW);
    qd_pixels px;
    qd_palette pal;
    qd_screen(&px, &pal);
    qd_fill(&px, b, b, erase ? read_rgb(erase) : (qd_rgb){0, 0, 0});
    Q.dirty = true;
    if (out)
        gm_w32(out, win);
    if (restore)
        gm_w32(restore, win);
    trap_return(QD_NO_ERR);
}

/* EndFullScreen(Ptr restoreState, long flags): disposes of the window and
   restores the screen size. */
static void h_end_full_screen(void) {
    port_info *info = need_port("EndFullScreen", trap_arg(0));
    dispose_port(info);
    uint32_t pm = gm_r32(Q.screen_pm);
    if (Q.saved_w && Q.saved_h)
        resize_screen(Q.saved_w, Q.saved_h, gm_r16(pm + PM_PIXEL_SIZE));
    trap_return(QD_NO_ERR);
}

/* ---- guest calls: drawing ---- */

static void h_clip_rect(void) {
    uint32_t rgn = gm_r32(Q.cur_port + PORT_CLIP_RGN);
    qd_write_rect(gm_r32(rgn) + 2, qd_read_rect(trap_arg(0)));
}

static void h_rgb_fore_color(void) {
    qd_rgb c = read_rgb(trap_arg(0));
    write_rgb(Q.cur_port + PORT_RGB_FG, c);
}

static void h_paint_rect(void) {
    qd_rect r = qd_read_rect(trap_arg(0));
    qd_pixels px;
    qd_palette pal;
    uint32_t bits = port_pixmap(Q.cur_port);
    qd_bits("PaintRect", bits, &px, &pal);
    qd_fill(&px, r, port_clip("PaintRect", Q.cur_port), read_rgb(Q.cur_port + PORT_RGB_FG));
    if (is_screen_bits(bits))
        Q.dirty = true;
}

/* CopyBits(srcBits, dstBits, srcRect, dstRect, mode, maskRgn) */
static void h_copy_bits(void) {
    uint32_t src_bits = trap_arg(0), dst_bits = trap_arg(1);
    int mode = (int16_t)trap_arg(4);
    if (trap_arg(5))
        trap_crash("CopyBits: mask regions are not supported");
    qd_pixels src, dst;
    qd_palette src_pal, dst_pal;
    qd_bits("CopyBits", src_bits, &src, &src_pal);
    qd_bits("CopyBits", dst_bits, &dst, &dst_pal);
    qd_rect clip = dst.bounds;
    if (dst_bits == port_pixmap(Q.cur_port))
        clip = port_clip("CopyBits", Q.cur_port);
    char err[128];
    if (!qd_blit(&src, qd_read_rect(trap_arg(2)), &dst, qd_read_rect(trap_arg(3)), clip, mode,
                 read_rgb(Q.cur_port + PORT_RGB_FG), read_rgb(Q.cur_port + PORT_RGB_BK), err,
                 sizeof err))
        trap_crash("CopyBits: %s", err);
    if (is_screen_bits(dst_bits))
        Q.dirty = true;
}

static void h_draw_picture(void) {
    uint32_t pic = trap_arg(0);
    if (!mm_is_handle(pic))
        trap_crash("DrawPicture: 0x%08x is not a handle", pic);
    uint32_t len = mm_handle_size(pic);
    const uint8_t *data = gm_ptr(gm_r32(pic), len);
    qd_pixels px;
    qd_palette pal;
    uint32_t bits = port_pixmap(Q.cur_port);
    qd_bits("DrawPicture", bits, &px, &pal);
    char err[128];
    if (!pict_draw(data, len, qd_read_rect(trap_arg(1)), &px, port_clip("DrawPicture", Q.cur_port),
                   read_rgb(Q.cur_port + PORT_RGB_FG), read_rgb(Q.cur_port + PORT_RGB_BK), err,
                   sizeof err))
        trap_crash("DrawPicture: %s", err);
    if (is_screen_bits(bits))
        Q.dirty = true;
}

/* Palette (Palettes.h): pmEntries (2), private fields (14), then 16-byte
   ColorInfos whose first 6 bytes are the RGBColor. The game never makes a
   palette (it imports no call that does), but it keeps these two calls for
   one it might have. */
#define PALETTE_INFO 16
#define COLOR_INFO_SIZE 16

/* GetEntryColor(PaletteHandle, short entry, RGBColor *rgb) */
static void h_get_entry_color(void) {
    uint32_t pal = trap_arg(0), out = trap_arg(2);
    int16_t i = (int16_t)trap_arg(1);
    if (!mm_is_handle(pal))
        trap_crash("GetEntryColor: 0x%08x is not a palette handle", pal);
    uint32_t p = gm_r32(pal), n = gm_r16(p);
    if (i < 0 || (uint32_t)i >= n || PALETTE_INFO + COLOR_INFO_SIZE * (uint32_t)(i + 1) > mm_handle_size(pal))
        trap_crash("GetEntryColor: entry %d is outside the palette (%u entries)", i, n);
    write_rgb(out, read_rgb(p + PALETTE_INFO + COLOR_INFO_SIZE * (uint32_t)i));
}

static void h_dispose_palette(void) {
    uint32_t pal = trap_arg(0);
    if (pal && mm_dispose_handle(pal) != MM_NO_ERR)
        trap_crash("DisposePalette: 0x%08x is not a palette handle", pal);
}

void qd_register(void) {
    trap_register("GetEntryColor", h_get_entry_color);
    trap_register("DisposePalette", h_dispose_palette);
    trap_register("SetRect", h_set_rect);
    trap_register("OffsetRect", h_offset_rect);
    trap_register("GetCTable", h_get_ctable);
    trap_register("GetMainDevice", h_get_main_device);
    trap_register("SetDepth", h_set_depth);
    trap_register("NewGWorld", h_new_gworld);
    trap_register("UpdateGWorld", h_update_gworld);
    trap_register("DisposeGWorld", h_dispose_gworld);
    trap_register("GetGWorldPixMap", h_get_gworld_pixmap);
    trap_register("LockPixels", h_lock_pixels);
    trap_register("UnlockPixels", h_unlock_pixels);
    trap_register("GetPixBaseAddr", h_get_pix_base_addr);
    trap_register("SetGWorld", h_set_gworld);
    trap_register("GetGWorld", h_get_gworld);
    trap_register("SetPortWindowPort", h_set_port_window_port);
    trap_register("GetWindowPort", h_get_window_port);
    trap_register("GetPortBounds", h_get_port_bounds);
    trap_register("GetWindowPortBounds", h_get_window_port_bounds);
    trap_register("GetPortBitMapForCopyBits", h_get_port_bitmap_for_copy_bits);
    trap_register("GetQDGlobalsScreenBits", h_get_qd_globals_screen_bits);
    trap_register("ShowWindow", h_show_window);
    trap_register("HideWindow", h_hide_window);
    trap_register("InvalWindowRect", h_inval_window_rect);
    trap_register("QDFlushPortBuffer", h_qd_flush_port_buffer);
    trap_register("BeginFullScreen", h_begin_full_screen);
    trap_register("EndFullScreen", h_end_full_screen);
    trap_register("ClipRect", h_clip_rect);
    trap_register("RGBForeColor", h_rgb_fore_color);
    trap_register("PaintRect", h_paint_rect);
    trap_register("CopyBits", h_copy_bits);
    trap_register("DrawPicture", h_draw_picture);
}
