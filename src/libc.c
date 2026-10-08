#include "libc.h"

#include <runetype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xlocale.h>

#include "guest_mem.h"
#include "memmgr.h"
#include "misc.h"
#include "trap.h"
#include "util.h"

/* A 32-bit PowerPC _RuneLocale (<runetype.h>): magic[8], encoding[32], three
   pointers, then the tables. */
#define RUNE_SIZE        3156u
#define RUNE_TYPE_OFF    52u
#define RUNE_LOWER_OFF   1076u
#define RUNE_UPPER_OFF   2100u
/* A 32-bit struct tm: nine ints, tm_gmtoff, tm_zone. */
#define TM_SIZE          44u
#define DL_HANDLE        0x0FFF0101u /* what dlopen returns */
#define KEYMGR_KEYS      16

static struct {
    char exe_path[1024];
    uint32_t errno_addr, rune, keymgr_global, mach_init_routine, cthread_init_routine;
    uint32_t tm, tm_zone;
    uint32_t rand_next;
    struct {
        uint32_t key, ptr;
    } keymgr[KEYMGR_KEYS];
    int nkeymgr;
} L;

static uint32_t new_object(uint32_t size) {
    uint32_t p = mm_new_ptr(size, true);
    if (!p)
        fatal("can't allocate the C library's data");
    return p;
}

void libc_init(const char *exe_path) {
    memset(&L, 0, sizeof L);
    snprintf(L.exe_path, sizeof L.exe_path, "%s", exe_path);
    L.rand_next = 1;
    L.errno_addr = new_object(4);
    L.keymgr_global = new_object(16);
    L.mach_init_routine = new_object(4);
    L.cthread_init_routine = new_object(4);
    L.tm = new_object(TM_SIZE);
    L.tm_zone = new_object(16);

    /* The C locale's character tables, from the host's. */
    L.rune = new_object(RUNE_SIZE);
    memcpy(gm_ptr(L.rune, 8), "RuneMagA", 8);
    memcpy(gm_ptr(L.rune + 8, 4), "NONE", 4);
    for (uint32_t c = 0; c < 256; c++) {
        gm_w32(L.rune + RUNE_TYPE_OFF + 4 * c, _DefaultRuneLocale.__runetype[c]);
        gm_w32(L.rune + RUNE_LOWER_OFF + 4 * c, (uint32_t)_DefaultRuneLocale.__maplower[c]);
        gm_w32(L.rune + RUNE_UPPER_OFF + 4 * c, (uint32_t)_DefaultRuneLocale.__mapupper[c]);
    }
}

uint32_t libc_data_symbol(const char *name) {
    if (!L.errno_addr)
        return 0;
    if (strcmp(name, "errno") == 0)
        return L.errno_addr;
    if (strcmp(name, "_DefaultRuneLocale") == 0)
        return L.rune;
    if (strcmp(name, "__keymgr_global") == 0)
        return L.keymgr_global;
    if (strcmp(name, "mach_init_routine") == 0)
        return L.mach_init_routine;
    if (strcmp(name, "_cthread_init_routine") == 0)
        return L.cthread_init_routine;
    return 0;
}

static uint32_t new_string(const char *s) {
    uint32_t p = new_object((uint32_t)strlen(s) + 1);
    gm_write_cstr(p, s);
    return p;
}

void libc_main_args(uint32_t *argv, uint32_t *envp, uint32_t *apple) {
    uint32_t path = new_string(L.exe_path);
    *argv = new_object(8);
    gm_w32(*argv, path);
    *envp = new_object(4);
    *apple = new_object(8);
    gm_w32(*apple, path);
}

/* ---- memory ---- */

static void h_malloc(void) {
    trap_return(mm_new_ptr(trap_arg(0), false));
}

static void h_calloc(void) {
    uint64_t total = (uint64_t)trap_arg(0) * trap_arg(1);
    trap_return(total > 0xFFFFFFFFu ? 0 : mm_new_ptr((uint32_t)total, true));
}

static void h_free(void) {
    uint32_t p = trap_arg(0);
    if (p && mm_dispose_ptr(p) != MM_NO_ERR)
        trap_crash("free(0x%08x): not a block malloc returned", p);
}

