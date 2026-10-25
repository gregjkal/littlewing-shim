#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "blit.h"

/* QuickDraw subset. The structures the game can see live in guest memory in
   their original big-endian layout (Inside Macintosh: Imaging With
   QuickDraw): Rects, PixMaps (in handles), CGrafPorts, the main GDevice and
   color tables. A WindowRef is the address of the window's CGrafPort.
   Windows have no frame: a window is its content, at a place on the
   screen, and its port's PixMap is the screen's pixels with bounds that
   put the window's (0, 0) there. HD mode follows the screen's pixels at
   the screen's own bounds, so it is right only for windows at the origin,
   which every game with HD art has.
   Drawing is clipped to the port's portRect and clip region, which may be
   any shape (region.h), and CopyBits to its mask region too. */

/* Rect: top, left, bottom, right (int16 each). */
#define RECT_SIZE 8

/* PixMap, 50 bytes. */
#define PM_BASE_ADDR   0
#define PM_ROW_BYTES   4  /* high bits 0x8000 mark a PixMap rather than a BitMap */
#define PM_BOUNDS      6
#define PM_VERSION     14
#define PM_PACK_TYPE   16
#define PM_PACK_SIZE   18
#define PM_HRES        22
#define PM_VRES        26
#define PM_PIXEL_TYPE  30 /* 0 indexed, 16 (RGBDirect) direct */
#define PM_PIXEL_SIZE  32
#define PM_CMP_COUNT   34
#define PM_CMP_SIZE    36
#define PM_PIXEL_FORMAT 38
#define PM_TABLE       42 /* CTabHandle */
#define PM_EXT         46
#define PIXMAP_SIZE    50

/* CGrafPort, 108 bytes. */
#define PORT_DEVICE    0
#define PORT_PIXMAP    2  /* PixMapHandle */
#define PORT_VERSION   6  /* 0xC000 for a color port */
#define PORT_RECT      16
#define PORT_VIS_RGN   24
#define PORT_CLIP_RGN  28
#define PORT_RGB_FG    36
#define PORT_RGB_BK    42
#define PORT_BK_PIXPAT 32 /* the background pattern (pen.c): a handle to its 8 bytes, or 0 for white */
#define PORT_PN_LOC    48 /* Point: v, h */
#define PORT_PN_SIZE   52
#define PORT_PN_MODE   56
#define PORT_PN_PIXPAT 58 /* the pen pattern, as PORT_BK_PIXPAT's, or 0 for black */
#define PORT_PN_VIS    66
#define PORT_TX_FONT   68
#define PORT_TX_FACE   70 /* a Style byte */
#define PORT_TX_MODE   72
#define PORT_TX_SIZE   74
#define PORT_SP_EXTRA  76 /* Fixed: added to each space's width */
#define PORT_FG_COLOR  80
#define PORT_BK_COLOR  84
#define CGRAFPORT_SIZE 108

/* QDGlobals, 206 bytes: a classic program's own, whose last field's
   address it passes to InitGraf. InitGraf leaves arrow, the Cursor at 94,
   zero: the shim has no cursor data, and no game reads it. */
#define QDG_RAND_SEED   76
#define QDG_SCREEN_BITS 80  /* a BitMap: baseAddr, rowBytes, bounds */
#define QDG_PATTERNS    162 /* dkGray, ltGray, gray, black, white */
#define QDG_THE_PORT    202
#define QDGLOBALS_SIZE  206

/* GDevice, 62 bytes. */
#define GD_TYPE  4  /* 0 CLUT, 2 direct */
#define GD_FLAGS 20
#define GD_PMAP  22
#define GD_RECT  34
#define GDEVICE_SIZE 62

/* ColorTable: ctSeed (4), ctFlags (2), ctSize (count - 1, 2), then 8-byte
   ColorSpecs: value, red, green, blue. */
#define CTAB_HEADER 8

#define QD_NO_ERR 0
#define QD_PARAM_ERR (-50)

qd_rect qd_read_rect(uint32_t addr);
void qd_write_rect(uint32_t addr, qd_rect r);

/* RGBColor: red, green, blue (uint16 each). */
qd_rgb qd_read_rgb(uint32_t addr);
void qd_write_rgb(uint32_t addr, qd_rgb c);

/* Reads a CTabHandle into pal. */
void qd_read_ctab(uint32_t ctab, qd_palette *pal);
/* A new CTabHandle holding pal (pal->n entries, values 0..n-1). */
uint32_t qd_new_ctab(const qd_palette *pal);

