#include "hd.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "pict.h"
#include "png.h"
#include "util.h"

/* The HD copy of one 1x buffer. ref holds the 1x pixel values the HD pixels
   show; a 1x pixel that differs from its ref changed behind our back and is
   redrawn as a block. A new copy starts with ref the complement of the 1x
   pixels, so each pixel is drawn the first time it's needed. */
typedef struct {
    int16_t left, right; /* 1x columns from bounds.left; none when left >= right */
} span;

typedef struct {
    const uint8_t *base;
    qd_rect bounds;
    int depth;
    uint32_t row_bytes;
    uint8_t *ref;
    uint8_t *rgba; /* (w * N) x (h * N) RGBA */
    span *changed; /* per 1x row: the columns whose HD pixels changed since hd_take_changes */
    bool sprite;   /* small, with art: see match_sprite */
    /* A larger buffer with art: its 1x pixels just after the art was drawn,
       and the art (an index into H.arts) and where it went, so pixels the
       game puts back (a lamp going out) get the art back. */
    uint8_t *orig;
    int orig_art;
    qd_rect orig_dst, orig_area;
} twin;

typedef struct {
    uint32_t hash;
    uint8_t *rgba; /* NULL: the picture has no art */
    int w, h;
    bool opaque;
} art;

static struct {
    int scale;
    char art_dir[1024], dump_dir[1024];
    twin **twins;
    int ntwins, twin_cap;
    art *arts;
    int narts, art_cap;
    uint32_t *dumped;
    int ndumped, dump_cap;
} H;

static void *grow(void *p, int *cap, size_t size) {
    *cap = *cap ? *cap * 2 : 16;
    p = realloc(p, (size_t)*cap * size);
    if (!p)
        fatal("out of memory");
    return p;
}

static void free_twin(twin *t) {
    free(t->ref);
    free(t->rgba);
    free(t->orig);
    free(t->changed);
    free(t);
}

void hd_configure(int scale, const char *art_dir, const char *dump_dir) {
    for (int i = 0; i < H.ntwins; i++)
        free_twin(H.twins[i]);
    for (int i = 0; i < H.narts; i++)
        free(H.arts[i].rgba);
    free(H.twins);
    free(H.arts);
    free(H.dumped);
    memset(&H, 0, sizeof H);
    H.scale = scale;
    snprintf(H.art_dir, sizeof H.art_dir, "%s", art_dir ? art_dir : "");
    snprintf(H.dump_dir, sizeof H.dump_dir, "%s", dump_dir ? dump_dir : "");
}

bool hd_bundled_art(const char *exe_path, const char *game_id, char *out, size_t cap) {
    const char *macos = exe_path ? strstr(exe_path, ".app/Contents/MacOS/") : NULL;
    if (!macos)
        return false;
    int n = snprintf(out, cap, "%.*s.app/Contents/Resources/hd-art/%s", (int)(macos - exe_path),
                     exe_path, game_id);
    struct stat st;
    return n > 0 && (size_t)n < cap && stat(out, &st) == 0 && S_ISDIR(st.st_mode);
}

void hd_init(const char *game_id, const char *exe_path) {
    const char *dir = getenv("LOONY_HD"), *sc = getenv("LOONY_HD_SCALE");
    const char *dump = getenv("LOONY_HD_DUMP");
    char bundled[1024];
    if (dir && strcmp(dir, "off") == 0)
        dir = NULL;
    else if (!(dir && *dir) && hd_bundled_art(exe_path, game_id, bundled, sizeof bundled))
        dir = bundled;
    int scale = 0;
    if (dir && *dir) {
        scale = sc && *sc ? atoi(sc) : 4;
        if (scale < 2 || scale > 8) {
            log_msg("hd: LOONY_HD_SCALE=%s is not 2 to 8; using 4", sc);
            scale = 4;
        }
        log_msg("hd: on at %dx, art from %s", scale, dir);
    }
    hd_configure(scale, dir, dump);
}

int hd_scale(void) { return H.scale; }

void hd_forget(const uint8_t *base) {
    for (int i = 0; i < H.ntwins; i++)
        if (H.twins[i]->base == base) {
            free_twin(H.twins[i]);
            H.twins[i] = H.twins[--H.ntwins];
            return;
        }
}

