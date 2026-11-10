#include "scorecard.h"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <ImageIO/ImageIO.h>
#include <ctype.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cf.h"
#include "font.h"
#include "util.h"

/* Share Tech Mono (fonts/ShareTechMono-Regular.ttf), which the build turns
   into a list of bytes. */
static const unsigned char font_ttf[] = {
#include "scorecard_font.inc"
};

#define LEAN 0.1405 /* tan(8 degrees) */
#define CAP_ROWS 7  /* a capital of the bitmap font is this many of its 8 rows tall */
#define MAX_GLYPHS 64

typedef struct {
    double r, g, b;
} rgb;
static const rgb ORANGE = {1.0, 0.353, 0.122}; /* #ff5a1f */
static const rgb GOLD = {1.0, 0.69, 0.0};      /* #ffb000 */
static const rgb GREEN = {0.373, 0.941, 0.784}; /* #5ff0c8 */
static const rgb GRAY = {0.604, 0.604, 0.604};  /* #9a9a9a */
static const rgb WHITE = {1.0, 1.0, 1.0};

/* Where things go in a shape, in pixels from the top left. */
typedef struct {
    int w, h;
    int table_x, table_y, table_w, table_h; /* the playfield, before clipping */
    int clip_w, clip_h;                     /* what shows of it, from the top left corner */
    int left, right;                        /* the margins everything else is set between */
    int name_x;
    int title_y, title_k; /* the bitmap font's lines: top, and pixels a font pixel */
    int sub_k;
    bool sub_beside; /* "HIGH SCORES" at the title's right, not under it with the date */
    int foot_y, foot_k;
    double size, big_size, glow;
    int base[GAME_HIGH_SCORES + 1]; /* baselines: the best's name, its score, the others */
} layout;

static const layout TALL = {
    .w = 1080, .h = 1350,
    .table_x = 0, .table_y = -648, .table_w = 1080, .table_h = 1646,
    .clip_w = 1080, .clip_h = 600,
    .left = 72, .right = 1008, .name_x = 134,
    .title_y = 652, .title_k = 5, .sub_k = 3, .sub_beside = true,
    .foot_y = 1286, .foot_k = 2,
    .size = 42, .big_size = 150, .glow = 22,
    .base = {806, 950, 1077, 1140, 1203},
};
static const layout WIDE = {
    .w = 1200, .h = 675,
    .table_x = 0, .table_y = 0, .table_w = 443, .table_h = 675,
    .clip_w = 443, .clip_h = 675,
    .left = 506, .right = 1136, .name_x = 552,
    .title_y = 52, .title_k = 4, .sub_k = 2, .sub_beside = false,
    .foot_y = 615, .foot_k = 2,
    .size = 30, .big_size = 92, .glow = 18,
    .base = {242, 332, 423, 468, 513},
};

static CGColorRef make_color(CGColorSpaceRef space, rgb c, double alpha) {
    CGFloat v[4] = {c.r, c.g, c.b, alpha};
    return CGColorCreate(space, v);
}

/* The playfield, cut from its picture and scaled into its place. */
static void draw_table(CGContextRef cg, const scorecard *c, const layout *l) {
    if (!c->table)
        return;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)c->table,
                                                           (CFIndex)strlen(c->table), false);
    CGImageSourceRef source = url ? CGImageSourceCreateWithURL(url, NULL) : NULL;
    CGImageRef art = source ? CGImageSourceCreateImageAtIndex(source, 0, NULL) : NULL;
    CGImageRef table = NULL;
    if (art) { /* at any scale: SCORECARD_ART_W of the game's pixels across */
        double k = (double)CGImageGetWidth(art) / SCORECARD_ART_W;
        table = CGImageCreateWithImageInRect(
            art, CGRectMake(0, SCORECARD_ART_TOP * k, SCORECARD_ART_W * k, SCORECARD_ART_ROWS * k));
    }
    if (table) {
        CGContextSaveGState(cg);
        CGContextClipToRect(cg, CGRectMake(0, 0, l->clip_w, l->clip_h));
        CGContextSetInterpolationQuality(cg, kCGInterpolationHigh);
        CGContextTranslateCTM(cg, l->table_x, l->table_y + l->table_h);
        CGContextScaleCTM(cg, 1, -1);
        CGContextDrawImage(cg, CGRectMake(0, 0, l->table_w, l->table_h), table);
        CGContextRestoreGState(cg);
    }
    CGImageRelease(table);
    CGImageRelease(art);
    if (source)
        CFRelease(source);
    if (url)
        CFRelease(url);
}

