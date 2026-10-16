#pragma once
#include <stddef.h>

/* The Sound menu in the macOS menu bar, before SDL's Window menu: a volume
   slider that sets sound_volume as it moves. The volume is saved
   (SETTINGS_VOLUME) when the menu closes, if it changed. Call once SDL's video
   is initialized, which makes the menu bar; does nothing with a video driver
   other than Cocoa (the tests' dummy), or when called again. In an app
   that bundles Sparkle and names its feed (a release), it also starts
   Sparkle, which checks for updates once a day, and adds Check for
   Updates... to the app menu, after About. An update found while the
   player plays retitles that item ("Update to LittleWing 0.5.0...") rather
   than interrupting with a window. */
void menu_install(void);

/* Adds a High Scores menu after the Sound menu: at once if menu_install
   has run (the picker's window made the menu bar), else when it does. It
   lists the game's high scores as the game holds them when the
   menu opens (game_high_scores, from cf's live preferences), best first.
   Each has a submenu to share the replay of the game that set it, the
   recording kept in rec_dir (replay_find_recording): Share Replay... makes
   a copy without the license (replay_share) and offers it to the share
   sheet (AirDrop, Mail, Messages), and Save Replay... writes that copy
   where the player chooses. A score with no recording, or no date, says
   so. title names the copy ("Loony Labyrinth 430,745,960 GREG.lwreplay"). */
void menu_add_high_scores(const char *game_id, const char *title, const char *rec_dir);

/* Adds a Replay menu after the Sound menu, for watching a replay (watch.h):
   at once if menu_install has run, else when it does. Export Clips and
   Export Marks export the watched recording's clips or marks
   (watch_export), and Cancel Export cancels (watch_cancel_export); each is
   disabled when it can't be chosen. The items have no key equivalents: E
   and Esc are the viewer's own keys, which AppKit would take first. */
void menu_add_replay(void);

/* Shows the n files in Finder, selected. Does nothing with a video driver
   other than Cocoa. */
void menu_reveal(const char *const *paths, size_t n);
