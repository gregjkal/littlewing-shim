#include "test.h"

#include <SDL3/SDL_scancode.h>

#include "keymap.h"

TEST(keymap_game_keys) {
    keymap_entry z = keymap_lookup(SDL_SCANCODE_Z);
    CHECK_EQ(z.vkey, 0x06);
    CHECK_EQ(z.chr, 'z');
    CHECK_EQ(z.shifted, 'Z');
    CHECK_EQ(z.modifier, 0);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_SLASH).vkey, 0x2C);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_SLASH).chr, '/');
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_RETURN).vkey, 0x24);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_RETURN).chr, 0x0D);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_KP_ENTER).vkey, 0x4C);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_SPACE).vkey, 0x31);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_ESCAPE).vkey, 0x35);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_ESCAPE).chr, 0x1B);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_LEFT).chr, 0x1C);
}

TEST(keymap_modifiers_distinguish_left_and_right) {
    keymap_entry l = keymap_lookup(SDL_SCANCODE_LSHIFT), r = keymap_lookup(SDL_SCANCODE_RSHIFT);
    CHECK_EQ(l.vkey, 0x38);
    CHECK_EQ(l.modifier, KM_SHIFT);
    CHECK_EQ(r.vkey, 0x3C);
    CHECK_EQ(r.modifier, KM_SHIFT | KM_RIGHT_SHIFT);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_LGUI).modifier, KM_CMD);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_RGUI).vkey, 0x36);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_RCTRL).modifier, KM_CONTROL | KM_RIGHT_CONTROL);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_RALT).modifier, KM_OPTION | KM_RIGHT_OPTION);
}

TEST(keymap_unknown_scancodes_have_no_key) {
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_UNKNOWN).vkey, -1);
    CHECK_EQ(keymap_lookup(SDL_SCANCODE_AC_BACK).vkey, -1);
}

TEST(keymap_characters_follow_shift_and_caps_lock) {
    keymap_entry a = keymap_lookup(SDL_SCANCODE_A), one = keymap_lookup(SDL_SCANCODE_1);
    CHECK_EQ(keymap_char(&a, 0), 'a');
    CHECK_EQ(keymap_char(&a, KM_SHIFT), 'A');
    CHECK_EQ(keymap_char(&a, KM_ALPHA_LOCK), 'A');
    CHECK_EQ(keymap_char(&one, KM_ALPHA_LOCK), '1');
    CHECK_EQ(keymap_char(&one, KM_SHIFT | KM_RIGHT_SHIFT), '!');
}

TEST(keymap_script_names) {
    CHECK_EQ(keymap_scancode_for_name("slash"), SDL_SCANCODE_SLASH);
    CHECK_EQ(keymap_scancode_for_name("RETURN"), SDL_SCANCODE_RETURN);
    CHECK_EQ(keymap_scancode_for_name("rshift"), SDL_SCANCODE_RSHIFT);
    CHECK_EQ(keymap_scancode_for_name("nope"), -1);
}
