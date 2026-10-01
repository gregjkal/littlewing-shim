#pragma once
#include <stdbool.h>
#include <stdint.h>

/* SDL scancodes to Mac virtual key codes (US layout) and character codes, and
   Carbon modifier bits. Reference: Inside Macintosh: Text, "Virtual Key
   Codes", and Carbon Events.h. */

/* Carbon modifier bits (kEventParamKeyModifiers). A right-side modifier sets
   both its general bit and its right-side bit. */
#define KM_CMD           0x0100
#define KM_SHIFT         0x0200
#define KM_ALPHA_LOCK    0x0400
#define KM_OPTION        0x0800
#define KM_CONTROL       0x1000
#define KM_RIGHT_SHIFT   0x2000
#define KM_RIGHT_OPTION  0x4000
#define KM_RIGHT_CONTROL 0x8000

typedef struct {
    int vkey;     /* Mac virtual key code, or -1 if the key has none */
    uint8_t chr;  /* Mac Roman character with no modifiers, 0 if none */
    uint8_t shifted; /* character with Shift */
    uint32_t modifier; /* nonzero for a modifier key: its KM_* bits */
} keymap_entry;

/* The mapping for an SDL scancode (SDL_Scancode values). */
keymap_entry keymap_lookup(int scancode);

/* The character a key produces with the given modifier bits. */
uint8_t keymap_char(const keymap_entry *k, uint32_t modifiers);

/* The SDL scancode for a script key name ("z", "slash", "return", "space",
   "esc", "lshift", "rshift", ...), or -1. */
int keymap_scancode_for_name(const char *name);
