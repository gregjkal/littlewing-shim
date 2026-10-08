#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "blit.h"

/* QuickDraw subset. The structures the game can see live in guest memory in
   their original big-endian layout (Inside Macintosh: Imaging With
   QuickDraw): Rects, PixMaps (in handles), CGrafPorts, the main GDevice and
   color tables. A WindowRef is the address of the window's CGrafPort. */

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
#define PORT_PN_SIZE   52
#define PORT_PN_MODE   56
#define PORT_PN_VIS    66
#define PORT_FG_COLOR  80
#define PORT_BK_COLOR  84
#define CGRAFPORT_SIZE 108

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

/* True if anything drew to the screen since the last call. */
bool qd_take_dirty(void);
uint32_t qd_current_port(void);

/* A new window port, hidden, whose portRect is (0, 0, height, width). It
   shares the screen's pixels, as every window does. */
uint32_t qd_new_window(int width, int height);

/* What happened to a window, for the code that draws it (dialogs.c draws
   nib windows). arg is RepositionWindow's method. Called before a disposed
   window's port goes away. */
typedef enum { QD_WINDOW_SHOWN, QD_WINDOW_HIDDEN, QD_WINDOW_DISPOSED, QD_WINDOW_REPOSITIONED } qd_window_change;
typedef void (*qd_window_fn)(uint32_t window, qd_window_change change, uint32_t arg);
void qd_set_window_hook(qd_window_fn fn);

/* Registers the QuickDraw, GWorld and window imports. */
void qd_register(void);
