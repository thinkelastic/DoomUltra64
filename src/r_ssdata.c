/*
 * r_ssdata -- per-subsector renderer data, built from Doom's level structures.
 *
 * Doom stores no polygon for a subsector, only the segs along its walls. The
 * edges where the BSP split open space carry no seg at all, so a fan over the
 * segs leaves gaps -- which is why GL ports historically needed a nodebuilder
 * to supply the missing edges.
 *
 * Reconstructing them is cheap the other way round: start with the whole map
 * as one convex polygon and clip it by every partition line on the path from
 * the root down to the subsector, then by the subsector's own segs. A convex
 * region intersected with half-planes stays convex, so the result is exactly
 * the subsector and a triangle fan over it is valid.
 *
 * This lives beside Doom's structures rather than inside them. Doom's
 * subsector_t has three fields and no room for a polygon, a thing list or
 * cached bounds, and the port keeps the vendored sources unmodified so they
 * can be re-synced. A parallel array indexed by subsector number costs one
 * extra indirection and keeps that property.
 */
#include "r_ssdata.h"

#include "doom/doomtype.h"
#include "doom/doomdata.h"
#include "doom/p_local.h"
#include "doom/m_bbox.h"
#include "doom/m_fixed.h"
#include "doom/r_state.h"
#include "doom/z_zone.h"

#include <libdragon.h>
#include <math.h>

#define MAX_POLY_PTS 24

/* Ten points per subsector is generous: the clip is convex and bounded by the
 * partition count, and E1M1 averages four. */
#define PTS_PER_SS   10

#define MAX_THINGS 512

r_thing_t  *r_things;
int         r_numthings;

/* Filled during P_LoadThings, before the subsector data exists, so the things
 * are collected flat here and bucketed by cell afterwards. */
static r_thing_t thing_pool[MAX_THINGS];

void R_ThingAdd(float x, float y, float z, void *spr)
{
    if (r_numthings >= MAX_THINGS) return;
    thing_pool[r_numthings].x   = x;
    thing_pool[r_numthings].y   = y;
    thing_pool[r_numthings].z   = z;
    thing_pool[r_numthings].spr = spr;
    r_numthings++;
}

r_ssdata_t *r_ssdata;
r_polypt_t *r_polypts;
int         r_numpolypts;

r_nodef_t  *r_nodef;
uint32_t   *r_nodekids;
uint16_t   *r_ss_flatgate;
r_segf_t   *r_segf;

/* Set by the mark hooks (T_MovePlane, P_UnArchiveWorld, D_InterpEnd);
 * cleared by r_render_level's consume. Starts dirty so the first rendered
 * frame of a level always refreshes, whatever wrote the heights. */
int r_nodez_dirty = 1;

/* Incremental node-Z machinery. Each child code appears exactly once in
 * the BSP, so a subsector (or node) hangs from exactly one (node, slot)
 * pair -- the parent tables below are total functions, and a sector's
 * height change can be bubbled from its subsectors' leaf slots up to the
 * root, stopping the moment a level's stored range absorbs it. A moving
 * door touches a handful of slots instead of walking the whole tree at
 * frame rate. Encoding: (node << 1) | slot; 0xFFFF for none. */
static uint16_t *r_node_parent;   /* [numnodes] */
static uint16_t *r_ss_parent;     /* [numsubsectors] */
static uint16_t *r_sec_ss_first;  /* [numsectors + 1]: prefix sums */
static uint16_t *r_sec_ss_list;   /* [numsubsectors], grouped by sector */
static uint32_t *r_nodez_bits;    /* per-sector dirty bitset */
static int       r_nodez_nwords;
static int       r_nodez_all;     /* full-walk request */
#if D_NODEZ_CHECK
r_nodef_t       *r_nodez_check_buf;
#endif

void R_NodeZMarkSector(int secnum)
{
    if (!r_nodez_bits || (unsigned)secnum >= (unsigned)numsectors) {
        r_nodez_all = 1; r_nodez_dirty = 1;
        return;
    }
    r_nodez_bits[secnum >> 5] |= 1u << (secnum & 31);
    r_nodez_dirty = 1;
}

void R_NodeZMarkAll(void)
{
    r_nodez_all = 1;
    r_nodez_dirty = 1;
}

/* See r_ssdata.h. Runs at the end of P_LoadSegs, before any render. */
void R_BuildSegFloatMirror(void)
{
    r_segf = Z_Malloc(numsegs * sizeof *r_segf, PU_LEVEL, NULL);
    for (int i = 0; i < numsegs; i++) {
        const seg_t *sg = &segs[i];
        r_segf_t    *sf = &r_segf[i];

        if (!sg->linedef || !sg->sidedef || !sg->frontsector) {
            /* The walk's old null checks, folded into one sentinel. */
            sf->ax = sf->ay = sf->bx = sf->by = 0.0f;
            sf->len = 0.0f; sf->u0seg = 0.0f;
            sf->frontsec = sf->backsec = 0;
            sf->sideidx = 0; sf->lineidx = 0xFFFF;
            continue;
        }

        /* Exactly the walk's own conversions, evaluated once. */
        sf->ax = (float)sg->v1->x * (1.0f / FRACUNIT);
        sf->ay = (float)sg->v1->y * (1.0f / FRACUNIT);
        sf->bx = (float)sg->v2->x * (1.0f / FRACUNIT);
        sf->by = (float)sg->v2->y * (1.0f / FRACUNIT);

        /* The r_seglen expression, verbatim, so the value is bit-identical
         * to what the retired array held. */
        {
            const float dx = (float)((sg->v2->x - sg->v1->x) >> FRACBITS);
            const float dy = (float)((sg->v2->y - sg->v1->y) >> FRACBITS);
            sf->len = sqrtf(dx * dx + dy * dy);
        }

        /* Seg offset in texels. fixed_t: P_LoadSegs stores it <<FRACBITS. */
        sf->u0seg = (float)sg->offset * (1.0f / FRACUNIT);

        sf->frontsec = (uint16_t)(sg->frontsector - sectors);
        sf->backsec  = sg->backsector
                     ? (uint16_t)(sg->backsector - sectors) : 0xFFFF;
        sf->sideidx  = (uint16_t)(sg->sidedef - sides);
        sf->lineidx  = (uint16_t)(sg->linedef - lines);
    }
}

r_flatregion_t *r_flatregion;
r_polypt_t     *r_regionpts;
uint32_t        r_flat_stamp;

typedef struct { float x, y; } vec2_t;

/* Signed side of the directed line (px,py)+(dx,dy). Matches r_bsp's
 * point_on_side: positive is Doom's "front". */
static inline float side_of(float x, float y,
                            float px, float py, float dx, float dy)
{
    return dy * (x - px) - dx * (y - py);
}

/* Sutherland-Hodgman clip of a convex polygon to the front half-plane.
 * `keep_front` false clips to the back instead. */
