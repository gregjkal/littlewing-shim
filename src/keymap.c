#include "keymap.h"

#include <SDL3/SDL_scancode.h>
#include <string.h>
#include <strings.h>

typedef struct {
    int scancode, vkey;
    uint8_t chr, shifted;
    uint32_t modifier;
    const char *name;
} row;

/* Character codes for keys without a printable character: Return 0x0D,
   keypad Enter 0x03, Tab 0x09, Escape 0x1B, Delete 0x08, forward delete
   0x7F, arrows 0x1C-0x1F, function keys 0x10. */
static const row table[] = {
    {SDL_SCANCODE_A, 0x00, 'a', 'A', 0, "a"},
    {SDL_SCANCODE_S, 0x01, 's', 'S', 0, "s"},
    {SDL_SCANCODE_D, 0x02, 'd', 'D', 0, "d"},
    {SDL_SCANCODE_F, 0x03, 'f', 'F', 0, "f"},
    {SDL_SCANCODE_H, 0x04, 'h', 'H', 0, "h"},
    {SDL_SCANCODE_G, 0x05, 'g', 'G', 0, "g"},
    {SDL_SCANCODE_Z, 0x06, 'z', 'Z', 0, "z"},
    {SDL_SCANCODE_X, 0x07, 'x', 'X', 0, "x"},
    {SDL_SCANCODE_C, 0x08, 'c', 'C', 0, "c"},
    {SDL_SCANCODE_V, 0x09, 'v', 'V', 0, "v"},
    {SDL_SCANCODE_B, 0x0B, 'b', 'B', 0, "b"},
    {SDL_SCANCODE_Q, 0x0C, 'q', 'Q', 0, "q"},
    {SDL_SCANCODE_W, 0x0D, 'w', 'W', 0, "w"},
    {SDL_SCANCODE_E, 0x0E, 'e', 'E', 0, "e"},
    {SDL_SCANCODE_R, 0x0F, 'r', 'R', 0, "r"},
    {SDL_SCANCODE_Y, 0x10, 'y', 'Y', 0, "y"},
    {SDL_SCANCODE_T, 0x11, 't', 'T', 0, "t"},
    {SDL_SCANCODE_1, 0x12, '1', '!', 0, "1"},
    {SDL_SCANCODE_2, 0x13, '2', '@', 0, "2"},
    {SDL_SCANCODE_3, 0x14, '3', '#', 0, "3"},
    {SDL_SCANCODE_4, 0x15, '4', '$', 0, "4"},
    {SDL_SCANCODE_6, 0x16, '6', '^', 0, "6"},
    {SDL_SCANCODE_5, 0x17, '5', '%', 0, "5"},
    {SDL_SCANCODE_EQUALS, 0x18, '=', '+', 0, "equals"},
    {SDL_SCANCODE_9, 0x19, '9', '(', 0, "9"},
    {SDL_SCANCODE_7, 0x1A, '7', '&', 0, "7"},
    {SDL_SCANCODE_MINUS, 0x1B, '-', '_', 0, "minus"},
    {SDL_SCANCODE_8, 0x1C, '8', '*', 0, "8"},
    {SDL_SCANCODE_0, 0x1D, '0', ')', 0, "0"},
    {SDL_SCANCODE_RIGHTBRACKET, 0x1E, ']', '}', 0, "rightbracket"},
    {SDL_SCANCODE_O, 0x1F, 'o', 'O', 0, "o"},
    {SDL_SCANCODE_U, 0x20, 'u', 'U', 0, "u"},
    {SDL_SCANCODE_LEFTBRACKET, 0x21, '[', '{', 0, "leftbracket"},
    {SDL_SCANCODE_I, 0x22, 'i', 'I', 0, "i"},
    {SDL_SCANCODE_P, 0x23, 'p', 'P', 0, "p"},
    {SDL_SCANCODE_RETURN, 0x24, 0x0D, 0x0D, 0, "return"},
    {SDL_SCANCODE_L, 0x25, 'l', 'L', 0, "l"},
    {SDL_SCANCODE_J, 0x26, 'j', 'J', 0, "j"},
    {SDL_SCANCODE_APOSTROPHE, 0x27, '\'', '"', 0, "apostrophe"},
    {SDL_SCANCODE_K, 0x28, 'k', 'K', 0, "k"},
    {SDL_SCANCODE_SEMICOLON, 0x29, ';', ':', 0, "semicolon"},
    {SDL_SCANCODE_BACKSLASH, 0x2A, '\\', '|', 0, "backslash"},
    {SDL_SCANCODE_COMMA, 0x2B, ',', '<', 0, "comma"},
    {SDL_SCANCODE_SLASH, 0x2C, '/', '?', 0, "slash"},
    {SDL_SCANCODE_N, 0x2D, 'n', 'N', 0, "n"},
    {SDL_SCANCODE_M, 0x2E, 'm', 'M', 0, "m"},
    {SDL_SCANCODE_PERIOD, 0x2F, '.', '>', 0, "period"},
    {SDL_SCANCODE_TAB, 0x30, 0x09, 0x09, 0, "tab"},
    {SDL_SCANCODE_SPACE, 0x31, ' ', ' ', 0, "space"},
    {SDL_SCANCODE_GRAVE, 0x32, '`', '~', 0, "grave"},
    {SDL_SCANCODE_BACKSPACE, 0x33, 0x08, 0x08, 0, "backspace"},
    {SDL_SCANCODE_ESCAPE, 0x35, 0x1B, 0x1B, 0, "esc"},
    {SDL_SCANCODE_RGUI, 0x36, 0, 0, KM_CMD, "rcmd"},
    {SDL_SCANCODE_LGUI, 0x37, 0, 0, KM_CMD, "lcmd"},
    {SDL_SCANCODE_LSHIFT, 0x38, 0, 0, KM_SHIFT, "lshift"},
    {SDL_SCANCODE_CAPSLOCK, 0x39, 0, 0, KM_ALPHA_LOCK, "capslock"},
    {SDL_SCANCODE_LALT, 0x3A, 0, 0, KM_OPTION, "loption"},
    {SDL_SCANCODE_LCTRL, 0x3B, 0, 0, KM_CONTROL, "lcontrol"},
    {SDL_SCANCODE_RSHIFT, 0x3C, 0, 0, KM_SHIFT | KM_RIGHT_SHIFT, "rshift"},
    {SDL_SCANCODE_RALT, 0x3D, 0, 0, KM_OPTION | KM_RIGHT_OPTION, "roption"},
    {SDL_SCANCODE_RCTRL, 0x3E, 0, 0, KM_CONTROL | KM_RIGHT_CONTROL, "rcontrol"},
    {SDL_SCANCODE_KP_PERIOD, 0x41, '.', '.', 0, "kp_period"},
    {SDL_SCANCODE_KP_MULTIPLY, 0x43, '*', '*', 0, "kp_multiply"},
    {SDL_SCANCODE_KP_PLUS, 0x45, '+', '+', 0, "kp_plus"},
    {SDL_SCANCODE_NUMLOCKCLEAR, 0x47, 0x1B, 0x1B, 0, "kp_clear"},
    {SDL_SCANCODE_KP_DIVIDE, 0x4B, '/', '/', 0, "kp_divide"},
    {SDL_SCANCODE_KP_ENTER, 0x4C, 0x03, 0x03, 0, "enter"},
    {SDL_SCANCODE_KP_MINUS, 0x4E, '-', '-', 0, "kp_minus"},
    {SDL_SCANCODE_KP_EQUALS, 0x51, '=', '=', 0, "kp_equals"},
    {SDL_SCANCODE_KP_0, 0x52, '0', '0', 0, "kp_0"},
    {SDL_SCANCODE_KP_1, 0x53, '1', '1', 0, "kp_1"},
    {SDL_SCANCODE_KP_2, 0x54, '2', '2', 0, "kp_2"},
    {SDL_SCANCODE_KP_3, 0x55, '3', '3', 0, "kp_3"},
    {SDL_SCANCODE_KP_4, 0x56, '4', '4', 0, "kp_4"},
    {SDL_SCANCODE_KP_5, 0x57, '5', '5', 0, "kp_5"},
    {SDL_SCANCODE_KP_6, 0x58, '6', '6', 0, "kp_6"},
    {SDL_SCANCODE_KP_7, 0x59, '7', '7', 0, "kp_7"},
    {SDL_SCANCODE_KP_8, 0x5B, '8', '8', 0, "kp_8"},
    {SDL_SCANCODE_KP_9, 0x5C, '9', '9', 0, "kp_9"},
    {SDL_SCANCODE_F5, 0x60, 0x10, 0x10, 0, "f5"},
    {SDL_SCANCODE_F6, 0x61, 0x10, 0x10, 0, "f6"},
    {SDL_SCANCODE_F7, 0x62, 0x10, 0x10, 0, "f7"},
    {SDL_SCANCODE_F3, 0x63, 0x10, 0x10, 0, "f3"},
    {SDL_SCANCODE_F8, 0x64, 0x10, 0x10, 0, "f8"},
    {SDL_SCANCODE_F9, 0x65, 0x10, 0x10, 0, "f9"},
    {SDL_SCANCODE_F11, 0x67, 0x10, 0x10, 0, "f11"},
    {SDL_SCANCODE_F10, 0x6D, 0x10, 0x10, 0, "f10"},
    {SDL_SCANCODE_F12, 0x6F, 0x10, 0x10, 0, "f12"},
    {SDL_SCANCODE_HOME, 0x73, 0x01, 0x01, 0, "home"},
    {SDL_SCANCODE_PAGEUP, 0x74, 0x0B, 0x0B, 0, "pageup"},
    {SDL_SCANCODE_DELETE, 0x75, 0x7F, 0x7F, 0, "delete"},
    {SDL_SCANCODE_F4, 0x76, 0x10, 0x10, 0, "f4"},
    {SDL_SCANCODE_END, 0x77, 0x04, 0x04, 0, "end"},
    {SDL_SCANCODE_F2, 0x78, 0x10, 0x10, 0, "f2"},
    {SDL_SCANCODE_PAGEDOWN, 0x79, 0x0C, 0x0C, 0, "pagedown"},
    {SDL_SCANCODE_F1, 0x7A, 0x10, 0x10, 0, "f1"},
    {SDL_SCANCODE_LEFT, 0x7B, 0x1C, 0x1C, 0, "left"},
    {SDL_SCANCODE_RIGHT, 0x7C, 0x1D, 0x1D, 0, "right"},
    {SDL_SCANCODE_DOWN, 0x7D, 0x1F, 0x1F, 0, "down"},
    {SDL_SCANCODE_UP, 0x7E, 0x1E, 0x1E, 0, "up"},
};

keymap_entry keymap_lookup(int scancode) {
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (table[i].scancode == scancode)
            return (keymap_entry){table[i].vkey, table[i].chr, table[i].shifted, table[i].modifier};
    return (keymap_entry){-1, 0, 0, 0};
}

uint8_t keymap_char(const keymap_entry *k, uint32_t modifiers) {
    bool shift = (modifiers & KM_SHIFT) != 0;
    bool caps = (modifiers & KM_ALPHA_LOCK) != 0 && k->chr >= 'a' && k->chr <= 'z';
    return (shift || caps) ? k->shifted : k->chr;
}

int keymap_scancode_for_name(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strcasecmp(table[i].name, name) == 0)
            return table[i].scancode;
    return -1;
}
