#include "mixer.h"

#include <stdlib.h>
#include <string.h>

#define CHUNK 512 /* frames mixed per pass */

typedef struct {
    bool open;
    mix_cmd queue[MIX_QUEUE_LEN];
    int head, count;
    mix_sound *voice; /* the sound playing, or NULL */
    uint64_t pos;     /* in voice, source frames as 32.32 fixed point */
    uint64_t step;    /* source frames per output frame, 32.32 */
    uint32_t wait;    /* output frames of silence left */
    int32_t vol_l, vol_r;
} channel;

static struct {
    channel ch[MIX_CHANNELS];
    mix_callback cb[MIX_CALLBACKS];
    int cb_head, cb_count;
} X;

mix_sound *mix_sound_new(uint32_t frames, int nch, uint32_t rate) {
    mix_sound *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->samples = calloc((size_t)frames * (size_t)nch + 1, sizeof *s->samples);
    if (!s->samples) {
        free(s);
        return NULL;
    }
    s->frames = frames;
    s->nch = nch;
    s->rate = rate;
    return s;
}

void mix_sound_free(mix_sound *s) {
    if (s) {
        free(s->samples);
        free(s);
    }
}

static void free_queue(channel *c) {
    for (int k = 0; k < c->count; k++)
        mix_sound_free(c->queue[(c->head + k) % MIX_QUEUE_LEN].sound);
    c->head = c->count = 0;
}

static void stop(channel *c) {
    mix_sound_free(c->voice);
    c->voice = NULL;
    c->wait = 0;
}

void mix_init(void) {
    for (int i = 0; i < MIX_CHANNELS; i++) {
        free_queue(&X.ch[i]);
        stop(&X.ch[i]);
    }
    memset(&X, 0, sizeof X);
}

static channel *get(int ch) {
    return ch >= 0 && ch < MIX_CHANNELS && X.ch[ch].open ? &X.ch[ch] : NULL;
}

void mix_open(int ch) {
    if (ch < 0 || ch >= MIX_CHANNELS)
        return;
    mix_close(ch);
    channel *c = &X.ch[ch];
    c->open = true;
    c->vol_l = c->vol_r = MIX_UNITY;
}

void mix_close(int ch) {
    if (ch < 0 || ch >= MIX_CHANNELS)
        return;
    channel *c = &X.ch[ch];
    free_queue(c);
    stop(c);
    memset(c, 0, sizeof *c);
    int kept = 0;
    for (int k = 0; k < X.cb_count; k++) {
        mix_callback cb = X.cb[(X.cb_head + k) % MIX_CALLBACKS];
        if (cb.channel != ch)
            X.cb[(X.cb_head + kept++) % MIX_CALLBACKS] = cb;
    }
    X.cb_count = kept;
}

static void start(channel *c, mix_sound *s) {
    stop(c);
    c->voice = s;
    c->pos = 0;
    c->step = ((uint64_t)s->rate << 16) / MIX_RATE;
}

/* Runs a command; returns false (doing nothing) if it is a callback and the
   callback queue is full. */
static bool exec(channel *c, int ch, mix_cmd m) {
    switch (m.kind) {
    case MIX_NULL:
        break;
    case MIX_BUFFER:
        if (m.sound)
            start(c, m.sound);
        break;
    case MIX_WAIT:
        stop(c);
        c->wait = m.frames;
        break;
    case MIX_CALLBACK:
        if (X.cb_count == MIX_CALLBACKS)
            return false;
        X.cb[(X.cb_head + X.cb_count++) % MIX_CALLBACKS] = (mix_callback){ch, m.param1, m.param2};
        break;
    case MIX_VOLUME:
        c->vol_l = (int32_t)(m.param2 & 0xFFFF);
        c->vol_r = (int32_t)(m.param2 >> 16);
        break;
    case MIX_QUIET:
        stop(c);
        break;
    case MIX_FLUSH:
        free_queue(c);
        break;
    }
    return true;
}

/* Runs queued commands until one occupies the channel (a sound or a wait),
   the queue runs dry, or a callback can't be posted. */
static void advance(channel *c, int ch) {
    while (!c->voice && !c->wait && c->count > 0) {
        mix_cmd m = c->queue[c->head]; /* popped first: a flush empties the queue */
        c->head = (c->head + 1) % MIX_QUEUE_LEN;
        c->count--;
        if (!exec(c, ch, m)) { /* put the callback back */
            c->head = (c->head + MIX_QUEUE_LEN - 1) % MIX_QUEUE_LEN;
            c->count++;
            return;
        }
    }
}

bool mix_queue(int ch, mix_cmd m) {
    channel *c = get(ch);
    if (!c) {
        mix_sound_free(m.sound);
        return true;
    }
    if (c->count == MIX_QUEUE_LEN)
        return false;
    c->queue[(c->head + c->count++) % MIX_QUEUE_LEN] = m;
    return true;
}

void mix_immediate(int ch, mix_cmd m) {
    channel *c = get(ch);
    if (!c) {
        mix_sound_free(m.sound);
        return;
    }
    if (m.kind != MIX_CALLBACK)
        exec(c, ch, m);
}

bool mix_busy(int ch) {
    channel *c = get(ch);
    return c && (c->voice || c->wait || c->count > 0);
}

static int32_t sample_at(const mix_sound *s, uint64_t pos, int side) {
    uint32_t i = (uint32_t)(pos >> 32);
    int k = side < s->nch ? side : 0;
    int32_t a = s->samples[(size_t)i * s->nch + k];
    int32_t b = i + 1 < s->frames ? s->samples[(size_t)(i + 1) * s->nch + k] : a;
    int64_t frac = (int64_t)((pos >> 16) & 0xFFFF);
    return a + (int32_t)(((int64_t)(b - a) * frac) >> 16);
}

static void render_channel(channel *c, int ch, int32_t *acc, uint32_t frames) {
    advance(c, ch);
    for (uint32_t i = 0; i < frames; i++) {
        if (c->voice) {
            mix_sound *s = c->voice;
            acc[2 * i] += (int32_t)(((int64_t)sample_at(s, c->pos, 0) * c->vol_l) >> 8);
            acc[2 * i + 1] += (int32_t)(((int64_t)sample_at(s, c->pos, 1) * c->vol_r) >> 8);
            c->pos += c->step;
            if ((c->pos >> 32) >= s->frames) {
                stop(c);
                advance(c, ch);
            }
        } else if (c->wait) {
            if (--c->wait == 0)
                advance(c, ch);
        } else {
            return;
        }
    }
}

void mix_render(int16_t *out, uint32_t frames) {
    int32_t acc[2 * CHUNK];
    while (frames > 0) {
        uint32_t n = frames < CHUNK ? frames : CHUNK;
        memset(acc, 0, sizeof acc);
        for (int ch = 0; ch < MIX_CHANNELS; ch++)
            if (X.ch[ch].open)
                render_channel(&X.ch[ch], ch, acc, n);
        for (uint32_t k = 0; k < 2 * n; k++)
            out[k] = (int16_t)(acc[k] > 32767 ? 32767 : acc[k] < -32768 ? -32768 : acc[k]);
        out += 2 * n;
        frames -= n;
    }
}

bool mix_take_callback(mix_callback *cb) {
    if (X.cb_count == 0)
        return false;
    *cb = X.cb[X.cb_head];
    X.cb_head = (X.cb_head + 1) % MIX_CALLBACKS;
    X.cb_count--;
    return true;
}