static int clip_half(const vec2_t *in, int n, vec2_t *out,
                     float px, float py, float dx, float dy, boolean keep_front)
{
    if (n == 0) return 0;
    const float sgn = keep_front ? 1.0f : -1.0f;

    int m = 0;
    for (int i = 0; i < n; i++) {
        const vec2_t a = in[i], b = in[(i + 1) % n];
        const float sa = sgn * side_of(a.x, a.y, px, py, dx, dy);
        const float sb = sgn * side_of(b.x, b.y, px, py, dx, dy);

        if (sa >= 0.0f) { if (m < MAX_POLY_PTS) out[m++] = a; }
        if ((sa > 0.0f && sb < 0.0f) || (sa < 0.0f && sb > 0.0f)) {
            const float t = sa / (sa - sb);
            if (m < MAX_POLY_PTS) {
                out[m].x = a.x + (b.x - a.x) * t;
                out[m].y = a.y + (b.y - a.y) * t;
                m++;
            }
        }
    }
    return m;
}

static void store_poly(int ssnum, vec2_t *poly, int n, int *ptcursor)
{
    r_ssdata_t *sd = &r_ssdata[ssnum];
    sd->numpts = 0;
    if (n < 3) return;                     /* degenerate: nothing to draw */

    if (*ptcursor + n > r_numpolypts) {        /* out of room */
        extern int r_drop_pool;
        r_drop_pool++;
        return;
    }

    sd->firstpt = (uint16_t)*ptcursor;
    sd->numpts  = (uint16_t)n;

    /* Nudge the polygon outward from its own centre.
     *
     * Neighbouring BSP cells tile the floor exactly, but a vertex of one can
     * land partway along another's edge -- a T-junction, which the rasteriser
     * does not fill identically on both sides and which shows as a hairline
     * between floor or ceiling patches. A fraction of a unit of overlap closes
     * it. Cells at different heights overlap by the same negligible amount,
     * and the depth buffer settles which is in front.
     *
     * THAT LAST SENTENCE IS ONLY TRUE AT DIFFERENT HEIGHTS. Two COPLANAR
     * cells -- a floor meeting a neighbouring floor at the same height, in
     * a different sector with a different flat or light level -- emit
     * exactly the same depth over the whole overlap band: for a horizontal
     * plane z is affine in screen y and independent of screen x, so the
     * band is a perfect tie and the winner is whichever the texture
     * bucketing happened to draw first. Its texture and shade then cover
     * up to ~1.5 world units of its neighbour, which is 3.5 px at depth 48
     * and 1 px at 162 -- a few stray coloured pixels in a line along the
     * seam, and the reason a clear-colour probe finds nothing: an overlap
     * never leaves a pixel uncovered. The IWADs are full of these: 10-21%
     * of two-sided lines join coplanar sectors with different art. */
#ifndef R_FLATNUDGE
#define R_FLATNUDGE 75          /* hundredths of a world unit */
#endif
/* Divided, NOT multiplied by 0.01f: that constant is not representable in
 * binary, so 75 * 0.01f is 0.749999983 and every vertex moved in its last
 * bits -- 1890 pixels off the reference for a lever that was supposed to
 * be neutral at its default. 75/100.0f is exactly 0.75. */
#define FLAT_NUDGE ((float)R_FLATNUDGE / 100.0f)
    float cxp = 0.0f, cyp = 0.0f;
    for (int i = 0; i < n; i++) { cxp += poly[i].x; cyp += poly[i].y; }
    cxp /= (float)n; cyp /= (float)n;

    float xl = 1e9f, xh = -1e9f, yl = 1e9f, yh = -1e9f;
    for (int i = 0; i < n; i++) {
        const float ox = poly[i].x - cxp, oy = poly[i].y - cyp;
        const float len = sqrtf(ox * ox + oy * oy);
        const float k = len > 1e-3f ? (1.0f + FLAT_NUDGE / len) : 1.0f;
        poly[i].x = cxp + ox * k;
        poly[i].y = cyp + oy * k;

        r_polypts[*ptcursor].x = poly[i].x;
        r_polypts[*ptcursor].y = poly[i].y;
        if (poly[i].x < xl) xl = poly[i].x;
        if (poly[i].x > xh) xh = poly[i].x;
        if (poly[i].y < yl) yl = poly[i].y;
        if (poly[i].y > yh) yh = poly[i].y;
        (*ptcursor)++;
    }

    /* Cached so the renderer can reject an off-screen cell with four
     * transforms instead of clipping every edge. Rounded OUTWARD: a plain
     * int cast truncates toward zero, shrinking the box by up to a unit --
     * harmless when these only fed a coarse frustum test, but they now feed
     * the occlusion span, and near the camera one world unit projects to
     * tens of pixels. A shrunken span let range_covered cull flats whose
     * true edge columns were still open, which showed as floor and ceiling
     * gaps opening at the screen border. */
    sd->bx0 = (int16_t)floorf(xl); sd->bx1 = (int16_t)ceilf(xh);
    sd->by0 = (int16_t)floorf(yl); sd->by1 = (int16_t)ceilf(yh);
}

static void build_polys(int num, vec2_t *poly, int n, int *cursor)
{
    if (num & NF_SUBSECTOR) {
        const int ssnum = num & ~NF_SUBSECTOR;
        if (ssnum >= numsubsectors) return;
        const subsector_t *ss = &subsectors[ssnum];

        /* Finally trim by the subsector's own walls. Doom's seg_t holds vertex
         * pointers where the old loader held indices, so no bounds check is
         * needed here -- P_LoadSegs already resolved them. */
        vec2_t a[MAX_POLY_PTS], b[MAX_POLY_PTS];
        int cn = n;
        for (int i = 0; i < cn; i++) a[i] = poly[i];

        for (int i = 0; i < ss->numlines && cn >= 3; i++) {
            const seg_t *sg = &segs[ss->firstline + i];

            const float x1 = (float)(sg->v1->x >> FRACBITS);
            const float y1 = (float)(sg->v1->y >> FRACBITS);
            const float x2 = (float)(sg->v2->x >> FRACBITS);
            const float y2 = (float)(sg->v2->y >> FRACBITS);

            cn = clip_half(a, cn, b, x1, y1, x2 - x1, y2 - y1, true);
            for (int k = 0; k < cn; k++) a[k] = b[k];
        }

        store_poly(ssnum, a, cn, cursor);
        return;
    }

    if (num >= numnodes) return;
    const node_t *nd = &nodes[num];
    const float px = (float)(nd->x >> FRACBITS), py = (float)(nd->y >> FRACBITS);
    const float dx = (float)(nd->dx >> FRACBITS), dy = (float)(nd->dy >> FRACBITS);

    vec2_t child[MAX_POLY_PTS];
    int cn;

    cn = clip_half(poly, n, child, px, py, dx, dy, true);
    build_polys(nd->children[0], child, cn, cursor);

    cn = clip_half(poly, n, child, px, py, dx, dy, false);
    build_polys(nd->children[1], child, cn, cursor);
}

