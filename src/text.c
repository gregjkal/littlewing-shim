#include "text.h"

#include <CoreText/CoreText.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "guest_mem.h"
#include "memmgr.h"
#include "qd.h"
#include "snapshot.h"
#include "trap.h"
#include "util.h"

#define SYSTEM_FONT 0
#define APPL_FONT 1
#define GENEVA 3
#define SYSTEM_SIZE 12

/* Style bits (txFace). */
#define FACE_BOLD 0x01
#define FACE_ITALIC 0x02
#define FACE_UNDERLINE 0x04
#define FACE_CONDENSE 0x20
#define FACE_EXTEND 0x40

#define FMOUTPUT_SIZE 26
#define MAX_TEXT 32767

static const struct {
    int16_t id;
    const char *mac;  /* its classic name */
    const char *host; /* the PostScript name it draws in */
} fonts[] = {
    {SYSTEM_FONT, "Chicago", "LucidaGrande-Bold"},
    {2, "New York", "Times-Roman"},
    {GENEVA, "Geneva", "Geneva"},
    {4, "Monaco", "Monaco"},
    {16, "Palatino", "Palatino-Roman"},
    {20, "Times", "Times-Roman"},
    {21, "Helvetica", "Helvetica"},
    {22, "Courier", "Courier"},
    {23, "Symbol", "Symbol"},
};
#define NFONTS (sizeof fonts / sizeof fonts[0])

static struct {
    uint32_t fm_output; /* FMSwapFont's FMOutput, once there is one */
} T;
SNAPSHOT_STATE(T)

/* The host's fonts, made as they are first used: the host's alone. */
#define FONT_CACHE 16
static struct {
    int16_t id, size;
    bool italic;
    CTFontRef font;
} cache[FONT_CACHE];
static int ncache;

static const char *host_name(int16_t id) {
    if (id == APPL_FONT)
        id = GENEVA;
    for (size_t i = 0; i < NFONTS; i++)
        if (fonts[i].id == id)
            return fonts[i].host;
    return "Geneva";
}

static CTFontRef font_for(int16_t id, int16_t size, bool italic) {
    for (int i = 0; i < ncache; i++)
        if (cache[i].id == id && cache[i].size == size && cache[i].italic == italic)
            return cache[i].font;
    CFStringRef name = CFStringCreateWithCString(NULL, host_name(id), kCFStringEncodingASCII);
    CGAffineTransform slant = {1, 0, 0.25, 1, 0, 0};
    CTFontRef f = CTFontCreateWithName(name, size, italic ? &slant : NULL);
    CFRelease(name);
    if (!f)
        fatal("CoreText has no font %s", host_name(id));
    if (ncache == FONT_CACHE) {
        CFRelease(cache[0].font);
        memmove(&cache[0], &cache[1], (FONT_CACHE - 1) * sizeof cache[0]);
        ncache--;
    }
    cache[ncache++] = (typeof(cache[0])){id, size, italic, f};
    return f;
}

/* ---- the port's text state ---- */

static uint32_t port(void) { return qd_current_port(); }

typedef struct {
    int16_t font, size;
    uint8_t face;
    int16_t mode;
    int32_t space_extra; /* spExtra, Fixed */
} text_state;

static text_state state_of(uint32_t p) {
    text_state s = {(int16_t)gm_r16(p + PORT_TX_FONT), (int16_t)gm_r16(p + PORT_TX_SIZE),
                    gm_r8(p + PORT_TX_FACE), (int16_t)gm_r16(p + PORT_TX_MODE),
                    (int32_t)gm_r32(p + PORT_SP_EXTRA)};
    if (s.size <= 0)
        s.size = SYSTEM_SIZE;
    return s;
}

static CTFontRef font_of(const text_state *s) { return font_for(s->font, s->size, s->face & FACE_ITALIC); }

/* ---- laying out a line ---- */

typedef struct {
    int n;
    CGGlyph *glyphs;
    int *x;     /* where each glyph starts, from the line's start */
    int width;  /* where the pen ends up */
    int ascent, descent;
} line;