/* The bitmap font's text: how many font pixels are blank before its first
   character's ink and after its last one's. */
static void bitmap_pad(const char *s, int *before, int *after) {
    *before = *after = 0;
    size_t n = strlen(s);
    if (!n)
        return;
    uint8_t first = 0, last = 0;
    for (int row = 0; row < FONT_H; row++) {
        first |= font_glyph((uint8_t)s[0])[row];
        last |= font_glyph((uint8_t)s[n - 1])[row];
    }
    while (first && *before < FONT_W && !(first & (0x80 >> *before)))
        (*before)++;
    while (last && *after < FONT_W && !(last & (1 << *after)))
        (*after)++;
}

/* Draws s in the bitmap font, k pixels a font pixel, its top at y: its ink
   starting at x, or with right set, ending there. */
static void bitmap_text(CGContextRef cg, CGColorRef color, int x, int y, const char *s, int k, bool right) {
    int before, after;
    bitmap_pad(s, &before, &after);
    int n = (int)strlen(s);
    x = right ? x - (n * FONT_W - after) * k : x - before * k;
    CGContextSetFillColorWithColor(cg, color);
    for (int i = 0; i < n; i++) {
        const uint8_t *g = font_glyph((uint8_t)s[i]);
        for (int row = 0; row < FONT_H; row++)
            for (int col = 0; col < FONT_W; col++)
                if (g[row] & (0x80 >> col))
                    CGContextFillRect(cg, CGRectMake(x + (i * FONT_W + col) * k, y + row * k, k, k));
    }
}

/* A line of n bytes of text (Mac Roman, which ASCII is part of). NULL if
   it can't be made. */
static CTLineRef make_line(CTFontRef font, CGColorRef color, const char *text, size_t n, double kern) {
    CFStringRef s = CFStringCreateWithBytes(NULL, (const UInt8 *)text, (CFIndex)n, kCFStringEncodingMacRoman, false);
    CFNumberRef k = CFNumberCreate(NULL, kCFNumberDoubleType, &kern);
    CTLineRef line = NULL;
    if (s && k) {
        const void *keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName, kCTKernAttributeName};
        const void *vals[] = {font, color, k};
        CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, vals, 3, &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
        CFAttributedStringRef as = attrs ? CFAttributedStringCreate(NULL, s, attrs) : NULL;
        if (as)
            line = CTLineCreateWithAttributedString(as);
        if (as)
            CFRelease(as);
        if (attrs)
            CFRelease(attrs);
    }
    if (k)
        CFRelease(k);
    if (s)
        CFRelease(s);
    return line;
}

static CGAffineTransform lean(void) { return CGAffineTransformMake(1, 0, LEAN, 1, 0, 0); }

/* Where the leaning line's ink starts and ends, from its origin: the
   outlines' own extent, not the characters' boxes. False for a line with
   no ink. */
static bool line_ink(CTLineRef line, double *left, double *right) {
    CGRect all = CGRectNull;
    CFArrayRef runs = CTLineGetGlyphRuns(line);
    for (CFIndex r = 0; r < CFArrayGetCount(runs); r++) {
        CTRunRef run = CFArrayGetValueAtIndex(runs, r);
        CTFontRef font = CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName);
        CFIndex n = CTRunGetGlyphCount(run);
        if (!font)
            continue;
        if (n > MAX_GLYPHS)
            n = MAX_GLYPHS;
        CGGlyph glyphs[MAX_GLYPHS];
        CGPoint at[MAX_GLYPHS];
        CTRunGetGlyphs(run, CFRangeMake(0, n), glyphs);
        CTRunGetPositions(run, CFRangeMake(0, n), at);
        for (CFIndex i = 0; i < n; i++) {
            CGAffineTransform m = CGAffineTransformConcat(CGAffineTransformMakeTranslation(at[i].x, at[i].y), lean());
            CGPathRef path = CTFontCreatePathForGlyph(font, glyphs[i], &m);
            if (!path)
                continue; /* a space */
            CGRect box = CGPathGetPathBoundingBox(path);
            if (!CGRectIsEmpty(box))
                all = CGRectUnion(all, box);
            CGPathRelease(path);
        }
    }
    if (CGRectIsNull(all))
        return false;
    *left = CGRectGetMinX(all);
    *right = CGRectGetMaxX(all);
    return true;
}

