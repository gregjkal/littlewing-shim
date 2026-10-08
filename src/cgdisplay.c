#include "cgdisplay.h"

#include <string.h>

#include "guest_mem.h"
#include "misc.h"
#include "qd.h"
#include "trap.h"
#include "util.h"

#define MAX_MODES 16
#define CG_NO_ERR 0           /* kCGErrorSuccess */
#define CG_ILLEGAL_ARG 1001   /* kCGErrorIllegalArgument */

typedef struct {
    int width, height, depth;
} mode;

static struct {
    mode modes[MAX_MODES];
    int nmodes;
    int captured; /* captures not yet released */
} G;

void cgdisplay_init(void) {
    memset(&G, 0, sizeof G);
    qd_set_direct_drawing(false);
}

bool cgdisplay_captured(void) { return G.captured > 0; }

static void need_main(const char *call, uint32_t display) {
    if (display != CGDISPLAY_MAIN)
        trap_crash("%s: 0x%08x is not the main display", call, display);
}

/* The ID of the mode for this size and depth, made on first use. */
static uint32_t mode_ref(const char *call, int width, int height, int depth) {
    int i = 0;
    while (i < G.nmodes && !(G.modes[i].width == width && G.modes[i].height == height &&
                             G.modes[i].depth == depth))
        i++;
    if (i == G.nmodes) {
        if (G.nmodes == MAX_MODES)
            trap_crash("%s: more than %d display modes", call, MAX_MODES);
        G.modes[G.nmodes++] = (mode){width, height, depth};
    }
    return CGDISPLAY_MODE_BASE + 16u * (uint32_t)i;
}

static const mode *need_mode(const char *call, uint32_t ref) {
    uint32_t i = (ref - CGDISPLAY_MODE_BASE) / 16u;
    if (ref < CGDISPLAY_MODE_BASE || (ref & 15u) || i >= (uint32_t)G.nmodes)
        trap_crash("%s: 0x%08x is not a display mode", call, ref);
    return &G.modes[i];
}

/* The screen's PixMap. */
static void screen(qd_pixels *px) {
    qd_palette pal;
    qd_screen(px, &pal);
}

static void h_main_display_id(void) { trap_return(CGDISPLAY_MAIN); }

static void h_pixels_wide(void) {
    need_main("CGDisplayPixelsWide", trap_arg(0));
    qd_pixels px;
    screen(&px);
    trap_return((uint32_t)rect_w(px.bounds));
}

static void h_pixels_high(void) {
    need_main("CGDisplayPixelsHigh", trap_arg(0));
    qd_pixels px;
    screen(&px);
    trap_return((uint32_t)rect_h(px.bounds));
}

static void h_bytes_per_row(void) {
    need_main("CGDisplayBytesPerRow", trap_arg(0));
    qd_pixels px;
    screen(&px);
    trap_return(px.row_bytes);
}

static void h_base_address(void) {
    need_main("CGDisplayBaseAddress", trap_arg(0));
    trap_return(qd_screen_base());
}

static void h_current_mode(void) {
    need_main("CGDisplayCurrentMode", trap_arg(0));
    qd_pixels px;
    screen(&px);
    trap_return(mode_ref("CGDisplayCurrentMode", rect_w(px.bounds), rect_h(px.bounds), px.depth));
}

/* CGDisplayBestModeForParameters(display, size_t bitsPerPixel, size_t width,
   size_t height, boolean_t *exactMatch) -> CFDictionaryRef. The emulated
   screen takes any size, so the best mode is always the one asked for. */
static void h_best_mode_for_parameters(void) {
    need_main("CGDisplayBestModeForParameters", trap_arg(0));
    uint32_t depth = trap_arg(1), width = trap_arg(2), height = trap_arg(3), exact = trap_arg(4);
    if (depth != 8 && depth != 16 && depth != 32)
        trap_crash("CGDisplayBestModeForParameters: %u bits per pixel is not supported", depth);
    if (width < 1 || width > 4096 || height < 1 || height > 4096)
        trap_crash("CGDisplayBestModeForParameters: a %ux%u display is not supported", width, height);
    if (exact)
        gm_w32(exact, 1);
    trap_return(mode_ref("CGDisplayBestModeForParameters", (int)width, (int)height, (int)depth));
}

static void h_switch_to_mode(void) {
    need_main("CGDisplaySwitchToMode", trap_arg(0));
    const mode *m = need_mode("CGDisplaySwitchToMode", trap_arg(1));
    log_msg("display mode %dx%d, %d bits", m->width, m->height, m->depth);
    qd_resize_screen(m->width, m->height, m->depth);
    trap_return(CG_NO_ERR);
}

static void h_capture(void) {
    need_main("CGDisplayCapture", trap_arg(0));
    G.captured++;
    qd_set_direct_drawing(true);
    trap_return(CG_NO_ERR);
}

/* CGDisplayRelease: ends one capture. Releasing a display that isn't
   captured is an illegal argument, and changes nothing. */
static void h_release(void) {
    need_main("CGDisplayRelease", trap_arg(0));
    if (!G.captured) {
        trap_return(CG_ILLEGAL_ARG);
        return;
    }
    if (--G.captured == 0)
        qd_set_direct_drawing(false);
    trap_return(CG_NO_ERR);
}

static void h_is_captured(void) {
    need_main("CGDisplayIsCaptured", trap_arg(0));
    trap_return(G.captured > 0);
}

static void h_hide_cursor(void) {
    need_main("CGDisplayHideCursor", trap_arg(0));
    misc_hide_cursor();
    trap_return(CG_NO_ERR);
}

static void h_show_cursor(void) {
    need_main("CGDisplayShowCursor", trap_arg(0));
    misc_show_cursor();
    trap_return(CG_NO_ERR);
}

void cgdisplay_register(void) {
    trap_register("CGMainDisplayID", h_main_display_id);
    trap_register("CGDisplayPixelsWide", h_pixels_wide);
    trap_register("CGDisplayPixelsHigh", h_pixels_high);
    trap_register("CGDisplayBytesPerRow", h_bytes_per_row);
    trap_register("CGDisplayBaseAddress", h_base_address);
    trap_register("CGDisplayCurrentMode", h_current_mode);
    trap_register("CGDisplayBestModeForParameters", h_best_mode_for_parameters);
    trap_register("CGDisplaySwitchToMode", h_switch_to_mode);
    trap_register("CGDisplayCapture", h_capture);
    trap_register("CGDisplayRelease", h_release);
    trap_register("CGDisplayIsCaptured", h_is_captured);
    trap_register("CGDisplayHideCursor", h_hide_cursor);
    trap_register("CGDisplayShowCursor", h_show_cursor);
}
