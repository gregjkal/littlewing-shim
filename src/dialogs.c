#include "dialogs.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cf.h"
#include "cgimage.h"
#include "events.h"
#include "font.h"
#include "guest_mem.h"
#include "keymap.h"
#include "memmgr.h"
#include "misc.h"
#include "nib.h"
#include "pict.h"
#include "qd.h"
#include "rsrc.h"
#include "trap.h"
#include "util.h"

#define MAX_ITEMS 16
#define MAX_TEXT 255  /* what typing can put in an edit field */
#define TEXT_CAP 511  /* the longest item text: nib and standard alert texts are longer */
#define FRAME 4 /* the border drawn outside a dialog's rectangle */

/* A nib image view (not a DITL type): a CGImage drawn in its frame. */
#define ITEM_IMAGE_VIEW 100

/* Control refs (HIViewRef, ControlRef) are opaque IDs: CONTROL_TAG_BASE +
   256 * dialog slot + item index, and ROOT_VIEW for the window's root view.
   IBNibRefs are NIB_TAG_BASE + 16 * slot. */
#define CONTROL_TAG_BASE (DLG_TAG_BASE + 0x40000u)
#define ROOT_VIEW 0xFF
#define NIB_TAG_BASE (DLG_TAG_BASE + 0x80000u)
#define NIB_MAX 4

/* Mac OS errors. */
#define NO_ERR 0
#define PARAM_ERR (-50)
#define FNF_ERR (-43)
#define ERR_UNKNOWN_CONTROL (-30584)

/* HICommand IDs. */
#define CMD_OK FOURCC('o', 'k', ' ', ' ')
#define CMD_CANCEL FOURCC('n', 'o', 't', '!')

/* Window positions (the ALRT and DLOG position word). */
#define POS_CENTER_MAIN 0x280A
#define POS_ALERT_MAIN 0x300A

/* Mac virtual key codes. */
#define VK_RETURN 0x24
#define VK_ENTER 0x4C
#define VK_TAB 0x30
#define VK_DELETE 0x33
#define VK_ESCAPE 0x35
#define VK_PERIOD 0x2F

typedef struct {
    uint8_t type; /* without DLG_ITEM_DISABLED */
    bool disabled;
    qd_rect r;    /* local to the dialog */
    uint8_t text[TEXT_CAP + 1]; /* button title, static text, an edit field's contents */
    int len;
    int16_t res_id;  /* pictures and icons */
    uint32_t handle; /* edit and static text: a guest handle holding the text */
    /* nib controls */
    uint32_t command;   /* a button's HICommand ID, 0 if none */
    uint32_t signature; /* the ControlID */
    int32_t id;
    bool hidden;        /* HIViewSetVisible(false) */
    uint32_t image;     /* an image view's CGImage (retained), 0 if none */
    bool scale_to_fit;  /* an image view's: otherwise drawn at its size, at the top left */
} item;

typedef enum {
    KIND_DLOG,      /* GetNewDialog */
    KIND_ALERT,     /* Alert, StopAlert */
    KIND_NIB,       /* CreateWindowFromNib */
    KIND_STD_ALERT, /* CreateStandardAlert */
} dialog_kind;

typedef struct {
    bool open;
    dialog_kind kind;
    int16_t res_id; /* the ALRT or DLOG */
    char name[64];  /* a nib window's name in the nib */
    uint32_t window; /* a nib window's port */
    bool quit_modal; /* QuitAppModalLoopForWindow was called */
    qd_rect bounds; /* on the screen */
    item items[MAX_ITEMS];
    int nitems;
    int default_item, cancel_item; /* 1-based, 0 = none */
    int focus;      /* the edit field with the caret (0-based), -1 = none */
    int pressed;    /* the button under a held mouse button (0-based), -1 = none */
    int hit;        /* the item ModalDialog or Alert returns (1-based), 0 = none yet */
    bool shown;
    unsigned order; /* when it opened: ModalDialog runs the newest */
    uint8_t *saved; /* the screen under the dialog and its frame, at saved_r */
    qd_rect saved_r, saved_screen;
    int saved_depth;
} dialog;

static struct {
    char param[4][256]; /* ParamText ^0..^3 */
    bool auto_alerts;
    dialog d[DLG_MAX];
    dialog *front; /* the one taking input */
    unsigned opened; /* dialogs opened so far */
    qd_palette pal;
    char *nibs[NIB_MAX]; /* open IBNibRefs: objects.xib's text */
    size_t nib_len[NIB_MAX];
} G;

static void window_changed(uint32_t window, qd_window_change change, uint32_t arg);

void dialogs_init(void) {
    for (int i = 0; i < DLG_MAX; i++)
        free(G.d[i].saved);
    for (int i = 0; i < NIB_MAX; i++)
        free(G.nibs[i]);
    memset(&G, 0, sizeof G);
    const char *a = getenv("LOONY_AUTO_ALERTS");
    G.auto_alerts = a && strcmp(a, "1") == 0;
    font_init();
    qd_set_window_hook(window_changed);
}

/* ---- text ---- */

/* Appends text to out, replacing ^0..^3 with the ParamText strings and
   carriage returns with spaces. */
static void append(char *out, size_t cap, const uint8_t *text, size_t n) {
    size_t o = strlen(out);
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        if (text[i] == '^' && i + 1 < n && text[i + 1] >= '0' && text[i + 1] <= '3') {
            const char *p = G.param[text[++i] - '0'];
            while (*p && o + 1 < cap)
                out[o++] = *p++;
        } else {
            out[o++] = text[i] == '\r' ? ' ' : (char)text[i];
        }
    }
    out[o] = '\0';
}

/* Like append, but keeps carriage returns and spells the result in ASCII. */
static void display_text(const item *it, char *out, size_t cap) {
    uint8_t mac[1024];
    size_t o = 0;
    for (int i = 0; i < it->len && o < sizeof mac; i++) {
        if (it->text[i] == '^' && i + 1 < it->len && it->text[i + 1] >= '0' && it->text[i + 1] <= '3') {
            for (const char *p = G.param[it->text[++i] - '0']; *p && o < sizeof mac; p++)
                mac[o++] = (uint8_t)*p;
        } else {
            mac[o++] = it->text[i];
        }
    }
    font_ascii(mac, o, out, cap);
}

