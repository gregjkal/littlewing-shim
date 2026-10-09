#include "test.h"

#include <stdlib.h>

#include "guest_mem.h"
#include "loader.h"
#include "macho_build.h"
#include "ppc.h"
#include "trap.h"
#include "util.h"

TEST(loader_loads_the_real_executable) {
    SKIP_UNLESS_GAME();
    size_t len;
    uint8_t *buf = read_file(test_game_exe_path(), &len);
    CHECK(buf != NULL);
    gm_init();
    loaded_image img;
    char err[256] = "";
    CHECK(image_load(buf, len, &img, err, sizeof err));

    CHECK_EQ(img.code_base, 0x00100000u);
    CHECK_EQ(img.code_len, 280528);
    CHECK_EQ(gm_r32(img.code_base), 0x7C0802A6u);
    CHECK_EQ(fnv1a32(gm_ptr(img.code_base, img.code_len), img.code_len), 0xA50D49B8u);

    CHECK_EQ(img.data_base, 0x00145000u);
    CHECK_EQ(img.data_len, 22892);
    CHECK_EQ(fnv1a32(gm_ptr(img.data_base, img.data_len), img.data_len), 0xA7C47401u);

    CHECK_EQ(img.import_area, 0x0014A970u);
    CHECK_EQ(img.import_addr[0], 0x0014A970u);
    CHECK_EQ(gm_r32(img.import_addr[0]), GUEST_TRAP_ADDR(0));
    CHECK_EQ(gm_r32(img.import_addr[0] + 4), 0);
    CHECK_EQ(gm_r32(img.import_addr[131]), GUEST_TRAP_ADDR(131));
    CHECK_EQ(img.import_addr[36], 0x0014A970u + 36 * 8);
    CHECK_EQ(gm_r32(img.import_addr[36]), 0);

    CHECK_EQ(img.main_tvector, 0x001462E0u);
    CHECK_EQ(gm_r32(img.main_tvector), 0x001387E0u);
    CHECK_EQ(gm_r32(img.main_tvector + 4), 0x00145000u);
    CHECK_EQ(img.init_tvector, 0);
    CHECK_EQ(image_find_import(&img, "kCFPreferencesCurrentApplication"), 36);
    CHECK_EQ(image_find_import(&img, "EndFullScreen"), 131);
    CHECK_EQ(image_find_import(&img, "NoSuchCall"), -1);

    image_free(&img);
    free(buf);
}

TEST(loader_reports_parse_errors) {
    uint8_t junk[64] = {0};
    gm_init();
    loaded_image img;
    char err[256] = "";
    CHECK(!image_load(junk, sizeof junk, &img, err, sizeof err));
    CHECK_CONTAINS(err, "not a PowerPC PEF file");
}

/* ---- Mach-O ---- */

#define KCF_OBJECT 0x80000u /* where the test resolver says the data symbol lives */

static uint32_t test_resolver(const char *name) {
    if (strcmp(name, "kCFPreferencesCurrentApplication") == 0)
        return KCF_OBJECT;
    if (strcmp(name, "CFRelease") == 0)
        return IMAGE_SYMBOL_CODE;
    return 0;
}

static bool load_built(const mb_opts *o, loaded_image *img, char *err, size_t errlen,
                       uint8_t **buf) {
    size_t len;
    *buf = mb_build(o, &len);
    gm_init_layout(GM_LAYOUT_MACHO);
    image_set_data_resolver(test_resolver);
    return image_load_macho(*buf, len, img, err, errlen);
}

TEST(macho_load_places_segments_at_their_addresses) {
    loaded_image img;
    char err[256] = "";
    uint8_t *buf;
    CHECK(load_built(&(mb_opts){0}, &img, err, sizeof err, &buf));
    CHECK_EQ(img.kind, IMAGE_MACHO);
    CHECK_EQ(gm_r32(0x1000), 0xFEEDFACE); /* __TEXT starts with the header */
    CHECK_EQ(gm_r32(MB_MAIN), 0x3860002A);
    CHECK_EQ(gm_r32(MB_MODINIT), MB_INIT);
    CHECK(!gm_is_backed(0, 4));
    CHECK_EQ(img.code_base, 0);
    CHECK_EQ(img.code_len, 0x2000); /* the end of __TEXT */
    image_free(&img);
    free(buf);
}