/* --- subtree height ranges -----------------------------------------------
 *
 * The vertical-occlusion pass culls a subtree when its projected height span
 * lies wholly outside the open window of its screen columns. That needs the
 * subtree's world height range, which Doom's nodes do not carry: computed
 * here from the sector heights, and refreshed when doors and lifts move
 * them. A sky ceiling makes a subtree unbounded above -- the backdrop drawn
 * behind it must never be culled by a window's top edge. */
extern int skyflatnum;

/* Subtree height range AND full geometric extent.
 *
 * Doom's node bounding boxes enclose the subtree's SEGS only. This renderer
 * draws whole subsector polygons, and the polygon edges cut by BSP
 * partitions carry no segs -- so a polygon routinely extends beyond its
 * subtree's seg box. Culling by the seg box alone threw away visible floors
 * and ceilings (a degenerate one-seg child culled a whole ceiling wedge).
 * The cull boxes are therefore widened here to the union of the seg box and
 * every polygon in the subtree. */
static void node_geo_range(int num, float *lo, float *hi,
                           float *xl, float *yl, float *xh, float *yh)
{
    if (num & NF_SUBSECTOR) {
        const int ssn = num & ~NF_SUBSECTOR;
        const subsector_t *ss = &subsectors[ssn];
        const sector_t *sec = ss->sector;

        *xl = *yl = 1e9f; *xh = *yh = -1e9f;
        if (r_ssdata && r_ssdata[ssn].numpts >= 3) {
            *xl = (float)r_ssdata[ssn].bx0; *yl = (float)r_ssdata[ssn].by0;
            *xh = (float)r_ssdata[ssn].bx1; *yh = (float)r_ssdata[ssn].by1;
        }
        /* Union the segs too: a cell with a degenerate polygon still has
         * walls to draw. */
        for (int i = 0; i < ss->numlines; i++) {
            const seg_t *sg = &segs[ss->firstline + i];
            const float ax = (float)sg->v1->x * (1.0f / FRACUNIT);
            const float ay = (float)sg->v1->y * (1.0f / FRACUNIT);
            const float bx = (float)sg->v2->x * (1.0f / FRACUNIT);
            const float by = (float)sg->v2->y * (1.0f / FRACUNIT);
            if (ax < *xl) *xl = ax;
            if (ax > *xh) *xh = ax;
            if (bx < *xl) *xl = bx;
            if (bx > *xh) *xh = bx;
            if (ay < *yl) *yl = ay;
            if (ay > *yh) *yh = ay;
            if (by < *yl) *yl = by;
            if (by > *yh) *yh = by;
        }

        if (!sec) { *lo = 1e9f; *hi = -1e9f; return; }
        *lo = (float)sec->floorheight * (1.0f / FRACUNIT);
        *hi = (sec->ceilingpic == skyflatnum)
            ? 1e6f
            : (float)sec->ceilingheight * (1.0f / FRACUNIT);
        return;
    }

    const node_t *nd = &nodes[num];
    r_nodef_t *nf = &r_nodef[num];
    for (int ch = 0; ch < 2; ch++) {
        float cxl, cyl, cxh, cyh;
        node_geo_range(nd->children[ch], &nf->zlo[ch], &nf->zhi[ch],
                       &cxl, &cyl, &cxh, &cyh);
        /* Widen the seg-derived box; never shrink it (idempotent across
         * refreshes -- the xy extent is static, only heights move). */
        if (cxl < nf->xl[ch]) nf->xl[ch] = cxl;
        if (cyl < nf->yl[ch]) nf->yl[ch] = cyl;
        if (cxh > nf->xh[ch]) nf->xh[ch] = cxh;
        if (cyh > nf->yh[ch]) nf->yh[ch] = cyh;
    }
    float l0 = nf->zlo[0], l1 = nf->zlo[1];
    float h0 = nf->zhi[0], h1 = nf->zhi[1];
    *lo = l0 < l1 ? l0 : l1;
    *hi = h0 > h1 ? h0 : h1;

    *xl = nf->xl[0] < nf->xl[1] ? nf->xl[0] : nf->xl[1];
    *yl = nf->yl[0] < nf->yl[1] ? nf->yl[0] : nf->yl[1];
    *xh = nf->xh[0] > nf->xh[1] ? nf->xh[0] : nf->xh[1];
    *yh = nf->yh[0] > nf->yh[1] ? nf->yh[0] : nf->yh[1];
}

/* The refresh half of node_geo_range: heights only.
 *
 * Everything else that function does is dead on a refresh, and the comment on
 * the widening above says why -- "the xy extent is static, only heights move".
 * The seg loop reads vertex coordinates, the box read reads r_ssdata bounds,
 * and both are load-time constants; the widening that consumes them is a
 * monotone `if (smaller) assign`, so after the first pass it can never change
 * anything again. What remains is a no-op that walks the whole tree.
 *
 * It is not a cheap no-op. Per subsector it chases sg->v1 and sg->v2 into
 * vertexes[] -- scattered loads, two per seg -- then four fixed-to-float
 * conversions and eight compares. On E1M1 that is the difference between
 * touching ~63 KB and ~8 KB, and it runs at FULL FRAME RATE for the whole
 * second a door or lift is moving: D_InterpBegin rewrites sector heights every
 * frame while anything is interpolating, so r_bsp.c's height-sum test sees a
 * change on every one of them, not once per tic.
 */
static void node_z_range(int num, float *lo, float *hi)
{
    if (num & NF_SUBSECTOR) {
        const sector_t *sec = subsectors[num & ~NF_SUBSECTOR].sector;
        if (!sec) { *lo = 1e9f; *hi = -1e9f; return; }
        *lo = (float)sec->floorheight * (1.0f / FRACUNIT);
        *hi = (sec->ceilingpic == skyflatnum)
            ? 1e6f
            : (float)sec->ceilingheight * (1.0f / FRACUNIT);
        return;
    }

    const node_t *nd = &nodes[num];
    r_nodef_t *nf = &r_nodef[num];
    node_z_range(nd->children[0], &nf->zlo[0], &nf->zhi[0]);
    node_z_range(nd->children[1], &nf->zlo[1], &nf->zhi[1]);

    const float l0 = nf->zlo[0], l1 = nf->zlo[1];
    const float h0 = nf->zhi[0], h1 = nf->zhi[1];
    *lo = l0 < l1 ? l0 : l1;
    *hi = h0 > h1 ? h0 : h1;
}

/* Load time: the full pass, which is what establishes the xy cull boxes. */
void R_BuildNodeGeo(void)
{
    if (!r_nodef || numnodes <= 0) return;
    float lo, hi, xl, yl, xh, yh;
    node_geo_range(numnodes - 1, &lo, &hi, &xl, &yl, &xh, &yh);
}