void dialogs_alert_text(int16_t id, char *out, size_t cap) {
    out[0] = '\0';
    rsrc_entry *alrt = rsrc_find(FOURCC('A', 'L', 'R', 'T'), id);
    if (!alrt || alrt->len < 12)
        return;
    rsrc_entry *ditl = rsrc_find(FOURCC('D', 'I', 'T', 'L'), (int16_t)rd_be16(rsrc_data(alrt) + 8));
    if (!ditl || ditl->len < 2)
        return;
    const uint8_t *d = rsrc_data(ditl);
    uint32_t n = rd_be16(d) + 1u, p = 2;
    for (uint32_t i = 0; i < n; i++) {
        if (p + 14 > ditl->len)
            return;
        uint8_t type = d[p + 12] & 0x7F, len = d[p + 13];
        p += 14;
        if (p + len > ditl->len)
            return;
        if (type == DLG_ITEM_STATIC_TEXT || type == DLG_ITEM_BUTTON) {
            if (out[0])
                append(out, cap, (const uint8_t *)" | ", 3);
            append(out, cap, d + p, len);
        }
        p += len + (len & 1u);
    }
}

/* ---- building a dialog from its resources ---- */

static qd_rect be_rect(const uint8_t *p) {
    return (qd_rect){(int16_t)rd_be16(p), (int16_t)rd_be16(p + 2), (int16_t)rd_be16(p + 4),
                     (int16_t)rd_be16(p + 6)};
}

static qd_rect offset(qd_rect r, int dx, int dy) {
    return (qd_rect){(int16_t)(r.top + dy), (int16_t)(r.left + dx), (int16_t)(r.bottom + dy),
                     (int16_t)(r.right + dx)};
}

static qd_rect outset(qd_rect r, int n) {
    return (qd_rect){(int16_t)(r.top - n), (int16_t)(r.left - n), (int16_t)(r.bottom + n),
                     (int16_t)(r.right + n)};
}

static qd_pixels screen(void) {
    qd_pixels px;
    qd_screen(&px, &G.pal);
    px.pal = &G.pal;
    return px;
}

/* Places r on the screen as the position word asks. */
static qd_rect place(qd_rect r, uint16_t pos) {
    qd_rect s = screen().bounds;
    int w = rect_w(r), h = rect_h(r);
    if (pos == POS_CENTER_MAIN)
        return offset(r, s.left + (rect_w(s) - w) / 2 - r.left, s.top + (rect_h(s) - h) / 2 - r.top);
    if (pos == POS_ALERT_MAIN)
        return offset(r, s.left + (rect_w(s) - w) / 2 - r.left, s.top + (rect_h(s) - h) / 3 - r.top);
    return r;
}

static bool is_control(uint8_t type) { return type >= DLG_ITEM_BUTTON && type <= DLG_ITEM_RADIO; }
static bool has_text_handle(uint8_t type) {
    return type == DLG_ITEM_EDIT_TEXT || type == DLG_ITEM_STATIC_TEXT;
}

static void set_handle_text(item *it) {
    if (mm_set_handle_size(it->handle, (uint32_t)it->len) != MM_NO_ERR)
        trap_crash("out of guest memory for dialog text");
    if (it->len)
        memcpy(gm_ptr(gm_r32(it->handle), (uint32_t)it->len), it->text, (size_t)it->len);
}

/* Reads DITL id into d. */
static void load_items(const char *call, dialog *d, int16_t ditl_id) {
    rsrc_entry *e = rsrc_find(FOURCC('D', 'I', 'T', 'L'), ditl_id);
    if (!e || e->len < 2)
        trap_crash("%s: DITL %d doesn't exist", call, ditl_id);
    const uint8_t *p = rsrc_data(e);
    uint32_t n = rd_be16(p) + 1u, at = 2;
    if (n > MAX_ITEMS)
        trap_crash("%s: DITL %d has %u items (at most %d supported)", call, ditl_id, n, MAX_ITEMS);
    for (uint32_t i = 0; i < n; i++) {
        if (at + 14 > e->len)
            trap_crash("%s: DITL %d is truncated", call, ditl_id);
        item *it = &d->items[i];
        it->r = be_rect(p + at + 4);
        it->type = p[at + 12] & 0x7F;
        it->disabled = (p[at + 12] & DLG_ITEM_DISABLED) != 0;
        uint8_t len = p[at + 13];
        at += 14;
        if (at + len > e->len)
            trap_crash("%s: DITL %d is truncated", call, ditl_id);
        if (it->type == DLG_ITEM_ICON || it->type == DLG_ITEM_PICTURE) {
            it->res_id = len >= 2 ? (int16_t)rd_be16(p + at) : 0;
        } else {
            memcpy(it->text, p + at, len);
            it->len = len;
        }
        if (has_text_handle(it->type)) {
            it->handle = mm_new_handle(0, false);
            if (!it->handle)
                trap_crash("%s: out of guest memory", call);
            set_handle_text(it);
        }
        at += len + (len & 1u);
    }
    d->nitems = (int)n;
    d->focus = -1;
    d->pressed = -1;
    for (int i = 0; i < d->nitems && d->focus < 0; i++)
        if (d->items[i].type == DLG_ITEM_EDIT_TEXT)
            d->focus = i;
    for (int i = 0; i < d->nitems; i++)
        if (d->items[i].type == DLG_ITEM_BUTTON && d->items[i].len == 6 &&
            strncasecmp((const char *)d->items[i].text, "Cancel", 6) == 0)
            d->cancel_item = i + 1;
}

static dialog *new_dialog(const char *call) {
    for (int i = 0; i < DLG_MAX; i++)
        if (!G.d[i].open) {
            memset(&G.d[i], 0, sizeof G.d[i]);
            G.d[i].open = true;
            G.d[i].order = ++G.opened;
            return &G.d[i];
        }
    trap_crash("%s: more than %d dialogs open", call, DLG_MAX);
}

/* ---- drawing ---- */

static const qd_rgb BLACK = {0, 0, 0}, WHITE = {0xFFFF, 0xFFFF, 0xFFFF}, GRAY = {0x8000, 0x8000, 0x8000};

static void fill(qd_rect r, qd_rgb c) {
    qd_pixels px = screen();
    qd_fill(&px, r, px.bounds, c);
}

static void frame(qd_rect r, int thick, qd_rgb c) {
    fill((qd_rect){r.top, r.left, (int16_t)(r.top + thick), r.right}, c);
    fill((qd_rect){(int16_t)(r.bottom - thick), r.left, r.bottom, r.right}, c);
    fill((qd_rect){r.top, r.left, r.bottom, (int16_t)(r.left + thick)}, c);
    fill((qd_rect){r.top, (int16_t)(r.right - thick), r.bottom, r.right}, c);
}

static void text_at(int x, int y, const char *s, int n, qd_rgb c, qd_rect clip) {
    qd_pixels px = screen();
    font_draw(&px, x, y, s, n, c, clip);
}

static void draw_picture(qd_rect r, int16_t id) {
    rsrc_entry *e = rsrc_find(FOURCC('P', 'I', 'C', 'T'), id);
    if (!e)
        return;
    qd_pixels px = screen();
    char err[128];
    if (!pict_draw(rsrc_data(e), e->len, r, &px, r, BLACK, WHITE, err, sizeof err))
        log_msg("dialog: can't draw PICT %d: %s", id, err);
}

