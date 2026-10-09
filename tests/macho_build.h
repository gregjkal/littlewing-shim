#pragma once
/* Builds a small PowerPC Mach-O executable in memory, shaped like MONSTER
   FAIR's: a fat file whose PowerPC slice has __TEXT, __DATA and __LINKEDIT.

   Addresses (all in the slice; __TEXT starts at file offset 0):
     __PAGEZERO 0x0-0x1000
     __TEXT     0x1000-0x2000   file 0x0
       __text           0x1800  entry: bl _start; trap
                        0x1810  _start: bl helper; bl main; bl exit stub; trap
                        0x1830  helper: blr
                        0x1840  main: li r3,42; blr
                        0x1850  a static initializer: blr
       __symbol_stub1   0x1900  2 stubs of 16 bytes: malloc, exit
     __DATA     0x2000-0x3000   file 0x1000
       __dyld           0x2000  0x8fe01000, 0x8fe01008
       __nl_symbol_ptr  0x2008  2 slots: kCFPreferencesCurrentApplication, local (holds 0x1234)
       __la_symbol_ptr  0x2010  2 slots: malloc, exit (0 in the file)
       __mod_init_func  0x2018  1 entry: 0x1850
       __data           0x2020  0x2020 holds 8 (an addend), 0x2024 holds 0
     __LINKEDIT 0x3000-0x4000   file 0x2000: symbols, indirect table, relocations, strings

   Symbols: _main (defined), then the imports _CFRelease, _exit,
   _kCFPreferencesCurrentApplication, _malloc. External relocations: 0x2020
   to kCFPreferencesCurrentApplication, 0x2024 to CFRelease. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

typedef struct {
    bool thin;            /* no fat header: the slice alone */
    bool no_ppc;          /* a fat file with only an i386 slice */
    bool section_outside; /* __text's file offset is past the end */
    bool bad_indirect;    /* an indirect entry names symbol 99 */
    bool no_exit_call;    /* _start never calls the exit stub */
} mb_opts;

enum {
    MB_ENTRY = 0x1800,
    MB_START = 0x1810,
    MB_MAIN = 0x1840,
    MB_INIT = 0x1850,
    MB_STUBS = 0x1900,
    MB_DYLD = 0x2000,
    MB_NL = 0x2008,
    MB_LA = 0x2010,
    MB_MODINIT = 0x2018,
    MB_DATA = 0x2020,
    MB_SLICE_LEN = 0x2200,
    MB_FAT_PPC_OFF = 0x2000, /* the PowerPC slice's offset in a fat file */
};

static inline uint32_t mb_bl(uint32_t from, uint32_t to) {
    return 0x48000001u | ((to - from) & 0x03FFFFFCu);
}

/* A section header in an LC_SEGMENT. */
static inline uint8_t *mb_section(uint8_t *p, const char *sect, const char *seg, uint32_t addr,
                                  uint32_t size, uint32_t off, uint32_t flags, uint32_t r1,
                                  uint32_t r2) {
    memset(p, 0, 68);
    strncpy((char *)p, sect, 16);
    strncpy((char *)p + 16, seg, 16);
    wr_be32(p + 32, addr);
    wr_be32(p + 36, size);
    wr_be32(p + 40, off);
    wr_be32(p + 56, flags);
    wr_be32(p + 60, r1);
    wr_be32(p + 64, r2);
    return p + 68;
}

static inline uint8_t *mb_segment(uint8_t *p, const char *name, uint32_t vmaddr, uint32_t vmsize,
                                  uint32_t fileoff, uint32_t filesize, uint32_t prot,
                                  uint32_t nsects) {
    memset(p, 0, 56);
    wr_be32(p, 1); /* LC_SEGMENT */
    wr_be32(p + 4, 56 + 68 * nsects);
    strncpy((char *)p + 8, name, 16);
    wr_be32(p + 24, vmaddr);
    wr_be32(p + 28, vmsize);
    wr_be32(p + 32, fileoff);
    wr_be32(p + 36, filesize);
    wr_be32(p + 40, 7);
    wr_be32(p + 44, prot);
    wr_be32(p + 48, nsects);
    return p + 56;
}

static inline uint8_t *mb_dylib(uint8_t *p, const char *path) {
    uint32_t size = (uint32_t)(24 + strlen(path) + 1 + 3) & ~3u;
    memset(p, 0, size);
    wr_be32(p, 0xC); /* LC_LOAD_DYLIB */
    wr_be32(p + 4, size);
    wr_be32(p + 8, 24);
    strcpy((char *)p + 24, path);
    return p + size;
}

