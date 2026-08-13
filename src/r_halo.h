/*
 * r_halo -- light you can see IN THE AIR, rather than only on surfaces.
 *
 * Two effects, one primitive. Doom's renderer lights surfaces; nothing in
 * it draws the medium between them. Both of these do, and both are the
 * same thing on this hardware: a soft translucent billboard, z-tested
 * against the world so geometry occludes it, never z-written so they
 * cannot carve holes in each other.
 *
 * HALOS wrap the dynamic lights -- a fireball glows in the air around
 * itself, not just on the wall it passes. Position, radius and colour
 * come straight from the light registry, so there is nothing to queue.
 *
 * SHAFTS fall from sky openings. The BSP walk offers every sky-ceilinged
 * subsector it draws; small enclosed wells become shafts and wide-open
 * sky is rejected, because a light shaft only reads as one where the sky
 * is a hole in a roof rather than the roof's absence.
 *
 * The camera never pitches, so a vertical billboard facing the viewer is
 * exact rather than an approximation -- the same property that makes
 * Doom's sprites and this port's reflections work. Horizontal slices,
 * the other classic way to fake a volume, would be nearly edge-on at
 * Doom's eye level and invisible; these are not.
 */
#ifndef R_HALO_H
#define R_HALO_H

#include "r_ssdata.h"
#include "r_wall.h"

#ifndef R_HALO
#define R_HALO 0
#endif

#if R_HALO

/* Bake the two soft textures. Once, at boot. */
void r_halo_init(void);

/* Drop last frame's shafts. Once per frame, before the walk. */
void r_halo_begin(void);

/* Offer a sky-ceilinged subsector. Rejected unless it is a well: small
 * enough in plan that light falling through it reads as a shaft. */
void r_shaft_add(const r_polypt_t *pts, int npts,
                 float floor_h, float ceil_h, int lightlevel);

/* Draw the shafts, then a halo for every live dynamic light. After the
 * world and the sprites; before the vapor, which is thicker and belongs
 * in front. */
void r_halo_flush(const r_camera_t *cam);

#else

static inline void r_halo_init(void) {}
static inline void r_halo_begin(void) {}
static inline void r_shaft_add(const r_polypt_t *pts, int npts,
                               float floor_h, float ceil_h, int lightlevel)
{ (void)pts; (void)npts; (void)floor_h; (void)ceil_h; (void)lightlevel; }
static inline void r_halo_flush(const r_camera_t *cam) { (void)cam; }

#endif /* R_HALO */

#endif /* R_HALO_H */
