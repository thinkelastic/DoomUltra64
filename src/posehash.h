/* Frame fingerprinting for two-build A/B comparison.
 *
 * The problem this solves: proving that a renderer change did not alter a
 * single pixel, across a whole route rather than at one vantage point. The
 * existing tools cannot. run.sh captures one frame; demo.sh samples a route
 * from one build, spaced by a host busy loop, so two builds that render at
 * different speeds sample different route positions -- and a screen capture
 * additionally has to be cropped around the emulator's own VPS counter before
 * it can be compared at all.
 *
 * So the ROM fingerprints its own framebuffer and prints the hash. Nothing is
 * captured, nothing is cropped, and nothing depends on host timing: the hash
 * is keyed to the game tic, which is the same in both builds because the
 * simulation is deterministic. Comparing two runs is then a diff of two lists
 * of numbers.
 *
 * POSEHASH=n hashes one frame every n tics. It requires INTERP=0 -- with
 * interpolation on, the camera pose within a tic depends on when the frame
 * happened to render, so the same tic in two builds is not the same picture.
 * The Makefile enforces that rather than trusting it.
 *
 * This is a measurement build, not a fast one: reading the framebuffer back
 * costs milliseconds on the hashed frames. Never read frame times from it.
 */
#ifndef POSEHASH_H
#define POSEHASH_H

#include <libdragon.h>

/* Fingerprint this frame if its tic is due. Call after the frame is submitted;
 * it drains the RDP itself, since in the pipelined path the pixels are still
 * being written when the CPU gets here. */
void posehash_frame(const surface_t *fb);

#endif