/* ---- strings and bytes ---- */

static uint32_t guest_strlen(uint32_t s) {
    uint32_t n = 0;
    while (gm_r8(s + n))
        n++;
    return n;
}

static void h_memmove(void) {
    uint32_t dst = trap_arg(0), src = trap_arg(1), n = trap_arg(2);
    if (n)
        memmove(gm_ptr(dst, n), gm_ptr(src, n), n);
    trap_return(dst);
}

static void h_memset(void) {
    uint32_t dst = trap_arg(0), n = trap_arg(2);
    if (n)
        memset(gm_ptr(dst, n), (int)(trap_arg(1) & 0xFF), n);
    trap_return(dst);
}

static void h_strlen(void) {
    trap_return(guest_strlen(trap_arg(0)));
}

static void h_strcmp(void) {
    uint32_t a = trap_arg(0), b = trap_arg(1);
    for (;; a++, b++) {
        uint8_t x = gm_r8(a), y = gm_r8(b);
        if (x != y || !x) {
            trap_return((uint32_t)((int)x - (int)y));
            return;
        }
    }
}

static void h_strcpy(void) {
    uint32_t dst = trap_arg(0), src = trap_arg(1), n = guest_strlen(src) + 1;
    memmove(gm_ptr(dst, n), gm_ptr(src, n), n);
    trap_return(dst);
}

static void h_strncpy(void) {
    uint32_t dst = trap_arg(0), src = trap_arg(1), n = trap_arg(2);
    uint32_t i = 0;
    for (; i < n; i++) {
        uint8_t c = gm_r8(src + i);
        gm_w8(dst + i, c);
        if (!c)
            break;
    }
    for (; i < n; i++)
        gm_w8(dst + i, 0);
    trap_return(dst);
}

static void h_strcat(void) {
    uint32_t dst = trap_arg(0), src = trap_arg(1);
    uint32_t end = dst + guest_strlen(dst), n = guest_strlen(src) + 1;
    memmove(gm_ptr(end, n), gm_ptr(src, n), n);
    trap_return(dst);
}

/* ---- qsort: a stable merge sort of indices, then one permutation ---- */

static struct {
    uint32_t base, size, cmp;
} Q;

static bool before_or_equal(uint32_t i, uint32_t j) {
    uint32_t args[2] = {Q.base + i * Q.size, Q.base + j * Q.size};
    return (int32_t)guest_call(Q.cmp, 2, args) <= 0;
}

static void merge_sort(uint32_t *v, uint32_t *tmp, uint32_t n) {
    if (n < 2)
        return;
    uint32_t h = n / 2;
    merge_sort(v, tmp, h);
    merge_sort(v + h, tmp, n - h);
    uint32_t i = 0, j = h, k = 0;
    while (i < h && j < n)
        tmp[k++] = before_or_equal(v[i], v[j]) ? v[i++] : v[j++];
    while (i < h)
        tmp[k++] = v[i++];
    while (j < n)
        tmp[k++] = v[j++];
    memcpy(v, tmp, n * sizeof *v);
}

static void h_qsort(void) {
    uint32_t base = trap_arg(0), n = trap_arg(1), size = trap_arg(2), cmp = trap_arg(3);
    if (n < 2 || size == 0)
        return;
    uint64_t bytes = (uint64_t)n * size;
    if (bytes > 0xFFFFFFFFu)
        trap_crash("qsort of %u elements of %u bytes", n, size);
    gm_ptr(base, (uint32_t)bytes); /* crashes now if the array isn't all guest memory */
    uint32_t *v = malloc(2 * n * sizeof *v);
    uint8_t *sorted = malloc(bytes);
    if (!v || !sorted)
        fatal("out of memory");
    for (uint32_t i = 0; i < n; i++)
        v[i] = i;
    typeof(Q) saved = Q; /* the comparator may sort too */
    Q.base = base, Q.size = size, Q.cmp = cmp;
    merge_sort(v, v + n, n);
    Q = saved;
    uint8_t *data = gm_ptr(base, (uint32_t)bytes);
    for (uint32_t i = 0; i < n; i++)
        memcpy(sorted + (uint64_t)i * size, data + (uint64_t)v[i] * size, size);
    memcpy(data, sorted, bytes);
    free(sorted);
    free(v);
}

