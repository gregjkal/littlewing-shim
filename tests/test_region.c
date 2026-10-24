#include "test.h"

#include "harness.h"
#include "memmgr.h"
#include "qd.h"
#include "region.h"

static const char *const names[] = {
    "NewRgn", "DisposeRgn", "RectRgn", "SetRectRgn", "SetEmptyRgn", "CopyRgn", "EmptyRgn",
    "EqualRgn", "OffsetRgn", "InsetRgn", "SectRgn", "UnionRgn", "DiffRgn", "XorRgn", "PtInRgn",
    "RectInRgn", "GetRegionBounds",
};

static void setup(void) {
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    region_register();
}

static qd_rect R(int top, int left, int bottom, int right) {
    return (qd_rect){(int16_t)top, (int16_t)left, (int16_t)bottom, (int16_t)right};
}

static void rect_rgn(rgn *r, int top, int left, int bottom, int right) {
    rgn_init(r);
    rgn_set_rect(r, R(top, left, bottom, right));
}

/* Rows 0-4 columns 0-9, and rows 5-9 columns 0-19. */
static void l_shape(rgn *out) {
    rgn b;
    rect_rgn(out, 0, 0, 10, 10);
    rect_rgn(&b, 5, 10, 10, 20);
    rgn_combine(out, &b, RGN_UNION, out);
    rgn_free(&b);
}

static uint32_t new_handle_of(const rgn *r) {
    uint32_t h = mm_new_handle(RGN_HEADER, true);
    if (!rgn_write(h, r))
        fatal("rgn_write failed");
    return h;
}

/* The L packed as QuickDraw packs it: each row's x's are where it differs
   from the row above. */
TEST(region_packs_an_l_as_quickdraw_does) {
    setup();
    rgn l;
    l_shape(&l);
    uint32_t h = new_handle_of(&l);
    static const uint16_t want[] = {
        36, 0, 0, 10, 20,        /* rgnSize, rgnBBox */
        0, 0, 10, 0x7FFF,        /* row 0: [0, 10) starts */
        5, 10, 20, 0x7FFF,       /* row 5: [10, 20) joins */
        10, 0, 20, 0x7FFF,       /* row 10: all of it ends */
        0x7FFF,
    };
    CHECK_EQ(mm_handle_size(h), sizeof want);
    for (uint32_t i = 0; i < sizeof want / 2; i++)
        CHECK_EQ(gm_r16(gm_r32(h) + 2 * i), want[i]);

    rgn back;
    char err[96];
    rgn_init(&back);
    CHECK(rgn_read(h, &back, err, sizeof err));
    CHECK(rgn_equal(&back, &l));
    rgn_free(&back);
    rgn_free(&l);
}

TEST(region_rectangles_and_empty_regions_are_their_header) {
    setup();
    rgn r;
    rect_rgn(&r, 1, 2, 3, 4);
    CHECK(rgn_is_rect(&r));
    uint32_t h = new_handle_of(&r);
    CHECK_EQ(mm_handle_size(h), RGN_HEADER);
    CHECK_EQ(gm_r16(gm_r32(h)), RGN_HEADER);
    CHECK_EQ(qd_read_rect(gm_r32(h) + 2).right, 4);

    rgn_set_rect(&r, R(5, 5, 5, 9)); /* no rows */
    CHECK(rgn_is_empty(&r));
    CHECK(rgn_write(h, &r));
    CHECK_EQ(qd_read_rect(gm_r32(h) + 2).bottom, 0);
    rgn_free(&r);
}

/* A frame: the rectangle less its inside, whose row 2 has two intervals. */
TEST(region_diff_makes_a_frame) {
    rgn f, hole;
    rect_rgn(&f, 0, 0, 5, 5);
    rect_rgn(&hole, 1, 1, 4, 4);
    rgn_combine(&f, &hole, RGN_DIFF, &f);
    CHECK_EQ(f.nbands, 3);
    CHECK_EQ(f.bands[1].n, 4);
    CHECK(rgn_has_point(&f, 0, 2));
    CHECK(!rgn_has_point(&f, 2, 2));
    CHECK(rgn_has_point(&f, 4, 2));
    CHECK(!rgn_has_point(&f, 5, 2));
    CHECK(!rgn_meets_rect(&f, R(1, 1, 4, 4)));
    CHECK(rgn_meets_rect(&f, R(3, 3, 5, 5)));
    qd_rect b = rgn_bounds(&f);
    CHECK(b.top == 0 && b.left == 0 && b.bottom == 5 && b.right == 5);

    /* Putting the hole back gives the rectangle: canonical, so equal. */
    rgn whole;
    rect_rgn(&whole, 0, 0, 5, 5);
    rgn_combine(&f, &hole, RGN_UNION, &f);
    CHECK(rgn_is_rect(&f));
    CHECK(rgn_equal(&f, &whole));
    rgn_combine(&f, &hole, RGN_XOR, &f);
    CHECK_EQ(f.nbands, 3);
    rgn_combine(&f, &f, RGN_SECT, &f);
    CHECK_EQ(f.nbands, 3);
    rgn_free(&f);
    rgn_free(&hole);
    rgn_free(&whole);
}

