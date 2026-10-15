#include "patch.h"

#include <stdint.h>
#include <stdio.h>

#include "guest_mem.h"

#define PPC_NOP 0x60000000u

/* Three words of code as linked, the last of which becomes a nop. */
typedef struct {
    uint32_t addr;
    uint32_t words[3];
} nop_out;

/* MONSTER FAIR 1.2.5's key handler (code+0x41b3c), after the second
   license check: "li r0,1; lis r2,6; stw r0,-3164(r2)" sets the flag at
   0x5f3a4 when the blacklist (code+0x42310) or the recomputation
   (code+0x42be0) refuses the license. These are the flag's only stores.
   The checks themselves still run: main calls code+0x42310 at launch for
   what it sets up, which the Register window's check needs. */
static const nop_out mf_recheck_verdicts[] = {
    {0x41BC8, {0x38000001, 0x3C400006, 0x9002F3A4}},
    {0x41BF4, {0x38000001, 0x3C400006, 0x9002F3A4}},
};

static bool is_linked_code(const nop_out *c) {
    if (!gm_is_backed(c->addr, sizeof c->words))
        return false;
    for (uint32_t w = 0; w < 3; w++)
        if (gm_r32(c->addr + 4 * w) != c->words[w])
            return false;
    return true;
}

bool patch_mf_skip_license_recheck(char *err, size_t errlen) {
    const size_t n = sizeof mf_recheck_verdicts / sizeof mf_recheck_verdicts[0];
    for (size_t i = 0; i < n; i++)
        if (!is_linked_code(&mf_recheck_verdicts[i])) {
            snprintf(err, errlen, "the code at 0x%x isn't MONSTER FAIR 1.2.5's", mf_recheck_verdicts[i].addr);
            return false;
        }
    for (size_t i = 0; i < n; i++)
        gm_w32(mf_recheck_verdicts[i].addr + 8, PPC_NOP);
    return true;
}

/* A word of code as linked, and what it becomes. */
typedef struct {
    uint32_t off; /* from the start of the code section */
    uint32_t linked, patched;
} replace_word;

/* LOONY LABYRINTH 3.0.1. RawKeyDownHandler (code+0x23d40) calls
   Runtime_Enter (code+0x23b20) for Esc, which opens the menu only when no
   game runs. It calls a new routine instead, written over the traceback
   table of the stub KeyCheckQuit (code+0x22a60), which only debuggers read.
   While a game runs (r2+0x4e36, set by Mach_StartNewGame and
   Mach_ResumePrevGame around the play loop) the routine sets the pause
   request (r2+0x4e33, as Control_PauseRequest at code+0x14fe0 does), then
   goes on to Runtime_Enter. Mach_ProcessingLoop (code+0x1c600) pauses on
   that request at its next pass, and starting or resuming a game clears
   it. */
static const replace_word loony_esc_in_cutscenes[] = {
    {0x23DEC, 0x4BFFFD35, 0x4BFFEC7D}, /* bl Runtime_Enter -> bl code+0x22a68 */
    {0x22A68, 0x00000000, 0x88024E36}, /* lbz r0,0x4e36(r2): a game runs */
    {0x22A6C, 0x00092040, 0x28000000}, /* cmplwi r0,0 */
    {0x22A70, 0x00000000, 0x4182000C}, /* beq 1f */
    {0x22A74, 0x00000008, 0x38000001}, /* li r0,1 */
    {0x22A78, 0x00112E4B, 0x98024E33}, /* stb r0,0x4e33(r2) */
    {0x22A7C, 0x65794368, 0x480010A4}, /* 1: b Runtime_Enter */
};

bool patch_loony_esc_in_cutscenes(uint32_t code_base, char *err, size_t errlen) {
    const size_t n = sizeof loony_esc_in_cutscenes / sizeof loony_esc_in_cutscenes[0];
    for (size_t i = 0; i < n; i++) {
        const replace_word *r = &loony_esc_in_cutscenes[i];
        if (!gm_is_backed(code_base + r->off, 4) || gm_r32(code_base + r->off) != r->linked) {
            snprintf(err, errlen, "the code at code+0x%x isn't LOONY LABYRINTH 3.0.1's", r->off);
            return false;
        }
    }
    for (size_t i = 0; i < n; i++)
        gm_w32(code_base + loony_esc_in_cutscenes[i].off, loony_esc_in_cutscenes[i].patched);
    return true;
}
