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
void r_sprite_add(const r_camera_t *cam, const r_thing_t *t, float shade);
void r_sprite_flush(void);

int  r_sprite_count(void);
int  r_sprite_uploads(void);
int  r_sprite_dropped(void);

#endif /* R_SPRITE_H */
