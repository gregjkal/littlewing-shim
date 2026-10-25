#include "coreaudio.h"

#include <stdbool.h>
#include <string.h>

#include "guest_mem.h"
#include "memmgr.h"
#include "mixer.h"
#include "snapshot.h"
#include "sound.h"
#include "trap.h"
#include "util.h"

#define CA_GRAPH        (CA_TAG_BASE + 0x10u)
#define CA_UNIT_OUTPUT  (CA_TAG_BASE + 0x100u)
#define CA_UNIT_MIXER   (CA_TAG_BASE + 0x200u)
#define CA_FILE_BASE    (CA_TAG_BASE + 0x100000u)
#define CA_CONV_BASE    (CA_TAG_BASE + 0x200000u)
#define MAX_INPUTS      128
#define MAX_FILES       128
#define MAX_CONVS       128

/* The error codes, as CoreAudio's headers have them. */
#define NO_ERR                  0
#define PARAM_ERR               (-50)
#define BAD_COMPONENT_TYPE      (-2005)
#define AU_INVALID_PROPERTY     (-10879)
#define AU_INVALID_ELEMENT      (-10877)
#define AU_INVALID_VALUE        (-10851)
#define AUGRAPH_NODE_NOT_FOUND  (-10860)
#define AU_FORMAT_NOT_SUPPORTED (-10868)
#define FILE_UNSUPPORTED_TYPE   FOURCC('t', 'y', 'p', '?')
#define FILE_UNSUPPORTED_FORMAT FOURCC('f', 'm', 't', '?')
#define FILE_UNSUPPORTED_PROP   FOURCC('p', 't', 'y', '?')
#define FILE_BAD_PROP_SIZE      FOURCC('!', 's', 'i', 'z')
#define FILE_INVALID            FOURCC('d', 't', 'a', '?')
#define CONV_FORMAT_UNSUPPORTED FOURCC('f', 'm', 't', '?')
#define CONV_PROP_UNSUPPORTED   FOURCC('p', 'r', 'o', 'p')

/* AudioUnit properties and scopes. */
#define AU_PROP_STREAM_FORMAT   8
#define AU_PROP_ELEMENT_COUNT   11
#define AU_PROP_RENDER_CALLBACK 23
#define AU_SCOPE_INPUT          1
#define AU_SCOPE_OUTPUT         2

/* An AudioStreamBasicDescription: mSampleRate (a double), then mFormatID,
   mFormatFlags, mBytesPerPacket, mFramesPerPacket, mBytesPerFrame,
   mChannelsPerFrame, mBitsPerChannel and a reserved word. */
#define ASBD_SIZE 40u
typedef struct {
    uint32_t rate, format, flags, bytes_per_packet, frames_per_packet, bytes_per_frame, channels,
        bits;
} asbd;

#define IMA4_PACKET_BYTES  34u /* per channel */
#define IMA4_PACKET_FRAMES 64u

/* The mixer's input format: signed integer, big-endian, packed. */
static const asbd mixer_format = {MIX_RATE, FOURCC('l', 'p', 'c', 'm'), 0xE, 4, 1, 4, 2, 16};
/* The other format the mixer's output (the output's input) may be set to:
   CoreAudio's canonical one, 32-bit float, big-endian, packed and not
   interleaved. Only sound.c ever sees what the mixer puts out, so either
   is taken and nothing else changes. */
static const asbd float_format = {MIX_RATE, FOURCC('l', 'p', 'c', 'm'), 0x2B, 4, 1, 4, 2, 32};

/* The guest scratch block the mixer and the converters pass the game's
   callbacks, allocated when the graph is initialized. */
#define SCR_FLAGS     0u   /* the render callback's ioActionFlags */
#define SCR_TIME      8u   /* its AudioTimeStamp, 64 bytes */
#define SCR_OUT_ABL   72u  /* its AudioBufferList: one buffer */
#define SCR_IN_ABL    88u  /* the input proc's AudioBufferList */
#define SCR_PACKETS   104u /* the input proc's ioNumberDataPackets */
#define SCR_DESCS     108u /* its outDataPacketDescription */
#define SCR_SAMPLES   112u /* CA_SLICE frames of the mixer's format */
#define SCR_SIZE      (SCR_SAMPLES + 4u * CA_SLICE)
/* A file's header is read through a block of its own. */
#define IO_SIZE       40u
#define IO_ACTUAL     36u

static struct {
    bool graph, opened, connected, initialized, started;
    uint32_t node_type[2]; /* node i + 1: 0, or 'auou' or 'aumx' */
    uint32_t ninputs;
    struct {
        uint32_t proc, refcon;
    } inputs[MAX_INPUTS];
    uint32_t scratch, io;
    uint64_t sample_time;
    int16_t slice[2 * CA_SLICE]; /* rendered, from slice_pos on not handed out */
    uint32_t slice_pos;
    struct {
        bool used;
        uint32_t client, read; /* the game's callbacks */
        uint32_t data_off;     /* where the packets start in the file */
        uint32_t nch, packets;
    } files[MAX_FILES];
    struct {
        bool used;
        uint32_t nch;
        int32_t map[2]; /* the source channel of each side, or -1 */
        int16_t left[2 * IMA4_PACKET_FRAMES]; /* decoded, from left_pos to left_n not yet out */
        int16_t last[2]; /* each channel's last sample, which the next packet may go on from */
        uint32_t left_n, left_pos;
    } convs[MAX_CONVS];
    bool warned_prop;
    bool out_float; /* the mixer's output was set to float_format */
} CA;

