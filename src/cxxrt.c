#include "cxxrt.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "guest_mem.h"
#include "loader.h"
#include "memmgr.h"
#include "trap.h"
#include "util.h"

/* libstdc++'s __cxa_exception, in front of every thrown object on 32-bit
   PowerPC: eleven words, then a 20-byte _Unwind_Exception, rounded to 64. */
#define EXCEPTION_HEADER 64u
#define VTABLE_SIZE 64u

static const char *const vtable_names[3] = {
    "_ZTVN10__cxxabiv117__class_type_infoE",
    "_ZTVN10__cxxabiv120__si_class_type_infoE",
    "_ZTVN10__cxxabiv121__vmi_class_type_infoE",
};

static struct {
    uint32_t vtables[3];
} X;

void cxxrt_init(void) {
    for (int i = 0; i < 3; i++) {
        X.vtables[i] = mm_new_ptr(VTABLE_SIZE, true);
        if (!X.vtables[i])
            fatal("can't allocate the C++ runtime's data");
    }
}

uint32_t cxxrt_data_symbol(const char *name) {
    for (int i = 0; i < 3; i++)
        if (strcmp(name, vtable_names[i]) == 0)
            return X.vtables[i];
    if (strcmp(name, "__cxa_pure_virtual") == 0 || strcmp(name, "__gxx_personality_v0") == 0)
        return IMAGE_SYMBOL_CODE;
    return 0;
}

/* Reads one <length><name> source name at *p. */
static bool source_name(const char **p, char *out, size_t cap, size_t *len) {
    if (!isdigit((unsigned char)**p))
        return false;
    char *end;
    unsigned long n = strtoul(*p, &end, 10);
    if (n == 0 || strlen(end) < n)
        return false;
    for (unsigned long i = 0; i < n && *len + 1 < cap; i++)
        out[(*len)++] = end[i];
    out[*len] = '\0';
    *p = end + n;
    return true;
}

void cxxrt_type_name(const char *mangled, char *out, size_t cap) {
    size_t len = 0;
    const char *p = mangled;
    bool ok;
    if (*p == 'N') {
        p++;
        ok = true;
        for (bool first = true; ok && *p && *p != 'E'; first = false) {
            if (!first && len + 3 < cap) {
                strcpy(out + len, "::");
                len += 2;
            }
            ok = source_name(&p, out, cap, &len);
        }
        ok = ok && *p == 'E' && p[1] == '\0';
    } else {
        ok = source_name(&p, out, cap, &len) && *p == '\0';
    }
    if (!ok)
        snprintf(out, cap, "%s", mangled);
}

/* ---- new and delete ---- */

static void h_new(void) {
    uint32_t size = trap_arg(0);
    uint32_t p = mm_new_ptr(size, false);
    if (!p)
        trap_crash("operator new(%u) failed: the heap is full", size);
    trap_return(p);
}

static void h_delete(void) {
    uint32_t p = trap_arg(0);
    if (p && mm_dispose_ptr(p) != MM_NO_ERR)
        trap_crash("operator delete(0x%08x): not a block new returned", p);
}

/* ---- static-local guards: one thread, so a flag byte ---- */

static void h_guard_acquire(void) {
    trap_return(gm_r8(trap_arg(0)) == 0);
}

static void h_guard_release(void) {
    gm_w8(trap_arg(0), 1);
}

static void h_pure_virtual(void) {
    trap_crash("pure virtual function called");
}

/* ---- exceptions ---- */

static void h_allocate_exception(void) {
    uint32_t size = trap_arg(0);
    uint32_t p = mm_new_ptr(EXCEPTION_HEADER + size, true);
    if (!p)
        trap_crash("__cxa_allocate_exception(%u) failed: the heap is full", size);
    trap_return(p + EXCEPTION_HEADER);
}

/* Where the game called the import from: the bl before the return address. */
static const char *call_site(char buf[static 32]) {
    return trap_format_addr(cpu_lr() - 4, buf);
}

static void h_throw(void) {
    uint32_t obj = trap_arg(0), tinfo = trap_arg(1);
    char mangled[256] = "?", name[256], at[32];
    if (tinfo && gm_is_backed(tinfo + 4, 4) && gm_r32(tinfo + 4))
        gm_read_cstr(gm_r32(tinfo + 4), mangled, sizeof mangled);
    cxxrt_type_name(mangled, name, sizeof name);
    if (obj && gm_is_backed(obj, 16))
        log_msg("thrown object: %08x %08x %08x %08x", gm_r32(obj), gm_r32(obj + 4),
                gm_r32(obj + 8), gm_r32(obj + 12));
    trap_crash("the game threw %s at %s", name, call_site(at));
}

static void h_rethrow(void) {
    char at[32];
    trap_crash("the game rethrew an exception at %s", call_site(at));
}

static void h_unwind_resume(void) {
    char at[32];
    trap_crash("_Unwind_Resume at %s: an exception is unwinding", call_site(at));
}

static void h_begin_catch(void) {
    char at[32];
    trap_crash("__cxa_begin_catch at %s, but nothing was thrown", call_site(at));
}

static void h_end_catch(void) {
    char at[32];
    trap_crash("__cxa_end_catch at %s, but nothing was thrown", call_site(at));
}

static void h_personality(void) {
    trap_crash("__gxx_personality_v0 called: an exception is unwinding");
}

void cxxrt_register(void) {
    trap_register("_Znwm", h_new);
    trap_register("_Znam", h_new);
    trap_register("_ZdlPv", h_delete);
    trap_register("_ZdaPv", h_delete);
    trap_register("__cxa_guard_acquire", h_guard_acquire);
    trap_register("__cxa_guard_release", h_guard_release);
    trap_register("__cxa_pure_virtual", h_pure_virtual);
    trap_register("__cxa_allocate_exception", h_allocate_exception);
    trap_register("__cxa_throw", h_throw);
    trap_register("__cxa_rethrow", h_rethrow);
    trap_register("_Unwind_Resume", h_unwind_resume);
    trap_register("__cxa_begin_catch", h_begin_catch);
    trap_register("__cxa_end_catch", h_end_catch);
    trap_register("__gxx_personality_v0", h_personality);
}
