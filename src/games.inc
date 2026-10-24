/* MONSTER FAIR 1.2.5 (the rehearsal; its public key works anyway). The
   license check, code+0x42b48, refuses a key of 23 characters or fewer, then
   runs three checks and a fourth (code+0x42628) that compares the key's third
   character with one it computes. All of them still run; only their verdicts
   are dropped, so a key of 24 or more letters and digits is accepted and a
   shorter one, or none, is still refused. Every license check goes through
   code+0x42b48: saving after Register (code+0x42ccc), the verdicts in main
   after Register and at launch (code+0x42e28), and the second check in play
   (code+0x42be0, whose own verdict src/patch.c already drops). */
static const code_patch monster_fair[] = {
    {0x42B78, {0x4BFFFB9D, 0x2F830000, 0x409E0048}, PPC_NOP},     /* bl 0x42714; bne refused */
    {0x42B8C, {0x4BFFFEE1, 0x2F830000, 0x409E0034}, PPC_NOP},     /* bl 0x42a6c; bne refused */
    {0x42BA0, {0x4BFFFAF1, 0x2F830000, 0x409E0020}, PPC_NOP},     /* bl 0x42690; bne refused */
    {0x42670, {0x7C000774, 0x3040FFFF, 0x7C620110}, PPC_LI_R3_0}, /* r3 = character differs */
};

/* Jinni Zeala 1.2.2 (no key known). Its validation, code+0x77b0, is
   MONSTER FAIR's shape: it refuses a key of 23 characters or fewer, then
   runs three checks (code+0x72c0, 0x7690, 0x7570) and a fourth,
   code+0x7610, that compares the key's third character with one it
   computes. All of them still run; the three branches past a refusal become
   unconditional, and the fourth check's result becomes `li r3,0`. Every
   license check goes through code+0x77b0: the one at launch
   (code+0x23a40, on the saved license) and Register's two (code+0x23a80,
   before and after copying the license to save it). Register keeps only
   the key's digits, so a made-up key needs 24 or more of them. There is no
   second check during play. A PEF game's code is its first section, loaded
   at GUEST_IMAGE_BASE (0x100000), so code+0x77f4 is guest 0x1077f4. */
static const code_patch jinni_zeala[] = {
    {0x1077F4, {0x4BFFFACD, 0x5460063F, 0x4182000C}, PPC_B_12},    /* bl 0x72c0; beq accepted */
    {0x107810, {0x4BFFFE81, 0x5460063F, 0x4182000C}, PPC_B_12},    /* bl 0x7690; beq accepted */
    {0x10782C, {0x4BFFFD45, 0x5460063F, 0x4182000C}, PPC_B_12},    /* bl 0x7570; beq accepted */
    {0x107668, {0x7C002050, 0x7C600378, 0x54030FFE}, PPC_LI_R3_0}, /* r3 = character differs */
};

/* Mad Daedalus 1.1.9 (no key known). Register keeps the key's letters and
   digits, upper-cased and cut to 24, and saves it encoded with the machine
   id without checking it. The checks run while the game loads its tables:
   four functions (code+0x207a8, 0x20984, 0x20b30, 0x20cdc, called from
   code+0x1fa34, 0x1f4e8, 0x1ba40 and 0x1ac74) each read the saved license,
   refuse an empty email or key or a key that isn't 24 characters, then ask
   the license object: a range test on six of its digits (code+0x21d3c, twice)
   or code+0x21ee0, 0x21f40 and 0x21fcc. Each sets the refusal word at +0x58
   of the game object, which main turns into AuthorizeFailed. All of them
   still run, and the length test is kept; only the verdict branches become
   unconditional. The license object's own methods are left alone: two
   self-tests during play (code+0x34fb0, 0x3505c) feed them a known bad key
   and show BadApplication if it is accepted. Demo play skips every check. */
static const code_patch mad_daedalus[] = {
    {0x20918, {0x6000980F, 0x7F830040, 0x40BD0034}, PPC_NOP}, /* digits <= 169999: refused */
    {0x20948, {0x7F830040, 0x38000000, 0x409D0008}, PPC_B_8}, /* digits <= 999999: accepted */
    {0x20AF4, {0x38000000, 0x2F830000, 0x419E0008}, PPC_B_8}, /* code+0x21ee0: beq accepted */
    {0x20CA0, {0x38000000, 0x2F830000, 0x419E0008}, PPC_B_8}, /* code+0x21f40: beq accepted */
    {0x20E48, {0x38000000, 0x2F830000, 0x419E0008}, PPC_B_8}, /* code+0x21fcc: beq accepted */
};

static const game_unlock games[] = {
    GAME("monster-fair", monster_fair),
    GAME("jinni-zeala", jinni_zeala),
    GAME("mad-daedalus", mad_daedalus),
};
