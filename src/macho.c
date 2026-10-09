#include "macho.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

enum {
    FAT_MAGIC = 0xCAFEBABE,
    MH_MAGIC = 0xFEEDFACE,     /* 32-bit, read big-endian */
    MH_CIGAM = 0xCEFAEDFE,     /* 32-bit little-endian (i386) */
    MH_MAGIC_64 = 0xFEEDFACF,
    MH_CIGAM_64 = 0xCFFAEDFE,
    CPU_TYPE_POWERPC = 18,
    MH_EXECUTE = 2,

    LC_SEGMENT = 0x1,
    LC_SYMTAB = 0x2,
    LC_UNIXTHREAD = 0x5,
    LC_DYSYMTAB = 0xB,
    LC_LOAD_DYLIB = 0xC,
    PPC_THREAD_STATE = 1,

    MAX_FAT_ARCHS = 16,
};

static bool fail(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static bool fail(char *err, size_t errlen, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return false;
}

static bool in_file(size_t file_len, uint64_t off, uint64_t len) {
    return off <= file_len && len <= file_len - off;
}

/* A 16-byte name field, which isn't NUL-terminated when it's full. */
static void copy_name16(char out[17], const uint8_t *p) {
    memcpy(out, p, 16);
    out[16] = '\0';
}

/* Finds the PowerPC code in buf: the whole buffer for a thin file, or one
   slice of a fat file. */
static bool find_slice(const uint8_t *buf, size_t len, const uint8_t **slice, size_t *slice_len,
                       char *err, size_t errlen) {
    if (len < 4)
        return fail(err, errlen, "not a Mach-O file");
    uint32_t magic = rd_be32(buf);
    if (magic == FAT_MAGIC) {
        if (len < 8)
            return fail(err, errlen, "fat header is truncated");
        uint32_t n = rd_be32(buf + 4);
        if (n > MAX_FAT_ARCHS || !in_file(len, 8, 20ull * n))
            return fail(err, errlen, "fat header is truncated");
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *a = buf + 8 + 20 * i;
            if (rd_be32(a) != CPU_TYPE_POWERPC)
                continue;
            uint32_t off = rd_be32(a + 8), size = rd_be32(a + 12);
            if (!in_file(len, off, size))
                return fail(err, errlen, "the PowerPC slice lies outside the file");
            *slice = buf + off;
            *slice_len = size;
            return true;
        }
        return fail(err, errlen, "no PowerPC code in this file");
    }
    if (magic == MH_CIGAM || magic == MH_MAGIC_64 || magic == MH_CIGAM_64)
        return fail(err, errlen, "no PowerPC code in this file");
    if (magic != MH_MAGIC)
        return fail(err, errlen, "not a Mach-O file");
    *slice = buf;
    *slice_len = len;
    return true;
}

static bool parse_segment(macho_file *m, const uint8_t *c, uint32_t cmdsize, char *err,
                          size_t errlen) {
    if (cmdsize < 56)
        return fail(err, errlen, "LC_SEGMENT is truncated");
    if (m->nsegments == MACHO_MAX_SEGMENTS)
        return fail(err, errlen, "too many segments");
    macho_segment *s = &m->segments[m->nsegments];
    copy_name16(s->name, c + 8);
    s->vmaddr = rd_be32(c + 24);
    s->vmsize = rd_be32(c + 28);
    s->fileoff = rd_be32(c + 32);
    s->filesize = rd_be32(c + 36);
    s->initprot = (int)rd_be32(c + 44);
    uint32_t nsects = rd_be32(c + 48);
    if (!in_file(m->len, s->fileoff, s->filesize))
        return fail(err, errlen, "segment %s lies outside the file", s->name);
    if (s->filesize > s->vmsize)
        return fail(err, errlen, "segment %s is bigger in the file than in memory", s->name);
    if (56ull + 68ull * nsects > cmdsize)
        return fail(err, errlen, "segment %s's sections are truncated", s->name);
    if (nsects > (uint32_t)(MACHO_MAX_SECTIONS - m->nsections))
        return fail(err, errlen, "too many sections");
    m->nsegments++;

    for (uint32_t i = 0; i < nsects; i++) {
        const uint8_t *h = c + 56 + 68 * i;
        macho_section *x = &m->sections[m->nsections++];
        copy_name16(x->sectname, h);
        copy_name16(x->segname, h + 16);
        x->addr = rd_be32(h + 32);
        x->size = rd_be32(h + 36);
        x->offset = rd_be32(h + 40);
        x->flags = rd_be32(h + 56);
        x->reserved1 = rd_be32(h + 60);
        x->reserved2 = rd_be32(h + 64);
        if ((x->flags & 0xFF) != MACHO_S_ZEROFILL && x->size && !in_file(m->len, x->offset, x->size))
            return fail(err, errlen, "section %s,%s lies outside the file", x->segname, x->sectname);
        if ((uint64_t)x->addr < s->vmaddr || (uint64_t)x->addr + x->size > (uint64_t)s->vmaddr + s->vmsize)
            return fail(err, errlen, "section %s,%s lies outside its segment", x->segname,
                        x->sectname);
    }
    return true;
}