static void free_line(line *l) {
    free(l->glyphs);
    free(l->x);
}

/* The extra pixels the face adds to each character. */
static int face_extra(uint8_t face) {
    return ((face & FACE_BOLD) ? 1 : 0) + ((face & FACE_EXTEND) ? 1 : 0) - ((face & FACE_CONDENSE) ? 1 : 0);
}

static line lay_out(const text_state *s, const uint8_t *bytes, int n) {
    line l = {.n = n};
    CTFontRef f = font_of(s);
    l.ascent = (int)ceil(CTFontGetAscent(f));
    l.descent = (int)ceil(CTFontGetDescent(f));
    l.glyphs = calloc((size_t)(n ? n : 1), sizeof *l.glyphs);
    l.x = calloc((size_t)(n ? n : 1), sizeof *l.x);
    UniChar *chars = calloc((size_t)(n ? n : 1), sizeof *chars);
    CGSize *adv = calloc((size_t)(n ? n : 1), sizeof *adv);
    if (!l.glyphs || !l.x || !chars || !adv)
        fatal("out of memory");
    CFStringRef str = CFStringCreateWithBytes(NULL, bytes, n, kCFStringEncodingMacRoman, false);
    if (str && CFStringGetLength(str) == n)
        CFStringGetCharacters(str, CFRangeMake(0, n), chars);
    else
        for (int i = 0; i < n; i++)
            chars[i] = bytes[i] < 0x80 ? bytes[i] : '?';
    if (str)
        CFRelease(str);
    CTFontGetGlyphsForCharacters(f, chars, l.glyphs, n);
    CTFontGetAdvancesForGlyphs(f, kCTFontOrientationHorizontal, l.glyphs, adv, n);
    int extra = face_extra(s->face), space = (int)lround(s->space_extra / 65536.0);
    for (int i = 0; i < n; i++) {
        l.x[i] = l.width;
        int a = (int)lround(adv[i].width) + extra + (bytes[i] == ' ' ? space : 0);
        l.width += a > 0 ? a : 0;
    }
    free(chars);
    free(adv);
    return l;
}

/* ---- drawing a line ---- */

typedef struct {
    const uint8_t *mask;
    qd_rect at;
    int mode;
    qd_rgb fg, bg;
} text_job;

static void text_clipped(const qd_pixels *px, qd_rect clip, void *ctx) {
    text_job *j = ctx;
    if (!qd_fill_mask(px, j->at, j->mask, clip, j->mode, j->fg, j->bg))
        trap_crash("DrawText: text mode %d is not supported", j->mode);
}

/* Draws bytes at the pen, the baseline at its v, and moves the pen past them. */
static void draw_text(const uint8_t *bytes, int n) {
    if (n <= 0)
        return;
    text_state s = state_of(port());
    line l = lay_out(&s, bytes, n);
    int slant = (s.face & FACE_ITALIC) ? (l.ascent + 3) / 4 : 0;
    int bold = (s.face & FACE_BOLD) ? 1 : 0;
    int w = l.width + slant + bold + 2, h = l.ascent + l.descent;
    uint8_t *bits = calloc((size_t)w * (size_t)h, 1);
    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    CGContextRef cg = CGBitmapContextCreate(bits, (size_t)w, (size_t)h, 8, (size_t)w, gray, kCGImageAlphaNone);
    CGColorSpaceRelease(gray);
    if (!bits || !cg)
        fatal("out of memory for text");
    CGContextSetShouldAntialias(cg, false);
    CGContextSetShouldSmoothFonts(cg, false);
    CGContextSetGrayFillColor(cg, 1, 1);
    CGPoint *at = calloc((size_t)n, sizeof *at);
    if (!at)
        fatal("out of memory");
    CTFontRef f = font_of(&s);
    for (int pass = 0; pass <= bold; pass++) {
        for (int i = 0; i < n; i++)
            at[i] = CGPointMake(l.x[i] + pass, l.descent);
        CTFontDrawGlyphs(f, l.glyphs, at, (size_t)n, cg);
    }
    CGContextRelease(cg);
    free(at);
    for (int i = 0; i < w * h; i++)
        bits[i] = bits[i] >= 0x80;
    if ((s.face & FACE_UNDERLINE) && l.descent > 1) {
        int row = l.ascent + 1; /* the line below the baseline */
        for (int x = 0; x < l.width; x++)
            bits[row * w + x] = 1;
    }
    int16_t h0 = (int16_t)gm_r16(port() + PORT_PN_LOC + 2), v0 = (int16_t)gm_r16(port() + PORT_PN_LOC);
    text_job j = {.mask = bits,
                  .at = {(int16_t)(v0 - l.ascent), h0, (int16_t)(v0 + l.descent), (int16_t)(h0 + w)},
                  .mode = s.mode,
                  .fg = qd_read_rgb(port() + PORT_RGB_FG),
                  .bg = qd_read_rgb(port() + PORT_RGB_BK)};
    qd_draw("DrawText", 0, text_clipped, &j);
    gm_w16(port() + PORT_PN_LOC + 2, (uint16_t)(h0 + l.width));
    free(bits);
    free_line(&l);
}

