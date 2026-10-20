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
   at once if menu_install has run, else when it does. Its items are the
   viewer's actions, built from watch_bindings: Play/Pause, the steps back
   and ahead, Slower and Faster, Mark In and Mark Out, Export Clips, Export
   Marks and Cancel Export. Each does what its key does (watch_act), is
   enabled when watch_can says, and shows its key as its key equivalent,
   as this keyboard types it. The keys themselves go to the viewer
   (watch_key), and an item ignores its own key, which AppKit would
   otherwise send it as well. */
void menu_add_replay(void);

/* Adds a Replays menu, before SDL's Window menu, and a File menu with Open
   Recent, after the app menu: at once if menu_install has run, else when
   it does; again does nothing. Replays lists the recordings in the
   recordings folder (recordings_dir, recordings_list), read each time it
   opens, newest first: "Oct 15, 8:15 PM, 14:32, 2 marks, clips". Each
   opens the viewer at its start, and under one with marks an indented
   "Play from 3:12" for each opens it at that mark. Show Recordings in
   Finder, at the bottom, makes the folder if it is missing and opens it.
   Open Recent lists the recordings watched lately (menu_note_recent), and
   Clear Menu forgets them. Every recording opens through
   display_request_open. */
void menu_add_replays(void);

/* Takes the Replays and File menus away again (the picker chose a game
   without replays). */
void menu_remove_replays(void);

/* Adds the recording at path, which this process watches, to Open Recent
   (NSDocumentController's recent documents). Does nothing with a video
   driver other than Cocoa. */
void menu_note_recent(const char *path);

/* Shows the n files in Finder, selected. Does nothing with a video driver
   other than Cocoa. */
void menu_reveal(const char *const *paths, size_t n);