/* A snapshot's walk (snapshot.h): all of it is the game's; the counts and
   positions are checked before anything indexes with them. */
static void snap_walk(snapshot_walker *w) {
    snapshot_bytes(w, &CA, sizeof CA);
    snapshot_check(w, CA.ninputs <= MAX_INPUTS, "%u mixer inputs", CA.ninputs);
    snapshot_check(w, CA.slice_pos <= CA_SLICE, "slice position %u", CA.slice_pos);
    for (int i = 0; i < MAX_CONVS; i++)
        snapshot_check(w, CA.convs[i].nch <= 2 && CA.convs[i].left_pos <= CA.convs[i].left_n &&
                              CA.convs[i].left_n <= IMA4_PACKET_FRAMES &&
                              CA.convs[i].map[0] < 2 && CA.convs[i].map[1] < 2,
                       "converter %d", i);
    for (int i = 0; i < MAX_FILES; i++)
        snapshot_check(w, !CA.files[i].used || (CA.files[i].nch >= 1 && CA.files[i].nch <= 2),
                       "file %d of %u channels", i, CA.files[i].nch);
}
SNAPSHOT_STATE_WALK(CA, snap_walk)

void coreaudio_init(void) {
    memset(&CA, 0, sizeof CA);
    CA.slice_pos = CA_SLICE;
}

static void reply(int32_t err) { trap_return((uint32_t)err); }

/* ---- ASBDs ---- */

static void write_asbd(uint32_t p, const asbd *a) {
    double rate = a->rate;
    uint64_t bits;
    memcpy(&bits, &rate, 8);
    gm_w32(p, (uint32_t)(bits >> 32));
    gm_w32(p + 4, (uint32_t)bits);
    const uint32_t w[8] = {a->format, a->flags, a->bytes_per_packet, a->frames_per_packet,
                           a->bytes_per_frame, a->channels, a->bits, 0};
    for (uint32_t i = 0; i < 8; i++)
        gm_w32(p + 8 + 4 * i, w[i]);
}

/* The rate is a whole number of Hz, or 0. */
static asbd read_asbd(uint32_t p) {
    uint64_t bits = (uint64_t)gm_r32(p) << 32 | gm_r32(p + 4);
    double rate;
    memcpy(&rate, &bits, 8);
    asbd a = {0};
    a.rate = rate >= 1 && rate <= 1e6 && rate == (double)(uint32_t)rate ? (uint32_t)rate : 0;
    uint32_t *w[7] = {&a.format, &a.flags, &a.bytes_per_packet, &a.frames_per_packet,
                      &a.bytes_per_frame, &a.channels, &a.bits};
    for (uint32_t i = 0; i < 7; i++)
        *w[i] = gm_r32(p + 8 + 4 * i);
    return a;
}

static bool same_asbd(const asbd *a, const asbd *b) {
    return a->rate == b->rate && a->format == b->format && a->flags == b->flags &&
           a->bytes_per_packet == b->bytes_per_packet && a->frames_per_packet == b->frames_per_packet &&
           a->bytes_per_frame == b->bytes_per_frame && a->channels == b->channels && a->bits == b->bits;
}

static asbd ima4_format(uint32_t nch) {
    return (asbd){MIX_RATE, FOURCC('i', 'm', 'a', '4'), 0, IMA4_PACKET_BYTES * nch,
                  IMA4_PACKET_FRAMES, 0, nch, 0};
}

/* ---- the graph ---- */

static void need_graph(const char *call, uint32_t g) {
    if (g != CA_GRAPH || !CA.graph)
        trap_crash("%s: 0x%08x is not an AUGraph", call, g);
}

static uint32_t unit_of(uint32_t type) { return type == FOURCC('a', 'u', 'm', 'x') ? CA_UNIT_MIXER : CA_UNIT_OUTPUT; }

/* The node's index, or -1. */
static int node_index(int32_t node) {
    return node >= 1 && node <= 2 && CA.node_type[node - 1] ? node - 1 : -1;
}

static void h_new_augraph(void) {
    if (CA.graph)
        trap_crash("NewAUGraph: the shim has one graph");
    CA.graph = true;
    gm_w32(trap_arg(0), CA_GRAPH);
    reply(NO_ERR);
}

/* AUGraphNewNode(graph, const ComponentDescription *desc, classDataSize,
   classData, AUNode *outNode): the default output, or the stereo mixer. */
