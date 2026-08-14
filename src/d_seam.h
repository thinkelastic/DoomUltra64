/*
 * d_seam -- the seam microscope: what did the RDP actually rasterise?
 *
 * Every instrument so far read pixels the console had finished with, and
 * every geometric theory built on top of them has died on the evidence
 * (see the running total in a0603c9). This one closes the loop at the
 * command level instead: it captures the REAL edge coefficients of every
 * triangle in a frame -- the words the RSP assembled, not the words we
 * think it assembled -- alongside the coordinates of the pixels nothing
 * drew, and ships both over USB. A host-side walker then replays the
 * hardware's span arithmetic over the dumped triangles and has to arrive
 * at the same holes; whichever two primitives bound a hole name the
 * mechanism, with no theory in between.
 *
 * SEAMPROBE builds only. The probe forces a magenta clear (unless
 * CLEARCOL overrides it), scans each finished frame for clear-coloured
 * pixels in the world viewport, and reports them continuously -- walk
 * until "seamhole" lines appear, then hold still. The first stable
 * sighting arms a one-shot dump of the following frame's triangle
 * stream, which is the same picture, because you are holding still.
 */
#ifndef D_SEAM_H
#define D_SEAM_H

#ifndef D_SEAMPROBE
#define D_SEAMPROBE 0
#endif

#if D_SEAMPROBE

#include <libdragon.h>

/* Install the RDP command hook. After rdpq_init. */
void d_seam_init(void);

/* Scan one finished frame. Requires the blocking detach path: the frame
 * must be complete, and the scan reads the buffer the CPU way (invalidate
 * first, exactly as posehash does). clear16 is the packed RGBA16 the
 * frame was cleared with -- pixels still wearing it are the holes. */
void d_seam_frame(const surface_t *fb, uint16_t clear16);

#else

static inline void d_seam_init(void) { }
/* No surface_t in sight when the probe is off; take no types from it. */
#define d_seam_frame(fb, clear16) ((void)0)

#endif /* D_SEAMPROBE */

#endif /* D_SEAM_H */
