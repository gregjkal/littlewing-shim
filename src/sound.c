#include "sound.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "guest_mem.h"
#include "memmgr.h"
#include "misc.h"
#include "mixer.h"
#include "replay.h"
#include "trap.h"
#include "util.h"
#include "wav.h"

/* SndChannel: nextChan, firstMod, callBack (+8), userInfo (+12), wait,
   cmdInProgress (+20, 8 bytes), flags, qLength, qHead, qTail, queue. */
#define CHAN_CALLBACK 8
#define CHAN_CMD_IN_PROGRESS 20
#define SND_CHANNEL_SIZE (36 + 8 * MIX_QUEUE_LEN)
#define RENDER_FRAMES 1024

static struct {
    struct {
        uint32_t addr; /* guest SndChannel, 0 = free slot */
        bool ours;     /* allocated by SndNewChannel */
    } ch[MIX_CHANNELS];
    bool warned[256], warned_encoding[256];
    SDL_AudioStream *stream; /* NULL: clock-driven */
    SDL_AudioStream *monitor; /* clock-driven, and played too (sound_open_monitor) */
    bool monitor_on;
    uint64_t rendered;       /* clock-driven: frames rendered since sound_init */
    wav_file wav;
    bool recording;
    int volume;
    sound_tap_fn tap;
} SND = {.volume = SOUND_DEFAULT_VOLUME};

/* The audio thread holds the stream's lock while it renders. */
static void lock(void) {
    if (SND.stream)
        SDL_LockAudioStream(SND.stream);
}

static void unlock(void) {
    if (SND.stream)
        SDL_UnlockAudioStream(SND.stream);
}

/* Plays n frames on the monitor. When it has run dry (at first, or the
   replay stalled), silence first keeps it SOUND_MONITOR_LEAD_MS ahead of the
   device, so the next frames come before these have played. */
static void monitor(const int16_t *buf, uint32_t n) {
    static const int16_t silence[2 * MIX_RATE * SOUND_MONITOR_LEAD_MS / 1000];
    if (SDL_GetAudioStreamQueued(SND.monitor) == 0)
        SDL_PutAudioStreamData(SND.monitor, silence, (int)sizeof silence);
    SDL_PutAudioStreamData(SND.monitor, buf, (int)(n * 4));
}

static void render(uint32_t frames) {
    int16_t buf[2 * RENDER_FRAMES];
    while (frames > 0) {
        uint32_t n = frames < RENDER_FRAMES ? frames : RENDER_FRAMES;
        mix_render(buf, n);
        if (SND.stream)
            SDL_PutAudioStreamData(SND.stream, buf, (int)(n * 4));
        if (SND.monitor_on)
            monitor(buf, n);
        if (SND.recording)
            wav_write(&SND.wav, buf, n);
        if (SND.tap)
            SND.tap(buf, n);
        frames -= n;
    }
}

/* SDL's audio thread, with the stream locked. */
static void SDLCALL feed(void *unused, SDL_AudioStream *stream, int additional, int total) {
    (void)unused, (void)stream, (void)total;
    if (additional > 0)
        render((uint32_t)additional / 4);
}

/* Clock-driven: renders the frames between the last call and now. On a
   recording or a replay, now is when the game last read the clock: reading
   it here would be a step, which a recording with an audio device doesn't
   take. */
static void catch_up(void) {
    if (SND.stream)
        return;
    double now = replay_recording() || replay_playing() ? misc_last_seconds() : misc_seconds();
    uint64_t target = (uint64_t)(now * MIX_RATE);
    if (target > SND.rendered) {
        render((uint32_t)(target - SND.rendered));
        SND.rendered = target;
    }
}

void sound_init(void) {
    mix_init();
    memset(SND.ch, 0, sizeof SND.ch);
    memset(SND.warned, 0, sizeof SND.warned);
    memset(SND.warned_encoding, 0, sizeof SND.warned_encoding);
    SND.rendered = 0;
}

