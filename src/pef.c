#include "pef.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

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

/* strdup of the NUL-terminated string at start, which must end before limit. */
static char *dup_string(const pef_file *pef, uint64_t start, uint64_t limit) {
    for (uint64_t i = start; i < limit; i++)
        if (pef->file[i] == '\0')
            return strdup((const char *)pef->file + start);
    return NULL;
}

static bool parse(const uint8_t *buf, size_t len, pef_file *out, char *err, size_t errlen) {
    out->file = buf;
    out->file_len = len;
    if (len < 40 || memcmp(buf, "Joy!peff", 8) != 0 || memcmp(buf + 8, "pwpc", 4) != 0)
        return fail(err, errlen, "not a PowerPC PEF file");

    uint16_t nsec = rd_be16(buf + 32);
    if (nsec > PEF_MAX_SECTIONS)
        return fail(err, errlen, "too many sections (%u)", nsec);
    if (!in_file(len, 40, 28ull * nsec))
        return fail(err, errlen, "section headers are truncated");

    int loader = -1;
    for (int i = 0; i < nsec; i++) {
        const uint8_t *h = buf + 40 + 28 * i;
        pef_section *s = &out->sections[i];
        s->total_len = rd_be32(h + 8);
        s->unpacked_len = rd_be32(h + 12);
        s->container_len = rd_be32(h + 16);
        s->container_off = rd_be32(h + 20);
        s->kind = h[24];
        if (!in_file(len, s->container_off, s->container_len))
            return fail(err, errlen, "section %d lies outside the file", i);
        if (s->kind == PEF_KIND_LOADER && loader < 0)
            loader = i;
    }
    out->nsections = nsec;
    if (loader < 0)
        return fail(err, errlen, "no loader section");

    uint64_t L = out->sections[loader].container_off;
    uint64_t Lend = L + out->sections[loader].container_len;
    if (Lend - L < 56)
        return fail(err, errlen, "loader section is truncated");
    const uint8_t *lh = buf + L;
    out->main_section = (int32_t)rd_be32(lh + 0);
    out->main_offset = rd_be32(lh + 4);
    out->init_section = (int32_t)rd_be32(lh + 8);
    out->init_offset = rd_be32(lh + 12);
    uint32_t nlibs = rd_be32(lh + 24);
    uint32_t nsyms = rd_be32(lh + 28);
    out->nreloc_sections = rd_be32(lh + 32);
    uint32_t reloc_instr = rd_be32(lh + 36);
    uint32_t strings = rd_be32(lh + 40);

    uint64_t libs_off = L + 56;
    uint64_t syms_off = libs_off + 24ull * nlibs;
    uint64_t rel_off = syms_off + 4ull * nsyms;
    if (rel_off + 12ull * out->nreloc_sections > Lend)
        return fail(err, errlen, "loader tables are truncated");
    if (L + reloc_instr > Lend || L + strings > Lend)
        return fail(err, errlen, "loader offsets are out of range");
    out->reloc_headers_off = (uint32_t)rel_off;
    out->reloc_instr_off = (uint32_t)(L + reloc_instr);

    out->nimports = nsyms;
    out->imports = calloc(nsyms ? nsyms : 1, sizeof *out->imports);
    if (!out->imports)
        return fail(err, errlen, "out of memory");

    uint64_t str_base = L + strings;
    for (uint32_t li = 0; li < nlibs; li++) {
        const uint8_t *e = buf + libs_off + 24ull * li;
        uint32_t name_off = rd_be32(e);
        uint32_t count = rd_be32(e + 12);
        uint32_t first = rd_be32(e + 16);
        if ((uint64_t)first + count > nsyms)
            return fail(err, errlen, "library %u has an out-of-range symbol list", li);
        for (uint32_t k = first; k < first + count; k++) {
            if (out->imports[k].library)
                return fail(err, errlen, "import %u belongs to two libraries", k);
            out->imports[k].library = dup_string(out, str_base + name_off, Lend);
            if (!out->imports[k].library)
                return fail(err, errlen, "library %u has a bad name", li);
        }
    }
    for (uint32_t i = 0; i < nsyms; i++) {
        if (!out->imports[i].library)
            return fail(err, errlen, "import %u has no library", i);
        uint32_t w = rd_be32(buf + syms_off + 4ull * i);
        out->imports[i].sym_class = (uint8_t)((w >> 24) & 0x0F);
        out->imports[i].weak = ((w >> 24) & 0x80) != 0;
        out->imports[i].name = dup_string(out, str_base + (w & 0xFFFFFF), Lend);
        if (!out->imports[i].name)
            return fail(err, errlen, "import %u has a bad name", i);
    }
    return true;
}

bool pef_parse(const uint8_t *buf, size_t len, pef_file *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    if (parse(buf, len, out, err, errlen))
        return true;
    pef_free(out);
    return false;
}

void pef_free(pef_file *pef) {
    if (pef->imports) {
        for (uint32_t i = 0; i < pef->nimports; i++) {
            free(pef->imports[i].name);
            free(pef->imports[i].library);
        }
        free(pef->imports);
    }
    pef->imports = NULL;
    pef->nimports = 0;
}

/* ---- pattern-initialized data ---- */

typedef struct {
    const uint8_t *p, *end;
    uint8_t *dst;
    size_t out, cap;
    char *err;
    size_t errlen;
} unpacker;

static bool u_arg(unpacker *u, uint32_t *v) {
    *v = 0;
    for (int i = 0; i < 5; i++) {
        if (u->p >= u->end)
            return fail(u->err, u->errlen, "truncated pattern data");
        uint8_t b = *u->p++;
        *v = (*v << 7) | (b & 0x7F);
        if (!(b & 0x80))
            return true;
    }
    return fail(u->err, u->errlen, "bad pattern-data argument");
}

static bool u_zero(unpacker *u, uint32_t n) {
    if (n > u->cap - u->out)
        return fail(u->err, u->errlen, "pattern data overflows the section");
    memset(u->dst + u->out, 0, n);
    u->out += n;
    return true;
}

static bool u_copy(unpacker *u, uint32_t n) {
    if (n > (size_t)(u->end - u->p))
        return fail(u->err, u->errlen, "truncated pattern data");
    if (n > u->cap - u->out)
        return fail(u->err, u->errlen, "pattern data overflows the section");
    memcpy(u->dst + u->out, u->p, n);
    u->p += n;
    u->out += n;
    return true;
}

bool pef_unpack_pattern(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen,
                        char *err, size_t errlen) {
    unpacker u = {src, src + srclen, dst, 0, dstlen, err, errlen};
    while (u.p < u.end) {
        uint8_t b = *u.p++;
        uint32_t op = b >> 5, count = b & 0x1F;
        if (count == 0 && !u_arg(&u, &count))
            return false;
        switch (op) {
        case 0: /* Zero */
            if (!u_zero(&u, count))
                return false;
            break;
        case 1: /* BlockCopy */
            if (!u_copy(&u, count))
                return false;
            break;
        case 4: { /* InterleaveRepeatBlockWithZero */
            uint32_t custom, repeat;
            if (!u_arg(&u, &custom) || !u_arg(&u, &repeat))
                return false;
            if (!u_zero(&u, count))
                return false;
            for (uint32_t i = 0; i < repeat; i++)
                if (!u_copy(&u, custom) || !u_zero(&u, count))
                    return false;
            break;
        }
        default:
            return fail(err, errlen, "unsupported pattern opcode %u", op);
        }
    }
    if (u.out != dstlen)
        return fail(err, errlen, "pattern data produced %zu of %zu bytes", u.out, dstlen);
    return true;
}
