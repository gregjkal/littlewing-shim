#include "cpu.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>

#include "guest_mem.h"
#include "util.h"

#define MSR_FP 0x2000u

struct cpu_context {
    uc_context *uc_ctx;
};

static uc_engine *uc;
static uc_hook mem_hook, intr_hook, write_hook;
static cpu_write_fn write_fn;
static bool mem_fault;
static uc_mem_type mem_fault_type;
static uint32_t mem_fault_addr;
static bool intr_seen;
static uint32_t intr_no;

static void check(uc_err err, const char *what) {
    if (err != UC_ERR_OK)
        fatal("unicorn: %s failed: %s", what, uc_strerror(err));
}

static bool on_mem_invalid(uc_engine *engine, uc_mem_type type, uint64_t address, int size,
                           int64_t value, void *user) {
    (void)engine;
    (void)size;
    (void)value;
    (void)user;
    mem_fault = true;
    mem_fault_type = type;
    mem_fault_addr = (uint32_t)address;
    return false;
}

static void on_intr(uc_engine *engine, uint32_t intno, void *user) {
    (void)user;
    intr_seen = true;
    intr_no = intno;
    uc_emu_stop(engine);
}

static const char *mem_fault_desc(uc_mem_type type) {
    switch (type) {
    case UC_MEM_READ_UNMAPPED: return "read of unmapped address";
    case UC_MEM_WRITE_UNMAPPED: return "write to unmapped address";
    case UC_MEM_FETCH_UNMAPPED: return "jump to unmapped address";
    case UC_MEM_WRITE_PROT: return "write to read-only address";
    case UC_MEM_READ_PROT: return "read of unreadable address";
    case UC_MEM_FETCH_PROT: return "jump to non-executable address";
    default: return "memory fault at";
    }
}

static uint64_t reg_read(int reg) {
    uint64_t v = 0;
    check(uc_reg_read(uc, reg, &v), "register read");
    return v;
}

static void reg_write(int reg, uint64_t v) {
    check(uc_reg_write(uc, reg, &v), "register write");
}

void cpu_init(void) {
    if (uc)
        cpu_shutdown();
    check(uc_open(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN, &uc), "uc_open");
    check(uc_ctl_set_cpu_model(uc, UC_CPU_PPC32_750_V3_1), "set CPU model");
    const gm_region *regions;
    int n = gm_regions(&regions);
    for (int i = 0; i < n; i++) {
        uint32_t prot = 0;
        if (regions[i].prot & GM_PROT_R)
            prot |= UC_PROT_READ;
        if (regions[i].prot & GM_PROT_W)
            prot |= UC_PROT_WRITE;
        if (regions[i].prot & GM_PROT_X)
            prot |= UC_PROT_EXEC;
        check(uc_mem_map_ptr(uc, regions[i].base, regions[i].size, prot,
                             gm_host_base() + regions[i].base),
              "map guest memory");
    }
    check(uc_hook_add(uc, &mem_hook, UC_HOOK_MEM_INVALID, (void *)on_mem_invalid, NULL, 1, 0),
          "add memory hook");
    check(uc_hook_add(uc, &intr_hook, UC_HOOK_INTR, (void *)on_intr, NULL, 1, 0),
          "add interrupt hook");
    reg_write(UC_PPC_REG_MSR, reg_read(UC_PPC_REG_MSR) | MSR_FP);
}

void cpu_shutdown(void) {
    if (uc)
        uc_close(uc);
    uc = NULL;
    write_fn = NULL;
}

static void on_write(uc_engine *engine, uc_mem_type type, uint64_t address, int size,
                     int64_t value, void *user) {
    (void)engine;
    (void)type;
    (void)user;
    if (write_fn)
        write_fn((uint32_t)address, size, (uint64_t)value);
}

void cpu_watch_writes(uint32_t begin, uint32_t end, cpu_write_fn fn) {
    if (write_fn) {
        check(uc_hook_del(uc, write_hook), "remove write hook");
        write_fn = NULL;
    }
    if (!fn)
        return;
    check(uc_hook_add(uc, &write_hook, UC_HOOK_MEM_WRITE, (void *)on_write, NULL, begin, end),
          "add write hook");
    write_fn = fn;
}

uint32_t cpu_gpr(int n) { return (uint32_t)reg_read(UC_PPC_REG_0 + n); }
void cpu_set_gpr(int n, uint32_t v) { reg_write(UC_PPC_REG_0 + n, v); }
uint32_t cpu_lr(void) { return (uint32_t)reg_read(UC_PPC_REG_LR); }
void cpu_set_lr(uint32_t v) { reg_write(UC_PPC_REG_LR, v); }
uint32_t cpu_ctr(void) { return (uint32_t)reg_read(UC_PPC_REG_CTR); }
uint32_t cpu_pc(void) { return (uint32_t)reg_read(UC_PPC_REG_PC); }

double cpu_fpr(int n) {
    uint64_t bits = reg_read(UC_PPC_REG_FPR0 + n);
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

void cpu_set_fpr(int n, double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    reg_write(UC_PPC_REG_FPR0 + n, bits);
}

cpu_stop cpu_run(uint32_t pc) {
    cpu_stop s;
    memset(&s, 0, sizeof s);
    mem_fault = false;
    intr_seen = false;
    uc_err err = uc_emu_start(uc, pc, GUEST_RETURN_MAGIC, 0, 0);
    s.pc = cpu_pc();

    if (mem_fault && mem_fault_type == UC_MEM_FETCH_UNMAPPED) {
        uint32_t a = mem_fault_addr;
        if (a == GUEST_RETURN_MAGIC) {
            s.kind = CPU_STOP_RETURN;
            s.addr = a;
            return s;
        }
        if (a >= GUEST_TRAP_BASE && a < GUEST_TRAP_LIMIT && (a & 3u) == 0) {
            s.kind = CPU_STOP_TRAP;
            s.addr = a;
            return s;
        }
    }
    if (err == UC_ERR_OK && !mem_fault && !intr_seen && s.pc == GUEST_RETURN_MAGIC) {
        s.kind = CPU_STOP_RETURN;
        s.addr = s.pc;
        return s;
    }

    s.kind = CPU_STOP_FAULT;
    if (mem_fault) {
        s.addr = mem_fault_addr;
        snprintf(s.detail, sizeof s.detail, "%s 0x%08x", mem_fault_desc(mem_fault_type),
                 mem_fault_addr);
    } else if (intr_seen) {
        s.addr = s.pc;
        snprintf(s.detail, sizeof s.detail, "cpu exception %u", intr_no);
    } else {
        s.addr = s.pc;
        snprintf(s.detail, sizeof s.detail, "%s",
                 err == UC_ERR_OK ? "emulation stopped unexpectedly" : uc_strerror(err));
    }
    return s;
}

cpu_context *cpu_save(void) {
    cpu_context *ctx = malloc(sizeof *ctx);
    if (!ctx)
        fatal("out of memory");
    check(uc_context_alloc(uc, &ctx->uc_ctx), "context alloc");
    check(uc_context_save(uc, ctx->uc_ctx), "context save");
    return ctx;
}

void cpu_restore(cpu_context *ctx) {
    check(uc_context_restore(uc, ctx->uc_ctx), "context restore");
    uc_context_free(ctx->uc_ctx);
    free(ctx);
}
