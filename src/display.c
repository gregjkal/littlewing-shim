#include "display.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

#include "png.h"
#include "qd.h"
#include "util.h"

static struct {
    bool sdl_ok, tried;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int tex_w, tex_h;
    unsigned frames;
    const char *screenshot;
    display_input input;
    bool no_vsync;
    bool cursor_hidden;
} D;

static uint8_t *screen_rgba(int *w, int *h) {
    qd_pixels px;
    qd_palette pal;
    qd_screen(&px, &pal);
    *w = rect_w(px.bounds);
    *h = rect_h(px.bounds);
    uint8_t *rgba = malloc((size_t)*w * (size_t)*h * 4);
    if (!rgba)
        fatal("out of memory");
    qd_to_rgba(&px, rgba);
    return rgba;
}

bool display_write_png(const char *path) {
    int w, h;
    uint8_t *rgba = screen_rgba(&w, &h);
    bool ok = png_write_rgba(path, rgba, w, h);
    free(rgba);
    return ok;
}

static void write_screenshot(void) {
    if (D.screenshot && !display_write_png(D.screenshot))
        log_msg("can't write the screenshot %s", D.screenshot);
}

void display_init(void) {
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
    int scale = w < 800 ? 2 : 1;
    D.window = SDL_CreateWindow("Loony Labyrinth", w * scale, h * scale, SDL_WINDOW_RESIZABLE);
    D.renderer = D.window ? SDL_CreateRenderer(D.window, NULL) : NULL;
    if (!D.renderer) {
        log_msg("display: can't create a window: %s (continuing without one)", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(D.renderer, D.no_vsync ? 0 : 1);
    return true;
}

void display_present(void) {
    D.frames++;
    int w, h;
    uint8_t *rgba = screen_rgba(&w, &h);
    if (!D.tried)
        D.sdl_ok = open_window(w, h);
    if (D.sdl_ok) {
        if (!D.texture || D.tex_w != w || D.tex_h != h) {
            if (D.texture)
                SDL_DestroyTexture(D.texture);
            D.texture = SDL_CreateTexture(D.renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STREAMING, w, h);
            SDL_SetTextureScaleMode(D.texture, SDL_SCALEMODE_NEAREST);
            SDL_SetRenderLogicalPresentation(D.renderer, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
            D.tex_w = w;
            D.tex_h = h;
        }
        SDL_UpdateTexture(D.texture, NULL, rgba, w * 4);
        SDL_SetRenderDrawColor(D.renderer, 0, 0, 0, 255);
        SDL_RenderClear(D.renderer);
        SDL_RenderTexture(D.renderer, D.texture, NULL, NULL);
        SDL_RenderPresent(D.renderer);
    }
    free(rgba);
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