static bool parse_symtab(macho_file *m, const uint8_t *c, uint32_t cmdsize, char *err,
                         size_t errlen) {
    if (cmdsize < 24)
        return fail(err, errlen, "LC_SYMTAB is truncated");
    if (m->syms)
        return fail(err, errlen, "two symbol tables");
    uint32_t symoff = rd_be32(c + 8), nsyms = rd_be32(c + 12);
    uint32_t stroff = rd_be32(c + 16), strsize = rd_be32(c + 20);
    if (!in_file(m->len, symoff, 12ull * nsyms))
        return fail(err, errlen, "the symbol table lies outside the file");
    if (!in_file(m->len, stroff, strsize))
        return fail(err, errlen, "the string table lies outside the file");
    m->syms = calloc(nsyms ? nsyms : 1, sizeof *m->syms);
    if (!m->syms)
        return fail(err, errlen, "out of memory");
    m->nsyms = nsyms;

    const char *strtab = (const char *)m->data + stroff;
    for (uint32_t i = 0; i < nsyms; i++) {
        const uint8_t *e = m->data + symoff + 12ull * i;
        uint32_t strx = rd_be32(e);
        if (strx >= strsize || !memchr(strtab + strx, '\0', strsize - strx))
            return fail(err, errlen, "symbol %u has a bad name", i);
        const char *name = strtab + strx;
        m->syms[i].name = name[0] == '_' ? name + 1 : name;
        m->syms[i].type = e[4];
        m->syms[i].desc = rd_be16(e + 6);
        m->syms[i].value = rd_be32(e + 8);
    }
    return true;
}

/* Reads LC_DYSYMTAB's indirect table and external relocations. Runs after
   LC_SYMTAB, since both index the symbol table. */
static bool parse_dysymtab(macho_file *m, const uint8_t *c, char *err, size_t errlen) {
    uint32_t indoff = rd_be32(c + 56), nind = rd_be32(c + 60);
    uint32_t extreloff = rd_be32(c + 64), nextrel = rd_be32(c + 68);
    if (!in_file(m->len, indoff, 4ull * nind))
        return fail(err, errlen, "the indirect symbol table lies outside the file");
    if (!in_file(m->len, extreloff, 8ull * nextrel))
        return fail(err, errlen, "the external relocations lie outside the file");

    m->indirect = calloc(nind ? nind : 1, sizeof *m->indirect);
    m->extrel = calloc(nextrel ? nextrel : 1, sizeof *m->extrel);
    if (!m->indirect || !m->extrel)
        return fail(err, errlen, "out of memory");
    m->nindirect = nind;
    m->nextrel = nextrel;

    for (uint32_t i = 0; i < nind; i++) {
        uint32_t v = rd_be32(m->data + indoff + 4ull * i);
        if (!(v & (MACHO_INDIRECT_SYMBOL_LOCAL | MACHO_INDIRECT_SYMBOL_ABS)) && v >= m->nsyms)
            return fail(err, errlen, "indirect symbol %u is past the symbol table", i);
        m->indirect[i] = v;
    }
    for (uint32_t i = 0; i < nextrel; i++) {
        const uint8_t *r = m->data + extreloff + 8ull * i;
        uint32_t address = rd_be32(r), info = rd_be32(r + 4);
        if (address & 0x80000000u)
            return fail(err, errlen, "external relocation %u is scattered", i);
        uint32_t symnum = info >> 8;
        bool pcrel = (info >> 7) & 1, ext = (info >> 4) & 1;
        uint32_t length = (info >> 5) & 3, type = info & 0xF;
        if (!ext || pcrel || length != 2 || type != 0)
            return fail(err, errlen,
                        "external relocation %u is not a 32-bit absolute word (type %u, length %u, "
                        "pcrel %d, extern %d)",
                        i, type, length, pcrel, ext);
        if (symnum >= m->nsyms)
            return fail(err, errlen, "external relocation %u names symbol %u, past the table", i,
                        symnum);
        m->extrel[i].address = address;
        m->extrel[i].symbol = symnum;
    }
    return true;
}

static bool parse_dylib(macho_file *m, const uint8_t *c, uint32_t cmdsize, char *err,
                        size_t errlen) {
    if (cmdsize < 24)
        return fail(err, errlen, "LC_LOAD_DYLIB is truncated");
    if (m->ndylibs == MACHO_MAX_DYLIBS)
        return fail(err, errlen, "too many libraries");
    uint32_t off = rd_be32(c + 8);
    if (off >= cmdsize || !memchr(c + off, '\0', cmdsize - off))
        return fail(err, errlen, "a library has a bad name");
    snprintf(m->dylibs[m->ndylibs++], sizeof m->dylibs[0], "%s", (const char *)c + off);
    return true;
}

