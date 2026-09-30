#pragma once
#include <stdint.h>

#include "cpu.h"

/* A C implementation of one imported function. Reads arguments with
   trap_arg() and sets the result with trap_return(). */
typedef void (*trap_handler)(void);

/* Requires cpu_init(). names[i] is import i's name and must outlive trap use.
   code_base/code_len are used to print code addresses as code+0xNNNNN. */
void trap_init(uint32_t nimports, const char *const *names, uint32_t code_base,
               uint32_t code_len);
void trap_shutdown(void);

/* Installs fn for the import called name. Ignored if the game doesn't import it. */
void trap_register(const char *name, trap_handler fn);

/* Calls the guest function whose transition vector is at tvector, with up to
   8 word arguments in r3..r10. Returns the guest's r3. Re-entrant: handlers
   may call it again. All registers are restored before it returns. */
uint32_t guest_call(uint32_t tvector, int nargs, const uint32_t *args);

const char *trap_import_name(uint32_t index);

/* Prints a crash report (message, registers, depth, recent imports) and exits 2. */
_Noreturn void trap_crash(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static inline uint32_t trap_arg(int n) { return cpu_gpr(3 + n); }
static inline void trap_return(uint32_t v) { cpu_set_gpr(3, v); }
