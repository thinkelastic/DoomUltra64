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
void r_sky_span_add(int x0, int x1);     /* inclusive screen columns */
bool r_sky_would_draw(void);             /* sky set AND span non-empty */
void r_sky_span(int *x0, int *x1);

#endif /* R_SKY_H */
