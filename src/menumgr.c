#include "menumgr.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "guest_mem.h"
#include "memmgr.h"
#include "rsrc.h"
#include "snapshot.h"
#include "trap.h"
#include "util.h"

#define TYPE_MENU 0x4D454E55u /* 'MENU' */
#define MENU_HEADER 14        /* id, width, height, procID, filler, enableFlags */
#define CMD_KEY 0x0100u       /* in an EventRecord's modifiers */
#define KEY_DOWN 3
#define AUTO_KEY 5
#define NAME_CAP 64

typedef struct {
    char text[NAME_CAP];
    uint8_t key, mark, style;
    bool enabled;
    uint32_t command;
} menu_item;

typedef struct {
    bool used;
    uint32_t handle; /* the MenuRef */
    int16_t id;
    char title[NAME_CAP];
    bool enabled;
    int nitems;
    menu_item items[MENU_MAX_ITEMS];
} menu;

static struct {
    menu menus[MENU_MAX];
    int bar[MENU_MAX]; /* the inserted menus' slots, left to right */
    int nbar;
    bool bar_visible;
    int16_t hilited;
} N;

/* A snapshot's walk (snapshot.h): only the counts and the slots in the
   menu bar need checking. */
static void snap_walk(snapshot_walker *w) {
    snapshot_bytes(w, &N, sizeof N);
    bool counted = N.nbar >= 0 && N.nbar <= MENU_MAX;
    snapshot_check(w, counted, "%d menus in the bar", N.nbar);
    for (int i = 0; counted && i < N.nbar; i++)
        snapshot_check(w, N.bar[i] >= 0 && N.bar[i] < MENU_MAX, "menu bar slot %d", N.bar[i]);
    for (int i = 0; i < MENU_MAX; i++)
        snapshot_check(w, N.menus[i].nitems >= 0 && N.menus[i].nitems <= MENU_MAX_ITEMS,
                       "menu %d has %d items", i, N.menus[i].nitems);
}
SNAPSHOT_STATE_WALK(N, snap_walk)

void menumgr_init(void) {
    memset(&N, 0, sizeof N);
    N.bar_visible = true;
}

static uint32_t menu_ref(int slot) { return N.menus[slot].handle; }

static menu *need_menu(const char *call, uint32_t ref) {
    for (int i = 0; i < MENU_MAX; i++)
        if (N.menus[i].used && N.menus[i].handle == ref)
            return &N.menus[i];
    trap_crash("%s: 0x%08x is not a menu", call, ref);
}

/* Item i (from 1) of m, or NULL past the end. */
static menu_item *item_of(menu *m, int i) { return i >= 1 && i <= m->nitems ? &m->items[i - 1] : NULL; }

static void pstr_to(const uint8_t *p, size_t avail, char *out, size_t *used) {
    size_t n = avail ? p[0] : 0;
    if (n + 1 > avail)
        n = avail ? avail - 1 : 0;
    size_t keep = n < NAME_CAP - 1 ? n : NAME_CAP - 1;
    memcpy(out, p + 1, keep);
    out[keep] = '\0';
    *used = n + 1;
}

/* Reads MENU resource bytes into m: the header, the title, then each item's
   name, icon, key equivalent, mark and style, up to a 0 length. Bit 0 of
   enableFlags is the menu; bit i, item i (items past 31 are enabled). */
static bool parse_menu(const uint8_t *d, uint32_t len, menu *m) {
    if (len < MENU_HEADER + 1)
        return false;
    memset(m, 0, sizeof *m);
    m->used = true;
    m->id = (int16_t)rd_be16(d);
    uint32_t flags = rd_be32(d + 10);
    m->enabled = flags & 1;
    size_t at = MENU_HEADER, used;
    pstr_to(d + at, len - at, m->title, &used);
    at += used;
    while (at < len && d[at] != 0 && m->nitems < MENU_MAX_ITEMS) {
        menu_item *it = &m->items[m->nitems];
        pstr_to(d + at, len - at, it->text, &used);
        at += used;
        if (at + 4 > len)
            return false;
        it->key = d[at + 1];
        it->mark = d[at + 2];
        it->style = d[at + 3];
        at += 4;
        m->nitems++;
        it->enabled = m->nitems > 31 || (flags >> m->nitems) & 1;
    }
    return true;
}

/* ---- guest calls ---- */

