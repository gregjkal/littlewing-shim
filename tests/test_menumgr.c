#include "test.h"

#include <stdlib.h>

#include "cgimage.h"
#include "files.h"
#include "harness.h"
#include "memmgr.h"
#include "menumgr.h"
#include "misc.h"
#include "mixer.h"
#include "rsrc.h"
#include "snapshot.h"
#include "util.h"

static const char *const names[] = {
    "GetMenu", "DisposeMenu", "InsertMenu", "DeleteMenu", "HiliteMenu", "ShowMenuBar",
    "HideMenuBar", "IsMenuBarVisible", "CountMenuItems", "EnableMenuItem", "DisableMenuItem",
    "IsMenuItemEnabled", "GetItemCmd", "GetItemMark", "SetItemMark", "SetMenuItemText",
    "MenuSelect", "MenuKey", "MenuEvent", "IsCmdChar", "GetCCursor", "InitMenus",
    "GetMenuItemText", "CheckItem", "InsertMenuItem", "DeleteMenuItem", "AppendResMenu",
    "EnableItem", "DisableItem", "OpenDeskAcc", "SystemClick", "SystemEdit",
};

/* ---- a resource fork: File (MENU 128), Operation (MENU 129), crsr 128 ---- */

typedef struct {
    uint32_t type;
    int16_t id;
    const uint8_t *data;
    uint32_t len;
} res;

static const uint8_t file_menu[] = {
    0, 128, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFB, /* item 2 disabled */
    4, 'F', 'i', 'l', 'e',
    3, 'N', 'e', 'w', 0, 'N', 0, 0,
    4, 'O', 'p', 'e', 'n', 0, 'O', 0, 0,
    4, 'Q', 'u', 'i', 't', 0, 'Q', 0, 0,
    0,
};

static const uint8_t operation_menu[] = {
    0, 129, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF,
    9, 'O', 'p', 'e', 'r', 'a', 't', 'i', 'o', 'n',
    5, 'P', 'a', 'u', 's', 'e', 0, 'p', 0x12, 0,
    0,
};

static const uint8_t cursor[] = {0x80, 0x01, 1, 2, 3, 4};

/* A resource fork holding rs, those of a type next to each other, as
   rsrc_open reads one. The caller keeps it until the test ends. */
static uint8_t *build_fork(const res *rs, int n, size_t *len) {
    int ntypes = 0;
    uint32_t data_len = 0;
    for (int i = 0; i < n; i++) {
        data_len += 4 + rs[i].len;
        ntypes += i == 0 || rs[i].type != rs[i - 1].type;
    }
    uint32_t type_list = 28, refs = type_list + 2 + 8u * (uint32_t)ntypes;
    uint32_t map_len = refs + 12u * (uint32_t)n;
    uint32_t data_off = 256, map_off = data_off + data_len;
    *len = map_off + map_len;
    uint8_t *f = calloc(1, *len);
    wr_be32(f, data_off);
    wr_be32(f + 4, map_off);
    wr_be32(f + 8, data_len);
    wr_be32(f + 12, map_len);
    uint8_t *m = f + map_off;
    wr_be16(m + 24, (uint16_t)type_list);
    wr_be16(m + 26, (uint16_t)map_len);
    wr_be16(m + type_list, (uint16_t)(ntypes - 1));
    uint32_t at = 0;
    int t = -1;
    for (int i = 0; i < n; i++) {
        uint8_t *r = m + refs + 12 * i;
        if (i == 0 || rs[i].type != rs[i - 1].type) {
            uint8_t *e = m + type_list + 2 + 8 * ++t;
            int count = 0;
            while (i + count < n && rs[i + count].type == rs[i].type)
                count++;
            wr_be32(e, rs[i].type);
            wr_be16(e + 4, (uint16_t)(count - 1));
            wr_be16(e + 6, (uint16_t)(refs + 12 * (uint32_t)i - type_list));
        }
        wr_be16(r, (uint16_t)rs[i].id);
        wr_be16(r + 2, 0xFFFF);
        wr_be32(r + 4, at);
        wr_be32(f + data_off + at, rs[i].len);
        memcpy(f + data_off + at + 4, rs[i].data, rs[i].len);
        at += 4 + rs[i].len;
    }
    return f;
}

static uint8_t *fork_buf;

