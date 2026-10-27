#include "display.h"

#include <SDL3/SDL.h>
#include <stdio.h>
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
    int lw, lh; /* the screen's own size, which the texture is shown as */
    bool tex_hd; /* holds the last HD frame, so the next one's changes can go on top */
    unsigned frames;
    const char *screenshot;
    display_input input;
    void (*open)(const char *path, uint32_t from_ticks);
    /* A file to open once display_poll's events are handled. */
    bool open_pending;
    char open_path[4096];
    uint32_t open_from;
    void (*close)(const char *why);
    /* The replay to close once display_poll's events are handled, saying why. */
    bool close_pending;
    char close_why[256];
    bool no_vsync;
    bool cursor_hidden;
    bool cursor_wanted; /* by the game (display_set_cursor) */
    const display_overlay *overlay;
    /* The left button went down on the strip and isn't up yet: the strip
       gets its release wherever it is. Where the pointer last was, in
       points, for a release that never comes (the window losing focus). */
    bool grabbed;
    float pointer_x, pointer_y;
    bool redraw;          /* the window wants drawing again: resized or uncovered */
    uint64_t presented;   /* SDL_GetTicksNS at the last present to the window */
    bool raise; /* on opening */
    const char *title;
    char flash[256];      /* a message in the title, in place of the title */
    uint64_t flash_until; /* SDL_GetTicks when it goes, or 0 */
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

#define REDRAW_GAP_NS (1000000000 / 60) /* between draws display_poll makes by itself */

void display_set_title(const char *title) {
    D.title = title;
    if (D.window && !D.flash_until)
        SDL_SetWindowTitle(D.window, title);
}

void display_raise_on_open(void) { D.raise = true; }

void display_flash(const char *msg) {
    if (D.overlay && D.overlay->say) {
        D.overlay->say(msg);
        return;
    }
    if (!D.window)
        return;
    snprintf(D.flash, sizeof D.flash, "%s: %s", D.title ? D.title : "LittleWing", msg);
    SDL_SetWindowTitle(D.window, D.flash);
    D.flash_until = SDL_GetTicks() + DISPLAY_FLASH_MS;
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
    /* HD frames, and the overlay's lines and text, at the display's own
       pixel density */
    if (hd_scale() || D.overlay)
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    int strip = D.overlay ? (int)D.overlay->height : 0;
    D.window = SDL_CreateWindow(D.title ? D.title : "LittleWing", w * scale, h * scale + strip, flags);
    D.renderer = D.window ? SDL_CreateRenderer(D.window, NULL) : NULL;
    if (!D.renderer) {
        log_msg("display: can't create a window: %s (continuing without one)", SDL_GetError());
        return false;
    }
    if (D.overlay) { /* the strip's controls fit, over a game a quarter of its size or more */
        int min_w = (int)D.overlay->min_width;
        int min_h = min_w * h / w;
        if (min_h < h / 2)
            min_h = h / 2;
        SDL_SetWindowMinimumSize(D.window, min_w, min_h + strip);
    }
    SDL_SetRenderVSync(D.renderer, D.no_vsync ? 0 : 1);
    if (D.raise)
        SDL_RaiseWindow(D.window);
    const char *flash = getenv("LOONY_FLASH");
    if (flash && *flash)
        display_flash(flash);
    unsetenv("LOONY_FLASH"); /* said once: a later restart doesn't say it again */
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

display_layout display_lay_out(float w, float h, int lw, int lh, float strip_h, bool fullscreen) {
    display_layout l = {.fullscreen = fullscreen, .density = 1};
    l.strip = (display_rect){0, h - strip_h, w, strip_h};
    float gh = fullscreen ? h : h - strip_h;
    if (gh < 0)
        gh = 0;
    /* SDL's letterbox: the side that fits fills, the other is rounded down
       and centered, and an aspect within a hair of the game's fills both. */
    if (lw <= 0 || lh <= 0 || w <= 0 || gh <= 0) {
        l.game = (display_rect){0, 0, 0, 0};
    } else if (SDL_fabsf((float)lw / (float)lh - w / gh) < 0.0001f) {
        l.game = (display_rect){0, 0, w, gh};
    } else if ((float)lw / (float)lh > w / gh) {
        float gh2 = SDL_floorf((float)lh * (w / (float)lw));
        l.game = (display_rect){0, (gh - gh2) / 2, w, gh2};
    } else {
        float gw = SDL_floorf((float)lw * (gh / (float)lh));
        l.game = (display_rect){(w - gw) / 2, 0, gw, gh};
    }
    return l;
}

/* The window's layout now. */
static display_layout layout(void) {
    int w = 0, h = 0;
    SDL_GetRenderOutputSize(D.renderer, &w, &h);
    float density = SDL_GetWindowPixelDensity(D.window);
    if (density <= 0)
        density = 1;
    float strip = D.overlay ? D.overlay->height * density : 0;
    display_layout l = display_lay_out((float)w, (float)h, D.lw, D.lh, strip, display_fullscreen());
    l.density = density;
    return l;
}

/* Draws the window, short of presenting it: the last frame, letterboxed,
   and the overlay. */
static void compose(void) {
    display_layout l = layout();
    SDL_SetRenderDrawColor(D.renderer, 0, 0, 0, 255);
    SDL_RenderClear(D.renderer);
    if (D.texture) {
        SDL_FRect r = {l.game.x, l.game.y, l.game.w, l.game.h};
        SDL_RenderTexture(D.renderer, D.texture, NULL, &r);
    }
    if (D.overlay)
        D.overlay->draw(D.renderer, &l);
    D.redraw = false;
}

uint8_t *display_read_window(int *w, int *h) {
    if (!D.sdl_ok)
        return NULL;
    compose();
    SDL_Surface *s = SDL_RenderReadPixels(D.renderer, NULL);
    SDL_Surface *rgba = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32) : NULL;
    SDL_DestroySurface(s);
    if (!rgba)
        return NULL;
    *w = rgba->w;
    *h = rgba->h;
    uint8_t *out = malloc((size_t)rgba->w * (size_t)rgba->h * 4);
    if (!out)
        fatal("out of memory");
    size_t row = (size_t)rgba->w * 4;
    for (int y = 0; y < rgba->h; y++)
        memcpy(out + (size_t)y * row, (uint8_t *)rgba->pixels + (size_t)y * (size_t)rgba->pitch, row);
    SDL_DestroySurface(rgba);
    return out;
}

