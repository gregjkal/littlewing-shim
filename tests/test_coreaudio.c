#include "test.h"

#include <stdio.h>
#include <stdlib.h>

#include "coreaudio.h"
#include "harness.h"
#include "memmgr.h"
#include "misc.h"
#include "sound.h"

/* The game's callbacks are test imports here: with direct calls, a
   callback's address is its trap's, and its handler is C. */
static const char *const names[] = {
    "NewAUGraph", "AUGraphNewNode", "AUGraphOpen", "AUGraphConnectNodeInput", "AUGraphGetNodeInfo",
    "AUGraphInitialize", "AUGraphStart", "AudioUnitGetProperty", "AudioUnitSetProperty",
    "AudioFileOpenWithCallbacks", "AudioFileGetPropertyInfo", "AudioFileGetProperty",
    "AudioFileReadPackets", "AudioFileClose", "AudioConverterNew", "AudioConverterSetProperty",
    "AudioConverterFillComplexBuffer", "ReadProc", "SizeProc", "RenderA", "RenderB", "RenderOwn", "InputProc",
};
#define NNAMES (uint32_t)(sizeof names / sizeof names[0])

static uint32_t cb(const char *name) { return GUEST_TRAP_ADDR(trap_find(name)); }

/* ---- a file, read through ReadProc and SizeProc ---- */

static uint8_t file[4096];
static uint32_t file_len;
/* Or a file of 4 GB: file, then far_len bytes of far at far_at. */
static uint32_t far_at, far_len;
static uint8_t far[16];

static void h_read_proc(void) {
    uint64_t pos = (uint64_t)trap_arg(1) << 32 | trap_arg(2);
    uint32_t n = trap_arg(3), buf = trap_arg(4), actual = trap_arg(5);
    if (far_at && pos >= far_at) {
        uint32_t k = (uint32_t)(pos - far_at);
        n = k < far_len && n > far_len - k ? far_len - k : k < far_len ? n : 0;
        if (n)
            memcpy(gm_ptr(buf, n), far + k, n);
        gm_w32(actual, n);
        trap_return(0);
        return;
    }
    if (pos > file_len)
        pos = file_len;
    if (n > file_len - pos)
        n = file_len - (uint32_t)pos;
    if (n)
        memcpy(gm_ptr(buf, n), file + pos, n);
    gm_w32(actual, n);
    trap_return(0);
}

static void h_size_proc(void) {
    trap_return(0);
    cpu_set_gpr(4, far_at ? 0xFFFFFFFFu : file_len);
}

static void put(uint32_t *at, const void *p, uint32_t n) {
    memcpy(file + *at, p, n);
    *at += n;
}

static void put32(uint32_t *at, uint32_t v) {
    uint8_t b[4];
    wr_be32(b, v);
    put(at, b, 4);
}

/* An AIFF-C file of packets packets of nch-channel sound compressed as
   comp at 44100 Hz, the packet bytes counting up from 0. */
static void make_aifc(uint32_t comp, uint32_t nch, uint32_t packets) {
    uint32_t data = packets * 34 * nch, at = 0;
    put32(&at, FOURCC('F', 'O', 'R', 'M'));
    put32(&at, 4 + 12 + 32 + 16 + data);
    put32(&at, FOURCC('A', 'I', 'F', 'C'));
    put32(&at, FOURCC('F', 'V', 'E', 'R'));
    put32(&at, 4);
    put32(&at, 0xA2805140);
    put32(&at, FOURCC('C', 'O', 'M', 'M'));
    put32(&at, 24);
    const uint8_t comm[24] = {0, (uint8_t)nch, (uint8_t)(packets >> 24), (uint8_t)(packets >> 16),
                              (uint8_t)(packets >> 8), (uint8_t)packets, 0, 16,
                              0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0,
                              (uint8_t)(comp >> 24), (uint8_t)(comp >> 16), (uint8_t)(comp >> 8),
                              (uint8_t)comp, 0, 0};
    put(&at, comm, 24);
    put32(&at, FOURCC('S', 'S', 'N', 'D'));
    put32(&at, 8 + data);
    put32(&at, 0);
    put32(&at, 0);
    for (uint32_t i = 0; i < data; i++)
        file[at++] = (uint8_t)i;
    file_len = at;
}