/* Checks that each pointer or stub section's indirect entries are in the table. */
static bool check_indirect_ranges(const macho_file *m, char *err, size_t errlen) {
    for (int i = 0; i < m->nsections; i++) {
        const macho_section *x = &m->sections[i];
        uint32_t type = x->flags & 0xFF, count;
        if (type == MACHO_S_NON_LAZY_SYMBOL_POINTERS || type == MACHO_S_LAZY_SYMBOL_POINTERS)
            count = x->size / 4;
        else if (type == MACHO_S_SYMBOL_STUBS && x->reserved2)
            count = x->size / x->reserved2;
        else if (type == MACHO_S_SYMBOL_STUBS)
            return fail(err, errlen, "stub section %s has no stub size", x->sectname);
        else
            continue;
        if ((uint64_t)x->reserved1 + count > m->nindirect)
            return fail(err, errlen, "section %s,%s runs past the indirect symbol table",
                        x->segname, x->sectname);
    }
    return true;
}

static bool parse(const uint8_t *buf, size_t len, macho_file *m, char *err, size_t errlen) {
    if (!find_slice(buf, len, &m->data, &m->len, err, errlen))
        return false;
    if (m->len < 28 || rd_be32(m->data) != MH_MAGIC)
        return fail(err, errlen, "the PowerPC slice is not a 32-bit Mach-O file");
    if (rd_be32(m->data + 4) != CPU_TYPE_POWERPC)
        return fail(err, errlen, "no PowerPC code in this file");
    if (rd_be32(m->data + 12) != MH_EXECUTE)
        return fail(err, errlen, "not an executable (file type %u)", rd_be32(m->data + 12));
    uint32_t ncmds = rd_be32(m->data + 16), sizeofcmds = rd_be32(m->data + 20);
    if (!in_file(m->len, 28, sizeofcmds))
        return fail(err, errlen, "load commands are truncated");

    const uint8_t *dysymtab = NULL;
    bool have_entry = false;
    uint32_t pos = 0;
    for (uint32_t i = 0; i < ncmds; i++) {
        if (sizeofcmds - pos < 8)
            return fail(err, errlen, "load commands are truncated");
        const uint8_t *c = m->data + 28 + pos;
        uint32_t cmd = rd_be32(c), cmdsize = rd_be32(c + 4);
        if (cmdsize < 8 || cmdsize % 4 || cmdsize > sizeofcmds - pos)
            return fail(err, errlen, "load command %u is truncated", i);
        switch (cmd) {
        case LC_SEGMENT:
            if (!parse_segment(m, c, cmdsize, err, errlen))
                return false;
            break;
        case LC_SYMTAB:
            if (!parse_symtab(m, c, cmdsize, err, errlen))
                return false;
            break;
        case LC_DYSYMTAB:
            if (cmdsize < 80)
                return fail(err, errlen, "LC_DYSYMTAB is truncated");
            dysymtab = c;
            break;
        case LC_LOAD_DYLIB:
            if (!parse_dylib(m, c, cmdsize, err, errlen))
                return false;
            break;
        case LC_UNIXTHREAD:
            /* flavor, count, then the thread state; PPC_THREAD_STATE starts with srr0. */
            if (cmdsize < 20 || rd_be32(c + 8) != PPC_THREAD_STATE)
                return fail(err, errlen, "LC_UNIXTHREAD has no PowerPC thread state");
            m->entry = rd_be32(c + 16);
            have_entry = true;
            break;
        default: /* LC_LOAD_DYLINKER, LC_UUID and the rest don't affect loading */
            break;
        }
        pos += cmdsize;
    }
    if (!have_entry)
        return fail(err, errlen, "no entry point (LC_UNIXTHREAD)");
    if (!m->syms)
        return fail(err, errlen, "no symbol table");
    if (dysymtab && !parse_dysymtab(m, dysymtab, err, errlen))
        return false;
    return check_indirect_ranges(m, err, errlen);
}

bool macho_parse(const uint8_t *buf, size_t len, macho_file *m, char *err, size_t errlen) {
    memset(m, 0, sizeof *m);
    if (parse(buf, len, m, err, errlen))
        return true;
    macho_free(m);
    return false;
}

void macho_free(macho_file *m) {
    free(m->syms);
    free(m->indirect);
    free(m->extrel);
    m->syms = NULL;
    m->indirect = NULL;
    m->extrel = NULL;
    m->nsyms = m->nindirect = m->nextrel = 0;
}

const macho_section *macho_find_section(const macho_file *m, const char *seg, const char *sect) {
    for (int i = 0; i < m->nsections; i++)
        if (strcmp(m->sections[i].segname, seg) == 0 && strcmp(m->sections[i].sectname, sect) == 0)
            return &m->sections[i];
    return NULL;
}

bool macho_symbol_is_import(const macho_symbol *s) {
    uint8_t t = s->type & MACHO_N_TYPE;
    return !(s->type & MACHO_N_STAB) && (s->type & MACHO_N_EXT) &&
           (t == MACHO_N_UNDF || t == MACHO_N_PBUD);
}
