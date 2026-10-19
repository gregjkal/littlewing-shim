#include "cpu.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>

#include "guest_mem.h"
#include "util.h"

#define MSR_FP 0x2000u
#define TCG_BUFFER_SIZE (64u << 20)

struct cpu_context {
    uc_context *uc_ctx;
};

static uc_engine *uc;
static uc_hook mem_hook, intr_hook, write_hook;
static cpu_write_fn write_fn;
static int nexec_hooks;
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
    /* The default translation cache is 1 GB; the game's code is 280 KB. */
    check(uc_ctl_set_tcg_buffer_size(uc, TCG_BUFFER_SIZE), "set translation cache size");
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

void cpu_no_exec(uint32_t begin, uint32_t end) {
    if (begin < end)
        check(uc_mem_protect(uc, begin, end - begin, UC_PROT_READ | UC_PROT_WRITE), "protect memory");
}

void cpu_shutdown(void) {
    if (uc)
        uc_close(uc);
    uc = NULL;
    write_fn = NULL;
    nexec_hooks = 0;
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

static void on_exec(uc_engine *engine, uint64_t address, uint32_t size, void *user) {
    (void)engine;
    (void)address;
    (void)size;
    ((cpu_exec_fn)user)();
}

void cpu_on_exec(uint32_t addr, cpu_exec_fn fn) {
    if (nexec_hooks == CPU_MAX_EXEC_HOOKS)
        fatal("cpu_on_exec: more than %d hooks", CPU_MAX_EXEC_HOOKS);
    uc_hook h;
    /* A bounded code hook is only compiled into the instructions it covers. */
    check(uc_hook_add(uc, &h, UC_HOOK_CODE, (void *)on_exec, (void *)fn, addr, addr),
          "add code hook");
    nexec_hooks++;
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

size_t cpu_context_size(void) { return uc_context_size(uc); }

/* The copies go through the CPU, so only Unicorn's public calls touch a
   context: load src, save it where it goes, and put the CPU back. */
void cpu_context_copy(cpu_context *dst, const cpu_context *src) {
    uc_context *live;
    check(uc_context_alloc(uc, &live), "context alloc");
    check(uc_context_save(uc, live), "context save");
    check(uc_context_restore(uc, src->uc_ctx), "context restore");
    check(uc_context_save(uc, dst->uc_ctx), "context save");
    check(uc_context_restore(uc, live), "context restore");
    uc_context_free(live);
}

cpu_context *cpu_context_clone(const cpu_context *ctx) {
    cpu_context *copy = cpu_save();
    cpu_context_copy(copy, ctx);
    return copy;
}

void cpu_context_free(cpu_context *ctx) {
    if (ctx) {
        uc_context_free(ctx->uc_ctx);
        free(ctx);
    }
}

void cpu_load(const cpu_context *ctx) {
    check(uc_context_restore(uc, ctx->uc_ctx), "context restore");
}

/* The machine's registers, in the order cpu_context_same and
   cpu_context_regs take them: the general ones, the floating-point ones,
   then these. */
static const struct {
    int id;
    const char *name;
} named[] = {{UC_PPC_REG_PC, "pc"},   {UC_PPC_REG_LR, "lr"},   {UC_PPC_REG_CTR, "ctr"},
             {UC_PPC_REG_CR, "cr"},   {UC_PPC_REG_XER, "xer"}, {UC_PPC_REG_MSR, "msr"},
             {UC_PPC_REG_FPSCR, "fpscr"}};
_Static_assert(64 + sizeof named / sizeof named[0] == CPU_REGS, "CPU_REGS");

static int reg_id(int i) {
    return i < 32 ? UC_PPC_REG_0 + i : i < 64 ? UC_PPC_REG_FPR0 + i - 32 : named[i - 64].id;
}

/* Register i of ctx, the 32-bit ones in their low half. */
static uint64_t reg_of(const cpu_context *ctx, int i) {
    uint64_t v = 0;
    check(uc_context_reg_read(ctx->uc_ctx, reg_id(i), &v), "context read");
    return i < 32 || i >= 64 ? (uint32_t)v : v;
}

bool cpu_context_same(const cpu_context *a, const cpu_context *b, char *what, size_t cap) {
    for (int i = 0; i < CPU_REGS; i++) {
        uint64_t va = reg_of(a, i), vb = reg_of(b, i);
        if (va == vb)
            continue;
        char name[8];
        if (i < 64)
            snprintf(name, sizeof name, "%s%d", i < 32 ? "r" : "f", i % 32);
        snprintf(what, cap, "%s 0x%0*llx, not 0x%0*llx", i < 64 ? name : named[i - 64].name,
                 i >= 32 && i < 64 ? 16 : 8, (unsigned long long)va, i >= 32 && i < 64 ? 16 : 8,
                 (unsigned long long)vb);
        return false;
    }
    return true;
}

void cpu_context_regs(const cpu_context *ctx, uint64_t regs[CPU_REGS]) {
    for (int i = 0; i < CPU_REGS; i++)
        regs[i] = reg_of(ctx, i);
}

cpu_context *cpu_context_with_regs(const uint64_t regs[CPU_REGS]) {
    cpu_context *ctx = cpu_save();
    for (int i = 0; i < CPU_REGS; i++) {
        uint64_t v = regs[i];
        check(uc_context_reg_write(ctx->uc_ctx, reg_id(i), &v), "context write");
    }
    return ctx;
}

void cpu_flush_translations(void) {
    check(uc_ctl_flush_tb(uc), "flush translations");
    check(uc_ctl_flush_tlb(uc), "flush TLB");
}
