#include "test.h"

#include "guest_mem.h"
#include "ppc.h"

TEST(gm_words_are_big_endian_in_host_memory) {
    gm_init();
    gm_w32(GUEST_IMAGE_BASE, 0x11223344u);
    uint8_t *p = gm_host_base() + GUEST_IMAGE_BASE;
    CHECK_EQ(p[0], 0x11);
    CHECK_EQ(p[3], 0x44);
    CHECK_EQ(gm_r32(GUEST_IMAGE_BASE), 0x11223344u);
    CHECK_EQ(gm_r16(GUEST_IMAGE_BASE + 2), 0x3344);
    CHECK_EQ(gm_r8(GUEST_IMAGE_BASE + 1), 0x22);
    gm_w16(GUEST_HEAP_BASE, 0xBEEF);
    gm_w8(GUEST_STACK_BASE, 0x7F);
    CHECK_EQ(gm_r16(GUEST_HEAP_BASE), 0xBEEF);
    CHECK_EQ(gm_r8(GUEST_STACK_BASE), 0x7F);
}

TEST(gm_init_gives_fresh_zeroed_memory) {
    gm_init();
    gm_w32(GUEST_HEAP_BASE, 0xFFFFFFFFu);
    gm_init();
    CHECK_EQ(gm_r32(GUEST_HEAP_BASE), 0);
}

TEST(gm_backed_ranges_match_the_memory_map) {
    gm_init();
    CHECK(gm_is_backed(0, 4));
    CHECK(gm_is_backed(GUEST_IMAGE_BASE, 4));
    CHECK(gm_is_backed(GUEST_HEAP_BASE + GUEST_HEAP_SIZE - 4, 4));
    CHECK(!gm_is_backed(GUEST_HEAP_BASE + GUEST_HEAP_SIZE, 4));
    CHECK(!gm_is_backed(GUEST_STACK_BASE - 16, 4));
    CHECK(gm_is_backed(GUEST_STACK_TOP - 4, 4));
    CHECK(!gm_is_backed(GUEST_STACK_TOP - 2, 4));
    CHECK(!gm_is_backed(GUEST_TRAP_BASE, 4));
    CHECK(!gm_is_backed(0xFFFFFFFCu, 8));
}

TEST(gm_regions_are_page_aligned) {
    const gm_region *r;
    int n = gm_regions(&r);
    CHECK_EQ(n, 4);
    for (int i = 0; i < n; i++) {
        CHECK_EQ(r[i].base % 0x4000, 0);
        CHECK_EQ(r[i].size % 0x4000, 0);
    }
}

static void child_bad_access(void *arg) {
    (void)arg;
    gm_init();
    gm_r32(0x05000000u);
}

TEST(gm_unbacked_access_is_fatal) {
    char out[1024];
    int status = test_run_child(child_bad_access, NULL, out, sizeof out);
    CHECK_EQ(status, 2);
    CHECK_CONTAINS(out, "unmapped guest address 0x05000000");
}

TEST(gm_zero_length_range_must_start_inside_a_region) {
    gm_init();
    CHECK(gm_is_backed(GUEST_HEAP_BASE, 0));
    CHECK(!gm_is_backed(GUEST_LOWMEM_BASE + GUEST_LOWMEM_SIZE, 0));
    CHECK(!gm_is_backed(GUEST_STACK_TOP, 0));
    CHECK(!gm_is_backed(GUEST_TRAP_BASE, 0));
}

/* Review Focus 1: the classic games' layout doesn't change. */
TEST(gm_pef_layout_is_unchanged) {
    gm_init();
    CHECK_EQ(gm_current_layout(), GM_LAYOUT_PEF);
    const gm_region *r;
    CHECK_EQ(gm_regions(&r), 4);
    const gm_region want[] = {
        {0x00000000u, 0x00010000u, GM_PROT_R | GM_PROT_W},
        {0x00100000u, 0x00F00000u, GM_PROT_R | GM_PROT_W | GM_PROT_X},
        {0x01000000u, 0x04000000u, GM_PROT_R | GM_PROT_W},
        {0x06000000u, 0x00100000u, GM_PROT_R | GM_PROT_W},
    };
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(r[i].base, want[i].base);
        CHECK_EQ(r[i].size, want[i].size);
        CHECK_EQ(r[i].prot, want[i].prot);
    }
    CHECK_EQ(gm_heap_base(), 0x01000000u);
    CHECK_EQ(gm_heap_size(), 0x04000000u);
}

TEST(gm_macho_layout_leaves_page_zero_unmapped) {
    gm_init_layout(GM_LAYOUT_MACHO);
    CHECK_EQ(gm_current_layout(), GM_LAYOUT_MACHO);
    CHECK(!gm_is_backed(0, 4));
    CHECK(!gm_is_backed(0xFFC, 4));
    CHECK(gm_is_backed(0x1000, 4));
    CHECK(gm_is_backed(0xFFFFC, 4));
    CHECK(!gm_is_backed(0x100000, 4));
    CHECK(gm_is_backed(GUEST_STACK_TOP - 4, 4));
    CHECK(!gm_is_backed(GUEST_TRAP_BASE, 4));
    const gm_region *r;
    int n = gm_regions(&r);
    for (int i = 0; i < n; i++) {
        CHECK_EQ(r[i].base % 0x1000, 0);
        CHECK_EQ(r[i].size % 0x1000, 0);
    }
    gm_init();
    CHECK(gm_is_backed(0, 4));
}

TEST(gm_macho_heap_is_256_mb) {
    gm_init_layout(GM_LAYOUT_MACHO);
    CHECK_EQ(gm_heap_base(), 0x10000000u);
    CHECK_EQ(gm_heap_size(), 0x10000000u);
    CHECK(gm_is_backed(0x10000000u, 4));
    CHECK(gm_is_backed(0x1FFFFFFCu, 4));
    CHECK(!gm_is_backed(0x20000000u, 4));
    CHECK(!gm_is_backed(GUEST_HEAP_BASE, 4)); /* the PEF heap isn't there */
    gm_w32(0x1FFFFFFCu, 0xCAFEF00Du);
    CHECK_EQ(gm_r32(0x1FFFFFFCu), 0xCAFEF00Du);
    gm_init();
}

TEST(gm_macho_code_runs_at_its_linked_address) {
    gm_init_layout(GM_LAYOUT_MACHO);
    cpu_init();
    cpu_set_gpr(1, GUEST_STACK_TOP - 64);
    cpu_set_lr(GUEST_RETURN_MAGIC);
    uint32_t code[] = {
        0x3860002A, /* li  r3,42 */
        0x4E800020, /* blr */
    };
    put_words(0x2fdc, code, 2);
    cpu_stop s = cpu_run(0x2fdc);
    CHECK_EQ(s.kind, CPU_STOP_RETURN);
    CHECK_EQ(cpu_gpr(3), 42);

    /* A null pointer faults. */
    uint32_t load_null[] = {
        0x38800000, /* li  r4,0 */
        0x80640000, /* lwz r3,0(r4) */
        0x4E800020,
    };
    put_words(0x3000, load_null, 3);
    cpu_set_lr(GUEST_RETURN_MAGIC);
    s = cpu_run(0x3000);
    CHECK(s.kind != CPU_STOP_RETURN);
    cpu_shutdown();
    gm_init();
}
