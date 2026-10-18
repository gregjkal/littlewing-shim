#pragma once
#include <stdbool.h>
#include <stddef.h>
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

/* A guest address as crash reports print it: code+0xNNNNN inside the code,
   0xNNNNNNNN elsewhere. Returns buf. */
const char *trap_format_addr(uint32_t a, char buf[static 32]);

/* True if import index has a handler. */
bool trap_has_handler(uint32_t index);

/* How many traps the guest has entered at each guest_call depth since
   trap_init: counts[1] at depth 1 (the game's own code, called from main),
   counts[2] inside a callback a handler ran (Loony's event loop timer and
   its event handlers, where snapshots are taken; snapshot.h), and so on, the last count holding every deeper
   entry. counts[0] is unused. Returns how many counts there are. */
#define TRAP_DEPTH_COUNTS 8
size_t trap_depth_counts(const uint64_t **counts);

/* For snapshots (snapshot.h). trap_depth: the guest_call depth now (0
   outside any). For the guest_call at depth, 1 to trap_depth(): the CPU
   context it saved on entry and restores on return, which a restore
   overwrites, and the guest code address it called; NULL and 0 at any other
   depth.
   trap_on_entry: a hook run at every trap entry, before the entry is counted
   or recorded, with the import's index. It returns the index to dispatch,
   and the handler returns to the link register as the hook leaves it. NULL
   removes it. */
#define TRAP_MAX_DEPTH 32
int trap_depth(void);
cpu_context *trap_saved_context(int depth);
uint32_t trap_call_target(int depth);
typedef uint32_t (*trap_entry_fn)(uint32_t index);
void trap_on_entry(trap_entry_fn fn);

/* trap_level: the trap the guest_call at depth is dispatching (or last
   dispatched) and where the guest goes on after it; false at a depth with
   no guest_call. trap_unwind: after a longjmp out of every guest_call,
   frees the contexts they saved and forgets the levels, with the depth 0.
   guest_resume: a guest_call that rebuilds a level instead of starting a
   call. It takes the level as a snapshot held it: the function it called,
   outer (the context it saved, which it restores on return, copied), and
   cpu (the CPU when the guest stopped at trap index with resume in the link
   register, loaded). It dispatches that trap, runs on as guest_call does,
   and returns the guest's r3. */
bool trap_level(int depth, uint32_t *index, uint32_t *resume);
void trap_unwind(void);
uint32_t guest_resume(uint32_t code, const cpu_context *outer, const cpu_context *cpu,
                      uint32_t index, uint32_t resume);

/* Prints a crash report (message, registers, depth, recent imports) and exits 2. */
_Noreturn void trap_crash(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Word argument n of the current import call: r3..r10 for n = 0..7, then the
   caller's parameter area at r1 + 24 + 4n. */
uint32_t trap_arg(int n);
static inline void trap_return(uint32_t v) { cpu_set_gpr(3, v); }
