/*
 * r_env -- environment-map sheen on polished metal walls.
 *
 * Replaces the door mirror, which reflected actual things across the door's
 * plane and never worked: its ghost-quad clip returned the empty set at poses
 * where it plainly should not. A true mirror also cannot show the room behind
 * the viewer without walking the BSP a second time from a mirrored camera,
 * which is a whole extra world render on a frame that is already CPU-bound.
 *
 * An environment map buys the same read -- metal that catches and moves the
 * room's light -- for a quad per door.
 */
#ifndef R_ENV_H
#define R_ENV_H

#include "r_wall.h"

#ifndef R_ENVMAP
#define R_ENVMAP 0
#endif

#if R_ENVMAP
void r_env_begin(void);
void r_env_wall_add(const r_wall_t *w);
void r_env_flush(const r_camera_t *cam);
#else
static inline void r_env_begin(void) {}
static inline void r_env_wall_add(const r_wall_t *w) { (void)w; }
static inline void r_env_flush(const r_camera_t *cam) { (void)cam; }
#endif

#endif /* R_ENV_H */