/* ---- the game's render callbacks: a constant on each side, and what
   they were called with ---- */

static uint32_t rendered_bus[8], rendered_frames, nrendered;
static double rendered_time;

static void fill(int16_t left, int16_t right) {
    uint32_t bus = trap_arg(3), frames = trap_arg(4), abl = trap_arg(5);
    if (nrendered < 8)
        rendered_bus[nrendered] = bus;
    nrendered++;
    rendered_frames = frames;
    uint64_t bits = (uint64_t)gm_r32(trap_arg(2)) << 32 | gm_r32(trap_arg(2) + 4);
    memcpy(&rendered_time, &bits, 8);
    uint32_t data = gm_r32(abl + 12);
    for (uint32_t f = 0; f < frames; f++) {
        gm_w16(data + 4 * f, (uint16_t)left);
        gm_w16(data + 4 * f + 2, (uint16_t)right);
    }
    trap_return(0);
}

static void h_render_a(void) { fill(1000, -1000); }
static void h_render_b(void) { fill(32000, -32000); }

/* A callback that points the buffer list at samples of its own. */
static uint32_t own_samples;

static void h_render_own(void) {
    uint32_t frames = trap_arg(4), abl = trap_arg(5);
    for (uint32_t f = 0; f < frames; f++) {
        gm_w16(own_samples + 4 * f, 300);
        gm_w16(own_samples + 4 * f + 2, (uint16_t)-300);
    }
    gm_w32(abl + 12, own_samples);
    trap_return(0);
}

/* ---- the converter's input proc: the packets at in_data, a few at a time ---- */

static uint32_t in_data, in_packets, in_next, in_bpp;

static void h_input_proc(void) {
    uint32_t io_packets = trap_arg(1), abl = trap_arg(2);
    uint32_t n = gm_r32(io_packets), left = in_packets - in_next;
    if (n > 3)
        n = 3;
    if (n > left)
        n = left;
    gm_w32(io_packets, n);
    gm_w32(abl + 8, n * in_bpp);
    gm_w32(abl + 12, in_data + in_next * in_bpp);
    in_next += n;
    trap_return(n ? 0 : 0xFFFFFFFFu);
}

static void setup(void) {
    harness_init_direct(names, NNAMES);
    mm_init();
    misc_init();
    sound_init();
    coreaudio_init();
    coreaudio_register();
    trap_register("ReadProc", h_read_proc);
    trap_register("SizeProc", h_size_proc);
    trap_register("RenderA", h_render_a);
    trap_register("RenderB", h_render_b);
    trap_register("RenderOwn", h_render_own);
    trap_register("InputProc", h_input_proc);
    nrendered = 0;
    far_at = far_len = 0;
}

/* For helpers, which can't CHECK: a call that must succeed. */
static void must(uint32_t err, const char *what) {
    if (err)
        fatal("%s failed: %d", what, (int32_t)err);
}

static uint32_t word(void) {
    uint32_t p = scratch(4);
    gm_w32(p, 0);
    return p;
}

static uint32_t desc(uint32_t type, uint32_t sub) {
    uint32_t d = scratch(20);
    memset(gm_ptr(d, 20), 0, 20);
    gm_w32(d, type);
    gm_w32(d + 4, sub);
    gm_w32(d + 8, FOURCC('a', 'p', 'p', 'l'));
    return d;
}

static uint32_t output_unit; /* the default output of the last graph built */

/* The game's graph: a mixer of ninputs inputs into the default output.
   Returns the mixer unit. */
