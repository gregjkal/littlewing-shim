#include "misc.h"

#include <ctype.h>
#include <math.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>

#include "cf.h"
#include "cpu.h"
#include "guest_mem.h"
#include "snapshot.h"
#include "replay.h"
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
    misc_idle_fn idle;
    uint32_t last_idle_tick;
    bool fixed;
    uint64_t virtual_us;
    uint64_t last_us; /* what elapsed_us last returned */
    uint32_t spin_tick; /* the tick TickCount last returned, and how often since it moved */
    int spins;
    uint64_t spin_ms; /* the ms Microseconds or Delay(0) last saw, and how often since */
    int ms_spins;
    int polls; /* time polls since the virtual clock last moved */
    misc_url_fn open_url;
    void (*exit_hook)(void);
} M;

/* A snapshot's walk (snapshot.h): the host's clock at launch,
   LOONY_FIXED_CLOCK and the shim's hooks are the host's. */
static void snap_walk(snapshot_walker *w) {
    snapshot_bytes(w, &M, sizeof M);
    snapshot_check(w, M.nae >= 0 && M.nae <= MAX_AE_HANDLERS, "%d AppleEvent handlers", M.nae);
    SNAPSHOT_HOST(w, M.fixed);
    SNAPSHOT_HOST(w, M.start);
    SNAPSHOT_HOST(w, M.idle);
    SNAPSHOT_HOST(w, M.open_url);
    SNAPSHOT_HOST(w, M.exit_hook);
}
SNAPSHOT_STATE_WALK(M, snap_walk)

static uint32_t system_version = MISC_SYSTEM_VERSION_PEF; /* Gestalt('sysv') */
SNAPSHOT_STATE(system_version)

extern char **environ;

