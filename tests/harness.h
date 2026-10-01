#pragma once
/* Calls imports through the real trap dispatch, without any guest code: each
   import's transition vector points straight at its trap address, so
   guest_call() stops at the trap, runs the handler, and returns. */
#include <stdarg.h>
#include <string.h>

#include "ppc.h"
#include "trap.h"
#include "util.h"

#define HARNESS_TV_BASE (GUEST_IMAGE_BASE + 0x80000)
#define HARNESS_SCRATCH (GUEST_IMAGE_BASE + 0x100000)

static struct {
    const char *const *names;
    uint32_t n;
    uint32_t scratch_next;
} harness;

/* Fresh machine and trap table with these import names. Register handlers after. */
static inline void harness_init(const char *const *names, uint32_t n) {
    fresh_machine();
    trap_init(n, names, GUEST_IMAGE_BASE, 0x10000);
    for (uint32_t i = 0; i < n; i++) {
        gm_w32(HARNESS_TV_BASE + 8 * i, GUEST_TRAP_ADDR(i));
        gm_w32(HARNESS_TV_BASE + 8 * i + 4, 0);
    }
    harness.names = names;
    harness.n = n;
    harness.scratch_next = HARNESS_SCRATCH;
}

/* Calls the named import with nargs word arguments and returns its r3. */
static inline uint32_t call_import(const char *name, int nargs, ...) {
    uint32_t args[8] = {0};
    va_list ap;
    va_start(ap, nargs);
    for (int i = 0; i < nargs; i++)
        args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    for (uint32_t i = 0; i < harness.n; i++)
        if (strcmp(harness.names[i], name) == 0)
            return guest_call(HARNESS_TV_BASE + 8 * i, nargs, args);
    fatal("harness: %s is not in the import table", name);
}

/* n bytes of zeroed guest memory in the image area, 16-byte aligned. */
static inline uint32_t scratch(uint32_t n) {
    uint32_t a = harness.scratch_next;
    harness.scratch_next = (a + n + 15) & ~15u;
    memset(gm_ptr(a, n ? n : 1), 0, n);
    return a;
}