static uint32_t build_graph(uint32_t ninputs) {
    uint32_t gp = word(), out_node = word(), mix_node = word(), unit = word(), count = word();
    must(call_import("NewAUGraph", 1, gp), "NewAUGraph");
    uint32_t g = gm_r32(gp);
    must(call_import("AUGraphNewNode", 5, g, desc(FOURCC('a', 'u', 'o', 'u'), FOURCC('d', 'e', 'f', ' ')),
                         0, 0, out_node), "AUGraphNewNode");
    must(call_import("AUGraphNewNode", 5, g, desc(FOURCC('a', 'u', 'm', 'x'), FOURCC('s', 'm', 'x', 'r')),
                         0, 0, mix_node), "AUGraphNewNode");
    must(call_import("AUGraphOpen", 1, g), "AUGraphOpen");
    must(call_import("AUGraphConnectNodeInput", 5, g, gm_r32(mix_node), 0, gm_r32(out_node), 0), "AUGraphConnectNodeInput");
    must(call_import("AUGraphGetNodeInfo", 6, g, gm_r32(mix_node), 0, 0, 0, unit), "AUGraphGetNodeInfo");
    uint32_t mixer = gm_r32(unit);
    must(call_import("AUGraphGetNodeInfo", 6, g, gm_r32(out_node), 0, 0, 0, unit), "AUGraphGetNodeInfo");
    output_unit = gm_r32(unit);
    gm_w32(count, ninputs);
    must(call_import("AudioUnitSetProperty", 6, mixer, 11, 1, 0, count, 4), "AudioUnitSetProperty");
    must(call_import("AUGraphInitialize", 1, g), "AUGraphInitialize");
    must(call_import("AUGraphStart", 1, g), "AUGraphStart");
    return mixer;
}

static uint32_t render_callback(uint32_t mixer, uint32_t elem, const char *proc) {
    uint32_t s = scratch(8);
    gm_w32(s, proc ? cb(proc) : 0);
    gm_w32(s + 4, 0x1234);
    return call_import("AudioUnitSetProperty", 6, mixer, 23, 1, elem, s, 8);
}

TEST(coreaudio_mixer_takes_16_bit_big_endian_stereo) {
    setup();
    uint32_t mixer = build_graph(4);
    uint32_t asbd = scratch(40), size = word();
    gm_w32(size, 40);
    CHECK_EQ(call_import("AudioUnitGetProperty", 6, mixer, 8, 1, 0, asbd, size), 0);
    CHECK_EQ(gm_r32(size), 40);
    CHECK_EQ(gm_r32(asbd), 0x40E58880); /* 44100.0 */
    CHECK_EQ(gm_r32(asbd + 4), 0);
    CHECK_EQ(gm_r32(asbd + 8), FOURCC('l', 'p', 'c', 'm'));
    CHECK_EQ(gm_r32(asbd + 12), 0xE); /* signed integer, big-endian, packed */
    CHECK_EQ(gm_r32(asbd + 16), 4);
    CHECK_EQ(gm_r32(asbd + 28), 2);
    CHECK_EQ(gm_r32(asbd + 32), 16);
    gm_w32(size, 39);
    CHECK_EQ(call_import("AudioUnitGetProperty", 6, mixer, 8, 1, 0, asbd, size), (uint32_t)-50);
    CHECK_EQ(call_import("AudioUnitGetProperty", 6, mixer, 1234, 1, 0, asbd, size), (uint32_t)-10879);
    CHECK_EQ(render_callback(mixer, 4, "RenderA"), (uint32_t)-10877); /* past the inputs */
}

/* FAIRY TOWER reads the output's input format, makes it CoreAudio's
   canonical 32-bit float (big-endian, packed, not interleaved), and sets
   it there and on the mixer's output: the one stream between them. The
   mixer's inputs keep its own format, and it still mixes them. */
