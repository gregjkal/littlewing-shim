#include "test.h"

#include "guest_mem.h"

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
