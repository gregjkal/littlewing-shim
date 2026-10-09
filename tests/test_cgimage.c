#include "test.h"

#include <stdlib.h>
#include <unistd.h>

#include "cf.h"
#include "cgimage.h"
#include "harness.h"
#include "memmgr.h"
#include "png.h"

static const char *const names[] = {
    "CGDataProviderCreateWithURL", "CGDataProviderRelease", "CGImageCreateWithPNGDataProvider",
    "CGImageRelease",
};

/* A 2x2 PNG: red, green / blue, transparent. */
static void write_test_png(char *path, size_t cap) {
    const char *t = getenv("TMPDIR");
    snprintf(path, cap, "%s/loony-png-XXXXXX", t && *t ? t : "/tmp");
    int fd = mkstemp(path);
    close(fd);
    const uint8_t rgba[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 9, 9, 9, 0};
    png_write_rgba(path, rgba, 2, 2);
}

TEST(cgimage_decodes_a_png) {
    char path[1024];
    write_test_png(path, sizeof path);
    cgimage_pixels px;
    char err[256] = "";
    bool ok = cgimage_decode_png(path, &px, err, sizeof err);
    unlink(path);
    CHECK(ok);
    CHECK_EQ(px.width, 2);
    CHECK_EQ(px.height, 2);
    CHECK_EQ(rd_be32(px.xrgb), 0x00FF0000u);
    CHECK_EQ(rd_be32(px.xrgb + 4), 0x0000FF00u);
    CHECK_EQ(rd_be32(px.xrgb + 8), 0x000000FFu);
    CHECK_EQ(rd_be32(px.xrgb + 12), 0x00FFFFFFu); /* transparent: white */
    free(px.xrgb);
    CHECK(!cgimage_decode_png("/nonexistent.png", &px, err, sizeof err));
    CHECK_CONTAINS(err, "can't decode /nonexistent.png");
}

TEST(cgimage_guest_calls_make_an_image_from_a_url) {
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    cf_init();
    cgimage_init();
    cgimage_register();
    char path[1024];
    write_test_png(path, sizeof path);
    uint32_t url = cf_url(path);
    uint32_t prov = call_import("CGDataProviderCreateWithURL", 1, url);
    uint32_t img = call_import("CGImageCreateWithPNGDataProvider", 4, prov, 0u, 1u, 0u);
    unlink(path);
    CHECK(prov >= CGIMAGE_TAG_BASE);
    const cgimage_pixels *px = cgimage_get(img);
    CHECK(px != NULL);
    CHECK_EQ(px->width, 2);
    CHECK(cgimage_get(prov) == NULL);
    call_import("CGDataProviderRelease", 1, prov);
    cgimage_retain(img); /* as an image view would */
    call_import("CGImageRelease", 1, img);
    CHECK(cgimage_get(img) != NULL);
    cgimage_release(img);
    CHECK(cgimage_get(img) == NULL);
    call_import("CGImageRelease", 1, 0u);
}

TEST(cgimage_reads_monster_fairs_icon) {
    SKIP_UNLESS_MF();
    char path[1200];
    snprintf(path, sizeof path, "%s/Contents/Resources/appl.png", test_mf_app());
    cgimage_pixels px;
    char err[256] = "";
    CHECK(cgimage_decode_png(path, &px, err, sizeof err));
    CHECK_EQ(px.width, 128);
    CHECK_EQ(px.height, 128);
    free(px.xrgb);
}
