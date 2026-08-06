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
 * the level arena, which is stable, so nothing is copied. */
void r_flat_add(const r_polypt_t *pts, int npts, float height, float shade,
                dt64_tex_t *tex);

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