TEST(macho_load_binds_lazy_pointers_to_traps) {
    loaded_image img;
    char err[256] = "";
    uint8_t *buf;
    CHECK(load_built(&(mb_opts){0}, &img, err, sizeof err, &buf));
    int32_t malloc_i = image_find_import(&img, "malloc"), exit_i = image_find_import(&img, "exit");
    CHECK(malloc_i >= 0 && exit_i >= 0);
    CHECK_EQ(gm_r32(MB_LA), GUEST_TRAP_ADDR(malloc_i));
    CHECK_EQ(gm_r32(MB_LA + 4), GUEST_TRAP_ADDR(exit_i));
    /* The imports in symbol order, then the synthetic ones. */
    CHECK_EQ(img.nnames, 6);
    CHECK_STR(img.names[0], "CFRelease");
    CHECK_STR(img.names[3], "malloc");
    CHECK_STR(img.names[4], "sprintf");
    CHECK_STR(img.names[5], "dyld_stub_binding_helper");
    CHECK_EQ(gm_r32(MB_DYLD), GUEST_TRAP_ADDR(5));
    CHECK_EQ(gm_r32(MB_DYLD + 4), GUEST_TRAP_ADDR(5));
    image_free(&img);
    free(buf);
}

TEST(macho_load_binds_data_imports_through_the_resolver) {
    loaded_image img;
    char err[256] = "";
    uint8_t *buf;
    CHECK(load_built(&(mb_opts){0}, &img, err, sizeof err, &buf));
    CHECK_EQ(gm_r32(MB_NL), KCF_OBJECT);
    image_free(&img);
    free(buf);
}

TEST(macho_load_leaves_local_indirect_slots_alone) {
    loaded_image img;
    char err[256] = "";
    uint8_t *buf;
    CHECK(load_built(&(mb_opts){0}, &img, err, sizeof err, &buf));
    CHECK_EQ(gm_r32(MB_NL + 4), 0x1234);
    image_free(&img);
    free(buf);
}

TEST(macho_load_adds_at_external_relocations) {
    loaded_image img;
    char err[256] = "";
    uint8_t *buf;
    CHECK(load_built(&(mb_opts){0}, &img, err, sizeof err, &buf));
    CHECK_EQ(gm_r32(MB_DATA), KCF_OBJECT + 8);
    CHECK_EQ(gm_r32(MB_DATA + 4), GUEST_TRAP_ADDR(image_find_import(&img, "CFRelease")));
    image_free(&img);
    free(buf);
}

static uint32_t forgetful_resolver(const char *name) {
    return strcmp(name, "CFRelease") == 0 ? IMAGE_SYMBOL_CODE : 0;
}

TEST(macho_load_fails_naming_an_unknown_data_symbol) {
    size_t len;
    uint8_t *buf = mb_build(&(mb_opts){0}, &len);
    gm_init_layout(GM_LAYOUT_MACHO);
    image_set_data_resolver(forgetful_resolver);
    loaded_image img;
    char err[256] = "";
    CHECK(!image_load_macho(buf, len, &img, err, sizeof err));
    CHECK_CONTAINS(err, "the shim doesn't know the symbol kCFPreferencesCurrentApplication");
    CHECK(img.names == NULL);
    free(buf);
}

TEST(macho_load_needs_the_macho_layout) {
    size_t len;
    uint8_t *buf = mb_build(&(mb_opts){0}, &len);
    gm_init();
    loaded_image img;
    char err[256] = "";
    CHECK(!image_load_macho(buf, len, &img, err, sizeof err));
    CHECK_CONTAINS(err, "needs the Mach-O memory layout");
    free(buf);
}

TEST(macho_load_finds_main_after_start) {
    loaded_image img;
    char err[256] = "";
    uint8_t *buf;
    CHECK(load_built(&(mb_opts){0}, &img, err, sizeof err, &buf));
    CHECK_EQ(img.main_addr, MB_MAIN);
    image_free(&img);
    free(buf);

    CHECK(!load_built(&(mb_opts){.no_exit_call = true}, &img, err, sizeof err, &buf));
    CHECK_CONTAINS(err, "can't find main");
    free(buf);
}

