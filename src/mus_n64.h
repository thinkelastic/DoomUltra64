/*
 * mus_n64 -- level music streamed from a flashcart's SD card.
 *
 * Every call is safe when no card is present; mus_init reports that once and
 * the rest become no-ops. See mus_n64.c for why music is the only asset here
 * that does not live in the ROM.
 */
#ifndef MUS_N64_H
#define MUS_N64_H

#include <stdbool.h>

/* Mixer channel reserved for music. Effects use 0..SFX_CHANNELS-1, so this
 * sits above them and is never stolen by a loud gunshot. */
#define MUS_CHANNEL 8

/* Root-directory listing captured when the music file cannot be opened, shown
 * on screen so the exact filenames are visible without a USB cable. */
#define MUS_LIST_MAX 10
extern char mus_listing[MUS_LIST_MAX][40];
extern int  mus_listing_count;

/* Mount the card and open the music WAD. False means no music this session --
 * no flashcart, no card, or no such file. Not an error. */
bool mus_init(const char *wadpath);

/* Start the track for a level, looping. Silently does nothing if that level
 * has no track. */
void mus_play(int episode, int map);
void mus_play_track(const char *track);

void mus_stop(void);

/* Top up the stream. Call once a frame from the main loop -- never from the
 * mixer callback, which must not touch the card. */
void mus_update(void);

/* A short word describing what music is doing, for the on-screen readout:
  * no-sd, no-music, no-track, ready, play, seek-died, read-died. The debug
 * needs a USB cable, so this is the only diagnostic visible on a console. */
const char *mus_status(void);

/* Bytes buffered ahead, or -1 when music is not running. Falling toward zero
 * means the card cannot keep up with 48 kHz stereo. */
int mus_ring_used(void);

#endif /* MUS_N64_H */