/* A black-and-white 'ICON' (32x32, 1 bit). */
static void draw_icon(qd_rect r, int16_t id) {
    rsrc_entry *e = rsrc_find(FOURCC('I', 'C', 'O', 'N'), id);
    if (!e || e->len < 128)
        return;
    const uint8_t *bits = rsrc_data(e);
    for (int y = 0; y < 32 && r.top + y < r.bottom; y++)
        for (int x = 0; x < 32 && r.left + x < r.right; x++)
            if (bits[y * 4 + x / 8] & (0x80 >> (x % 8)))
                fill((qd_rect){(int16_t)(r.top + y), (int16_t)(r.left + x), (int16_t)(r.top + y + 1),
                               (int16_t)(r.left + x + 1)},
                     BLACK);
}

/* An image view's CGImage: scaled to r, or at its own size at r's top left. */
static void draw_image(const item *it, qd_rect r) {
    const cgimage_pixels *img = cgimage_get(it->image);
    if (!img)
        return;
    qd_pixels src = {img->xrgb, (uint32_t)img->width * 4u,
                     {0, 0, (int16_t)img->height, (int16_t)img->width}, 32, NULL};
    qd_rect to = it->scale_to_fit ? r : offset(src.bounds, r.left, r.top);
    qd_pixels px = screen();
    char err[128];
    if (!qd_blit(&src, src.bounds, &px, to, r, QD_SRC_COPY, BLACK, WHITE, err, sizeof err))
        log_msg("dialog: can't draw an image view: %s", err);
}

/* The height static text item i can take: down to the nearest visible item
   below it (clear of a default button's outline), or the dialog's bottom. */
static int text_room(const dialog *d, int i) {
    qd_rect r = d->items[i].r;
    int bottom = rect_h(d->bounds) - 2;
    for (int k = 0; k < d->nitems; k++) {
        qd_rect o = d->items[k].r;
        if (k != i && !d->items[k].hidden && o.top > r.top && o.left < r.right && o.right > r.left &&
            o.top - 6 < bottom)
            bottom = o.top - 6;
    }
    return bottom - r.top - 2;
}

static void draw_item(dialog *d, int i) {
    item *it = &d->items[i];
    if (it->hidden)
        return;
    qd_rect r = offset(it->r, d->bounds.left, d->bounds.top);
    char text[1024];
    display_text(it, text, sizeof text);
    switch (it->type) {
    case DLG_ITEM_BUTTON:
    case DLG_ITEM_CHECKBOX:
    case DLG_ITEM_RADIO: {
        bool down = d->pressed == i;
        if (d->default_item == i + 1)
            frame(outset(r, 4), 3, BLACK);
        fill(r, down ? BLACK : WHITE);
        frame(r, 1, BLACK);
        int n = (int)strlen(text);
        if (n * FONT_W > rect_w(r) - 4)
            n = rect_w(r) > 4 ? (rect_w(r) - 4) / FONT_W : 0;
        int x = r.left + (rect_w(r) - n * FONT_W) / 2, y = r.top + (rect_h(r) - FONT_H) / 2;
        qd_rgb ink = down ? WHITE : it->disabled ? GRAY : BLACK;
        text_at(x, y, text, n, ink, r);
        break;
    }
    case DLG_ITEM_STATIC_TEXT: {
        int starts[32], lens[32];
        int n = font_wrap(text, rect_w(it->r), 32, starts, lens), line = FONT_LINE;
        /* A nib's texts were laid out in small system fonts. One that runs
           into the item below it gets closer lines, as far as the 8x8 font
           allows. */
        if (d->kind == KIND_NIB) {
            int room = text_room(d, i);
            if (n * line > room)
                line = room / n > FONT_H + 1 ? room / n : FONT_H + 1;
        }
        for (int k = 0; k < n && k < 32; k++)
            text_at(r.left, r.top + 2 + k * line, text + starts[k], lens[k], BLACK, d->bounds);
        break;
    }
    case DLG_ITEM_EDIT_TEXT: {
        qd_rect box = outset(r, 3);
        fill(box, WHITE);
        frame(box, 1, BLACK);
        int room = rect_w(r) > 2 ? (rect_w(r) - 2) / FONT_W : 0, n = (int)strlen(text);
        const char *tail = n > room ? text + (n - room) : text; /* the end, where typing happens */
        int sn = n > room ? room : n;
        int y = r.top + (rect_h(r) - FONT_H) / 2;
        text_at(r.left, y, tail, sn, BLACK, r);
        if (d->focus == i)
            fill((qd_rect){(int16_t)(y - 1), (int16_t)(r.left + sn * FONT_W),
                           (int16_t)(y + FONT_H + 1), (int16_t)(r.left + sn * FONT_W + 1)},
                 BLACK);
        break;
    }
    case DLG_ITEM_PICTURE: draw_picture(r, it->res_id); break;
    case DLG_ITEM_ICON: draw_icon(r, it->res_id); break;
    case ITEM_IMAGE_VIEW: draw_image(it, r); break;
    default: break;
    }
    qd_mark_dirty();
}

/* Draws the dialog's box and items over what's there. */
static void draw_all(dialog *d) {
    fill(outset(d->bounds, FRAME), BLACK);
    frame(outset(d->bounds, FRAME - 1), 1, WHITE);
    fill(d->bounds, WHITE);
    for (int i = 0; i < d->nitems; i++)
        draw_item(d, i);
    qd_mark_dirty();
}

/* Saves what's under the dialog, then draws it. */
static void show(dialog *d) {
    qd_pixels px = screen();
    qd_rect r = rect_sect(outset(d->bounds, FRAME), px.bounds);
    d->shown = true;
    d->saved_r = r;
    d->saved_screen = px.bounds;
    d->saved_depth = px.depth;
    int bpp = px.depth / 8;
    if (px.depth < 8 || rect_empty(r)) {
        d->saved = NULL;
    } else {
        size_t row = (size_t)rect_w(r) * (size_t)bpp;
        d->saved = malloc(row * (size_t)rect_h(r));
        if (!d->saved)
            fatal("out of memory");
        for (int y = r.top; y < r.bottom; y++)
            memcpy(d->saved + (size_t)(y - r.top) * row,
                   px.base + (size_t)(y - px.bounds.top) * px.row_bytes + (size_t)(r.left - px.bounds.left) * (size_t)bpp,
                   row);
    }
    draw_all(d);
}

/* Puts the screen back as it was before show(), unless the screen has
   changed size or depth since. */
