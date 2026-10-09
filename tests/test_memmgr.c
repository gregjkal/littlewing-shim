#include "test.h"

#include "guest_mem.h"
#include "memmgr.h"

#define FULL (GUEST_HEAP_SIZE - MM_HEADER_SIZE)

static void fresh_heap(void) {
    gm_init();
    mm_init();
}

TEST(mm_new_ptr_is_aligned_inside_the_heap) {
    fresh_heap();
    uint32_t p = mm_new_ptr(10, false);
    CHECK(p >= GUEST_HEAP_BASE && p < GUEST_HEAP_BASE + GUEST_HEAP_SIZE);
    CHECK_EQ(p % 16, 0);
    CHECK(mm_is_ptr(p));
    CHECK_EQ(mm_ptr_size(p), 10);
}

TEST(mm_new_ptr_clear_zeroes_reused_memory) {
    fresh_heap();
    uint32_t p = mm_new_ptr(64, false);
    gm_w32(p, 0x12345678u);
    CHECK_EQ(mm_dispose_ptr(p), MM_NO_ERR);
    uint32_t q = mm_new_ptr(64, true);
    CHECK_EQ(q, p);
    CHECK_EQ(gm_r32(q), 0);
}

TEST(mm_freed_memory_is_filled_with_deadbeef) {
    fresh_heap();
    uint32_t p = mm_new_ptr(64, true);
    mm_new_ptr(16, false); /* keeps p's block from merging into the free tail */
    mm_dispose_ptr(p);
    CHECK_EQ(gm_r32(p), 0xDEADBEEFu);
    CHECK_EQ(gm_r32(p + 60), 0xDEADBEEFu);
}

TEST(mm_blocks_do_not_overlap) {
    fresh_heap();
    uint32_t a = mm_new_ptr(100, true), b = mm_new_ptr(100, true);
    CHECK(b >= a + 100 + MM_HEADER_SIZE || a >= b + 100 + MM_HEADER_SIZE);
}

TEST(mm_freeing_everything_restores_the_whole_heap) {
    fresh_heap();
    CHECK_EQ(mm_free_bytes(), FULL);
    uint32_t a = mm_new_ptr(100, false), b = mm_new_handle(200, false), c = mm_new_ptr(5, true);
    CHECK(mm_free_bytes() < FULL);
    CHECK_EQ(mm_dispose_ptr(a), MM_NO_ERR);
    CHECK_EQ(mm_dispose_handle(b), MM_NO_ERR);
    CHECK_EQ(mm_dispose_ptr(c), MM_NO_ERR);
    CHECK_EQ(mm_free_bytes(), FULL);
}

TEST(mm_allocates_in_the_macho_heap) {
    gm_init_layout(GM_LAYOUT_MACHO);
    mm_init();
    CHECK_EQ(mm_free_bytes(), GUEST_MACHO_HEAP_SIZE - MM_HEADER_SIZE);
    uint32_t p = mm_new_ptr(10, true);
    CHECK(p >= GUEST_MACHO_HEAP_BASE && p < GUEST_MACHO_HEAP_BASE + GUEST_MACHO_HEAP_SIZE);
    CHECK(mm_is_ptr(p));
    uint32_t h = mm_new_handle(200, false);
    CHECK(mm_is_handle(h));
    CHECK(gm_r32(h) >= GUEST_MACHO_HEAP_BASE);
    /* The game's data files total about 50 MB. */
    uint32_t big = mm_new_ptr(80u << 20, false);
    CHECK(big != 0);
    CHECK_EQ(mm_dispose_ptr(big), MM_NO_ERR);
    CHECK_EQ(mm_dispose_ptr(p), MM_NO_ERR);
    CHECK_EQ(mm_dispose_handle(h), MM_NO_ERR);
    CHECK_EQ(mm_free_bytes(), GUEST_MACHO_HEAP_SIZE - MM_HEADER_SIZE);
    fresh_heap();
}

TEST(mm_first_fit_reuses_a_freed_hole) {
    fresh_heap();
    uint32_t a = mm_new_ptr(256, false);
    mm_new_ptr(16, false);
    mm_dispose_ptr(a);
    CHECK_EQ(mm_new_ptr(100, false), a);
}

/* Review Focus 2: allocation larger than the heap. */
TEST(mm_exhausting_the_heap_returns_zero) {
    fresh_heap();
    CHECK_EQ(mm_new_ptr(GUEST_HEAP_SIZE, false), 0);
    CHECK_EQ(mm_new_ptr(0xFFFFFFF0u, false), 0);
    CHECK_EQ(mm_new_handle(GUEST_HEAP_SIZE, false), 0);
    CHECK_EQ(mm_free_bytes(), FULL);
    CHECK(mm_new_ptr(FULL, false) != 0);
    CHECK_EQ(mm_new_ptr(1, false), 0);
}

