#pragma once
#include "cpu.h"
#include "guest_mem.h"

static inline void put_words(uint32_t addr, const uint32_t *words, int n) {
    for (int i = 0; i < n; i++)
        gm_w32(addr + 4u * (uint32_t)i, words[i]);
}

/* Fresh memory and CPU, stack pointer near the stack top, LR = RETURN_MAGIC. */
static inline void fresh_machine(void) {
    gm_init();
    cpu_init();
    cpu_set_gpr(1, GUEST_STACK_TOP - 64);
    cpu_set_lr(GUEST_RETURN_MAGIC);
}