void display_set_overlay(const display_overlay *o) {
    D.overlay = o;
    D.grabbed = false;
    D.redraw = true;
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
        D.lw = lw;
        D.lh = lh;
        uint64_t t1 = SDL_GetTicksNS();
        compose();
        SDL_RenderPresent(D.renderer);
        uint64_t t2 = D.presented = SDL_GetTicksNS();
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

void display_set_open(void (*fn)(const char *path, uint32_t from_ticks)) { D.open = fn; }

static bool is_replay_path(const char *path) {
    size_t n = strlen(path);
    return n >= 9 && strcmp(path + n - 9, ".lwreplay") == 0;
}

void display_request_open(const char *path, uint32_t from_ticks) {
    if (D.open_pending && is_replay_path(D.open_path))
        return; /* the first replay asked for wins, as when each opened at once */
    if ((size_t)snprintf(D.open_path, sizeof D.open_path, "%s", path) >= sizeof D.open_path) {
        log_msg("can't open %s: its path is too long", path);
        return;
    }
    D.open_from = from_ticks;
    D.open_pending = true;
}

void display_set_close(void (*fn)(const char *why)) { D.close = fn; }

void display_request_close(const char *why) {
    snprintf(D.close_why, sizeof D.close_why, "%s", why);
    D.close_pending = true;
}

/* Closes the replay display_request_close asked for, else opens the file
   display_request_open asked for, if any. */
static void open_pending(void) {
    if (D.close_pending) {
        char why[sizeof D.close_why];
        snprintf(why, sizeof why, "%s", D.close_why);
        D.close_pending = false;
        D.open_pending = false;
        if (D.close)
            D.close(why); /* restarts the program */
        return;
    }
    if (!D.open_pending)
        return;
    char path[sizeof D.open_path];
    snprintf(path, sizeof path, "%s", D.open_path);
    D.open_pending = false;
    if (D.open)
        D.open(path, D.open_from); /* restarts the program, if it opens */
}

void display_take_launch_events(uint32_t wait_ms) {
    const char *driver = getenv("SDL_VIDEO_DRIVER");
    if ((driver && strcmp(driver, "cocoa") != 0) || !SDL_Init(SDL_INIT_VIDEO))
        return;
    /* Only the drops: the rest stay queued for display_poll. */
    uint64_t until = SDL_GetTicks() + wait_ms;
    do {
        SDL_Event e;
        SDL_PumpEvents();
        while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_DROP_FILE, SDL_EVENT_DROP_POSITION) > 0)
            if (e.type == SDL_EVENT_DROP_FILE && e.drop.data)
                display_request_open(e.drop.data, 0);
        open_pending();
        if (SDL_GetTicks() < until)
            SDL_Delay(5);
    } while (SDL_GetTicks() < until);
}