TEST(macho_load_lists_static_initializers) {
    loaded_image img;
    char err[256] = "";
    uint8_t *buf;
    CHECK(load_built(&(mb_opts){0}, &img, err, sizeof err, &buf));
    CHECK_EQ(img.ninit, 1);
    CHECK_EQ(img.init_addrs[0], MB_INIT);
    image_free(&img);
    free(buf);
}

static void h_malloc(void) {
    trap_return(trap_arg(0) + 0x10000000u);
}

/* A call through a stub and its lazy pointer reaches the import's handler
   and returns to the caller. */
TEST(macho_load_stub_calls_reach_the_handler) {
    loaded_image img;
    char err[256] = "";
    uint8_t *buf;
    CHECK(load_built(&(mb_opts){0}, &img, err, sizeof err, &buf));
    cpu_init();
    trap_init(img.nnames, img.names, img.code_base, img.code_len);
    trap_set_direct_calls(true);
    trap_register("malloc", h_malloc);
    uint32_t caller[] = {
        0x7C0802A6,                  /* mflr  r0 */
        0x90010008,                  /* stw   r0,8(r1) */
        0x9421FFC0,                  /* stwu  r1,-64(r1) */
        mb_bl(0x186C, MB_STUBS),     /* bl    malloc stub */
        0x38630001,                  /* addi  r3,r3,1 */
        0x38210040,                  /* addi  r1,r1,64 */
        0x80010008,                  /* lwz   r0,8(r1) */
        0x7C0803A6,                  /* mtlr  r0 */
        0x4E800020,                  /* blr */
    };
    put_words(0x1860, caller, 9);
    uint32_t arg = 0x40;
    CHECK_EQ(guest_call(0x1860, 1, &arg), 0x10000041u);
    CHECK_EQ(guest_call(MB_MAIN, 0, NULL), 42);
    trap_shutdown();
    image_free(&img);
    free(buf);
}

/* What MONSTER FAIR's non-lazy pointers and external relocations name. */
static uint32_t mf_resolver(const char *name) {
    static const char *const data[] = {
        "mach_init_routine",
        "errno",
        "_cthread_init_routine",
        "__keymgr_global",
        "kCFPreferencesCurrentApplication",
        "_DefaultRuneLocale",
        "__CFConstantStringClassReference",
        "_ZTVN10__cxxabiv117__class_type_infoE",
        "_ZTVN10__cxxabiv120__si_class_type_infoE",
        "_ZTVN10__cxxabiv121__vmi_class_type_infoE",
    };
    for (uint32_t i = 0; i < sizeof data / sizeof data[0]; i++)
        if (strcmp(name, data[i]) == 0)
            return 0x80000u + 0x100u * i;
    if (strcmp(name, "__gxx_personality_v0") == 0 || strcmp(name, "__cxa_pure_virtual") == 0)
        return IMAGE_SYMBOL_CODE;
    fprintf(stderr, "  mf_resolver: unknown %s\n", name);
    return 0;
}

TEST(macho_load_monster_fair) {
    SKIP_UNLESS_MF();
    size_t len;
    uint8_t *buf = read_file(test_mf_exe_path(), &len);
    CHECK(buf != NULL);
    gm_init_layout(GM_LAYOUT_MACHO);
    image_set_data_resolver(mf_resolver);
    loaded_image img;
    char err[256] = "";
    CHECK(image_load_macho(buf, len, &img, err, sizeof err));
    CHECK_EQ(img.main_addr, 0x41ca8);
    CHECK_EQ(img.ninit, 13);
    CHECK_EQ(img.nnames, 205 + 2);
    uint32_t slot = gm_r32(0x5b144);
    CHECK(slot >= GUEST_TRAP_BASE && slot < GUEST_TRAP_ADDR(205));
    /* The first constant CFString's isa is the class reference (addend 0). */
    CHECK_EQ(gm_r32(0x5d2c8), 0x80000u + 0x100u * 6);
    CHECK_EQ(gm_r32(0x5d2c8 + 8), 0x45748);
    char name[8];
    CHECK(gm_read_cstr(0x45748, name, sizeof name));
    CHECK_STR(name, "main");
    /* Every lazy pointer holds a trap address. */
    for (uint32_t a = 0x5b144; a < 0x5b144 + 4 * 193; a += 4) {
        uint32_t v = gm_r32(a);
        CHECK(v >= GUEST_TRAP_BASE && v < GUEST_TRAP_ADDR(img.nnames));
    }
    image_free(&img);
    free(buf);
}