/* Notes that t's HD pixels for the 1x rect r (inside t's bounds) changed. */
static void touch(twin *t, qd_rect r) {
    int16_t left = (int16_t)(r.left - t->bounds.left), right = (int16_t)(r.right - t->bounds.left);
    for (int y = r.top; y < r.bottom; y++) {
        span *c = &t->changed[y - t->bounds.top];
        if (left < c->left)
            c->left = left;
        if (right > c->right)
            c->right = right;
    }
}

static twin *twin_for(const qd_pixels *px) {
    for (int i = 0; i < H.ntwins; i++) {
        twin *t = H.twins[i];
        if (t->base != px->base)
            continue;
        if (t->depth == px->depth && t->row_bytes == px->row_bytes &&
            memcmp(&t->bounds, &px->bounds, sizeof t->bounds) == 0)
            return t;
        hd_forget(px->base); /* the buffer was reused without our seeing it freed */
        break;
    }
    size_t n = (size_t)px->row_bytes * (size_t)rect_h(px->bounds);
    size_t hd = (size_t)rect_w(px->bounds) * H.scale * (size_t)rect_h(px->bounds) * H.scale * 4;
    twin *t = calloc(1, sizeof *t);
    int h = rect_h(px->bounds);
    if (!t || !(t->ref = malloc(n ? n : 1)) || !(t->rgba = calloc(hd ? hd : 1, 1)) ||
        !(t->changed = malloc(sizeof *t->changed * (size_t)(h ? h : 1))))
        fatal("out of memory for a %dx%d HD copy", rect_w(px->bounds), rect_h(px->bounds));
    for (size_t i = 0; i < n; i++)
        t->ref[i] = (uint8_t)~px->base[i];
    t->base = px->base;
    t->bounds = px->bounds;
    t->depth = px->depth;
    t->row_bytes = px->row_bytes;
    for (int y = 0; y < h; y++) /* all of it: nothing has shown its pixels yet */
        t->changed[y] = (span){0, (int16_t)rect_w(px->bounds)};
    if (H.ntwins == H.twin_cap)
        H.twins = grow(H.twins, &H.twin_cap, sizeof *H.twins);
    H.twins[H.ntwins++] = t;
    return t;
}

static int hd_w(const twin *t) { return rect_w(t->bounds) * H.scale; }

static uint8_t *hd_px(const twin *t, int hx, int hy) {
    return t->rgba + ((size_t)hy * (size_t)hd_w(t) + (size_t)hx) * 4;
}

static qd_pixels ref_pixels(const twin *t, const qd_pixels *px) {
    qd_pixels r = *px;
    r.base = t->ref;
    return r;
}

/* The bytes holding pixels [left, right) of a row. */
static void byte_span(const qd_pixels *px, int left, int right, size_t *b0, size_t *b1) {
    *b0 = (size_t)(left - px->bounds.left) * (size_t)px->depth / 8;
    *b1 = ((size_t)(right - px->bounds.left) * (size_t)px->depth + 7) / 8;
}

static qd_pixels twin_pixels(const twin *t) {
    return (qd_pixels){(uint8_t *)t->base, t->row_bytes, t->bounds, t->depth, NULL};
}

/* Whether two copies of t's 1x pixels are equal, ignoring row padding. */
static bool same_pixels(const twin *t, const uint8_t *a, const uint8_t *b) {
    size_t span = ((size_t)rect_w(t->bounds) * (size_t)t->depth + 7) / 8;
    for (int y = 0; y < rect_h(t->bounds); y++)
        if (memcmp(a + (size_t)y * t->row_bytes, b + (size_t)y * t->row_bytes, span) != 0)
            return false;
    return true;
}

/* Copies the block of 1x pixel (sx, sy) of s's copy to 1x pixel (x, y) of t's. */
static void copy_block(const twin *s, int sx, int sy, const twin *t, int x, int y) {
    int n = H.scale;
    int shx = (sx - s->bounds.left) * n, shy = (sy - s->bounds.top) * n;
    int hx = (x - t->bounds.left) * n, hy = (y - t->bounds.top) * n;
    for (int j = 0; j < n; j++)
        memcpy(hd_px(t, hx, hy + j), hd_px(s, shx, shy + j), (size_t)n * 4);
}

/* Sprites: small pictures with art. The game copies their pixels into other
   buffers with its own code (lamps, flippers), so a changed patch that holds
   exactly a sprite's pixels gets that sprite's HD pixels. */
