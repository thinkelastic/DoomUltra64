/*
 * r_ssdata -- renderer data hanging off Doom's subsectors.
 *
 * Doom's subsector_t carries a seg range and a sector pointer, and nothing
 * else. This port needs three more things per cell: the polygon covering it
 * (Doom has none -- see r_ssdata.c), the things standing in it, and cached
 * bounds for a cheap off-screen reject. They live here rather than in Doom's
 * structure so the vendored sources stay unmodified.
 *
 * Indexed by subsector number throughout: r_ssdata[n] describes subsectors[n].
 */
#ifndef R_SSDATA_H
#define R_SSDATA_H

#include <stdint.h>

/* Deliberately layout-compatible with the renderer's existing polypt_t so
 * r_flat_add and the wall path need no change when the loader is switched. */
typedef struct { float x, y; } r_polypt_t;

typedef struct {
    uint16_t firstpt;      /* index into r_polypts */
    uint16_t numpts;       /* 0 when the polygon degenerated */
    uint16_t firstthing;   /* things standing in this cell, sorted */
    uint16_t numthings;
    int16_t  bx0, by0, bx1, by1;   /* polygon bounds, for frustum reject */
    uint16_t region;       /* merged flat region this cell belongs to */
} r_ssdata_t;

/* A thing as the renderer needs it: a position and a sprite.
 *
 * Doom's mapthing_t is the on-disk record and its mobj_t is the live actor;
 * this is neither. It exists because the sprite path must not depend on the
 * old loader's structures while the two coexist, and because until the state
 * tables land there is no mobj to ask for a frame. Stage 2 replaces it. */
typedef struct {
    float x, y, z;
    void *spr;             /* dt64_tex_t*, opaque here to keep Doom's headers clean */
} r_thing_t;

extern r_thing_t *r_things;
extern int        r_numthings;

/* Called by P_SpawnMapThing as Doom's loader walks the THINGS lump. */
void R_ThingAdd(float x, float y, float z, void *spr);

extern r_ssdata_t *r_ssdata;
extern r_polypt_t *r_polypts;
extern int         r_numpolypts;

/* Per-seg world length, precomputed at load: seg geometry never moves, and
 * the traversal was paying a sqrtf per visible seg per frame for it. Indexed
 * by seg number, like Doom's own parallel arrays. */
extern float *r_seglen;

/* Merged flat region: adjacent same-sector subsector polygons whose union
 * stayed convex, unioned once at load. BSP splitting fragments each sector
 * into many convex cells, and every cell used to be its own flat job --
 * ~90 jobs a frame on E1M1, each re-running the whole transform/clip/band
 * pipeline that r_flat identifies as the largest single frame cost. Merging
 * halves the job count with identical pixels. `stamp` marks the frame the
 * region was last queued in, so whichever member cell the walk visits first
 * queues it and the rest skip. */
typedef struct {
    uint16_t firstpt;      /* into r_regionpts */
    uint16_t numpts;
    int16_t  bx0, by0, bx1, by1;
    uint32_t stamp;
} r_flatregion_t;

extern r_flatregion_t *r_flatregion;
extern r_polypt_t     *r_regionpts;
extern uint32_t        r_flat_stamp;   /* bumped once per frame by the walk */

/* Float mirror of the BSP nodes: partition line and per-child bounds, in the
 * renderer's units. Doom's node_t stores fixed_t, and the traversal was
 * converting four to eight of them to float at every node visit of every
 * frame -- multi-cycle cvt instructions on values that never change. */
typedef struct {
    float x, y, dx, dy;                /* partition line */
    float xl[2], yl[2], xh[2], yh[2];  /* child bounding boxes */
    float zlo[2], zhi[2];              /* child subtree height ranges */
} r_nodef_t;

extern r_nodef_t *r_nodef;

/* Recompute the per-child height ranges (doors and lifts move sectors).
 * Cheap -- one pass over the tree -- and called only on frames where some
 * sector height actually changed. */
void R_RefreshNodeZ(void);

/* Build from the level Doom's P_SetupLevel has just loaded. Allocates from the
 * zone with PU_LEVEL, so a level change frees it automatically. */
void R_BuildSubsectorData(void);

#endif /* R_SSDATA_H */
