#include "test.h"

#include "guest_mem.h"
#include "patch.h"

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