#define SPRITE_MAX 128
#define SPRITE_MIN_DIRTY 8

static struct {
    uint8_t *dirty; /* one byte per pixel of the rect being synced */
    int *stack;
    size_t cap;
} S;

/* Whether sprite s, with its top-left at (ox, oy) in px, shows every dirty
   pixel of the component and at least half of its own pixels. */
static bool sprite_fits(const qd_pixels *px, const qd_pixels *sp, int ox, int oy, qd_rect r,
                        const int *comp, int ncomp) {
    for (int i = 0; i < ncomp; i++) {
        int x = r.left + comp[i] % rect_w(r), y = r.top + comp[i] / rect_w(r);
        int sx = sp->bounds.left + x - ox, sy = sp->bounds.top + y - oy;
        if (sx < sp->bounds.left || sx >= sp->bounds.right || sy < sp->bounds.top ||
            sy >= sp->bounds.bottom || qd_get_pixel(sp, sx, sy) != qd_get_pixel(px, x, y))
            return false;
    }
    int w = rect_w(sp->bounds), h = rect_h(sp->bounds), same = 0, total = 0;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int x = ox + i, y = oy + j;
            if (x < px->bounds.left || x >= px->bounds.right || y < px->bounds.top ||
                y >= px->bounds.bottom)
                continue;
            total++;
            same += qd_get_pixel(sp, sp->bounds.left + i, sp->bounds.top + j) == qd_get_pixel(px, x, y);
        }
    return same * 2 >= total;
}

/* Finds a sprite for one component of dirty pixels (indexes into r, with
   bounding box b) and draws it. Pixels it explains are marked clean. */
static void match_sprite(twin *t, const qd_pixels *px, qd_rect r, qd_rect b, const int *comp,
                         int ncomp) {
    for (int k = 0; k < H.ntwins; k++) {
        const twin *s = H.twins[k];
        int w = rect_w(s->bounds), h = rect_h(s->bounds);
        if (!s->sprite || s == t || s->depth != px->depth || w < rect_w(b) || h < rect_h(b))
            continue;
        qd_pixels sp = twin_pixels(s);
        if (!same_pixels(s, s->ref, s->base))
            continue; /* the game changed it since its art was drawn */
        for (int oy = b.bottom - h; oy <= b.top; oy++)
            for (int ox = b.right - w; ox <= b.left; ox++) {
                if (!sprite_fits(px, &sp, ox, oy, r, comp, ncomp))
                    continue;
                qd_pixels ref = ref_pixels(t, px);
                for (int j = 0; j < h; j++)
                    for (int i = 0; i < w; i++) {
                        int x = ox + i, y = oy + j;
                        if (x < px->bounds.left || x >= px->bounds.right || y < px->bounds.top ||
                            y >= px->bounds.bottom)
                            continue;
                        uint32_t v = qd_get_pixel(px, x, y);
                        if (qd_get_pixel(&sp, s->bounds.left + i, s->bounds.top + j) != v)
                            continue;
                        copy_block(s, s->bounds.left + i, s->bounds.top + j, t, x, y);
                        touch(t, (qd_rect){(int16_t)y, (int16_t)x, (int16_t)(y + 1), (int16_t)(x + 1)});
                        qd_set_pixel(&ref, x, y, v);
                    }
                return;
            }
    }
}

/* Groups the dirty pixels of r into 8-connected components and matches each
   small enough one against the sprites. */
static void match_sprites(twin *t, const qd_pixels *px, qd_rect r) {
    int w = rect_w(r), h = rect_h(r);
    for (int start = 0; start < w * h; start++) {
        if (S.dirty[start] != 1)
            continue;
        int n = 0;
        S.stack[n++] = start;
        S.dirty[start] = 2;
        qd_rect b = {INT16_MAX, INT16_MAX, INT16_MIN, INT16_MIN};
        for (int i = 0; i < n; i++) { /* the stack doubles as the component's list */
            int x = S.stack[i] % w, y = S.stack[i] / w;
            if (r.left + x < b.left) b.left = (int16_t)(r.left + x);
            if (r.left + x >= b.right) b.right = (int16_t)(r.left + x + 1);
            if (r.top + y < b.top) b.top = (int16_t)(r.top + y);
            if (r.top + y >= b.bottom) b.bottom = (int16_t)(r.top + y + 1);
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || nx >= w || ny < 0 || ny >= h || S.dirty[ny * w + nx] != 1)
                        continue;
                    S.dirty[ny * w + nx] = 2;
                    S.stack[n++] = ny * w + nx;
                }
        }
        if (n >= SPRITE_MIN_DIRTY && rect_w(b) <= SPRITE_MAX && rect_h(b) <= SPRITE_MAX)
            match_sprite(t, px, r, b, S.stack, n);
    }
}