/* Creates the main screen (GDevice, PixMap and pixels in the guest heap) at
   width x height and depth (8, 16 or 32), and makes it the current port and
   device. Requires mm_init() and rsrc_open(). */
void qd_init(int width, int height, int depth);

uint32_t qd_main_device(void);
/* Describes a BitMap or PixMap (a pointer, not a handle). pal receives the
   colors for indexed depths. Crashes on an unsupported pixel format. */
void qd_bits(const char *call, uint32_t bits, qd_pixels *out, qd_palette *pal);
/* The screen's pixels, for display. */
void qd_screen(qd_pixels *out, qd_palette *pal);

/* Notes that something drew to the screen outside QuickDraw (dialogs). */
void qd_mark_dirty(void);

/* True if anything drew to the screen since the last call, and always
   while direct drawing is on. */
bool qd_take_dirty(void);

/* Pads every new GWorld's rows as System 7.1's NewGWorld did: rounded up
   to 16 bytes, and 16 more. Angel Egg's data files hold images at those
   strides (a 384-pixel 8-bit row is 400 bytes, a 16-bit one 784) and read
   them straight into GWorld pixels, so they need it. Off by default: the
   other games size buffers of their own from a GWorld's width and copy by
   its rowBytes, and change if their rows grow. */
void qd_pad_gworld_rows(bool on);

/* A game is drawing into the screen's pixels itself (a captured display),
   so QuickDraw can't tell when they change. */
void qd_set_direct_drawing(bool on);

/* The guest address of the screen's pixels. */
uint32_t qd_screen_base(void);

/* Makes the screen width x height at depth (8, 16 or 32), with new zeroed
   pixels. The GDevice, the PixMap handle and every port keep their
   addresses; window ports take the new size. */
void qd_resize_screen(int width, int height, int depth);
uint32_t qd_current_port(void);

/* A new window port, hidden, at the screen's origin, whose portRect is
   (0, 0, height, width) until the screen changes size, when it takes the
   screen's. */
uint32_t qd_new_window(int width, int height);

/* What happened to a window, for the code that draws it (dialogs.c draws
   nib windows). arg is RepositionWindow's method. Called before a disposed
   window's port goes away. */
typedef enum {
    QD_WINDOW_SHOWN,
    QD_WINDOW_HIDDEN,
    QD_WINDOW_DISPOSED,
    QD_WINDOW_REPOSITIONED,
    QD_WINDOW_SELECTED, /* only for a dialog hook's DialogRef */
} qd_window_change;
/* The frontmost shown window that GetNewCWindow made and that needs
   drawing (an update event: it was shown, or PaintBehind), until
   BeginUpdate; 0 if none. Other windows draw themselves. */
uint32_t qd_pending_update(void);

typedef void (*qd_window_fn)(uint32_t window, qd_window_change change, uint32_t arg);
void qd_set_window_hook(qd_window_fn fn);

/* A classic DialogRef (GetNewDialog's) is not a port here: dialogs.c draws
   the dialog on the screen itself. ShowWindow, HideWindow and SelectWindow
   on something that isn't a port ask this hook (QD_WINDOW_SHOWN,
   QD_WINDOW_HIDDEN, QD_WINDOW_SELECTED), which returns true if ref is a
   dialog of its; otherwise the call crashes, as on any other non-window. */
typedef bool (*qd_dialog_fn)(uint32_t ref, qd_window_change change);
void qd_set_dialog_hook(qd_dialog_fn fn);

/* The front visible window (FrontWindow), or 0; and whether w is a window
   that hasn't been disposed of. */
uint32_t qd_front_window(void);
bool qd_is_window(uint32_t w);

/* For the modules that draw into ports (pen.c): calls fn with the current
   port's pixels and each rectangle drawing there is clipped to (its
   portRect, within its clip region, and within the region mask unless it
   is 0), then notes the screen changed if the pixels are its. call names
   the drawing call in a crash. */
typedef void (*qd_draw_fn)(const qd_pixels *px, qd_rect clip, void *ctx);
void qd_draw(const char *call, uint32_t mask, qd_draw_fn fn, void *ctx);

/* Registers the QuickDraw, GWorld and window imports, including
   CreateNewWindow (the first one sizes the screen to its content, and every
   one is at the origin), CreateCustomWindow (at its bounds),
   ChangeWindowAttributes and SetWindowTitleWithCFString. */
void qd_register(void);