TEST(coreaudio_stream_to_the_output_may_be_float) {
    setup();
    uint32_t mixer = build_graph(1);
    uint32_t asbd = scratch(40), size = word();
    gm_w32(size, 40);
    CHECK_EQ(call_import("AudioUnitGetProperty", 6, output_unit, 8, 1, 0, asbd, size), 0);
    CHECK_EQ(gm_r32(asbd + 12), 0xE); /* the mixer's until it is set */
    CHECK_EQ(gm_r32(asbd + 32), 16);
    gm_w32(asbd + 12, 0x2B); /* float, big-endian, packed, non-interleaved */
    gm_w32(asbd + 32, 32);
    CHECK_EQ(call_import("AudioUnitSetProperty", 6, output_unit, 8, 1, 0, asbd, 40), 0);
    CHECK_EQ(call_import("AudioUnitSetProperty", 6, mixer, 8, 2, 0, asbd, 40), 0);
    CHECK_EQ(call_import("AudioUnitSetProperty", 6, mixer, 8, 1, 0, asbd, 40), (uint32_t)-10868);
    uint32_t back = scratch(40);
    CHECK_EQ(call_import("AudioUnitGetProperty", 6, output_unit, 8, 1, 0, back, size), 0);
    CHECK_EQ(gm_r32(back + 12), 0x2B);
    CHECK_EQ(gm_r32(back + 32), 32);
    CHECK_EQ(call_import("AudioUnitGetProperty", 6, mixer, 8, 2, 0, back, size), 0);
    CHECK_EQ(gm_r32(back + 12), 0x2B);
    CHECK_EQ(call_import("AudioUnitGetProperty", 6, mixer, 8, 1, 0, back, size), 0);
    CHECK_EQ(gm_r32(back + 12), 0xE); /* the inputs are still the mixer's own */
    CHECK_EQ(call_import("AudioUnitGetProperty", 6, mixer, 8, 0, 0, back, size), 0);
    CHECK_EQ(gm_r32(back + 12), 0xE); /* so is its global scope */
    /* Any other format is refused, and the output's other scope has none. */
    gm_w32(asbd + 28, 1); /* mono */
    CHECK_EQ(call_import("AudioUnitSetProperty", 6, output_unit, 8, 1, 0, asbd, 40), (uint32_t)-10868);
    CHECK_EQ(call_import("AudioUnitGetProperty", 6, output_unit, 8, 2, 0, back, size), (uint32_t)-10879);
    CHECK_EQ(render_callback(mixer, 0, "RenderA"), 0);
    int16_t out[2 * 4] = {0};
    coreaudio_render(out, 4);
    CHECK_EQ(out[0], 1000);
    CHECK_EQ(out[1], -1000);
}

TEST(coreaudio_mixer_sums_the_inputs_callbacks_a_slice_at_a_time) {
    setup();
    uint32_t mixer = build_graph(4);
    CHECK_EQ(render_callback(mixer, 1, "RenderA"), 0);
    CHECK_EQ(render_callback(mixer, 3, "RenderA"), 0);
    static int16_t out[2 * 700];
    memset(out, 0, sizeof out);
    out[0] = 5;
    coreaudio_render(out, 700); /* two slices */
    CHECK_EQ(nrendered, 4);
    CHECK_EQ(rendered_bus[0], 1);
    CHECK_EQ(rendered_bus[1], 3);
    CHECK_EQ(rendered_frames, CA_SLICE);
    CHECK(rendered_time == CA_SLICE);
    CHECK_EQ(out[0], 2005); /* added to what was there */
    CHECK_EQ(out[1], -2000);
    CHECK_EQ(out[2 * 699 + 1], -2000);
    /* What is left of the second slice comes first, with no new render. */
    memset(out, 0, sizeof out);
    coreaudio_render(out, 2 * CA_SLICE - 700);
    CHECK_EQ(nrendered, 4);
    CHECK_EQ(out[0], 2000);
    /* Loud inputs clamp; an input without its callback is silent. */
    CHECK_EQ(render_callback(mixer, 1, "RenderB"), 0);
    CHECK_EQ(render_callback(mixer, 3, NULL), 0);
    CHECK_EQ(render_callback(mixer, 0, "RenderB"), 0);
    coreaudio_render(out, 1);
    CHECK_EQ(out[0], 32767);
    CHECK_EQ(out[1], -32768);
}

