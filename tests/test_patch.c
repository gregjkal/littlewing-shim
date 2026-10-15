#include "test.h"

#include "guest_mem.h"
#include "patch.h"
#include "ppc.h"
#include "trap.h"

/* MONSTER FAIR 1.2.5's key handler, where the second license check's
   verdict sets the flag: "li r0,1; lis r2,6; stw r0,-3164(r2)", twice. */
static void write_mf_verdict_code(void) {
    static const uint32_t code[3] = {0x38000001, 0x3C400006, 0x9002F3A4};
    for (uint32_t i = 0; i < 3; i++) {
        gm_w32(0x41BC8 + 4 * i, code[i]);
        gm_w32(0x41BF4 + 4 * i, code[i]);
    }
}

TEST(patch_mf_license_recheck_drops_the_verdict) {
    gm_init_layout(GM_LAYOUT_MACHO);
    write_mf_verdict_code();
    char err[128] = "";
    CHECK(patch_mf_skip_license_recheck(err, sizeof err));
    CHECK_EQ(gm_r32(0x41BC8), 0x38000001u); /* li r0,1 stays */
    CHECK_EQ(gm_r32(0x41BCC), 0x3C400006u);
    CHECK_EQ(gm_r32(0x41BD0), 0x60000000u); /* the store is a nop */
    CHECK_EQ(gm_r32(0x41BF4), 0x38000001u);
    CHECK_EQ(gm_r32(0x41BFC), 0x60000000u);
}

/* Another version's code is left alone, in both places. */
TEST(patch_mf_license_recheck_refuses_other_code) {
    gm_init_layout(GM_LAYOUT_MACHO);
    write_mf_verdict_code();
    gm_w32(0x41BF4, 0x38000002);
    char err[128] = "";
    CHECK(!patch_mf_skip_license_recheck(err, sizeof err));
    CHECK_STR(err, "the code at 0x41bf4 isn't MONSTER FAIR 1.2.5's");
    CHECK_EQ(gm_r32(0x41BD0), 0x9002F3A4u);
    CHECK_EQ(gm_r32(0x41BFC), 0x9002F3A4u);
}

/* A classic game's layout has no code at those addresses. */
TEST(patch_mf_license_recheck_refuses_the_pef_layout) {
    gm_init();
    char err[128] = "";
    CHECK(!patch_mf_skip_license_recheck(err, sizeof err));
    CHECK_STR(err, "the code at 0x41bc8 isn't MONSTER FAIR 1.2.5's");
}

/* LOONY LABYRINTH 3.0.1, placed as the loader places it: RawKeyDownHandler's
   call to Runtime_Enter for Esc, and KeyCheckQuit with its traceback table.
   Runtime_Enter is a stand-in that returns 0x77, so a test can tell it ran. */
#define LL_CODE GUEST_IMAGE_BASE
#define LL_TOC  (GUEST_IMAGE_BASE + 0x50000)
#define LL_TV   (GUEST_IMAGE_BASE + 0x60000) /* the patched routine's */

static void write_loony_code(void) {
    gm_w32(LL_CODE + 0x23DEC, 0x4BFFFD35); /* bl Runtime_Enter */
    static const uint32_t key_check_quit[] = {
        0x38600000, 0x4E800020,                         /* li r3,0; blr */
        0x00000000, 0x00092040, 0x00000000, 0x00000008, /* its traceback table */
        0x00112E4B, 0x65794368,
    };
    put_words(LL_CODE + 0x22A60, key_check_quit, 8);
    static const uint32_t runtime_enter[] = {0x38600077, 0x4E800020}; /* li r3,0x77; blr */
    put_words(LL_CODE + 0x23B20, runtime_enter, 2);
}

TEST(patch_loony_esc_calls_the_new_routine) {
    gm_init();
    write_loony_code();
    char err[128] = "";
    CHECK(patch_loony_esc_in_cutscenes(LL_CODE, err, sizeof err));
    CHECK_EQ(gm_r32(LL_CODE + 0x23DEC), 0x4BFFEC7Du); /* bl code+0x22a68 */
    CHECK_EQ(gm_r32(LL_CODE + 0x22A60), 0x38600000u); /* KeyCheckQuit is unchanged */
    CHECK_EQ(gm_r32(LL_CODE + 0x22A64), 0x4E800020u);
    CHECK_EQ(gm_r32(LL_CODE + 0x22A7C), 0x480010A4u); /* b Runtime_Enter */
}

/* Runs the patched routine as RawKeyDownHandler does for an Esc. */
static uint32_t press_esc(uint8_t game_runs) {
    gm_w8(LL_TOC + 0x4E36, game_runs);
    gm_w32(LL_TV, LL_CODE + 0x22A68);
    gm_w32(LL_TV + 4, LL_TOC);
    return guest_call(LL_TV, 0, NULL);
}

static void patched_loony_machine(void) {
    fresh_machine();
    trap_init(0, NULL, LL_CODE, 0x50000);
    write_loony_code();
    char err[128] = "";
    CHECK(patch_loony_esc_in_cutscenes(LL_CODE, err, sizeof err));
}

TEST(patch_loony_esc_requests_a_pause_while_a_game_runs) {
    patched_loony_machine();
    CHECK_EQ(press_esc(1), 0x77u); /* Runtime_Enter still runs */
    CHECK_EQ(gm_r8(LL_TOC + 0x4E33), 1);
}

/* In the menus, Esc does what it did. */
TEST(patch_loony_esc_requests_nothing_between_games) {
    patched_loony_machine();
    CHECK_EQ(press_esc(0), 0x77u);
    CHECK_EQ(gm_r8(LL_TOC + 0x4E33), 0);
}

TEST(patch_loony_esc_refuses_other_code) {
    gm_init();
    write_loony_code();
    gm_w32(LL_CODE + 0x22A78, 0);
    char err[128] = "";
    CHECK(!patch_loony_esc_in_cutscenes(LL_CODE, err, sizeof err));
    CHECK_STR(err, "the code at code+0x22a78 isn't LOONY LABYRINTH 3.0.1's");
    CHECK_EQ(gm_r32(LL_CODE + 0x23DEC), 0x4BFFFD35u);
    CHECK_EQ(gm_r32(LL_CODE + 0x22A68), 0x00000000u);
}