static bool open_with_open(const char *url) {
    char *argv[] = {"/usr/bin/open", (char *)url, NULL};
    pid_t pid;
    if (posix_spawn(&pid, argv[0], NULL, NULL, argv, environ) != 0)
        return false;
    int status;
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void misc_set_url_opener(misc_url_fn fn) { M.open_url = fn; }

void misc_set_idle(misc_idle_fn fn) { M.idle = fn; }

void misc_init(void) {
    misc_idle_fn idle = M.idle;
    misc_url_fn open_url = M.open_url;
    memset(&M, 0, sizeof M);
    system_version = MISC_SYSTEM_VERSION_PEF;
    M.idle = idle;
    M.open_url = open_url ? open_url : open_with_open;
    clock_gettime(CLOCK_MONOTONIC, &M.start);
    const char *f = getenv("LOONY_FIXED_CLOCK");
    M.fixed = f && strcmp(f, "1") == 0;
}

bool misc_fixed_clock(void) { return M.fixed; }

static uint64_t elapsed_us(void) {
    if (M.fixed)
        return M.virtual_us;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t us = (int64_t)(now.tv_sec - M.start.tv_sec) * 1000000 +
                 (now.tv_nsec - M.start.tv_nsec) / 1000;
    M.last_us = us < 0 ? 0 : (uint64_t)us;
    if (replay_recording() || replay_playing())
        M.last_us = replay_clock(M.last_us);
    return M.last_us;
}

double misc_last_seconds(void) { return (double)(M.fixed ? M.virtual_us : M.last_us) / 1e6; }

uint32_t misc_ticks(void) { return (uint32_t)(elapsed_us() * 60 / 1000000); }

double misc_seconds(void) { return (double)elapsed_us() / 1e6; }

void misc_wait(double seconds) {
    if (seconds <= 0 || replay_playing()) /* a replay's clock comes from its file */
        return;
    if (M.fixed) {
        /* Round up: a wait that rounds to 0 us would never reach its deadline. */
        uint64_t us = (uint64_t)ceil(seconds * 1e6);
        M.virtual_us += us ? us : 1;
        M.polls = 0;
        return;
    }
    struct timespec ts = {(time_t)seconds, (long)((seconds - (double)(time_t)seconds) * 1e9)};
    nanosleep(&ts, NULL);
}

/* Virtual clock: the start of the next tick. */
static void advance_to_next_tick(void) {
    uint64_t tick_us = 1000000 / 60;
    uint64_t t = (M.virtual_us / tick_us + 1) * tick_us;
    /* 1000000/60 isn't whole: step until misc_ticks() really changes */
    uint32_t before = misc_ticks();
    M.virtual_us = t;
    while (misc_ticks() == before)
        M.virtual_us++;
    M.polls = 0;
}

void misc_poll(void) {
    if (M.fixed && ++M.polls >= 200) /* a loop that polls without ever waiting */
        advance_to_next_tick();
}

bool misc_cursor_visible(void) { return M.cursor_level == 0; }

void misc_hide_cursor(void) { M.cursor_level--; }

void misc_show_cursor(void) {
    if (M.cursor_level < 0)
        M.cursor_level++;
}

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

void misc_set_system_version(uint32_t v) { system_version = v; }

static const struct {
    uint32_t selector, value;
} gestalt_table[] = {
    {FOURCC('s', 'y', 's', 'v'), 0},          /* system_version */
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
            gm_w32(resp, sel == FOURCC('s', 'y', 's', 'v') ? system_version : gestalt_table[i].value);
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

static void idle_if_new_tick(uint32_t t) {
    if (M.idle && t != M.last_idle_tick) {
        M.last_idle_tick = t;
        M.idle();
    }
}

/* Recording or replaying: a game that polls TickCount waiting for the next
   tick (Loony Labyrinth spins on TickCount and Delay(0)) sleeps until it
   instead, so neither has to repeat thousands of polls a tick, and a replay,
   which skips the sleep, runs faster than real time. The polls are counted,
   not timed, so a replay takes the same path. */
#define SPIN_POLLS 50

static uint32_t wait_out_spin(uint32_t t) {
    if (!replay_recording() && !replay_playing())
        return t;
    if (t != M.spin_tick) {
        M.spin_tick = t;
        M.spins = 0;
        return t;
    }
    if (++M.spins < SPIN_POLLS)
        return t;
    M.spins = 0;
    /* The first whole ms in the next tick: a recording's clock moves in ms. */
    uint64_t next_us = ((((uint64_t)t + 1) * 1000000 + 59) / 60 + 999) / 1000 * 1000;
    if (next_us > M.last_us)
        misc_wait((double)(next_us - M.last_us) / 1e6);
    return misc_ticks();
}

/* The same for a game that polls Microseconds and Delay(0) until a time
   comes (Loony Labyrinth's menus do, tens of thousands of times a second):
   a recording's clock moves in whole ms, so after MS_SPIN_POLLS polls in
   the same ms it sleeps until the next. Counted, not timed, as above. */
#define MS_SPIN_POLLS 3

static void wait_out_ms_spin(void) {
    if (!replay_recording() && !replay_playing())
        return;
    uint64_t ms = M.last_us / 1000;
    if (ms != M.spin_ms) {
        M.spin_ms = ms;
        M.ms_spins = 1; /* this is the ms's first poll */
        return;
    }
    if (++M.ms_spins < MS_SPIN_POLLS)
        return;
    M.ms_spins = 0;
    misc_wait((double)((ms + 1) * 1000 - M.last_us) / 1e6);
}

static void h_tick_count(void) {
    misc_poll();
    uint32_t t = wait_out_spin(misc_ticks());
    idle_if_new_tick(t);
    trap_return(t + MISC_BOOT_TICKS);
}

static void h_microseconds(void) {
    misc_poll();
    wait_out_ms_spin();
    uint64_t us = elapsed_us() + MISC_BOOT_TICKS * 1000000ull / 60;
    uint32_t out = trap_arg(0);
    gm_w32(out, (uint32_t)(us >> 32));
    gm_w32(out + 4, (uint32_t)us);
}

void misc_sleep_us(uint64_t us) {
    if (us == 0) {
        misc_poll();
        return;
    }
    uint64_t end = elapsed_us() + us, tick_us = 1000000 / 60;
    for (;;) {
        idle_if_new_tick(misc_ticks());
        uint64_t now = elapsed_us();
        if (now >= end)
            break;
        if (M.fixed) {
            uint64_t next = (now / tick_us + 1) * tick_us;
            M.virtual_us = next < end ? next : end;
            M.polls = 0;
        } else {
            uint64_t step = end - now < tick_us / 4 ? end - now : tick_us / 4;
            misc_wait((double)step / 1e6);
        }
    }
}

/* Delay(ticks, &finalTicks): sleeps in steps of at most one tick, running
   the idle hook between steps. */
static void h_delay(void) {
    uint32_t ticks = trap_arg(0), final_ticks = trap_arg(1);
    if ((int32_t)ticks <= 0)
        wait_out_ms_spin();
    uint32_t end = misc_ticks() + ((int32_t)ticks > 0 ? ticks : 0);
    if (M.fixed && end == misc_ticks()) /* Delay(0) on the virtual clock: one tick passes */
        end++;
    for (;;) {
        uint32_t t = misc_ticks();
        idle_if_new_tick(t);
        if (t >= end)
            break;
        if (M.fixed)
            advance_to_next_tick();
        else
            misc_wait(1.0 / 60 / 4);
    }
    if (final_ticks)
        gm_w32(final_ticks, misc_ticks() + MISC_BOOT_TICKS);
}

/* The virtual clock's calendar starts at 2003-01-01 00:00:00 (Mac time), so
   fixed-clock runs see the same date every time. */
#define FIXED_CLOCK_EPOCH 3124224000u

/* The same instant in Unix time: 2003-01-01 00:00:00 UTC. */
#define FIXED_CLOCK_UNIX 1041379200

/* The host's wall clock (Unix seconds) and time zone. On a recording or a
   replay: the recording's start plus the game's clock, in
   the time zone it started in, so dates (a high score's timestamp) replay
   too. */
static int64_t wall_clock(int32_t *gmtoff, bool *dst) {
    int64_t start;
    if (replay_calendar(&start, gmtoff, dst))
        return start + (int64_t)(elapsed_us() / 1000000);
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    *gmtoff = (int32_t)lt.tm_gmtoff;
    *dst = lt.tm_isdst > 0;
    return (int64_t)now;
}

int64_t misc_unix_time(void) {
    if (M.fixed)
        return FIXED_CLOCK_UNIX + (int64_t)(M.virtual_us / 1000000);
    int32_t gmtoff;
    bool dst;
    return wall_clock(&gmtoff, &dst);
}

static void h_get_date_time(void) {
    if (M.fixed) {
        gm_w32(trap_arg(0), FIXED_CLOCK_EPOCH + (uint32_t)(M.virtual_us / 1000000));
        return;
    }
    int32_t gmtoff;
    bool dst;
    int64_t now = wall_clock(&gmtoff, &dst);
    gm_w32(trap_arg(0), (uint32_t)(now + gmtoff + MAC_EPOCH_OFFSET));
}

/* ReadLocation(MachineLocation *): latitude and longitude 0, then gmtDelta in
   the low 24 bits with the daylight-saving flag (0x80) in the high byte. */
static void h_read_location(void) {
    uint32_t loc = trap_arg(0);
    gm_w32(loc, 0);
    gm_w32(loc + 4, 0);
    if (M.fixed) { /* GMT with no daylight saving, so runs don't depend on the host */
        gm_w32(loc + 8, 0);
        return;
    }
    int32_t gmtoff;
    bool dst;
    wall_clock(&gmtoff, &dst);
    gm_w32(loc + 8, ((dst ? 0x80u : 0u) << 24) | ((uint32_t)gmtoff & 0x00FFFFFFu));
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
static void h_show_cursor(void) { misc_show_cursor(); }
static void h_set_theme_cursor(void) { trap_return(0); }
static void h_key_script(void) {}
/* The keyboard's input source is the host's: these change nothing. */
static void h_set_text_service_language(void) { trap_return(0); }
static void h_tis_copy_ascii_capable_source(void) { trap_return(0); }
static void h_tis_select_input_source(void) { trap_return((uint32_t)-50); }
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

/* Opens an http or https URL the game asked call to open, except on a
   replay. False, logged, for any other URL. */
static bool open_web(const char *call, const char *url) {
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        log_msg("%s: not opening \"%s\" (only http and https)", call, url);
        return false;
    }
    if (replay_playing()) { /* the person who recorded it saw the page */
        log_msg("replay: not opening %s", url);
        return true;
    }
    log_msg("opening %s", url);
    if (!M.open_url(url))
        log_msg("%s: couldn't open %s", call, url);
    return true;
}

/* ICLaunchURL(ICInstance, ConstStr255Param hint, const void *data, long len,
   long *selStart, long *selEnd) -> OSStatus. The URL is data[*selStart,
   *selEnd). Only http and https URLs are opened. */
static void h_ic_launch_url(void) {
    uint32_t data = trap_arg(2), start_p = trap_arg(4), end_p = trap_arg(5);
    int32_t len = (int32_t)trap_arg(3);
    int32_t start = (int32_t)gm_r32(start_p), end = (int32_t)gm_r32(end_p);
    if (len < 0 || start < 0 || end < start || end > len || end - start > 1023) {
        trap_return((uint32_t)-50); /* paramErr */
        return;
    }
    char url[1024];
    memcpy(url, gm_ptr(data + (uint32_t)start, (uint32_t)(end - start) + 1), (size_t)(end - start));
    url[end - start] = '\0';
    trap_return(open_web("ICLaunchURL", url) ? 0 : (uint32_t)-50);
}

/* LSOpenCFURLRef(CFURLRef url, CFURLRef *outLaunchedURL) -> OSStatus. */
static void h_ls_open_cfurl_ref(void) {
    if (trap_arg(1))
        gm_w32(trap_arg(1), 0);
    trap_return(open_web("LSOpenCFURLRef", cf_url_path("LSOpenCFURLRef", trap_arg(0))) ? 0 : (uint32_t)-50);
}

/* ---- SANE ---- */

void misc_num2dec(int style, int digits, double x, misc_decimal *out) {
    memset(out, 0, sizeof *out);
    out->negative = signbit(x) != 0;
    double a = fabs(x);
    if (isnan(x) || isinf(x)) {
        out->sig[0] = isnan(x) ? 'N' : 'I';
        return;
    }
    char buf[400];
    if (style == 0) {
        if (digits < 1)
            digits = 1;
        if (digits > MISC_SIGDIGLEN)
            digits = MISC_SIGDIGLEN;
        if (a == 0) {
            out->sig[0] = '0';
            return;
        }
        snprintf(buf, sizeof buf, "%.*e", digits - 1, a); /* d.ddde[+-]x */
        int e = atoi(strchr(buf, 'e') + 1);
        int n = 0;
        for (const char *p = buf; *p != 'e'; p++)
            if (isdigit((unsigned char)*p))
                out->sig[n++] = *p;
        out->exp = (int16_t)(e - (n - 1));
        return;
    }
    if (digits < 0)
        digits = 0;
    if (digits > 80)
        digits = 80;
    snprintf(buf, sizeof buf, "%.*f", digits, a);
    char d[400];
    int n = 0;
    for (const char *p = buf; *p; p++)
        if (isdigit((unsigned char)*p))
            d[n++] = *p;
    d[n] = '\0';
    const char *first = d;
    while (*first == '0' && first[1])
        first++;
    if (strlen(first) > MISC_SIGDIGLEN) {
        out->sig[0] = '?';
        return;
    }
    strcpy(out->sig, first);
    out->exp = (int16_t)(strcmp(first, "0") == 0 ? 0 : -digits);
}

/* num2dec(const decform *f, double x, decimal *d). decform is {char style;
   char unused; short digits}; decimal is {char sgn; char unused; short exp;
   unsigned char length; unsigned char text[36]; unsigned char unused}. x
   arrives in f1 and takes up r4-r5, so d is in r6. */
static void h_num2dec(void) {
    uint32_t f = trap_arg(0), d = trap_arg(3);
    misc_decimal dec;
    misc_num2dec((int8_t)gm_r8(f), (int16_t)gm_r16(f + 2), cpu_fpr(1), &dec);
    gm_w8(d, dec.negative);
    gm_w8(d + 1, 0);
    gm_w16(d + 2, (uint16_t)dec.exp);
    size_t n = strlen(dec.sig);
    gm_w8(d + 4, (uint8_t)n);
    memcpy(gm_ptr(d + 5, MISC_SIGDIGLEN), dec.sig, n);
}

void misc_set_exit_hook(void (*fn)(void)) { M.exit_hook = fn; }

void misc_exit(const char *why, int status) {
    log_msg("%s", why);
    if (M.exit_hook)
        M.exit_hook();
    exit(status);
}

void misc_main_returned(int32_t status) {
    char why[32];
    snprintf(why, sizeof why, "main returned %d", status);
    misc_exit(why, status & 0xFF);
}

static void h_exit_to_shell(void) {
    misc_exit("ExitToShell", 0);
}

/* Fix2X(Fixed) -> double, in f1. */
static void h_fix2x(void) { cpu_set_fpr(1, (int32_t)trap_arg(0) / 65536.0); }

/* Long2Fix(long) -> Fixed, saturating as the Toolbox does. */
static void h_long2fix(void) {
    int32_t v = (int32_t)trap_arg(0);
    trap_return(v > 0x7FFF ? 0x7FFFFFFFu : v < -0x8000 ? 0x80000000u : (uint32_t)v << 16);
}

void misc_register(void) {
    trap_register("Fix2X", h_fix2x);
    trap_register("Long2Fix", h_long2fix);
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
    trap_register("ShowCursor", h_show_cursor);
    trap_register("SetThemeCursor", h_set_theme_cursor);
    trap_register("KeyScript", h_key_script);
    trap_register("GetMBarHeight", h_get_mbar_height);
    trap_register("NewAEEventHandlerUPP", h_new_ae_event_handler_upp);
    trap_register("AEInstallEventHandler", h_ae_install_event_handler);
    trap_register("ICStart", h_ic_start);
    trap_register("ICStop", h_ic_stop);
    trap_register("ICLaunchURL", h_ic_launch_url);
    trap_register("LSOpenCFURLRef", h_ls_open_cfurl_ref);
    trap_register("SetTextServiceLanguage", h_set_text_service_language);
    trap_register("TISCopyCurrentASCIICapableKeyboardInputSource", h_tis_copy_ascii_capable_source);
    trap_register("TISSelectInputSource", h_tis_select_input_source);
    trap_register("num2dec", h_num2dec);
    trap_register("ExitToShell", h_exit_to_shell);
}
