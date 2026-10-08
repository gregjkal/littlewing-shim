#include "test.h"

#include <stdlib.h>

#include "macho.h"
#include "macho_build.h"
#include "util.h"

static bool parse_built(const mb_opts *o, macho_file *m, char *err, size_t errlen,
                        uint8_t **buf_out) {
    size_t len;
    *buf_out = mb_build(o, &len);
    return macho_parse(*buf_out, len, m, err, errlen);
}

TEST(macho_parses_a_fat_file_with_a_ppc_slice) {
    mb_opts o = {0};
    macho_file m;
    char err[256] = "";
    uint8_t *buf;
    CHECK(parse_built(&o, &m, err, sizeof err, &buf));
    CHECK(m.data == buf + MB_FAT_PPC_OFF);
    CHECK_EQ(m.len, MB_SLICE_LEN);
    CHECK_EQ(m.entry, MB_ENTRY);

    CHECK_EQ(m.nsegments, 4);
    CHECK_STR(m.segments[0].name, "__PAGEZERO");
    CHECK_STR(m.segments[1].name, "__TEXT");
    CHECK_EQ(m.segments[1].vmaddr, 0x1000);
    CHECK_EQ(m.segments[1].initprot, MACHO_VM_PROT_READ | MACHO_VM_PROT_EXECUTE);
    CHECK_STR(m.segments[2].name, "__DATA");
    CHECK_EQ(m.segments[2].fileoff, 0x1000);
    CHECK_EQ(m.segments[2].filesize, 0x1000);

    CHECK_EQ(m.nsections, 7);
    const macho_section *stubs = macho_find_section(&m, "__TEXT", "__symbol_stub1");
    CHECK(stubs != NULL);
    CHECK_EQ(stubs->addr, MB_STUBS);
    CHECK_EQ(stubs->flags & 0xFF, MACHO_S_SYMBOL_STUBS);
    CHECK_EQ(stubs->reserved2, 16);
    const macho_section *la = macho_find_section(&m, "__DATA", "__la_symbol_ptr");
    CHECK(la != NULL);
    CHECK_EQ(la->addr, MB_LA);
    CHECK_EQ(la->reserved1, 4);
    CHECK(macho_find_section(&m, "__DATA", "__nope") == NULL);

    CHECK_EQ(m.ndylibs, 2);
    CHECK_STR(m.dylibs[0], "/usr/lib/libSystem.B.dylib");
    CHECK_STR(m.dylibs[1], "/System/Library/Frameworks/Carbon.framework/Versions/A/Carbon");

    CHECK_EQ(m.nsyms, 5);
    CHECK_EQ(m.nindirect, 6);
    CHECK_EQ(m.indirect[3], MACHO_INDIRECT_SYMBOL_LOCAL);
    CHECK(!macho_symbol_is_import(&m.syms[0]));
    CHECK(macho_symbol_is_import(&m.syms[1]));
    CHECK(macho_symbol_is_import(&m.syms[4])); /* prebound undefined */
    CHECK_EQ(m.syms[1].desc >> 8, 2);
    macho_free(&m);
    macho_free(&m);
    free(buf);
}

TEST(macho_parses_a_thin_ppc_file) {
    mb_opts o = {.thin = true};
    macho_file m;
    char err[256] = "";
    uint8_t *buf;
    CHECK(parse_built(&o, &m, err, sizeof err, &buf));
    CHECK(m.data == buf);
    CHECK_EQ(m.entry, MB_ENTRY);
    CHECK_EQ(m.nsections, 7);
    macho_free(&m);
    free(buf);
}

TEST(macho_refuses_a_file_without_a_ppc_slice) {
    mb_opts o = {.no_ppc = true};
    macho_file m;
    char err[256] = "";
    uint8_t *buf;
    CHECK(!parse_built(&o, &m, err, sizeof err, &buf));
    CHECK_CONTAINS(err, "no PowerPC code");
    CHECK(m.syms == NULL);
    free(buf);

    uint8_t junk[64] = "this is not a Mach-O file";
    CHECK(!macho_parse(junk, sizeof junk, &m, err, sizeof err));
    CHECK_CONTAINS(err, "not a Mach-O file");
    CHECK(!macho_parse(junk, 0, &m, err, sizeof err));
}

TEST(macho_refuses_truncated_load_commands) {
    mb_opts o = {.thin = true};
    size_t len;
    uint8_t *buf = mb_build(&o, &len);
    macho_file m;
    char err[256] = "";
    CHECK(!macho_parse(buf, 28 + 40, &m, err, sizeof err));
    CHECK_CONTAINS(err, "load commands are truncated");

    /* A command whose size runs past sizeofcmds. */
    wr_be32(buf + 28 + 4, 0x10000);
    CHECK(!macho_parse(buf, len, &m, err, sizeof err));
    CHECK_CONTAINS(err, "load command 0 is truncated");
    free(buf);
}

