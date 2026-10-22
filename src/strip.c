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
    strip_spot spot; /* under the pointer on the bar */
    bool have_in;
    uint32_t in;
    size_t nmarks, nclips;
} look;

static struct {
    double (*clock)(void);
    double active; /* when the pointer last moved, or a key was pressed */
    bool over;     /* the pointer is on the strip */
    strip_button hover;
    double hover_since;
    strip_spot spot;
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

/* ---- the bar ---- */

display_rect strip_bar(const display_layout *l, const watch_state *w) { return place(l, w).bar; }

static float x_of(display_rect bar, const watch_state *w, uint32_t tick) {
    float f = w->length ? (float)tick / (float)w->length : 0;
    return bar.x + bar.w * (f > 1 ? 1 : f);
}

strip_spot strip_spot_at(const display_layout *l, const watch_state *w, float x, float y) {
    strip_spot s = {STRIP_ON_NOTHING, -1, 0};
    display_rect bar = place(l, w).bar;
    float snap = STRIP_SNAP * l->density;
    if (bar.w <= 0 || !inside(l->strip, x, y) || x < bar.x - snap || x >= bar.x + bar.w + snap)
        return s;
    float best = snap;
    for (size_t i = 0; i < w->nmarks; i++) {
        float d = SDL_fabsf(x - x_of(bar, w, w->marks[i]));
        if (d <= best) {
            s = (strip_spot){STRIP_ON_MARK, (int)i, w->marks[i]};
            best = d;
        }
    }
    bool on_mark = s.on == STRIP_ON_MARK;
    for (size_t i = 0; i < w->nclips && !on_mark; i++) {
        float d = SDL_fabsf(x - x_of(bar, w, w->clips[i].start));
        if (d <= best) {
            s = (strip_spot){STRIP_ON_CLIP, (int)i, w->clips[i].start};
            best = d;
        }
    }
    if (s.on != STRIP_ON_NOTHING)
        return s;
    float f = (x - bar.x) / bar.w;
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    return (strip_spot){STRIP_ON_TIME, -1, (uint32_t)(f * (float)w->length + 0.5f)};
}

/* Whether s's index is still in w: the list of clips is read again at O
   and before an export, and may be shorter than when the pointer moved. */
static bool spot_holds(const strip_spot *s, const watch_state *w) {
    switch (s->on) {
    case STRIP_ON_MARK: return s->index >= 0 && (size_t)s->index < w->nmarks;
    case STRIP_ON_CLIP: return s->index >= 0 && (size_t)s->index < w->nclips;
    default: return true;
    }
}

static const char *whole_seconds(uint32_t ticks, char *buf, size_t cap) {
    return clip_format_time(ticks - ticks % 60, buf, cap);
}

void strip_spot_tip(const strip_spot *s, const watch_state *w, char *out, size_t cap) {
    char a[32], b[32];
    out[0] = '\0';
    if (!spot_holds(s, w))
        return;
    switch (s->on) {
    case STRIP_ON_TIME: snprintf(out, cap, "%s", whole_seconds(s->tick, a, sizeof a)); break;
    case STRIP_ON_MARK:
        snprintf(out, cap, "Mark %d: %s", s->index + 1, whole_seconds(s->tick, a, sizeof a));
        break;
    case STRIP_ON_CLIP: {
        const clip_span *c = &w->clips[s->index];
        snprintf(out, cap, "Clip %d: %s-%s", s->index + 1, whole_seconds(c->start, a, sizeof a),
                 whole_seconds(c->end, b, sizeof b));
        break;
    }
    default: break;
    }
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
    bool was_over = S.over;
    S.over = in && inside(l->strip, x, y);
    if (in || was_over) /* leaving the strip, its time to go starts then */
        S.active = now();
    S.spot = (strip_spot){STRIP_ON_NOTHING, -1, 0};
    if (S.over) {
        watch_state w;
        watch_get_state(&w);
        b = strip_button_at(l, x, y);
        S.spot = strip_spot_at(l, &w, x, y);
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
        return;
    }
    watch_state w;
    watch_get_state(&w);
    strip_spot s = strip_spot_at(l, &w, x, y);
    if (s.on != STRIP_ON_NOTHING)
        watch_go_to(s.tick);
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
    if (!spot_holds(&S.spot, &w)) /* the clip under the pointer is gone */
        S.spot = (strip_spot){STRIP_ON_NOTHING, -1, 0};
    if (op > 0)
        k.spot = S.spot;
    else
        k.spot = (strip_spot){STRIP_ON_NOTHING, -1, 0};
    k.have_in = w.have_in;
    k.in = w.have_in ? w.in : 0;
    k.nmarks = w.nmarks;
    k.nclips = w.nclips;
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

/* A diamond, half h wide and tall, centered on (cx, cy), in the draw color. */
static void diamond(SDL_Renderer *r, float cx, float cy, float h) {
    Uint8 c[4];
    SDL_GetRenderDrawColor(r, &c[0], &c[1], &c[2], &c[3]);
    SDL_FColor fc = {c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, c[3] / 255.f};
    SDL_Vertex v[4] = {
        {{cx, cy - h}, fc, {0, 0}},
        {{cx + h, cy}, fc, {0, 0}},
        {{cx, cy + h}, fc, {0, 0}},
        {{cx - h, cy}, fc, {0, 0}},
    };
    static const int ix[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(r, NULL, v, 4, ix, 6);
}

/* A tip above the strip, from x, kept in the window. */
static void draw_tip(SDL_Renderer *r, const display_layout *l, float k, float x, const char *t,
                     float op) {
    float ch = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * k, pad = ch / 2;
    display_rect tip = {x, l->strip.y - ch - 3 * pad, (float)strlen(t) * ch + 2 * pad, ch + 2 * pad};
    if (tip.x + tip.w > l->strip.x + l->strip.w)
        tip.x = l->strip.x + l->strip.w - tip.w;
    if (tip.x < 0)
        tip.x = 0;
    SDL_SetRenderDrawColor(r, 16, 16, 16, (Uint8)(op * 0.9f * 255 + 0.5f));
    fill(r, tip);
    color(r, 235, op);
    text(r, tip, k, t);
}

/* The bar: how far the replay has got, the clips, a clip's start, the
   marks, and what's under the pointer. */
static void draw_bar(SDL_Renderer *r, const display_layout *l, const places *p, const watch_state *w,
                     const look *k, float op) {
    display_rect bar = p->bar;
    float d = l->density, cy = bar.y + bar.h / 2;
    color(r, 255, 0.25f * op);
    fill(r, bar);
    SDL_SetRenderDrawColor(r, 220, 50, 40, (Uint8)(op * 255 + 0.5f));
    fill(r, (display_rect){bar.x, bar.y, x_of(bar, w, w->now) - bar.x, bar.h});
    float ch = 2 * bar.h; /* a clip is taller than the bar */
    for (size_t i = 0; i < w->nclips; i++) {
        float a = x_of(bar, w, w->clips[i].start), b = x_of(bar, w, w->clips[i].end);
        bool on = k->spot.on == STRIP_ON_CLIP && k->spot.index == (int)i;
        SDL_SetRenderDrawColor(r, 240, 190, 40, (Uint8)(op * (on ? 0.9f : 0.6f) * 255 + 0.5f));
        fill(r, (display_rect){a, cy - ch / 2, b - a > d ? b - a : d, ch});
    }
    if (w->have_in) { /* [ at the start, and the span to now lighter */
        float a = x_of(bar, w, w->in), now = x_of(bar, w, w->now), h = 3 * bar.h;
        float t = SDL_ceilf(d * 1.5f), x = SDL_floorf(a - t / 2), y = SDL_floorf(cy - h / 2);
        if (now > a) {
            SDL_SetRenderDrawColor(r, 240, 190, 40, (Uint8)(op * 0.3f * 255 + 0.5f));
            fill(r, (display_rect){a, cy - ch / 2, now - a, ch});
        }
        color(r, 235, op); /* on whole pixels, so the ends match */
        fill(r, (display_rect){x, y, t, h});
        fill(r, (display_rect){x, y, 3 * d, t});
        fill(r, (display_rect){x, y + h - t, 3 * d, t});
    }
    for (size_t i = 0; i < w->nmarks; i++) {
        float x = x_of(bar, w, w->marks[i]);
        bool on = k->spot.on == STRIP_ON_MARK && k->spot.index == (int)i;
        float h = (on ? 5 : 4) * d;
        SDL_SetRenderDrawColor(r, 16, 16, 16, (Uint8)(op * 255 + 0.5f));
        diamond(r, x, cy, h + d);
        color(r, on ? 255 : 235, op);
        diamond(r, x, cy, h);
    }
    if (k->spot.on == STRIP_ON_NOTHING)
        return;
    float x = x_of(bar, w, k->spot.tick);
    if (k->spot.on == STRIP_ON_TIME) { /* a caret where a click goes */
        color(r, 255, 0.8f * op);
        fill(r, (display_rect){x - d / 2, cy - 2 * bar.h, d, 4 * bar.h});
    }
    char t[64];
    strip_spot_tip(&k->spot, w, t, sizeof t);
    float cw = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * p->k;
    draw_tip(r, l, p->k, x - ((float)strlen(t) + 1) * cw / 2, t, op); /* centered on the spot */
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
    if (p.bar.w > 0)
        draw_bar(r, l, &p, &w, k, op);
    if (k->tip) { /* above the strip, from the button's left */
        strip_tip(k->hover, t, sizeof t);
        draw_tip(r, l, p.k, p.button[k->hover].x, t, op);
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
