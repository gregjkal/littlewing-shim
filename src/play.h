#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Where the player is in Loony Labyrinth 3.0.1: in a game or not, and
   playing it or not, read from the game's own state (found with
   tools/disasm.py). A recording logs it (replay.h), so a replay can start
   where a game starts and leave out what isn't play.

   Mach_StartNewGame (code+0x1df90) runs a whole game inside
   Mach_ProcessingLoop, with a flag set (TOC+0x4e36) while the loop runs;
   the self-playing demo comes through it too, with TOC+0x4dd1 set. Esc, or
   the window losing focus (the game asks itself to pause then), ends the
   loop and leaves the game suspended (TOC+0x4e35) under the pause menu
   until Event_ResumeGame runs the loop again. At game over the game sets
   TOC+0x4e34 and the loop ends once the initials of a high score are in.
   Event_ResetGame (code+0x15400) throws away a suspended game, as the menu
   does when a new game starts from it and as the unregistered game does at
   once on Esc.

   So a game starts at Mach_StartNewGame without the demo flag, and ends
   when its loop ends at game over, when Event_ResetGame throws it away, or
   when another starts. It is played while its loop runs and the window has
   focus. */

/* Once the program is loaded, before it runs: watches the game when it is
   game_id "loony-labyrinth" and its code is 3.0.1's (code_base, and toc,
   the program's TOC). Off otherwise, and on a replay, which reads what was
   logged. Returns whether it is on. */
bool play_init(const char *game_id, uint32_t code_base, uint32_t toc);

/* The event pump, once a pump, after the host's input, with whether the
   window has focus: tells the recording (replay_game, replay_play) when a
   game starts or ends and when play goes on or off. */
void play_pump(bool active);

/* Now: in a game, and playing it. */
bool play_in_game(void);
bool play_playing(void);

/* For tests: forget everything. */
void play_reset(void);
