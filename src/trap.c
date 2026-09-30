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
} T;

static const char *fmt_addr(uint32_t a, char buf[static 32]) {
    if (T.code_len && a >= T.code_base && a - T.code_base < T.code_len)
        snprintf(buf, 32, "code+0x%05x", a - T.code_base);
    else
        snprintf(buf, 32, "0x%08x", a);
    return buf;
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
    cpu_set_gpr(1, GUEST_STACK_TOP - 64);
}

void trap_shutdown(void) {
    free(T.handlers);
    memset(&T, 0, sizeof T);
}

void trap_register(const char *name, trap_handler fn) {
    for (uint32_t i = 0; i < T.n; i++)
        if (strcmp(T.names[i], name) == 0)
            T.handlers[i] = fn;
}

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
    va_list ap;
    va_start(ap, fmt);
    fputs("loony: crash: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    report_state();
    exit(2);
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
    uint32_t code = gm_r32(tvector), toc = gm_r32(tvector + 4);
    cpu_context *saved = cpu_save();

    uint32_t old_sp = cpu_gpr(1);
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
        if (!T.handlers[index])
            trap_crash("unimplemented import %s", T.names[index]);
        uint32_t resume = cpu_lr();
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
    T.depth--;

    uint32_t result = cpu_gpr(3);
    cpu_restore(saved);
    return result;
}