/* Writes the PowerPC slice into s (MB_SLICE_LEN bytes, zeroed). */
static inline void mb_build_slice(uint8_t *s, const mb_opts *o) {
    /* Header; ncmds and sizeofcmds are filled in at the end. */
    wr_be32(s, 0xFEEDFACE);
    wr_be32(s + 4, 18);  /* CPU_TYPE_POWERPC */
    wr_be32(s + 8, 10);  /* ppc7400 */
    wr_be32(s + 12, 2);  /* MH_EXECUTE */
    wr_be32(s + 24, 0x10095);
    uint8_t *p = s + 28;
    uint32_t ncmds = 0;

    p = mb_segment(p, "__PAGEZERO", 0, 0x1000, 0, 0, 0, 0), ncmds++;
    p = mb_segment(p, "__TEXT", 0x1000, 0x1000, 0, 0x1000, 5, 2), ncmds++;
    p = mb_section(p, "__text", "__TEXT", MB_ENTRY, 0x60,
                   o->section_outside ? 0x100000 : MB_ENTRY - 0x1000, 0x80000400, 0, 0);
    p = mb_section(p, "__symbol_stub1", "__TEXT", MB_STUBS, 32, MB_STUBS - 0x1000, 0x80000408, 0,
                   16);
    p = mb_segment(p, "__DATA", 0x2000, 0x1000, 0x1000, 0x1000, 3, 5), ncmds++;
    p = mb_section(p, "__dyld", "__DATA", MB_DYLD, 8, 0x1000, 0, 0, 0);
    p = mb_section(p, "__nl_symbol_ptr", "__DATA", MB_NL, 8, 0x1008, 6, 2, 0);
    p = mb_section(p, "__la_symbol_ptr", "__DATA", MB_LA, 8, 0x1010, 7, 4, 0);
    p = mb_section(p, "__mod_init_func", "__DATA", MB_MODINIT, 4, 0x1018, 9, 0, 0);
    p = mb_section(p, "__data", "__DATA", MB_DATA, 16, 0x1020, 0, 0, 0);
    p = mb_segment(p, "__LINKEDIT", 0x3000, 0x1000, 0x2000, MB_SLICE_LEN - 0x2000, 1, 0), ncmds++;

    /* LC_SYMTAB and LC_DYSYMTAB. */
    const uint32_t symoff = 0x2000, nsyms = 5, indoff = 0x2040, nind = 6, reloff = 0x2060,
                   nrel = 2, stroff = 0x2080, strsize = 0x100;
    wr_be32(p, 2), wr_be32(p + 4, 24), wr_be32(p + 8, symoff), wr_be32(p + 12, nsyms);
    wr_be32(p + 16, stroff), wr_be32(p + 20, strsize);
    p += 24, ncmds++;
    memset(p, 0, 80);
    wr_be32(p, 0xB), wr_be32(p + 4, 80);
    wr_be32(p + 16, 0), wr_be32(p + 20, 1); /* iextdefsym, nextdefsym */
    wr_be32(p + 24, 1), wr_be32(p + 28, 4); /* iundefsym, nundefsym */
    wr_be32(p + 56, indoff), wr_be32(p + 60, nind);
    wr_be32(p + 64, reloff), wr_be32(p + 68, nrel);
    p += 80, ncmds++;

    /* LC_LOAD_DYLINKER, two LC_LOAD_DYLIBs, LC_UNIXTHREAD. */
    wr_be32(p, 0xE), wr_be32(p + 4, 28), wr_be32(p + 8, 12);
    strcpy((char *)p + 12, "/usr/lib/dyld");
    p += 28, ncmds++;
    p = mb_dylib(p, "/usr/lib/libSystem.B.dylib"), ncmds++;
    p = mb_dylib(p, "/System/Library/Frameworks/Carbon.framework/Versions/A/Carbon"), ncmds++;
    wr_be32(p, 5), wr_be32(p + 4, 16 + 160), wr_be32(p + 8, 1), wr_be32(p + 12, 40);
    wr_be32(p + 16, MB_ENTRY);
    p += 16 + 160, ncmds++;
    wr_be32(s + 16, ncmds);
    wr_be32(s + 20, (uint32_t)(p - s - 28));

    /* Code. */
    uint8_t *t = s + MB_ENTRY - 0x1000;
    wr_be32(t + 0x00, mb_bl(MB_ENTRY, MB_START));
    wr_be32(t + 0x04, 0x7FE00008); /* trap */
    wr_be32(t + 0x10, mb_bl(MB_START, 0x1830));
    wr_be32(t + 0x14, mb_bl(MB_START + 4, MB_MAIN));
    wr_be32(t + 0x18, o->no_exit_call ? 0x60000000 : mb_bl(MB_START + 8, MB_STUBS + 16));
    wr_be32(t + 0x1C, 0x7FE00008);
    wr_be32(t + 0x30, 0x4E800020); /* blr */
    wr_be32(t + 0x40, 0x3860002A); /* li r3,42 */
    wr_be32(t + 0x44, 0x4E800020);
    wr_be32(t + 0x50, 0x4E800020);
    /* Stubs: lis r11,ha(ptr); lwzu r12,lo(ptr)(r11); mtctr r12; bctr */
    for (int i = 0; i < 2; i++) {
        uint8_t *st = s + MB_STUBS - 0x1000 + 16 * i;
        uint32_t ptr = MB_LA + 4u * (uint32_t)i;
        wr_be32(st, 0x3D600000u | (((ptr + 0x8000) >> 16) & 0xFFFF));
        wr_be32(st + 4, 0x858B0000u | (ptr & 0xFFFF));
        wr_be32(st + 8, 0x7D8903A6);
        wr_be32(st + 12, 0x4E800420);
    }

    /* Data. */
    uint8_t *d = s + 0x1000;
    wr_be32(d + 0x00, 0x8FE01000);
    wr_be32(d + 0x04, 0x8FE01008);
    wr_be32(d + 0x0C, 0x1234); /* the local non-lazy slot */
    wr_be32(d + 0x18, MB_INIT);
    wr_be32(d + 0x20, 8);

    /* Strings and symbols. */
    static const char *const names[] = {"_main", "_CFRelease", "_exit",
                                        "_kCFPreferencesCurrentApplication", "_malloc"};
    uint32_t strx = 1; /* offset 0 is the empty name */
    for (uint32_t i = 0; i < nsyms; i++) {
        uint8_t *e = s + symoff + 12 * i;
        strcpy((char *)s + stroff + strx, names[i]);
        wr_be32(e, strx);
        strx += (uint32_t)strlen(names[i]) + 1;
        if (i == 0) {
            e[4] = 0x0F; /* N_SECT | N_EXT */
            e[5] = 1;
            wr_be32(e + 8, MB_MAIN);
        } else {
            e[4] = i == 4 ? 0x0D : 0x01; /* N_PBUD | N_EXT (as in the game), or N_UNDF | N_EXT */
            wr_be16(e + 6, (uint16_t)((i == 1 || i == 3 ? 2 : 1) << 8));
        }
    }

    /* Indirect table: stubs [malloc, exit], non-lazy [kCF..., local], lazy [malloc, exit]. */
    const uint32_t ind[6] = {4, 2, 3, 0x80000000u, 4, o->bad_indirect ? 99 : 2};
    for (int i = 0; i < 6; i++)
        wr_be32(s + indoff + 4 * i, ind[i]);

    /* External relocations: symbolnum:24 pcrel:1 length:2 extern:1 type:4. */
    wr_be32(s + reloff, MB_DATA);
    wr_be32(s + reloff + 4, (3u << 8) | (2u << 5) | (1u << 4));
    wr_be32(s + reloff + 8, MB_DATA + 4);
    wr_be32(s + reloff + 12, (1u << 8) | (2u << 5) | (1u << 4));
}