void R_RefreshNodeZ(void)
{
    if (!r_nodef || numnodes <= 0) return;

#if D_ZREFRESH_CHECK
    /* Self-check: run the full pass into a scratch copy and prove the z-only
     * one produced identical bytes for every node. Temporary -- it costs more
     * than the work it is checking. Boot with a door and a lift in view. */
    {
        static r_nodef_t *scratch;
        if (!scratch) scratch = Z_Malloc(numnodes * sizeof *scratch, PU_LEVEL, NULL);
        node_z_range(numnodes - 1, &(float){0}, &(float){0});
        for (int i = 0; i < numnodes; i++) scratch[i] = r_nodef[i];

        float lo, hi, xl, yl, xh, yh;
        node_geo_range(numnodes - 1, &lo, &hi, &xl, &yl, &xh, &yh);

        for (int i = 0; i < numnodes; i++)
            for (int ch = 0; ch < 2; ch++)
                assertf(scratch[i].zlo[ch] == r_nodef[i].zlo[ch] &&
                        scratch[i].zhi[ch] == r_nodef[i].zhi[ch] &&
                        scratch[i].xl[ch]  == r_nodef[i].xl[ch]  &&
                        scratch[i].yl[ch]  == r_nodef[i].yl[ch]  &&
                        scratch[i].xh[ch]  == r_nodef[i].xh[ch]  &&
                        scratch[i].yh[ch]  == r_nodef[i].yh[ch],
                        "node %d child %d diverged on z-only refresh", i, ch);
        return;
    }
#endif

    float lo, hi;
    node_z_range(numnodes - 1, &lo, &hi);
}

/* Bubble one sector's height change up the tree. Leaf values use VERBATIM
 * node_z_range's expressions (same TU, same compiled code), so a level
 * whose stored range already equals the incoming one has absorbed the
 * change -- float equality here is bit-identity: the inputs are
 * (float)fixed products and the 1e6/1e9 sentinels, no NaNs, and fixed zero
 * converts to +0.0. Each of the sector's subsectors bubbles independently;
 * a node whose other child changes later is re-walked by that sector's own
 * bubble, so order between dirty sectors cannot matter. */
static void nodez_refresh_sector(int secnum)
{
    const sector_t *sec = &sectors[secnum];
    const float lo = (float)sec->floorheight * (1.0f / FRACUNIT);
    const float hi = (sec->ceilingpic == skyflatnum)
                   ? 1e6f
                   : (float)sec->ceilingheight * (1.0f / FRACUNIT);

    const int e = r_sec_ss_first[secnum + 1];
    for (int k = r_sec_ss_first[secnum]; k < e; k++) {
        uint16_t at = r_ss_parent[r_sec_ss_list[k]];
        float nlo = lo, nhi = hi;
        while (at != 0xFFFF) {
            r_nodef_t *nf = &r_nodef[at >> 1];
            const int ch = at & 1;
            if (nf->zlo[ch] == nlo && nf->zhi[ch] == nhi)
                break;                   /* absorbed: ancestors already exact */
            nf->zlo[ch] = nlo;
            nf->zhi[ch] = nhi;
            /* Fold for the level above -- node_z_range's fold, verbatim. */
            const float l0 = nf->zlo[0], l1 = nf->zlo[1];
            const float h0 = nf->zhi[0], h1 = nf->zhi[1];
            nlo = l0 < l1 ? l0 : l1;
            nhi = h0 > h1 ? h0 : h1;
            at = r_node_parent[at >> 1];
        }
    }
}

/* The render's dirty-consume: full walk when everything is suspect
 * (savegame, level build), else one bubble per marked sector. Bits are
 * cleared only here, so marks accumulate across multi-tic bursts and
 * frames that skip rendering (automap, wipes) by construction. */
void R_NodeZConsume(void)
{
    if (!r_nodef || numnodes <= 0) return;

    if (r_nodez_all || !r_nodez_bits) {
        r_nodez_all = 0;
        if (r_nodez_bits)
            memset(r_nodez_bits, 0, (size_t)r_nodez_nwords * 4);
        R_RefreshNodeZ();
        return;
    }

    for (int w = 0; w < r_nodez_nwords; w++) {
        uint32_t bits = r_nodez_bits[w];
        if (!bits) continue;
        r_nodez_bits[w] = 0;
        const int base = w << 5;
        do {
            nodez_refresh_sector(base + __builtin_ctz(bits));
            bits &= bits - 1;
        } while (bits);
    }

#if D_NODEZ_CHECK
    /* Prove the incremental result equals the full z-only walk, every
     * frame: snapshot the slots, rerun node_z_range, compare bit-exactly.
     * The buffer is allocated eagerly at build (a lazy static would dangle
     * across level frees). */
    {
        extern r_nodef_t *r_nodez_check_buf;
        for (int i = 0; i < numnodes; i++) r_nodez_check_buf[i] = r_nodef[i];
        float lo, hi;
        node_z_range(numnodes - 1, &lo, &hi);
        for (int i = 0; i < numnodes; i++)
            for (int ch = 0; ch < 2; ch++)
                assertf(r_nodez_check_buf[i].zlo[ch] == r_nodef[i].zlo[ch] &&
                        r_nodez_check_buf[i].zhi[ch] == r_nodef[i].zhi[ch],
                        "nodez: node %d ch %d have %f/%f want %f/%f",
                        i, ch,
                        r_nodez_check_buf[i].zlo[ch],
                        r_nodez_check_buf[i].zhi[ch],
                        r_nodef[i].zlo[ch], r_nodef[i].zhi[ch]);
    }
#endif
}

/* --- merged flat regions -------------------------------------------------
 *
 * The BSP fragments every sector into convex cells; a big rectangular room
 * can be a dozen subsectors, each of which was a separate flat job through
 * the transform/clip/band pipeline every frame. Adjacent cells of one sector
 * whose union is still convex can be one polygon instead -- the geometry is
 * identical, just fewer, larger fans. Merging happens here, once, at load.
 *
 * Two polygons merge when they share a full edge (matched with tolerance:
 * store_poly nudged every vertex outward by up to ~0.75 units to close
 * T-junctions, so copies of one edge in two cells differ slightly) and the
 * spliced ring stays convex within a small angular tolerance. */

