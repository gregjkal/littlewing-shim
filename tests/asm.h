#pragma once
/* A few PowerPC instruction encoders for hand-written guest code in tests. */
#include <stdint.h>

static inline uint32_t ppc_lis(int rd, uint32_t imm) { return 0x3C000000u | (uint32_t)rd << 21 | (imm & 0xFFFF); }
static inline uint32_t ppc_ori(int ra, int rs, uint32_t imm) {
    return 0x60000000u | (uint32_t)rs << 21 | (uint32_t)ra << 16 | (imm & 0xFFFF);
}
static inline uint32_t ppc_lwz(int rd, int16_t d, int ra) {
    return 0x80000000u | (uint32_t)rd << 21 | (uint32_t)ra << 16 | (uint16_t)d;
}
static inline uint32_t ppc_stw(int rs, int16_t d, int ra) {
    return 0x90000000u | (uint32_t)rs << 21 | (uint32_t)ra << 16 | (uint16_t)d;
}
static inline uint32_t ppc_addi(int rd, int ra, int16_t imm) {
    return 0x38000000u | (uint32_t)rd << 21 | (uint32_t)ra << 16 | (uint16_t)imm;
}
static inline uint32_t ppc_cmpwi(int ra, int16_t imm) { return 0x2C000000u | (uint32_t)ra << 16 | (uint16_t)imm; }
/* blt with a byte offset relative to this instruction. */
static inline uint32_t ppc_blt(int16_t off) { return 0x41800000u | ((uint16_t)off & 0xFFFC); }

#define PPC_MFLR_R0   0x7C0802A6u
#define PPC_MTLR_R0   0x7C0803A6u
#define PPC_MTCTR_R0  0x7C0903A6u
#define PPC_BCTRL     0x4E800421u
#define PPC_BLR       0x4E800020u
#define PPC_SAVE_LR   0x90010008u /* stw r0,8(r1) */
#define PPC_LOAD_LR   0x80010008u /* lwz r0,8(r1) */
#define PPC_PUSH64    0x9421FFC0u /* stwu r1,-64(r1) */
#define PPC_POP64     0x38210040u /* addi r1,r1,64 */