/* GetMenu(short resourceID) -> MenuRef, or NULL with no such MENU. */
static void h_get_menu(void) {
    int16_t id = (int16_t)trap_arg(0);
    rsrc_entry *e = rsrc_find(TYPE_MENU, id);
    if (!e) {
        trap_return(0);
        return;
    }
    int slot = 0;
    while (slot < MENU_MAX && N.menus[slot].used)
        slot++;
    if (slot == MENU_MAX)
        trap_crash("GetMenu: more than %d menus", MENU_MAX);
    if (!parse_menu(rsrc_data(e), e->len, &N.menus[slot]))
        trap_crash("GetMenu: MENU %d is malformed", id);
    uint32_t h = mm_new_handle(e->len, false);
    if (!h)
        trap_crash("GetMenu: out of guest memory");
    memcpy(gm_ptr(gm_r32(h), e->len), rsrc_data(e), e->len);
    N.menus[slot].handle = h;
    trap_return(h);
}

static int bar_index(int16_t id) {
    for (int i = 0; i < N.nbar; i++)
        if (N.menus[N.bar[i]].id == id)
            return i;
    return -1;
}

static void remove_from_bar(int at) {
    memmove(&N.bar[at], &N.bar[at + 1], (size_t)(N.nbar - at - 1) * sizeof N.bar[0]);
    N.nbar--;
}

static void h_dispose_menu(void) {
    menu *m = need_menu("DisposeMenu", trap_arg(0));
    int at = bar_index(m->id);
    if (at >= 0 && &N.menus[N.bar[at]] == m)
        remove_from_bar(at);
    mm_dispose_handle(m->handle);
    m->used = false;
}

/* InsertMenu(MenuRef, MenuID beforeID): before that menu, at the end if
   there's none (0), or nowhere for a submenu (-1), which needs no bar. A
   menu already in the bar stays where it is. */
static void h_insert_menu(void) {
    menu *m = need_menu("InsertMenu", trap_arg(0));
    int16_t before = (int16_t)trap_arg(1);
    if (before == -1 || bar_index(m->id) >= 0)
        return;
    int at = bar_index(before);
    if (at < 0)
        at = N.nbar;
    memmove(&N.bar[at + 1], &N.bar[at], (size_t)(N.nbar - at) * sizeof N.bar[0]);
    N.bar[at] = (int)(m - N.menus);
    N.nbar++;
}

static void h_delete_menu(void) {
    int at = bar_index((int16_t)trap_arg(0));
    if (at >= 0)
        remove_from_bar(at);
}

static void h_hilite_menu(void) { N.hilited = (int16_t)trap_arg(0); }
static void h_nothing(void) {}
static void h_show_menu_bar(void) { N.bar_visible = true; }
static void h_hide_menu_bar(void) { N.bar_visible = false; }
static void h_is_menu_bar_visible(void) { trap_return(N.bar_visible); }

static void h_count_menu_items(void) { trap_return((uint32_t)need_menu("CountMenuItems", trap_arg(0))->nitems); }

/* EnableMenuItem and DisableMenuItem (MenuRef, item): item 0 is the menu. */
static void set_enabled(const char *call, bool on) {
    menu *m = need_menu(call, trap_arg(0));
    int i = (int16_t)trap_arg(1);
    if (i == 0)
        m->enabled = on;
    else if (item_of(m, i))
        item_of(m, i)->enabled = on;
}

static void h_enable_menu_item(void) { set_enabled("EnableMenuItem", true); }
static void h_disable_menu_item(void) { set_enabled("DisableMenuItem", false); }

static void h_is_menu_item_enabled(void) {
    menu *m = need_menu("IsMenuItemEnabled", trap_arg(0));
    int i = (int16_t)trap_arg(1);
    menu_item *it = item_of(m, i);
    trap_return(i == 0 ? m->enabled : it ? m->enabled && it->enabled : false);
}

/* EnableMenuCommand and DisableMenuCommand (MenuRef or NULL for every
   menu, MenuCommand): the items with that command. Items get commands only
   from SetMenuItemCommandID or an xmnu, neither of which these games use,
   so this finds none unless one does. */
static void set_command_enabled(bool on) {
    uint32_t ref = trap_arg(0), cmd = trap_arg(1);
    for (int s = 0; s < MENU_MAX; s++) {
        menu *m = &N.menus[s];
        if (!m->used || (ref && menu_ref(s) != ref))
            continue;
        for (int i = 0; i < m->nitems; i++)
            if (cmd && m->items[i].command == cmd)
                m->items[i].enabled = on;
    }
}

static void h_enable_menu_command(void) { set_command_enabled(true); }
static void h_disable_menu_command(void) { set_command_enabled(false); }