/* Draws art a, placed on the 1x rect dst, into t's copy over the 1x rect area
   (inside dst and t's bounds). Transparent art pixels leave what's there. */
static void draw_art(twin *t, const art *a, qd_rect dst, qd_rect area) {
    int n = H.scale, dw = rect_w(dst) * n, dh = rect_h(dst) * n;
    int x0 = area.left * n, x1 = area.right * n, tx0 = t->bounds.left * n;
    int small[64], *axs = x1 - x0 <= 64 ? small : malloc(sizeof *axs * (size_t)(x1 - x0));
    if (!axs)
        fatal("out of memory");
    for (int x = x0; x < x1; x++)
        axs[x - x0] = (int)((int64_t)(x - dst.left * n) * a->w / dw);
    for (int y = area.top * n; y < area.bottom * n; y++) {
        int ay = (int)((int64_t)(y - dst.top * n) * a->h / dh);
        const uint8_t *arow = a->rgba + (size_t)ay * (size_t)a->w * 4;
        uint8_t *q = hd_px(t, x0 - tx0, y - t->bounds.top * n);
        for (int x = x0; x < x1; x++, q += 4) {
            const uint8_t *p = arow + (size_t)axs[x - x0] * 4;
            if (p[3] < 0x80)
                continue;
            memcpy(q, p, 3);
            q[3] = 0xFF;
        }
    }
    if (axs != small)
        free(axs);
}

/* Brings the HD pixels for r up to date: changed pixels that match a sprite
   get its HD pixels, and the rest are drawn as NxN blocks. */
static void sync(twin *t, const qd_pixels *px, qd_rect r) {
    r = rect_sect(r, px->bounds);
    if (rect_empty(r))
        return;
    qd_pixels ref = ref_pixels(t, px);
    int n = H.scale, w = rect_w(r);
    size_t b0, b1;
    byte_span(px, r.left, r.right, &b0, &b1);
    size_t npx = (size_t)w * (size_t)rect_h(r);
    bool any = false;
    for (int y = r.top; y < r.bottom; y++) {
        size_t row = (size_t)(y - px->bounds.top) * px->row_bytes;
        if (memcmp(px->base + row + b0, t->ref + row + b0, b1 - b0) == 0)
            continue;
        if (!any) {
            if (S.cap < npx) {
                free(S.dirty);
                free(S.stack);
                S.cap = npx;
                if (!(S.dirty = malloc(npx)) || !(S.stack = malloc(npx * sizeof *S.stack)))
                    fatal("out of memory");
            }
            memset(S.dirty, 0, npx);
            any = true;
        }
        for (int x = r.left; x < r.right; x++)
            S.dirty[(size_t)(y - r.top) * w + (x - r.left)] =
                qd_get_pixel(px, x, y) != qd_get_pixel(&ref, x, y);
    }
    if (!any)
        return;
    if (t->orig) { /* pixels back to what the art was drawn over get the art back */
        qd_pixels orig = ref_pixels(t, px);
        orig.base = t->orig;
        qd_rect ra = rect_sect(r, t->orig_area);
        for (int y = ra.top; y < ra.bottom; y++)
            for (int x = ra.left; x < ra.right; x++) {
                uint8_t *d = &S.dirty[(size_t)(y - r.top) * w + (x - r.left)];
                uint32_t v = qd_get_pixel(px, x, y);
                if (!*d || v != qd_get_pixel(&orig, x, y))
                    continue;
                qd_rect one = {(int16_t)y, (int16_t)x, (int16_t)(y + 1), (int16_t)(x + 1)};
                draw_art(t, &H.arts[t->orig_art], t->orig_dst, one);
                touch(t, one);
                qd_set_pixel(&ref, x, y, v);
                *d = 0;
            }
    }
    if (!t->sprite)
        match_sprites(t, px, r);
    for (int y = r.top; y < r.bottom; y++) {
        for (int x = r.left; x < r.right; x++) {
            if (!S.dirty[(size_t)(y - r.top) * w + (x - r.left)])
                continue;
            uint32_t v = qd_get_pixel(px, x, y);
            if (v == qd_get_pixel(&ref, x, y)) /* a sprite covered it */
                continue;
            qd_set_pixel(&ref, x, y, v);
            qd_rgb c = qd_color_of(v, px->depth, px->pal);
            uint8_t rgba[4] = {(uint8_t)(c.r >> 8), (uint8_t)(c.g >> 8), (uint8_t)(c.b >> 8), 0xFF};
            int hx = (x - px->bounds.left) * n, hy = (y - px->bounds.top) * n;
            for (int j = 0; j < n; j++)
                for (int i = 0; i < n; i++)
                    memcpy(hd_px(t, hx + i, hy + j), rgba, 4);
            touch(t, (qd_rect){(int16_t)y, (int16_t)x, (int16_t)(y + 1), (int16_t)(x + 1)});
        }
    }
}