static void h_augraph_new_node(void) {
    need_graph("AUGraphNewNode", trap_arg(0));
    uint32_t d = trap_arg(1), type = gm_r32(d), sub = gm_r32(d + 4);
    bool output = type == FOURCC('a', 'u', 'o', 'u') && sub == FOURCC('d', 'e', 'f', ' ');
    bool mixer = type == FOURCC('a', 'u', 'm', 'x') && sub == FOURCC('s', 'm', 'x', 'r');
    int i = 0;
    while (i < 2 && CA.node_type[i] != 0)
        i++;
    if ((!output && !mixer) || i == 2 || (CA.node_type[0] == type && i == 1)) {
        log_msg("coreaudio: an AUGraph node '%.4s' '%.4s' is not provided",
                (const char *)gm_ptr(d, 4), (const char *)gm_ptr(d + 4, 4));
        reply(BAD_COMPONENT_TYPE);
        return;
    }
    CA.node_type[i] = type;
    gm_w32(trap_arg(4), (uint32_t)i + 1);
    reply(NO_ERR);
}

static void h_augraph_open(void) {
    need_graph("AUGraphOpen", trap_arg(0));
    CA.opened = true;
    reply(NO_ERR);
}

/* The mixer's output 0 to the output's input 0, the one connection. */
static void h_augraph_connect_node_input(void) {
    need_graph("AUGraphConnectNodeInput", trap_arg(0));
    int src = node_index((int32_t)trap_arg(1)), dst = node_index((int32_t)trap_arg(3));
    if (src < 0 || dst < 0) {
        reply(AUGRAPH_NODE_NOT_FOUND);
        return;
    }
    if (CA.node_type[src] != FOURCC('a', 'u', 'm', 'x') || CA.node_type[dst] != FOURCC('a', 'u', 'o', 'u') ||
        trap_arg(2) != 0 || trap_arg(4) != 0) {
        log_msg("coreaudio: only the mixer's output 0 to the output's input 0 is connected");
        reply(PARAM_ERR);
        return;
    }
    CA.connected = true;
    reply(NO_ERR);
}

/* AUGraphGetNodeInfo(graph, node, outDescription, outClassDataSize,
   outClassData, outAudioUnit): any of the outs may be NULL. */
static void h_augraph_get_node_info(void) {
    need_graph("AUGraphGetNodeInfo", trap_arg(0));
    int i = node_index((int32_t)trap_arg(1));
    if (i < 0) {
        reply(AUGRAPH_NODE_NOT_FOUND);
        return;
    }
    uint32_t type = CA.node_type[i], desc = trap_arg(2);
    if (desc) {
        gm_w32(desc, type);
        gm_w32(desc + 4, type == FOURCC('a', 'u', 'm', 'x') ? FOURCC('s', 'm', 'x', 'r') : FOURCC('d', 'e', 'f', ' '));
        gm_w32(desc + 8, FOURCC('a', 'p', 'p', 'l'));
        gm_w32(desc + 12, 0);
        gm_w32(desc + 16, 0);
    }
    if (trap_arg(3))
        gm_w32(trap_arg(3), 0);
    if (trap_arg(4))
        gm_w32(trap_arg(4), 0);
    if (trap_arg(5))
        gm_w32(trap_arg(5), unit_of(type));
    reply(NO_ERR);
}

static void h_augraph_initialize(void) {
    need_graph("AUGraphInitialize", trap_arg(0));
    if (!CA.scratch && !(CA.scratch = mm_new_ptr(SCR_SIZE, true)))
        trap_crash("AUGraphInitialize: out of guest memory");
    CA.initialized = true;
    reply(NO_ERR);
}

static void h_augraph_start(void) {
    need_graph("AUGraphStart", trap_arg(0));
    if (!CA.initialized || !CA.connected) {
        log_msg("coreaudio: AUGraphStart before the graph is connected and initialized");
        reply(PARAM_ERR);
        return;
    }
    CA.started = true;
    sound_set_source(coreaudio_render);
    reply(NO_ERR);
}

/* ---- the mixer's properties ---- */

static void need_unit(const char *call, uint32_t u) {
    if ((u != CA_UNIT_MIXER && u != CA_UNIT_OUTPUT) || !CA.graph)
        trap_crash("%s: 0x%08x is not an AudioUnit", call, u);
}

static void unsupported_property(const char *call, uint32_t unit, uint32_t prop, uint32_t scope) {
    if (!CA.warned_prop)
        log_msg("coreaudio: %s of property %u, scope %u, on the %s is not supported (and no "
                "other will be logged)",
                call, prop, scope, unit == CA_UNIT_MIXER ? "mixer" : "output");
    CA.warned_prop = true;
    reply(AU_INVALID_PROPERTY);
}

/* Whether a unit's scope has a stream format: 1 for the mixer's inputs, 2
   for the mixer's output and the output's input, which are one stream. */
static int stream_between(uint32_t u, uint32_t scope) {
    if (u == CA_UNIT_MIXER && scope == AU_SCOPE_INPUT)
        return 1;
    if ((u == CA_UNIT_MIXER && scope == AU_SCOPE_OUTPUT) || (u == CA_UNIT_OUTPUT && scope == AU_SCOPE_INPUT))
        return 2;
    return 0;
}

/* AudioUnitSetProperty(unit, id, scope, element, const void *data, size):
   the mixer's input count, its inputs' render callbacks, and the stream
   formats: the mixer's inputs take only its own format, the stream from
   the mixer to the output that or float_format. */
