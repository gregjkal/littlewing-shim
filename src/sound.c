#include "sound.h"

#include <string.h>

#include "guest_mem.h"
#include "memmgr.h"
#include "misc.h"
#include "trap.h"
#include "util.h"

#define MAX_CHANNELS 16
#define QUEUE_LEN 128
/* SndChannel: nextChan, firstMod, callBack (+8), userInfo (+12), wait,
   cmdInProgress (+20, 8 bytes), flags, qLength, qHead, qTail, queue. */
#define CHAN_CALLBACK 8
#define CHAN_CMD_IN_PROGRESS 20
#define SND_CHANNEL_SIZE (36 + 8 * QUEUE_LEN)

typedef struct {
    uint16_t cmd, param1;
    uint32_t param2;
} snd_cmd;

static struct {
    struct {
        uint32_t addr; /* guest SndChannel, 0 = free slot */
        bool ours;     /* allocated by SndNewChannel */
        snd_cmd queue[QUEUE_LEN];
        int head, count;
        double busy_until; /* seconds */
        uint16_t volume_l, volume_r;
    } ch[MAX_CHANNELS];
    bool warned[256];
} SND;

static double now(void) { return misc_seconds(); }

void sound_init(void) { memset(&SND, 0, sizeof SND); }

static int find(uint32_t chan) {
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (chan && SND.ch[i].addr == chan)
            return i;
    return -1;
}

static void warn_once(uint16_t cmd) {
    if (!SND.warned[cmd & 0xFF]) {
        SND.warned[cmd & 0xFF] = true;
        log_msg("sound: command %u is not supported (ignored)", cmd);
    }
}

double sound_header_seconds(uint32_t h) {
    uint32_t rate = gm_r32(h + 8); /* Fixed 16.16 */
    uint8_t encode = gm_r8(h + 20);
    uint32_t frames;
    if (encode == 0x00) /* stdSH: length is the frame count (8-bit mono) */
        frames = gm_r32(h + 4);
    else if (encode == 0xFF || encode == 0xFE) /* extSH / cmpSH */
        frames = gm_r32(h + 22);
    else {
        log_msg("sound: unknown sound header encoding 0x%02x", encode);
        return 0;
    }
    if (rate < 0x10000)
        return 0;
    return frames / (rate / 65536.0);
}

static void deliver_callback(int i, snd_cmd c) {
    uint32_t chan = SND.ch[i].addr, proc = gm_r32(chan + CHAN_CALLBACK);
    if (!proc)
        return;
    uint32_t cmd_p = chan + CHAN_CMD_IN_PROGRESS;
    gm_w16(cmd_p, c.cmd);
    gm_w16(cmd_p + 2, c.param1);
    gm_w32(cmd_p + 4, c.param2);
    uint32_t args[2] = {chan, cmd_p};
    guest_call(proc, 2, args);
}

/* Executes one command now. */
static void run(int i, snd_cmd c) {
    switch (c.cmd) {
    case SND_NULL_CMD:
    case SND_SOUND_CMD: /* installs a voice; doesn't play */
        break;
    case SND_QUIET_CMD:
        SND.ch[i].busy_until = 0;
        break;
    case SND_FLUSH_CMD:
        SND.ch[i].count = 0;
        break;
    case SND_WAIT_CMD: /* param1 in half-milliseconds */
        SND.ch[i].busy_until = now() + c.param1 / 2000.0;
        break;
    case SND_BUFFER_CMD:
        SND.ch[i].busy_until = now() + sound_header_seconds(c.param2);
        break;
    case SND_VOLUME_CMD:
        SND.ch[i].volume_l = (uint16_t)(c.param2 & 0xFFFF);
        SND.ch[i].volume_r = (uint16_t)(c.param2 >> 16);
        break;
    case SND_CALLBACK_CMD:
        deliver_callback(i, c);
        break;
    default:
        warn_once(c.cmd);
        break;
    }
}

void sound_pump(void) {
    for (int i = 0; i < MAX_CHANNELS; i++) {
        while (SND.ch[i].addr && SND.ch[i].count > 0 && now() >= SND.ch[i].busy_until) {
            snd_cmd c = SND.ch[i].queue[SND.ch[i].head];
            SND.ch[i].head = (SND.ch[i].head + 1) % QUEUE_LEN;
            SND.ch[i].count--;
            run(i, c);
        }
    }
}

static snd_cmd read_cmd(uint32_t p) { return (snd_cmd){gm_r16(p), gm_r16(p + 2), gm_r32(p + 4)}; }

static void h_new_snd_callback_upp(void) { trap_return(trap_arg(0)); }

/* SndNewChannel(SndChannelPtr *chan, short synth, long init, SndCallBackUPP userRoutine) */
static void h_snd_new_channel(void) {
    uint32_t pp = trap_arg(0);
    int i;
    for (i = 0; i < MAX_CHANNELS && SND.ch[i].addr; i++)
        ;
    if (i == MAX_CHANNELS)
        trap_crash("SndNewChannel: more than %d channels", MAX_CHANNELS);
    uint32_t chan = gm_r32(pp);
    bool ours = chan == 0;
    if (ours) {
        chan = mm_new_ptr(SND_CHANNEL_SIZE, true);
        if (!chan)
            trap_crash("SndNewChannel: out of guest memory");
        gm_w32(pp, chan);
    }
    gm_w32(chan + CHAN_CALLBACK, trap_arg(3));
    memset(&SND.ch[i], 0, sizeof SND.ch[i]);
    SND.ch[i].addr = chan;
    SND.ch[i].ours = ours;
    SND.ch[i].volume_l = SND.ch[i].volume_r = 0x100;
    trap_return(0);
}

static void h_snd_dispose_channel(void) {
    int i = find(trap_arg(0));
    if (i < 0) {
        trap_return((uint32_t)SND_BAD_CHANNEL_ERR);
        return;
    }
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
    if (SND.ch[i].count == QUEUE_LEN) {
        trap_return((uint32_t)SND_QUEUE_FULL_ERR);
        return;
    }
    SND.ch[i].queue[(SND.ch[i].head + SND.ch[i].count) % QUEUE_LEN] = read_cmd(trap_arg(1));
    SND.ch[i].count++;
    trap_return(0);
}

static void h_snd_do_immediate(void) {
    int i = find(trap_arg(0));
    if (i < 0) {
        trap_return((uint32_t)SND_BAD_CHANNEL_ERR);
        return;
    }
    run(i, read_cmd(trap_arg(1)));
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
    memset(gm_ptr(st, 24), 0, 24);
    gm_w8(st + 12, now() < SND.ch[i].busy_until || SND.ch[i].count > 0);
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
