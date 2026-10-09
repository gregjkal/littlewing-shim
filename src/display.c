#include "display.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "hd.h"
#include "menu.h"
#include "png.h"
#include "qd.h"
#include "util.h"

static struct {
    bool sdl_ok, tried;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int tex_w, tex_h;
    bool tex_hd; /* holds the last HD frame, so the next one's changes can go on top */
    unsigned frames;
    const char *screenshot;
    display_input input;
    bool no_vsync;
    bool cursor_hidden;
    const char *title;
    /* What presenting costs, logged at exit: presents that reached the
       window, bytes uploaded, and nanoseconds in the upload alone and in the
       upload plus drawing and presenting (which waits for vsync when on). */
    unsigned shown;
    uint64_t bytes, upload_ns, upload_max_ns, total_ns, total_max_ns;
    bool stats_logged;
    /* LOONY_HD_VERIFY: a copy of the frame built from the same uploads as the
       texture, and how many presents it matched the whole frame after. */
    bool verify;
    uint8_t *shadow;
    int shadow_w, shadow_h;
    unsigned verified, verify_ok;
} D;

#define MAX_CHANGES 64 /* rects uploaded per HD present; more are merged by hd_take_changes */

void display_set_title(const char *title) {
    D.title = title;
    if (D.window)
        SDL_SetWindowTitle(D.window, title);
}

/* The frame to show, w x h: the screen's pixels, or their HD copy in HD mode.
   *lw x *lh is the screen's own size. Free *owned afterwards. */
static const uint8_t *screen_frame(int *w, int *h, int *lw, int *lh, uint8_t **owned) {
    qd_pixels px;
    qd_palette pal;
    qd_screen(&px, &pal);
    *lw = rect_w(px.bounds);
    *lh = rect_h(px.bounds);
    *owned = NULL;
    if (hd_scale())
        return hd_frame(&px, w, h);
    *w = *lw;
    *h = *lh;
    uint8_t *rgba = malloc((size_t)*w * (size_t)*h * 4);
    if (!rgba)
        fatal("out of memory");
    qd_to_rgba(&px, rgba);
    return *owned = rgba;
}

bool display_write_png(const char *path) {
    int w, h, lw, lh;
    uint8_t *owned;
    const uint8_t *rgba = screen_frame(&w, &h, &lw, &lh, &owned);
    bool ok = png_write_rgba(path, rgba, w, h);
    free(owned);
    return ok;
}

static void write_screenshot(void) {
    if (D.screenshot && !display_write_png(D.screenshot))
        log_msg("can't write the screenshot %s", D.screenshot);
}

void display_log_stats(void) {
    if (D.stats_logged || !D.shown)
        return;
    D.stats_logged = true;
    log_msg("display: %u presents, %llu bytes uploaded per present on average; upload %.2f ms "
            "average, %.2f ms max; upload and present %.2f ms average, %.2f ms max",
            D.shown, (unsigned long long)(D.bytes / D.shown), D.upload_ns / 1e6 / D.shown,
            D.upload_max_ns / 1e6, D.total_ns / 1e6 / D.shown, D.total_max_ns / 1e6);
    if (D.verify)
        log_msg("display: LOONY_HD_VERIFY: the uploads made the whole frame after %u of %u presents",
                D.verify_ok, D.verified);
}

void display_init(void) {
    const char *v = getenv("LOONY_HD_VERIFY");
    D.verify = v && strcmp(v, "1") == 0;
    D.screenshot = getenv("LOONY_SCREENSHOT");
    if (D.screenshot && *D.screenshot)
        atexit(write_screenshot);
    else
        D.screenshot = NULL;
}

