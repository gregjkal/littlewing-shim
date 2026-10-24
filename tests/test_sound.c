#include "test.h"

#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "asm.h"
#include "harness.h"
#include "memmgr.h"
#include "misc.h"
#include "mixer.h"
#include "sound.h"
#include "util.h"

static const char *const names[] = {
    "NewSndCallBackUPP", "SndNewChannel", "SndDisposeChannel", "SndDoCommand", "SndDoImmediate",
    "SndChannelStatus", "SndPlay", "GetSoundHeaderOffset", "SndSoundManagerVersion",
};

#define CB      (GUEST_IMAGE_BASE + 0x200)
#define TV_CB   (GUEST_IMAGE_BASE + 0x8100)
#define GOT     (GUEST_IMAGE_BASE + 0x8200)

/* A callback that stores cmd->param2 at GOT. */
static void emit_callback(void) {
    uint32_t code[] = {
        ppc_lwz(5, 4, 4), ppc_lis(6, GOT >> 16), ppc_ori(6, 6, GOT & 0xFFFF), ppc_stw(5, 0, 6),
        PPC_BLR,
    };
    put_words(CB, code, 5);
    gm_w32(TV_CB, CB);
    gm_w32(TV_CB + 4, 0);
    gm_w32(GOT, 0);
}

static void setup(void) {
    harness_init(names, sizeof names / sizeof names[0]);
    mm_init();
    misc_init();
    sound_init();
    sound_register();
    emit_callback();
}

/* A standard 8-bit header (encode 0) of frames of silence at rate Hz, the
   samples following the header. */
static uint32_t std_header(uint32_t frames, uint32_t rate) {
    uint32_t h = scratch(22 + frames);
    gm_w32(h + 4, frames);
    gm_w32(h + 8, rate << 16);
    gm_w8(h + 20, 0);
    memset(gm_ptr(h + 22, frames), 0x80, frames);
    return h;
}

static uint32_t cmd(uint16_t c, uint16_t p1, uint32_t p2) {
    uint32_t a = scratch(8);
    gm_w16(a, c);
    gm_w16(a + 2, p1);
    gm_w32(a + 4, p2);
    return a;
}

static uint32_t new_channel(void) {
    uint32_t pp = scratch(4);
    uint32_t upp = call_import("NewSndCallBackUPP", 1, TV_CB);
    if (call_import("SndNewChannel", 4, pp, 5u, 0x84u, upp) != 0)
        fatal("SndNewChannel failed");
    return gm_r32(pp);
}

static bool busy(uint32_t chan) {
    uint32_t st = scratch(24);
    call_import("SndChannelStatus", 3, chan, 24u, st);
    return gm_r8(st + 12) != 0;
}

static void sleep_s(double s) {
    struct timespec ts = {0, (long)(s * 1e9)};
    nanosleep(&ts, NULL);
}

/* Renders through the mixer directly, bypassing sound_pump's clock. */
static void render(int16_t *out, uint32_t frames) { mix_render(out, frames); }

TEST(sound_buffers_are_copied_when_queued) {
    setup();
    uint32_t chan = new_channel();
    uint32_t h = std_header(4, MIX_RATE);
    gm_w8(h + 22, 0xC0); /* +64 of 128: 0x4000 */
    call_import("SndDoCommand", 3, chan, cmd(SND_BUFFER_CMD, 0, h), 0u);
    gm_w8(h + 22, 0x00); /* too late: the sound was copied */
    int16_t out[8];
    render(out, 4);
    CHECK_EQ((uint16_t)out[0], 0x4000);
    CHECK_EQ((uint16_t)out[1], 0x4000);
    CHECK_EQ(out[2], 0);
}

TEST(sound_extended_headers_play_16_bit_stereo) {
    setup();
    uint32_t chan = new_channel();
    uint32_t h = scratch(64 + 8);
    gm_w32(h + 4, 2); /* channels */
    gm_w32(h + 8, (uint32_t)MIX_RATE << 16);
    gm_w8(h + 20, 0xFF);
    gm_w32(h + 22, 2); /* frames */
    gm_w16(h + 48, 16);
    gm_w16(h + 64, 0x1234);
    gm_w16(h + 66, 0xFEDC);
    call_import("SndDoCommand", 3, chan, cmd(SND_BUFFER_CMD, 0, h), 0u);
    int16_t out[4];
    render(out, 2);
    CHECK_EQ((uint16_t)out[0], 0x1234);
    CHECK_EQ((uint16_t)out[1], 0xFEDC);
}

static void child_bad_header(void *unused) {
    (void)unused;
    setup();
    uint32_t chan = new_channel();
    uint32_t h = std_header(4, 600);
    gm_w32(h, 0x7FFFFFF0u); /* samples outside memory */
    call_import("SndDoCommand", 3, chan, cmd(SND_BUFFER_CMD, 0, h), 0u);
    uint32_t c = std_header(4, 600);
    gm_w8(c + 20, 0xFE); /* compressed */
    call_import("SndDoCommand", 3, chan, cmd(SND_BUFFER_CMD, 0, c), 0u);
    call_import("SndDoCommand", 3, chan, cmd(SND_BUFFER_CMD, 0, c), 0u);
    if (busy(chan)) /* skipped buffers take no time */
        _exit(3);
}