static int text_width(const uint8_t *bytes, int n) {
    if (n <= 0)
        return 0;
    text_state s = state_of(port());
    line l = lay_out(&s, bytes, n);
    int w = l.width;
    free_line(&l);
    return w;
}

static const uint8_t *text_at(uint32_t p, int first, int count) {
    if (count < 0 || count > MAX_TEXT)
        trap_crash("DrawText: %d characters", count);
    return gm_ptr(p + (uint32_t)first, (uint32_t)(count ? count : 1));
}

static void h_draw_string(void) {
    uint32_t s = trap_arg(0);
    draw_text(gm_ptr(s + 1, 255), gm_r8(s));
}

/* DrawText(const void *textBuf, short firstByte, short byteCount) */
static void h_draw_text(void) {
    int first = (int16_t)trap_arg(1), count = (int16_t)trap_arg(2);
    draw_text(text_at(trap_arg(0), first, count), count);
}

static void h_draw_char(void) {
    uint8_t c = (uint8_t)trap_arg(0);
    draw_text(&c, 1);
}

static void h_string_width(void) {
    uint32_t s = trap_arg(0);
    trap_return((uint32_t)text_width(gm_ptr(s + 1, 255), gm_r8(s)) & 0xFFFF);
}

static void h_text_width(void) {
    int first = (int16_t)trap_arg(1), count = (int16_t)trap_arg(2);
    trap_return((uint32_t)text_width(text_at(trap_arg(0), first, count), count) & 0xFFFF);
}

static void h_char_width(void) {
    uint8_t c = (uint8_t)trap_arg(0);
    trap_return((uint32_t)text_width(&c, 1) & 0xFFFF);
}

/* ---- metrics ---- */

typedef struct {
    int ascent, descent, wid_max, leading;
} metrics;

static metrics metrics_of(const text_state *s) {
    CTFontRef f = font_of(s);
    metrics m = {(int)ceil(CTFontGetAscent(f)), (int)ceil(CTFontGetDescent(f)), 0,
                 (int)lround(CTFontGetLeading(f))};
    UniChar wide[2] = {'W', 'M'};
    CGGlyph g[2];
    CGSize adv[2];
    CTFontGetGlyphsForCharacters(f, wide, g, 2);
    CTFontGetAdvancesForGlyphs(f, kCTFontOrientationHorizontal, g, adv, 2);
    for (int i = 0; i < 2; i++) {
        int a = (int)lround(adv[i].width) + face_extra(s->face);
        if (a > m.wid_max)
            m.wid_max = a;
    }
    return m;
}

