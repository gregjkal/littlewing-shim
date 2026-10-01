#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The Sound Manager's mixer: per-channel command queues played out as 16-bit
   stereo at MIX_RATE. All host-side and integer-only, so a rendered stream is
   the same in every build. It touches no guest memory and takes no locks:
   sound.c serializes every call. */

#define MIX_RATE 44100
#define MIX_CHANNELS 16
#define MIX_QUEUE_LEN 128
#define MIX_CALLBACKS 256
#define MIX_UNITY 0x100 /* volume: 0x100 plays a sound at its recorded level */

/* A sampled sound copied out of guest memory: signed 16-bit, interleaved. */
typedef struct {
    int16_t *samples;
    uint32_t frames;
    int nch;       /* 1 or 2 */
    uint32_t rate; /* Fixed 16.16 Hz, at least 1 Hz */
} mix_sound;

typedef enum {
    MIX_NULL,
    MIX_BUFFER,   /* plays sound to its end */
    MIX_WAIT,     /* silence for frames output frames */
    MIX_CALLBACK, /* posts param1/param2 to mix_take_callback */
    MIX_VOLUME,   /* param2: left in the low word, right in the high word */
    MIX_QUIET,    /* stops the sound playing (or the wait) */
    MIX_FLUSH,    /* drops every queued command */
} mix_kind;

typedef struct {
    mix_kind kind;
    mix_sound *sound; /* MIX_BUFFER; owned by the mixer once accepted */
    uint32_t frames;  /* MIX_WAIT */
    uint16_t param1;
    uint32_t param2;
} mix_cmd;

typedef struct {
    int channel;
    uint16_t param1;
    uint32_t param2;
} mix_callback;

/* A sound of frames frames and nch channels with zeroed samples. Returns
   NULL if out of memory. */
mix_sound *mix_sound_new(uint32_t frames, int nch, uint32_t rate);
void mix_sound_free(mix_sound *s);

/* Frees every channel and pending callback. */
void mix_init(void);

/* A channel slot is open between mix_open and mix_close; commands for a
   closed slot are ignored (and their sounds freed). mix_close frees what the
   channel holds and drops its pending callbacks. */
void mix_open(int ch);
void mix_close(int ch);

/* Appends to the channel's queue. Returns false, and takes nothing, if the
   queue is full. */
bool mix_queue(int ch, mix_cmd c);

/* Runs a command now, ahead of the queue. A MIX_BUFFER replaces the sound
   playing. MIX_CALLBACK is the caller's to deliver, so it does nothing here. */
void mix_immediate(int ch, mix_cmd c);

/* True while the channel plays a sound, waits, or has queued commands. */
bool mix_busy(int ch);

/* Renders frames stereo frames into out (2 * frames samples), advancing every
   channel. A channel whose next command is a callback stalls while the
   callback queue is full. */
void mix_render(int16_t *out, uint32_t frames);

/* Takes the oldest callback the queues reached. Returns false if none. */
bool mix_take_callback(mix_callback *cb);