TEST(sound_bad_headers_are_logged_and_skipped) {
    char out[4096];
    CHECK_EQ(test_run_child(child_bad_header, NULL, out, sizeof out), 0);
    CHECK_CONTAINS(out, "are outside memory (ignored)");
    const char *msg = "loony: sound: sound header encoding 254 is not supported (ignored)";
    const char *first = strstr(out, msg);
    CHECK(first != NULL);
    CHECK(strstr(first + strlen(msg), msg) == NULL);
}

TEST(sound_new_channel_allocates_one) {
    setup();
    uint32_t chan = new_channel();
    CHECK(mm_is_ptr(chan));
    CHECK_EQ(gm_r32(chan + 8), TV_CB);
    CHECK(!busy(chan));
    CHECK_EQ(call_import("SndDisposeChannel", 2, chan, 1u), 0);
    CHECK_EQ((int16_t)call_import("SndDisposeChannel", 2, chan, 1u), SND_BAD_CHANNEL_ERR);
}

TEST(sound_buffer_keeps_the_channel_busy_then_calls_back) {
    setup();
    uint32_t chan = new_channel();
    /* 30 frames at 600 Hz: 50 ms. */
    call_import("SndDoCommand", 3, chan, cmd(SND_BUFFER_CMD, 0, std_header(30, 600)), 0u);
    call_import("SndDoCommand", 3, chan, cmd(SND_CALLBACK_CMD, 0, 0xCAFEu), 0u);
    sound_pump(); /* starts the buffer */
    CHECK(busy(chan));
    sound_pump();
    CHECK_EQ(gm_r32(GOT), 0); /* the buffer is still playing */
    sleep_s(0.08);
    sound_pump();
    CHECK_EQ(gm_r32(GOT), 0xCAFE);
    CHECK(!busy(chan));
}

TEST(sound_immediate_commands) {
    setup();
    uint32_t chan = new_channel();
    call_import("SndDoCommand", 3, chan, cmd(SND_BUFFER_CMD, 0, std_header(600, 600)), 0u);
    call_import("SndDoCommand", 3, chan, cmd(SND_CALLBACK_CMD, 0, 1u), 0u);
    sound_pump();
    CHECK(busy(chan));
    call_import("SndDoImmediate", 2, chan, cmd(SND_FLUSH_CMD, 0, 0));
    call_import("SndDoImmediate", 2, chan, cmd(SND_QUIET_CMD, 0, 0));
    CHECK(!busy(chan));
    sound_pump();
    CHECK_EQ(gm_r32(GOT), 0); /* the flushed callback never runs */
    call_import("SndDoImmediate", 2, chan, cmd(SND_CALLBACK_CMD, 0, 7u));
    CHECK_EQ(gm_r32(GOT), 7);
}

static void child_unknown_command(void *unused) {
    (void)unused;
    setup();
    uint32_t chan = new_channel();
    call_import("SndDoImmediate", 2, chan, cmd(99, 0, 0));
    call_import("SndDoImmediate", 2, chan, cmd(99, 0, 0));
}

TEST(sound_unknown_commands_are_logged_once) {
    char out[4096];
    CHECK_EQ(test_run_child(child_unknown_command, NULL, out, sizeof out), 0);
    const char *msg = "loony: sound: command 99 is not supported (ignored)";
    const char *first = strstr(out, msg);
    CHECK(first != NULL);
    CHECK(strstr(first + strlen(msg), msg) == NULL);
}

TEST(sound_volume_is_clamped_and_cubed) {
    CHECK_EQ(sound_volume(), SOUND_DEFAULT_VOLUME);
    sound_set_volume(150);
    CHECK_EQ(sound_volume(), 100);
    sound_set_volume(-5);
    CHECK_EQ(sound_volume(), 0);
    sound_set_volume(SOUND_DEFAULT_VOLUME);
    CHECK(sound_gain(100) == 1.0f);
    CHECK(sound_gain(0) == 0.0f);
    CHECK(sound_gain(50) == 0.125f);
    CHECK(sound_gain(80) > 0.511f && sound_gain(80) < 0.513f);
}

static uint32_t source_frames, source_calls;

static void source(int16_t *out, uint32_t frames) {
    source_calls++;
    source_frames += frames;
    for (uint32_t k = 0; k < 2 * frames; k++)
        out[k] = (int16_t)(out[k] + 100);
}

/* A source renders on the clock, from sound_pump alone: a Sound Manager
   call catches the mixer up without it. */