/* Records that the HD copy now shows the 1x pixels in r. */
static void mark(twin *t, const qd_pixels *px, qd_rect r) {
    r = rect_sect(r, px->bounds);
    if (rect_empty(r))
        return;
    if (px->depth >= 8) {
        size_t b0, b1;
        byte_span(px, r.left, r.right, &b0, &b1);
        for (int y = r.top; y < r.bottom; y++) {
            size_t row = (size_t)(y - px->bounds.top) * px->row_bytes;
            memcpy(t->ref + row + b0, px->base + row + b0, b1 - b0);
        }
        return;
    }
    qd_pixels ref = ref_pixels(t, px);
    for (int y = r.top; y < r.bottom; y++)
        for (int x = r.left; x < r.right; x++)
            qd_set_pixel(&ref, x, y, qd_get_pixel(px, x, y));
}

/* Of n RGBA pixels in a and b: the first that differs (n when none do), and
   one past the last that differs (0 when none do). Equal runs are skipped 16
   pixels at a time. */
static int first_diff(const uint8_t *a, const uint8_t *b, int n) {
    int i = 0;
    while (i + 16 <= n && memcmp(a + (size_t)i * 4, b + (size_t)i * 4, 64) == 0)
        i += 16;
    while (i < n && memcmp(a + (size_t)i * 4, b + (size_t)i * 4, 4) == 0)
        i++;
    return i;
}

static int last_diff(const uint8_t *a, const uint8_t *b, int n) {
    int i = n;
    while (i >= 16 && memcmp(a + (size_t)(i - 16) * 4, b + (size_t)(i - 16) * 4, 64) == 0)
        i -= 16;
    while (i > 0 && memcmp(a + (size_t)(i - 1) * 4, b + (size_t)(i - 1) * 4, 4) == 0)
        i--;
    return i;
}