static void h_audio_unit_set_property(void) {
    uint32_t u = trap_arg(0), prop = trap_arg(1), scope = trap_arg(2), elem = trap_arg(3);
    uint32_t data = trap_arg(4), size = trap_arg(5);
    need_unit("AudioUnitSetProperty", u);
    if (u == CA_UNIT_MIXER && scope == AU_SCOPE_INPUT && prop == AU_PROP_ELEMENT_COUNT && size >= 4) {
        uint32_t n = gm_r32(data);
        if (n > MAX_INPUTS) {
            log_msg("coreaudio: %u mixer inputs; the shim's mixer has %d", n, MAX_INPUTS);
            reply(AU_INVALID_VALUE);
            return;
        }
        for (uint32_t i = n; i < CA.ninputs; i++)
            CA.inputs[i].proc = CA.inputs[i].refcon = 0;
        CA.ninputs = n;
        reply(NO_ERR);
        return;
    }
    if (u == CA_UNIT_MIXER && scope == AU_SCOPE_INPUT && prop == AU_PROP_RENDER_CALLBACK && size >= 8) {
        if (elem >= CA.ninputs) {
            reply(AU_INVALID_ELEMENT);
            return;
        }
        CA.inputs[elem].proc = gm_r32(data);
        CA.inputs[elem].refcon = gm_r32(data + 4);
        reply(NO_ERR);
        return;
    }
    if (prop == AU_PROP_STREAM_FORMAT && stream_between(u, scope) && size >= ASBD_SIZE) {
        asbd a = read_asbd(data);
        bool between = stream_between(u, scope) == 2;
        if (same_asbd(&a, &mixer_format) || (between && same_asbd(&a, &float_format))) {
            if (between)
                CA.out_float = same_asbd(&a, &float_format);
            reply(NO_ERR);
            return;
        }
        log_msg("coreaudio: a stream format of %u Hz '%.4s' flags 0x%x, %u bytes a packet, %u "
                "frames a packet, %u bytes a frame, %u channels of %u bits on the %s is not "
                "the mixer's",
                a.rate, (const char *)gm_ptr(data + 8, 4), a.flags, a.bytes_per_packet,
                a.frames_per_packet, a.bytes_per_frame, a.channels, a.bits,
                u == CA_UNIT_MIXER ? "mixer" : "output");
        reply(AU_FORMAT_NOT_SUPPORTED);
        return;
    }
    unsupported_property("AudioUnitSetProperty", u, prop, scope);
}

/* AudioUnitGetProperty(unit, id, scope, element, void *outData, UInt32
   *ioDataSize): a stream format, the mixer's own unless the stream from
   the mixer to the output was set to float_format. */
static void h_audio_unit_get_property(void) {
    uint32_t u = trap_arg(0), prop = trap_arg(1), scope = trap_arg(2), out = trap_arg(4),
             size = trap_arg(5);
    need_unit("AudioUnitGetProperty", u);
    int between = stream_between(u, scope);
    if (prop == AU_PROP_STREAM_FORMAT && between) {
        if (gm_r32(size) < ASBD_SIZE) {
            reply(PARAM_ERR);
            return;
        }
        write_asbd(out, between == 2 && CA.out_float ? &float_format : &mixer_format);
        gm_w32(size, ASBD_SIZE);
        reply(NO_ERR);
        return;
    }
    unsupported_property("AudioUnitGetProperty", u, prop, scope);
}

/* ---- rendering ---- */

/* One slice from every input with a callback, summed into CA.slice. */
static void render_slice(void) {
    static int32_t sum[2 * CA_SLICE];
    memset(sum, 0, sizeof sum);
    uint32_t s = CA.scratch, samples = s + SCR_SAMPLES;
    for (uint32_t e = 0; e < CA.ninputs; e++) {
        uint32_t proc = CA.inputs[e].proc;
        if (!proc)
            continue;
        memset(gm_ptr(s, SCR_IN_ABL), 0, SCR_IN_ABL); /* the flags, the time stamp and the list */
        memset(gm_ptr(samples, 4 * CA_SLICE), 0, 4 * CA_SLICE);
        double t = (double)CA.sample_time;
        uint64_t bits;
        memcpy(&bits, &t, 8);
        gm_w32(s + SCR_TIME, (uint32_t)(bits >> 32));
        gm_w32(s + SCR_TIME + 4, (uint32_t)bits);
        gm_w32(s + SCR_TIME + 56, 1); /* kAudioTimeStampSampleTimeValid */
        gm_w32(s + SCR_OUT_ABL, 1);
        gm_w32(s + SCR_OUT_ABL + 4, 2);
        gm_w32(s + SCR_OUT_ABL + 8, 4 * CA_SLICE);
        gm_w32(s + SCR_OUT_ABL + 12, samples);
        uint32_t args[6] = {CA.inputs[e].refcon, s + SCR_FLAGS, s + SCR_TIME, e, CA_SLICE, s + SCR_OUT_ABL};
        guest_call(proc, 6, args);
        /* The callback may have pointed the buffer at its own samples. */
        const uint8_t *p = gm_ptr(gm_r32(s + SCR_OUT_ABL + 12), 4 * CA_SLICE);
        for (uint32_t k = 0; k < 2 * CA_SLICE; k++)
            sum[k] += (int16_t)rd_be16(p + 2 * k);
    }
    for (uint32_t k = 0; k < 2 * CA_SLICE; k++)
        CA.slice[k] = (int16_t)(sum[k] > 32767 ? 32767 : sum[k] < -32768 ? -32768 : sum[k]);
    CA.sample_time += CA_SLICE;
    CA.slice_pos = 0;
}

