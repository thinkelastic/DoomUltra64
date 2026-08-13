/*
 * d_rumble -- Rumble Pak feedback.
 *
 * Output-only: reads the simulation, drives a peripheral. It cannot move a
 * pixel or desync a demo, the two gates everything else here answers to.
 */
#ifndef D_RUMBLE_H
#define D_RUMBLE_H

/* Feed an impulse in roughly [0,1]: a solid hit ~0.5, a close blast ~1.
 * Impulses stack and clamp; the motor runs them down over ~180 ms. */
void D_RumbleAdd(float amount);

/* Hold the motor at a floor while something is running -- a chainsaw in
 * the hands, say. Call it every tic with the level wanted (0 to stop);
 * it fades by itself in a few frames if the caller stops calling, so a
 * pause or a level end cannot leave the motor stuck on. */
void D_RumbleSustain(float level);

/* Once per rendered frame: decay the level, dither it into the motor. */
void D_RumbleFrame(void);

#endif /* D_RUMBLE_H */