static void setup(void) {
    harness_init_direct(names, sizeof names / sizeof names[0]);
    mm_init();
    const res rs[] = {
        {FOURCC('M', 'E', 'N', 'U'), 128, file_menu, sizeof file_menu},
        {FOURCC('M', 'E', 'N', 'U'), 129, operation_menu, sizeof operation_menu},
        {FOURCC('c', 'r', 's', 'r'), 128, cursor, sizeof cursor},
    };
    size_t len;
    free(fork_buf);
    fork_buf = build_fork(rs, 3, &len);
    char err[256];
    if (!rsrc_open(fork_buf, len, err, sizeof err))
        fatal("rsrc_open: %s", err);
    misc_init();
    misc_register();
    menumgr_init();
    menumgr_register();
}

#define ITEM(menu, item) ((uint32_t)(menu) << 16 | (item))

TEST(menumgr_get_menu_reads_the_items) {
    setup();
    uint32_t m = call_import("GetMenu", 1, 128u);
    CHECK(mm_is_handle(m));
    CHECK_EQ(gm_r16(gm_r32(m)), 128); /* the MENU resource's menuID */
    CHECK_EQ(call_import("CountMenuItems", 1, m), 3);
    CHECK_EQ(call_import("IsMenuItemEnabled", 2, m, 1u), 1);
    CHECK_EQ(call_import("IsMenuItemEnabled", 2, m, 2u), 0);
    uint32_t c = scratch(2);
    call_import("GetItemCmd", 3, m, 3u, c);
    CHECK_EQ(gm_r16(c), 'Q');
    call_import("SetItemMark", 3, m, 1u, 0x12u);
    call_import("GetItemMark", 3, m, 1u, c);
    CHECK_EQ(gm_r16(c), 0x12);
    call_import("SetMenuItemText", 3, m, 1u, scratch(256)); /* an empty name */
    CHECK_EQ(call_import("GetMenu", 1, 999u), 0);
    call_import("DisposeMenu", 1, m);
}

TEST(menumgr_menu_key_finds_enabled_items_in_the_bar) {
    setup();
    uint32_t file = call_import("GetMenu", 1, 128u);
    CHECK_EQ(call_import("MenuKey", 1, (uint32_t)'q'), 0); /* not in the bar yet */
    call_import("InsertMenu", 2, file, 0u);
    CHECK_EQ(call_import("MenuKey", 1, (uint32_t)'q'), ITEM(128, 3));
    CHECK_EQ(call_import("MenuKey", 1, (uint32_t)'O'), 0); /* disabled */
    call_import("EnableMenuItem", 2, file, 2u);
    CHECK_EQ(call_import("MenuKey", 1, (uint32_t)'o'), ITEM(128, 2));
    call_import("DisableMenuItem", 2, file, 0u); /* the whole menu */
    CHECK_EQ(call_import("MenuKey", 1, (uint32_t)'q'), 0);
    call_import("EnableMenuItem", 2, file, 0u);

    uint32_t op = call_import("GetMenu", 1, 129u);
    call_import("InsertMenu", 2, op, 128u); /* before File */
    CHECK_EQ(call_import("MenuKey", 1, (uint32_t)'P'), ITEM(129, 1));
    call_import("DeleteMenu", 1, 128u);
    CHECK_EQ(call_import("MenuKey", 1, (uint32_t)'q'), 0);
    CHECK_EQ(call_import("MenuKey", 1, (uint32_t)'p'), ITEM(129, 1));
    call_import("HiliteMenu", 1, 0u);
}

static uint32_t key_record(uint8_t c, uint16_t modifiers) {
    uint32_t r = scratch(16);
    gm_w16(r, 3); /* keyDown */
    gm_w32(r + 2, (uint32_t)0x0C << 8 | c);
    gm_w16(r + 14, modifiers);
    return r;
}

TEST(menumgr_menu_event_and_is_cmd_char_want_command) {
    setup();
    call_import("InsertMenu", 2, call_import("GetMenu", 1, 128u), 0u);
    CHECK_EQ(call_import("MenuEvent", 1, key_record('q', 0x0100)), ITEM(128, 3));
    CHECK_EQ(call_import("MenuEvent", 1, key_record('q', 0)), 0);
    CHECK_EQ(call_import("IsCmdChar", 2, key_record('.', 0x0100), (uint32_t)'.'), 1);
    CHECK_EQ(call_import("IsCmdChar", 2, key_record('.', 0), (uint32_t)'.'), 0);
    CHECK_EQ(call_import("MenuSelect", 1, 0x00050005u), 0); /* no bar on screen to click */
}

TEST(menumgr_the_menu_bar_shows_until_hidden) {
    setup();
    CHECK_EQ(call_import("IsMenuBarVisible", 0), 1);
    call_import("HideMenuBar", 0);
    CHECK_EQ(call_import("IsMenuBarVisible", 0), 0);
    call_import("ShowMenuBar", 0);
    CHECK_EQ(call_import("IsMenuBarVisible", 0), 1);
}

