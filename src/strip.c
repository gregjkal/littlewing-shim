#include "strip.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "clip.h"
#include "util.h"
#include "watch.h"

/* What each button does. */
static const watch_action actions[STRIP_NBUTTONS] = {
    [STRIP_PLAY] = WATCH_PLAY_PAUSE,
    [STRIP_BACK] = WATCH_BACK,
    [STRIP_AHEAD] = WATCH_AHEAD,
    [STRIP_SLOWER] = WATCH_SLOWER,
    [STRIP_FASTER] = WATCH_FASTER,
};

/* What the strip looks like: when it changes, it's drawn again. */
typedef struct {
    int opacity; /* in 64ths */
    uint32_t seconds, length;
    bool paused, at_end, can_play;
    int speed;
    strip_button hover;
    bool tip;
} look;

static struct {
    double (*clock)(void);
    double active; /* when the pointer last moved, or a key was pressed */
    bool over;     /* the pointer is on the strip */
    strip_button hover;
    double hover_since;
    /* The fade: toward up (or down) from opacity from_op at from_t. */
    bool up;
    double from_op, from_t;
    look drawn;
} S;

static double now(void) { return S.clock ? S.clock() : wall_seconds(); }

void strip_reset(double (*clock)(void)) {
    memset(&S, 0, sizeof S);
    S.clock = clock;
    S.hover = STRIP_NONE;
    S.drawn.hover = STRIP_NONE;
}

/* ---- showing and hiding ---- */

static double opacity_at(double t) {
    double d = (t - S.from_t) / STRIP_FADE;
    double op = S.up ? S.from_op + d : S.from_op - d;
    return op < 1e-6 ? 0 : op > 1 - 1e-6 ? 1 : op; /* ends that rounding misses by a hair */
}

/* How far up the strip is now, after turning it toward up or down. It
   turns up at an event, which settles it before and after (settle), and
   down STRIP_HIDE_AFTER after the last, however late that's seen. */
static double opacity(void) {
    double t = now();
    bool up = S.over || t - S.active < STRIP_HIDE_AFTER;
    if (up != S.up) {
        double at = t;
        if (!up && S.active + STRIP_HIDE_AFTER < t && S.active + STRIP_HIDE_AFTER > S.from_t)
            at = S.active + STRIP_HIDE_AFTER;
        S.from_op = opacity_at(at);
        S.from_t = at;
        S.up = up;
    }
    return opacity_at(t);
}

static void settle(void) { (void)opacity(); }

void strip_poke(void) {
    settle();
    S.active = now();
    settle();
}

static float shown(void) { return (float)opacity(); }

/* ---- where things go ---- */

/* The places in the strip, in the window's pixels. */
typedef struct {
    display_rect button[STRIP_NBUTTONS];
    display_rect speed, time, bar;
    float k; /* the font's scale: pixels a font pixel */
} places;

static float text_scale(const display_layout *l) {
    float k = SDL_floorf(l->density * 1.25f + 0.5f);
    return k < 1 ? 1 : k;
}

static void time_text(const watch_state *w, char *out, size_t cap) {
    char a[32], b[32];
    clip_format_time(w->now - w->now % 60, a, sizeof a);
    clip_format_time(w->length - w->length % 60, b, sizeof b);
    snprintf(out, cap, "%s / %s", a, b);
}

static void speed_text(int speed, char *out, size_t cap) {
    if (speed)
        snprintf(out, cap, "%dx", speed);
    else
        snprintf(out, cap, "max");
}

static places place(const display_layout *l, const watch_state *w) {
    places p;
    memset(&p, 0, sizeof p);
    float b = l->strip.h, y = l->strip.y, x = l->strip.x + b / 4;
    p.k = text_scale(l);
    float ch = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * p.k;
    for (int i = STRIP_PLAY; i <= STRIP_AHEAD; i++, x += b)
        p.button[i] = (display_rect){x, y, b, b};
    x += b / 4;
    p.button[STRIP_SLOWER] = (display_rect){x, y, b * 0.75f, b};
    x += b * 0.75f;
    p.speed = (display_rect){x, y, 4 * ch, b}; /* "max", with room */
    x += 4 * ch;
    p.button[STRIP_FASTER] = (display_rect){x, y, b * 0.75f, b};
    x += b * 0.75f + b / 2;
    char t[64];
    time_text(w, t, sizeof t);
    p.time = (display_rect){x, y, (float)strlen(t) * ch, b};
    x += p.time.w + b / 2;
    float right = l->strip.x + l->strip.w - b / 2, th = 4 * l->density;
    p.bar = (display_rect){x, y + (b - th) / 2, right > x ? right - x : 0, th};
    return p;
}

