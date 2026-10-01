#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Sound Manager, silent for now: channels, command queues and callbacks
   behave as on a real Mac (a buffer keeps its channel busy for as long as it
   would play), but nothing is heard. Audio output arrives with milestone 5.
   Sound problems are logged, never fatal. */

/* SndCommand numbers. */
#define SND_NULL_CMD     0
#define SND_QUIET_CMD    3
#define SND_FLUSH_CMD    4
#define SND_WAIT_CMD     10
#define SND_CALLBACK_CMD 13
#define SND_VOLUME_CMD   46
#define SND_SOUND_CMD    80
#define SND_BUFFER_CMD   81

#define SND_QUEUE_FULL_ERR (-203)
#define SND_BAD_CHANNEL_ERR (-205)

void sound_init(void);

/* Runs queued commands whose time has come and delivers due callbacks
   (guest_call). The event loop calls this on every iteration. */
void sound_pump(void);

/* Seconds a sampled sound header plays for: frames / sample rate. 0 for a
   header that can't be parsed (which is logged). */
double sound_header_seconds(uint32_t header);

/* Registers NewSndCallBackUPP, SndNewChannel, SndDisposeChannel,
   SndDoCommand, SndDoImmediate and SndChannelStatus. */
void sound_register(void);