void hd_copy(const qd_pixels *src, qd_rect sr, const qd_pixels *dst, qd_rect dr, qd_rect clip) {
    /* A 1-bit source is colorized with the port's colors: left to sync. */
    if (!H.scale || src->depth == 1 || rect_empty(sr) || rect_empty(dr))
        return;
    qd_rect area = rect_sect(rect_sect(dr, clip), dst->bounds);
    if (rect_empty(area))
        return;
    twin *s = twin_for(src), *d = twin_for(dst);
    sync(s, src, sr);
    sync(d, dst, area);
    int n = H.scale;
    int sw = rect_w(sr), sh = rect_h(sr), dw = rect_w(dr), dh = rect_h(dr);
    int shw = hd_w(s), shh = rect_h(s->bounds) * n;
    int dx0 = dst->bounds.left * n, dy0 = dst->bounds.top * n;
    if (sw == dw && sh == dh) {
        /* No scaling: whole rows, limited to the source. HD x in dst plus ox
           is x in src's copy (and y plus oy). Moving rows down within one
           copy goes bottom-up, so no row is read after it's overwritten.
           The game often copies pixels that are already there (the whole
           table, each frame), so only the part of a row that differs is
           copied, and noted as changed. */
        int ox = (sr.left - dr.left - src->bounds.left) * n, oy = (sr.top - dr.top - src->bounds.top) * n;
        int x0 = area.left * n > -ox ? area.left * n : -ox;
        int x1 = area.right * n < shw - ox ? area.right * n : shw - ox;
        int y0 = area.top * n > -oy ? area.top * n : -oy;
        int y1 = area.bottom * n < shh - oy ? area.bottom * n : shh - oy;
        bool up = s == d && oy < -dy0;
        for (int i = 0; x0 < x1 && i < y1 - y0; i++) {
            int y = up ? y1 - 1 - i : y0 + i;
            uint8_t *to = hd_px(d, x0 - dx0, y - dy0);
            const uint8_t *from = hd_px(s, x0 + ox, y + oy);
            int first = first_diff(to, from, x1 - x0);
            if (first == x1 - x0)
                continue;
            int last = last_diff(to, from, x1 - x0);
            memmove(to + (size_t)first * 4, from + (size_t)first * 4, (size_t)(last - first) * 4);
            int hx = x0 - dx0 + first, hy = y - dy0; /* in d's copy */
            touch(d, (qd_rect){(int16_t)(d->bounds.top + hy / n), (int16_t)(d->bounds.left + hx / n),
                               (int16_t)(d->bounds.top + hy / n + 1),
                               (int16_t)(d->bounds.left + (x0 - dx0 + last + n - 1) / n)});
        }
        mark(d, dst, area);
        return;
    }
    /* Scaling within one copy: read from a snapshot. */
    uint8_t *from = s->rgba, *snap = NULL;
    if (s == d) {
        size_t len = (size_t)shw * (size_t)shh * 4;
        if (!(snap = malloc(len)))
            fatal("out of memory");
        memcpy(snap, s->rgba, len);
        from = snap;
    }
    int x0 = area.left * n, x1 = area.right * n;
    int *xs = malloc(sizeof *xs * (size_t)(x1 - x0));
    if (!xs)
        fatal("out of memory");
    for (int x = x0; x < x1; x++) { /* HD x in dst -> HD x in src's copy, or -1 */
        int sx = sr.left * n + (int)((int64_t)(x - dr.left * n) * sw / dw) - src->bounds.left * n;
        xs[x - x0] = sx >= 0 && sx < shw ? sx : -1;
    }
    for (int y = area.top * n; y < area.bottom * n; y++) {
        int sy = sr.top * n + (int)((int64_t)(y - dr.top * n) * sh / dh) - src->bounds.top * n;
        if (sy < 0 || sy >= shh)
            continue;
        const uint8_t *srow = from + (size_t)sy * (size_t)shw * 4;
        for (int x = x0; x < x1; x++)
            if (xs[x - x0] >= 0)
                memcpy(hd_px(d, x - dx0, y - dy0), srow + (size_t)xs[x - x0] * 4, 4);
    }
    free(xs);
    free(snap);
    touch(d, area);
    mark(d, dst, area);
}

static art *art_for(uint32_t hash) {
    for (int i = 0; i < H.narts; i++)
        if (H.arts[i].hash == hash)
            return &H.arts[i];
    if (H.narts == H.art_cap)
        H.arts = grow(H.arts, &H.art_cap, sizeof *H.arts);
    art *a = &H.arts[H.narts++];
    *a = (art){hash, NULL, 0, 0, false};
    if (!H.art_dir[0])
        return a;
    char path[1100];
    snprintf(path, sizeof path, "%s/%08x.png", H.art_dir, hash);
    SDL_Surface *s = SDL_LoadPNG(path);
    if (!s)
        return a;
    SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    if (!c) {
        log_msg("hd: can't convert %s: %s", path, SDL_GetError());
        return a;
    }
    a->w = c->w;
    a->h = c->h;
    a->rgba = malloc((size_t)c->w * (size_t)c->h * 4);
    if (!a->rgba)
        fatal("out of memory");
    for (int y = 0; y < c->h; y++)
        memcpy(a->rgba + (size_t)y * (size_t)c->w * 4, (uint8_t *)c->pixels + (size_t)y * c->pitch,
               (size_t)c->w * 4);
    SDL_DestroySurface(c);
    a->opaque = true;
    for (size_t i = 3; i < (size_t)a->w * (size_t)a->h * 4; i += 4)
        if (a->rgba[i] < 0x80) {
            a->opaque = false;
            break;
        }
    log_msg("hd: art %08x.png (%dx%d)", hash, a->w, a->h);
    return a;
}

