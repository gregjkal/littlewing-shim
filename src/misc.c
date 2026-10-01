#include "misc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "guest_mem.h"
#include "trap.h"
#include "util.h"

/* Seconds from the Mac epoch (1904-01-01) to the Unix epoch (1970-01-01). */
#define MAC_EPOCH_OFFSET 2082844800u
#define MAX_AE_HANDLERS 16

static struct {
    struct timespec start;
    int cursor_level; /* 0 = visible; HideCursor decrements, InitCursor resets */
    struct {
        uint32_t event_class, event_id, handler, refcon;
    } ae[MAX_AE_HANDLERS];
    int nae;
} M;

void misc_init(void) {
    memset(&M, 0, sizeof M);
    clock_gettime(CLOCK_MONOTONIC, &M.start);
}

static uint64_t elapsed_us(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t us = (int64_t)(now.tv_sec - M.start.tv_sec) * 1000000 +
                 (now.tv_nsec - M.start.tv_nsec) / 1000;
    return us < 0 ? 0 : (uint64_t)us;
}

uint32_t misc_ticks(void) { return (uint32_t)(elapsed_us() * 60 / 1000000); }

bool misc_cursor_visible(void) { return M.cursor_level == 0; }

bool misc_ae_handler(uint32_t event_class, uint32_t event_id, uint32_t *handler,
                     uint32_t *refcon) {
    for (int i = 0; i < M.nae; i++) {
        if (M.ae[i].event_class == event_class && M.ae[i].event_id == event_id) {
            *handler = M.ae[i].handler;
            *refcon = M.ae[i].refcon;
            return true;
        }
    }
    return false;
}

/* ---- Gestalt ---- */

static const struct {
    uint32_t selector, value;
} gestalt_table[] = {
    {FOURCC('s', 'y', 's', 'v'), 0x1028},     /* Mac OS X 10.2.8 */
    {FOURCC('c', 'b', 'o', 'n'), 0x0160},     /* Carbon 1.6 */
    {FOURCC('p', 'p', 'c', 'f'), 0x0003},     /* G3: graphics ops and stfiwx, no AltiVec (bit 4) */
    {FOURCC('v', 'm', ' ', ' '), 0x0001},     /* virtual memory present */
    {FOURCC('m', 'a', 'c', 'h'), 510},        /* gestaltPowerMacG3 */
    {FOURCC('l', 'r', 'a', 'm'), 0x10000000}, /* 256 MB logical RAM */
    {FOURCC('r', 'a', 'm', ' '), 0x10000000}, /* 256 MB physical RAM */
};

static void h_gestalt(void) {
    uint32_t sel = trap_arg(0), resp = trap_arg(1);
    for (size_t i = 0; i < sizeof gestalt_table / sizeof gestalt_table[0]; i++) {
        if (gestalt_table[i].selector == sel) {
            gm_w32(resp, gestalt_table[i].value);
            trap_return(0);
            return;
        }
    }
    char s[5];
    for (int i = 0; i < 4; i++) {
        int c = (int)((sel >> (24 - 8 * i)) & 0xFF);
        s[i] = isprint(c) ? (char)c : '?';
    }
    s[4] = '\0';
    log_msg("Gestalt: unknown selector '%s' (0x%08x)", s, sel);
    trap_return((uint32_t)MISC_GESTALT_UNDEF_SELECTOR_ERR);
}

/* ---- time ---- */

static void h_tick_count(void) { trap_return(misc_ticks()); }

static void h_microseconds(void) {
    uint64_t us = elapsed_us();
    uint32_t out = trap_arg(0);
    gm_w32(out, (uint32_t)(us >> 32));
    gm_w32(out + 4, (uint32_t)us);
}

static void h_delay(void) {
    uint32_t ticks = trap_arg(0), final_ticks = trap_arg(1);
    if ((int32_t)ticks > 0) {
        uint64_t us = (uint64_t)ticks * 1000000 / 60;
        struct timespec ts = {(time_t)(us / 1000000), (long)(us % 1000000) * 1000};
        nanosleep(&ts, NULL);
    }
    if (final_ticks)
        gm_w32(final_ticks, misc_ticks());
}

static void h_get_date_time(void) {
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    gm_w32(trap_arg(0), (uint32_t)((int64_t)now + lt.tm_gmtoff + MAC_EPOCH_OFFSET));
}