TEST(macho_refuses_a_section_outside_the_file) {
    mb_opts o = {.section_outside = true};
    macho_file m;
    char err[256] = "";
    uint8_t *buf;
    CHECK(!parse_built(&o, &m, err, sizeof err, &buf));
    CHECK_CONTAINS(err, "section __TEXT,__text lies outside the file");
    free(buf);
}

TEST(macho_refuses_an_indirect_index_past_the_symbol_table) {
    mb_opts o = {.bad_indirect = true};
    macho_file m;
    char err[256] = "";
    uint8_t *buf;
    CHECK(!parse_built(&o, &m, err, sizeof err, &buf));
    CHECK_CONTAINS(err, "indirect symbol 5 is past the symbol table");
    free(buf);
}

TEST(macho_strips_one_leading_underscore) {
    mb_opts o = {.thin = true};
    size_t len;
    uint8_t *buf = mb_build(&o, &len);
    /* Rename _exit (8 bytes at its string) to ___cxa (C++ runtime style). */
    uint32_t strx = rd_be32(buf + 0x2000 + 12 * 2);
    memcpy(buf + 0x2080 + strx, "___cxa", 7);
    strx = rd_be32(buf + 0x2000 + 12 * 4);
    memcpy(buf + 0x2080 + strx, "__Znwm", 7);
    macho_file m;
    char err[256] = "";
    CHECK(macho_parse(buf, len, &m, err, sizeof err));
    CHECK_STR(m.syms[0].name, "main");
    CHECK_STR(m.syms[1].name, "CFRelease");
    CHECK_STR(m.syms[2].name, "__cxa");
    CHECK_STR(m.syms[4].name, "_Znwm");
    macho_free(&m);
    free(buf);
}

TEST(macho_reads_external_relocations) {
    mb_opts o = {0};
    macho_file m;
    char err[256] = "";
    uint8_t *buf;
    CHECK(parse_built(&o, &m, err, sizeof err, &buf));
    CHECK_EQ(m.nextrel, 2);
    CHECK_EQ(m.extrel[0].address, MB_DATA);
    CHECK_STR(m.syms[m.extrel[0].symbol].name, "kCFPreferencesCurrentApplication");
    CHECK_EQ(m.extrel[1].address, MB_DATA + 4);
    CHECK_STR(m.syms[m.extrel[1].symbol].name, "CFRelease");
    macho_free(&m);

    /* A PC-relative one is refused. */
    size_t len;
    uint8_t *thin = mb_build(&(mb_opts){.thin = true}, &len);
    wr_be32(thin + 0x2060 + 4, (3u << 8) | (1u << 7) | (2u << 5) | (1u << 4));
    CHECK(!macho_parse(thin, len, &m, err, sizeof err));
    CHECK_CONTAINS(err, "external relocation 0 is not a 32-bit absolute word");
    free(thin);
    free(buf);
}

TEST(macho_monster_fair_facts) {
    SKIP_UNLESS_MF();
    size_t len;
    uint8_t *buf = read_file(test_mf_exe_path(), &len);
    CHECK(buf != NULL);
    macho_file m;
    char err[256] = "";
    CHECK(macho_parse(buf, len, &m, err, sizeof err));
    CHECK(m.data == buf + 0x1000);
    CHECK_EQ(m.len, 392648);
    CHECK_EQ(m.entry, 0x2fdc);

    const macho_section *la = macho_find_section(&m, "__DATA", "__la_symbol_ptr");
    CHECK(la != NULL);
    CHECK_EQ(la->addr, 0x5b144);
    CHECK_EQ(la->size / 4, 193);
    const macho_section *nl = macho_find_section(&m, "__DATA", "__nl_symbol_ptr");
    CHECK(nl != NULL);
    CHECK_EQ(nl->addr, 0x5b03c);
    CHECK_EQ(nl->size / 4, 66);
    const macho_section *stubs = macho_find_section(&m, "__TEXT", "__symbol_stub1");
    CHECK(stubs != NULL);
    CHECK_EQ(stubs->addr, 0x431f0);
    CHECK_EQ(stubs->size / stubs->reserved2, 193);
    const macho_section *init = macho_find_section(&m, "__DATA", "__mod_init_func");
    CHECK(init != NULL);
    CHECK_EQ(init->addr, 0x5b008);
    CHECK_EQ(init->size / 4, 13);

    uint32_t imports = 0;
    for (uint32_t i = 0; i < m.nsyms; i++)
        imports += macho_symbol_is_import(&m.syms[i]);
    CHECK_EQ(imports, 205);
    CHECK_EQ(m.nextrel, 212);
    macho_free(&m);
    free(buf);
}
