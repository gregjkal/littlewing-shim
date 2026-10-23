#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The classic Menu Manager, as a PowerPlant game (Golden Logres, Angel
   Egg) uses it. A MenuRef is an opaque ID (MENU_TAG_BASE and up) for a
   menu read from a MENU resource: its title and items, each with its text,
   key equivalent, mark, style and whether it is enabled. The menu bar is
   the list of inserted menus, in order, and whether it shows.

   Nothing draws it: the games play full screen with the menu bar hidden.
   So MenuSelect, which tracks a click in the menu bar, chooses nothing,
   while MenuKey and MenuEvent find the item a Command-key stands for, as
   they would on a Mac, so the game's own shortcuts work. HiliteMenu is
   noted and nothing more.

   (menu.m is the host's own menu bar, the app's, which is another thing.) */

#define MENU_TAG_BASE 0x0E000000u
#define MENU_MAX 32
#define MENU_MAX_ITEMS 64

/* Resets the menus and shows the menu bar. */
void menumgr_init(void);

/* Registers GetMenu, DisposeMenu, InsertMenu, DeleteMenu, HiliteMenu,
   InvalMenuBar, DrawMenuBar, ShowMenuBar, HideMenuBar, IsMenuBarVisible,
   CountMenuItems, EnableMenuItem, DisableMenuItem, IsMenuItemEnabled,
   EnableMenuCommand, DisableMenuCommand, GetItemCmd, GetItemMark,
   SetItemMark, SetMenuItemText, MenuSelect, MenuKey, MenuEvent and
   IsCmdChar. Requires trap_init(). */
void menumgr_register(void);