/* Draws the line leaning, its ink starting at x, or with right set, ending
   there, on the baseline at y. */
static void draw_line(CGContextRef cg, CTLineRef line, double x, double y, bool right) {
    double l, r;
    if (!line_ink(line, &l, &r))
        return;
    CGContextSaveGState(cg);
    CGContextTranslateCTM(cg, right ? x - r : x - l, y);
    CGContextScaleCTM(cg, 1, -1); /* text is drawn y up */
    CGContextConcatCTM(cg, lean());
    CGContextSetTextPosition(cg, 0, 0);
    CTLineDraw(line, cg);
    CGContextRestoreGState(cg);
}

static double ink_width(CTLineRef line) {
    double l, r;
    return line && line_ink(line, &l, &r) ? r - l : 0;
}

/* "430,745,960" */
static size_t format_score(int64_t score, char *out, size_t cap) {
    char digits[32];
    int n = snprintf(digits, sizeof digits, "%lld", (long long)(score < 0 ? 0 : score));
    size_t o = 0;
    for (int i = 0; i < n && o + 2 < cap; i++) {
        if (i && (n - i) % 3 == 0)
            out[o++] = ',';
        out[o++] = digits[i];
    }
    out[o] = '\0';
    return o;
}

/* "NOV 10, 2026" */
static void format_date(time_t when, char *out, size_t cap) {
    static const char *const months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                         "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    struct tm tm;
    if (!localtime_r(&when, &tm)) {
        out[0] = '\0';
        return;
    }
    snprintf(out, cap, "%s %d, %d", months[tm.tm_mon % 12], tm.tm_mday, tm.tm_year + 1900);
}

/* Share Tech Mono at size, or Menlo if the built-in font can't be read. */
static CTFontRef score_font(double size) {
    CTFontRef font = NULL;
    CGDataProviderRef data = CGDataProviderCreateWithData(NULL, font_ttf, sizeof font_ttf, NULL);
    CGFontRef cg = data ? CGFontCreateWithDataProvider(data) : NULL;
    if (cg)
        font = CTFontCreateWithGraphicsFont(cg, size, NULL, NULL);
    CGFontRelease(cg);
    CGDataProviderRelease(data);
    return font ? font : CTFontCreateWithName(CFSTR("Menlo-Regular"), size, NULL);
}

/* A name, at x: as many of its characters as end gap or more before limit. */
static void draw_name(CGContextRef cg, CTFontRef font, CGColorRef color, const char *name, double x,
                      double y, double limit, double gap) {
    for (size_t n = strlen(name); n > 0; n--) {
        CTLineRef line = make_line(font, color, name, n, 4);
        if (!line)
            return;
        bool fits = x + ink_width(line) + gap <= limit;
        if (fits)
            draw_line(cg, line, x, y, false);
        CFRelease(line);
        if (fits)
            return;
    }
}