static void dump(uint32_t hash, const uint8_t *data, size_t len) {
    for (int i = 0; i < H.ndumped; i++)
        if (H.dumped[i] == hash)
            return;
    if (H.ndumped == H.dump_cap)
        H.dumped = grow(H.dumped, &H.dump_cap, sizeof *H.dumped);
    H.dumped[H.ndumped++] = hash;
    qd_rect frame;
    if (!pict_frame(data, len, &frame) || rect_empty(frame))
        return;
    int w = rect_w(frame), h = rect_h(frame);
    qd_rect b = {0, 0, (int16_t)h, (int16_t)w};
    qd_pixels px = {calloc((size_t)w * (size_t)h, 4), (uint32_t)w * 4, b, 32, NULL};
    uint8_t *rgba = malloc((size_t)w * (size_t)h * 4);
    if (!px.base || !rgba)
        fatal("out of memory");
    char err[128], path[1100];
    if (pict_draw(data, len, b, &px, b, (qd_rgb){0, 0, 0}, (qd_rgb){0xFFFF, 0xFFFF, 0xFFFF}, err,
                  sizeof err)) {
        qd_to_rgba(&px, rgba);
        snprintf(path, sizeof path, "%s/%08x.png", H.dump_dir, hash);
        if (!png_write_rgba(path, rgba, w, h))
            log_msg("hd: can't write %s", path);
    }
    free(px.base);
    free(rgba);
}

void hd_picture(const uint8_t *data, size_t len, qd_rect dst, const qd_pixels *target,
                qd_rect clip) {
    if (!H.scale && !H.dump_dir[0])
        return;
    uint32_t hash = fnv1a32(data, len);
    if (H.dump_dir[0])
        dump(hash, data, len);
    if (!H.scale || rect_empty(dst))
        return;
    art *a = art_for(hash);
    qd_rect area = rect_sect(rect_sect(dst, clip), target->bounds);
    if (!a->rgba || rect_empty(area))
        return;
    twin *t = twin_for(target);
    if (!a->opaque) /* what shows through must be up to date */
        sync(t, target, area);
    draw_art(t, a, dst, area);
    touch(t, area);
    mark(t, target, area);
    if (rect_w(t->bounds) <= SPRITE_MAX && rect_h(t->bounds) <= SPRITE_MAX) {
        t->sprite = true;
        return;
    }
    size_t n1 = (size_t)t->row_bytes * (size_t)rect_h(t->bounds);
    if (!t->orig && !(t->orig = malloc(n1)))
        fatal("out of memory");
    memcpy(t->orig, target->base, n1);
    t->orig_art = (int)(a - H.arts);
    t->orig_dst = dst;
    t->orig_area = area;
}

const uint8_t *hd_frame(const qd_pixels *screen, int *w, int *h) {
    twin *t = twin_for(screen);
    sync(t, screen, screen->bounds);
    *w = hd_w(t);
    *h = rect_h(t->bounds) * H.scale;
    return t->rgba;
}

int hd_take_changes(const qd_pixels *screen, qd_rect *out, int max) {
    if (!H.scale)
        return 0;
    twin *t = twin_for(screen);
    int n = H.scale, h = rect_h(t->bounds), count = 0;
    for (int y = 0; y < h && max > 0; y++) { /* the y++ skips the row after a run: unchanged */
        if (t->changed[y].left >= t->changed[y].right)
            continue;
        qd_rect r = {(int16_t)y, INT16_MAX, (int16_t)y, 0};
        for (; y < h && t->changed[y].left < t->changed[y].right; y++) { /* a run of rows */
            span *c = &t->changed[y];
            if (c->left < r.left)
                r.left = c->left;
            if (c->right > r.right)
                r.right = c->right;
            *c = (span){INT16_MAX, 0};
        }
        r.bottom = (int16_t)y;
        if (count < max) {
            out[count++] = r;
            continue;
        }
        qd_rect *last = &out[max - 1]; /* out of room: the last rect grows to hold the rest */
        *last = (qd_rect){last->top, r.left < last->left ? r.left : last->left, r.bottom,
                          r.right > last->right ? r.right : last->right};
    }
    for (int i = 0; i < count; i++) /* HD pixels of the copy */
        out[i] = (qd_rect){(int16_t)(out[i].top * n), (int16_t)(out[i].left * n),
                           (int16_t)(out[i].bottom * n), (int16_t)(out[i].right * n)};
    return count;
}