static void shutdown_output(void) {
    if (SND.monitor) {
        SDL_DestroyAudioStream(SND.monitor);
        SND.monitor = NULL;
        SND.monitor_on = false;
    }
    if (SND.stream) {
        SDL_DestroyAudioStream(SND.stream);
        SND.stream = NULL;
    } else {
        catch_up();
    }
    if (SND.recording) {
        SND.recording = false;
        if (!wav_close(&SND.wav))
            log_msg("sound: can't finish the recording %s", getenv("LOONY_WAV"));
    }
}

void sound_start_output(void) {
    const char *wav = getenv("LOONY_WAV");
    if (wav && *wav) {
        if (wav_open(&SND.wav, wav, MIX_RATE))
            SND.recording = true;
        else
            log_msg("sound: can't create the recording %s", wav);
    }
    atexit(shutdown_output);
    if (misc_fixed_clock() || replay_playing())
        return;
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        log_msg("sound: no audio output: %s (continuing silently)", SDL_GetError());
        return;
    }
    const SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, MIX_RATE};
    SDL_AudioStream *s =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, NULL);
    if (!s) {
        log_msg("sound: no audio output: %s (continuing silently)", SDL_GetError());
        return;
    }
    catch_up(); /* the clock-driven frames so far */
    SDL_SetAudioStreamGain(s, sound_gain(SND.volume));
    SND.stream = s;
    SDL_ResumeAudioStreamDevice(s);
}

void sound_open_monitor(void) {
    if (SND.stream || SND.monitor)
        return;
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        log_msg("sound: no audio output: %s (continuing silently)", SDL_GetError());
        return;
    }
    const SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, MIX_RATE};
    SND.monitor = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!SND.monitor) {
        log_msg("sound: no audio output: %s (continuing silently)", SDL_GetError());
        return;
    }
    SDL_SetAudioStreamGain(SND.monitor, sound_gain(SND.volume));
    SDL_ResumeAudioStreamDevice(SND.monitor);
}

void sound_monitor(bool on) {
    if (!SND.monitor || on == SND.monitor_on)
        return;
    SND.monitor_on = on;
    if (!on)
        SDL_ClearAudioStream(SND.monitor);
}

float sound_gain(int percent) {
    float v = (float)percent / 100.0f;
    return v * v * v;
}

void sound_set_volume(int percent) {
    SND.volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    if (SND.stream)
        SDL_SetAudioStreamGain(SND.stream, sound_gain(SND.volume));
    if (SND.monitor)
        SDL_SetAudioStreamGain(SND.monitor, sound_gain(SND.volume));
}

int sound_volume(void) { return SND.volume; }

void sound_set_tap(sound_tap_fn fn) { SND.tap = fn; }

static int find(uint32_t chan) {
    for (int i = 0; i < MIX_CHANNELS; i++)
        if (chan && SND.ch[i].addr == chan)
            return i;
    return -1;
}

static void warn_once(bool *flags, uint8_t key, const char *what) {
    if (!flags[key]) {
        flags[key] = true;
        log_msg("sound: %s %u is not supported (ignored)", what, key);
    }
}

/* Copies a sampled sound header's sound to the host: a standard header (8-bit
   mono) or an extended one (8 or 16 bits, mono or stereo). NULL, logged, for
   anything else. */
static mix_sound *copy_sound(uint32_t h) {
    if (!gm_is_backed(h, 22)) {
        log_msg("sound: sound header at 0x%08x is outside memory (ignored)", h);
        return NULL;
    }
    uint32_t ptr = gm_r32(h), rate = gm_r32(h + 8);
    uint8_t encode = gm_r8(h + 20);
    uint32_t frames, data, nch = 1, bits = 8;
    if (encode == 0x00) { /* stdSH: length is the frame count */
        frames = gm_r32(h + 4);
        data = ptr ? ptr : h + 22;
    } else if (encode == 0xFF && gm_is_backed(h, 64)) { /* extSH */
        nch = gm_r32(h + 4);
        frames = gm_r32(h + 22);
        bits = gm_r16(h + 48);
        data = ptr ? ptr : h + 64;
    } else {
        warn_once(SND.warned_encoding, encode, "sound header encoding");
        return NULL;
    }
    if (rate < 0x10000 || (nch != 1 && nch != 2) || (bits != 8 && bits != 16)) {
        log_msg("sound: sound header at 0x%08x has rate 0x%08x, %u channels, %u bits (ignored)",
                h, rate, nch, bits);
        return NULL;
    }
    uint64_t bytes = (uint64_t)frames * nch * (bits / 8);
    if (frames == 0)
        return NULL;
    if (bytes > UINT32_MAX || !gm_is_backed(data, (uint32_t)bytes)) {
        log_msg("sound: the samples of the sound header at 0x%08x are outside memory (ignored)", h);
        return NULL;
    }
    mix_sound *s = mix_sound_new(frames, (int)nch, rate);
    if (!s) {
        log_msg("sound: out of memory for a %u-frame sound (ignored)", frames);
        return NULL;
    }
    const uint8_t *p = gm_ptr(data, (uint32_t)bytes);
    for (uint32_t k = 0; k < frames * nch; k++)
        s->samples[k] = bits == 8 ? (int16_t)((p[k] - 128) * 256) : (int16_t)rd_be16(p + 2 * k);
    return s;
}

