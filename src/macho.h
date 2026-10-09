#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Mach-O, the Mac OS X executable format: a thin 32-bit PowerPC executable,
   or the PowerPC slice of a fat (universal) one. Every field is big-endian.
   Reference: Apple's <mach-o/loader.h>, <mach-o/nlist.h>, <mach-o/reloc.h>
   and <mach-o/fat.h>. */

#define MACHO_MAX_SEGMENTS 8
#define MACHO_MAX_SECTIONS 32
#define MACHO_MAX_DYLIBS 16

enum {
    MACHO_VM_PROT_READ = 1,
    MACHO_VM_PROT_WRITE = 2,
    MACHO_VM_PROT_EXECUTE = 4,
};

/* Section types (the low byte of flags). */
enum {
    MACHO_S_REGULAR = 0,
    MACHO_S_ZEROFILL = 1,
    MACHO_S_NON_LAZY_SYMBOL_POINTERS = 6,
    MACHO_S_LAZY_SYMBOL_POINTERS = 7,
    MACHO_S_SYMBOL_STUBS = 8,
    MACHO_S_MOD_INIT_FUNC_POINTERS = 9,
};

/* n_type bits. */
enum {
    MACHO_N_STAB = 0xE0,
    MACHO_N_TYPE = 0x0E,
    MACHO_N_EXT = 0x01,
    MACHO_N_UNDF = 0x00,
    MACHO_N_ABS = 0x02,
    MACHO_N_PBUD = 0x0C, /* undefined, prebound: how a PREBOUND executable marks its imports */
    MACHO_N_SECT = 0x0E,
};

/* Indirect symbol table entries that name no symbol. */
#define MACHO_INDIRECT_SYMBOL_LOCAL 0x80000000u
#define MACHO_INDIRECT_SYMBOL_ABS 0x40000000u

typedef struct {
    char name[17];
    uint32_t vmaddr, vmsize, fileoff, filesize;
    int initprot; /* MACHO_VM_PROT_* bits */
} macho_segment;

typedef struct {
    char segname[17], sectname[17];
    uint32_t addr, size, offset;
    uint32_t flags;     /* low byte: MACHO_S_* type */
    uint32_t reserved1; /* first indirect-symbol index (pointer and stub sections) */
    uint32_t reserved2; /* stub size (stub sections) */
} macho_section;

typedef struct {
    const char *name; /* without the leading '_': "CFRelease", "_Znwm" */
    uint8_t type;     /* n_type */
    uint16_t desc;    /* n_desc: library ordinal in the high byte */
    uint32_t value;
} macho_symbol;

typedef struct {
    uint32_t address; /* where the word to relocate lives */
    uint32_t symbol;  /* index into syms */
} macho_extern_reloc;

typedef struct {
    const uint8_t *data; /* the PowerPC slice; points into the caller's buffer */
    size_t len;
    uint32_t entry; /* LC_UNIXTHREAD srr0 */
    int nsegments, nsections;
    macho_segment segments[MACHO_MAX_SEGMENTS];
    macho_section sections[MACHO_MAX_SECTIONS];
    uint32_t nsyms;
    macho_symbol *syms;
    uint32_t nindirect;
    uint32_t *indirect; /* MACHO_INDIRECT_SYMBOL_LOCAL, _ABS, or an index into syms */
    uint32_t nextrel;
    macho_extern_reloc *extrel;
    int ndylibs;
    char dylibs[MACHO_MAX_DYLIBS][128];
} macho_file;

/* Parses a thin PowerPC Mach-O executable, or the PowerPC slice of a fat
   one. buf must outlive m. On failure writes err and leaves nothing to free. */
bool macho_parse(const uint8_t *buf, size_t len, macho_file *m, char *err, size_t errlen);

/* Frees the symbol, indirect and relocation tables. Safe to call more than once. */
void macho_free(macho_file *m);

/* The section named seg,sect, or NULL. */
const macho_section *macho_find_section(const macho_file *m, const char *seg, const char *sect);

/* True for an undefined external symbol (an import). */
bool macho_symbol_is_import(const macho_symbol *s);