/* GetItemCmd and GetItemMark (MenuRef, item, CharParameter *). */
static void h_get_item_cmd(void) {
    menu_item *it = item_of(need_menu("GetItemCmd", trap_arg(0)), (int16_t)trap_arg(1));
    gm_w16(trap_arg(2), it ? it->key : 0);
}

static void h_get_item_mark(void) {
    menu_item *it = item_of(need_menu("GetItemMark", trap_arg(0)), (int16_t)trap_arg(1));
    gm_w16(trap_arg(2), it ? it->mark : 0);
}

static void h_set_item_mark(void) {
    menu_item *it = item_of(need_menu("SetItemMark", trap_arg(0)), (int16_t)trap_arg(1));
    if (it)
        it->mark = (uint8_t)trap_arg(2);
}

/* SetMenuItemText(MenuRef, item, ConstStr255Param) */
static void h_set_menu_item_text(void) {
    menu_item *it = item_of(need_menu("SetMenuItemText", trap_arg(0)), (int16_t)trap_arg(1));
    if (!it)
        return;
    char s[256];
    gm_read_pstr(trap_arg(2), s);
    snprintf(it->text, sizeof it->text, "%s", s);
}

/* GetMenuItemText(MenuRef, item, Str255): an empty string past the end. */
static void h_get_menu_item_text(void) {
    menu_item *it = item_of(need_menu("GetMenuItemText", trap_arg(0)), (int16_t)trap_arg(1));
    gm_write_pstr(trap_arg(2), it ? it->text : "");
}

/* CheckItem(MenuRef, item, Boolean): the check mark, or none. */
#define CHECK_MARK 0x12
static void h_check_item(void) {
    menu_item *it = item_of(need_menu("CheckItem", trap_arg(0)), (int16_t)trap_arg(1));
    if (it)
        it->mark = (trap_arg(2) & 0xFF) ? CHECK_MARK : 0;
}

/* Puts an item made from text, with the Menu Manager's metacharacters,
   after item `after` of m (0 for the first, past the end for the last):
   '(' disables it, "/c" gives it key c, "!c" mark c, and "<c" (a style)
   and "^c" (an icon) are left out. */
static void insert_item(menu *m, const char *text, size_t len, int after) {
    if (m->nitems == MENU_MAX_ITEMS)
        trap_crash("a menu with more than %d items", MENU_MAX_ITEMS);
    menu_item it = {.enabled = true};
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == '(' && n == 0 && it.enabled) {
            it.enabled = false;
        } else if ((c == '/' || c == '!' || c == '<' || c == '^') && i + 1 < len) {
            uint8_t arg = (uint8_t)text[++i];
            if (c == '/')
                it.key = arg;
            else if (c == '!')
                it.mark = arg;
        } else if (n < NAME_CAP - 1) {
            it.text[n++] = c;
        }
    }
    if (after < 0)
        after = 0;
    if (after > m->nitems)
        after = m->nitems;
    memmove(&m->items[after + 1], &m->items[after], (size_t)(m->nitems - after) * sizeof it);
    m->items[after] = it;
    m->nitems++;
}

/* InsertMenuItem(MenuRef, ConstStr255Param, afterItem): each item of the
   string, which ';' and return separate, in order. */
static void h_insert_menu_item(void) {
    menu *m = need_menu("InsertMenuItem", trap_arg(0));
    char s[256];
    gm_read_pstr(trap_arg(1), s);
    int after = (int16_t)trap_arg(2);
    for (const char *p = s;;) {
        size_t len = strcspn(p, ";\r");
        insert_item(m, p, len, after++);
        if (!p[len])
            break;
        p += len + 1;
    }
}

/* DeleteMenuItem(MenuRef, item) */
static void h_delete_menu_item(void) {
    menu *m = need_menu("DeleteMenuItem", trap_arg(0));
    int i = (int16_t)trap_arg(1);
    if (!item_of(m, i))
        return;
    memmove(&m->items[i - 1], &m->items[i], (size_t)(m->nitems - i) * sizeof m->items[0]);
    m->nitems--;
}

/* AppendResMenu(MenuRef, ResType): an item for each named resource of
   that type. There are no desk accessories ('DRVR') or fonts to list. */
static void h_append_res_menu(void) {
    menu *m = need_menu("AppendResMenu", trap_arg(0));
    uint32_t type = trap_arg(1);
    for (uint32_t i = 0; i < rsrc_total(); i++) {
        rsrc_entry *e = rsrc_at(i);
        if (e->type == type && e->name && m->nitems < MENU_MAX_ITEMS) {
            menu_item it = {.enabled = true};
            snprintf(it.text, sizeof it.text, "%s", e->name);
            m->items[m->nitems++] = it;
        }
    }
}

