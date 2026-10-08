#pragma once

/* The Sound menu in the macOS menu bar, before SDL's Window menu: a volume
   slider that sets sound_volume as it moves. The volume is saved
   (SETTINGS_VOLUME) when the menu closes, if it changed. Call once SDL's video
   is initialized, which makes the menu bar; does nothing with a video driver
   other than Cocoa (the tests' dummy), or when called again. */
void menu_install(void);
