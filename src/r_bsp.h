/*
 * r_bsp -- walk Doom's BSP front-to-back and feed visible segs to the RDP
 * wall renderer. See r_bsp.c for why the traversal order matters.
 */
#ifndef R_BSP_H
#define R_BSP_H

#include <stdint.h>

#include "dt64.h"
#include "r_ssdata.h"
#include "r_wall.h"

/* Traverse `lev` from `cam` and emit walls, each with the texture its sidedef
 * names. */
/* Renders the level Doom's P_SetupLevel loaded. Geometry comes from Doom's
 * globals (sectors, segs, subsectors, nodes) and the per-cell polygons and
 * thing lists from r_ssdata, so nothing needs passing in but the camera. */
void r_render_level(const r_camera_t *cam);

int r_bsp_segs(void);   /* segs visited by the last traversal */
int r_bsp_walls(void);  /* segs that produced a wall */
int r_bsp_nodes(void);  /* nodes descended into */
int r_bsp_culled(void); /* subtrees rejected by the frustum test */
int r_bsp_flatcull(void); /* subsectors whose flats were fully occluded */
void r_bsp_dump(void);  /* one-shot breakdown of where segs went */


#endif /* R_BSP_H */
