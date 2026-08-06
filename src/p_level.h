/*
 * p_level -- Doom level geometry loaded from a WAD in ROM.
 *
 * Structures deliberately mirror Doom's own (r_defs.h): same fields, same
 * meanings, fixed-point where Doom uses fixed-point. The renderer only needs
 * geometry today, but the game logic ported later expects exactly these, so
 * matching now avoids a translation layer forever.
 *
 * What is NOT copied from Doom is the loader itself. Chocolate Doom's
 * p_setup.c drags in the zone allocator, I_Error and the whole doomdef.h
 * world to fill structures we can populate directly from the lumps -- and it
 * assumes a little-endian host besides.
 */
#ifndef P_LEVEL_H
#define P_LEVEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dt64.h"

/* Doom's 16.16 fixed point. Map coordinates arrive as whole units and are
 * shifted up; keeping the same representation as Doom means the game logic
 * ported later needs no conversion. */
typedef int32_t fixed_t;
#define FRACBITS 16
#define FRACUNIT (1 << FRACBITS)

typedef struct {
    fixed_t x, y;
} vertex_t;

typedef struct {
    fixed_t floorheight, ceilingheight;
    int16_t floorpic, ceilingpic;      /* flat indices, resolved at load */
    uint8_t skyceiling;                /* ceiling is F_SKY1: draw nothing */
    int16_t lightlevel;
    int16_t special, tag;
} sector_t;

typedef struct {
    fixed_t  textureoffset, rowoffset;
    int16_t  toptexture, bottomtexture, midtexture;
    uint16_t sector;                   /* index into sectors */
} side_t;

typedef struct {
    uint16_t v1, v2;                   /* indices into vertexes */
    int16_t  flags, special, tag;
    uint16_t sidenum[2];               /* 0xFFFF when absent */
    /* Bounds in whole map units, precomputed: collision tests every line
     * against a moving box, and recomputing these was most of that cost. */
    int16_t  bx0, by0, bx1, by1;
} line_t;

typedef struct {
    uint16_t v1, v2;
    int16_t  angle;
    uint16_t linedef;
    int16_t  side;                     /* 0 = front, 1 = back */
    int16_t  offset;
} seg_t;

typedef struct {
    uint16_t numsegs;
    uint16_t firstseg;
    uint16_t firstpt;      /* index into level_t.polypts */
    uint16_t numpts;       /* 0 when the polygon degenerated */
    uint16_t sector;       /* resolved from the first seg's front sidedef */
    uint16_t firstthing;   /* things standing in this cell, sorted */
    uint16_t numthings;
    int16_t  bx0, by0, bx1, by1;   /* polygon bounds, for cheap frustum reject */
} subsector_t;

/* A floor/ceiling vertex. Subsector polygons are convex and planar, so a
 * triangle fan over these is all the geometry a flat needs. */
typedef struct {
    float x, y;
} polypt_t;

typedef struct {
    fixed_t  x, y, dx, dy;             /* partition line */
    int16_t  bbox[2][4];
    uint16_t children[2];              /* high bit set => subsector index */
} node_t;

#define NF_SUBSECTOR 0x8000

/* Linedef flags. ML_BLOCKING stops the player at a two-sided line that would
 * otherwise be passable -- Doom uses it for railings and scenery edges. */
#define ML_BLOCKING      0x0001
#define ML_BLOCKMONSTERS 0x0002
#define ML_TWOSIDED      0x0004
#define ML_DONTPEGTOP    0x0008
#define ML_DONTPEGBOTTOM 0x0010

/* A thing placed on the map. Doom's full actor model lives in info.c; this is
 * only what the renderer needs to draw one. */
typedef struct {
    float       x, y, z;      /* z is the sector floor it stands on */
    int16_t     type;
    int16_t     angle;
    int16_t     ssector;      /* BSP cell it stands in, for culling */
    dt64_tex_t *spr;          /* resolved sprite frame, NULL if not drawn */
} mapthing_t;

typedef struct {
    char          name[9];

    vertex_t     *vertexes;    int numvertexes;
    sector_t     *sectors;     int numsectors;
    side_t       *sides;       int numsides;
    line_t       *lines;       int numlines;
    seg_t        *segs;        int numsegs;
    subsector_t  *subsectors;  int numsubsectors;
    node_t       *nodes;       int numnodes;
    polypt_t     *polypts;     int numpolypts;
    mapthing_t   *things;      int numthings;
    dt64_tex_t   *sky;         /* backdrop texture, NULL if the map has none */

    size_t        bytes;       /* total RAM occupied, for the budget report */
} level_t;

/* Load one map ("E1M1", "MAP01") from the mounted WAD into the level arena.
 * Returns false and logs the reason on failure. */
bool p_load_level(level_t *lev, const char *mapname);

/* Textures referenced by the current level, loaded into the texture arena at
 * load time. Sidedef fields hold indices into this table, or -1 for "no
 * texture" -- Doom's "-" marker, which is common and means draw nothing. */
dt64_tex_t *p_level_texture(int index);
int         p_level_numtextures(void);
int         p_level_resolve(const char *name, const char *prefix);

/* Index if the name is already in this level's table, else -1. Never loads:
 * use it to ask whether a level references something at all. */
int         p_level_lookup(const char *name, const char *prefix);

/* Texture animation: build this level's frame lists, and advance them one
 * tic. p_level_texture resolves through the result. */
void        p_level_anim_init(void);
void        p_level_anim_tick(void);

/* Which sector contains a point, found by descending the BSP. */
const sector_t *p_sector_at(const level_t *lev, float x, float y);
int             p_subsector_at(const level_t *lev, float x, float y);

/* Player 1 start from the THINGS lump (type 1), in map units plus a Doom
 * angle in degrees. Returns false if the map has no player start. */
bool p_player_start(const char *mapname, float *x, float *y, float *angle_deg);

#endif /* P_LEVEL_H */
