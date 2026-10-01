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
    SDL_SetRenderVSync(D.renderer, 1);
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
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                log_msg("window closed");
                exit(0);
            }
    }
    free(rgba);
}

void display_present_if_dirty(void) {
    if (qd_take_dirty())
        display_present();
}

unsigned display_frames(void) { return D.frames; }