/* The mixer command for a SndCommand, or false for one we don't support. */
static bool translate(uint32_t p, mix_cmd *m) {
    uint16_t cmd = gm_r16(p), param1 = gm_r16(p + 2);
    uint32_t param2 = gm_r32(p + 4);
    memset(m, 0, sizeof *m);
    m->param1 = param1;
    m->param2 = param2;
    switch (cmd) {
    case SND_NULL_CMD:
    case SND_SOUND_CMD: /* installs a voice; doesn't play */
        m->kind = MIX_NULL;
        return true;
    case SND_QUIET_CMD: m->kind = MIX_QUIET; return true;
    case SND_FLUSH_CMD: m->kind = MIX_FLUSH; return true;
    case SND_WAIT_CMD: /* param1 in half-milliseconds */
        m->kind = MIX_WAIT;
        m->frames = (uint32_t)((uint64_t)param1 * MIX_RATE / 2000);
        return true;
    case SND_CALLBACK_CMD: m->kind = MIX_CALLBACK; return true;
    case SND_VOLUME_CMD: m->kind = MIX_VOLUME; return true;
    case SND_BUFFER_CMD:
        m->sound = copy_sound(param2);
        m->kind = m->sound ? MIX_BUFFER : MIX_NULL;
        return true;
    default:
        warn_once(SND.warned, (uint8_t)cmd, "command");
        return false;
    }
}

static void deliver_callback(int i, uint16_t param1, uint32_t param2) {
    uint32_t chan = SND.ch[i].addr, proc = chan ? gm_r32(chan + CHAN_CALLBACK) : 0;
    if (!proc)
        return;
    uint32_t cmd_p = chan + CHAN_CMD_IN_PROGRESS;
    gm_w16(cmd_p, SND_CALLBACK_CMD);
    gm_w16(cmd_p + 2, param1);
    gm_w32(cmd_p + 4, param2);
    uint32_t args[2] = {chan, cmd_p};
    guest_call(proc, 2, args);
}

/* On a replay the callbacks come from its file, when the recording
   delivered them; the mixer's own are dropped. */
void sound_pump(void) {
    catch_up();
    mix_callback cb;
    if (replay_playing()) {
        while (mix_take_callback(&cb))
            ;
        uint32_t v[3];
        while (replay_take(REPLAY_SOUND_CALLBACK, v)) {
            if (v[0] >= MIX_CHANNELS)
                replay_desync("a sound callback for channel %u, of %d", v[0], MIX_CHANNELS);
            deliver_callback((int)v[0], (uint16_t)v[1], v[2]);
        }
        return;
    }
    for (;;) {
        lock();
        bool got = mix_take_callback(&cb);
        unlock();
        if (!got)
            return;
        replay_log(REPLAY_SOUND_CALLBACK, (uint32_t[3]){(uint32_t)cb.channel, cb.param1, cb.param2});
        deliver_callback(cb.channel, cb.param1, cb.param2);
    }
}

static void h_new_snd_callback_upp(void) { trap_return(trap_arg(0)); }