static inline float pdist2(r_polypt_t a, r_polypt_t b)
{
    const float dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

#define MERGE_EPS2 (2.0f * 2.0f)

/* Signed-area sign gives the ring's winding; convexity means every corner
 * turns the same way, within a tolerance for the nudge distortion. */
static boolean region_convex(const r_polypt_t *p, int n)
{
    float area = 0.0f;
    for (int i = 0; i < n; i++) {
        const r_polypt_t a = p[i], b = p[(i + 1) % n];
        area += a.x * b.y - b.x * a.y;
    }
    const float sign = area >= 0.0f ? 1.0f : -1.0f;

    for (int i = 0; i < n; i++) {
        const r_polypt_t a = p[i], b = p[(i + 1) % n], c = p[(i + 2) % n];
        const float e1x = b.x - a.x, e1y = b.y - a.y;
        const float e2x = c.x - b.x, e2y = c.y - b.y;
        const float cr = e1x * e2y - e1y * e2x;
        const float norm = sqrtf((e1x * e1x + e1y * e1y) *
                                 (e2x * e2x + e2y * e2y));
        /* Essentially-strict: only float noise is forgiven. The looser
         * -0.02 tolerance admitted slightly concave unions, and a concave
         * ring fanned from vertex 0 leaves a wedge-shaped hole -- seen as
         * jagged black notches in floors near the screen edge. */
        if (norm > 1e-6f && cr * sign < -1e-3f * norm) return false;
    }
    return true;
}

/* Splice B into A across one shared edge; returns the merged vertex count or
 * 0 when no shared edge exists or the union is not convex. */
static int region_try_merge(const r_polypt_t *A, int na,
                            const r_polypt_t *B, int nb,
                            r_polypt_t *out, int maxout)
{
    if (na + nb - 2 > maxout) return 0;

    for (int i = 0; i < na; i++) {
        const r_polypt_t a0 = A[i], a1 = A[(i + 1) % na];
        /* Only match edges meaningfully longer than the match tolerance:
         * BSP slivers produce sub-unit edges, and with a 2-unit endpoint
         * epsilon two DIFFERENT tiny edges can "match", splicing a
         * self-intersecting ring whose defects are too small for the
         * convexity test to see. */
        if (pdist2(a0, a1) < 4.0f * 4.0f) continue;
        for (int j = 0; j < nb; j++) {
            const r_polypt_t b0 = B[j], b1 = B[(j + 1) % nb];
            if (pdist2(b0, b1) < 4.0f * 4.0f) continue;
            if (pdist2(a0, b1) >= MERGE_EPS2 || pdist2(a1, b0) >= MERGE_EPS2)
                continue;

            /* Shared edge a0->a1 == b1->b0. Walk A up to a0, then B from
             * b1's successor around to b0's predecessor, then A from a1. */
            int m = 0;
            for (int k = 0; k <= i; k++)              out[m++] = A[k];
            for (int k = 2; k < nb; k++)              out[m++] = B[(j + k) % nb];
            for (int k = i + 1; k < na; k++)          out[m++] = A[k];

            if (!region_convex(out, m)) return 0;
            return m;
        }
    }
    return 0;
}

/* Build r_flatregion/r_regionpts from the per-subsector polygons and point
 * every cell at its region. */
static void build_flat_regions(void)
{
    typedef struct {
        r_polypt_t pts[MAX_POLY_PTS];
        int        n;
        int        sector;
        boolean    live;
        float      xl, xh, yl, yh;   /* for the cheap adjacency reject */
    } work_t;

    work_t *work  = malloc(sizeof(work_t) * numsubsectors);
    int    *owner = malloc(sizeof(int) * numsubsectors);   /* ss -> work idx */
    if (!work || !owner) {
        free(work); free(owner);
        work = NULL;
    }

    if (!work) {
        /* Out of scratch: fall back to one region per subsector. */
        r_flatregion = Z_Malloc(numsubsectors * sizeof *r_flatregion,
                                PU_LEVEL, NULL);
        r_regionpts  = r_polypts;
        for (int i = 0; i < numsubsectors; i++) {
            r_flatregion[i] = (r_flatregion_t){
                .firstpt = r_ssdata[i].firstpt, .numpts = r_ssdata[i].numpts,
                .bx0 = r_ssdata[i].bx0, .by0 = r_ssdata[i].by0,
                .bx1 = r_ssdata[i].bx1, .by1 = r_ssdata[i].by1,
                .stamp = 0,
            };
            r_ssdata[i].region = (uint16_t)i;
        }
        return;
    }

    for (int i = 0; i < numsubsectors; i++) {
        work[i].n      = r_ssdata[i].numpts;
        work[i].sector = subsectors[i].sector
                       ? (int)(subsectors[i].sector - sectors) : -1;
        work[i].live   = work[i].n >= 3 && work[i].sector >= 0;
        work[i].xl = 1e9f; work[i].xh = -1e9f;
        work[i].yl = 1e9f; work[i].yh = -1e9f;
        for (int k = 0; k < work[i].n; k++) {
            const r_polypt_t p = r_polypts[r_ssdata[i].firstpt + k];
            work[i].pts[k] = p;
            if (p.x < work[i].xl) work[i].xl = p.x;
            if (p.x > work[i].xh) work[i].xh = p.x;
            if (p.y < work[i].yl) work[i].yl = p.y;
            if (p.y > work[i].yh) work[i].yh = p.y;
        }
        owner[i] = i;
    }

    /* Greedy pairwise merging within each sector, iterated until dry. */
    boolean changed = true;
    int rounds = 0;
    while (changed && rounds++ < 8) {
        changed = false;
        for (int a = 0; a < numsubsectors; a++) {
            if (!work[a].live) continue;
            for (int b = a + 1; b < numsubsectors; b++) {
                if (!work[b].live || work[b].sector != work[a].sector) continue;

                /* Non-touching cells cannot share an edge; this reject is
                 * what keeps the pairwise pass affordable at console load. */
                if (work[a].xl > work[b].xh + 2.0f ||
                    work[b].xl > work[a].xh + 2.0f ||
                    work[a].yl > work[b].yh + 2.0f ||
                    work[b].yl > work[a].yh + 2.0f) continue;

                r_polypt_t merged[MAX_POLY_PTS];
                const int m = region_try_merge(work[a].pts, work[a].n,
                                               work[b].pts, work[b].n,
                                               merged, MAX_POLY_PTS);
                if (!m) continue;

                for (int k = 0; k < m; k++) work[a].pts[k] = merged[k];
                work[a].n    = m;
                work[a].xl = work[a].xl < work[b].xl ? work[a].xl : work[b].xl;
                work[a].xh = work[a].xh > work[b].xh ? work[a].xh : work[b].xh;
                work[a].yl = work[a].yl < work[b].yl ? work[a].yl : work[b].yl;
                work[a].yh = work[a].yh > work[b].yh ? work[a].yh : work[b].yh;
                work[b].live = false;
                for (int s = 0; s < numsubsectors; s++)
                    if (owner[s] == b) owner[s] = a;
                changed = true;
            }
        }
    }

    /* --- weld the T-junctions ------------------------------------------
     *
     * Neighbouring cells tile the plane exactly, but a corner of one can
     * land partway along another's EDGE. The two primitives then disagree
     * about that boundary by a fraction of a pixel and the rasteriser
     * leaves a hairline between them. store_poly's answer was to inflate
     * every polygon so neighbours overlap and the crack is buried -- which
     * works, and costs a band of one surface painted over its neighbour.
     * Where the two are coplanar with different art that band is the stray
     * coloured line along a floor seam: measured at ~30000 px a frame, in
     * two-pixel-tall runs.
     *
     * The textbook answer instead: give the edge the vertex it is missing.
     * A polygon that carries its neighbour's corner as a vertex of its own
     * has no T-junction there, both sides interpolate the same boundary,
     * and no overlap is needed to hide anything. It costs a few fan
     * triangles at load and nothing per frame.
     *
     * Bounded by MAX_POLY_PTS, which is also r_flat.c's FLAT_MAX_PTS: a
     * region pushed past it would be REJECTED outright by r_flat_add and
     * its floor would vanish, which is a far worse artifact than the one
     * being fixed. A region that cannot take another vertex simply keeps
     * its T-junction.
     *
     * OFF BY DEFAULT, because it does not fix what it was written for. It
     * welds 385 vertices on E1M1 -- verified on the console log, so the
     * pass genuinely fires -- and with FLATNUDGE=0 removing the overlap as
     * well, the reported seam is UNCHANGED. That eliminates the overlap
     * and the T-junction as its cause rather than leaving them untested.
     * Kept because removing a real T-junction is right on its own terms
     * and the machinery may be wanted later; not on, because it changes
     * every floor's geometry for no observed benefit. */
#if R_FLATWELD
    {
        const float EPS_PERP = 0.05f;   /* how near the edge counts as "on" */
        const float EPS_END  = 0.25f;   /* ignore what is already a corner  */
        int welded = 0;

        for (int a = 0; a < numsubsectors; a++) {
            if (!work[a].live) continue;
            for (int b = 0; b < numsubsectors; b++) {
                if (b == a || !work[b].live) continue;
                if (work[a].xl > work[b].xh + 2.0f ||
                    work[b].xl > work[a].xh + 2.0f ||
                    work[a].yl > work[b].yh + 2.0f ||
                    work[b].yl > work[a].yh + 2.0f) continue;

                for (int vb = 0; vb < work[b].n; vb++) {
                    if (work[a].n >= MAX_POLY_PTS) break;
                    const r_polypt_t v = work[b].pts[vb];

                    for (int e = 0; e < work[a].n; e++) {
                        const int f = e + 1 < work[a].n ? e + 1 : 0;
                        const r_polypt_t p = work[a].pts[e];
                        const r_polypt_t q = work[a].pts[f];
                        const float ex = q.x - p.x, ey = q.y - p.y;
                        const float len2 = ex * ex + ey * ey;
                        if (len2 < 1e-4f) continue;

                        /* Where along pq the point projects, and how far
                         * off the line it sits. */
                        const float t = ((v.x - p.x) * ex +
                                         (v.y - p.y) * ey) / len2;
                        const float len = sqrtf(len2);
                        if (t * len <= EPS_END) continue;
                        if ((1.0f - t) * len <= EPS_END) continue;
                        const float perp =
                            ((v.x - p.x) * ey - (v.y - p.y) * ex) / len;
                        if (perp > EPS_PERP || perp < -EPS_PERP) continue;

                        /* Insert between p and q, keeping the winding. */
                        for (int k = work[a].n; k > f; k--)
                            work[a].pts[k] = work[a].pts[k - 1];
                        work[a].pts[f] = v;
                        work[a].n++;
                        welded++;
                        break;              /* this vertex is placed */
                    }
                }
            }
        }
        debugf("ssdata: welded %d T-junction vertices\n", welded);
    }
#endif /* R_FLATWELD */

    /* Pack the survivors. */
    int nregions = 0, npts = 0;
    int *regidx = malloc(sizeof(int) * numsubsectors);
    for (int i = 0; i < numsubsectors; i++) {
        regidx[i] = -1;
        if (work[i].live) { regidx[i] = nregions++; npts += work[i].n; }
    }

    r_flatregion = Z_Malloc((nregions ? nregions : 1) * sizeof *r_flatregion,
                            PU_LEVEL, NULL);
    r_regionpts  = Z_Malloc((npts ? npts : 1) * sizeof *r_regionpts,
                            PU_LEVEL, NULL);

    int cursor = 0;
    for (int i = 0; i < numsubsectors; i++) {
        if (!work[i].live) continue;
        r_flatregion_t *rg = &r_flatregion[regidx[i]];
        rg->firstpt = (uint16_t)cursor;
        rg->numpts  = (uint16_t)work[i].n;
        rg->stamp   = 0;

        float xl = 1e9f, xh = -1e9f, yl = 1e9f, yh = -1e9f;
        for (int k = 0; k < work[i].n; k++) {
            r_regionpts[cursor++] = work[i].pts[k];
            if (work[i].pts[k].x < xl) xl = work[i].pts[k].x;
            if (work[i].pts[k].x > xh) xh = work[i].pts[k].x;
            if (work[i].pts[k].y < yl) yl = work[i].pts[k].y;
            if (work[i].pts[k].y > yh) yh = work[i].pts[k].y;
        }
        rg->bx0 = (int16_t)floorf(xl); rg->bx1 = (int16_t)ceilf(xh);
        rg->by0 = (int16_t)floorf(yl); rg->by1 = (int16_t)ceilf(yh);
    }

    for (int i = 0; i < numsubsectors; i++) {
        const int root = owner[i];
        r_ssdata[i].region =
            (uint16_t)(regidx[root] >= 0 ? regidx[root] : 0);
        /* Cells whose own polygon degenerated keep numpts 0 and are never
         * queued, so their region index is never read. */
    }

    /* The walk's flat gate: region index for cells that can draw, 0xFFFF
     * for the rest. Folds the numpts and sector checks the walk made per
     * visit into the load it was already paying. */
    r_ss_flatgate = Z_Malloc(numsubsectors * sizeof *r_ss_flatgate,
                             PU_LEVEL, NULL);
    for (int i = 0; i < numsubsectors; i++)
        r_ss_flatgate[i] =
            (r_ssdata[i].numpts >= 3 && subsectors[i].sector)
                ? r_ssdata[i].region : 0xFFFF;

    debugf("ssdata: %d flat regions from %d subsectors\n",
           nregions, numsubsectors);

    free(regidx);
    free(owner);
    free(work);
}

/* --- BSP point location ------------------------------------------------
 *
 * Taken verbatim from Doom's r_main.c rather than rewritten. Both are needed
 * by the ported game code -- P_SpawnMapThing now, and every mobj that moves
 * later -- but r_main.c itself is the software renderer and would drag column
 * and span rasterisation in with it.
 *
 * Verbatim matters here. R_PointOnSide's early-out cases and its sign-bit test
 * exist to avoid overflow in the fixed-point cross product, and a "cleaner"
 * float rewrite puts points on the wrong side of a partition at shallow
 * angles -- which files a thing under the wrong subsector and makes it vanish
 * behind a wall it should be standing in front of. */

int
R_PointOnSide
( fixed_t	x,
  fixed_t	y,
  node_t*	node )
{
    fixed_t	dx;
    fixed_t	dy;
    fixed_t	left;
    fixed_t	right;
	
    if (!node->dx)
    {
	if (x <= node->x)
	    return node->dy > 0;
	
	return node->dy < 0;
    }
    if (!node->dy)
    {
	if (y <= node->y)
	    return node->dx < 0;
	
	return node->dx > 0;
    }
	
    dx = (x - node->x);
    dy = (y - node->y);
	
    // Try to quickly decide by looking at sign bits.
    if ( (node->dy ^ node->dx ^ dx ^ dy)&0x80000000 )
    {
	if  ( (node->dy ^ dx) & 0x80000000 )
	{
	    // (left is negative)
	    return 1;
	}
	return 0;
    }

    left = FixedMul ( node->dy>>FRACBITS , dx );
    right = FixedMul ( dy , node->dx>>FRACBITS );
	
    if (right < left)
    {
	// front side
	return 0;
    }
    // back side
    return 1;			
}

subsector_t*
R_PointInSubsector
( fixed_t	x,
  fixed_t	y )
{
    node_t*	node;
    int		side;
    int		nodenum;

    // single subsector is a special case
    if (!numnodes)				
	return subsectors;
		
    nodenum = numnodes-1;

    while (! (nodenum & NF_SUBSECTOR) )
    {
	node = &nodes[nodenum];
	side = R_PointOnSide (x, y, node);
	nodenum = node->children[side];
    }
	
    return &subsectors[nodenum & ~NF_SUBSECTOR];
}

/* The view position R_PointToAngle measures from. Doom's renderer sets these
 * each frame in R_SetupFrame; here the AI is the only caller, and it sets them
 * through R_PointToAngle2 before every use. */
fixed_t viewx, viewy;

angle_t
R_PointToAngle
( fixed_t	x,
  fixed_t	y )
{	
    x -= viewx;
    y -= viewy;
    
    if ( (!x) && (!y) )
	return 0;

    if (x>= 0)
    {
	// x >=0
	if (y>= 0)
	{
	    // y>= 0

	    if (x>y)
	    {
		// octant 0
		return tantoangle[ SlopeDiv(y,x)];
	    }
	    else
	    {
		// octant 1
		return ANG90-1-tantoangle[ SlopeDiv(x,y)];
	    }
	}
	else
	{
	    // y<0
	    y = -y;

	    if (x>y)
	    {
		// octant 8
		return -tantoangle[SlopeDiv(y,x)];
	    }
	    else
	    {
		// octant 7
		return ANG270+tantoangle[ SlopeDiv(x,y)];
	    }
	}
    }
    else
    {
	// x<0
	x = -x;

	if (y>= 0)
	{
	    // y>= 0
	    if (x>y)
	    {
		// octant 3
		return ANG180-1-tantoangle[ SlopeDiv(y,x)];
	    }
	    else
	    {
		// octant 2
		return ANG90+ tantoangle[ SlopeDiv(x,y)];
	    }
	}
	else
	{
	    // y<0
	    y = -y;

	    if (x>y)
	    {
		// octant 4
		return ANG180+tantoangle[ SlopeDiv(y,x)];
	    }
	    else
	    {
		 // octant 5
		return ANG270-1-tantoangle[ SlopeDiv(x,y)];
	    }
	}
    }
    return 0;
}

/* Angle between two points, in Doom's binary angle measure.
 *
 * Verbatim from r_main.c for the same reason as R_PointOnSide: the AI uses it
 * to decide facing, and its table-driven form is exact where a float atan2
 * would drift. R_PointToAngle2 sets viewx/viewy and delegates, so both are
 * needed together. */
angle_t
R_PointToAngle2
( fixed_t	x1,
  fixed_t	y1,
  fixed_t	x2,
  fixed_t	y2 )
{	
    viewx = x1;
    viewy = y1;
    
    return R_PointToAngle (x2, y2);
}

void R_BuildSubsectorData(void)
{
    if (numsubsectors <= 0 || numnodes <= 0) {
        r_ssdata = NULL; r_polypts = NULL; r_numpolypts = 0;
        r_node_parent = r_ss_parent = NULL;
        r_sec_ss_first = r_sec_ss_list = NULL;
        r_nodez_bits = NULL; r_nodez_nwords = 0;
        r_nodez_all = 1; r_nodez_dirty = 1;
        return;
    }

    r_ssdata = Z_Malloc(numsubsectors * sizeof *r_ssdata, PU_LEVEL, NULL);
    for (int i = 0; i < numsubsectors; i++) r_ssdata[i].numpts = 0;

    /* Seg lengths live in r_segf now, built by R_BuildSegFloatMirror at the
     * end of P_LoadSegs. */

    /* Float mirrors of the nodes, for the traversal's point-side tests and
     * bounding-box culling. Fixed-to-float conversion happens here, once,
     * instead of at every node visit of every frame. The packed children
     * ride in their own dense array (not in r_nodef_t: 64 bytes is exactly
     * four D-cache lines and must stay so). */
    r_nodef = Z_Malloc(numnodes * sizeof *r_nodef, PU_LEVEL, NULL);
    r_nodekids = Z_Malloc(numnodes * sizeof *r_nodekids, PU_LEVEL, NULL);
    for (int i = 0; i < numnodes; i++) {
        const node_t *nd = &nodes[i];
        r_nodef_t *nf = &r_nodef[i];
        r_nodekids[i] = ((uint32_t)nd->children[0] << 16) | nd->children[1];
        nf->x  = (float)nd->x  * (1.0f / FRACUNIT);
        nf->y  = (float)nd->y  * (1.0f / FRACUNIT);
        nf->dx = (float)nd->dx * (1.0f / FRACUNIT);
        nf->dy = (float)nd->dy * (1.0f / FRACUNIT);
        for (int ch = 0; ch < 2; ch++) {
            nf->xl[ch] = (float)nd->bbox[ch][BOXLEFT]   * (1.0f / FRACUNIT);
            nf->xh[ch] = (float)nd->bbox[ch][BOXRIGHT]  * (1.0f / FRACUNIT);
            nf->yl[ch] = (float)nd->bbox[ch][BOXBOTTOM] * (1.0f / FRACUNIT);
            nf->yh[ch] = (float)nd->bbox[ch][BOXTOP]    * (1.0f / FRACUNIT);
        }
    }

    /* Parent attachments and per-sector subsector lists for the
     * incremental node-Z refresh. Child node references are 15-bit
     * (NF_SUBSECTOR claims the top bit), so (node<<1)|slot fits u16. */
    assertf(numnodes <= 0x7FFF, "node count %d overflows the parent encoding",
            numnodes);
    r_node_parent = Z_Malloc(numnodes * sizeof *r_node_parent, PU_LEVEL, NULL);
    r_ss_parent   = Z_Malloc(numsubsectors * sizeof *r_ss_parent, PU_LEVEL, NULL);
    for (int i = 0; i < numnodes; i++)      r_node_parent[i] = 0xFFFF;
    for (int i = 0; i < numsubsectors; i++) r_ss_parent[i]   = 0xFFFF;
    for (int n = 0; n < numnodes; n++)
        for (int ch = 0; ch < 2; ch++) {
            const int c = nodes[n].children[ch];
            if (c & NF_SUBSECTOR) {
                const int ssn = c & ~NF_SUBSECTOR;
                if (ssn < numsubsectors)
                    r_ss_parent[ssn] = (uint16_t)((n << 1) | ch);
            } else if (c < numnodes) {
                r_node_parent[c] = (uint16_t)((n << 1) | ch);
            }
        }

    /* sector -> subsectors, counting sort. Null-sector cells are skipped:
     * their leaf slots hold the constant empty range from the build and
     * never change. */
    r_sec_ss_first = Z_Malloc((numsectors + 1) * sizeof *r_sec_ss_first,
                              PU_LEVEL, NULL);
    r_sec_ss_list  = Z_Malloc(numsubsectors * sizeof *r_sec_ss_list,
                              PU_LEVEL, NULL);
    for (int i = 0; i <= numsectors; i++) r_sec_ss_first[i] = 0;
    for (int i = 0; i < numsubsectors; i++)
        if (subsectors[i].sector)
            r_sec_ss_first[(subsectors[i].sector - sectors) + 1]++;
    for (int i = 0; i < numsectors; i++)
        r_sec_ss_first[i + 1] = (uint16_t)(r_sec_ss_first[i + 1] +
                                           r_sec_ss_first[i]);
    {
        uint16_t *cursor = malloc(numsectors * sizeof *cursor);
        for (int i = 0; i < numsectors; i++) cursor[i] = r_sec_ss_first[i];
        for (int i = 0; i < numsubsectors; i++)
            if (subsectors[i].sector)
                r_sec_ss_list[cursor[subsectors[i].sector - sectors]++] =
                    (uint16_t)i;
        free(cursor);
    }

    /* Dirty set; Z_Malloc does not zero. Everything starts dirty-all so
     * the first frame refreshes whatever wrote the heights. */
    r_nodez_nwords = (numsectors + 31) >> 5;
    r_nodez_bits = Z_Malloc(r_nodez_nwords * sizeof *r_nodez_bits,
                            PU_LEVEL, NULL);
    memset(r_nodez_bits, 0, (size_t)r_nodez_nwords * 4);
    r_nodez_all = 1;
    r_nodez_dirty = 1;
#if D_NODEZ_CHECK
    r_nodez_check_buf = Z_Malloc(numnodes * sizeof *r_nodez_check_buf,
                                 PU_LEVEL, NULL);
#endif

    r_numpolypts = numsubsectors * PTS_PER_SS;
    r_polypts = Z_Malloc(r_numpolypts * sizeof *r_polypts, PU_LEVEL, NULL);

    /* Start from the level's own bounds, not the coordinate system's.
     *
     * Either encloses the map, but the tight box matters downstream: cells on
     * the edge inherit whatever the starting polygon had, and a corner 32768
     * units out is a vertex the renderer transforms every frame. Those are
     * exactly the magnitudes that overflow the RDP's s11.2 screen coordinates
     * and cost precision in the perspective divide. 64 units of padding keeps
     * the boundary clear of real geometry. */
    float xl = 1e9f, xh = -1e9f, yl = 1e9f, yh = -1e9f;
    for (int i = 0; i < numvertexes; i++) {
        const float vx = (float)(vertexes[i].x >> FRACBITS);
        const float vy = (float)(vertexes[i].y >> FRACBITS);
        if (vx < xl) xl = vx;
        if (vx > xh) xh = vx;
        if (vy < yl) yl = vy;
        if (vy > yh) yh = vy;
    }
    xl -= 64.0f; yl -= 64.0f; xh += 64.0f; yh += 64.0f;

    vec2_t world[4] = { { xl, yl }, { xh, yl }, { xh, yh }, { xl, yh } };

    int cursor = 0;
    build_polys(numnodes - 1, world, 4, &cursor);

    /* Bucket things by the cell they stand in, so the renderer walks only the
     * things in subsectors it has already decided are visible -- sprites then
     * inherit the BSP and occlusion culling the geometry does. Sorted into one
     * contiguous array rather than per-cell lists: 512 things is a single pass
     * and keeps the traversal reading forwards. */
    if (r_numthings > 0) {
        r_things = Z_Malloc(r_numthings * sizeof *r_things, PU_LEVEL, NULL);

        for (int i = 0; i < numsubsectors; i++) {
            r_ssdata[i].firstthing = 0;
            r_ssdata[i].numthings  = 0;
        }

        int out = 0;
        for (int ssn = 0; ssn < numsubsectors; ssn++) {
            r_ssdata[ssn].firstthing = (uint16_t)out;
            for (int t = 0; t < r_numthings; t++) {
                const subsector_t *ss = R_PointInSubsector(
                    (fixed_t)(thing_pool[t].x * FRACUNIT),
                    (fixed_t)(thing_pool[t].y * FRACUNIT));
                if (ss - subsectors != ssn) continue;
                r_things[out++] = thing_pool[t];
                r_ssdata[ssn].numthings++;
            }
        }
    }

    build_flat_regions();
    R_BuildNodeGeo();   /* full pass: establishes the xy cull boxes */

    int built = 0;
    for (int i = 0; i < numsubsectors; i++) if (r_ssdata[i].numpts) built++;

    /* Cross-check against the loader already proven on hardware. A whole-level
     * point total that differs by one says a single cell clipped differently,
     * which is worth locating: an extra vertex on a shared edge is exactly the
     * shape of a T-junction. */
    {
        int p_level_ss_numpts(int i);
        int diffs = 0;
        for (int i = 0; i < numsubsectors && diffs < 6; i++) {
            const int old = p_level_ss_numpts(i);
            if (old >= 0 && old != r_ssdata[i].numpts) {
                debugf("ssdata: subsector %d has %d pts, old loader %d\n",
                       i, r_ssdata[i].numpts, old);
                diffs++;
            }
        }
        if (!diffs) debugf("ssdata: per-subsector counts identical\n");
    }

    debugf("ssdata: %d things placed in cells\n", r_numthings);
    debugf("ssdata: %d/%d subsector polygons, %d points (%u KB)\n",
           built, numsubsectors, cursor,
           (unsigned)((r_numpolypts * sizeof *r_polypts +
                       numsubsectors * sizeof *r_ssdata) / 1024));
}
