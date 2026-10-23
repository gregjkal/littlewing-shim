#pragma once
#include <stdint.h>

/* The CoreAudio calls Mad Daedalus plays its sound with: an AUGraph of a
   stereo mixer feeding the default output, AudioFiles read through the
   game's own callbacks, and AudioConverters that decode them.

   The graph is one mixer ('aumx' 'smxr') connected to one output ('auou'
   'def '). The game sets the mixer's input count and, on an input, a
   render callback: game code that fills a slice of the mixer's input
   format. Once the graph has started, the mixer is sound.c's source
   (sound_set_source): it renders CA_SLICE frames at a time on the main
   thread, calling each input's callback in input order, and sums them,
   clamped, into 16-bit stereo. So the sound is clock-driven, rendered as
   the game's clock goes, and the same on every run of a fixed clock.

   The mixer's input format, which the game asks for and converts to, is
   16-bit signed big-endian interleaved stereo at MIX_RATE: integers, so
   what is rendered is the same in every build.

   An AudioFile is an AIFF-C file of IMA4 (Apple's 'ima4') sound, read
   with the callbacks AudioFileOpenWithCallbacks is given: the header when
   it opens, and the packets AudioFileReadPackets asks for, straight into
   the game's buffer. Nothing of the file is kept.

   An AudioConverter decodes IMA4 at the mixer's rate to the mixer's
   format, with a channel map (kAudioConverterChannelMap) that sends a
   mono sound to both sides. AudioConverterFillComplexBuffer asks the
   game's input proc for packets, which point into the game's memory. Each
   34-byte IMA4 packet holds 64 frames of one channel, and a converter
   keeps what is left of the last packet and each channel's last sample,
   which the next packet may go on from as Apple's decoder does.

   Graphs, units, files and converters are opaque IDs from CA_TAG_BASE.
   Problems are logged and answered with CoreAudio's error codes, never
   fatal, except an ID that isn't one, which is the game's bug. */

#define CA_TAG_BASE 0x0D000000u
#define CA_SLICE 512 /* frames the mixer asks each input for at a time */

/* Forgets the graph, the files and the converters. */
void coreaudio_init(void);

/* The mixer, as sound.c's source once the graph has started: adds the next
   frames stereo frames to out, clamped, rendering slices as it needs them.
   Before the graph starts it adds nothing. */
void coreaudio_render(int16_t *out, uint32_t frames);

/* Registers NewAUGraph, AUGraphNewNode, AUGraphOpen,
   AUGraphConnectNodeInput, AUGraphGetNodeInfo, AUGraphInitialize,
   AUGraphStart, AudioUnitGetProperty, AudioUnitSetProperty,
   AudioFileOpenWithCallbacks, AudioFileGetPropertyInfo,
   AudioFileGetProperty, AudioFileReadPackets, AudioFileClose,
   AudioConverterNew, AudioConverterSetProperty and
   AudioConverterFillComplexBuffer. */
void coreaudio_register(void);