/* ---- rand: Darwin 10.4's, from Apple's Libc-391 stdlib/FreeBSD/rand.c
   (USE_WEAK_SEEDING is not defined there): Park and Miller's "minimal
   standard" generator, with a zero state replaced by 123459876. ---- */

static void h_rand(void) {
    if (L.rand_next == 0)
        L.rand_next = 123459876;
    int32_t hi = (int32_t)(L.rand_next / 127773), lo = (int32_t)(L.rand_next % 127773);
    int32_t x = 16807 * lo - 2836 * hi;
    if (x < 0)
        x += 0x7FFFFFFF;
    L.rand_next = (uint32_t)x;
    trap_return((uint32_t)x % 0x80000000u);
}

static void h_srand(void) {
    L.rand_next = trap_arg(0);
}

/* ---- time ---- */

static void h_time(void) {
    uint32_t t = (uint32_t)misc_unix_time(), out = trap_arg(0);
    if (out)
        gm_w32(out, t);
    trap_return(t);
}

/* localtime: UTC on the virtual clock, so fixed-clock runs match anywhere. */
static void h_localtime(void) {
    time_t t = (time_t)(int32_t)gm_r32(trap_arg(0));
    struct tm tm;
    if (misc_fixed_clock())
        gmtime_r(&t, &tm);
    else
        localtime_r(&t, &tm);
    const int32_t f[9] = {tm.tm_sec,  tm.tm_min,  tm.tm_hour, tm.tm_mday, tm.tm_mon,
                          tm.tm_year, tm.tm_wday, tm.tm_yday, tm.tm_isdst};
    for (int i = 0; i < 9; i++)
        gm_w32(L.tm + 4u * (uint32_t)i, (uint32_t)f[i]);
    gm_w32(L.tm + 36, (uint32_t)(int32_t)tm.tm_gmtoff);
    char zone[16];
    snprintf(zone, sizeof zone, "%s", tm.tm_zone ? tm.tm_zone : "UTC");
    gm_write_cstr(L.tm_zone, zone);
    gm_w32(L.tm + 40, L.tm_zone);
    trap_return(L.tm);
}

static void h_strftime(void) {
    uint32_t dst = trap_arg(0), max = trap_arg(1), fmt_addr = trap_arg(2), t = trap_arg(3);
    char fmt[256], zone[16] = "";
    if (!gm_read_cstr(fmt_addr, fmt, sizeof fmt))
        trap_crash("strftime: the format is longer than %zu bytes", sizeof fmt - 1);
    struct tm tm = {0};
    int *f[9] = {&tm.tm_sec,  &tm.tm_min,  &tm.tm_hour, &tm.tm_mday, &tm.tm_mon,
                 &tm.tm_year, &tm.tm_wday, &tm.tm_yday, &tm.tm_isdst};
    for (int i = 0; i < 9; i++)
        *f[i] = (int32_t)gm_r32(t + 4u * (uint32_t)i);
    tm.tm_gmtoff = (int32_t)gm_r32(t + 36);
    if (gm_r32(t + 40))
        gm_read_cstr(gm_r32(t + 40), zone, sizeof zone);
    tm.tm_zone = zone;
    static locale_t c_locale;
    if (!c_locale)
        c_locale = newlocale(LC_ALL_MASK, "C", NULL);
    char out[1024];
    size_t n = strftime_l(out, sizeof out, fmt, &tm, c_locale);
    if (n == 0 || n + 1 > max) {
        trap_return(0);
        return;
    }
    memcpy(gm_ptr(dst, (uint32_t)n + 1), out, n + 1);
    trap_return((uint32_t)n);
}

static void h_usleep(void) {
    misc_sleep_us(trap_arg(0));
    trap_return(0);
}

/* ---- exit ---- */

static void h_exit(void) {
    char why[32];
    snprintf(why, sizeof why, "exit(%d)", (int32_t)trap_arg(0));
    misc_exit(why, (int)(trap_arg(0) & 0xFF));
}

static void h_abort(void) {
    trap_crash("the game called abort()");
}

/* ---- ctype ---- */