/* Builds the file. The caller frees it. */
static inline uint8_t *mb_build(const mb_opts *o, size_t *len) {
    if (o->thin) {
        uint8_t *s = calloc(1, MB_SLICE_LEN);
        mb_build_slice(s, o);
        *len = MB_SLICE_LEN;
        return s;
    }
    /* Fat: an i386 slice first (junk), then the PowerPC one unless no_ppc. */
    size_t total = MB_FAT_PPC_OFF + MB_SLICE_LEN;
    uint8_t *f = calloc(1, total);
    wr_be32(f, 0xCAFEBABE);
    wr_be32(f + 4, o->no_ppc ? 1 : 2);
    wr_be32(f + 8, 7); /* CPU_TYPE_I386 */
    wr_be32(f + 12, 3);
    wr_be32(f + 16, 0x1000);
    wr_be32(f + 20, 0x100);
    wr_be32(f + 24, 12);
    memcpy(f + 0x1000, "\xCE\xFA\xED\xFE", 4);
    if (!o->no_ppc) {
        wr_be32(f + 28, 18);
        wr_be32(f + 32, 10);
        wr_be32(f + 36, MB_FAT_PPC_OFF);
        wr_be32(f + 40, MB_SLICE_LEN);
        wr_be32(f + 44, 12);
        mb_build_slice(f + MB_FAT_PPC_OFF, o);
    }
    *len = total;
    return f;
}
