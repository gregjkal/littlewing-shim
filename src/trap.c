#include "trap.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_mem.h"
#include "util.h"

#define HISTORY 64
/* Below the caller's r1: 224-byte red zone plus room for a linkage area. */
#define CALL_FRAME_GAP 512u

typedef struct {
    uint32_t index, lr, a[4];
} hist_entry;

static struct {
    uint32_t n;
    const char *const *names;
    trap_handler *handlers;
    uint32_t code_base, code_len;
    hist_entry hist[HISTORY];
    uint32_t hist_count;
    int depth;
    bool trace_imports;
    bool trace_calls;
    bool stub_all;
    bool direct_calls;
    uint8_t lowmem_seen[GUEST_LOWMEM_SIZE / 8]; /* one bit per address already logged */
} T;

static const char *fmt_addr(uint32_t a, char buf[static 32]) {
    if (T.code_len && a >= T.code_base && a - T.code_base < T.code_len)
        snprintf(buf, 32, "code+0x%05x", a - T.code_base);
    else
        snprintf(buf, 32, "0x%08x", a);
    return buf;
}

static _Noreturn void on_guest_fault(const char *msg) {
    trap_crash("%s", msg);
}

/* Logs the first write to each low-memory address. */
static void on_lowmem_write(uint32_t addr, int size, uint64_t value) {
    uint32_t off = addr - GUEST_LOWMEM_BASE;
    if (T.lowmem_seen[off / 8] & (1u << (off % 8)))
        return;
    T.lowmem_seen[off / 8] |= (uint8_t)(1u << (off % 8));
    char a[32];
    fprintf(stderr, "loony: lowmem: write 0x%04x = 0x%0*llx (%d bytes) at %s\n", addr, size * 2,
            (unsigned long long)value, size, fmt_addr(cpu_pc(), a));
}

void trap_init(uint32_t nimports, const char *const *names, uint32_t code_base,
               uint32_t code_len) {
    trap_shutdown();
    T.n = nimports;
    T.names = names;
    T.handlers = calloc(nimports ? nimports : 1, sizeof *T.handlers);
    if (!T.handlers)
        fatal("out of memory");
    T.code_base = code_base;
    T.code_len = code_len;
    const char *trace = getenv("LOONY_TRACE");
    T.trace_imports = trace && strstr(trace, "imports");
    T.trace_calls = trace && strstr(trace, "calls");
    /* A Mach-O program has no low memory: its image starts at 0x1000. */
    if (trace && strstr(trace, "lowmem") && gm_current_layout() == GM_LAYOUT_PEF)
        cpu_watch_writes(GUEST_LOWMEM_BASE, GUEST_LOWMEM_BASE + GUEST_LOWMEM_SIZE - 1,
                         on_lowmem_write);
    const char *stub = getenv("LOONY_STUB");
    T.stub_all = stub && strcmp(stub, "all") == 0;
    gm_set_fault_handler(on_guest_fault);
    cpu_set_gpr(1, GUEST_STACK_TOP - 64);
}

void trap_shutdown(void) {
    gm_set_fault_handler(NULL);
    free(T.handlers);
    memset(&T, 0, sizeof T);
}

void trap_register(const char *name, trap_handler fn) {
    for (uint32_t i = 0; i < T.n; i++)
        if (strcmp(T.names[i], name) == 0)
            T.handlers[i] = fn;
}

void trap_set_direct_calls(bool on) { T.direct_calls = on; }

bool trap_has_handler(uint32_t index) { return index < T.n && T.handlers[index]; }

const char *trap_import_name(uint32_t index) {
    return index < T.n ? T.names[index] : "(unknown)";
}

static void report_state(void) {
    char a[32], b[32];
    fprintf(stderr, "  pc %s  lr %s  ctr 0x%08x  depth %d\n", fmt_addr(cpu_pc(), a),
            fmt_addr(cpu_lr(), b), cpu_ctr(), T.depth);
    for (int r = 0; r < 32; r += 4)
        fprintf(stderr, "  r%-2d 0x%08x  r%-2d 0x%08x  r%-2d 0x%08x  r%-2d 0x%08x\n", r,
                cpu_gpr(r), r + 1, cpu_gpr(r + 1), r + 2, cpu_gpr(r + 2), r + 3, cpu_gpr(r + 3));
    uint32_t n = T.hist_count < HISTORY ? T.hist_count : HISTORY;
    fprintf(stderr, "  last %u imports (oldest first):\n", n);
    for (uint32_t k = T.hist_count - n; k < T.hist_count; k++) {
        const hist_entry *h = &T.hist[k % HISTORY];
        fprintf(stderr, "    #%u %s(0x%08x, 0x%08x, 0x%08x, 0x%08x) from %s\n", k,
                trap_import_name(h->index), h->a[0], h->a[1], h->a[2], h->a[3],
                fmt_addr(h->lr, a));
    }
}

