#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Scripted input for headless runs (LOONY_SCRIPT). One action per line:
     <tick> down <key>        press a key (names from keymap_scancode_for_name)
     <tick> up <key>          release it
     <tick> screenshot <file> write the screen as a PNG
     <tick> quit              ask the game to quit (the quit Apple Event)
   Blank lines and lines starting with '#' are ignored. Ticks are 1/60 s
   since launch and must not decrease. */

typedef enum { SCRIPT_KEY_DOWN, SCRIPT_KEY_UP, SCRIPT_SCREENSHOT, SCRIPT_QUIT } script_kind;

typedef struct {
    uint32_t tick;
    script_kind kind;
    int scancode;       /* key actions */
    char path[256];     /* screenshot */
} script_action;

/* Parses a script file. On failure writes err (with the line number) and
   returns false. Replaces any script loaded before. */
bool script_load(const char *path, char *err, size_t errlen);

/* Parses script text (for tests). */
bool script_parse(const char *text, char *err, size_t errlen);

/* The next action due at or before tick, removed from the script. False if
   none is due. */
bool script_next(uint32_t tick, script_action *out);

/* Actions not yet taken. */
int script_remaining(void);