static void draw_scores(CGContextRef cg, CGColorSpaceRef space, const scorecard *c, const layout *l) {
    CTFontRef font = score_font(l->size);
    CGColorRef green = make_color(space, GREEN, 1), gray = make_color(space, GRAY, 1),
               white = make_color(space, WHITE, 1), glow = make_color(space, GREEN, 0.55);
    double room = l->right - l->left;
    int n = c->n > GAME_HIGH_SCORES ? GAME_HIGH_SCORES : c->n;
    for (int i = 0; i < n && font; i++) {
        char rank[8], score[40];
        snprintf(rank, sizeof rank, "%d", i + 1);
        size_t len = format_score(c->scores[i].score, score, sizeof score);
        int y = l->base[i ? i + 1 : 0];
        CTLineRef r = make_line(font, i ? gray : green, rank, strlen(rank), 0);
        if (r) {
            draw_line(cg, r, l->left, y, false);
            CFRelease(r);
        }
        if (i == 0) { /* the best: its score on the next line, as large as fits */
            draw_name(cg, font, green, c->scores[i].name, l->name_x, y, l->right, 0);
            double size = l->big_size;
            CTFontRef big = CTFontCreateCopyWithAttributes(font, size, NULL, NULL);
            CTLineRef s = big ? make_line(big, green, score, len, 0) : NULL;
            double w = ink_width(s);
            if (w > room) {
                if (s)
                    CFRelease(s);
                if (big)
                    CFRelease(big);
                big = CTFontCreateCopyWithAttributes(font, size * room / w * 0.995, NULL, NULL);
                s = big ? make_line(big, green, score, len, 0) : NULL;
            }
            if (s) {
                CGContextSaveGState(cg);
                CGContextSetShadowWithColor(cg, CGSizeZero, l->glow, glow);
                draw_line(cg, s, l->right, l->base[1], true);
                CGContextRestoreGState(cg);
                CFRelease(s);
            }
            if (big)
                CFRelease(big);
            continue;
        }
        CTLineRef s = make_line(font, white, score, len, 0);
        double w = ink_width(s);
        if (s) {
            draw_line(cg, s, l->right, y, true);
            CFRelease(s);
        }
        draw_name(cg, font, white, c->scores[i].name, l->name_x, y, l->right - w, l->size);
    }
    CGColorRelease(green);
    CGColorRelease(gray);
    CGColorRelease(white);
    CGColorRelease(glow);
    if (font)
        CFRelease(font);
}

/* The title, "HIGH SCORES", the link and the date, in the bitmap font. */
static void draw_labels(CGContextRef cg, CGColorSpaceRef space, const scorecard *c, const layout *l) {
    font_init();
    CGColorRef orange = make_color(space, ORANGE, 1), gold = make_color(space, GOLD, 1),
               gray = make_color(space, GRAY, 1);
    char title[64], date[32];
    size_t n = 0;
    for (const char *p = c->title ? c->title : ""; *p && n + 1 < sizeof title; p++)
        title[n++] = (char)toupper((unsigned char)*p);
    title[n] = '\0';
    format_date(c->when, date, sizeof date);
    bitmap_text(cg, orange, l->left, l->title_y, title, l->title_k, false);
    if (l->sub_beside) { /* on the title's baseline */
        int y = l->title_y + CAP_ROWS * (l->title_k - l->sub_k);
        bitmap_text(cg, gold, l->right, y, "HIGH SCORES", l->sub_k, true);
        bitmap_text(cg, gray, l->right, l->foot_y, date, l->foot_k, true);
    } else {
        int y = l->title_y + FONT_H * l->title_k + 6 * l->sub_k;
        bitmap_text(cg, gold, l->left, y, "HIGH SCORES", l->sub_k, false);
        bitmap_text(cg, gray, l->right, y, date, l->sub_k, true);
    }
    bitmap_text(cg, gold, l->left, l->foot_y, SCORECARD_LINK, l->foot_k, false);
    CGColorRelease(orange);
    CGColorRelease(gold);
    CGColorRelease(gray);
}

uint8_t *scorecard_render(const scorecard *c, scorecard_shape shape, int *w, int *h) {
    const layout *l = shape == SCORECARD_WIDE ? &WIDE : &TALL;
    if (c->n < 1 || !c->scores)
        return NULL;
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef cg = space ? CGBitmapContextCreate(NULL, (size_t)l->w, (size_t)l->h, 8, (size_t)l->w * 4, space,
                                                    (CGBitmapInfo)kCGImageAlphaPremultipliedLast)
                            : NULL;
    uint8_t *out = NULL;
    if (cg) {
        /* y down, from the top left, as the layout counts */
        CGContextTranslateCTM(cg, 0, l->h);
        CGContextScaleCTM(cg, 1, -1);
        CGContextSetTextMatrix(cg, CGAffineTransformIdentity);
        CGContextSetRGBFillColor(cg, 0, 0, 0, 1);
        CGContextFillRect(cg, CGRectMake(0, 0, l->w, l->h));
        draw_table(cg, c, l);
        draw_labels(cg, space, c, l);
        draw_scores(cg, space, c, l);
        size_t len = (size_t)l->w * (size_t)l->h * 4;
        out = malloc(len);
        if (!out)
            fatal("out of memory");
        memcpy(out, CGBitmapContextGetData(cg), len);
        *w = l->w;
        *h = l->h;
    }
    CGContextRelease(cg);
    CGColorSpaceRelease(space);
    return out;
}