/* SndNewChannel(SndChannelPtr *chan, short synth, long init, SndCallBackUPP userRoutine) */
static void h_snd_new_channel(void) {
    uint32_t pp = trap_arg(0);
    int i;
    for (i = 0; i < MIX_CHANNELS && SND.ch[i].addr; i++)
        ;
    if (i == MIX_CHANNELS)
        trap_crash("SndNewChannel: more than %d channels", MIX_CHANNELS);
    uint32_t chan = gm_r32(pp);
    bool ours = chan == 0;
    if (ours) {
        chan = mm_new_ptr(SND_CHANNEL_SIZE, true);
        if (!chan)
            trap_crash("SndNewChannel: out of guest memory");
        gm_w32(pp, chan);
    }
    gm_w32(chan + CHAN_CALLBACK, trap_arg(3));
    SND.ch[i].addr = chan;
    SND.ch[i].ours = ours;
    lock();
    mix_open(i);
    unlock();
    trap_return(0);
}

/* SndDisposeChannel(chan, Boolean quietNow): stops at once either way (the
   game always passes quietNow = true). */
static void h_snd_dispose_channel(void) {
    int i = find(trap_arg(0));
    if (i < 0) {
        trap_return((uint32_t)SND_BAD_CHANNEL_ERR);
        return;
    }
    lock();
    mix_close(i);
    unlock();
    if (SND.ch[i].ours)
        mm_dispose_ptr(SND.ch[i].addr);
    SND.ch[i].addr = 0;
    trap_return(0);
}

/* SndDoCommand(chan, const SndCommand *cmd, Boolean noWait) */
static void h_snd_do_command(void) {
    int i = find(trap_arg(0));
    if (i < 0) {
        trap_return((uint32_t)SND_BAD_CHANNEL_ERR);
        return;
    }
    mix_cmd m;
    if (!translate(trap_arg(1), &m) || m.kind == MIX_NULL) { /* nothing to queue */
        trap_return(0);
        return;
    }
    catch_up();
    replay_step();
    bool full = replay_playing() && replay_take(REPLAY_SOUND_FULL, NULL);
    lock();
    bool ok = !full && mix_queue(i, m);
    unlock();
    if (!ok)
        mix_sound_free(m.sound);
    if (replay_playing()) /* the replay's mixer may differ: the recording's answer counts */
        ok = !full;
    else if (!ok)
        replay_log(REPLAY_SOUND_FULL, NULL);
    trap_return(ok ? 0 : (uint32_t)SND_QUEUE_FULL_ERR);
}

static void h_snd_do_immediate(void) {
    int i = find(trap_arg(0));
    if (i < 0) {
        trap_return((uint32_t)SND_BAD_CHANNEL_ERR);
        return;
    }
    mix_cmd m;
    if (translate(trap_arg(1), &m)) {
        if (m.kind == MIX_CALLBACK) {
            deliver_callback(i, m.param1, m.param2);
        } else {
            catch_up();
            lock();
            mix_immediate(i, m);
            unlock();
        }
    }
    trap_return(0);
}

/* SndChannelStatus(chan, short theLength, SCStatus *status): 24 bytes; the
   busy flag is at +12. */
static void h_snd_channel_status(void) {
    int i = find(trap_arg(0));
    if (i < 0) {
        trap_return((uint32_t)SND_BAD_CHANNEL_ERR);
        return;
    }
    int16_t len = (int16_t)trap_arg(1);
    uint32_t st = trap_arg(2);
    if (len < 24)
        trap_crash("SndChannelStatus: status length %d is too small", len);
    catch_up();
    replay_step();
    lock();
    bool busy = mix_busy(i);
    unlock();
    if (replay_playing())
        busy = replay_take(REPLAY_SOUND_BUSY, NULL);
    else if (busy)
        replay_log(REPLAY_SOUND_BUSY, NULL);
    memset(gm_ptr(st, 24), 0, 24);
    gm_w8(st + 12, busy);
    trap_return(0);
}

void sound_register(void) {
    trap_register("NewSndCallBackUPP", h_new_snd_callback_upp);
    trap_register("SndNewChannel", h_snd_new_channel);
    trap_register("SndDisposeChannel", h_snd_dispose_channel);
    trap_register("SndDoCommand", h_snd_do_command);
    trap_register("SndDoImmediate", h_snd_do_immediate);
    trap_register("SndChannelStatus", h_snd_channel_status);
}