TEST(region_inset_moves_every_edge) {
    rgn r, want;
    rect_rgn(&r, 0, 0, 10, 20);
    rgn_inset(&r, 3, 2);
    rect_rgn(&want, 2, 3, 8, 17);
    CHECK(rgn_equal(&r, &want)); /* as InsetRect */
    rgn_inset(&r, -3, -2);
    rgn_set_rect(&want, R(0, 0, 10, 20));
    CHECK(rgn_equal(&r, &want));

    /* The L, in by 1: a point stays when the 3 x 3 box around it is in. */
    rgn l;
    l_shape(&l);
    rgn_inset(&l, 1, 1);
    for (int v = -1; v <= 10; v++)
        for (int h = -1; h <= 20; h++) {
            rgn whole;
            l_shape(&whole);
            bool in = true;
            for (int dv = -1; dv <= 1; dv++)
                for (int dh = -1; dh <= 1; dh++)
                    in &= rgn_has_point(&whole, h + dh, v + dv);
            CHECK_EQ(rgn_has_point(&l, h, v), in);
            rgn_free(&whole);
        }

    /* Two rectangles 4 apart, out by 2, meet. */
    rgn two, b;
    rect_rgn(&two, 0, 0, 4, 4);
    rect_rgn(&b, 0, 8, 4, 12);
    rgn_combine(&two, &b, RGN_UNION, &two);
    rgn_inset(&two, -2, 0);
    rgn_set_rect(&want, R(0, -2, 4, 14));
    CHECK(rgn_equal(&two, &want));
    rgn_free(&r);
    rgn_free(&want);
    rgn_free(&l);
    rgn_free(&two);
    rgn_free(&b);
}

TEST(region_offset_moves_and_clamps_at_the_edge) {
    rgn r, want;
    l_shape(&r);
    rgn_offset(&r, 5, -3);
    CHECK(rgn_has_point(&r, 5, -3));
    CHECK(!rgn_has_point(&r, 4, -3));
    rect_rgn(&r, 0, 0, 10, 10);
    rgn_offset(&r, 32760, 0);
    rect_rgn(&want, 0, 32760, 10, 32766);
    CHECK(rgn_equal(&r, &want));
    rgn_offset(&r, 100, 0); /* both edges at the limit: nothing left */
    CHECK(rgn_is_empty(&r));
    rgn_free(&r);
    rgn_free(&want);
}

static void put_halfwords(uint32_t h, const uint16_t *w, uint32_t n) {
    if (mm_set_handle_size(h, 2 * n) != MM_NO_ERR)
        fatal("can't size the handle");
    for (uint32_t i = 0; i < n; i++)
        gm_w16(gm_r32(h) + 2 * i, w[i]);
}

static const char *read_error(const uint16_t *w, uint32_t n) {
    static char err[96];
    uint32_t h = mm_new_handle(0, false);
    put_halfwords(h, w, n);
    rgn r;
    rgn_init(&r);
    bool ok = rgn_read(h, &r, err, sizeof err);
    rgn_free(&r);
    mm_dispose_handle(h);
    return ok ? "" : err;
}

