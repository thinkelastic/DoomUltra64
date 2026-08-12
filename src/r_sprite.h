/*
 * r_sprite -- things drawn as screen-aligned billboards with alpha-tested
 * transparency. See r_sprite.c for why they are screen-aligned rather than
 * world-space quads.
 */
#ifndef R_SPRITE_H
#define R_SPRITE_H

#include "dt64.h"
#include "r_ssdata.h"
#include "r_wall.h"

void r_sprite_begin(void);
void r_psprite_draw(void);
/* sh[] is the three-channel shade (dynamic lights can colour an actor);
 * fog_ll is the sector light level for the distance falloff, or -1 for a
 * fullbright frame, which is exempt exactly as vanilla's colormap-0 was. */
/* alpha 1.0 joins the opaque cutout groups; lower values (smoke, puffs)
 * draw in the translucent tail: far-to-near, blended, z-tested, never
 * z-written. */
void r_sprite_add(const r_camera_t *cam, const r_thing_t *t,
                  const float sh[3], int fog_ll, float alpha);
void r_sprite_flush(void);

/* Liquid reflections: mirror images of things over glowing pools, drawn
 * after the sprite pass and masked to pool pixels by the z-buffer alone
 * (see r_reflect_flush in r_sprite.c). Host builds compile them out. */
#ifndef R_REFLECT
#define R_REFLECT 0
#endif
#if R_REFLECT
void r_reflect_add(const r_camera_t *cam, const r_thing_t *t,
                   const float sh[3], int fog_ll,
                   float plane_h, const float pool_rgb[3]);
/* The visible footprint of a reflective flat, straight from the BSP walk's
 * flat regions (the same polygons the vapor pass consumes). Ghosts are
 * clipped to these in screen space, which is what confines a mirror image
 * to its own surface: the z-test alone cannot tell a reflective floor
 * from a same-height neighbour, a farther pool, or untouched sky. */
void r_reflect_region(const r_polypt_t *pts, int npts, float h);
void r_reflect_flush(const r_camera_t *cam);
#endif

int  r_sprite_count(void);
int  r_sprite_uploads(void);
int  r_sprite_dropped(void);

#endif /* R_SPRITE_H */
