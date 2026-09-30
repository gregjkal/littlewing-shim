#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* PEF (Preferred Executable Format), the classic Mac OS PowerPC container.
   Reference: "Mac OS Runtime Architectures", chapter 8. */

enum {
    PEF_KIND_CODE = 0,
    PEF_KIND_UNPACKED_DATA = 1,
    PEF_KIND_PATTERN_DATA = 2,
    PEF_KIND_CONSTANT = 3,
    PEF_KIND_LOADER = 4,
};

enum {
    PEF_SYM_CODE = 0,
    PEF_SYM_DATA = 1,
    PEF_SYM_TVECTOR = 2,
    PEF_SYM_TOC = 3,
    PEF_SYM_GLUE = 4,
};

#define PEF_MAX_SECTIONS 16

typedef struct {
    uint32_t total_len;     /* bytes in memory, including trailing zero fill */
    uint32_t unpacked_len;  /* initialized bytes */
    uint32_t container_len; /* bytes in the file */
    uint32_t container_off; /* file offset */
    uint8_t kind;           /* PEF_KIND_* */
} pef_section;

typedef struct {
    char *name;
    char *library;
    uint8_t sym_class; /* PEF_SYM_* */
    bool weak;
} pef_import;

typedef struct {
    const uint8_t *file;
    size_t file_len;
    int nsections;
    pef_section sections[PEF_MAX_SECTIONS];
    int32_t main_section; /* -1 if none */
    uint32_t main_offset;
    int32_t init_section; /* -1 if none */
    uint32_t init_offset;
    uint32_t nimports;
    pef_import *imports;
    uint32_t nreloc_sections;
    uint32_t reloc_headers_off; /* file offset of the first 12-byte relocation header */
    uint32_t reloc_instr_off;   /* file offset of the relocation instructions */
} pef_file;

/* Parses buf (which must outlive out). On failure writes a message to err,
   frees anything allocated, and returns false. */
bool pef_parse(const uint8_t *buf, size_t len, pef_file *out, char *err, size_t errlen);

/* Frees the import table. Safe to call more than once. */
void pef_free(pef_file *pef);

/* Task 5: unpacks pattern-initialized data. Must produce exactly dstlen bytes. */
bool pef_unpack_pattern(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen,
                        char *err, size_t errlen);

/* Task 6: one section's relocation state. */
typedef struct {
    uint8_t *host;     /* the instantiated section's bytes */
    uint32_t len;      /* bytes available at host */
    uint32_t section_c; /* initial sectionC: address of section 0 */
    uint32_t section_d; /* initial sectionD: address of section 1 */
    const uint32_t *import_addr;
    uint32_t nimports;
} pef_reloc_target;

/* Task 6: runs ninstrs 16-bit big-endian relocation instructions against t. */
bool pef_reloc_run(const uint8_t *instrs, uint32_t ninstrs, const pef_reloc_target *t,
                   char *err, size_t errlen);

/* Task 6: runs every relocation header in the file. Arrays are indexed by
   section; section_host[i] is NULL for sections that were not instantiated. */
bool pef_relocate(const pef_file *pef, uint8_t *const section_host[],
                  const uint32_t section_addr[], const uint32_t section_len[],
                  const uint32_t import_addr[], char *err, size_t errlen);