/* The Desk Manager: there are no desk accessories, so OpenDeskAcc opens
   none (0), SystemClick has no window of one to pass a click to, and
   SystemEdit has no front one to edit (false: the game handles it). */
static void h_open_desk_acc(void) { trap_return(0); }
static void h_system_click(void) {}
static void h_system_edit(void) { trap_return(0); }

/* InitMenus: the menus were made empty at the start. */
static void h_init_menus(void) {}

/* MenuSelect(Point) -> long: nothing is drawn to click on, so nothing. */
static void h_menu_select(void) {
    N.hilited = 0;
    trap_return(0);
}

/* The menu and item, (menuID << 16) | item, of the enabled item in the
   bar whose key equivalent is c (letters in either case), or 0. */
static uint32_t find_key(uint8_t c) {
    for (int b = 0; b < N.nbar; b++) {
        const menu *m = &N.menus[N.bar[b]];
        if (!m->enabled)
            continue;
        for (int i = 0; i < m->nitems; i++) {
            const menu_item *it = &m->items[i];
            if (it->enabled && it->key > ' ' && it->key < 0x7F && toupper(it->key) == toupper(c)) {
                N.hilited = m->id;
                return (uint32_t)(uint16_t)m->id << 16 | (uint32_t)(i + 1);
            }
        }
    }
    return 0;
}

/* MenuKey(CharParameter) -> long */
static void h_menu_key(void) { trap_return(find_key((uint8_t)trap_arg(0))); }

/* MenuEvent(const EventRecord *) -> UInt32: MenuKey for a key down with
   Command. */
static void h_menu_event(void) {
    uint32_t ev = trap_arg(0);
    uint16_t what = gm_r16(ev), mods = gm_r16(ev + 14);
    bool key = (what == KEY_DOWN || what == AUTO_KEY) && (mods & CMD_KEY);
    trap_return(key ? find_key((uint8_t)gm_r32(ev + 2)) : 0);
}

/* IsCmdChar(const EventRecord *, short test) -> Boolean */
static void h_is_cmd_char(void) {
    uint32_t ev = trap_arg(0);
    uint16_t what = gm_r16(ev), mods = gm_r16(ev + 14);
    bool key = (what == KEY_DOWN || what == AUTO_KEY) && (mods & CMD_KEY);
    trap_return(key && (uint8_t)gm_r32(ev + 2) == (uint8_t)trap_arg(1));
}

void menumgr_register(void) {
    trap_register("GetMenu", h_get_menu);
    trap_register("InitMenus", h_init_menus);
    trap_register("OpenDeskAcc", h_open_desk_acc);
    trap_register("SystemClick", h_system_click);
    trap_register("SystemEdit", h_system_edit);
    trap_register("GetMenuItemText", h_get_menu_item_text);
    trap_register("CheckItem", h_check_item);
    trap_register("InsertMenuItem", h_insert_menu_item);
    trap_register("DeleteMenuItem", h_delete_menu_item);
    trap_register("AppendResMenu", h_append_res_menu);
    trap_register("EnableItem", h_enable_menu_item);
    trap_register("DisableItem", h_disable_menu_item);
    trap_register("DisposeMenu", h_dispose_menu);
    trap_register("InsertMenu", h_insert_menu);
    trap_register("DeleteMenu", h_delete_menu);
    trap_register("HiliteMenu", h_hilite_menu);
    trap_register("InvalMenuBar", h_nothing);
    trap_register("DrawMenuBar", h_nothing);
    trap_register("ShowMenuBar", h_show_menu_bar);
    trap_register("HideMenuBar", h_hide_menu_bar);
    trap_register("IsMenuBarVisible", h_is_menu_bar_visible);
    trap_register("CountMenuItems", h_count_menu_items);
    trap_register("EnableMenuItem", h_enable_menu_item);
    trap_register("DisableMenuItem", h_disable_menu_item);
    trap_register("IsMenuItemEnabled", h_is_menu_item_enabled);
    trap_register("EnableMenuCommand", h_enable_menu_command);
    trap_register("DisableMenuCommand", h_disable_menu_command);
    trap_register("GetItemCmd", h_get_item_cmd);
    trap_register("GetItemMark", h_get_item_mark);
    trap_register("SetItemMark", h_set_item_mark);
    trap_register("SetMenuItemText", h_set_menu_item_text);
    trap_register("MenuSelect", h_menu_select);
    trap_register("MenuKey", h_menu_key);
    trap_register("MenuEvent", h_menu_event);
    trap_register("IsCmdChar", h_is_cmd_char);
}
