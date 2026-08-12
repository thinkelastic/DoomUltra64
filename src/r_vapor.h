/* Vapor over emissive liquids: a green haze hugging the nukage, a smoke
 * pall over lava.
 *
 * One translucent fan per glowing pool, floated a few units above the
 * liquid, sampling a boot-generated tiling noise texture whose UVs drift
 * with leveltime. Drawn after every opaque pass with the z-buffer probed
 * but not written, so world geometry still hides it and overlapping hazes
 * simply blend. The noise is amorphous, which is what makes this cheap:
 * the perspective-warp banding the floors need exists to keep straight
 * texel rows straight, and a cloud has none, so a pool is one fan however
 * deep it runs.
 *
 * VAPOR=0 removes the pass entirely.
 */
#ifndef R_VAPOR_H
#define R_VAPOR_H

#include "r_ssdata.h"
#include "r_wall.h"

#ifndef R_VAPOR
#define R_VAPOR 0
#endif

#if R_VAPOR

void r_vapor_init(void);     /* bake the noise texture; once at boot */
void r_vapor_begin(void);    /* reset the frame's queue */
/* Queue one pool's layer. `cls` is the D_GLOW_* family; unknown families
 * queue nothing. `light` is the sector light 0..255 for the fog falloff. */
void r_vapor_add(const r_polypt_t *pts, int npts, float liquid_h, int cls,
                 int light);
void r_vapor_flush(const r_camera_t *cam);

#else

static inline void r_vapor_init(void) { }
static inline void r_vapor_begin(void) { }
static inline void r_vapor_add(const r_polypt_t *pts, int npts,
                               float liquid_h, int cls, int light)
{ (void)pts; (void)npts; (void)liquid_h; (void)cls; (void)light; }
static inline void r_vapor_flush(const r_camera_t *cam) { (void)cam; }

#endif /* R_VAPOR */

#endif /* R_VAPOR_H */