/* GetFontInfo(FontInfo *): ascent, descent, widMax, leading. */
static void h_get_font_info(void) {
    text_state s = state_of(port());
    metrics m = metrics_of(&s);
    uint32_t out = trap_arg(0);
    gm_w16(out, (uint16_t)m.ascent);
    gm_w16(out + 2, (uint16_t)m.descent);
    gm_w16(out + 4, (uint16_t)m.wid_max);
    gm_w16(out + 6, (uint16_t)m.leading);
}

/* FMSwapFont(const FMInput *inRec) -> FMOutput *: the metrics of the font,
   size and face asked for, in a record the shim keeps. No font handle. */
static void h_fm_swap_font(void) {
    uint32_t in = trap_arg(0);
    text_state s = {(int16_t)gm_r16(in), (int16_t)gm_r16(in + 2), gm_r8(in + 4), 0, 0};
    if (s.size <= 0)
        s.size = SYSTEM_SIZE;
    metrics m = metrics_of(&s);
    if (!T.fm_output && !(T.fm_output = mm_new_ptr(FMOUTPUT_SIZE, true)))
        trap_crash("FMSwapFont: out of guest memory");
    uint32_t out = T.fm_output;
    memset(gm_ptr(out, FMOUTPUT_SIZE), 0, FMOUTPUT_SIZE);
    gm_w8(out + 6, (s.face & FACE_BOLD) ? 1 : 0);                       /* boldPixels */
    gm_w8(out + 7, (s.face & FACE_ITALIC) ? (uint8_t)((m.ascent + 3) / 4) : 0); /* italicPixels */
    gm_w8(out + 8, 1);                                                  /* ulOffset */
    gm_w8(out + 10, 1);                                                 /* ulThick */
    gm_w8(out + 12, (uint8_t)face_extra(s.face));                       /* extra */
    gm_w8(out + 13, (uint8_t)m.ascent);
    gm_w8(out + 14, (uint8_t)m.descent);
    gm_w8(out + 15, (uint8_t)m.wid_max);
    gm_w8(out + 16, (uint8_t)m.leading);
    gm_w8(out + 17, s.face);
    for (int k = 0; k < 4; k++) {
        gm_w16(out + 18 + 2 * (uint32_t)k, 1); /* numer and denom: 1:1 */
    }
    trap_return(out);
}

/* ---- the port's text state ---- */

static void h_text_font(void) { gm_w16(port() + PORT_TX_FONT, (uint16_t)trap_arg(0)); }
static void h_text_size(void) { gm_w16(port() + PORT_TX_SIZE, (uint16_t)trap_arg(0)); }
static void h_text_face(void) { gm_w8(port() + PORT_TX_FACE, (uint8_t)trap_arg(0)); }
static void h_text_mode(void) { gm_w16(port() + PORT_TX_MODE, (uint16_t)trap_arg(0)); }
static void h_get_port_text_font(void) { trap_return(gm_r16(trap_arg(0) + PORT_TX_FONT)); }
static void h_get_port_text_size(void) { trap_return(gm_r16(trap_arg(0) + PORT_TX_SIZE)); }
static void h_get_port_text_face(void) { trap_return(gm_r8(trap_arg(0) + PORT_TX_FACE)); }
static void h_get_port_text_mode(void) { trap_return(gm_r16(trap_arg(0) + PORT_TX_MODE)); }

/* GetFNum(ConstStr255Param name, short *familyID): 0 for a name it doesn't know. */
static void h_get_fnum(void) {
    char name[256];
    gm_read_pstr(trap_arg(0), name);
    int16_t id = 0;
    for (size_t i = 0; i < NFONTS; i++)
        if (strcasecmp(fonts[i].mac, name) == 0)
            id = fonts[i].id;
    gm_w16(trap_arg(1), (uint16_t)id);
}

static void h_get_app_font(void) { trap_return(APPL_FONT); }
static void h_get_sys_font(void) { trap_return(SYSTEM_FONT); }

/* ---- the Text Utilities ---- */

