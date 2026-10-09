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
