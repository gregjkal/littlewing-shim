#include "dialogs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "events.h"
#include "font.h"
#include "guest_mem.h"
#include "keymap.h"
#include "memmgr.h"
#include "misc.h"
#include "pict.h"
#include "qd.h"
#include "rsrc.h"
#include "trap.h"
#include "util.h"

#define MAX_ITEMS 16
#define MAX_TEXT 255
#define FRAME 4 /* the border drawn outside a dialog's rectangle */

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
    uint8_t text[MAX_TEXT + 1]; /* button title, static text, an edit field's contents */
    int len;
    int16_t res_id;  /* pictures and icons */
    uint32_t handle; /* edit and static text: a guest handle holding the text */
} item;

typedef struct {
    bool open;
    int16_t res_id; /* the ALRT or DLOG */
    bool is_alert;
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
} G;

void dialogs_init(void) {
    for (int i = 0; i < DLG_MAX; i++)
        free(G.d[i].saved);
    memset(&G, 0, sizeof G);
    const char *a = getenv("LOONY_AUTO_ALERTS");
    G.auto_alerts = a && strcmp(a, "1") == 0;
    font_init();
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

static void draw_item(dialog *d, int i) {
    item *it = &d->items[i];
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
        int n = font_wrap(text, rect_w(it->r), 32, starts, lens);
        for (int k = 0; k < n && k < 32; k++)
            text_at(r.left, r.top + 2 + k * FONT_LINE, text + starts[k], lens[k], BLACK, d->bounds);
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
    default: break;
    }
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
    fill(outset(d->bounds, FRAME), BLACK);
    frame(outset(d->bounds, FRAME - 1), 1, WHITE);
    fill(d->bounds, WHITE);
    for (int i = 0; i < d->nitems; i++)
        draw_item(d, i);
    qd_mark_dirty();
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
    for (int i = 0; i < d->nitems; i++)
        if (d->items[i].handle)
            mm_dispose_handle(d->items[i].handle);
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
        if (inside(r, x, y))
            return i;
    }
    return -1;
}

static void press(dialog *d, int item1) {
    if (item1 > 0 && item1 <= d->nitems && !d->items[item1 - 1].disabled)
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
            if (d->items[i].type == DLG_ITEM_EDIT_TEXT) {
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
   stage's 4 bits in the ALRT's stages word picks item 2, otherwise item 1. */
static void h_alert(void) {
    int16_t id = (int16_t)trap_arg(0);
    rsrc_entry *alrt = rsrc_find(FOURCC('A', 'L', 'R', 'T'), id);
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
    d->is_alert = true;
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
    if (ref < DLG_TAG_BASE || (ref - DLG_TAG_BASE) % 16u || i >= DLG_MAX || !G.d[i].open || G.d[i].is_alert)
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
        if (G.d[i].open && !G.d[i].is_alert && (!d || G.d[i].order > d->order))
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

void dialogs_register(void) {
    trap_register("Alert", h_alert);
    trap_register("StopAlert", h_alert);
    trap_register("ParamText", h_param_text);
    trap_register("GetNewDialog", h_get_new_dialog);
    trap_register("ModalDialog", h_modal_dialog);
    trap_register("GetDialogItem", h_get_dialog_item);
    trap_register("GetDialogItemText", h_get_dialog_item_text);
    trap_register("DisposeDialog", h_dispose_dialog);
}