/* Review Focus 1: freeing something that isn't an allocated block. */
TEST(mm_dispose_rejects_bad_and_double_frees) {
    fresh_heap();
    uint32_t p = mm_new_ptr(32, false);
    CHECK_EQ(mm_dispose_ptr(p + 4), MM_MEM_WZ_ERR);
    CHECK_EQ(mm_dispose_ptr(GUEST_IMAGE_BASE), MM_MEM_WZ_ERR);
    CHECK_EQ(mm_dispose_ptr(p), MM_NO_ERR);
    CHECK_EQ(mm_dispose_ptr(p), MM_MEM_WZ_ERR);
    uint32_t h = mm_new_handle(8, false);
    CHECK_EQ(mm_dispose_ptr(h), MM_MEM_WZ_ERR);
    CHECK_EQ(mm_dispose_ptr(gm_r32(h)), MM_MEM_WZ_ERR);
    CHECK_EQ(mm_dispose_handle(h), MM_NO_ERR);
    CHECK_EQ(mm_dispose_handle(h), MM_MEM_WZ_ERR);
}

TEST(mm_set_ptr_size_shrinks_and_grows_in_place) {
    fresh_heap();
    uint32_t p = mm_new_ptr(64, true);
    CHECK_EQ(mm_set_ptr_size(p, 16), MM_NO_ERR);
    CHECK_EQ(mm_ptr_size(p), 16);
    CHECK_EQ(mm_set_ptr_size(p, 1000), MM_NO_ERR); /* the free tail follows p */
    CHECK_EQ(mm_ptr_size(p), 1000);
    CHECK(mm_is_ptr(p));
}

TEST(mm_set_ptr_size_fails_when_the_next_block_is_used) {
    fresh_heap();
    uint32_t p = mm_new_ptr(32, false);
    uint32_t q = mm_new_ptr(32, false);
    CHECK_EQ(mm_set_ptr_size(p, 64), MM_MEM_FULL_ERR);
    CHECK_EQ(mm_ptr_size(p), 32);
    CHECK(mm_is_ptr(q));
    CHECK_EQ(mm_set_ptr_size(q + 16, 8), MM_MEM_WZ_ERR);
}

TEST(mm_handles_point_at_their_data) {
    fresh_heap();
    uint32_t h = mm_new_handle(12, true);
    CHECK(mm_is_handle(h));
    CHECK(!mm_is_ptr(h));
    uint32_t d = gm_r32(h);
    CHECK_EQ(d % 16, 0);
    CHECK_EQ(gm_r32(d), 0);
    CHECK_EQ(mm_handle_size(h), 12);
    CHECK_EQ(mm_recover_handle(d), h);
    CHECK_EQ(mm_recover_handle(d + 16), 0);
    CHECK(!mm_is_handle(d));
}

TEST(mm_zero_size_handle_has_a_block) {
    fresh_heap();
    uint32_t h = mm_new_handle(0, false);
    CHECK(mm_is_handle(h));
    CHECK(gm_r32(h) != 0);
    CHECK_EQ(mm_handle_size(h), 0);
}

TEST(mm_handle_state_is_recorded) {
    fresh_heap();
    uint32_t h = mm_new_handle(4, false);
    CHECK_EQ(mm_handle_state(h), 0);
    mm_set_handle_state(h, MM_STATE_LOCKED | MM_STATE_RESOURCE);
    CHECK_EQ(mm_handle_state(h), MM_STATE_LOCKED | MM_STATE_RESOURCE);
    CHECK(mm_is_handle(h));
}

TEST(mm_set_handle_size_grows_in_place_or_moves) {
    fresh_heap();
    uint32_t h = mm_new_handle(5, true);
    memcpy(gm_ptr(gm_r32(h), 5), "hello", 5);
    uint32_t d = gm_r32(h);
    CHECK_EQ(mm_set_handle_size(h, 12), MM_NO_ERR); /* within the 16-byte block */
    CHECK_EQ(gm_r32(h), d);
    CHECK_EQ(mm_handle_size(h), 12);
    uint32_t blocker = mm_new_ptr(16, false); /* the next block is taken */
    CHECK_EQ(mm_set_handle_size(h, 300), MM_NO_ERR);
    CHECK(gm_r32(h) != d);
    CHECK_EQ(mm_handle_size(h), 300);
    CHECK(memcmp(gm_ptr(gm_r32(h), 5), "hello", 5) == 0);
    CHECK_EQ(mm_recover_handle(gm_r32(h)), h);
    CHECK_EQ(mm_set_handle_size(h, 2), MM_NO_ERR);
    CHECK_EQ(mm_handle_size(h), 2);
    CHECK_EQ(mm_set_handle_size(blocker, 2), MM_MEM_WZ_ERR);
    CHECK_EQ(mm_set_handle_size(h, GUEST_HEAP_SIZE), MM_MEM_FULL_ERR);
    CHECK_EQ(mm_handle_size(h), 2);
}
