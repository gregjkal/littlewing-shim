/* Each game's checks all still run and its length test is kept; only the
   verdicts are dropped. Most games validate in MONSTER FAIR's shape: refuse a
   key of 23 characters or fewer, run three checks, then compare the key's
   third character with a computed one. The three branches past a refusal
   become unconditional and the compare's result becomes `li r3,0`. Addresses
   are guest addresses: a Mach-O game's equal its code+ offsets, a PEF game's
   are the offset plus GUEST_IMAGE_BASE (0x100000). docs/development.md has each
   game's path to the verdict. */

/* MONSTER FAIR 1.2.5 (the rehearsal; its public key works anyway). A key of
   24 or more letters and digits. The check during play (code+0x42be0) has its
   verdict dropped in src/patch.c. */
static const code_patch monster_fair[] = {
    {0x42B78, {0x4BFFFB9D, 0x2F830000, 0x409E0048}, PPC_NOP},     /* bl 0x42714; bne refused */
    {0x42B8C, {0x4BFFFEE1, 0x2F830000, 0x409E0034}, PPC_NOP},     /* bl 0x42a6c; bne refused */
    {0x42BA0, {0x4BFFFAF1, 0x2F830000, 0x409E0020}, PPC_NOP},     /* bl 0x42690; bne refused */
    {0x42670, {0x7C000774, 0x3040FFFF, 0x7C620110}, PPC_LI_R3_0}, /* r3 = character differs */
};

/* Jinni Zeala 1.2.2 (PEF, no key known). Register keeps only digits, so a key
   of 24 or more digits. */
static const code_patch jinni_zeala[] = {
    {0x1077F4, {0x4BFFFACD, 0x5460063F, 0x4182000C}, PPC_B_12},    /* bl 0x72c0; beq accepted */
    {0x107810, {0x4BFFFE81, 0x5460063F, 0x4182000C}, PPC_B_12},    /* bl 0x7690; beq accepted */
    {0x10782C, {0x4BFFFD45, 0x5460063F, 0x4182000C}, PPC_B_12},    /* bl 0x7570; beq accepted */
    {0x107668, {0x7C002050, 0x7C600378, 0x54030FFE}, PPC_LI_R3_0}, /* r3 = character differs */
};

/* Mad Daedalus 1.1.9 (no key known). Not MONSTER FAIR's shape: four checks run
   while loading tables, each needing a 24-character key. Register keeps
   letters and digits and cuts the key to 24, so a key of 24 or more digits
   (letters untested). The license object's methods are left alone, since
   two self-tests during play feed them a known bad key and show
   BadApplication if it is accepted. */
static const code_patch mad_daedalus[] = {
    {0x20918, {0x6000980F, 0x7F830040, 0x40BD0034}, PPC_NOP}, /* digits <= 169999: refused */
    {0x20948, {0x7F830040, 0x38000000, 0x409D0008}, PPC_B_8}, /* digits <= 999999: accepted */
    {0x20AF4, {0x38000000, 0x2F830000, 0x419E0008}, PPC_B_8}, /* code+0x21ee0: beq accepted */
    {0x20CA0, {0x38000000, 0x2F830000, 0x419E0008}, PPC_B_8}, /* code+0x21f40: beq accepted */
    {0x20E48, {0x38000000, 0x2F830000, 0x419E0008}, PPC_B_8}, /* code+0x21fcc: beq accepted */
};

/* Angel Egg 3.0.1 (no key known). Register keeps only digits, so a key of 24
   or more digits. */
static const code_patch angel_egg[] = {
    {0x1A7D4, {0x48000079, 0x5460063F, 0x4182000C}, PPC_B_12},    /* bl 0x1a84c; beq accepted */
    {0x1A7F0, {0x48000359, 0x5460063F, 0x4182000C}, PPC_B_12},    /* bl 0x1ab48; beq accepted */
    {0x1A80C, {0x48000531, 0x5460063F, 0x4182000C}, PPC_B_12},    /* bl 0x1ad3c; beq accepted */
    {0x1AE4C, {0x7C001850, 0x7C400378, 0x54030FFE}, PPC_LI_R3_0}, /* r3 = character differs */
};

/* Golden Logres 3.0.4 (no key known). Register keeps only digits, so a key of
   24 or more digits. */
static const code_patch golden_logres[] = {
    {0x2F16C, {0x48008BF1, 0x5460063F, 0x4182000C}, PPC_B_12},    /* bl 0x37d5c; beq accepted */
    {0x2F188, {0x4BFFFE8D, 0x5460063F, 0x4182000C}, PPC_B_12},    /* bl 0x2f014; beq accepted */
    {0x2F1A4, {0x4BFFFD39, 0x5460063F, 0x4182000C}, PPC_B_12},    /* bl 0x2eedc; beq accepted */
    {0x2EFEC, {0x7C001850, 0x7C400378, 0x54030FFE}, PPC_LI_R3_0}, /* r3 = character differs */
};