TEST(coreaudio_mixer_plays_a_callbacks_own_buffer) {
    setup();
    uint32_t mixer = build_graph(1);
    own_samples = scratch(4 * CA_SLICE);
    CHECK_EQ(render_callback(mixer, 0, "RenderOwn"), 0);
    int16_t out[2 * 4] = {0};
    coreaudio_render(out, 4);
    CHECK_EQ(out[0], 300);
    CHECK_EQ(out[7], -300);
}

TEST(coreaudio_mixer_is_silent_until_the_graph_starts) {
    setup();
    int16_t out[2 * 16] = {0};
    coreaudio_render(out, 16);
    for (int i = 0; i < 32; i++)
        CHECK_EQ(out[i], 0);
}

static uint32_t open_file(uint32_t *file_id) {
    uint32_t out = word();
    uint32_t err = call_import("AudioFileOpenWithCallbacks", 7, 0x5555, cb("ReadProc"), 0, cb("SizeProc"), 0, 0,
                               out);
    *file_id = gm_r32(out);
    return err;
}

TEST(coreaudio_reads_an_ima4_aifc_file_through_the_games_callbacks) {
    setup();
    make_aifc(FOURCC('i', 'm', 'a', '4'), 2, 10);
    uint32_t f, size = word();
    CHECK_EQ(open_file(&f), 0);
    uint32_t asbd = scratch(40);
    gm_w32(size, 40);
    CHECK_EQ(call_import("AudioFileGetProperty", 4, f, FOURCC('d', 'f', 'm', 't'), size, asbd), 0);
    CHECK_EQ(gm_r32(asbd + 8), FOURCC('i', 'm', 'a', '4'));
    CHECK_EQ(gm_r32(asbd + 16), 68); /* bytes per packet */
    CHECK_EQ(gm_r32(asbd + 20), 64); /* frames per packet */
    CHECK_EQ(gm_r32(asbd + 28), 2);
    uint32_t v = scratch(8);
    gm_w32(size, 8);
    CHECK_EQ(call_import("AudioFileGetProperty", 4, f, FOURCC('b', 'c', 'n', 't'), size, v), 0);
    CHECK_EQ(gm_r32(v + 4), 680);
    CHECK_EQ(call_import("AudioFileGetProperty", 4, f, FOURCC('p', 'c', 'n', 't'), size, v), 0);
    CHECK_EQ(gm_r32(v + 4), 10);
    gm_w32(size, 4);
    CHECK_EQ(call_import("AudioFileGetProperty", 4, f, FOURCC('p', 's', 'z', 'e'), size, v), 0);
    CHECK_EQ(gm_r32(v), 68);
    CHECK_EQ(call_import("AudioFileGetProperty", 4, f, FOURCC('b', 'c', 'n', 't'), size, v),
             FOURCC('!', 's', 'i', 'z'));
    CHECK_EQ(call_import("AudioFileGetPropertyInfo", 4, f, FOURCC('m', 'g', 'i', 'c'), size, 0),
             FOURCC('p', 't', 'y', '?')); /* no magic cookie */
    /* Packets 8 and 9, though the game asks for 5. */
    uint32_t buf = scratch(400), bytes = word(), packets = word();
    gm_w32(bytes, 400);
    gm_w32(packets, 5);
    CHECK_EQ(call_import("AudioFileReadPackets", 8, f, 0, bytes, 0, 0, 8, packets, buf), 0);
    CHECK_EQ(gm_r32(packets), 2);
    CHECK_EQ(gm_r32(bytes), 136);
    CHECK_EQ(gm_r8(buf), (uint8_t)(8 * 68));
    CHECK_EQ(gm_r8(buf + 135), (uint8_t)(10 * 68 - 1));
    CHECK_EQ(call_import("AudioFileClose", 1, f), 0);
}