/* The comparisons fold ASCII letters only. */
static int compare(const uint8_t *a, int alen, const uint8_t *b, int blen, bool fold) {
    int n = alen < blen ? alen : blen;
    for (int i = 0; i < n; i++) {
        int x = fold && a[i] < 0x80 ? tolower(a[i]) : a[i], y = fold && b[i] < 0x80 ? tolower(b[i]) : b[i];
        if (x != y)
            return x < y ? -1 : 1;
    }
    return alen == blen ? 0 : alen < blen ? -1 : 1;
}

static const uint8_t *pstr(uint32_t p, int *len) {
    *len = gm_r8(p);
    return gm_ptr(p + 1, 255);
}

/* EqualString(str1, str2, Boolean caseSensitive, Boolean diacSensitive) */
static void h_equal_string(void) {
    int alen, blen;
    const uint8_t *a = pstr(trap_arg(0), &alen), *b = pstr(trap_arg(1), &blen);
    trap_return(compare(a, alen, b, blen, !(trap_arg(2) & 0xFF)) == 0);
}

/* IdenticalString(str1, str2, Handle itl4): 0 when they are the same. */
static void h_identical_string(void) {
    int alen, blen;
    const uint8_t *a = pstr(trap_arg(0), &alen), *b = pstr(trap_arg(1), &blen);
    trap_return(compare(a, alen, b, blen, false) != 0);
}

/* IdenticalText(aPtr, bPtr, aLen, bLen, itl2) */
static void h_identical_text(void) {
    int alen = (int16_t)trap_arg(2), blen = (int16_t)trap_arg(3);
    trap_return(compare(text_at(trap_arg(0), 0, alen), alen, text_at(trap_arg(1), 0, blen), blen, false) != 0);
}

/* CompareText(aPtr, bPtr, aLen, bLen, itl2) -> -1, 0 or 1: without case
   first, and then with it. */
static void h_compare_text(void) {
    int alen = (int16_t)trap_arg(2), blen = (int16_t)trap_arg(3);
    const uint8_t *a = text_at(trap_arg(0), 0, alen), *b = text_at(trap_arg(1), 0, blen);
    int c = compare(a, alen, b, blen, true);
    trap_return((uint32_t)(int32_t)(c ? c : compare(a, alen, b, blen, false)));
}

/* StringToNum(ConstStr255Param, long *): an optional sign and digits. */
static void h_string_to_num(void) {
    int len;
    const uint8_t *s = pstr(trap_arg(0), &len);
    int i = 0;
    bool minus = len > 0 && s[0] == '-';
    if (len > 0 && (s[0] == '-' || s[0] == '+'))
        i++;
    uint32_t v = 0;
    for (; i < len && isdigit(s[i]); i++)
        v = v * 10 + (uint32_t)(s[i] - '0');
    gm_w32(trap_arg(1), minus ? 0u - v : v);
}

void text_register(void) {
    trap_register("TextFont", h_text_font);
    trap_register("TextSize", h_text_size);
    trap_register("TextFace", h_text_face);
    trap_register("TextMode", h_text_mode);
    trap_register("GetPortTextFont", h_get_port_text_font);
    trap_register("GetPortTextSize", h_get_port_text_size);
    trap_register("GetPortTextFace", h_get_port_text_face);
    trap_register("GetPortTextMode", h_get_port_text_mode);
    trap_register("GetFNum", h_get_fnum);
    trap_register("GetAppFont", h_get_app_font);
    trap_register("GetSysFont", h_get_sys_font);
    trap_register("GetFontInfo", h_get_font_info);
    trap_register("FMSwapFont", h_fm_swap_font);
    trap_register("DrawString", h_draw_string);
    trap_register("DrawText", h_draw_text);
    trap_register("DrawChar", h_draw_char);
    trap_register("StringWidth", h_string_width);
    trap_register("TextWidth", h_text_width);
    trap_register("CharWidth", h_char_width);
    trap_register("EqualString", h_equal_string);
    trap_register("IdenticalString", h_identical_string);
    trap_register("IdenticalText", h_identical_text);
    trap_register("CompareText", h_compare_text);
    trap_register("StringToNum", h_string_to_num);
}
