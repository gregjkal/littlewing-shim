#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Sound Manager: channels, command queues and callbacks, played through
   mixer.c. Commands are copied to the host when the game issues them, so the
   mixer never reads guest memory. With an audio device, SDL's audio thread
   plays the queues in real time; without one (the fixed clock, tests, no
   device), sound_pump renders them up to the current time, so a channel stays
   busy for as long as its sounds would play either way. Callbacks always run
   on the main thread. Sound problems are logged, never fatal. */

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

/* Drops every channel. Rendering is clock-driven until sound_start_output. */
void sound_init(void);

/* Opens the audio device (unless LOONY_FIXED_CLOCK=1) and starts recording
   to $LOONY_WAV if it is set. Without a device, sound stays clock-driven and
   silent. Registers an atexit handler that stops the device and finishes the
   recording. */
void sound_start_output(void);

/* The volume the sound is played at, from 0 (silent) to 100 (as the game
   made it); SOUND_DEFAULT_VOLUME until set. It scales what the audio device
   is given, not what is mixed, so a LOONY_WAV recording doesn't change with
   it. Values outside 0-100 are clamped. Can be set before sound_start_output. */
#define SOUND_DEFAULT_VOLUME 80
void sound_set_volume(int percent);
int sound_volume(void);

/* The gain for a volume: cubed, so equal steps of the volume sound about
   equally large (80 is about 6 dB down; 50 is 18 dB down). */
float sound_gain(int percent);

/* Watching a replay (watch.h): opens the audio device for the replay's
   sound, which stays clock-driven: what the mixer renders as the replay's
   clock moves is also played, while sound_monitor is on (off at first),
   SOUND_MONITOR_LEAD_MS behind. Without a device it stays silent. */
#define SOUND_MONITOR_LEAD_MS 50
void sound_open_monitor(void);
/* On: plays from now. Off: silent, dropping what is waiting to play. */
void sound_monitor(bool on);

/* Gets everything the mixer renders, in order from sound_init, as it is
   rendered (on the audio thread with a device), at the mixer's level like
   LOONY_WAV. Can be set before sound_init. When the clock drives the sound
   (no device), at is where the first frame is in the game's sound, counted
   from sound_init: a replay that goes to a snapshot (gamestart.h) renders
   from the snapshot's frame on, not from where it was. With a device, 0. */
typedef void (*sound_tap_fn)(const int16_t *samples, uint32_t frames, uint64_t at);
void sound_set_tap(sound_tap_fn fn);

/* Renders up to now if clock-driven, then delivers the callbacks the queues
   have reached (guest_call), oldest first. The event loop calls this on every
   iteration. */
void sound_pump(void);

/* Clock-driven: the wall-clock seconds spent rendering since sound_init
   (watch.h measures the mixer's share of an unpaced replay with it). */
double sound_render_seconds(void);

/* Registers NewSndCallBackUPP, SndNewChannel, SndDisposeChannel,
   SndDoCommand, SndDoImmediate and SndChannelStatus. */
void sound_register(void);