void coreaudio_render(int16_t *out, uint32_t frames) {
    if (!CA.started)
        return;
    while (frames > 0) {
        if (CA.slice_pos == CA_SLICE)
            render_slice();
        uint32_t n = CA_SLICE - CA.slice_pos;
        if (n > frames)
            n = frames;
        const int16_t *in = CA.slice + 2 * CA.slice_pos;
        for (uint32_t k = 0; k < 2 * n; k++) {
            int32_t v = out[k] + in[k];
            out[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        out += 2 * n;
        frames -= n;
        CA.slice_pos += n;
    }
}

/* ---- AudioFiles ---- */

static int need_file(const char *call, uint32_t f) {
    uint32_t i = (f - CA_FILE_BASE) / 16u;
    if (f < CA_FILE_BASE || (f & 15u) || i >= MAX_FILES || !CA.files[i].used)
        trap_crash("%s: 0x%08x is not an AudioFile", call, f);
    return (int)i;
}

/* Reads n bytes (at most IO_SIZE - 4) at pos of the file through its read
   callback into the io block. False if the callback fails or reads less. */
static bool read_at(uint32_t client, uint32_t read, uint64_t pos, uint32_t n, uint32_t buf) {
    uint32_t args[6] = {client, (uint32_t)(pos >> 32), (uint32_t)pos, n, buf, CA.io + IO_ACTUAL};
    gm_w32(CA.io + IO_ACTUAL, 0);
    return guest_call(read, 6, args) == 0 && gm_r32(CA.io + IO_ACTUAL) == n;
}

/* An 80-bit extended float's value, if it is a whole number below 2^32. */
static uint32_t ext80_whole(const uint8_t *b) {
    int e = (int)(rd_be16(b) & 0x7FFF) - 16383;
    uint64_t m = (uint64_t)rd_be32(b + 2) << 32 | rd_be32(b + 6);
    if (b[0] & 0x80 || e < 0 || e > 31)
        return 0;
    return (uint32_t)(m >> (63 - e));
}

/* AudioFileOpenWithCallbacks(clientData, readFunc, writeFunc, getSizeFunc,
   setSizeFunc, typeHint, AudioFileID *out): an AIFF-C file of IMA4 at
   the mixer's rate, read only. */
static void h_audio_file_open_with_callbacks(void) {
    uint32_t client = trap_arg(0), read = trap_arg(1), get_size = trap_arg(3), out = trap_arg(6);
    int i = 0;
    while (i < MAX_FILES && CA.files[i].used)
        i++;
    if (i == MAX_FILES)
        trap_crash("AudioFileOpenWithCallbacks: more than %d files open", MAX_FILES);
    if (!CA.io && !(CA.io = mm_new_ptr(IO_SIZE, true)))
        trap_crash("AudioFileOpenWithCallbacks: out of guest memory");
    uint32_t size_args[1] = {client};
    uint32_t hi = guest_call(get_size, 1, size_args), lo = guest_last_r4();
    uint64_t size = (uint64_t)hi << 32 | lo;
    uint32_t b = CA.io;
    const uint8_t *p = gm_ptr(b, IO_SIZE);
    if (size < 12 || size > 0xFFFFFFFFu || !read_at(client, read, 0, 12, b) || rd_be32(p) != FOURCC('F', 'O', 'R', 'M') ||
        rd_be32(p + 8) != FOURCC('A', 'I', 'F', 'C')) {
        log_msg("coreaudio: AudioFileOpenWithCallbacks: not an AIFF-C file");
        reply(FILE_UNSUPPORTED_TYPE);
        return;
    }
    uint32_t nch = 0, packets = 0, rate = 0, comp = 0, data_off = 0, data_len = 0;
    bool comm = false, ssnd = false;
    for (uint64_t pos = 12; pos + 8 <= size;) {
        if (!read_at(client, read, pos, 8, b))
            break;
        uint32_t id = rd_be32(p), len = rd_be32(p + 4);
        if (id == FOURCC('C', 'O', 'M', 'M') && len >= 22 && read_at(client, read, pos + 8, 22, b)) {
            nch = rd_be16(p), packets = rd_be32(p + 2), rate = ext80_whole(p + 8), comp = rd_be32(p + 18);
            comm = true;
        } else if (id == FOURCC('S', 'S', 'N', 'D') && len >= 8 && read_at(client, read, pos + 8, 8, b)) {
            uint32_t skip = rd_be32(p);
            if (skip <= len - 8 && pos + 16 + skip <= size) {
                data_off = (uint32_t)(pos + 16 + skip);
                data_len = len - 8 - skip;
                ssnd = true;
            }
        }
        pos += 8 + (uint64_t)len + (len & 1);
    }
    if (ssnd && (uint64_t)data_off + data_len > size)
        ssnd = false; /* the sound runs past the end */
    if (comm && ssnd && (comp != FOURCC('i', 'm', 'a', '4') || rate != MIX_RATE || nch < 1 || nch > 2)) {
        log_msg("coreaudio: AudioFileOpenWithCallbacks: an AIFF-C file of %u channels of "
                "'%c%c%c%c' at %u Hz; only IMA4 at %d Hz is supported",
                nch, (char)(comp >> 24), (char)(comp >> 16), (char)(comp >> 8), (char)comp, rate, MIX_RATE);
        reply(FILE_UNSUPPORTED_FORMAT);
        return;
    }
    if (!comm || !ssnd || (uint64_t)packets * IMA4_PACKET_BYTES * nch > data_len) {
        log_msg("coreaudio: AudioFileOpenWithCallbacks: the AIFF-C file's COMM or SSND chunk is "
                "missing or doesn't fit");
        reply(FILE_INVALID);
        return;
    }
    CA.files[i].used = true;
    CA.files[i].client = client, CA.files[i].read = read;
    CA.files[i].data_off = data_off, CA.files[i].nch = nch, CA.files[i].packets = packets;
    gm_w32(out, CA_FILE_BASE + 16u * (uint32_t)i);
    reply(NO_ERR);
}

/* A property's size, or 0 for one that isn't there. */
static uint32_t file_property_size(uint32_t prop) {
    switch (prop) {
    case FOURCC('d', 'f', 'm', 't'): return ASBD_SIZE;
    case FOURCC('b', 'c', 'n', 't'):
    case FOURCC('p', 'c', 'n', 't'): return 8;
    case FOURCC('p', 's', 'z', 'e'): return 4;
    default: return 0; /* among them 'mgic': an IMA4 file has no magic cookie */
    }
}

/* AudioFileGetPropertyInfo(file, id, UInt32 *outDataSize, UInt32 *isWritable) */
static void h_audio_file_get_property_info(void) {
    need_file("AudioFileGetPropertyInfo", trap_arg(0));
    uint32_t size = file_property_size(trap_arg(1));
    if (!size) {
        reply(FILE_UNSUPPORTED_PROP);
        return;
    }
    if (trap_arg(2))
        gm_w32(trap_arg(2), size);
    if (trap_arg(3))
        gm_w32(trap_arg(3), 0);
    reply(NO_ERR);
}

/* AudioFileGetProperty(file, id, UInt32 *ioDataSize, void *outData): the
   data format, the bytes and packets of sound, and the packet size. */
static void h_audio_file_get_property(void) {
    int i = need_file("AudioFileGetProperty", trap_arg(0));
    uint32_t prop = trap_arg(1), io = trap_arg(2), out = trap_arg(3);
    uint32_t size = file_property_size(prop), nch = CA.files[i].nch;
    if (!size) {
        reply(FILE_UNSUPPORTED_PROP);
        return;
    }
    if (gm_r32(io) < size) {
        reply(FILE_BAD_PROP_SIZE);
        return;
    }
    uint64_t packets = CA.files[i].packets;
    switch (prop) {
    case FOURCC('d', 'f', 'm', 't'): {
        asbd a = ima4_format(nch);
        write_asbd(out, &a);
        break;
    }
    case FOURCC('b', 'c', 'n', 't'):
        gm_w32(out, 0);
        gm_w32(out + 4, (uint32_t)(packets * IMA4_PACKET_BYTES * nch));
        break;
    case FOURCC('p', 'c', 'n', 't'):
        gm_w32(out, 0);
        gm_w32(out + 4, (uint32_t)packets);
        break;
    default:
        gm_w32(out, IMA4_PACKET_BYTES * nch);
        break;
    }
    gm_w32(io, size);
    reply(NO_ERR);
}

/* AudioFileReadPackets(file, Boolean useCache, UInt32 *ioNumBytes,
   AudioStreamPacketDescription *outDescriptions, SInt64 startPacket,
   UInt32 *ioNumPackets, void *outBuffer): read through the game's
   callback into its buffer, as many whole packets as fit. */
static void h_audio_file_read_packets(void) {
    int i = need_file("AudioFileReadPackets", trap_arg(0));
    uint32_t io_bytes = trap_arg(2), io_packets = trap_arg(6), buf = trap_arg(7);
    uint64_t start = (uint64_t)trap_arg(4) << 32 | trap_arg(5);
    uint32_t bpp = IMA4_PACKET_BYTES * CA.files[i].nch, total = CA.files[i].packets;
    uint32_t n = start < total ? total - (uint32_t)start : 0;
    if (n > gm_r32(io_packets))
        n = gm_r32(io_packets);
    if (n > gm_r32(io_bytes) / bpp)
        n = gm_r32(io_bytes) / bpp;
    if (n) {
        gm_ptr(buf, n * bpp); /* crashes now if the buffer isn't all guest memory */
        uint32_t args[6] = {CA.files[i].client, 0, CA.files[i].data_off + (uint32_t)start * bpp, n * bpp,
                            buf, CA.io + IO_ACTUAL};
        gm_w32(CA.io + IO_ACTUAL, 0);
        if (guest_call(CA.files[i].read, 6, args) != 0 || gm_r32(CA.io + IO_ACTUAL) != n * bpp) {
            reply(FILE_INVALID);
            return;
        }
    }
    gm_w32(io_bytes, n * bpp);
    gm_w32(io_packets, n);
    reply(NO_ERR);
}

static void h_audio_file_close(void) {
    CA.files[need_file("AudioFileClose", trap_arg(0))].used = false;
    reply(NO_ERR);
}

/* ---- AudioConverters ---- */

static int need_conv(const char *call, uint32_t c) {
    uint32_t i = (c - CA_CONV_BASE) / 16u;
    if (c < CA_CONV_BASE || (c & 15u) || i >= MAX_CONVS || !CA.convs[i].used)
        trap_crash("%s: 0x%08x is not an AudioConverter", call, c);
    return (int)i;
}

/* AudioConverterNew(const ASBD *source, const ASBD *dest, AudioConverterRef
   *out): IMA4 at the mixer's rate to the mixer's format. */
static void h_audio_converter_new(void) {
    asbd src = read_asbd(trap_arg(0)), dst = read_asbd(trap_arg(1));
    asbd want = ima4_format(src.channels);
    if ((src.channels != 1 && src.channels != 2) || !same_asbd(&src, &want) || !same_asbd(&dst, &mixer_format)) {
        log_msg("coreaudio: AudioConverterNew from '%c%c%c%c' (%u channels, %u Hz) to '%c%c%c%c': "
                "only IMA4 at %d Hz to the mixer's format is supported",
                (char)(src.format >> 24), (char)(src.format >> 16), (char)(src.format >> 8), (char)src.format,
                src.channels, src.rate, (char)(dst.format >> 24), (char)(dst.format >> 16),
                (char)(dst.format >> 8), (char)dst.format, MIX_RATE);
        reply(CONV_FORMAT_UNSUPPORTED);
        return;
    }
    int i = 0;
    while (i < MAX_CONVS && CA.convs[i].used)
        i++;
    if (i == MAX_CONVS)
        trap_crash("AudioConverterNew: more than %d converters", MAX_CONVS);
    memset(&CA.convs[i], 0, sizeof CA.convs[i]);
    CA.convs[i].used = true;
    CA.convs[i].nch = src.channels;
    CA.convs[i].map[0] = 0;
    CA.convs[i].map[1] = src.channels == 2 ? 1 : -1;
    gm_w32(trap_arg(2), CA_CONV_BASE + 16u * (uint32_t)i);
    reply(NO_ERR);
}

/* AudioConverterSetProperty(conv, id, size, const void *data): the
   channel map, and a decompression cookie, which IMA4 has no use for. */
static void h_audio_converter_set_property(void) {
    int i = need_conv("AudioConverterSetProperty", trap_arg(0));
    uint32_t prop = trap_arg(1), size = trap_arg(2), data = trap_arg(3);
    if (prop == FOURCC('d', 'm', 'g', 'c')) {
        reply(NO_ERR);
        return;
    }
    if (prop == FOURCC('c', 'h', 'm', 'p') && size == 8) {
        int32_t m[2] = {(int32_t)gm_r32(data), (int32_t)gm_r32(data + 4)};
        for (int c = 0; c < 2; c++)
            if (m[c] < -1 || m[c] >= (int32_t)CA.convs[i].nch) {
                reply(PARAM_ERR);
                return;
            }
        CA.convs[i].map[0] = m[0], CA.convs[i].map[1] = m[1];
        reply(NO_ERR);
        return;
    }
    log_msg("coreaudio: AudioConverterSetProperty '%c%c%c%c' (%u bytes) is not supported",
            (char)(prop >> 24), (char)(prop >> 16), (char)(prop >> 8), (char)prop, size);
    reply(CONV_PROP_UNSUPPORTED);
}

static const int16_t ima_steps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t ima_index_steps[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

/* Decodes one channel's 34-byte IMA4 block into 64 samples, stride apart.
   The block's header holds the top nine bits of the predictor; when they
   are those of the channel's last sample, Apple's decoder goes on from that
   sample instead, and so does this one (it matches afconvert's output
   sample for sample). */
static void decode_ima4(const uint8_t *b, int16_t *out, int stride, int16_t *last) {
    uint16_t head = rd_be16(b);
    int pred = (int16_t)(head & 0xFF80), index = head & 0x7F;
    if ((*last & 0xFF80) == (pred & 0xFF80))
        pred = *last;
    if (index > 88)
        index = 88;
    for (uint32_t k = 0; k < IMA4_PACKET_FRAMES; k++) {
        uint8_t byte = b[2 + k / 2], n = k & 1 ? byte >> 4 : byte & 0xF;
        int step = ima_steps[index], diff = step >> 3;
        if (n & 1)
            diff += step >> 2;
        if (n & 2)
            diff += step >> 1;
        if (n & 4)
            diff += step;
        pred += n & 8 ? -diff : diff;
        pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
        index += ima_index_steps[n];
        index = index < 0 ? 0 : index > 88 ? 88 : index;
        out[k * (uint32_t)stride] = (int16_t)pred;
    }
    *last = (int16_t)pred;
}

/* AudioConverterFillComplexBuffer(conv, inputProc, userData, UInt32
   *ioOutputDataPacketSize, AudioBufferList *outData, outPacketDescription):
   frames of the mixer's format into the one buffer, decoding the packets
   the input proc gives (inputProc(conv, UInt32 *ioNumberDataPackets,
   AudioBufferList *ioData, outDataPacketDescription, userData)). When the
   proc gives no packets, or an error, the frames so far are the answer,
   with its error. */
/* Writes what is left of the converter's packet to out, from frame *done
   up to want, through its channel map. */
static void drain(int i, uint8_t *out, uint32_t *done, uint32_t want) {
    typeof(CA.convs[0]) *c = &CA.convs[i];
    for (; c->left_pos < c->left_n && *done < want; c->left_pos++, (*done)++)
        for (uint32_t side = 0; side < 2; side++) {
            int32_t src = c->map[side];
            int16_t v = src < 0 ? 0 : c->left[2 * c->left_pos + (uint32_t)src];
            wr_be16(out + 4 * *done + 2 * side, (uint16_t)v);
        }
}

static void h_audio_converter_fill_complex_buffer(void) {
    int i = need_conv("AudioConverterFillComplexBuffer", trap_arg(0));
    uint32_t proc = trap_arg(1), user = trap_arg(2), io_size = trap_arg(3), abl = trap_arg(4);
    if (!CA.scratch)
        trap_crash("AudioConverterFillComplexBuffer: the graph isn't initialized");
    if (gm_r32(abl) != 1) {
        reply(PARAM_ERR);
        return;
    }
    uint32_t dst = gm_r32(abl + 12), cap = gm_r32(abl + 8) / 4, want = gm_r32(io_size);
    if (want > cap)
        want = cap;
    uint8_t *out = want ? gm_ptr(dst, 4 * want) : NULL;
    uint32_t done = 0, nch = CA.convs[i].nch, s = CA.scratch;
    int32_t status = NO_ERR;
    while (done < want) {
        if (CA.convs[i].left_pos < CA.convs[i].left_n) {
            drain(i, out, &done, want);
            continue;
        }
        uint32_t ask = (want - done + IMA4_PACKET_FRAMES - 1) / IMA4_PACKET_FRAMES;
        gm_w32(s + SCR_IN_ABL, 1);
        gm_w32(s + SCR_IN_ABL + 4, nch);
        gm_w32(s + SCR_IN_ABL + 8, 0);
        gm_w32(s + SCR_IN_ABL + 12, 0);
        gm_w32(s + SCR_PACKETS, ask);
        gm_w32(s + SCR_DESCS, 0);
        uint32_t args[5] = {trap_arg(0), s + SCR_PACKETS, s + SCR_IN_ABL, s + SCR_DESCS, user};
        status = (int32_t)guest_call(proc, 5, args);
        uint32_t got = gm_r32(s + SCR_PACKETS), data = gm_r32(s + SCR_IN_ABL + 12);
        uint32_t bytes = gm_r32(s + SCR_IN_ABL + 8), bpp = IMA4_PACKET_BYTES * nch;
        if (got > bytes / bpp)
            got = bytes / bpp;
        if (got > ask)
            got = ask; /* more than asked for: the rest is dropped */
        for (uint32_t k = 0; k < got; k++) {
            const uint8_t *pk = gm_ptr(data + k * bpp, bpp);
            for (uint32_t ch = 0; ch < nch; ch++)
                decode_ima4(pk + ch * IMA4_PACKET_BYTES, CA.convs[i].left + ch, 2, &CA.convs[i].last[ch]);
            CA.convs[i].left_n = IMA4_PACKET_FRAMES, CA.convs[i].left_pos = 0;
            drain(i, out, &done, want);
        }
        if (got == 0 || status != NO_ERR)
            break;
    }
    gm_w32(io_size, done);
    gm_w32(abl + 8, 4 * done);
    reply(status);
}

void coreaudio_register(void) {
    trap_register("NewAUGraph", h_new_augraph);
    trap_register("AUGraphNewNode", h_augraph_new_node);
    trap_register("AUGraphOpen", h_augraph_open);
    trap_register("AUGraphConnectNodeInput", h_augraph_connect_node_input);
    trap_register("AUGraphGetNodeInfo", h_augraph_get_node_info);
    trap_register("AUGraphInitialize", h_augraph_initialize);
    trap_register("AUGraphStart", h_augraph_start);
    trap_register("AudioUnitSetProperty", h_audio_unit_set_property);
    trap_register("AudioUnitGetProperty", h_audio_unit_get_property);
    trap_register("AudioFileOpenWithCallbacks", h_audio_file_open_with_callbacks);
    trap_register("AudioFileGetPropertyInfo", h_audio_file_get_property_info);
    trap_register("AudioFileGetProperty", h_audio_file_get_property);
    trap_register("AudioFileReadPackets", h_audio_file_read_packets);
    trap_register("AudioFileClose", h_audio_file_close);
    trap_register("AudioConverterNew", h_audio_converter_new);
    trap_register("AudioConverterSetProperty", h_audio_converter_set_property);
    trap_register("AudioConverterFillComplexBuffer", h_audio_converter_fill_complex_buffer);
}