bool scorecard_write(const scorecard *c, scorecard_shape shape, const char *path) {
    int w, h;
    uint8_t *rgba = scorecard_render(c, shape, &w, &h);
    if (!rgba)
        return false;
    bool ok = false;
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef data = CGDataProviderCreateWithData(NULL, rgba, (size_t)w * (size_t)h * 4, NULL);
    CGImageRef image = space && data ? CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, space,
                                                     (CGBitmapInfo)kCGImageAlphaNoneSkipLast, data, NULL, false,
                                                     kCGRenderingIntentDefault)
                                     : NULL;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef dest = url ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
    if (image && dest) {
        CGImageDestinationAddImage(dest, image, NULL);
        ok = CGImageDestinationFinalize(dest);
    }
    if (dest)
        CFRelease(dest);
    if (url)
        CFRelease(url);
    CGImageRelease(image);
    CGDataProviderRelease(data);
    CGColorSpaceRelease(space);
    free(rgba);
    return ok;
}

static struct {
    char title[64];
    char table[PATH_MAX]; /* the playfield's picture, or empty */
    const char *tall, *wide;
} S;

bool scorecard_share(scorecard_shape shape, const char *path, char *err, size_t errlen) {
    game_high_score scores[GAME_HIGH_SCORES];
    int n = game_high_scores(cf_pref_string, cf_pref_int, scores);
    if (n == 0) {
        snprintf(err, errlen, "There are no high scores yet.");
        return false;
    }
    scorecard c = {.title = S.title, .scores = scores, .n = n, .table = S.table[0] ? S.table : NULL,
                   .when = time(NULL)};
    if (!scorecard_write(&c, shape, path)) {
        snprintf(err, errlen, "The picture can't be written to %s.", path);
        return false;
    }
    return true;
}

static void write_at_exit(void) {
    char err[PATH_MAX + 64];
    if (S.tall && !scorecard_share(SCORECARD_TALL, S.tall, err, sizeof err))
        log_msg("LOONY_SCORECARD: %s", err);
    if (S.wide && !scorecard_share(SCORECARD_WIDE, S.wide, err, sizeof err))
        log_msg("LOONY_SCORECARD_WIDE: %s", err);
}

/* The folder of this program. */
static bool program_dir(char *out, size_t cap) {
    uint32_t size = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &size) != 0)
        return false;
    char *slash = strrchr(out, '/');
    if (!slash)
        return false;
    *slash = '\0';
    return true;
}

void scorecard_init(const char *game_id, const char *title) {
    snprintf(S.title, sizeof S.title, "%s", title);
    /* in the app, beside the binary's folder; from a build folder, in the repo */
    static const char *const places[] = {"../Resources/" SCORECARD_ART_DIR, "../" SCORECARD_ART_DIR};
    char dir[PATH_MAX];
    S.table[0] = '\0';
    for (size_t i = 0; i < sizeof places / sizeof places[0] && program_dir(dir, sizeof dir); i++) {
        char path[PATH_MAX + 128];
        snprintf(path, sizeof path, "%s/%s/%s.png", dir, places[i], game_id);
        if (strlen(path) < sizeof S.table && access(path, R_OK) == 0) {
            snprintf(S.table, sizeof S.table, "%s", path);
            break;
        }
    }
    if (!S.table[0])
        log_msg("scorecard: no picture of the table (%s/%s.png) beside the program", SCORECARD_ART_DIR, game_id);
    const char *tall = getenv("LOONY_SCORECARD"), *wide = getenv("LOONY_SCORECARD_WIDE");
    S.tall = tall && *tall ? tall : NULL;
    S.wide = wide && *wide ? wide : NULL;
    if (S.tall || S.wide)
        atexit(write_at_exit);
}
