#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "guest_mem.h"
#include "util.h" /* log_msg */

#define PPC_NOP 0x60000000u
#define PPC_LI_R3_0 0x38600000u
#define PPC_B_12 0x4800000Cu /* b .+12 */
#define PPC_B_8 0x48000008u  /* b .+8 */

/* Three words of code as linked, the last of which becomes `last`. */
typedef struct {
    uint32_t addr;
    uint32_t words[3];
    uint32_t last;
} code_patch;

typedef struct {
    const char *game_id;
    const code_patch *patches;
    size_t n;
} game_unlock;

#define GAME(id, patches) {id, patches, sizeof patches / sizeof patches[0]}
#include "games.inc"

static bool matches(const code_patch *c) {
    if (!gm_is_backed(c->addr, sizeof c->words))
        return false;
    for (int w = 0; w < 3; w++)
        if (gm_r32(c->addr + 4 * w) != c->words[w])
            return false;
    return true;
}

void local_patch_hook(const char *game_id) {
    for (size_t g = 0; g < sizeof games / sizeof games[0]; g++) {
        const game_unlock *u = &games[g];
        if (strcmp(game_id, u->game_id) != 0)
            continue;
        for (size_t i = 0; i < u->n; i++)
            if (!matches(&u->patches[i])) {
                log_msg("LOCAL DEV UNLOCK: code at 0x%x isn't the expected version; nothing patched",
                        u->patches[i].addr);
                return;
            }
        for (size_t i = 0; i < u->n; i++)
            gm_w32(u->patches[i].addr + 8, u->patches[i].last);
        log_msg("UNLOCK ACTIVE for %s: %zu patches", game_id, u->n);
        return;
    }
}
