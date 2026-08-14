/*
 * r_flat -- floors and ceilings. See r_flat.c for why they are depth-buffered
 * while walls are not.
 */
#ifndef R_FLAT_H
#define R_FLAT_H

#include "dt64.h"
#include "r_ssdata.h"
#include "r_wall.h"

/* Must match NEAR_PLANE in r_wall.c so walls and flats share a depth scale. */
#define R_FLAT_NEAR 4.0f

/* --- the depth-bias stack ------------------------------------------------
 *
 * Three kinds of surface meet at shared edges, and the z-buffer has to be
 * told which wins, because the RDP interpolates z LINEARLY in screen space
 * while the true curve is convex -- so two primitives approaching the same
 * edge from different vertex sets sag apart by a fraction of a pixel and
 * whichever sags lower steals the junction. Each gets a near constant, and
 * a LARGER constant means a SMALLER z, which means nearer and wins:
 *
 *   walls    R_FLAT_NEAR      the reference
 *   flats    R_FLAT_Z_NEAR    ahead of walls -- see FLATZ in the Makefile
 *   sprites  R_SPR_Z_NEAR     ahead of flats
 *
 * The order is forced by what each contact looks like when it goes wrong.
 * Flats must beat walls or a wall bites a notch out of a ceiling corner.
 * Sprites must beat flats or the floor clips the feet off anything standing
 * on it. Every one is expressed RELATIVE to the flats' constant, so moving
 * the FLATZ lever slides the whole stack and cannot invert it -- which is
 * exactly how the feet got clipped: flats were raised past the sprites'
 * fixed 4.0 and silently changed who won. */
#ifndef R_FLATZ
#define R_FLATZ 43
#endif
#define R_FLAT_Z_NEAR ((float)R_FLATZ * 0.1f)

/* Sprites stand ON flats and must win that contact -- but every unit of
 * margin here is also margin over the WALLS, because the flats already
 * lead them. That is unavoidable: flats ahead of walls plus sprites ahead
 * of flats puts sprites ahead of walls, and a sprite beats a wall whenever
 * it stands within (R_SPR_Z_NEAR/R_FLAT_NEAR - 1) of the wall's depth --
 * about a fifth of it at +0.5, which showed as a monster visible through a
 * wall it stood close behind.
 *
 * So the margin is the smallest that still holds the floor off. It can be
 * small: what it has to beat is the floor's z SAG, and a billboard has
 * none of its own -- all four corners sit at one depth, so the sprite's z
 * is flat while the floor beneath it bows toward the camera mid-span. */
#ifndef R_SPRZ
#define R_SPRZ (R_FLATZ + 2)          /* tenths, relative to the flats */
#endif
#define R_SPR_Z_NEAR  ((float)R_SPRZ * 0.1f)

/* A reflection sits just in front of the pool that shows it: the margin
 * the ghost had when flats were 3.5 and it was 4.5. */
#define R_REFL_Z_NEAR (R_FLAT_Z_NEAR + 1.0f)

/* Flats are batched exactly like walls, and for the same reason: a texture
 * upload costs far more than a triangle. Drawing them as the BSP walk finds
 * them means one upload per surface -- about 120 a frame against 29 for every
 * wall combined -- while grouping costs one per distinct flat on screen, which
 * is a handful.
 *
 * Reordering is safe here only because floors are depth-buffered. Walls needed
 * occlusion culling to make their batching sound; flats get it from the Z
 * buffer they already required. */
void r_flat_begin(void);

/* Queue one subsector surface. `pts` must outlive the frame -- it points into
 * the level arena, which is stable, so nothing is copied. `lightlevel` is the
 * sector light already clamped to 0..255: the byte is what the job record
 * stores (24 -> 16 bytes against an 8 KB D-cache), and the flush rebuilds the
 * exact float shade with the same * (1/255.0f) the callers used to apply. */
/* `lit` is nonzero when a dynamic light can reach the surface (the caller
 * tests its region box against the light spheres); zero lets the fan skip
 * every per-vertex light query with identical output. */
/* `reflective` pushes the surface a hair deeper in Z (floors that mirror:
 * pools, polished plate). The reflection pass biases its ghosts between
 * the true plane and that push, so the z-buffer itself becomes the
 * stencil: ghosts pass only where the reflective flat owns the pixel and
 * fail on same-height non-reflective neighbours. Zero for everything
 * else, and the push compiles to nothing for it. */
void r_flat_add(const r_polypt_t *pts, int npts, float height, int lightlevel,
                dt64_tex_t *tex, float glow, const float tint[3], int lit,
                int reflective);

/* Bind each distinct texture once and draw everything that uses it. */
void r_flat_flush(const r_camera_t *cam);

int  r_flat_count(void);
int  r_flat_uploads(void);
int  r_flat_calls(void);
int  r_flat_bands(void);
int  r_flat_emit_us(void);
int  r_flat_bind_us(void);
int  r_flat_dropped(void);

#endif /* R_FLAT_H */
