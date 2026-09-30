#include "test.h"

#include "pef.h"
#include "util.h"

#define C 0x1000u
#define D 0x2000u

static const uint32_t imports[] = {0xA000, 0xB000, 0xC000};

typedef struct {
    uint8_t mem[32]; /* 8 big-endian words, each starting as 0x10 */
    pef_reloc_target t;
} fixture;

static void setup(fixture *f, uint32_t len) {
    for (int i = 0; i < 8; i++)
        wr_be32(f->mem + 4 * i, 0x10);
    f->t = (pef_reloc_target){f->mem, len, C, D, imports, 3};
}

static uint32_t word(const fixture *f, int i) { return rd_be32(f->mem + 4 * i); }

static bool run(fixture *f, const uint16_t *ops, uint32_t n, char *err) {
    uint8_t bytes[64];
    for (uint32_t i = 0; i < n; i++)
        wr_be16(bytes + 2 * i, ops[i]);
    return pef_reloc_run(bytes, n, &f->t, err, 256);
}

TEST(reloc_by_sect_d_with_skip) {
    fixture f;
    setup(&f, 32);
    uint16_t ops[] = {(1 << 6) | 2};
    char err[256];
    CHECK(run(&f, ops, 1, err));
    CHECK_EQ(word(&f, 0), 0x10);
    CHECK_EQ(word(&f, 1), D + 0x10);
    CHECK_EQ(word(&f, 2), D + 0x10);
    CHECK_EQ(word(&f, 3), 0x10);
}

TEST(reloc_runs_by_section) {
    fixture f;
    setup(&f, 32);
    uint16_t ops[] = {0x4001 /* BySectC x2 */, 0x4200 /* BySectD x1 */};
    char err[256];
    CHECK(run(&f, ops, 2, err));
    CHECK_EQ(word(&f, 0), C + 0x10);
    CHECK_EQ(word(&f, 1), C + 0x10);
    CHECK_EQ(word(&f, 2), D + 0x10);
}

TEST(reloc_tvector8) {
    fixture f;
    setup(&f, 32);
    uint16_t ops[] = {0x4601 /* TVector8 x2 */};
    char err[256];
    CHECK(run(&f, ops, 1, err));
    CHECK_EQ(word(&f, 0), C + 0x10);
    CHECK_EQ(word(&f, 1), D + 0x10);
    CHECK_EQ(word(&f, 2), C + 0x10);
    CHECK_EQ(word(&f, 3), D + 0x10);
}

TEST(reloc_import_run_advances_import_index) {
    fixture f;
    setup(&f, 32);
    uint16_t ops[] = {0x4A00 /* ImportRun x1 */, 0x4A00 /* ImportRun x1 */};
    char err[256];
    CHECK(run(&f, ops, 2, err));
    CHECK_EQ(word(&f, 0), 0xA000 + 0x10);
    CHECK_EQ(word(&f, 1), 0xB000 + 0x10);
}

TEST(reloc_small_by_import_sets_import_index) {
    fixture f;
    setup(&f, 32);
    uint16_t ops[] = {0x6001 /* SmByImport 1 */, 0x4A00 /* ImportRun x1 */};
    char err[256];
    CHECK(run(&f, ops, 2, err));
    CHECK_EQ(word(&f, 0), 0xB000 + 0x10);
    CHECK_EQ(word(&f, 1), 0xC000 + 0x10);
}

TEST(reloc_incr_position) {
    fixture f;
    setup(&f, 32);
    uint16_t ops[] = {0x8007 /* IncrPosition 8 */, 0x4200 /* BySectD x1 */};
    char err[256];
    CHECK(run(&f, ops, 2, err));
    CHECK_EQ(word(&f, 1), 0x10);
    CHECK_EQ(word(&f, 2), D + 0x10);
}

TEST(reloc_rejects_unsupported_opcode) {
    fixture f;
    setup(&f, 32);
    uint16_t ops[] = {0x9000 /* RelocSmRepeat */};
    char err[256];
    CHECK(!run(&f, ops, 1, err));
    CHECK_CONTAINS(err, "unsupported relocation opcode 0x9000");
}

TEST(reloc_rejects_write_past_section_end) {
    fixture f;
    setup(&f, 8);
    uint16_t ops[] = {0x4002 /* BySectC x3 over a 2-word section */};
    char err[256];
    CHECK(!run(&f, ops, 1, err));
    CHECK_CONTAINS(err, "past the end");
}

TEST(reloc_rejects_import_out_of_range) {
    fixture f;
    setup(&f, 32);
    uint16_t ops[] = {0x6005 /* SmByImport 5 */};
    char err[256];
    CHECK(!run(&f, ops, 1, err));
    CHECK_CONTAINS(err, "import index 5");
}