TEST(sound_source_renders_from_the_pump_on_the_clock) {
    setenv("LOONY_FIXED_CLOCK", "1", 1);
    setup();
    sound_start_output(); /* the fixed clock: no device */
    source_frames = source_calls = 0;
    sound_set_source(source);
    misc_sleep_us(100000);
    uint32_t chan = new_channel();
    CHECK(!busy(chan)); /* SndChannelStatus catches up */
    CHECK_EQ(source_calls, 0);
    misc_sleep_us(100000);
    sound_pump();
    CHECK(source_calls > 0);
    CHECK(source_frames >= MIX_RATE / 10 && source_frames <= MIX_RATE / 5 + 1);
    uint32_t before = source_frames;
    sound_pump(); /* no time passed */
    CHECK_EQ(source_frames, before);
    sound_set_source(NULL);
    misc_sleep_us(100000);
    sound_pump();
    CHECK_EQ(source_frames, before);
    unsetenv("LOONY_FIXED_CLOCK");
}

/* With a device, a source plays SOUND_SOURCE_LEAD_MS behind. A stall's
   sound is late, so the monitor drops it and is back to its lead at the
   next pump, and on the real clock a gap of more than a second (the Mac
   slept) isn't rendered at all. Each gap is measured, since a loaded
   machine sleeps longer than asked. */
TEST(sound_source_stays_its_lead_behind_after_a_stall) {
    setup();
    sound_set_source(source);
    sound_start_output(); /* the dummy device, on the real clock */
    source_frames = 0;
    double lead = SOUND_SOURCE_LEAD_MS / 1000.0, t = wall_seconds();
    usleep(300000);
    sound_pump();
    double gap = wall_seconds() - t;
    if (gap < 0.9) { /* a stall, not a sleep */
        CHECK(source_frames >= MIX_RATE / 4);
        CHECK(sound_monitor_queued_seconds() < 0.01);
    }
    t = wall_seconds();
    usleep(20000);
    sound_pump();
    gap = wall_seconds() - t;
    double q = sound_monitor_queued_seconds();
    if (gap < 1.5 * lead) /* on time: the lead and this pump's frames */
        CHECK(q > lead && q <= lead + gap + 0.005);
    uint32_t before = source_frames;
    usleep(1100000);
    sound_pump();
    CHECK_EQ(source_frames, before);
    sound_set_source(NULL);
}

/* A format 1 'snd ' resource in a handle: one modifier, then a bufferCmd
   whose data pointer is the offset of a standard header of frames samples
   of value at rate Hz. */
static uint32_t snd_resource(uint16_t format, uint32_t frames, uint32_t rate, uint8_t value) {
    uint32_t pre = format == 1 ? 4 + 6 + 2 + 8 : 4 + 2 + 8;
    uint32_t h = mm_new_handle(pre + 22 + frames, true), p = gm_r32(h);
    gm_w16(p, format);
    uint32_t at = 4;
    if (format == 1) {
        gm_w16(p + 2, 1);
        gm_w16(p + 4, 5); /* sampledSynth */
        at = 10;
    }
    gm_w16(p + at, 1);
    gm_w16(p + at + 2, 0x8000 | SND_BUFFER_CMD);
    gm_w32(p + at + 6, pre);
    uint32_t hdr = p + pre;
    gm_w32(hdr + 4, frames);
    gm_w32(hdr + 8, rate << 16);
    memset(gm_ptr(hdr + 22, frames), value, frames);
    return h;
}

TEST(sound_snd_resources_play_with_sndplay) {
    setup();
    uint32_t off = scratch(4);
    uint32_t one = snd_resource(1, 4, MIX_RATE, 0xC0), two = snd_resource(2, 4, MIX_RATE, 0xC0);
    CHECK_EQ(call_import("GetSoundHeaderOffset", 2, one, off), 0);
    CHECK_EQ(gm_r32(off), 20);
    CHECK_EQ(call_import("GetSoundHeaderOffset", 2, two, off), 0);
    CHECK_EQ(gm_r32(off), 14);
    uint32_t junk = mm_new_handle(4, true);
    gm_w16(gm_r32(junk), 3);
    CHECK_EQ((int16_t)call_import("GetSoundHeaderOffset", 2, junk, off), SND_BAD_FORMAT_ERR);
    CHECK_EQ((int16_t)call_import("SndPlay", 3, 0u, junk, 0u), SND_BAD_FORMAT_ERR);

    uint32_t chan = new_channel();
    CHECK_EQ(call_import("SndPlay", 3, chan, one, 1u), 0);
    int16_t out[8];
    render(out, 4);
    CHECK_EQ((uint16_t)out[0], 0x4000);
}

TEST(sound_sndplay_without_a_channel_plays_on_its_own) {
    setup();
    CHECK_EQ(call_import("SndPlay", 3, 0u, snd_resource(2, 4, MIX_RATE, 0xC0), 0u), 0);
    CHECK_EQ(call_import("SndPlay", 3, 0u, snd_resource(2, 4, MIX_RATE, 0x40), 0u), 0);
    int16_t out[16];
    render(out, 8); /* one after the other */
    CHECK_EQ((uint16_t)out[0], 0x4000);
    CHECK_EQ(out[8], -0x4000);
    CHECK_EQ(call_import("SndSoundManagerVersion", 0), 0x03608000u);
}