TEST(coreaudio_refuses_files_that_arent_ima4_aifc) {
    setup();
    uint32_t f;
    make_aifc(FOURCC('N', 'O', 'N', 'E'), 1, 4);
    CHECK_EQ(open_file(&f), FOURCC('f', 'm', 't', '?'));
    make_aifc(FOURCC('i', 'm', 'a', '4'), 1, 4);
    memcpy(file + 8, "AIFF", 4);
    CHECK_EQ(open_file(&f), FOURCC('t', 'y', 'p', '?'));
    make_aifc(FOURCC('i', 'm', 'a', '4'), 1, 4);
    file_len -= 10; /* the packets run past the end */
    CHECK_EQ(open_file(&f), FOURCC('d', 't', 'a', '?'));
    /* In a 4 GB file, an SSND chunk whose sound would start past the end
       (and at 4 bytes in, counted in 32 bits). */
    make_aifc(FOURCC('i', 'm', 'a', '4'), 1, 0);
    uint32_t at = 56; /* over the SSND chunk */
    put32(&at, FOURCC('J', 'U', 'N', 'K'));
    put32(&at, 0x7FFFFFC4);
    file_len = at;
    far_at = 0x80000004;
    far_len = 16;
    wr_be32(far, FOURCC('S', 'S', 'N', 'D'));
    wr_be32(far + 4, 0x7FFFFFF8);
    wr_be32(far + 8, 0x7FFFFFF0);
    wr_be32(far + 12, 0);
    CHECK_EQ(open_file(&f), FOURCC('d', 't', 'a', '?'));
}

/* Six packets of MECHA_SLING, a mono sound of Mad Daedalus, and what
   afconvert decodes them to: packets 3 to 5 go on from the last sample of
   the one before. */
static const char *const sling_hex =
    "00000000000000000000000000000000000000000000f0ff050808bf878b8580c008"
    "ff2304880088f08885808000088800af86808000f880848080e0808480800d488b40"
    "ffa58080e048b880b508808008800808b780070c030808f8088480d04880808c0408"
    "0023d830d080840b03800880088008800880088080808008080809ff04080808f85f"
    "001e0808f80a0608088e84808080008f80040808be08588080858000880088800088"
    "ff950088008880808000080808000000000000000000000000000000f0ff05f88906";
#define SLING_PACKETS 6
#define SLING_HASH 0x5ae680b9u /* fnv1a32 of afconvert's samples, as both sides big-endian */

static uint32_t new_converter(uint32_t nch, bool mono_to_both) {
    uint32_t src = scratch(40), dst = scratch(40), size = word(), out = word();
    make_aifc(FOURCC('i', 'm', 'a', '4'), nch, 1);
    uint32_t f;
    must(open_file(&f), "AudioFileOpenWithCallbacks");
    gm_w32(size, 40);
    must(call_import("AudioFileGetProperty", 4, f, FOURCC('d', 'f', 'm', 't'), size, src), "AudioFileGetProperty");
    uint32_t mixer = build_graph(1);
    must(call_import("AudioUnitGetProperty", 6, mixer, 8, 1, 0, dst, size), "AudioUnitGetProperty");
    must(call_import("AudioConverterNew", 3, src, dst, out), "AudioConverterNew");
    uint32_t conv = gm_r32(out);
    if (mono_to_both) {
        uint32_t map = scratch(8);
        gm_w32(map, 0);
        gm_w32(map + 4, 0);
        must(call_import("AudioConverterSetProperty", 4, conv, FOURCC('c', 'h', 'm', 'p'), 8, map), "AudioConverterSetProperty");
    }
    return conv;
}