TEST(region_read_refuses_what_isnt_a_region) {
    setup();
    static const uint16_t odd[] = {20, 0, 0, 1, 1, 0, 0, 0x7FFF, 0x7FFF, 0x7FFF};
    CHECK_CONTAINS(read_error(odd, 10), "odd number of edges");
    static const uint16_t order[] = {28, 0, 0, 5, 5, 3, 0, 5, 0x7FFF, 1, 0, 5, 0x7FFF, 0x7FFF};
    CHECK_CONTAINS(read_error(order, 14), "rows out of order");
    static const uint16_t xs[] = {20, 0, 0, 1, 1, 0, 5, 0, 0x7FFF, 0x7FFF};
    CHECK_CONTAINS(read_error(xs, 10), "x's out of order");
    static const uint16_t open[] = {20, 0, 0, 1, 1, 0, 0, 5, 0x7FFF, 0x7FFF};
    CHECK_CONTAINS(read_error(open, 10), "doesn't close");
    static const uint16_t unended[] = {18, 0, 0, 1, 1, 0, 0, 5, 0x7FFF};
    CHECK_CONTAINS(read_error(unended, 9), "no end mark");
    static const uint16_t big[] = {40, 0, 0, 1, 1};
    CHECK_CONTAINS(read_error(big, 5), "doesn't fit");
    static const uint16_t ok[] = {28, 0, 0, 1, 5, 0, 0, 5, 0x7FFF, 1, 0, 5, 0x7FFF, 0x7FFF};
    CHECK_STR(read_error(ok, 14), "");
}

/* ---- the guest calls ---- */

static uint32_t rect_at(int top, int left, int bottom, int right) {
    uint32_t a = scratch(8);
    qd_write_rect(a, R(top, left, bottom, right));
    return a;
}

static uint32_t point(int h, int v) { return (uint32_t)(uint16_t)v << 16 | (uint16_t)h; }

TEST(region_calls_build_and_test_regions) {
    setup();
    uint32_t a = call_import("NewRgn", 0), b = call_import("NewRgn", 0);
    CHECK(mm_is_handle(a));
    CHECK_EQ(call_import("EmptyRgn", 1, a), 1);
    call_import("RectRgn", 2, a, rect_at(0, 0, 10, 10));
    call_import("SetRectRgn", 5, b, 10u, 5u, 20u, 10u); /* left, top, right, bottom */
    call_import("UnionRgn", 3, a, b, a);
    CHECK_EQ(call_import("EmptyRgn", 1, a), 0);
    CHECK_EQ(call_import("PtInRgn", 2, point(15, 7), a), 1);
    CHECK_EQ(call_import("PtInRgn", 2, point(15, 2), a), 0);
    CHECK_EQ(call_import("RectInRgn", 2, rect_at(0, 12, 4, 20), a), 0);
    CHECK_EQ(call_import("RectInRgn", 2, rect_at(0, 9, 4, 20), a), 1);
    uint32_t r = scratch(8);
    CHECK_EQ(call_import("GetRegionBounds", 2, a, r), r);
    qd_rect bb = qd_read_rect(r);
    CHECK(bb.top == 0 && bb.left == 0 && bb.bottom == 10 && bb.right == 20);

    uint32_t c = call_import("NewRgn", 0);
    call_import("CopyRgn", 2, a, c);
    CHECK_EQ(call_import("EqualRgn", 2, a, c), 1);
    call_import("OffsetRgn", 3, c, (uint32_t)-5, 2u);
    CHECK_EQ(call_import("PtInRgn", 2, point(10, 9), c), 1);
    CHECK_EQ(call_import("EqualRgn", 2, a, c), 0);
    call_import("SectRgn", 3, a, b, c);
    CHECK_EQ(call_import("EqualRgn", 2, b, c), 1);
    call_import("DiffRgn", 3, a, b, c);
    CHECK_EQ(call_import("PtInRgn", 2, point(15, 7), c), 0);
    call_import("XorRgn", 3, c, b, c);
    CHECK_EQ(call_import("EqualRgn", 2, a, c), 1);
    call_import("InsetRgn", 3, c, 1u, 1u);
    CHECK_EQ(call_import("PtInRgn", 2, point(0, 0), c), 0);
    CHECK_EQ(call_import("PtInRgn", 2, point(1, 1), c), 1);
    call_import("SetEmptyRgn", 1, c);
    CHECK_EQ(call_import("EmptyRgn", 1, c), 1);
    call_import("DisposeRgn", 1, c);
    CHECK(!mm_is_handle(c));
}

static void child_not_a_region(void *unused) {
    (void)unused;
    setup();
    call_import("EmptyRgn", 1, 0x1234u);
}

TEST(region_calls_crash_on_what_isnt_a_region) {
    char out[16384];
    CHECK_EQ(test_run_child(child_not_a_region, NULL, out, sizeof out), 2);
    CHECK_CONTAINS(out, "EmptyRgn: 0x00001234 is not a region (not a handle)");
}
