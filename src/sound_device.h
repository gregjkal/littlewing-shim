#pragma once
#include <SDL3/SDL_audio.h>
#include <stdbool.h>
#include <stdint.h>

/* The app's own audio devices: the Sound Manager's and the monitor
   (sound.c), and the picker's music (theme.c). Each closes after
   SOUND_NAP_MS of silence (sound.h), since an open device keeps the Mac
   from sleeping, and opens again for the next sound. */

/* The silence of a device of the app's own (the Sound Manager's, the
   monitor, the picker's music), closed after SOUND_NAP_MS of it (above). */
typedef struct {
    uint64_t since; /* SDL_GetTicks when last heard */
} sound_quiet;

/* Starts SDL's audio, before a device is opened; each start is matched by
   an SDL_QuitSubSystem(SDL_INIT_AUDIO), or by the program's end. False
   (logged as who's) if it can't start. */
bool sound_start_audio(const char *who);

/* Whether to try opening a device again that was closed for a silence, or
   failed: not more than once a second (*retry_at holds when), so a device
   that can't be opened isn't tried at every sound. */
bool sound_may_open(uint64_t *retry_at);

/* Opens the default output for 16-bit stereo at rate Hz, at sound_volume,
   paused for the caller to resume once ready; fn is SDL's callback for the
   stream, or NULL for one the caller puts frames in. Starts q's silence.
   NULL, logged as who's, if it can't be opened. */
SDL_AudioStream *sound_open_device(int rate, SDL_AudioStreamCallback fn, sound_quiet *q,
                                   const char *who);

/* Something was heard: q's silence starts again. */
void sound_quiet_heard(sound_quiet *q);

/* Closes *s, making it NULL, once q has been silent long enough. True if
   it closed it (logged as who's). */
bool sound_quiet_close(SDL_AudioStream **s, const sound_quiet *q, const char *who);
