#include "test.h"

#include "mixer.h"

#define RATE ((uint32_t)MIX_RATE << 16) /* plays one source frame per output frame */

static mix_sound *mono(uint32_t rate, int n, const int16_t *v) {
    mix_sound *s = mix_sound_new((uint32_t)n, 1, rate);
    for (int i = 0; i < n; i++)
        s->samples[i] = v[i];
    return s;
}

static mix_cmd buffer(mix_sound *s) { return (mix_cmd){.kind = MIX_BUFFER, .sound = s}; }
static mix_cmd callback(uint16_t p1, uint32_t p2) {
    return (mix_cmd){.kind = MIX_CALLBACK, .param1 = p1, .param2 = p2};
}

TEST(mixer_plays_a_sound_on_both_sides) {
    mix_init();
    mix_open(0);
    CHECK(!mix_busy(0));
    mix_queue(0, buffer(mono(RATE, 3, (int16_t[]){100, -200, 300})));
    CHECK(mix_busy(0));
    int16_t out[8];
    mix_render(out, 4);
    CHECK_EQ(out[0], 100);
    CHECK_EQ(out[1], 100);
    CHECK_EQ(out[2], -200);
    CHECK_EQ(out[5], 300);
    CHECK_EQ(out[6], 0); /* finished */
    CHECK(!mix_busy(0));
    mix_init();
}

TEST(mixer_resamples_linearly) {
    mix_init();
    mix_open(0);
    mix_queue(0, buffer(mono(RATE / 2, 2, (int16_t[]){0, 1000})));
    int16_t out[10];
    mix_render(out, 5);
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[2], 500);
    CHECK_EQ(out[4], 1000);
    CHECK_EQ(out[6], 1000); /* the last frame holds */
    CHECK_EQ(out[8], 0);    /* 2 frames at half rate last 4 output frames */
    mix_init();
}

TEST(mixer_plays_stereo_sounds_on_their_sides) {
    mix_init();
    mix_open(0);
    mix_sound *s = mix_sound_new(1, 2, RATE);
    s->samples[0] = 11;
    s->samples[1] = -22;
    mix_queue(0, buffer(s));
    int16_t out[2];
    mix_render(out, 1);
    CHECK_EQ(out[0], 11);
    CHECK_EQ(out[1], -22);
    mix_init();
}

TEST(mixer_chains_buffers_without_a_gap_then_calls_back) {
    mix_init();
    mix_open(3);
    mix_queue(3, buffer(mono(RATE, 2, (int16_t[]){1, 2})));
    mix_queue(3, buffer(mono(RATE, 2, (int16_t[]){3, 4})));
    mix_queue(3, callback(7, 0xCAFE));
    int16_t out[8];
    mix_callback cb;
    mix_render(out, 3);
    CHECK(!mix_take_callback(&cb));
    mix_render(out + 6, 1); /* the last frame: the callback is reached as it ends */
    CHECK_EQ(out[0], 1);
    CHECK_EQ(out[2], 2);
    CHECK_EQ(out[4], 3);
    CHECK_EQ(out[6], 4);
    CHECK(mix_take_callback(&cb));
    CHECK_EQ(cb.channel, 3);
    CHECK_EQ(cb.param1, 7);
    CHECK_EQ(cb.param2, 0xCAFE);
    CHECK(!mix_take_callback(&cb));
    CHECK(!mix_busy(3));
    mix_init();
}

TEST(mixer_waits_before_the_next_command) {
    mix_init();
    mix_open(0);
    mix_queue(0, (mix_cmd){.kind = MIX_WAIT, .frames = 100});
    mix_queue(0, callback(1, 2));
    int16_t out[200];
    mix_callback cb;
    mix_render(out, 99);
    CHECK(!mix_take_callback(&cb));
    CHECK(mix_busy(0));
    mix_render(out, 1);
    CHECK(mix_take_callback(&cb));
    CHECK(!mix_busy(0));
    mix_init();
}