bool display_due(void) {
    if (D.no_vsync || !D.window)
        return true;
    float hz = 0;
    const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(D.window));
    if (m)
        hz = m->refresh_rate;
    if (hz < 30)
        hz = 60;
    return SDL_GetTicksNS() - D.presented >= (uint64_t)(1e9 / hz * 3 / 4);
}

void display_set_vsync(bool on) {
    D.no_vsync = !on;
    if (D.renderer)
        SDL_SetRenderVSync(D.renderer, on ? 1 : 0);
}

/* Shows the pointer, or hides it, as the overlay or else the game wants. */
static void apply_cursor(void) {
    bool visible = D.overlay ? D.overlay->shown() > 0 : D.cursor_wanted;
    if (!D.sdl_ok || visible == !D.cursor_hidden)
        return;
    D.cursor_hidden = !visible;
    if (visible)
        SDL_ShowCursor();
    else
        SDL_HideCursor();
}

void display_set_cursor(bool visible) {
    D.cursor_wanted = visible;
    apply_cursor();
}

static bool inside(display_rect r, float x, float y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* A left-button press or release at (x, y) in points: the strip's, when
   it's on the strip (in full screen, while the strip is up) or the press
   was, else the game's, in the emulated screen's coordinates. */
static void press(float x, float y, bool down) {
    if (down && D.grabbed) /* the last release never came: it ends here */
        press(D.pointer_x, D.pointer_y, false);
    D.pointer_x = x;
    D.pointer_y = y;
    display_layout l = layout();
    x *= l.density;
    y *= l.density;
    bool on_strip = inside(l.strip, x, y) && (!l.fullscreen || (D.overlay && D.overlay->shown() > 0));
    bool released = D.grabbed && !down;
    D.grabbed = false;
    if (D.overlay && (on_strip || released)) {
        D.grabbed = down;
        D.overlay->press(&l, x, y, down);
        return;
    }
    if (!D.input.mouse || l.game.w <= 0 || l.game.h <= 0)
        return;
    float gx = (x - l.game.x) * (float)D.lw / l.game.w, gy = (y - l.game.y) * (float)D.lh / l.game.h;
    D.input.mouse((int)SDL_floorf(gx), (int)SDL_floorf(gy), down);
}

void display_poll(void) {
    if (!D.sdl_ok)
        return;
    if (D.flash_until && SDL_GetTicks() >= D.flash_until) {
        D.flash_until = 0;
        SDL_SetWindowTitle(D.window, D.title ? D.title : "LittleWing");
    }
    SDL_Event e;
    /* Once the replay is to close, the rest of the events go unhandled: a
       key could save a clip beside a recording that is gone. */
    while (!D.close_pending && SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (D.input.quit)
                D.input.quit();
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            if (D.grabbed && e.type == SDL_EVENT_WINDOW_FOCUS_LOST)
                press(D.pointer_x, D.pointer_y, false);
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
            if ((e.key.mod & SDL_KMOD_GUI) && e.key.scancode == SDL_SCANCODE_S) {
                if (down && !e.key.repeat && D.input.mark) {
                    char msg[64];
                    int n = D.input.mark();
                    if (n)
                        snprintf(msg, sizeof msg, "clip %d marked", n);
                    else
                        snprintf(msg, sizeof msg, "no game is being recorded, nothing marked");
                    display_flash(msg);
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
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button == SDL_BUTTON_LEFT)
                press(e.button.x, e.button.y, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            break;
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            if (e.type == SDL_EVENT_MOUSE_MOTION) {
                D.pointer_x = e.motion.x;
                D.pointer_y = e.motion.y;
            }
            if (D.overlay && !(D.grabbed && e.type == SDL_EVENT_WINDOW_MOUSE_LEAVE)) {
                display_layout l = layout();
                bool in = e.type == SDL_EVENT_MOUSE_MOTION;
                D.overlay->pointer(&l, in ? e.motion.x * l.density : 0, in ? e.motion.y * l.density : 0, in);
            }
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_EXPOSED:
            D.redraw |= D.overlay != NULL; /* live play presents often enough */
            break;
        case SDL_EVENT_DROP_FILE:
            if (e.drop.data)
                display_request_open(e.drop.data, 0);
            break;
        default:
            break;
        }
    }
    if (D.overlay) {
        apply_cursor();
        D.redraw |= D.overlay->changed();
    }
    /* Paused, nothing presents: the window is drawn again here, at most
       once a frame. */
    if (D.redraw && D.texture && SDL_GetTicksNS() - D.presented >= REDRAW_GAP_NS) {
        compose();
        SDL_RenderPresent(D.renderer);
        D.presented = SDL_GetTicksNS();
    }
    open_pending();
}
