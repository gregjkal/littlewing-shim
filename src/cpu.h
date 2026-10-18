#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Why cpu_run() stopped. */
typedef enum {
    CPU_STOP_RETURN, /* reached GUEST_RETURN_MAGIC */
    CPU_STOP_TRAP,   /* jumped to a trap address; addr = that address */
    CPU_STOP_FAULT,  /* anything else; addr = faulting address, detail = why */
} cpu_stop_kind;

typedef struct {
    cpu_stop_kind kind;
    uint32_t addr;
    uint32_t pc;
    char detail[96];
} cpu_stop;

typedef struct cpu_context cpu_context;

/* Creates the CPU and maps guest memory. Requires gm_init(). Replaces any
   previous CPU. */
void cpu_init(void);
void cpu_shutdown(void);

uint32_t cpu_gpr(int n);
void cpu_set_gpr(int n, uint32_t v);
double cpu_fpr(int n);
void cpu_set_fpr(int n, double v);
uint32_t cpu_lr(void);
void cpu_set_lr(uint32_t v);
uint32_t cpu_ctr(void);
uint32_t cpu_pc(void);

/* Runs guest code from pc until it returns to GUEST_RETURN_MAGIC, jumps to a
   trap address, or faults. Resume after a trap with cpu_run(cpu_lr()). */
cpu_stop cpu_run(uint32_t pc);

/* Snapshot of all registers. cpu_restore() restores and frees it. */
cpu_context *cpu_save(void);
void cpu_restore(cpu_context *ctx);

/* The bytes a saved context takes (what a snapshot of the emulator holds
   for the CPU; snapshot.h). */
size_t cpu_context_size(void);

/* For snapshots (snapshot.h): a copy of ctx, freed by cpu_restore() or
   cpu_context_free(); overwriting dst with src's registers; loading ctx's
   registers into the CPU, keeping ctx; and dropping every translated block,
   for after guest memory changed behind the CPU's back. */
cpu_context *cpu_context_clone(const cpu_context *ctx);
void cpu_context_copy(cpu_context *dst, const cpu_context *src);
void cpu_context_free(cpu_context *ctx);
void cpu_load(const cpu_context *ctx);
void cpu_flush_translations(void);

/* Whether a and b hold the same registers (the general and floating-point
   ones, pc, lr, ctr, cr, xer, msr and fpscr); if not, the first that
   differs, in what ("r3 0x00000001, not 0x00000002"). The rest of a saved
   context is the emulator's, not the machine's. */
bool cpu_context_same(const cpu_context *a, const cpu_context *b, char *what, size_t cap);

/* For a snapshot kept in a recording (snapshot.h), which another process
   puts back: a context as those registers alone, in that order, each in
   64 bits (the 32-bit ones in the low half); and a context of the CPU as
   it is now with regs written into it, freed with cpu_context_free. */
#define CPU_REGS 71
void cpu_context_regs(const cpu_context *ctx, uint64_t regs[CPU_REGS]);
cpu_context *cpu_context_with_regs(const uint64_t regs[CPU_REGS]);

/* Calls fn for every guest write that touches [begin, end] (inclusive), with
   the CPU's registers (including pc) current. NULL removes the watch.
   cpu_init() clears it. */
typedef void (*cpu_write_fn)(uint32_t addr, int size, uint64_t value);
void cpu_watch_writes(uint32_t begin, uint32_t end, cpu_write_fn fn);

/* Calls fn each time the guest is about to run the instruction at addr,
   with the CPU's registers current. Only that instruction pays for it. At
   most CPU_MAX_EXEC_HOOKS; cpu_init() clears them. */
#define CPU_MAX_EXEC_HOOKS 4
typedef void (*cpu_exec_fn)(void);
void cpu_on_exec(uint32_t addr, cpu_exec_fn fn);
