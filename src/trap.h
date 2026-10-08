#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "cpu.h"

/* A C implementation of one imported function. Reads arguments with
   trap_arg() and sets the result with trap_return(). */
typedef void (*trap_handler)(void);

/* Requires cpu_init(). names[i] is import i's name and must outlive trap use.
   code_base/code_len are used to print code addresses as code+0xNNNNN. Reads
   LOONY_TRACE and LOONY_STUB (with LOONY_STUB=all, imports with no handler
   log a "stub:" line and return 0 instead of crashing). */
void trap_init(uint32_t nimports, const char *const *names, uint32_t code_base,
               uint32_t code_len);
void trap_shutdown(void);

/* Installs fn for the import called name. Ignored if the game doesn't import it. */
void trap_register(const char *name, trap_handler fn);

/* Calls the guest function whose transition vector is at tvector, with up to
   8 word arguments in r3..r10. Returns the guest's r3. Re-entrant: handlers
   may call it again. All registers are restored before it returns. With
   direct calls on, tvector is the function's code address instead. */
uint32_t guest_call(uint32_t tvector, int nargs, const uint32_t *args);

/* On: guest_call's first argument is a code address, as a Mach-O program's
   function pointers are. Off (the default, for PEF): a transition vector.
   trap_init() turns it off. */
void trap_set_direct_calls(bool on);

const char *trap_import_name(uint32_t index);

/* Index of the import called name, or -1. */
int32_t trap_find(const char *name);

/* True if import index has a handler. */
bool trap_has_handler(uint32_t index);

/* Prints a crash report (message, registers, depth, recent imports) and exits 2. */
_Noreturn void trap_crash(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Word argument n of the current import call: r3..r10 for n = 0..7, then the
   caller's parameter area at r1 + 24 + 4n. */
uint32_t trap_arg(int n);
static inline void trap_return(uint32_t v) { cpu_set_gpr(3, v); }
