#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The classic Menu Manager, as a PowerPlant game (Golden Logres, Angel
   Egg) and a System 7 program (Tristan) use it. A MenuRef is a guest
   handle holding a copy of the menu's MENU resource, which an older
   program looks into (Tristan writes its Apple menu's title there), but
   the menu as the Menu Manager knows it is read from the resource once,
   into the shim: its title and items, each with its text, key
   equivalent, mark, style and whether it is enabled. Changes to the
   handle's bytes don't reach it. The menu bar is
   the list of inserted menus, in order, and whether it shows.

   Nothing draws it here: the games play full screen with the menu bar
   hidden. So MenuSelect, which tracks a click in the menu bar, chooses
   nothing, while MenuKey and MenuEvent find the item a Command-key stands
   for, as they would on a Mac, so the game's own shortcuts work. HiliteMenu
   is noted and nothing more.

   The host's menu bar (menu.m) shows the game's menus instead, read
   through menumgr_bar_menu and menumgr_item, and an item chosen there
   reaches the game as a Command-key would (menumgr_choose). */

#define MENU_MAX 32
#define MENU_MAX_ITEMS 64

/* Resets the menus and shows the menu bar. */
void menumgr_init(void);

/* Registers GetMenu, DisposeMenu, InsertMenu, DeleteMenu, HiliteMenu,
   InvalMenuBar, DrawMenuBar, ShowMenuBar, HideMenuBar, IsMenuBarVisible,
   CountMenuItems, EnableMenuItem, DisableMenuItem, IsMenuItemEnabled,
   EnableMenuCommand, DisableMenuCommand, GetItemCmd, GetItemMark,
   SetItemMark, SetMenuItemText, MenuSelect, MenuKey, MenuEvent and
   IsCmdChar, and the older calls a System 7 program makes: InitMenus,
   EnableItem, DisableItem, CheckItem, GetMenuItemText, InsertMenuItem,
   DeleteMenuItem and AppendResMenu, and the Desk Manager's OpenDeskAcc,
   SystemClick and SystemEdit, with no desk accessories to open. Requires
   trap_init(). */
void menumgr_register(void);

/* For the host's menu bar. A menu in the game's bar, i from 0, left to
   right, and an item of one, from 1: false past the end. Titles and text
   are Mac Roman, and an item's text starting with '-' is a separator. */
typedef struct {
    int16_t id;
    char title[64];
    bool enabled;
    int nitems;
} menumgr_menu_info;

typedef struct {
    char text[64];
    uint8_t key;  /* its Command-key, or 0 */
    bool checked; /* its mark is the check mark */
    bool enabled; /* and so is its menu, and the shim can open what it opens */
} menumgr_item_info;

bool menumgr_bar_menu(int i, menumgr_menu_info *out);
bool menumgr_item(int16_t menu_id, int item, menumgr_item_info *out);

/* The player chose an item in the host's menu bar: the game is sent a
   Command-key down, as on a Mac, for its event loop to pass to MenuKey or
   MenuEvent, which answer with that item (events_post_command_key, which
   pauses a game in play to reach its event loop). The key is the item's own
   Command-key if it has one, else a letter none of the bar's items has.
   Does nothing if the item isn't there and enabled, or a dialog is up.
   Only a game with menus gets here, which leaves out Loony Labyrinth, the
   one game that records, so the choice isn't recorded. */
void menumgr_choose(int16_t menu_id, int item);

/* The choice's Command-key will never reach the game (events.c dropped
   it), so a Command-key typed later stands for its own item again. */
void menumgr_forget_choice(void);