/* FAIRY TOWER 1.1.7 (no key known). Mad Daedalus's shape with a fifth check
   (code+0x1459c, method 0x1c), and the range test's upper bound is 269999. A
   key of 24 or more digits. The license object's methods are left alone, as
   in Mad Daedalus. */
static const code_patch fairy_tower[] = {
    {0x15784, {0x6000980F, 0x7F830040, 0x40BD0034}, PPC_NOP}, /* digits <= 169999: refused */
    {0x157B4, {0x7F830040, 0x38000000, 0x409D0008}, PPC_B_8}, /* digits <= 269999: accepted */
    {0x14710, {0x38000000, 0x2F830000, 0x419E0008}, PPC_B_8}, /* method 0x1c: beq accepted */
    {0x1596C, {0x38000000, 0x2F830000, 0x419E0008}, PPC_B_8}, /* method 0x20: beq accepted */
    {0x15B24, {0x38000000, 0x2F830000, 0x419E0008}, PPC_B_8}, /* method 0x28: beq accepted */
    {0x15CDC, {0x38000000, 0x2F830000, 0x419E0008}, PPC_B_8}, /* method 0x2c: beq accepted */
};

/* Tristan 2.5.1 (PEF, no key known). MONSTER FAIR's shape with a fourth
   check in place of the character compare. Register keeps only digits, so a
   key of 24 or more digits. */
static const code_patch tristan[] = {
    {0x10D24C, {0x4BFFFA55, 0x5460063F, 0x4182000C}, PPC_B_12}, /* bl 0xcca0; beq accepted */
    {0x10D268, {0x4BFFFE89, 0x5460063F, 0x4182000C}, PPC_B_12}, /* bl 0xd0f0; beq accepted */
    {0x10D284, {0x4BFFFD55, 0x5460063F, 0x4182000C}, PPC_B_12}, /* bl 0xcfd8; beq accepted */
    {0x10D2A0, {0x4BFFFDD5, 0x5460063F, 0x4182000C}, PPC_B_12}, /* bl 0xd074; beq accepted */
};

/* Loony Labyrinth 3.0.1 (PEF). Jinni Zeala's shape at code+0xe800, called
   from VerifyLicense at launch and twice from RegisterLicense. Register keeps
   only digits, so a key of 24 or more digits. */
static const code_patch loony_labyrinth[] = {
    {0x10E844, {0x4BFFF99D, 0x5460063F, 0x4182000C}, PPC_B_12},    /* bl 0xe1e0; beq accepted */
    {0x10E860, {0x4BFFFE41, 0x5460063F, 0x4182000C}, PPC_B_12},    /* bl 0xe6a0; beq accepted */
    {0x10E87C, {0x4BFFFC65, 0x5460063F, 0x4182000C}, PPC_B_12},    /* bl 0xe4e0; beq accepted */
    {0x10E628, {0x7C002050, 0x7C600378, 0x54030FFE}, PPC_LI_R3_0}, /* r3 = character differs */
};

/* Crystal Caliburn 3.0.1 (PEF). Loony Labyrinth's code at code+0xa540.
   Register keeps only digits, so a key of 24 or more digits. */
static const code_patch crystal_caliburn[] = {
    {0x10A584, {0x4BFFFACD, 0x5460063F, 0x4182000C}, PPC_B_12},    /* bl 0xa050; beq accepted */
    {0x10A5A0, {0x4BFFFE81, 0x5460063F, 0x4182000C}, PPC_B_12},    /* bl 0xa420; beq accepted */
    {0x10A5BC, {0x4BFFFD45, 0x5460063F, 0x4182000C}, PPC_B_12},    /* bl 0xa300; beq accepted */
    {0x10A3F8, {0x7C002050, 0x7C600378, 0x54030FFE}, PPC_LI_R3_0}, /* r3 = character differs */
};

/* Psychedelic"O" 1.2.3 (PEF, the 2006 bonus disc, no shareware demo). Not
   MONSTER FAIR's shape: at launch code+0x1ad10 checks "user id" as a disc
   serial, its first 12 letters and digits split into two six-digit numbers
   (characters 0, 1, 6, 7, 9, 11 and the rest), and asks for one (DLOG 129)
   until it passes; Cancel quits. A serial whose first number isn't 0
   passes, so a key of 12 or more digits. */
static const code_patch psychedelic_o[] = {
    {0x11ACE4, {0x7C001A78, 0x7C800379, 0x40820008}, PPC_NOP}, /* second number differs: refused */
};

static const game_unlock games[] = {
    GAME("loony-labyrinth", loony_labyrinth),
    GAME("crystal-caliburn", crystal_caliburn),
    GAME("monster-fair", monster_fair),
    GAME("jinni-zeala", jinni_zeala),
    GAME("mad-daedalus", mad_daedalus),
    GAME("angel-egg", angel_egg),
    GAME("golden-logres", golden_logres),
    GAME("fairy-tower", fairy_tower),
    GAME("tristan", tristan),
    GAME("psychedelic-o", psychedelic_o),
};
