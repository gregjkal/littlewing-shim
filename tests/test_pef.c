#include "test.h"

#include <stdlib.h>

#include "pef.h"
#include "util.h"

TEST(pef_parses_the_real_executable) {
    SKIP_UNLESS_GAME();
    size_t len;
    uint8_t *buf = read_file(test_game_exe_path(), &len);
    CHECK(buf != NULL);
    pef_file pef;
    char err[256] = "";
    CHECK(pef_parse(buf, len, &pef, err, sizeof err));

    CHECK_EQ(pef.nsections, 3);
    CHECK_EQ(pef.sections[0].kind, PEF_KIND_CODE);
    CHECK_EQ(pef.sections[0].container_off, 0xCE0);
    CHECK_EQ(pef.sections[0].total_len, 280528);
    CHECK_EQ(pef.sections[1].kind, PEF_KIND_PATTERN_DATA);
    CHECK_EQ(pef.sections[1].total_len, 22892);
    CHECK_EQ(pef.sections[1].unpacked_len, 13616);
    CHECK_EQ(pef.sections[1].container_len, 8374);
    CHECK_EQ(pef.sections[2].kind, PEF_KIND_LOADER);

    CHECK_EQ(pef.main_section, 1);
    CHECK_EQ(pef.main_offset, 4832);
    CHECK_EQ(pef.init_section, -1);
    CHECK_EQ(pef.nreloc_sections, 1);

    CHECK_EQ(pef.nimports, 132);
    CHECK_STR(pef.imports[0].name, "FSClose");
    CHECK_STR(pef.imports[0].library, "CarbonLib");
    CHECK_EQ(pef.imports[0].sym_class, PEF_SYM_TVECTOR);
    CHECK(!pef.imports[0].weak);
    CHECK_STR(pef.imports[1].name, "StopAlert");
    CHECK_STR(pef.imports[36].name, "kCFPreferencesCurrentApplication");
    CHECK_EQ(pef.imports[36].sym_class, PEF_SYM_DATA);
    CHECK(pef.imports[107].weak);
    CHECK_STR(pef.imports[107].library, "CarbonLib");
    CHECK_STR(pef.imports[131].name, "EndFullScreen");
    CHECK_STR(pef.imports[131].library, "Apple;Carbon;Multimedia");
    CHECK(pef.imports[131].weak);

    int weak = 0;
    for (uint32_t i = 0; i < pef.nimports; i++)
        weak += pef.imports[i].weak;
    CHECK_EQ(weak, 25);

    pef_free(&pef);
    pef_free(&pef);
    free(buf);
}

/* Review Focus 2: wrong or corrupt executable. */
TEST(pef_rejects_non_pef_data) {
    uint8_t junk[64] = "this is not a PEF file at all";
    pef_file pef;
    char err[256] = "";
    CHECK(!pef_parse(junk, sizeof junk, &pef, err, sizeof err));
    CHECK_CONTAINS(err, "not a PowerPC PEF file");
    CHECK(!pef_parse(junk, 0, &pef, err, sizeof err));
}

TEST(pef_rejects_truncated_executable) {
    SKIP_UNLESS_GAME();
    size_t len;
    uint8_t *buf = read_file(test_game_exe_path(), &len);
    CHECK(buf != NULL);
    size_t cuts[] = {39, 60, 0x100, 0x400, 0xCE0 + 1000};
    for (size_t i = 0; i < sizeof cuts / sizeof cuts[0]; i++) {
        /* Copy so ASan catches any read past the cut. */
        uint8_t *part = malloc(cuts[i]);
        memcpy(part, buf, cuts[i]);
        pef_file pef;
        char err[256] = "";
        bool ok = pef_parse(part, cuts[i], &pef, err, sizeof err);
        free(part);
        CHECK(!ok);
        CHECK(err[0] != '\0');
    }
    free(buf);
}
