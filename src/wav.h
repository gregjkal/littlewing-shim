#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Streams 16-bit stereo PCM into a .wav file. The header's sizes are filled
   in by wav_close, so a file that was never closed has sizes of zero. */

typedef struct {
    FILE *f;
    uint32_t rate, frames;
    bool failed;
} wav_file;

/* Opens path for writing at rate Hz. Returns false if it can't be created. */
bool wav_open(wav_file *w, const char *path, uint32_t rate);

/* Appends frames stereo frames (2 * frames samples). */
void wav_write(wav_file *w, const int16_t *samples, uint32_t frames);

/* Fills in the header and closes the file. Returns false if any write
   failed. */
bool wav_close(wav_file *w);