TEST(menumgr_get_ccursor_copies_the_crsr) {
    setup();
    uint32_t h = call_import("GetCCursor", 1, 128u);
    CHECK(mm_is_handle(h));
    CHECK_EQ(mm_handle_size(h), sizeof cursor);
    CHECK_EQ(gm_r8(gm_r32(h) + 2), 1);
    CHECK_EQ(call_import("GetCCursor", 1, 129u), 0);
}

static void child_not_a_menu(void *unused) {
    (void)unused;
    setup();
    call_import("CountMenuItems", 1, 0x1234u);
}

TEST(menumgr_calls_crash_on_what_isnt_a_menu) {
    char out[8192];
    CHECK_EQ(test_run_child(child_not_a_menu, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "CountMenuItems: 0x00001234 is not a menu");
}

/* The menus go back with a snapshot. */
TEST(menumgr_menus_round_trip_through_a_snapshot) {
    setup();
    const char *dir = getenv("LOONY_DATA_DIR");
    files_init(dir, dir); /* the other modules a snapshot walks, set up */
    mix_init();
    cgimage_init();
    call_import("InsertMenu", 2, call_import("GetMenu", 1, 128u), 0u);
    size_t len;
    uint8_t *saved = snapshot_state_save(&len);
    call_import("DeleteMenu", 1, 128u);
    CHECK_EQ(call_import("MenuKey", 1, (uint32_t)'q'), 0);
    snapshot_state_load(saved, len);
    CHECK_EQ(call_import("MenuKey", 1, (uint32_t)'q'), ITEM(128, 3));
    free(saved);
}

static const char *item_text(uint32_t m, int item) {
    static char s[256];
    uint32_t p = scratch(256);
    call_import("GetMenuItemText", 3, m, (uint32_t)item, p);
    gm_read_pstr(p, s);
    return s;
}

/* The calls a System 7 program (Tristan) makes on its menus. */
TEST(menumgr_classic_item_calls) {
    setup();
    call_import("InitMenus", 0);
    uint32_t m = call_import("GetMenu", 1, 128u);
    CHECK_STR(item_text(m, 2), "Open");
    CHECK_STR(item_text(m, 9), "");
    call_import("CheckItem", 3, m, 1u, 1u);
    uint32_t c = scratch(2);
    call_import("GetItemMark", 3, m, 1u, c);
    CHECK_EQ(gm_r16(c), 0x12);
    call_import("CheckItem", 3, m, 1u, 0u);
    call_import("GetItemMark", 3, m, 1u, c);
    CHECK_EQ(gm_r16(c), 0);
    call_import("EnableItem", 2, m, 2u);
    CHECK_EQ(call_import("IsMenuItemEnabled", 2, m, 2u), 1);
    call_import("DisableItem", 2, m, 3u);
    CHECK_EQ(call_import("IsMenuItemEnabled", 2, m, 3u), 0);

    uint32_t text = scratch(256);
    gm_write_pstr(text, "(Sound/S;Music!x");
    call_import("InsertMenuItem", 3, m, text, 1u);
    CHECK_EQ(call_import("CountMenuItems", 1, m), 5);
    CHECK_STR(item_text(m, 2), "Sound");
    CHECK_EQ(call_import("IsMenuItemEnabled", 2, m, 2u), 0);
    call_import("GetItemCmd", 3, m, 2u, c);
    CHECK_EQ(gm_r16(c), 'S');
    CHECK_STR(item_text(m, 3), "Music");
    call_import("GetItemMark", 3, m, 3u, c);
    CHECK_EQ(gm_r16(c), 'x');
    CHECK_STR(item_text(m, 4), "Open");
    gm_write_pstr(text, "Last");
    call_import("InsertMenuItem", 3, m, text, 99u);
    CHECK_STR(item_text(m, 6), "Last");

    call_import("DeleteMenuItem", 2, m, 2u);
    CHECK_EQ(call_import("CountMenuItems", 1, m), 5);
    CHECK_STR(item_text(m, 2), "Music");
    call_import("DeleteMenuItem", 2, m, 9u);
    CHECK_EQ(call_import("CountMenuItems", 1, m), 5);

    call_import("AppendResMenu", 2, m, FOURCC('D', 'R', 'V', 'R')); /* no desk accessories */
    CHECK_EQ(call_import("CountMenuItems", 1, m), 5);
    uint32_t name = scratch(256);
    gm_write_pstr(name, "Calculator");
    CHECK_EQ(call_import("OpenDeskAcc", 1, name), 0);
    call_import("SystemClick", 2, scratch(16), 0u);
    CHECK_EQ(call_import("SystemEdit", 1, 2u), 0); /* the game edits */
}
