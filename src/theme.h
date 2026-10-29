#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "game.h"

/* The picker's music: the selected card plays its game's opening music,
   heard from the user's own copy of the game. Nothing of a game's is
   shipped, so each clip is rendered once, the first time the picker shows
   that game: a second copy of this program plays the game headless on the
   fixed clock, at utility priority, one game at a time, the selected game
   first, with LOONY_WAV recording what it plays, in a save folder of its
   own that is then removed (so the user's preferences and high scores are
   never touched). The game's row says what to press first (theme_script,
   a LOONY_SCRIPT) and from when to listen (theme_from). theme_clip trims
   the recording to the music, and the clip is kept as THEME_FILE in the
   game's save folder: an empty one for a game that played nothing, so
   it isn't rendered again. Delete it to render it again.

   No music without a save root (LOONY_DATA_DIR set, as in the tests), on
   the fixed clock, or with LOONY_PICKER_MUSIC=0. Its loudness follows the
   Sound menu (sound_volume). Problems are logged, never fatal. */

#define THEME_FILE "picker-music.wav"
#define THEME_RATE 44100
#define THEME_MAX_SECONDS 20 /* a clip is at most this long */
#define THEME_GAP_MS 2000    /* this much silence ends the music */
#define THEME_FADE_MS 1500   /* a clip cut at its longest fades out over this */
#define THEME_QUIET 64       /* a sample at most this loud is silence */

/* The clip in a recording of frames 16-bit stereo frames: from the first
   sound at or after frame from, to the first THEME_GAP_MS of silence after
   it (a pause in a jingle, or between a sting and the music, is shorter)
   or THEME_MAX_SECONDS, whichever is first, the latter faded out. The
   clip's frames are written to a new buffer (*out, NULL for no sound) and
   their number returned: 0 if there is no sound from from on. */
uint32_t theme_clip(const int16_t *samples, uint32_t frames, uint32_t from, int16_t **out);

/* Starts the music for the picker's games (n of them): renders those
   without a clip, the selected one first. self is this program. */
void theme_start(const game_info *const *games, int n, int selected, const char *self);

/* Plays the clip of games[selected] (theme_start's games) from its start,
   once, stopping the one playing; one being rendered plays when it is. */
void theme_select(int selected);

/* Called every frame: collects a finished render and starts the next. */
void theme_tend(void);

/* Stops the music and any render (its recording is dropped). */
void theme_stop(void);