TEST(mixer_volume_and_clamping) {
    mix_init();
    mix_open(0);
    mix_open(1);
    mix_queue(0, (mix_cmd){.kind = MIX_VOLUME, .param2 = 0x0200u << 16 | 0x0080u});
    mix_queue(0, buffer(mono(RATE, 1, (int16_t[]){1000})));
    mix_queue(1, buffer(mono(RATE, 1, (int16_t[]){32000})));
    int16_t out[2];
    mix_render(out, 1);
    CHECK_EQ(out[0], 32500); /* 1000 at half volume + 32000 */
    CHECK_EQ(out[1], 32767); /* 1000 at double volume + 32000, clamped */
    mix_queue(0, buffer(mono(RATE, 1, (int16_t[]){-1000})));
    mix_render(out, 1);
    CHECK_EQ(out[0], -500);
    CHECK_EQ(out[1], -2000);
    mix_init();
}

TEST(mixer_quiet_and_flush_stop_a_channel) {
    mix_init();
    mix_open(0);
    int16_t v[1000] = {0};
    mix_queue(0, buffer(mono(RATE, 1000, v)));
    mix_queue(0, buffer(mono(RATE, 1000, v)));
    mix_queue(0, callback(0, 0));
    int16_t out[20];
    mix_render(out, 10);
    mix_immediate(0, (mix_cmd){.kind = MIX_FLUSH});
    CHECK(mix_busy(0)); /* the sound playing goes on */
    mix_immediate(0, (mix_cmd){.kind = MIX_QUIET});
    CHECK(!mix_busy(0));
    mix_render(out, 10);
    mix_callback cb;
    CHECK(!mix_take_callback(&cb)); /* flushed */
    mix_init();
}

TEST(mixer_a_queued_flush_keeps_the_queue_whole) {
    mix_init();
    mix_open(0);
    mix_queue(0, (mix_cmd){.kind = MIX_FLUSH});
    int16_t out[4];
    mix_render(out, 1); /* runs the flush */
    mix_queue(0, buffer(mono(RATE, 1, (int16_t[]){77})));
    CHECK(mix_busy(0));
    mix_render(out, 1);
    CHECK_EQ(out[0], 77);
    CHECK(!mix_busy(0));
    mix_init();
}

TEST(mixer_an_immediate_buffer_replaces_the_sound_playing) {
    mix_init();
    mix_open(0);
    mix_queue(0, buffer(mono(RATE, 3, (int16_t[]){5, 5, 5})));
    int16_t out[6];
    mix_render(out, 1);
    mix_immediate(0, buffer(mono(RATE, 1, (int16_t[]){9})));
    mix_render(out, 2);
    CHECK_EQ(out[0], 9);
    CHECK_EQ(out[2], 0);
    mix_init();
}

TEST(mixer_a_full_callback_queue_stalls_channels_in_order) {
    mix_init();
    for (int ch = 0; ch < 3; ch++) {
        mix_open(ch);
        for (int i = 0; i < MIX_QUEUE_LEN; i++)
            CHECK(mix_queue(ch, callback(0, (uint32_t)i)));
    }
    CHECK(!mix_queue(0, callback(0, 999))); /* a command queue is full */
    int16_t out[2];
    mix_render(out, 1); /* 3 x 128 callbacks: the 256-entry queue fills */
    mix_callback cb;
    int got = 0;
    uint32_t next[3] = {0};
    bool in_order = true;
    while (mix_take_callback(&cb)) {
        in_order &= cb.param2 == next[cb.channel]++;
        got++;
        mix_render(out, 1); /* the stalled channel resumes as the queue drains */
    }
    CHECK(in_order);
    CHECK_EQ(got, 3 * MIX_QUEUE_LEN);
    CHECK_EQ(next[2], MIX_QUEUE_LEN);
    mix_init();
}

TEST(mixer_closing_a_channel_drops_its_callbacks) {
    mix_init();
    mix_open(0);
    mix_open(1);
    mix_queue(0, callback(0, 1));
    mix_queue(1, callback(1, 2));
    mix_queue(0, buffer(mono(RATE, 1, (int16_t[]){1})));
    int16_t out[2];
    mix_render(out, 1);
    mix_close(0);
    mix_callback cb;
    CHECK(mix_take_callback(&cb));
    CHECK_EQ(cb.channel, 1);
    CHECK(!mix_take_callback(&cb));
    CHECK(!mix_busy(0));
    CHECK(mix_queue(0, buffer(mono(RATE, 1, (int16_t[]){1})))); /* a closed channel ignores it */
    CHECK(!mix_busy(0));
    mix_init();
}
