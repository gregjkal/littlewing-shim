#include "sound.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "guest_mem.h"
#include "snapshot.h"
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
    uint32_t play_chan;      /* SndPlay's own channel, once it has one */
    SDL_AudioStream *stream; /* NULL: clock-driven */
    SDL_AudioStream *monitor; /* clock-driven, and played too (sound_open_monitor) */
    bool monitor_on;
    uint64_t rendered;       /* clock-driven: frames rendered since sound_init */
    double render_seconds;   /* and the wall clock spent rendering them */
    wav_file wav;
    bool recording;
    int volume;
    sound_tap_fn tap;
    sound_source_fn source;
    bool in_pump; /* the source renders only from sound_pump */
    uint32_t lead_ms; /* how far behind the monitor plays */
} SND = {.volume = SOUND_DEFAULT_VOLUME, .lead_ms = SOUND_MONITOR_LEAD_MS};

/* A snapshot's walk (snapshot.h): what is the host's and not the game's
   stays as it is: the streams, the volume, a recording of the output, the
   tap, and the wall clock spent rendering. */
static void snap_walk(snapshot_walker *w) {
    snapshot_bytes(w, &SND, sizeof SND);
    SNAPSHOT_HOST(w, SND.stream);
    SNAPSHOT_HOST(w, SND.monitor);
    SNAPSHOT_HOST(w, SND.monitor_on);
    SNAPSHOT_HOST(w, SND.render_seconds);
    SNAPSHOT_HOST(w, SND.wav);
    SNAPSHOT_HOST(w, SND.recording);
    SNAPSHOT_HOST(w, SND.volume);
    SNAPSHOT_HOST(w, SND.tap);
    SNAPSHOT_HOST(w, SND.source);
    SNAPSHOT_HOST(w, SND.in_pump);
    SNAPSHOT_HOST(w, SND.lead_ms);
}
SNAPSHOT_STATE_WALK(SND, snap_walk)

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
   replay stalled), silence first keeps it the lead (SOUND_MONITOR_LEAD_MS,
   or SOUND_SOURCE_LEAD_MS for a source) ahead of the device, so the next
   frames come before these have played. For a source, when the game's
   clock has run ahead of the device's by four leads, what waits is
   dropped (catch_up drops a stall's frames before they get here). */
static void monitor(const int16_t *buf, uint32_t n) {
    static const int16_t silence[2 * MIX_RATE * SOUND_SOURCE_LEAD_MS / 1000];
    _Static_assert(SOUND_SOURCE_LEAD_MS >= SOUND_MONITOR_LEAD_MS, "the silence covers both leads");
    int lead = (int)(4 * MIX_RATE * SND.lead_ms / 1000);
    int queued = SDL_GetAudioStreamQueued(SND.monitor);
    if (SND.source && queued > 4 * lead) {
        SDL_ClearAudioStream(SND.monitor);
        queued = 0;
    }
    if (queued == 0)
        SDL_PutAudioStreamData(SND.monitor, silence, lead);
    SDL_PutAudioStreamData(SND.monitor, buf, (int)(n * 4));
}

/* Renders frames, the first of them frame at of the game's sound (counted
   from sound_init) when the clock drives it, and plays them on the monitor
   if heard. */
static void render(uint32_t frames, uint64_t at, bool heard) {
    int16_t buf[2 * RENDER_FRAMES];
    while (frames > 0) {
        uint32_t n = frames < RENDER_FRAMES ? frames : RENDER_FRAMES;
        mix_render(buf, n);
        if (SND.source && SND.in_pump)
            SND.source(buf, n);
        if (SND.stream)
            SDL_PutAudioStreamData(SND.stream, buf, (int)(n * 4));
        if (SND.monitor_on && heard)
            monitor(buf, n);
        if (SND.recording)
            wav_write(&SND.wav, buf, n);
        if (SND.tap)
            SND.tap(buf, n, at);
        frames -= n;
        at += n;
    }
}

/* SDL's audio thread, with the stream locked. */
static void SDLCALL feed(void *unused, SDL_AudioStream *stream, int additional, int total) {
    (void)unused, (void)stream, (void)total;
    if (additional > 0)
        render((uint32_t)additional / 4, 0, true);
}

/* Clock-driven: renders the frames between the last call and now. On a
   recording or a replay, now is when the game last read the clock: reading
   it here would be a step, which a recording with an audio device doesn't
   take. A source's frames that come after a stall of more than two leads
   are late already: the monitor drops them rather than stay behind by them,
   and on the real clock, more than a second of them (the Mac slept) isn't
   rendered at all, since the game's render callbacks would run through all
   of it. */
static void catch_up(void) {
    if (SND.stream)
        return;
    bool real = !replay_recording() && !replay_playing();
    double now = real ? misc_seconds() : misc_last_seconds();
    uint64_t target = (uint64_t)(now * MIX_RATE);
    if (SND.source && real && !misc_fixed_clock() && target > SND.rendered + MIX_RATE)
        SND.rendered = target;
    if (target > SND.rendered) {
        uint64_t frames = target - SND.rendered;
        bool late = SND.source && frames > 2 * MIX_RATE * SND.lead_ms / 1000;
        if (late && SND.monitor_on)
            SDL_ClearAudioStream(SND.monitor);
        double t0 = wall_seconds();
        render((uint32_t)frames, SND.rendered, !late);
        SND.rendered = target;
        SND.render_seconds += wall_seconds() - t0;
    }
}

double sound_render_seconds(void) { return SND.render_seconds; }

double sound_monitor_queued_seconds(void) {
    return SND.monitor ? SDL_GetAudioStreamQueued(SND.monitor) / (4.0 * MIX_RATE) : 0;
}

void sound_init(void) {
    mix_init();
    memset(SND.ch, 0, sizeof SND.ch);
    memset(SND.warned, 0, sizeof SND.warned);
    memset(SND.warned_encoding, 0, sizeof SND.warned_encoding);
    SND.play_chan = 0;
    SND.rendered = 0;
    SND.render_seconds = 0;
}

static void play_clock_driven(void);

static void shutdown_output(void) {
    SND.source = NULL; /* the game has ended: its code doesn't run again */
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
    if (SND.source) {
        play_clock_driven();
        return;
    }
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

/* A source's sound: rendered as the clock goes (catch_up) and played
   behind it, from now. */
static void play_clock_driven(void) {
    if (SND.stream) {
        SDL_DestroyAudioStream(SND.stream);
        SND.stream = NULL;
    }
    double now = replay_recording() || replay_playing() ? misc_last_seconds() : misc_seconds();
    SND.rendered = (uint64_t)(now * MIX_RATE);
    SND.lead_ms = SOUND_SOURCE_LEAD_MS;
    sound_open_monitor();
    sound_monitor(true);
}

void sound_set_source(sound_source_fn fn) {
    SND.source = fn;
    if (fn && SND.stream)
        play_clock_driven();
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
    SND.in_pump = true;
    catch_up();
    SND.in_pump = false;
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

/* Opens a channel at chan (a new one, ours, if 0) with callback proc, and
   returns its address. */
static uint32_t open_channel(const char *call, uint32_t chan, uint32_t proc) {
    int i;
    for (i = 0; i < MIX_CHANNELS && SND.ch[i].addr; i++)
        ;
    if (i == MIX_CHANNELS)
        trap_crash("%s: more than %d channels", call, MIX_CHANNELS);
    bool ours = chan == 0;
    if (ours) {
        chan = mm_new_ptr(SND_CHANNEL_SIZE, true);
        if (!chan)
            trap_crash("%s: out of guest memory", call);
    }
    gm_w32(chan + CHAN_CALLBACK, proc);
    SND.ch[i].addr = chan;
    SND.ch[i].ours = ours;
    lock();
    mix_open(i);
    unlock();
    return chan;
}

/* SndNewChannel(SndChannelPtr *chan, short synth, long init, SndCallBackUPP userRoutine) */
static void h_snd_new_channel(void) {
    uint32_t pp = trap_arg(0);
    gm_w32(pp, open_channel("SndNewChannel", gm_r32(pp), trap_arg(3)));
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

/* Queues m on channel i, a step of the replay (whether the queue was
   full). Returns an OSErr. */
static int16_t queue(int i, mix_cmd m) {
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
    return ok ? 0 : SND_QUEUE_FULL_ERR;
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
    trap_return((uint32_t)(int32_t)queue(i, m));
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

/* Where an 'snd ' resource's sampled sound header is, from the start of
   its data: the offset its first soundCmd or bufferCmd with the data
   pointer flag gives, in a format 1 or 2 resource. -1 if it has none. */
#define SND_DATA_POINTER_FLAG 0x8000
static int32_t header_offset(uint32_t h) {
    if (!mm_is_handle(h))
        return -1;
    uint32_t len = mm_handle_size(h), p = gm_r32(h), at;
    if (len < 4)
        return -1;
    uint16_t format = gm_r16(p);
    if (format == 1) {
        at = 4 + 6u * gm_r16(p + 2); /* past the modifiers */
    } else if (format == 2) {
        at = 4;
    } else {
        return -1;
    }
    if (at + 2 > len)
        return -1;
    uint32_t n = gm_r16(p + at);
    for (uint32_t k = 0; k < n && at + 2 + 8 * (k + 1) <= len; k++) {
        uint32_t c = p + at + 2 + 8 * k;
        uint16_t cmd = gm_r16(c);
        uint32_t off = gm_r32(c + 4);
        if ((cmd & SND_DATA_POINTER_FLAG) &&
            ((cmd & ~SND_DATA_POINTER_FLAG) == SND_SOUND_CMD || (cmd & ~SND_DATA_POINTER_FLAG) == SND_BUFFER_CMD) &&
            off < len)
            return (int32_t)off;
    }
    return -1;
}

/* GetSoundHeaderOffset(SndListHandle, long *offset) */
static void h_get_sound_header_offset(void) {
    int32_t off = header_offset(trap_arg(0));
    gm_w32(trap_arg(1), off < 0 ? 0 : (uint32_t)off);
    trap_return(off < 0 ? (uint32_t)SND_BAD_FORMAT_ERR : 0);
}

/* SndPlay(SndChannelPtr chan, SndListHandle, Boolean async): queues the
   resource's sound on chan, or with none on a channel of SndPlay's own,
   where one sound follows another. It never waits for the sound to end, so
   a synchronous play returns at once. */
static void h_snd_play(void) {
    uint32_t chan = trap_arg(0), h = trap_arg(1);
    int32_t off = header_offset(h);
    if (off < 0) {
        log_msg("sound: SndPlay of 0x%08x, which has no sampled sound (ignored)", h);
        trap_return((uint32_t)SND_BAD_FORMAT_ERR);
        return;
    }
    if (!chan) {
        if (!SND.play_chan)
            SND.play_chan = open_channel("SndPlay", 0, 0);
        chan = SND.play_chan;
    }
    int i = find(chan);
    if (i < 0) {
        trap_return((uint32_t)SND_BAD_CHANNEL_ERR);
        return;
    }
    mix_cmd m = {.kind = MIX_BUFFER, .sound = copy_sound(gm_r32(h) + (uint32_t)off)};
    if (!m.sound) {
        trap_return(0);
        return;
    }
    trap_return((uint32_t)(int32_t)queue(i, m));
}

/* SndSoundManagerVersion() -> NumVersion: 3.6, final. */
static void h_snd_sound_manager_version(void) { trap_return(0x03608000u); }

void sound_register(void) {
    trap_register("GetSoundHeaderOffset", h_get_sound_header_offset);
    trap_register("SndPlay", h_snd_play);
    trap_register("SndSoundManagerVersion", h_snd_sound_manager_version);
    trap_register("NewSndCallBackUPP", h_new_snd_callback_upp);
    trap_register("SndNewChannel", h_snd_new_channel);
    trap_register("SndDisposeChannel", h_snd_dispose_channel);
    trap_register("SndDoCommand", h_snd_do_command);
    trap_register("SndDoImmediate", h_snd_do_immediate);
    trap_register("SndChannelStatus", h_snd_channel_status);
}