static void h_maskrune(void) {
    int32_t c = (int32_t)trap_arg(0);
    uint32_t t = c >= 0 && c < 256 ? _DefaultRuneLocale.__runetype[c] : 0;
    trap_return(t & trap_arg(1));
}

static void h_toupper(void) {
    int32_t c = (int32_t)trap_arg(0);
    trap_return(c >= 0 && c < 256 ? (uint32_t)_DefaultRuneLocale.__mapupper[c] : (uint32_t)c);
}

/* ---- dynamic lookup: the game looks up sprintf$LDBL128 ---- */

static uint32_t lookup(const char *name) {
    if (strcmp(name, "sprintf$LDBL128") == 0 || strcmp(name, "sprintf") == 0) {
        int32_t i = trap_find("sprintf");
        if (i >= 0)
            return GUEST_TRAP_ADDR(i);
    }
    log_msg("dynamic lookup of %s: not provided", name);
    return 0;
}

/* NS* calls take the symbol's linker name, with its leading underscore. */
static uint32_t lookup_linker_name(uint32_t addr) {
    char name[256];
    gm_read_cstr(addr, name, sizeof name);
    return lookup(name[0] == '_' ? name + 1 : name);
}

static void h_dlopen(void) {
    char path[256] = "(null)";
    if (trap_arg(0))
        gm_read_cstr(trap_arg(0), path, sizeof path);
    log_msg("dlopen(%s)", path);
    trap_return(DL_HANDLE);
}

static void h_dlsym(void) {
    char name[256];
    gm_read_cstr(trap_arg(1), name, sizeof name);
    trap_return(lookup(name));
}

static void h_ns_is_symbol_name_defined(void) {
    trap_return(lookup_linker_name(trap_arg(0)) != 0);
}

/* An NSSymbol is the symbol's address. */
static void h_ns_lookup_and_bind_symbol(void) {
    trap_return(lookup_linker_name(trap_arg(0)));
}

static void h_ns_address_of_symbol(void) {
    trap_return(trap_arg(0));
}

/* ---- sprintf ---- */

static void append(char *out, size_t cap, size_t *len, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++, (*len)++)
        if (*len + 1 < cap)
            out[*len] = s[i];
}

