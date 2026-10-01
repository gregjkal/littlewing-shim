#include "dialogs.h"

#include <stdio.h>
#include <string.h>

#include "guest_mem.h"
#include "rsrc.h"
#include "trap.h"
#include "util.h"

#define ITEM_STATIC_TEXT 8
#define ITEM_BUTTON 4

static char param[4][256]; /* ParamText ^0..^3 */

void dialogs_init(void) { memset(param, 0, sizeof param); }

/* Appends text to out, replacing ^0..^3 with the ParamText strings and
   carriage returns with spaces. */
static void append(char *out, size_t cap, const uint8_t *text, size_t n) {
    size_t o = strlen(out);
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        if (text[i] == '^' && i + 1 < n && text[i + 1] >= '0' && text[i + 1] <= '3') {
            const char *p = param[text[++i] - '0'];
            while (*p && o + 1 < cap)
                out[o++] = *p++;
        } else {
            out[o++] = text[i] == '\r' ? ' ' : (char)text[i];
        }
    }
    out[o] = '\0';
}

void dialogs_alert_text(int16_t id, char *out, size_t cap) {
    out[0] = '\0';
    rsrc_entry *alrt = rsrc_find(FOURCC('A', 'L', 'R', 'T'), id);
    if (!alrt || alrt->len < 12)
        return;
    rsrc_entry *ditl = rsrc_find(FOURCC('D', 'I', 'T', 'L'), (int16_t)rd_be16(rsrc_data(alrt) + 8));
    if (!ditl || ditl->len < 2)
        return;
    const uint8_t *d = rsrc_data(ditl);
    uint32_t n = rd_be16(d) + 1u, p = 2;
    for (uint32_t i = 0; i < n; i++) {
        if (p + 14 > ditl->len)
            return;
        uint8_t type = d[p + 12] & 0x7F, len = d[p + 13];
        p += 14;
        if (p + len > ditl->len)
            return;
        if (type == ITEM_STATIC_TEXT || type == ITEM_BUTTON) {
            if (out[0])
                append(out, cap, (const uint8_t *)" | ", 3);
            append(out, cap, d + p, len);
        }
        p += len + (len & 1u);
    }
}

/* The default item: bit 3 of the first stage's 4 bits in the ALRT's stages
   word picks item 2, otherwise item 1. */
static void h_alert(void) {
    int16_t id = (int16_t)trap_arg(0);
    rsrc_entry *alrt = rsrc_find(FOURCC('A', 'L', 'R', 'T'), id);
    if (!alrt || alrt->len < 12)
        trap_crash("Alert: ALRT %d doesn't exist", id);
    uint16_t stages = rd_be16(rsrc_data(alrt) + 10);
    int item = (stages & 0x8) ? 2 : 1;
    char text[1024];
    dialogs_alert_text(id, text, sizeof text);
    log_msg("Alert %d (answering item %d): %s", id, item, text);
    trap_return((uint32_t)item);
}

static void h_param_text(void) {
    for (int i = 0; i < 4; i++) {
        uint32_t s = trap_arg(i);
        if (s)
            gm_read_pstr(s, param[i]);
        else
            param[i][0] = '\0';
    }
}

void dialogs_register(void) {
    trap_register("Alert", h_alert);
    trap_register("StopAlert", h_alert);
    trap_register("ParamText", h_param_text);
}