void trap_crash(const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "loony: crash: %s\n", msg);
    report_state();
    util_report_failure(msg);
    exit(2);
}

uint32_t trap_arg(int n) {
    if (n < 0)
        fatal("trap_arg: bad argument index %d", n);
    if (n < 8)
        return cpu_gpr(3 + n);
    return gm_r32(cpu_gpr(1) + 24 + 4u * (uint32_t)n);
}

static const hist_entry *record(uint32_t index) {
    hist_entry *h = &T.hist[T.hist_count % HISTORY];
    h->index = index;
    h->lr = cpu_lr();
    for (int i = 0; i < 4; i++)
        h->a[i] = cpu_gpr(3 + i);
    T.hist_count++;
    return h;
}

uint32_t guest_call(uint32_t tvector, int nargs, const uint32_t *args) {
    if (nargs < 0 || nargs > 8)
        fatal("guest_call: bad argument count %d", nargs);
    /* GCC's Darwin code expects the callee's address in r12 when it's called
       through a pointer, and has no TOC: r2 is left as it is. */
    uint32_t code = T.direct_calls ? tvector : gm_r32(tvector);
    uint32_t toc = T.direct_calls ? cpu_gpr(2) : gm_r32(tvector + 4);
    cpu_context *saved = cpu_save();

    uint32_t old_sp = cpu_gpr(1);
    if (old_sp < GUEST_STACK_BASE || old_sp > GUEST_STACK_TOP)
        trap_crash("guest stack pointer 0x%08x is outside the stack", old_sp);
    uint32_t sp = (old_sp - CALL_FRAME_GAP) & ~15u;
    if (sp < GUEST_STACK_BASE + 4096)
        trap_crash("guest stack exhausted");
    gm_w32(sp, old_sp); /* back chain */
    cpu_set_gpr(1, sp);
    cpu_set_gpr(2, toc);
    cpu_set_gpr(12, tvector);
    for (int i = 0; i < nargs; i++)
        cpu_set_gpr(3 + i, args[i]);
    cpu_set_lr(GUEST_RETURN_MAGIC);

    T.depth++;
    if (T.trace_calls) {
        char a[32];
        fprintf(stderr, "loony: trace: call %s(", fmt_addr(code, a));
        for (int i = 0; i < nargs; i++)
            fprintf(stderr, "%s0x%08x", i ? ", " : "", args[i]);
        fprintf(stderr, ") depth %d\n", T.depth);
    }
    uint32_t pc = code;
    for (;;) {
        cpu_stop s = cpu_run(pc);
        if (s.kind == CPU_STOP_RETURN)
            break;
        if (s.kind == CPU_STOP_FAULT) {
            char a[32];
            trap_crash("guest fault: %s (pc %s)", s.detail, fmt_addr(s.pc, a));
        }
        uint32_t index = (s.addr - GUEST_TRAP_BASE) / 4;
        if (index >= T.n)
            trap_crash("jump to unused trap address 0x%08x", s.addr);
        const hist_entry *h = record(index);
        uint32_t resume = cpu_lr();
        if (!T.handlers[index]) {
            if (!T.stub_all)
                trap_crash("unimplemented import %s", T.names[index]);
            char a[32];
            fprintf(stderr, "loony: stub: #%u %s(0x%08x, 0x%08x, 0x%08x, 0x%08x) from %s\n",
                    T.hist_count - 1, T.names[index], h->a[0], h->a[1], h->a[2], h->a[3],
                    fmt_addr(h->lr, a));
            trap_return(0);
            pc = resume;
            continue;
        }
        if (T.trace_imports) {
            char a[32];
            fprintf(stderr, "loony: trace: #%u %s(0x%08x, 0x%08x, 0x%08x, 0x%08x) from %s\n",
                    T.hist_count - 1, T.names[index], h->a[0], h->a[1], h->a[2], h->a[3],
                    fmt_addr(h->lr, a));
        }
        T.handlers[index]();
        if (T.trace_imports)
            fprintf(stderr, "loony: trace:   -> 0x%08x\n", cpu_gpr(3));
        pc = resume;
    }
    uint32_t result = cpu_gpr(3);
    if (T.trace_calls) {
        char a[32];
        fprintf(stderr, "loony: trace: return 0x%08x from %s depth %d\n", result,
                fmt_addr(code, a), T.depth);
    }
    T.depth--;

    cpu_restore(saved);
    return result;
}