static bool inside(display_rect r, float x, float y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

strip_button strip_button_at(const display_layout *l, float x, float y) {
    watch_state w;
    watch_get_state(&w);
    places p = place(l, &w);
    for (int i = 0; i < STRIP_NBUTTONS; i++)
        if (inside(p.button[i], x, y))
            return (strip_button)i;
    return STRIP_NONE;
}

/* ---- tips ---- */

static const char *const names[STRIP_NBUTTONS] = {
    [STRIP_PLAY] = "Play/Pause", [STRIP_BACK] = "Back 10 s", [STRIP_AHEAD] = "Ahead 10 s",
    [STRIP_SLOWER] = "Slower",   [STRIP_FASTER] = "Faster",
};

void strip_tip(strip_button b, char *out, size_t cap) {
    out[0] = '\0';
    if (b < 0 || b >= STRIP_NBUTTONS)
        return;
    for (size_t i = 0; i < watch_nbindings; i++) {
        const watch_binding *k = &watch_bindings[i];
        if (k->action != actions[b] || !k->scancode)
            continue;
        /* The key as typed: Shift-comma is "<". A letter keeps its name,
           so with Shift it's said. */
        SDL_Keycode plain = SDL_GetKeyFromScancode((SDL_Scancode)k->scancode, 0, false);
        SDL_Keycode typed = SDL_GetKeyFromScancode((SDL_Scancode)k->scancode, k->mods, false);
        char key[64];
        snprintf(key, sizeof key, "%s", SDL_GetKeyName(typed));
        bool said = (k->mods & SDL_KMOD_SHIFT) && typed == plain;
        snprintf(out, cap, "%s (%s%s)", names[b], said ? "Shift-" : "", key);
        return;
    }
    snprintf(out, cap, "%s", names[b]);
}

/* ---- the pointer ---- */

static void pointer(const display_layout *l, float x, float y, bool in) {
    settle();
    strip_button b = STRIP_NONE;
    S.over = in && inside(l->strip, x, y);
    if (in) {
        S.active = now();
        if (S.over)
            b = strip_button_at(l, x, y);
    }
    if (b != S.hover) {
        S.hover = b;
        S.hover_since = now();
    }
    settle();
}

static void press(const display_layout *l, float x, float y, bool down) {
    settle();
    S.active = now();
    settle();
    if (!down)
        return;
    strip_button b = strip_button_at(l, x, y);
    if (b != STRIP_NONE) {
        S.hover_since = now(); /* the tip goes, and comes back after resting again */
        watch_act(actions[b], false);
    }
}

/* ---- drawing ---- */

static look look_now(void) {
    watch_state w;
    watch_get_state(&w);
    double op = opacity();
    look k;
    memset(&k, 0, sizeof k); /* padding too: looks are compared whole */
    k.opacity = (int)(op * 64 + 0.5);
    k.seconds = w.now / 60;
    k.length = w.length;
    k.paused = w.paused;
    k.at_end = w.at_end;
    k.can_play = watch_can(WATCH_PLAY_PAUSE);
    k.speed = w.speed;
    k.hover = op > 0 ? S.hover : STRIP_NONE;
    k.tip = k.hover != STRIP_NONE && now() - S.hover_since >= STRIP_TIP_AFTER;
    return k;
}

static bool changed(void) {
    look k = look_now();
    return memcmp(&k, &S.drawn, sizeof k) != 0;
}

/* The colors, at an opacity. */
static void color(SDL_Renderer *r, int v, float alpha) {
    SDL_SetRenderDrawColor(r, (Uint8)v, (Uint8)v, (Uint8)v, (Uint8)(alpha * 255 + 0.5f));
}

static void fill(SDL_Renderer *r, display_rect d) {
    SDL_FRect f = {d.x, d.y, d.w, d.h};
    SDL_RenderFillRect(r, &f);
}

/* A triangle pointing right (dir 1) or left (-1), size s, centered on
   (cx, cy), in the draw color. */
static void triangle(SDL_Renderer *r, float cx, float cy, float s, int dir) {
    Uint8 c[4];
    SDL_GetRenderDrawColor(r, &c[0], &c[1], &c[2], &c[3]);
    SDL_FColor fc = {c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, c[3] / 255.f};
    float h = s / 2, w = s * 0.433f * (float)dir; /* half of an equilateral triangle's height */
    SDL_Vertex v[3] = {
        {{cx - w, cy - h}, fc, {0, 0}},
        {{cx + w, cy}, fc, {0, 0}},
        {{cx - w, cy + h}, fc, {0, 0}},
    };
    SDL_RenderGeometry(r, NULL, v, 3, NULL, 0);
}

/* Text centered in d, at scale k. */
static void text(SDL_Renderer *r, display_rect d, float k, const char *t) {
    float ch = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
    float x = d.x + (d.w - (float)strlen(t) * ch * k) / 2, y = d.y + (d.h - ch * k) / 2;
    SDL_SetRenderScale(r, k, k);
    SDL_RenderDebugText(r, SDL_roundf(x / k), SDL_roundf(y / k), t);
    SDL_SetRenderScale(r, 1, 1);
}

static void draw_icon(SDL_Renderer *r, strip_button b, display_rect d, const look *k, float kk) {
    float cx = d.x + d.w / 2, cy = d.y + d.h / 2, s = d.h * 0.4f;
    switch (b) {
    case STRIP_PLAY:
        if (k->paused || k->at_end) {
            triangle(r, cx, cy, s, 1);
        } else { /* pause: two bars */
            float bw = s * 0.3f;
            fill(r, (display_rect){cx - s * 0.4f, cy - s / 2, bw, s});
            fill(r, (display_rect){cx + s * 0.4f - bw, cy - s / 2, bw, s});
        }
        break;
    case STRIP_BACK:
    case STRIP_AHEAD: {
        int dir = b == STRIP_AHEAD ? 1 : -1;
        triangle(r, cx - s * 0.22f * (float)dir, cy, s * 0.8f, dir);
        triangle(r, cx + s * 0.22f * (float)dir, cy, s * 0.8f, dir);
        break;
    }
    case STRIP_SLOWER: text(r, d, kk, "-"); break;
    case STRIP_FASTER: text(r, d, kk, "+"); break;
    default: break;
    }
}

/* A thin line across the bottom of the window: how far the replay has got. */
static void draw_line(SDL_Renderer *r, const display_layout *l, const watch_state *w, float alpha) {
    float th = 2 * l->density, y = l->strip.y + l->strip.h - th;
    float f = w->length ? (float)w->now / (float)w->length : 0;
    f = f > 1 ? 1 : f;
    color(r, 255, 0.2f * alpha);
    fill(r, (display_rect){l->strip.x, y, l->strip.w, th});
    SDL_SetRenderDrawColor(r, 220, 50, 40, (Uint8)(alpha * 255 + 0.5f));
    fill(r, (display_rect){l->strip.x, y, l->strip.w * f, th});
}

static void draw(void *renderer, const display_layout *l) {
    SDL_Renderer *r = renderer;
    S.drawn = look_now();
    const look *k = &S.drawn;
    watch_state w;
    watch_get_state(&w);
    float op = (float)k->opacity / 64;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    if (op < 1)
        draw_line(r, l, &w, 1 - op);
    if (op <= 0)
        return;
    places p = place(l, &w);
    SDL_SetRenderDrawColor(r, 16, 16, 16, (Uint8)(op * 0.85f * 255 + 0.5f));
    fill(r, l->strip);
    for (int i = 0; i < STRIP_NBUTTONS; i++) {
        bool on = i != STRIP_PLAY || k->can_play;
        if (i == k->hover && on) {
            color(r, 255, 0.15f * op);
            fill(r, p.button[i]);
        }
        color(r, 235, (on ? 1 : 0.4f) * op);
        draw_icon(r, (strip_button)i, p.button[i], k, p.k);
    }
    char t[64];
    color(r, 235, op);
    speed_text(w.speed, t, sizeof t);
    text(r, p.speed, p.k, t);
    time_text(&w, t, sizeof t);
    text(r, p.time, p.k, t);
    if (p.bar.w > 0) {
        float f = w.length ? (float)w.now / (float)w.length : 0;
        f = f > 1 ? 1 : f;
        color(r, 255, 0.25f * op);
        fill(r, p.bar);
        SDL_SetRenderDrawColor(r, 220, 50, 40, (Uint8)(op * 255 + 0.5f));
        fill(r, (display_rect){p.bar.x, p.bar.y, p.bar.w * f, p.bar.h});
    }
    if (k->tip) { /* above the strip, from the button's left, kept in the window */
        strip_tip(k->hover, t, sizeof t);
        float ch = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * p.k, pad = ch / 2;
        display_rect tip = {p.button[k->hover].x, l->strip.y - ch - 3 * pad,
                            (float)strlen(t) * ch + 2 * pad, ch + 2 * pad};
        if (tip.x + tip.w > l->strip.x + l->strip.w)
            tip.x = l->strip.x + l->strip.w - tip.w;
        if (tip.x < 0)
            tip.x = 0;
        SDL_SetRenderDrawColor(r, 16, 16, 16, (Uint8)(op * 0.9f * 255 + 0.5f));
        fill(r, tip);
        color(r, 235, op);
        text(r, tip, p.k, t);
    }
}

static const display_overlay overlay = {
    .height = STRIP_HEIGHT,
    .min_width = STRIP_MIN_WIDTH,
    .draw = draw,
    .changed = changed,
    .shown = shown,
    .pointer = pointer,
    .press = press,
};

const display_overlay *strip_overlay(void) { return &overlay; }

void strip_install(void) {
    strip_reset(S.clock);
    S.active = now();
    display_set_overlay(&overlay);
}
