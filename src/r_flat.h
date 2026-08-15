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

/* Sprites stand ON flats and must win that contact. They used to buy that
 * with a near constant of their own, two tenths ahead of the flats -- and
 * every tenth of it was also margin over the WALLS, because the flats
 * already lead them. That shape is the problem, not the size.
 *
 * A NEAR offset separates two surfaces by dNEAR/d, so the zone in which a
 * sprite wrongly beats a wall is a fixed FRACTION of depth --
 * (R_SPR_Z_NEAR/R_FLAT_NEAR - 1), an eighth at +0.2 -- and an eighth of
 * the depth GROWS in world units with distance: 37 units at 300, 125 at
 * 1000. A monster standing that far behind a wall drew straight through
 * it. Shrinking the constant only slides between 12.5% and the flats' own
 * 7.5%, and the bottom of that range clips the feet off everything.
 *
 * So the sprite is pulled toward the camera by a constant number of WORLD
 * UNITS instead, and shares the flats' curve. The zone where it beats a
 * wall becomes those few units at EVERY range, and the two artifacts
 * separate cleanly because they live at opposite ends of it:
 *
 *   near offset   separation ~ 1/d    strongest far away
 *   world pull    separation ~ 1/d^2  strongest up close
 *
 * Clipped feet are only visible up close, where a sprite is big; a sprite
 * punching through a wall is worst far away, where the old shape spent
 * its strength. The pull is strongest exactly where it is needed and
 * fades exactly where it stops mattering. Eight units holds more than an
 * LSB of this z buffer out to ~1000, by which point a sprite is ten
 * pixels tall and a tied pixel at its feet cannot be seen.
 *
 * The pull only ever makes a sprite win, so its failure mode is bounded:
 * a thing standing within R_SPR_PULL units BEHIND a wall shows through
 * it, and at eight units it is touching the wall anyway. */
/* One tenth ahead of the flats, NOT level with them.
 *
 * Dropping to the flats' own curve and relying on the world pull alone was
 * wrong at range, and barrel explosions are what showed it: the pull
 * separates as 1/d^2 while a near offset separates as 1/d, so past a few
 * hundred units the pull is worth less than a z LSB and a big sprite
 * standing on a floor simply lost to it and vanished. Clipped feet at that
 * distance are invisible, which is the case the pull was reasoned about;
 * a whole explosion disappearing is not.
 *
 * So the two are combined rather than traded: a tenth of near offset to
 * hold the ordering at any depth, and the world pull to do the work up
 * close where the old +0.2 bought its through-wall zone. That puts sprites
 * 10% of depth ahead of walls instead of 12.5%, and the pull covers the
 * near field the reduction gives up. */
#ifndef R_SPRZ
#define R_SPRZ (R_FLATZ + 1)          /* tenths, relative to the flats */
#endif
#define R_SPR_Z_NEAR  ((float)R_SPRZ * 0.1f)

/* ZERO by default, and the reasoning is worth keeping because the lever
 * was added to solve a problem the tenth above solves better.
 *
 * The pull's whole job was to beat the floor at the sprite's feet. The
 * tenth of near offset already does that at every depth -- 0.1/d is three
 * z LSBs of separation even a thousand units out -- so a pull on top of it
 * wins a contest already won, while adding its own world units to the zone
 * where a sprite shows through a wall. It made sprites visible through
 * walls for nothing.
 *
 * Kept as a lever because it is the right tool if contact ties ever
 * reappear up close, where its 1/d^2 separation is strongest. */
#ifndef R_SPRPULL
#define R_SPRPULL 0                   /* whole world units toward the eye */
#endif
#define R_SPR_PULL ((float)R_SPRPULL)

/* How fast the margin above decays back toward the flats' own constant, in
 * world units -- see SPRFADE in the Makefile for the table and for why the
 * decay stops at the flats rather than at the walls. The margin is what
 * keeps a floor from clipping the feet off what stands on it, and it is
 * only visible up close; the same tenths sit ahead of the WALLS at every
 * depth, where they buy a distant thing drawing through one. Fading the
 * first away with distance keeps the near contact and drops most of the
 * far cost. 0 restores the constant margin. */
#ifndef R_SPRFADE
#define R_SPRFADE 256
#endif

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
