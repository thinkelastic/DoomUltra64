/*
 * r_sky -- the sky backdrop. See r_sky.c for why it is screen space rather
 * than geometry.
 */
#ifndef R_SKY_H
#define R_SKY_H

#include <stdbool.h>

#include "dt64.h"
#include "r_wall.h"

void r_sky_set(dt64_tex_t *tex);   /* NULL disables the sky entirely */
bool r_sky_present(void);
void r_sky_draw(const r_camera_t *cam);

/* Visible-sky column span, accumulated during the BSP walk and consumed by
 * r_sky_draw (which draws only those columns) and by the frame loop (which
 * colour-clears only what the sky will not cover). */
void r_sky_span_reset(void);
/* `above_eye` is whether EVERY sky surface in this opening sits above the
 * camera. It decides the vertical clamp in r_sky_draw: a sky ceiling above
 * the eye projects to rows strictly above the horizon at every depth, so the
 * backdrop's wrapped repeats below it cannot show and need not be drawn. One
 * opening at or below eye level (a courtyard seen from a high ledge) makes
 * that false for the whole frame and the clamp is abandoned. */
void r_sky_span_add(int x0, int x1, bool above_eye);  /* inclusive columns */
bool r_sky_would_draw(void);             /* sky set AND span non-empty */
void r_sky_span(int *x0, int *x1);

#endif /* R_SKY_H */