static bool open_window(int w, int h) {
    D.tried = true;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log_msg("display: SDL_Init failed: %s (continuing without a window)", SDL_GetError());
        return false;
    }
    menu_install();
    int scale = w < 800 ? 2 : 1;
    const char *fs = getenv("LOONY_FULLSCREEN");
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | (fs && strcmp(fs, "1") == 0 ? SDL_WINDOW_FULLSCREEN : 0);
    if (hd_scale()) /* draw HD frames at the display's own pixel density */
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    D.window = SDL_CreateWindow(D.title ? D.title : "LittleWing", w * scale, h * scale, flags);
    D.renderer = D.window ? SDL_CreateRenderer(D.window, NULL) : NULL;
    if (!D.renderer) {
        log_msg("display: can't create a window: %s (continuing without one)", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(D.renderer, D.no_vsync ? 0 : 1);
    return true;
}

/* Copies the rects of rgba (w x h) into the same rects of to. */
static void copy_rects(uint8_t *to, const uint8_t *rgba, int w, const qd_rect *rects, int n) {
    for (int i = 0; i < n; i++)
        for (int y = rects[i].top; y < rects[i].bottom; y++) {
            size_t at = ((size_t)y * (size_t)w + (size_t)rects[i].left) * 4;
            memcpy(to + at, rgba + at, (size_t)rect_w(rects[i]) * 4);
        }
}

/* LOONY_HD_VERIFY: applies an upload (the whole frame when n < 0) to the
   shadow copy, as to the texture, and checks that it now equals the frame. */
static void verify_upload(const uint8_t *rgba, int w, int h, const qd_rect *changes, int n) {
    size_t len = (size_t)w * (size_t)h * 4;
    if (D.shadow_w != w || D.shadow_h != h) {
        free(D.shadow);
        if (!(D.shadow = malloc(len)))
            fatal("out of memory");
        D.shadow_w = w;
        D.shadow_h = h;
    }
    if (n < 0)
        memcpy(D.shadow, rgba, len);
    else
        copy_rects(D.shadow, rgba, w, changes, n);
    D.verified++;
    D.verify_ok += memcmp(D.shadow, rgba, len) == 0;
}

/* Shows a w x h frame of a screen whose own size is lw x lh, the size mouse
   coordinates are reported in. changes (n of them, in the frame's pixels)
   cover everything that differs from the HD frame before; n < 0 means the
   frame isn't an HD one, and is uploaded whole. So is the first frame in a
   new texture, or after a frame that wasn't HD. */
static void present(const uint8_t *rgba, int w, int h, int lw, int lh, const qd_rect *changes,
                    int n) {
    D.frames++;
    if (!D.tried) {
        D.sdl_ok = open_window(lw, lh);
        atexit(display_log_stats);
    }
    if (D.sdl_ok) {
        if (!D.texture || D.tex_w != w || D.tex_h != h) {
            if (D.texture)
                SDL_DestroyTexture(D.texture);
            D.texture = SDL_CreateTexture(D.renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STREAMING, w, h);
            /* An HD frame is usually shrunk to fit, which wants smoothing. */
            SDL_SetTextureScaleMode(D.texture, w == lw ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
            SDL_SetRenderLogicalPresentation(D.renderer, lw, lh, SDL_LOGICAL_PRESENTATION_LETTERBOX);
            D.tex_w = w;
            D.tex_h = h;
            D.tex_hd = false;
        }
        bool hd = n >= 0;
        if (!D.tex_hd)
            n = -1;
        D.tex_hd = hd;
        uint64_t bytes = 0, t0 = SDL_GetTicksNS();
        if (n < 0) {
            SDL_UpdateTexture(D.texture, NULL, rgba, w * 4);
            bytes = (uint64_t)w * (uint64_t)h * 4;
        }
        for (int i = 0; i < n; i++) {
            const qd_rect *c = &changes[i];
            SDL_Rect r = {c->left, c->top, rect_w(*c), rect_h(*c)};
            SDL_UpdateTexture(D.texture, &r, rgba + ((size_t)r.y * (size_t)w + (size_t)r.x) * 4, w * 4);
            bytes += (uint64_t)r.w * (uint64_t)r.h * 4;
        }
        uint64_t t1 = SDL_GetTicksNS();
        SDL_SetRenderDrawColor(D.renderer, 0, 0, 0, 255);
        SDL_RenderClear(D.renderer);
        SDL_RenderTexture(D.renderer, D.texture, NULL, NULL);
        SDL_RenderPresent(D.renderer);
        uint64_t t2 = SDL_GetTicksNS();
        D.shown++;
        D.bytes += bytes;
        D.upload_ns += t1 - t0;
        D.total_ns += t2 - t0;
        if (t1 - t0 > D.upload_max_ns)
            D.upload_max_ns = t1 - t0;
        if (t2 - t0 > D.total_max_ns)
            D.total_max_ns = t2 - t0;
        if (D.verify)
            verify_upload(rgba, w, h, changes, n);
    }
}

void display_present_rgba(const uint8_t *rgba, int w, int h) {
    present(rgba, w, h, w, h, NULL, -1);
}

void display_present(void) {
    int w, h, lw, lh;
    uint8_t *owned;
    const uint8_t *rgba = screen_frame(&w, &h, &lw, &lh, &owned);
    static qd_rect changes[MAX_CHANGES];
    int n = -1;
    if (hd_scale()) {
        qd_pixels px;
        qd_palette pal;
        qd_screen(&px, &pal);
        n = hd_take_changes(&px, changes, MAX_CHANGES);
    }
    present(rgba, w, h, lw, lh, changes, n);
    free(owned);
}

bool display_fullscreen(void) {
    return D.window && (SDL_GetWindowFlags(D.window) & SDL_WINDOW_FULLSCREEN) != 0;
}

void display_present_if_dirty(void) {
    if (qd_take_dirty())
        display_present();
}

unsigned display_frames(void) { return D.frames; }

void display_set_input(const display_input *in) { D.input = *in; }

void display_set_vsync(bool on) {
    D.no_vsync = !on;
    if (D.renderer)
        SDL_SetRenderVSync(D.renderer, on ? 1 : 0);
}

void display_set_cursor(bool visible) {
    if (!D.sdl_ok || visible == !D.cursor_hidden)
        return;
    D.cursor_hidden = !visible;
    if (visible)
        SDL_ShowCursor();
    else
        SDL_HideCursor();
}

void display_poll(void) {
    if (!D.sdl_ok)
        return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (D.input.quit)
                D.input.quit();
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            if (D.input.focus)
                D.input.focus(e.type == SDL_EVENT_WINDOW_FOCUS_GAINED);
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            bool down = e.type == SDL_EVENT_KEY_DOWN;
            if (down && (e.key.mod & SDL_KMOD_GUI) && e.key.scancode == SDL_SCANCODE_Q) {
                if (D.input.quit)
                    D.input.quit();
                break;
            }
            if ((e.key.mod & SDL_KMOD_GUI) && e.key.scancode == SDL_SCANCODE_V) {
                if (down && D.input.paste) {
                    char *text = SDL_GetClipboardText();
                    if (text && *text)
                        D.input.paste(text);
                    SDL_free(text);
                }
                break;
            }
            if ((e.key.mod & SDL_KMOD_GUI) && e.key.scancode == SDL_SCANCODE_F) {
                if (down && !e.key.repeat) {
                    bool fs = (SDL_GetWindowFlags(D.window) & SDL_WINDOW_FULLSCREEN) != 0;
                    SDL_SetWindowFullscreen(D.window, !fs);
                }
                break;
            }
            if (D.input.key)
                D.input.key((int)e.key.scancode, down, e.key.repeat);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (e.button.button != SDL_BUTTON_LEFT || !D.input.mouse)
                break;
            float x = e.button.x, y = e.button.y;
            SDL_RenderCoordinatesFromWindow(D.renderer, e.button.x, e.button.y, &x, &y);
            D.input.mouse((int)SDL_floorf(x), (int)SDL_floorf(y), e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            break;
        }
        default:
            break;
        }
    }
}
