#include "cgimage.h"

#include <ImageIO/ImageIO.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cf.h"
#include "trap.h"
#include "util.h"

#define MAX_OBJECTS 32

typedef struct {
    int refs; /* 0 = free slot */
    bool is_image;
    char *path;           /* providers */
    cgimage_pixels px;    /* images */
} cg_obj;

static struct {
    cg_obj objs[MAX_OBJECTS];
} G;

bool cgimage_decode_png(const char *path, cgimage_pixels *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path,
                                                           (CFIndex)strlen(path), false);
    CGImageSourceRef src = url ? CGImageSourceCreateWithURL(url, NULL) : NULL;
    CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, NULL) : NULL;
    if (url)
        CFRelease(url);
    if (src)
        CFRelease(src);
    if (!img) {
        snprintf(err, errlen, "can't decode %s as an image", path);
        return false;
    }
    int w = (int)CGImageGetWidth(img), h = (int)CGImageGetHeight(img);
    uint8_t *rgba = calloc((size_t)w * (size_t)h, 4);
    out->xrgb = malloc((size_t)w * (size_t)h * 4);
    if (!rgba || !out->xrgb)
        fatal("out of memory");
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(rgba, (size_t)w, (size_t)h, 8, (size_t)w * 4, cs,
                                             kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) {
        CGImageRelease(img);
        free(rgba);
        free(out->xrgb);
        out->xrgb = NULL;
        snprintf(err, errlen, "can't make a bitmap for %s (%dx%d)", path, w, h);
        return false;
    }
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
    CGContextRelease(ctx);
    CGImageRelease(img);
    /* Premultiplied color over white: c + (255 - alpha). */
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        const uint8_t *s = rgba + 4 * i;
        uint8_t *d = out->xrgb + 4 * i;
        int under = 255 - s[3];
        d[0] = 0;
        d[1] = (uint8_t)(s[0] + under);
        d[2] = (uint8_t)(s[1] + under);
        d[3] = (uint8_t)(s[2] + under);
    }
    free(rgba);
    out->width = w;
    out->height = h;
    return true;
}

static void free_obj(cg_obj *o) {
    free(o->path);
    free(o->px.xrgb);
    memset(o, 0, sizeof *o);
}

void cgimage_init(void) {
    for (int i = 0; i < MAX_OBJECTS; i++)
        if (G.objs[i].refs)
            free_obj(&G.objs[i]);
}

static uint32_t ref_of(int i) { return CGIMAGE_TAG_BASE + 16u * (uint32_t)i; }

static cg_obj *lookup(uint32_t ref) {
    uint32_t i = (ref - CGIMAGE_TAG_BASE) / 16u;
    if (ref < CGIMAGE_TAG_BASE || (ref & 15u) || i >= MAX_OBJECTS || !G.objs[i].refs)
        return NULL;
    return &G.objs[i];
}

static cg_obj *new_obj(const char *call) {
    for (int i = 0; i < MAX_OBJECTS; i++)
        if (!G.objs[i].refs) {
            G.objs[i].refs = 1;
            return &G.objs[i];
        }
    trap_crash("%s: more than %d CoreGraphics objects", call, MAX_OBJECTS);
}

const cgimage_pixels *cgimage_get(uint32_t ref) {
    cg_obj *o = lookup(ref);
    return o && o->is_image ? &o->px : NULL;
}

void cgimage_retain(uint32_t ref) {
    cg_obj *o = lookup(ref);
    if (o)
        o->refs++;
}

void cgimage_release(uint32_t ref) {
    cg_obj *o = lookup(ref);
    if (o && --o->refs == 0)
        free_obj(o);
}

/* CGDataProviderCreateWithURL(CFURLRef) -> CGDataProviderRef */
static void h_data_provider_create_with_url(void) {
    const char *path = cf_url_path("CGDataProviderCreateWithURL", trap_arg(0));
    cg_obj *o = new_obj("CGDataProviderCreateWithURL");
    o->path = strdup(path);
    if (!o->path)
        fatal("out of memory");
    trap_return(ref_of((int)(o - G.objs)));
}

/* CGImageCreateWithPNGDataProvider(provider, decode, shouldInterpolate,
   intent) -> CGImageRef, or NULL if the file isn't a PNG it can read. */
static void h_image_create_with_png_data_provider(void) {
    cg_obj *p = lookup(trap_arg(0));
    if (!p || p->is_image)
        trap_crash("CGImageCreateWithPNGDataProvider: 0x%08x is not a data provider", trap_arg(0));
    if (trap_arg(1))
        trap_crash("CGImageCreateWithPNGDataProvider: decode arrays are not supported");
    cgimage_pixels px;
    char err[512];
    if (!cgimage_decode_png(p->path, &px, err, sizeof err)) {
        log_msg("CGImageCreateWithPNGDataProvider: %s", err);
        trap_return(0);
        return;
    }
    cg_obj *o = new_obj("CGImageCreateWithPNGDataProvider");
    o->is_image = true;
    o->px = px;
    trap_return(ref_of((int)(o - G.objs)));
}

static void h_release(void) {
    uint32_t ref = trap_arg(0);
    if (!ref)
        return;
    if (!lookup(ref))
        trap_crash("CGImageRelease / CGDataProviderRelease: 0x%08x is not a live object", ref);
    cgimage_release(ref);
}

void cgimage_register(void) {
    trap_register("CGDataProviderCreateWithURL", h_data_provider_create_with_url);
    trap_register("CGDataProviderRelease", h_release);
    trap_register("CGImageCreateWithPNGDataProvider", h_image_create_with_png_data_provider);
    trap_register("CGImageRelease", h_release);
}