static void hide(dialog *d) {
    qd_pixels px = screen();
    qd_rect r = d->saved_r;
    if (d->saved && px.depth == d->saved_depth && memcmp(&px.bounds, &d->saved_screen, sizeof r) == 0) {
        int bpp = px.depth / 8;
        size_t row = (size_t)rect_w(r) * (size_t)bpp;
        for (int y = r.top; y < r.bottom; y++)
            memcpy(px.base + (size_t)(y - px.bounds.top) * px.row_bytes + (size_t)(r.left - px.bounds.left) * (size_t)bpp,
                   d->saved + (size_t)(y - r.top) * row, row);
    }
    free(d->saved);
    d->saved = NULL;
    d->shown = false;
    qd_mark_dirty();
}

static void close_dialog(dialog *d) {
    hide(d);
    for (int i = 0; i < d->nitems; i++) {
        if (d->items[i].handle)
            mm_dispose_handle(d->items[i].handle);
        cgimage_release(d->items[i].image);
    }
    if (G.front == d)
        G.front = NULL;
    d->open = false;
}

/* ---- input ---- */

static bool inside(qd_rect r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

static int item_at(dialog *d, int x, int y) {
    for (int i = d->nitems - 1; i >= 0; i--) {
        qd_rect r = offset(d->items[i].r, d->bounds.left, d->bounds.top);
        if (d->items[i].type == DLG_ITEM_EDIT_TEXT)
            r = outset(r, 3);
        if (!d->items[i].hidden && inside(r, x, y))
            return i;
    }
    return -1;
}

static void press(dialog *d, int item1) {
    if (item1 > 0 && item1 <= d->nitems && !d->items[item1 - 1].disabled && !d->items[item1 - 1].hidden)
        d->hit = item1;
}

static void set_focus(dialog *d, int i) {
    int old = d->focus;
    d->focus = i;
    if (old >= 0)
        draw_item(d, old);
    if (i >= 0)
        draw_item(d, i);
}

static void type_char(dialog *d, uint8_t c) {
    if (d->focus < 0)
        return;
    item *it = &d->items[d->focus];
    if (c == 0x08) {
        if (it->len > 0)
            it->len--;
    } else if (it->len < MAX_TEXT) {
        it->text[it->len++] = c;
    }
    if (it->handle)
        set_handle_text(it);
    draw_item(d, d->focus);
}

static void on_key(uint32_t vkey, uint8_t chr, uint32_t mods) {
    dialog *d = G.front;
    if (!d)
        return;
    bool cmd = (mods & KM_CMD) != 0;
    if (vkey == VK_RETURN || vkey == VK_ENTER) {
        press(d, d->default_item);
    } else if (vkey == VK_ESCAPE || (cmd && vkey == VK_PERIOD)) {
        press(d, d->cancel_item);
    } else if (vkey == VK_TAB) {
        int n = d->nitems, step = (mods & KM_SHIFT) ? n - 1 : 1;
        for (int k = 1, i = d->focus; k <= n && d->focus >= 0; k++) {
            i = (i + step) % n;
            if (d->items[i].type == DLG_ITEM_EDIT_TEXT && !d->items[i].hidden) {
                set_focus(d, i);
                break;
            }
        }
    } else if (vkey == VK_DELETE) {
        type_char(d, 0x08);
    } else if (!cmd && chr >= 0x20 && chr < 0x7F) {
        type_char(d, chr);
    }
}

static void on_mouse(int x, int y, bool down) {
    dialog *d = G.front;
    if (!d)
        return;
    int i = item_at(d, x, y);
    if (down) {
        if (i >= 0 && d->items[i].type == DLG_ITEM_EDIT_TEXT) {
            set_focus(d, i);
        } else if (i >= 0 && is_control(d->items[i].type) && !d->items[i].disabled) {
            d->pressed = i;
            draw_item(d, i);
        }
        return;
    }
    int was = d->pressed;
    if (was < 0)
        return;
    d->pressed = -1;
    draw_item(d, was);
    if (i == was)
        press(d, was + 1);
}

/* Inserts pasted or scripted text: printable ASCII only (the fields hold
   e-mail addresses and key codes). */
static void on_text(const char *utf8) {
    dialog *d = G.front;
    if (!d)
        return;
    for (const uint8_t *p = (const uint8_t *)utf8; *p; p++)
        if (*p >= 0x20 && *p < 0x7F)
            type_char(d, *p);
}

static const ev_modal_sink sink = {on_key, on_mouse, on_text};

/* Takes the input until an item is hit; returns it (1-based). */
static int run_modal(dialog *d) {
    dialog *outer = G.front;
    G.front = d;
    d->hit = 0;
    events_set_modal(&sink);
    while (!d->hit) {
        events_pump();
        if (!d->hit)
            misc_wait(1.0 / 60);
    }
    G.front = outer;
    events_set_modal(outer ? &sink : NULL);
    return d->hit;
}

/* ---- Alert ---- */

static const char *button_title(dialog *d, int item1, char *buf, size_t cap) {
    buf[0] = '\0';
    if (item1 >= 1 && item1 <= d->nitems)
        display_text(&d->items[item1 - 1], buf, cap);
    return buf;
}

/* Alert(id, filter) and StopAlert. The default item: bit 3 of the first
   stage's 4 bits in the ALRT's stages word picks item 2, otherwise item 1.
   A program with no resources at all (MONSTER FAIR) still calls Alert on
   a few paths it kept from Mac OS 9; there Alert finds no ALRT and returns
   -1, as Mac OS does, showing nothing. In a classic game a missing ALRT is
   a bug in the shim, so it crashes. */
static void h_alert(void) {
    int16_t id = (int16_t)trap_arg(0);
    rsrc_entry *alrt = rsrc_find(FOURCC('A', 'L', 'R', 'T'), id);
    if (!alrt && rsrc_total() == 0) {
        log_msg("Alert %d: the game has no ALRT %d; returning -1, as Mac OS does", id, id);
        trap_return((uint32_t)-1);
        return;
    }
    if (!alrt || alrt->len < 12)
        trap_crash("Alert: ALRT %d doesn't exist", id);
    if (trap_arg(1))
        trap_crash("Alert: filter procs are not supported");
    const uint8_t *a = rsrc_data(alrt);
    uint16_t stages = rd_be16(a + 10);
    int item1 = (stages & 0x8) ? 2 : 1;
    char text[1024];
    dialogs_alert_text(id, text, sizeof text);
    if (G.auto_alerts) {
        log_msg("Alert %d (answering item %d): %s", id, item1, text);
        trap_return((uint32_t)item1);
        return;
    }
    log_msg("Alert %d: %s", id, text);
    dialog *d = new_dialog("Alert");
    d->res_id = id;
    d->kind = KIND_ALERT;
    d->bounds = place(be_rect(a), alrt->len >= 14 ? rd_be16(a + 12) : 0);
    load_items("Alert", d, (int16_t)rd_be16(a + 8));
    d->default_item = item1;
    show(d);
    int hit = run_modal(d);
    char title[256];
    log_msg("Alert %d: answered item %d (%s)", id, hit, button_title(d, hit, title, sizeof title));
    close_dialog(d);
    trap_return((uint32_t)hit);
}

static void h_param_text(void) {
    for (int i = 0; i < 4; i++) {
        uint32_t s = trap_arg(i);
        if (s)
            gm_read_pstr(s, G.param[i]);
        else
            G.param[i][0] = '\0';
    }
}

/* ---- dialogs ---- */

static uint32_t ref_of(dialog *d) { return DLG_TAG_BASE + 16u * (uint32_t)(d - G.d); }

static dialog *need_dialog(const char *call, uint32_t ref) {
    uint32_t i = (ref - DLG_TAG_BASE) / 16u;
    if (ref < DLG_TAG_BASE || (ref - DLG_TAG_BASE) % 16u || i >= DLG_MAX || !G.d[i].open || G.d[i].kind != KIND_DLOG)
        trap_crash("%s: 0x%08x is not a dialog", call, ref);
    return &G.d[i];
}

/* GetNewDialog(short id, void *storage, WindowRef behind) -> DialogRef.
   DLOG: rect (8), procID (2), visible (1), pad (1), goAway (1), pad (1),
   refCon (4), DITL id (2), title (Str255, padded to even), position (2).
   The default button is item 1 if it is a button, otherwise the first
   button (the Dialog Manager would use item 1 regardless). */
static void h_get_new_dialog(void) {
    int16_t id = (int16_t)trap_arg(0);
    if (trap_arg(1))
        trap_crash("GetNewDialog: caller-supplied dialog storage is not supported");
    rsrc_entry *e = rsrc_find(FOURCC('D', 'L', 'O', 'G'), id);
    if (!e || e->len < 21)
        trap_crash("GetNewDialog: DLOG %d doesn't exist", id);
    const uint8_t *p = rsrc_data(e);
    uint32_t pos_at = 21u + p[20];
    pos_at += pos_at & 1u;
    dialog *d = new_dialog("GetNewDialog");
    d->res_id = id;
    d->bounds = place(be_rect(p), pos_at + 2 <= e->len ? rd_be16(p + pos_at) : 0);
    load_items("GetNewDialog", d, (int16_t)rd_be16(p + 18));
    for (int i = 0; i < d->nitems && !d->default_item; i++)
        if (d->items[i].type == DLG_ITEM_BUTTON && (i == 0 || d->items[0].type != DLG_ITEM_BUTTON))
            d->default_item = i + 1;
    if (p[10])
        show(d);
    log_msg("GetNewDialog %d", id);
    trap_return(ref_of(d));
}

/* ModalDialog(ModalFilterUPP filter, DialogItemIndex *itemHit). Returns
   when an enabled button is clicked (or chosen with Return or Esc); typing
   is handled inside. */
static void h_modal_dialog(void) {
    if (trap_arg(0))
        trap_crash("ModalDialog: filter procs are not supported");
    dialog *d = NULL;
    for (int i = 0; i < DLG_MAX; i++)
        if (G.d[i].open && G.d[i].kind == KIND_DLOG && (!d || G.d[i].order > d->order))
            d = &G.d[i];
    if (!d)
        trap_crash("ModalDialog: no dialog is open");
    if (G.auto_alerts)
        trap_crash("ModalDialog: DLOG %d can't be answered automatically (LOONY_AUTO_ALERTS)", d->res_id);
    if (!d->shown) /* an invisible DLOG appears when it's used */
        show(d);
    int hit = run_modal(d);
    gm_w16(trap_arg(1), (uint16_t)hit);
}

/* GetDialogItem(DialogRef, DialogItemIndex, DialogItemType *type, Handle
   *item, Rect *box). Text items' handles hold their current text; other
   items have none. */
static void h_get_dialog_item(void) {
    dialog *d = need_dialog("GetDialogItem", trap_arg(0));
    int16_t n = (int16_t)trap_arg(1);
    if (n < 1 || n > d->nitems)
        trap_crash("GetDialogItem: DLOG %d has no item %d", d->res_id, n);
    item *it = &d->items[n - 1];
    if (trap_arg(2))
        gm_w16(trap_arg(2), (uint16_t)(it->type | (it->disabled ? DLG_ITEM_DISABLED : 0)));
    if (trap_arg(3))
        gm_w32(trap_arg(3), it->handle);
    if (trap_arg(4))
        qd_write_rect(trap_arg(4), it->r);
}

/* GetDialogItemText(Handle, Str255 text): the handle's bytes, at most 255. */
static void h_get_dialog_item_text(void) {
    uint32_t h = trap_arg(0), out = trap_arg(1);
    if (!mm_is_handle(h))
        trap_crash("GetDialogItemText: 0x%08x is not a handle", h);
    uint32_t n = mm_handle_size(h);
    if (n > MAX_TEXT)
        n = MAX_TEXT;
    gm_w8(out, (uint8_t)n);
    if (n)
        memcpy(gm_ptr(out + 1, n), gm_ptr(gm_r32(h), n), n);
}

static void h_dispose_dialog(void) {
    dialog *d = need_dialog("DisposeDialog", trap_arg(0));
    close_dialog(d);
}

/* ---- nib windows ---- */

static void fourcc_text(uint32_t c, char out[5]) {
    for (int i = 0; i < 4; i++) {
        char ch = (char)(c >> (24 - 8 * i));
        out[i] = ch >= 0x20 && ch < 0x7F ? ch : '?';
    }
    out[4] = '\0';
}

/* Sets an item's text from UTF-8, with line breaks as carriage returns (the
   8x8 font is ASCII, which is all the game's dialogs use). */
static void set_text(item *it, const char *s) {
    int n = 0;
    for (; s[n] && n < TEXT_CAP; n++)
        it->text[n] = s[n] == '\n' ? '\r' : (uint8_t)s[n];
    it->len = n;
}

static uint32_t control_ref(dialog *d, int index) {
    return CONTROL_TAG_BASE + 256u * (uint32_t)(d - G.d) + (uint32_t)index;
}

static dialog *window_dialog(uint32_t window) {
    for (int i = 0; i < DLG_MAX; i++)
        if (G.d[i].open && G.d[i].kind == KIND_NIB && G.d[i].window == window)
            return &G.d[i];
    return NULL;
}

static dialog *need_nib_window(const char *call, uint32_t window) {
    dialog *d = window_dialog(window);
    if (!d)
        trap_crash("%s: 0x%08x is not a nib window", call, window);
    return d;
}

/* The control a ref names: its window's dialog, and the item index or
   ROOT_VIEW. */
static dialog *need_control(const char *call, uint32_t ref, int *index) {
    uint32_t off = ref - CONTROL_TAG_BASE, slot = off / 256u, i = off % 256u;
    if (ref < CONTROL_TAG_BASE || slot >= DLG_MAX || !G.d[slot].open || G.d[slot].kind != KIND_NIB ||
        (i != ROOT_VIEW && (int)i >= G.d[slot].nitems))
        trap_crash("%s: 0x%08x is not a control", call, ref);
    *index = (int)i;
    return &G.d[slot];
}

/* A control other than a root view, of the given item type. */
static item *need_item(const char *call, uint32_t ref, uint8_t type, dialog **dp) {
    int i;
    dialog *d = need_control(call, ref, &i);
    if (i == ROOT_VIEW || d->items[i].type != type)
        trap_crash("%s: control 0x%08x of nib window %s is the wrong kind", call, ref, d->name);
    *dp = d;
    return &d->items[i];
}

/* What the window's port went through (qd_set_window_hook): a nib window is
   drawn while its port is shown. */
static void window_changed(uint32_t window, qd_window_change change, uint32_t arg) {
    dialog *d = window_dialog(window);
    if (!d)
        return;
    switch (change) {
    case QD_WINDOW_SHOWN:
        if (!d->shown) {
            log_msg("nib window %s: shown", d->name);
            show(d);
        }
        break;
    case QD_WINDOW_HIDDEN:
        if (d->shown)
            hide(d);
        break;
    case QD_WINDOW_DISPOSED: close_dialog(d); break;
    case QD_WINDOW_REPOSITIONED: {
        /* kWindowAlertPositionOnMainScreen (7) and the alert positions on a
           parent window (8, 9, with no parent: the main screen); anything
           else centers it. */
        bool was = d->shown;
        if (was)
            hide(d);
        d->bounds = place(d->bounds, arg >= 7 && arg <= 9 ? POS_ALERT_MAIN : POS_CENTER_MAIN);
        if (was)
            show(d);
        break;
    }
    }
}

/* CreateNibReference(CFStringRef name, IBNibRef *out) -> OSStatus. Reads
   <bundle>/Contents/Resources/English.lproj/<name>.nib/objects.xib. */
static void h_create_nib_reference(void) {
    const char *name = cf_string_text("CreateNibReference", trap_arg(0));
    uint32_t out = trap_arg(1);
    const char *bundle = cf_bundle_path();
    if (!bundle)
        trap_crash("CreateNibReference: the game isn't a bundle");
    int slot = 0;
    while (slot < NIB_MAX && G.nibs[slot])
        slot++;
    if (slot == NIB_MAX)
        trap_crash("CreateNibReference: more than %d nibs open", NIB_MAX);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/Contents/Resources/English.lproj/%s.nib/objects.xib", bundle, name);
    size_t len = 0;
    uint8_t *xml = read_file(path, &len);
    if (!xml) {
        log_msg("CreateNibReference: can't read %s", path);
        gm_w32(out, 0);
        trap_return((uint32_t)FNF_ERR);
        return;
    }
    G.nibs[slot] = (char *)xml;
    G.nib_len[slot] = len;
    gm_w32(out, NIB_TAG_BASE + 16u * (uint32_t)slot);
    trap_return(NO_ERR);
}

static int need_nib(const char *call, uint32_t ref) {
    uint32_t i = (ref - NIB_TAG_BASE) / 16u;
    if (ref < NIB_TAG_BASE || (ref - NIB_TAG_BASE) % 16u || i >= NIB_MAX || !G.nibs[i])
        trap_crash("%s: 0x%08x is not a nib", call, ref);
    return (int)i;
}

static void h_dispose_nib_reference(void) {
    int i = need_nib("DisposeNibReference", trap_arg(0));
    free(G.nibs[i]);
    G.nibs[i] = NULL;
}

/* CreateWindowFromNib(IBNibRef, CFStringRef name, WindowRef *out) ->
   OSStatus. The window is a hidden port the size of the nib's window, centered
   until RepositionWindow moves it. The default button is the one whose
   command is 'ok  ' (or else the nib's default button), the cancel button
   the one whose command is 'not!' (or else the nib's cancel button). */
static void h_create_window_from_nib(void) {
    int slot = need_nib("CreateWindowFromNib", trap_arg(0));
    const char *name = cf_string_text("CreateWindowFromNib", trap_arg(1));
    static nib_window w;
    char err[256];
    if (!nib_read_window(G.nibs[slot], G.nib_len[slot], name, &w, err, sizeof err))
        trap_crash("CreateWindowFromNib: %s", err);
    if (w.ncontrols > MAX_ITEMS)
        trap_crash("CreateWindowFromNib: %s has %d controls (at most %d supported)", name, w.ncontrols,
                   MAX_ITEMS);
    dialog *d = new_dialog("CreateWindowFromNib");
    d->kind = KIND_NIB;
    snprintf(d->name, sizeof d->name, "%s", name);
    int width = w.rect.right - w.rect.left, height = w.rect.bottom - w.rect.top;
    d->window = qd_new_window(width, height);
    d->bounds = place((qd_rect){0, 0, (int16_t)height, (int16_t)width}, POS_CENTER_MAIN);
    d->focus = d->pressed = -1;
    int default_type = 0, cancel_type = 0;
    for (int i = 0; i < w.ncontrols; i++) {
        const nib_control *c = &w.controls[i];
        item *it = &d->items[i];
        it->r = (qd_rect){(int16_t)c->bounds.top, (int16_t)c->bounds.left, (int16_t)c->bounds.bottom,
                          (int16_t)c->bounds.right};
        it->command = c->command;
        it->signature = c->signature;
        it->id = c->id;
        switch (c->kind) {
        case NIB_BUTTON:
            it->type = DLG_ITEM_BUTTON;
            set_text(it, c->title);
            if (c->command == CMD_OK && !d->default_item)
                d->default_item = i + 1;
            if (c->command == CMD_CANCEL && !d->cancel_item)
                d->cancel_item = i + 1;
            if (c->button_type == 1 && !default_type)
                default_type = i + 1;
            if (c->button_type == 2 && !cancel_type)
                cancel_type = i + 1;
            break;
        case NIB_STATIC_TEXT:
            it->type = DLG_ITEM_STATIC_TEXT;
            set_text(it, c->title);
            break;
        case NIB_EDIT_TEXT:
            it->type = DLG_ITEM_EDIT_TEXT;
            if (d->focus < 0)
                d->focus = i;
            break;
        case NIB_IMAGE_VIEW:
            it->type = ITEM_IMAGE_VIEW;
            it->scale_to_fit = true;
            break;
        case NIB_ICON: it->type = DLG_ITEM_ICON; break; /* the game's has no icon resource: blank */
        }
    }
    d->nitems = w.ncontrols;
    if (!d->default_item)
        d->default_item = default_type;
    if (!d->cancel_item)
        d->cancel_item = cancel_type;
    log_msg("nib window %s: %dx%d, %d controls", name, width, height, d->nitems);
    gm_w32(trap_arg(2), d->window);
    trap_return(NO_ERR);
}

/* Sends item i's command to the window's handlers, as clicking it does. */
static void send_item_command(dialog *d, int i) {
    item *it = &d->items[i];
    char title[TEXT_CAP + 1], cmd[5];
    display_text(it, title, sizeof title);
    fourcc_text(it->command, cmd);
    log_msg("nib window %s: command '%s' (%s)", d->name, cmd, title);
    if (it->command)
        events_send_command(d->window, it->command);
}

static bool still_open(const dialog *d, unsigned order) { return d->open && d->order == order; }

/* RunAppModalLoopForWindow(WindowRef) -> OSStatus. Takes the input until
   QuitAppModalLoopForWindow is called for the window, or it's disposed of.
   A button's command is sent from here, not from inside the input sink, so
   the game's handler can open (and run) other windows. With
   LOONY_AUTO_ALERTS=1 the default button is pressed at once, and it must
   end the loop. */
static void h_run_app_modal_loop_for_window(void) {
    dialog *d = need_nib_window("RunAppModalLoopForWindow", trap_arg(0));
    unsigned order = d->order;
    dialog *outer = G.front;
    unsigned outer_order = outer ? outer->order : 0;
    G.front = d;
    d->hit = 0;
    d->quit_modal = false;
    events_set_modal(&sink);
    if (G.auto_alerts) {
        if (!d->default_item)
            trap_crash("RunAppModalLoopForWindow: nib window %s has no default button (LOONY_AUTO_ALERTS)",
                       d->name);
        log_msg("nib window %s: answering with its default button", d->name);
        send_item_command(d, d->default_item - 1);
        if (still_open(d, order) && !d->quit_modal)
            trap_crash("RunAppModalLoopForWindow: nib window %s's default button didn't end its modal "
                       "loop (LOONY_AUTO_ALERTS)",
                       d->name);
    }
    while (still_open(d, order) && !d->quit_modal) {
        events_pump();
        if (!still_open(d, order) || d->quit_modal)
            break;
        if (d->hit) {
            int i = d->hit - 1;
            d->hit = 0;
            send_item_command(d, i);
            continue;
        }
        misc_wait(1.0 / 60);
    }
    if (still_open(d, order))
        d->quit_modal = false;
    G.front = outer && still_open(outer, outer_order) ? outer : NULL;
    events_set_modal(G.front ? &sink : NULL);
    trap_return(NO_ERR);
}

static void h_quit_app_modal_loop_for_window(void) {
    need_nib_window("QuitAppModalLoopForWindow", trap_arg(0))->quit_modal = true;
    trap_return(NO_ERR);
}

/* ---- controls ---- */

static void h_hiview_get_root(void) {
    trap_return(control_ref(need_nib_window("HIViewGetRoot", trap_arg(0)), ROOT_VIEW));
}

/* Writes the window's control with this ControlID to out. */
static void find_control(dialog *d, uint32_t signature, int32_t id, uint32_t out) {
    for (int i = 0; i < d->nitems; i++)
        if (d->items[i].signature == signature && d->items[i].id == id) {
            gm_w32(out, control_ref(d, i));
            trap_return(NO_ERR);
            return;
        }
    char sig[5];
    fourcc_text(signature, sig);
    log_msg("nib window %s has no control '%s' %d", d->name, sig, id);
    gm_w32(out, 0);
    trap_return((uint32_t)ERR_UNKNOWN_CONTROL);
}

/* HIViewFindByID(HIViewRef start, HIViewID id, HIViewRef *out) -> OSStatus.
   The HIViewID is passed by value: its signature in r4, its id in r5. A
   nib window's controls are all the root view's children, so any view of
   the window finds them all. */
static void h_hiview_find_by_id(void) {
    int i;
    dialog *d = need_control("HIViewFindByID", trap_arg(0), &i);
    find_control(d, trap_arg(1), (int32_t)trap_arg(2), trap_arg(3));
}

/* GetControlByID(WindowRef, const ControlID *id, ControlRef *out) -> OSStatus */
static void h_get_control_by_id(void) {
    dialog *d = need_nib_window("GetControlByID", trap_arg(0));
    uint32_t id = trap_arg(1);
    find_control(d, gm_r32(id), (int32_t)gm_r32(id + 4), trap_arg(2));
}

/* HIViewSetVisible(HIViewRef, Boolean) -> OSStatus */
static void h_hiview_set_visible(void) {
    int i;
    dialog *d = need_control("HIViewSetVisible", trap_arg(0), &i);
    if (i == ROOT_VIEW)
        trap_crash("HIViewSetVisible: showing or hiding a root view is not supported");
    bool hidden = (trap_arg(1) & 0xFF) == 0;
    if (d->items[i].hidden != hidden) {
        d->items[i].hidden = hidden;
        if (d->shown)
            draw_all(d);
    }
    trap_return(NO_ERR);
}

/* GetControlData(ControlRef, ControlPartCode, ResType tag, Size size, void
   *buffer, Size *actual) -> OSStatus, for an edit text:
   kControlEditTextCFStringTag ('cfst') makes a CFString of its text, and
   kControlEditTextTextTag ('text') copies the bytes (at most size). */
static void h_get_control_data(void) {
    dialog *d;
    item *it = need_item("GetControlData", trap_arg(0), DLG_ITEM_EDIT_TEXT, &d);
    uint32_t tag = trap_arg(2), size = trap_arg(3), buf = trap_arg(4), actual = trap_arg(5);
    uint32_t n;
    if (tag == FOURCC('c', 'f', 's', 't')) {
        n = 4;
        if (buf && size < n) {
            trap_return((uint32_t)PARAM_ERR);
            return;
        }
        if (buf) {
            char text[TEXT_CAP + 1];
            memcpy(text, it->text, (size_t)it->len);
            text[it->len] = '\0';
            gm_w32(buf, cf_string(text));
        }
    } else if (tag == FOURCC('t', 'e', 'x', 't')) {
        n = (uint32_t)it->len;
        uint32_t copy = n < size ? n : size;
        if (buf && copy)
            memcpy(gm_ptr(buf, copy), it->text, copy);
    } else {
        char t[5];
        fourcc_text(tag, t);
        trap_crash("GetControlData: tag '%s' is not supported", t);
    }
    if (actual)
        gm_w32(actual, n);
    trap_return(NO_ERR);
}

/* HIImageViewSetImage(HIViewRef, CGImageRef) -> OSStatus: the view retains
   the image. */
static void h_hiimage_view_set_image(void) {
    dialog *d;
    item *it = need_item("HIImageViewSetImage", trap_arg(0), ITEM_IMAGE_VIEW, &d);
    uint32_t image = trap_arg(1);
    if (image && !cgimage_get(image))
        trap_crash("HIImageViewSetImage: 0x%08x is not a CGImage", image);
    cgimage_retain(image);
    cgimage_release(it->image);
    it->image = image;
    if (d->shown)
        draw_all(d);
    trap_return(NO_ERR);
}

static void h_hiimage_view_set_scale_to_fit(void) {
    dialog *d;
    item *it = need_item("HIImageViewSetScaleToFit", trap_arg(0), ITEM_IMAGE_VIEW, &d);
    it->scale_to_fit = (trap_arg(1) & 0xFF) != 0;
    if (d->shown)
        draw_all(d);
    trap_return(NO_ERR);
}

/* HIImageViewSetOpaque(view, Boolean) and HIImageViewSetAlpha(view, float):
   the image is always drawn opaque, over white. */
static void h_hiimage_view_set_opaque(void) {
    dialog *d;
    need_item("HIImageViewSetOpaque", trap_arg(0), ITEM_IMAGE_VIEW, &d);
    trap_return(NO_ERR);
}

static void h_hiimage_view_set_alpha(void) {
    dialog *d;
    need_item("HIImageViewSetAlpha", trap_arg(0), ITEM_IMAGE_VIEW, &d);
    trap_return(NO_ERR);
}

/* ---- standard alerts ---- */

#define STD_ALERT_W 400
#define STD_ALERT_MARGIN 20

/* Adds a static text item of the alert's width at y; returns its bottom. */
static int add_alert_text(dialog *d, int y, const char *s) {
    item *it = &d->items[d->nitems++];
    it->type = DLG_ITEM_STATIC_TEXT;
    set_text(it, s);
    char text[1024];
    display_text(it, text, sizeof text);
    int w = STD_ALERT_W - 2 * STD_ALERT_MARGIN, starts[32], lens[32];
    int lines = font_wrap(text, w, 32, starts, lens);
    it->r = (qd_rect){(int16_t)y, STD_ALERT_MARGIN, (int16_t)(y + lines * FONT_LINE + 2),
                      (int16_t)(STD_ALERT_MARGIN + w)};
    return it->r.bottom;
}

/* CreateStandardAlert(AlertType, CFStringRef error, CFStringRef explanation,
   const AlertStdCFStringAlertParamRec *param, DialogRef *out) -> OSStatus.
   Only a NULL param, the game's: one button, OK (item 1). Laid out as an
   alert: the error, the explanation, then the button at the bottom right. */
static void h_create_standard_alert(void) {
    if (trap_arg(3))
        trap_crash("CreateStandardAlert: an AlertStdCFStringAlertParamRec is not supported");
    dialog *d = new_dialog("CreateStandardAlert");
    d->kind = KIND_STD_ALERT;
    d->focus = d->pressed = -1;
    item *ok = &d->items[d->nitems++];
    ok->type = DLG_ITEM_BUTTON;
    set_text(ok, "OK");
    d->default_item = 1;
    int y = add_alert_text(d, STD_ALERT_MARGIN, cf_string_text("CreateStandardAlert", trap_arg(1)));
    if (trap_arg(2)) {
        const char *explanation = cf_string_text("CreateStandardAlert", trap_arg(2));
        if (*explanation)
            y = add_alert_text(d, y + 10, explanation);
    }
    y += 16;
    ok->r = (qd_rect){(int16_t)y, STD_ALERT_W - STD_ALERT_MARGIN - 70, (int16_t)(y + 20),
                      STD_ALERT_W - STD_ALERT_MARGIN};
    d->bounds = place((qd_rect){0, 0, (int16_t)(y + 20 + STD_ALERT_MARGIN), STD_ALERT_W}, POS_ALERT_MAIN);
    gm_w32(trap_arg(4), ref_of(d));
    trap_return(NO_ERR);
}

/* RunStandardAlert(DialogRef, ModalFilterUPP, DialogItemIndex *hit) ->
   OSStatus. Runs the alert, writes the item hit, and disposes of it. */
static void h_run_standard_alert(void) {
    uint32_t ref = trap_arg(0), i = (ref - DLG_TAG_BASE) / 16u;
    if (ref < DLG_TAG_BASE || (ref - DLG_TAG_BASE) % 16u || i >= DLG_MAX || !G.d[i].open ||
        G.d[i].kind != KIND_STD_ALERT)
        trap_crash("RunStandardAlert: 0x%08x is not a standard alert", ref);
    if (trap_arg(1))
        trap_crash("RunStandardAlert: filter procs are not supported");
    dialog *d = &G.d[i];
    char text[1024] = "";
    for (int k = 1; k < d->nitems; k++) {
        char part[TEXT_CAP + 1];
        display_text(&d->items[k], part, sizeof part);
        if (text[0])
            append(text, sizeof text, (const uint8_t *)" | ", 3);
        append(text, sizeof text, (const uint8_t *)part, strlen(part));
    }
    int hit;
    if (G.auto_alerts) {
        hit = d->default_item;
        log_msg("standard alert (answering item %d): %s", hit, text);
    } else {
        log_msg("standard alert: %s", text);
        show(d);
        hit = run_modal(d);
        log_msg("standard alert: answered item %d", hit);
    }
    close_dialog(d);
    if (trap_arg(2))
        gm_w16(trap_arg(2), (uint16_t)hit);
    trap_return(NO_ERR);
}

void dialogs_register(void) {
    trap_register("Alert", h_alert);
    trap_register("StopAlert", h_alert);
    trap_register("ParamText", h_param_text);
    trap_register("GetNewDialog", h_get_new_dialog);
    trap_register("ModalDialog", h_modal_dialog);
    trap_register("GetDialogItem", h_get_dialog_item);
    trap_register("GetDialogItemText", h_get_dialog_item_text);
    trap_register("DisposeDialog", h_dispose_dialog);
    trap_register("CreateNibReference", h_create_nib_reference);
    trap_register("DisposeNibReference", h_dispose_nib_reference);
    trap_register("CreateWindowFromNib", h_create_window_from_nib);
    trap_register("RunAppModalLoopForWindow", h_run_app_modal_loop_for_window);
    trap_register("QuitAppModalLoopForWindow", h_quit_app_modal_loop_for_window);
    trap_register("HIViewGetRoot", h_hiview_get_root);
    trap_register("HIViewFindByID", h_hiview_find_by_id);
    trap_register("GetControlByID", h_get_control_by_id);
    trap_register("HIViewSetVisible", h_hiview_set_visible);
    trap_register("GetControlData", h_get_control_data);
    trap_register("HIImageViewSetImage", h_hiimage_view_set_image);
    trap_register("HIImageViewSetScaleToFit", h_hiimage_view_set_scale_to_fit);
    trap_register("HIImageViewSetOpaque", h_hiimage_view_set_opaque);
    trap_register("HIImageViewSetAlpha", h_hiimage_view_set_alpha);
    trap_register("CreateStandardAlert", h_create_standard_alert);
    trap_register("RunStandardAlert", h_run_standard_alert);
}