size_t libc_format(const char *fmt, uint32_t (*arg)(int n), int first, char *out, size_t cap) {
    size_t len = 0;
    int next = first;
    for (const char *p = fmt; *p;) {
        if (*p != '%') {
            append(out, cap, &len, p++, 1);
            continue;
        }
        p++;
        char spec[48] = "%";
        size_t k = 1;
        while (*p && strchr("-+ #0", *p) && k < 8)
            spec[k++] = *p++;
        for (int part = 0; part < 2; part++) { /* width, then precision */
            if (part == 1) {
                if (*p != '.')
                    break;
                spec[k++] = *p++;
            }
            if (*p == '*') {
                k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", (int32_t)arg(next++));
                p++;
            } else {
                while (*p >= '0' && *p <= '9' && k < sizeof spec - 8)
                    spec[k++] = *p++;
            }
        }
        /* A long is a word on 32-bit PowerPC, so l is dropped; h and hh
           narrow the word, as the host's printf does. */
        if (p[0] == 'l' && p[1] == 'l')
            trap_crash("sprintf: unsupported conversion %%ll in \"%s\"", fmt);
        if (*p == 'l')
            p++;
        while (*p == 'h' && k < sizeof spec - 4)
            spec[k++] = *p++;
        char conv = *p ? *p++ : '\0';
        spec[k++] = conv;
        spec[k] = '\0';
        char tmp[4096];
        int n;
        switch (conv) {
        case 'd':
        case 'i':
        case 'c':
            n = snprintf(tmp, sizeof tmp, spec, (int32_t)arg(next++));
            break;
        case 'u':
        case 'x':
        case 'X':
            n = snprintf(tmp, sizeof tmp, spec, arg(next++));
            break;
        case 's': {
            char s[2048] = "(null)";
            uint32_t a = arg(next++);
            if (a)
                gm_read_cstr(a, s, sizeof s);
            n = snprintf(tmp, sizeof tmp, spec, s);
            break;
        }
        case '%':
            n = snprintf(tmp, sizeof tmp, "%%");
            break;
        default:
            trap_crash("sprintf: unsupported conversion %%%c in \"%s\"", conv ? conv : '?', fmt);
        }
        append(out, cap, &len, tmp, n < 0 ? 0 : (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
    }
    if (cap)
        out[len < cap ? len : cap - 1] = '\0';
    return len < cap ? len : cap - 1;
}

static void h_sprintf(void) {
    char fmt[1024];
    if (!gm_read_cstr(trap_arg(1), fmt, sizeof fmt))
        trap_crash("sprintf: the format is longer than %zu bytes", sizeof fmt - 1);
    static char out[65536];
    size_t n = libc_format(fmt, trap_arg, 2, out, sizeof out);
    if (n + 1 == sizeof out)
        trap_crash("sprintf: more than %zu bytes from \"%s\"", sizeof out - 2, fmt);
    memcpy(gm_ptr(trap_arg(0), (uint32_t)n + 1), out, n + 1);
    trap_return((uint32_t)n);
}

/* ---- keymgr and dyld: what crt1 and libgcc's startup would call ---- */

static void h_keymgr_get_and_lock(void) {
    uint32_t key = trap_arg(0);
    for (int i = 0; i < L.nkeymgr; i++)
        if (L.keymgr[i].key == key) {
            trap_return(L.keymgr[i].ptr);
            return;
        }
    trap_return(0);
}

static void h_keymgr_set_and_unlock(void) {
    uint32_t key = trap_arg(0), ptr = trap_arg(1);
    for (int i = 0; i < L.nkeymgr; i++)
        if (L.keymgr[i].key == key) {
            L.keymgr[i].ptr = ptr;
            trap_return(0);
            return;
        }
    if (L.nkeymgr == KEYMGR_KEYS)
        trap_crash("keymgr: more than %d keys", KEYMGR_KEYS);
    L.keymgr[L.nkeymgr].key = key;
    L.keymgr[L.nkeymgr++].ptr = ptr;
    trap_return(0);
}

static void h_nothing(void) {
    trap_return(0);
}

static void h_dyld_stub_binding_helper(void) {
    trap_crash("a lazy pointer was not bound (dyld_stub_binding_helper)");
}

void libc_register(void) {
    trap_register("malloc", h_malloc);
    trap_register("calloc", h_calloc);
    trap_register("free", h_free);
    trap_register("memcpy", h_memmove);
    trap_register("memmove", h_memmove);
    trap_register("memset", h_memset);
    trap_register("strlen", h_strlen);
    trap_register("strcmp", h_strcmp);
    trap_register("strcpy", h_strcpy);
    trap_register("strncpy", h_strncpy);
    trap_register("strcat", h_strcat);
    trap_register("qsort", h_qsort);
    trap_register("rand", h_rand);
    trap_register("srand", h_srand);
    trap_register("time", h_time);
    trap_register("localtime", h_localtime);
    trap_register("strftime", h_strftime);
    trap_register("usleep", h_usleep);
    trap_register("exit", h_exit);
    trap_register("abort", h_abort);
    trap_register("__maskrune", h_maskrune);
    trap_register("__toupper", h_toupper);
    trap_register("dlopen", h_dlopen);
    trap_register("dlsym", h_dlsym);
    trap_register("NSIsSymbolNameDefinedWithHint", h_ns_is_symbol_name_defined);
    trap_register("NSLookupAndBindSymbolWithHint", h_ns_lookup_and_bind_symbol);
    trap_register("NSAddressOfSymbol", h_ns_address_of_symbol);
    trap_register("sprintf", h_sprintf);
    trap_register("_keymgr_get_and_lock_processwide_ptr", h_keymgr_get_and_lock);
    trap_register("_keymgr_set_and_unlock_processwide_ptr", h_keymgr_set_and_unlock);
    trap_register("_init_keymgr", h_nothing);
    trap_register("__keymgr_dwarf2_register_sections", h_nothing);
    trap_register("_dyld_register_func_for_add_image", h_nothing);
    trap_register("_dyld_register_func_for_remove_image", h_nothing);
    trap_register("dyld_stub_binding_helper", h_dyld_stub_binding_helper);
}