/* Fills up to frames frames into buf; returns the status, the count in *got. */
static uint32_t fill_buffer(uint32_t conv, uint32_t buf, uint32_t frames, uint32_t *got) {
    uint32_t abl = scratch(16), io = word();
    gm_w32(abl, 1);
    gm_w32(abl + 4, 2);
    gm_w32(abl + 8, 4 * frames);
    gm_w32(abl + 12, buf);
    gm_w32(io, frames);
    uint32_t err = call_import("AudioConverterFillComplexBuffer", 6, conv, cb("InputProc"), 0x77, io, abl, 0);
    *got = gm_r32(io);
    if (gm_r32(abl + 8) != 4 * *got)
        fatal("the buffer's size isn't what was filled");
    return err;
}

TEST(coreaudio_converter_decodes_ima4_as_afconvert_does) {
    setup();
    uint32_t conv = new_converter(1, true);
    in_bpp = 34, in_packets = SLING_PACKETS, in_next = 0;
    in_data = scratch(34 * SLING_PACKETS);
    for (uint32_t i = 0; i < 34 * SLING_PACKETS; i++) {
        unsigned b;
        sscanf(sling_hex + 2 * i, "%2x", &b);
        gm_w8(in_data + i, (uint8_t)b);
    }
    /* In pieces that end inside packets. */
    uint32_t out = scratch(4 * 64 * SLING_PACKETS), done = 0, got;
    while (done < 64 * SLING_PACKETS) {
        uint32_t n = 64 * SLING_PACKETS - done < 100 ? 64 * SLING_PACKETS - done : 100;
        CHECK_EQ(fill_buffer(conv, out + 4 * done, n, &got), 0);
        CHECK_EQ(got, n);
        done += got;
    }
    CHECK_EQ(fnv1a32(gm_ptr(out, 4 * done), 4 * done), SLING_HASH);
    CHECK_EQ((int16_t)gm_r16(out + 4 * 192), -1); /* packet 3, from the last sample of 2 */
    CHECK_EQ((int16_t)gm_r16(out + 4 * 320 + 2), 0);
    /* The proc has no more: no frames, and its error. */
    CHECK_EQ(fill_buffer(conv, out, 10, &got), 0xFFFFFFFFu);
    CHECK_EQ(got, 0);
}

TEST(coreaudio_converter_without_a_map_plays_mono_on_the_left) {
    setup();
    uint32_t conv = new_converter(1, false);
    in_bpp = 34, in_packets = 1, in_next = 0;
    in_data = scratch(34);
    memset(gm_ptr(in_data, 34), 0x77, 34); /* every step up */
    uint32_t out = scratch(4 * 64), got;
    CHECK_EQ(fill_buffer(conv, out, 64, &got), 0);
    CHECK_EQ(got, 64);
    CHECK((int16_t)gm_r16(out + 4 * 63) > 0);
    CHECK_EQ(gm_r16(out + 4 * 63 + 2), 0);
}

TEST(coreaudio_converter_takes_only_ima4_to_the_mixers_format) {
    setup();
    uint32_t conv = new_converter(2, false);
    uint32_t map = scratch(8);
    gm_w32(map, 2); /* no third channel */
    gm_w32(map + 4, 0);
    CHECK_EQ(call_import("AudioConverterSetProperty", 4, conv, FOURCC('c', 'h', 'm', 'p'), 8, map), (uint32_t)-50);
    CHECK_EQ(call_import("AudioConverterSetProperty", 4, conv, FOURCC('x', 'x', 'x', 'x'), 4, map),
             FOURCC('p', 'r', 'o', 'p'));
    uint32_t src = scratch(40), dst = scratch(40), out = word();
    memset(gm_ptr(src, 40), 0, 40);
    memset(gm_ptr(dst, 40), 0, 40);
    gm_w32(src + 8, FOURCC('a', 'a', 'c', ' '));
    CHECK_EQ(call_import("AudioConverterNew", 3, src, dst, out), FOURCC('f', 'm', 't', '?'));
}
