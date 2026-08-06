/* Doom's screen melt, drawn by the RDP. See r_wipe.c. */
#ifndef R_WIPE_H
#define R_WIPE_H

#include <stdbool.h>
#include <libdragon.h>

/* Begin a melt from the frame in `from`, which must still hold the
 * outgoing picture (the previously shown framebuffer). */
void r_wipe_start(const surface_t *from);

bool r_wipe_active(void);
bool r_wipe_tick(void);      /* advance one tic; false once finished */
void r_wipe_draw(void);      /* draw the melt over the current frame */
void r_wipe_free(void);

#endif
