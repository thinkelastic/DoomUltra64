/*
 * The milestone-1 test room, shared by the ROM and the host harness so the
 * two cannot drift apart -- a regression the harness caught would otherwise
 * be meaningless if it were testing different geometry than the console runs.
 *
 * Proportions follow Doom's: rooms in E1M1 run roughly 512 units across with
 * 128-unit ceilings, and the player's eye sits 41 units off the floor.
 */
#ifndef SCENE_H
#define SCENE_H

#include "r_wall.h"

#define ROOM_R  256.0f    /* half-extent, so the room is 512 units across */
#define WALL_H  128.0f
#define EYE_Z   41.0f

#define SCENE_NUM_WALLS 4

/* Wound so every face points inward. Light levels differ per wall to show
 * sector lighting modulating the texture independently of distance fog. */
static inline void scene_build(r_wall_t out[SCENE_NUM_WALLS],
                               dt64_tex_t *startan,
                               dt64_tex_t *compute,
                               dt64_tex_t *brown)
{
    const float R = ROOM_R;
    /* ztex = WALL_H puts texture row 0 at the ceiling, which for a 128-unit
     * wall and a 128-tall texture is the 1:1 case the room was built around. */
    out[0] = (r_wall_t){ .x1 = -R, .y1 = -R, .x2 =  R, .y2 = -R,
                         .zbot = 0.0f, .ztop = WALL_H, .ztex = WALL_H, .len = 2.0f * R,
                         .tex = startan, .light = 255 };
    out[1] = (r_wall_t){ .x1 =  R, .y1 = -R, .x2 =  R, .y2 =  R,
                         .zbot = 0.0f, .ztop = WALL_H, .ztex = WALL_H, .len = 2.0f * R,
                         .tex = compute, .light = 255 };
    out[2] = (r_wall_t){ .x1 =  R, .y1 =  R, .x2 = -R, .y2 =  R,
                         .zbot = 0.0f, .ztop = WALL_H, .ztex = WALL_H, .len = 2.0f * R,
                         .tex = brown,   .light = 200 };
    out[3] = (r_wall_t){ .x1 = -R, .y1 =  R, .x2 = -R, .y2 = -R,
                         .zbot = 0.0f, .ztop = WALL_H, .ztex = WALL_H, .len = 2.0f * R,
                         .tex = startan, .light = 160 };
}

#endif /* SCENE_H */
