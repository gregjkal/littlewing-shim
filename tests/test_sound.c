#include "test.h"

#include <stdlib.h>
#include <time.h>

#include "asm.h"
#include "harness.h"
#include "memmgr.h"
#include "misc.h"
#include "sound.h"

static const char *const names[] = {
    "NewSndCallBackUPP", "SndNewChannel", "SndDisposeChannel", "SndDoCommand", "SndDoImmediate",
    "SndChannelStatus",
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

/* A standard 8-bit header (encode 0) of frames at rate Hz. */
static uint32_t std_header(uint32_t frames, uint32_t rate) {
    uint32_t h = scratch(22);
    gm_w32(h + 4, frames);
    gm_w32(h + 8, rate << 16);
    gm_w8(h + 20, 0);
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

TEST(sound_header_durations) {
    setup();
    CHECK(sound_header_seconds(std_header(600, 600)) == 1.0);
    uint32_t ext = scratch(64);
    gm_w32(ext + 8, 22050u << 16);
    gm_w8(ext + 20, 0xFF);
    gm_w32(ext + 22, 44100);
    CHECK(sound_header_seconds(ext) == 2.0);
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
