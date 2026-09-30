#pragma once
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