/* ReadLocation(MachineLocation *): latitude and longitude 0, then gmtDelta in
   the low 24 bits with the daylight-saving flag (0x80) in the high byte. */
static void h_read_location(void) {
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    uint32_t loc = trap_arg(0);
    gm_w32(loc, 0);
    gm_w32(loc + 4, 0);
    uint32_t dls = lt.tm_isdst > 0 ? 0x80u : 0u;
    gm_w32(loc + 8, (dls << 24) | ((uint32_t)lt.tm_gmtoff & 0x00FFFFFFu));
}

/* ---- strings and memory ---- */

static void h_num_to_string(void) {
    char buf[16];
    snprintf(buf, sizeof buf, "%d", (int32_t)trap_arg(0));
    gm_write_pstr(trap_arg(1), buf);
}

static void h_p2cstrcpy(void) {
    char s[256];
    gm_read_pstr(trap_arg(1), s);
    gm_write_cstr(trap_arg(0), s);
}

static void h_c2pstrcpy(void) {
    char s[256];
    gm_read_cstr(trap_arg(1), s, sizeof s);
    gm_write_pstr(trap_arg(0), s);
}

static void h_block_move_data(void) {
    uint32_t src = trap_arg(0), dst = trap_arg(1);
    int32_t n = (int32_t)trap_arg(2);
    if (n <= 0)
        return;
    const uint8_t *s = gm_ptr(src, (uint32_t)n);
    memmove(gm_ptr(dst, (uint32_t)n), s, (size_t)n);
}

/* ---- cursor, menus, keyboard ---- */

static void h_init_cursor(void) { M.cursor_level = 0; }
static void h_hide_cursor(void) { M.cursor_level--; }
static void h_set_theme_cursor(void) { trap_return(0); }
static void h_key_script(void) {}
static void h_get_mbar_height(void) { trap_return(0); }

/* ---- Apple Events and Internet Config ---- */

static void h_new_ae_event_handler_upp(void) { trap_return(trap_arg(0)); }

static void h_ae_install_event_handler(void) {
    uint32_t cls = trap_arg(0), id = trap_arg(1);
    int slot = M.nae;
    for (int i = 0; i < M.nae; i++)
        if (M.ae[i].event_class == cls && M.ae[i].event_id == id)
            slot = i;
    if (slot == MAX_AE_HANDLERS)
        trap_crash("AEInstallEventHandler: more than %d handlers", MAX_AE_HANDLERS);
    M.ae[slot].event_class = cls;
    M.ae[slot].event_id = id;
    M.ae[slot].handler = trap_arg(2);
    M.ae[slot].refcon = trap_arg(3);
    if (slot == M.nae)
        M.nae++;
    trap_return(0);
}

static void h_ic_start(void) {
    gm_w32(trap_arg(0), MISC_IC_INSTANCE);
    trap_return(0);
}

static void h_ic_stop(void) { trap_return(0); }

static void h_exit_to_shell(void) {
    log_msg("ExitToShell");
    exit(0);
}

void misc_register(void) {
    trap_register("Gestalt", h_gestalt);
    trap_register("TickCount", h_tick_count);
    trap_register("Microseconds", h_microseconds);
    trap_register("Delay", h_delay);
    trap_register("GetDateTime", h_get_date_time);
    trap_register("ReadLocation", h_read_location);
    trap_register("NumToString", h_num_to_string);
    trap_register("p2cstrcpy", h_p2cstrcpy);
    trap_register("c2pstrcpy", h_c2pstrcpy);
    trap_register("BlockMoveData", h_block_move_data);
    trap_register("InitCursor", h_init_cursor);
    trap_register("HideCursor", h_hide_cursor);
    trap_register("SetThemeCursor", h_set_theme_cursor);
    trap_register("KeyScript", h_key_script);
    trap_register("GetMBarHeight", h_get_mbar_height);
    trap_register("NewAEEventHandlerUPP", h_new_ae_event_handler_upp);
    trap_register("AEInstallEventHandler", h_ae_install_event_handler);
    trap_register("ICStart", h_ic_start);
    trap_register("ICStop", h_ic_stop);
    trap_register("ExitToShell", h_exit_to_shell);
}
