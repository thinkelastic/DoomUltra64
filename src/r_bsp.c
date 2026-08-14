/*
 * r_bsp -- BSP traversal feeding the RDP wall renderer.
 *
 * This is the Path B hinge: Doom's data structures and BSP are kept, its
 * software rasteriser is not. The tree is walked front-to-back from the
 * camera exactly as Doom does, but instead of handing segs to r_segs.c to
 * draw columns, each seg becomes a quad for the RDP.
 *
 * Front-to-back order matters for more than tradition. It is what will let
 * solid-segment clipping remove overdraw on the CPU later, and that in turn
 * is what makes the renderer's tile batching safe to reorder.
 */
#include "r_bsp.h"
#include "r_fastmath.h"

#include <math.h>
#include <string.h>

/* Geometry now comes from Doom's own loader. Its structures replace the ones
 * p_level.h defined, which is why that header is gone from here: the two sets
 * share type names and cannot meet in one translation unit. */
#include "doom/doomtype.h"
#include "doom/doomdata.h"
#include "doom/m_bbox.h"
#include "doom/p_local.h"
#include "doom/p_mobj.h"
#include "doom/info.h"
#include "doom/doomstat.h"
#include "doom/r_state.h"

/* Texture lookup still belongs to the asset side, which keeps its own table of
 * what the level references. Declared rather than included for the reason
 * above. */
dt64_tex_t *p_level_texture(int index);
void       *R_SpriteFrame(int sprite, int frame, int rot);
/* Emissive-flat lookups: inline, d_glow.h. */
#include "d_glow.h"
angle_t     R_PointToAngle2(fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2);

#include "r_flat.h"
#include "r_halo.h"

/* Polished metal surfaces mirror what stands in front of them. Set
 * alongside every texture choice rather than once per seg, because a
 * single sidedef can carry a shiny door as its middle and plain
 * concrete above it. */
#if R_DOORMIRROR
int D_TextureIsMirror(int picnum);
#define D_WALL_MIRROR(pic) \
    (cur_wall.mirror = (uint8_t)D_TextureIsMirror(pic))
#else
#define D_WALL_MIRROR(pic) ((void)0)
#endif
#include "r_light.h"
#include "r_pvs.h"
#include "r_sky.h"
#include "r_sprite.h"
#include "r_vapor.h"
#include "r_wall.h"

/* Doom fixed-point to float. Map units are the same units the camera uses, so
 * no scaling beyond the fractional shift. */
static inline float fx(fixed_t v) { return (float)v * (1.0f / (float)FRACUNIT); }

static const r_camera_t *cur_cam;

/* Bumped once a frame so each sector contributes its things exactly once. */
static int              r_thing_vc;

/* Why a floor or ceiling was not drawn. Every one of these is a legitimate
 * reject most of the time; they are counted so that a surface visibly missing
 * on screen can be attributed to one of them instead of guessed at. */
int r_drop_bbox, r_drop_span, r_drop_cover;
static int              stat_segs;
static int              stat_walls;
static int              stat_nodes;
static int              stat_culled;
static int              stat_flatcull;

static int stat_1s, stat_closed, stat_same, stat_window, stat_back, stat_span;
static int stat_em_solid, stat_em_pass;
#if D_HWSTAT
/* Only ever read to classify a counter, so it goes with them. */
static int in_pass;
#endif

/* The counters below sit in the walk's innermost branches -- roughly 1260
 * read-modify-writes a frame, each a gp-relative load, add and store injected
 * between a compare and a continue -- and their only consumer is r_bsp_dump(),
 * which main.c calls exactly once ever behind a static guard. The phase timers
 * a few lines up already take this treatment; this extends it to the counts.
 * The variables stay defined either way: the r_bsp_* accessors read them, so
 * they simply report zero in a build that did not collect them. */
#if D_HWSTAT
#define STAT(x) (x)
#else
#define STAT(x) ((void)0)
#endif

/* Half-tangent of the horizontal FOV, cached per frame. The basis itself
 * (r_view_cs/r_view_sn) is shared with r_wall via r_set_view, so the
 * traversal and the wall renderer agree bit-for-bit about camera space --
 * which is what lets screen_span's endpoint transform be handed to
 * r_draw_wall_clipped instead of recomputed there. */
#define view_cs r_view_cs
#define view_sn r_view_sn
static float view_k;

/* Must match NEAR_PLANE in r_wall.c: a subtree rejected here is never given
 * the chance to be clipped there. */
#define BSP_NEAR 4.0f

/* Linedef flags live in p_level.h. Doom's naming is confusing: "don't peg"
 * means the texture stays put when the surface moves, e.g. a door track that
 * must not slide with the door. */

/* Screen column span of an axis-aligned world-space box, conservatively.
 *
 * Doom's R_CheckBBox trick: from any viewpoint outside the box, the box's
 * angular extent is bounded by TWO silhouette corners picked by the
 * camera's position code -- so two transforms and at most two divides
 * replace projecting all four corners. Inside the box, or straddling the
 * near plane, the whole screen is reported instead of risking a wrong
 * reject (that is the subsector the camera stands in, every frame).
 *
 * Returns false only when the box is provably invisible: wholly behind the
 * camera, or projecting outside the screen. The caller pairs the span with
 * range_covered for occlusion culling of whole subtrees. */
static bool span_of_bbox(float xl, float yl, float xh, float yh,
                         int *out_lo, int *out_hi,
                         float *out_dmin, float *out_dmax)
{
    const float px = cur_cam->x, py = cur_cam->y;

    /* Depth is separable over the box axes, so the maximum over all four
     * corners is one add of two maxima: a box wholly behind the near plane
     * is rejected without projecting anything. The same products give the
     * minimum for free, so the box's camera-space depth range rides out to
     * the caller -- child_visible_inner was recomputing all four products
     * to derive exactly these two numbers. The inside/wrap paths report
     * dmin = BSP_NEAR, which is what the caller's clamp made of the true
     * value anyway (both cases put geometry at or behind the near plane). */
    const float txl = (xl - px) * view_cs, txh = (xh - px) * view_cs;
    const float tyl = (yl - py) * view_sn, tyh = (yh - py) * view_sn;
    const float dmax = fmaxf(txl, txh) + fmaxf(tyl, tyh);
    if (dmax < BSP_NEAR) return false;
    *out_dmax = dmax;
    *out_dmin = BSP_NEAR;

    const int cxc = px < xl ? 0 : (px > xh ? 2 : 1);
    const int cyc = py < yl ? 0 : (py > yh ? 2 : 1);

    if (cxc == 1 && cyc == 1) {                  /* camera inside the box */
        *out_lo = 0; *out_hi = SCREEN_W - 1;
        return true;
    }
    *out_dmin = fminf(txl, txh) + fminf(tyl, tyh);

    /* The two extremal corners per outside region: the near face's corners
     * from the edge-on regions, the flanking pair from the diagonals. */
    float ax, ay, bx, by;
    switch (cyc * 3 + cxc) {
    case 0:  ax = xl; ay = yh; bx = xh; by = yl; break;   /* below-left   */
    case 1:  ax = xl; ay = yl; bx = xh; by = yl; break;   /* below        */
    case 2:  ax = xl; ay = yl; bx = xh; by = yh; break;   /* below-right  */
    case 3:  ax = xl; ay = yl; bx = xl; by = yh; break;   /* left         */
    case 5:  ax = xh; ay = yl; bx = xh; by = yh; break;   /* right        */
    case 6:  ax = xl; ay = yl; bx = xh; by = yh; break;   /* above-left   */
    case 7:  ax = xl; ay = yh; bx = xh; by = yh; break;   /* above        */
    default: ax = xl; ay = yh; bx = xh; by = yl; break;   /* above-right  */
    }

    const float adx = ax - px, ady = ay - py;
    const float bdx = bx - px, bdy = by - py;
    const float da = adx * view_cs + ady * view_sn;
    const float db = bdx * view_cs + bdy * view_sn;

    /* A silhouette corner behind the near plane means the box wraps around
     * the viewer; the chord between the corners bounds nothing then. */
    if (da < BSP_NEAR || db < BSP_NEAR) {
        *out_lo = 0; *out_hi = SCREEN_W - 1;
        return true;
    }

    const float oa = adx * view_sn - ady * view_cs;
    const float ob = bdx * view_sn - bdy * view_cs;

    const float cx = SCREEN_W * 0.5f;
    const float xa = cx + oa * cur_cam->focal_x / da;
    const float xb = cx + ob * cur_cam->focal_x / db;

    /* Inline floor/ceil (see r_fastmath.h), converting both ends and taking
     * the integer min/max: floor is monotonic so min(floor(a),floor(b)) ==
     * floor(min(a,b)), likewise ceil with max -- same integers as the float
     * select this replaces, without the FPU compare-and-branch. |xa| here is
     * bounded by ~focal_x * 65k/BSP_NEAR < 2^23, inside trunc.w.s range. */
    const int fa = rf_floor_i32(xa), fb = rf_floor_i32(xb);
    const int ca = rf_ceil_i32(xa),  cb = rf_ceil_i32(xb);
    int lo = fa < fb ? fa : fb;
    int hi = (ca > cb ? ca : cb) - 1;
    if (lo < 0) lo = 0;
    if (hi > SCREEN_W - 1) hi = SCREEN_W - 1;
    if (lo > hi) return false;

    *out_lo = lo; *out_hi = hi;
    return true;
}

/* --- solid segment list --------------------------------------------------
 *
 * Doom's occlusion algorithm, and the reason front-to-back traversal matters.
 * `solidsegs` holds the screen column ranges already fully covered by nearer
 * one-sided walls. A seg arriving later is drawn only in the columns still
 * open, and a whole subtree can be discarded when its screen span is covered.
 *
 * This is what actually costs time: frustum culling cut the segs visited by
 * two thirds and barely moved the frame, because the expense was never the
 * tree walk -- it was rasterising walls hidden behind nearer ones. Removing
 * overdraw on the CPU is also what makes the renderer's tile batching sound,
 * since non-overlapping geometry can be reordered freely.
 *
 * Transcribed from R_ClipSolidWallSegment; the sentinel entries at both ends
 * are what let the walk run without bounds checks. */
typedef struct { int first, last; } cliprange_t;

/* A range needs at least two columns to be worth recording, so the list can
 * never exceed half the screen width, plus the two sentinels. */
#define MAXSEGS (SCREEN_W / 2 + 2)

static cliprange_t  solidsegs[MAXSEGS];
static cliprange_t *newend;

/* --- per-column vertical closure ------------------------------------------
 *
 * The full transcription of vanilla's ceilingclip/floorclip, which is the
 * half of Doom's occlusion this port lacked. Per column, [clip_top,
 * clip_bot) is the vertical range not yet guaranteed covered. A solid seg
 * closes its columns outright: the wall covers its own band, and the
 * subsector's ceiling and floor cover the rest -- their polygon reaches
 * exactly from the previous closure edge to the seg, which is the visplane
 * argument in polygon form. A pass seg narrows the window to its opening's
 * projected extent, because everything above and below the opening is
 * covered by its strips and flats the same way. Both edges of a wall are
 * straight lines in screen space, so the per-column step is a lerp.
 *
 * Everything culls against this: a subtree or flat region whose columns are
 * all closed is invisible, whatever its height; a subtree whose height
 * range projects outside the loosest open window of its span is hidden
 * behind a doorway's lintel or sill. Unlike the solid-segment list alone,
 * a column only ever counts as covered when drawn geometry actually closed
 * it -- which is what keeps sky from leaking through corridor ceilings. */
static uint8_t clip_top[SCREEN_W];
static uint8_t clip_bot[SCREEN_W];

/* Loosest-window summaries per 16-column block. clip_window is the hottest
 * read in the traversal -- every node child, flat region and emit range asks
 * for the loosest window over a span, up to 320 column reads per ask. The
 * blocks answer full 16-column stretches in one read each; only the partial
 * blocks at a span's ends still scan columns. The writers below refresh a
 * block exactly after touching it, so the summaries never go stale-tight;
 * a min/max over a block is by construction the block's loosest values, so
 * the block path returns byte-identical results to the column scan. */
#define CLIP_BLK_SHIFT 4
#define CLIP_NBLK ((SCREEN_W + 15) >> CLIP_BLK_SHIFT)
static uint8_t clip_blk_top[CLIP_NBLK];
static uint8_t clip_blk_bot[CLIP_NBLK];

static void clip_blk_refresh_one(int b)
{
    const int lo = b << CLIP_BLK_SHIFT;
    int hi = lo + (1 << CLIP_BLK_SHIFT);
    if (hi > SCREEN_W) hi = SCREEN_W;
    uint8_t t = 255, bo = 0;
    for (int x = lo; x < hi; x++) {
        if (clip_top[x] < t) t = clip_top[x];
        if (clip_bot[x] > bo) bo = clip_bot[x];
    }
    clip_blk_top[b] = t;
    clip_blk_bot[b] = bo;
}

static void clip_blk_refresh(int x1, int x2)
{
    for (int b = x1 >> CLIP_BLK_SHIFT; b <= x2 >> CLIP_BLK_SHIFT; b++)
        clip_blk_refresh_one(b);
}

/* Close columns outright (solid seg: wall plus own flats cover them). */
R_HOT static void clip_close(int x1, int x2)
{
#ifdef R_NOCLOSURE
    (void)x1; (void)x2; return;
#endif
    if (x1 < 0) x1 = 0;
    if (x2 > SCREEN_W - 1) x2 = SCREEN_W - 1;
    for (int x = x1; x <= x2; x++) {
        clip_top[x] = SCREEN_H;
        clip_bot[x] = 0;
    }
    /* Block summaries. A block wholly inside [x1,x2] is all (SCREEN_H, 0)
     * after the writes above, so its min and max ARE those constants --
     * rescanning sixteen bytes to compute them was most of this function's
     * cost. Only the at-most-two partial edge blocks still need the scan. */
    const int b2 = x2 >> CLIP_BLK_SHIFT;
    for (int b = x1 >> CLIP_BLK_SHIFT; b <= b2; b++) {
        const int lo = b << CLIP_BLK_SHIFT;
        if (lo >= x1 && lo + (1 << CLIP_BLK_SHIFT) - 1 <= x2) {
            clip_blk_top[b] = SCREEN_H;
            clip_blk_bot[b] = 0;
        } else
            clip_blk_refresh_one(b);
    }
}

/* Narrow columns to a pass seg's opening. Edge y values are given at the
 * seg's two projected ends and stepped linearly; rounded a pixel outward so
 * the window is never smaller than the real opening. */
static void clip_narrow(int x1, int x2,
                        float ya_top, float yb_top,
                        float ya_bot, float yb_bot,
                        float xs_a, float xs_b)
{
#ifdef R_NOCLOSURE
    (void)x1; (void)x2; (void)ya_top; (void)yb_top;
    (void)ya_bot; (void)yb_bot; (void)xs_a; (void)xs_b; return;
#endif
    if (x1 < 0) x1 = 0;
    if (x2 > SCREEN_W - 1) x2 = SCREEN_W - 1;

    const float span = xs_b - xs_a;
    const float inv  = fabsf(span) > 1e-3f ? 1.0f / span : 0.0f;

    /* Two exact cuts to the per-column work. (1) A 16-column block already
     * fully closed -- summary (SCREEN_H, 0), which forces every column to
     * exactly clip_top==SCREEN_H, clip_bot==0 -- admits no narrowing at
     * all, so the whole block is skipped; fx_ is recomputed from x, so the
     * jump is exact. (2) The trailing summary refresh covers only the
     * columns a store actually landed on; everywhere else the bytes are
     * unchanged and the summaries still equal their min/max by induction.
     * Both leave every array byte-identical to the full scan. */
    int mn = SCREEN_W, mx = -1;
    int x = x1;
#if R_FASTNARROW
    /* Strength-reduced edge stepping: constant deltas replace the per-
     * column lerp derivation. Reseeded with the exact lerp after every
     * block skip, so drift is bounded by one run's accumulation (~5e-3 px
     * over 320 columns) -- well inside the one-pixel outward rounding
     * below, but not bit-identical; see the Makefile note. */
    const float dyt_ = (yb_top - ya_top) * inv;
    const float dyb_ = (yb_bot - ya_bot) * inv;
    float fyt = 0.0f, fyb = 0.0f;
    bool  seeded = false;
#endif
    while (x <= x2) {
        if ((x & ((1 << CLIP_BLK_SHIFT) - 1)) == 0 &&
            x + (1 << CLIP_BLK_SHIFT) - 1 <= x2) {
            const int blk = x >> CLIP_BLK_SHIFT;
            if (clip_blk_top[blk] == SCREEN_H && clip_blk_bot[blk] == 0) {
                x += 1 << CLIP_BLK_SHIFT;
#if R_FASTNARROW
                seeded = false;
#endif
                continue;
            }
        }
#if R_FASTNARROW
        if (!seeded) {
            const float fx_ = ((float)x + 0.5f - xs_a) * inv;
            fyt = ya_top + (yb_top - ya_top) * fx_;
            fyb = ya_bot + (yb_bot - ya_bot) * fx_;
            seeded = true;
        }
        const float yt = fyt, yb = fyb;
        fyt += dyt_; fyb += dyb_;
#else
        const float fx_ = ((float)x + 0.5f - xs_a) * inv;
        const float yt  = ya_top + (yb_top - ya_top) * fx_;
        const float yb  = ya_bot + (yb_bot - ya_bot) * fx_;
#endif

        int t = (int)yt - 1;
        int b = (int)yb + 2;
        if (t < 0) t = 0;
        if (t > SCREEN_H) t = SCREEN_H;
        if (b < 0) b = 0;
        if (b > SCREEN_H) b = SCREEN_H;

        if (t > clip_top[x]) { clip_top[x] = (uint8_t)t; if (x < mn) mn = x; mx = x; }
        if (b < clip_bot[x]) { clip_bot[x] = (uint8_t)b; if (x < mn) mn = x; mx = x; }
        x++;
    }
    if (mx >= 0) clip_blk_refresh(mn, mx);
}

/* Loosest open window over an inclusive column range; top >= bot means
 * every column in the range is closed. */
R_HOT static void clip_window(int x1, int x2, float *top, float *bot)
{
    if (x1 < 0) x1 = 0;
    if (x2 > SCREEN_W - 1) x2 = SCREEN_W - 1;
    uint8_t t = 255, b = 0;

    int x = x1;
    while (x <= x2) {
        if ((x & ((1 << CLIP_BLK_SHIFT) - 1)) == 0 &&
            x + (1 << CLIP_BLK_SHIFT) - 1 <= x2) {
            const int blk = x >> CLIP_BLK_SHIFT;
            if (clip_blk_top[blk] < t) t = clip_blk_top[blk];
            if (clip_blk_bot[blk] > b) b = clip_blk_bot[blk];
            x += 1 << CLIP_BLK_SHIFT;
        } else {
            if (clip_top[x] < t) t = clip_top[x];
            if (clip_bot[x] > b) b = clip_bot[x];
            x++;
        }
    }
    *top = (float)t;
    *bot = (float)b;
}

/* Wall currently being clipped. Held by value rather than by pointer so it
 * cannot outlive the caller's stack frame. */
static r_wall_t cur_wall;

static void clip_reset(void)
{
    solidsegs[0].first = -0x7fffffff; solidsegs[0].last = -1;
    solidsegs[1].first = SCREEN_W;    solidsegs[1].last =  0x7fffffff;
    newend = solidsegs + 2;

    memset(clip_top, 0, sizeof clip_top);
    memset(clip_bot, SCREEN_H, sizeof clip_bot);
    memset(clip_blk_top, 0, sizeof clip_blk_top);
    memset(clip_blk_bot, SCREEN_H, sizeof clip_blk_bot);
}

/* Half a pixel of overlap on each side of a clipped span.
 *
 * The solid-segment list hands out adjacent, non-overlapping column ranges,
 * and each becomes a pair of clip planes. Two walls meeting at such a boundary
 * should project their clipped edges to the same X, but they are different
 * segments at different depths and the arithmetic gets there by different
 * routes -- so they can land a fraction of a pixel apart and leave a hairline
 * of background between them. Overlapping slightly closes the seam, and is
 * free of consequences now that a depth buffer decides what wins. */
#define SEAM_BLEED 1.0f

R_HOT static void emit_range(int x1, int x2)
{
    if (x2 < x1) return;
    STAT(in_pass ? stat_em_pass++ : stat_em_solid++);

    /* Clamp to the loosest open window over the range. Sound now that the
     * closure arrays only record what drawn geometry really covers: front-
     * to-back order means every narrowing came from a strictly nearer seg,
     * and this seg is only ever visible inside those openings. */
    float wtop, wbot;
    clip_window(x1, x2, &wtop, &wbot);
    if (wtop >= wbot) return;                /* every column already closed */

#if R_PVS == 2
    { extern int r_ss_emitted; r_ss_emitted = 1; }
#endif
    r_draw_wall_win(cur_cam, &cur_wall,
                    (float)x1 - SEAM_BLEED, (float)(x2 + 1) + SEAM_BLEED,
                    wtop, wbot);
    STAT(stat_walls++);
}

/* Draw the visible parts of [first,last] and mark the range as covered. */
R_HOT static void clip_solid(int first, int last)
{
#ifdef R_FLATDUMP
    if ((r_flat_stamp & 127) == 0 && r_flat_stamp > 0)
        debugf("cs: x=%d..%d zb=%d zt=%d tex=%d\n",
               first, last, (int)cur_wall.zbot, (int)cur_wall.ztop,
               cur_wall.tex != NULL);
#endif
    cliprange_t *next, *start = solidsegs;

    while (start->last < first - 1) start++;

    if (first < start->first) {
        if (last < start->first - 1) {
            /* Entirely visible: draw it and insert a new range. */
            emit_range(first, last);
            if (newend < solidsegs + MAXSEGS - 1) {
                next = newend++;
                while (next != start) { *next = *(next - 1); next--; }
                next->first = first;
                next->last  = last;
            }
            return;
        }
        emit_range(first, start->first - 1);
        start->first = first;
    }

    if (last <= start->last) return;             /* already fully covered */

    next = start;
    while (last >= (next + 1)->first - 1) {
        emit_range(next->last + 1, (next + 1)->first - 1);
        next++;
        if (last <= next->last) { start->last = next->last; goto crunch; }
    }
    emit_range(next->last + 1, last);
    start->last = last;

crunch:
    if (next == start) return;
    while (next++ != newend) *++start = *next;
    newend = start + 1;
}

/* clip_solid plus the vertical half: a solid seg's columns are fully closed
 * -- wall in its band, the marking subsector's ceiling and floor above and
 * below. Kept out of clip_solid itself so the drawing inside it still sees
 * the pre-closure window state. */
static void clip_solid_close(int first, int last)
{
    clip_solid(first, last);
    clip_close(first, last);
}

/* Mark a column range covered without drawing anything. Used where a surface
 * is opaque but has no texture assigned. */
static void mark_solid(int first, int last)
{
    /* Only .tex needs saving: NULL is what makes emit_range a no-op, and
     * nothing here touches the other fields. The whole-struct copy pair
     * this replaces was 2 x 76 bytes of stack traffic per textureless
     * surface. */
    dt64_tex_t *const saved_tex = cur_wall.tex;
    cur_wall.tex = NULL;
    clip_solid(first, last);
    clip_close(first, last);
    cur_wall.tex = saved_tex;
}

/* Draw the visible parts of [first,last] WITHOUT marking them covered.
 *
 * Used for two-sided lines. The upper and lower strips around a window are
 * solid surfaces, but the window itself is see-through, so the columns must
 * stay open for whatever lies beyond. Marking them here would erase the room
 * on the other side of every doorway.
 *
 * Transcribed from R_ClipPassWallSegment. */
R_HOT static void clip_pass(int first, int last)
{
    cliprange_t *start = solidsegs;
    STAT(in_pass = 1);

    while (start->last < first - 1) start++;

    if (first < start->first) {
        if (last < start->first - 1) { emit_range(first, last); STAT(in_pass = 0); return; }
        emit_range(first, start->first - 1);
    }

    if (last <= start->last) { STAT(in_pass = 0); return; }

    while (last >= (start + 1)->first - 1) {
        emit_range(start->last + 1, (start + 1)->first - 1);
        start++;
        if (last <= start->last) { STAT(in_pass = 0); return; }
    }
    emit_range(start->last + 1, last);
    STAT(in_pass = 0);
}

/* Is a screen column range entirely inside an already-covered range? */
static bool range_covered(int first, int last)
{
    const cliprange_t *start = solidsegs;
    while (start->last < first) start++;
    return first >= start->first && last <= start->last;
}

/* Which side of a node's partition line a point lies on: 0 = front, 1 = back,
 * matching Doom's R_PointOnSide, in floats rather than fixed point.
 *
 * The sense matters and is easy to get backwards. Doom computes
 * left = dy_node * dx, right = dy * dx_node and returns 0 when right < left,
 * i.e. front when (dy_node * dx - dx_node * dy) > 0. Inverting it walks the
 * tree back-to-front, which is invisible while overdraw hides it -- the last
 * wall drawn simply wins -- and then breaks completely once occlusion culling
 * is added, because distant walls claim the screen columns first and
 * everything nearer is clipped away. */
static int point_on_side(float x, float y, int nodenum)
{
    /* Reads the float mirror rather than converting four fixed_t per visit. */
    const r_nodef_t *nf = &r_nodef[nodenum];
    return (nf->dy * (x - nf->x) - nf->dx * (y - nf->y)) > 0.0f ? 0 : 1;
}

/* Narrow [lo,hi] against the linear constraint a + b*u >= 0. */
static inline bool clipr(float *lo, float *hi, float a, float b)
{
    if (fabsf(b) < 1e-9f) return a >= 0.0f;
    const float root = -a / b;
    if (b > 0.0f) { if (root > *lo) *lo = root; }
    else          { if (root < *hi) *hi = root; }
    return *lo < *hi;
}

/* Project a world-space segment to an inclusive screen column range.
 * Returns false when it falls outside the view entirely. On success, `xf`
 * receives the endpoints' camera-space transform {d1, o1, d2, o2} -- so the
 * wall renderer can start from it instead of redoing the rotation; it is
 * the identical computation, on the identical basis -- plus the clipped
 * ends' depths {da, db} and projected screen positions {xa, xb}, which the
 * per-column closure narrows opening edges with. */
static bool screen_span(float ax, float ay, float bx, float by,
                        int *x1, int *x2, float xf[8])
{
    const float adx = ax - cur_cam->x, ady = ay - cur_cam->y;
    const float bdx = bx - cur_cam->x, bdy = by - cur_cam->y;

    const float d1 = adx * view_cs + ady * view_sn;
    const float o1 = adx * view_sn - ady * view_cs;
    const float d2 = bdx * view_cs + bdy * view_sn;
    const float o2 = bdx * view_sn - bdy * view_cs;

    xf[0] = d1; xf[1] = o1; xf[2] = d2; xf[3] = o2;

    const float dd = d2 - d1, od = o2 - o1;
    float ua = 0.0f, ub = 1.0f;
    if (!clipr(&ua, &ub, d1 - BSP_NEAR, dd))                     return false;
    if (!clipr(&ua, &ub, o1 + d1 * view_k, od + dd * view_k))    return false;
    if (!clipr(&ua, &ub, d1 * view_k - o1, dd * view_k - od))    return false;

    const float da = d1 + dd * ua, oa = o1 + od * ua;
    const float db = d1 + dd * ub, ob = o1 + od * ub;
    if (da <= 0.0f || db <= 0.0f) return false;

    xf[4] = da; xf[5] = db;

    const float cx = SCREEN_W * 0.5f;
    const float xa = cx + oa * cur_cam->focal_x / da;
    const float xb = cx + ob * cur_cam->focal_x / db;

    xf[6] = xa; xf[7] = xb;

    /* Same inline floor/ceil + integer min/max as span_of_bbox; same
     * bounds argument (da,db >= BSP_NEAR keeps |xa|,|xb| far inside 2^23). */
    const int fa = rf_floor_i32(xa), fb = rf_floor_i32(xb);
    const int ca = rf_ceil_i32(xa),  cb = rf_ceil_i32(xb);
    int lo = fa < fb ? fa : fb;
    int hi = (ca > cb ? ca : cb) - 1;
    if (lo < 0) lo = 0;
    if (hi > SCREEN_W - 1) hi = SCREEN_W - 1;
    if (lo > hi) return false;

    *x1 = lo; *x2 = hi;
    return true;
}

/* Floor and ceiling for one subsector.
 *
 * Only surfaces the camera is actually on the visible side of are drawn: a
 * floor above eye level and a ceiling below it both face away, and Doom never
 * shows either. */
/* SHOW selects which subsystems draw, so a missing surface can be attributed
 * without guessing: 0 = everything, 1 = walls only, 2 = flats only.
 * Whatever is still missing with only one of them enabled is the culprit. */
#ifndef R_SHOW
#define R_SHOW 0
#endif

uint32_t r_ph_cull_us, r_ph_seg_us, r_ph_flat_us;
static bool child_visible_inner(int nodenum, int child);
static void render_flats_inner(const subsector_t *ss);

#if D_HWSTAT
static void render_flats(const subsector_t *ss)
{
    const uint32_t t_ = TICKS_READ();
    render_flats_inner(ss);
    r_ph_flat_us += TICKS_TO_US(TICKS_SINCE(t_));
}
#else
#define render_flats render_flats_inner
#endif

static void render_flats_inner(const subsector_t *ss)
{
#if R_SHOW == 1
    (void)ss; return;
#else
    /* One dense u16 answers "can this cell draw flats, and which region":
     * 0xFFFF folds the old numpts and sector checks (r_ssdata straddles
     * cache lines; this array packs eight cells per line in visit order). */
    const uint16_t rgi = r_ss_flatgate[ss - subsectors];
    if (rgi == 0xFFFF) return;

    /* Skip cells whose screen span is already covered by nearer solid walls.
     *
     * Walls get this from solid-segment clipping, but floors and ceilings were
     * drawn regardless and left to the depth buffer. That is expensive: the
     * RDP compares depth after texturing, so an occluded floor still costs a
     * texel fetch per pixel, and a subsector's ceiling alone can span the
     * screen. Reaching here before this cell's own walls are added means the
     * test sees exactly the geometry in front of it.
     *
     * The span comes from the cell's cached bounds via the two-corner
     * silhouette projection -- one call replaces the old frustum test plus
     * a full polygon span (which transformed every vertex and divided per
     * in-front point, twice over per cell per frame). Bounds are slightly
     * wider than the polygon, which only makes the cull conservative. */
    /* The cell's flats belong to a merged region; whichever member cell the
     * walk reaches first queues the whole region and stamps it, so the rest
     * skip both the culling arithmetic and the (duplicate) jobs. */
    r_flatregion_t *rg = &r_flatregion[rgi];
#ifdef R_FLATDUMP
    const int dbg_sec = (int)(ss->sector - sectors);
    const int dbg_ss  = (int)(ss - subsectors);
    #define FDUMP(what, a, b) do { if ((r_flat_stamp & 127) == 0 && r_flat_stamp > 0)         debugf("fd: ss=%d sec=%d rg=%d %s %d %d\n", dbg_ss, dbg_sec,                (int)rgi, what, a, b); } while (0)
#else
    #define FDUMP(what, a, b)
#endif
    if (rg->stamp == r_flat_stamp) { FDUMP("dup", 0, 0); return; }

    /* Stamp on REJECT too, not only on queue: a region rejected here is
     * rejected for every later member cell as well -- the span is pose-
     * constant within the frame, and coverage only ever grows during the
     * front-to-back walk -- so the duplicates would redo the projection and
     * the coverage scan to reach the same answer. */
    int lo, hi;
    float dmn_, dmx_;
    if (!span_of_bbox((float)rg->bx0, (float)rg->by0,
                      (float)rg->bx1, (float)rg->by1, &lo, &hi, &dmn_, &dmx_))
        { FDUMP("spanrej", 0, 0); r_drop_span++; rg->stamp = r_flat_stamp; return; }
    if (range_covered(lo, hi)) { FDUMP("covered", lo, hi); r_drop_cover++;
                                 stat_flatcull++; rg->stamp = r_flat_stamp; return; }

    FDUMP("queue", lo, hi);
    rg->stamp = r_flat_stamp;

    const sector_t   *sec = ss->sector;      /* a pointer in Doom's structures */

    /* A sky ceiling means these columns open onto the backdrop: record them
     * so the sky pass draws only where sky can actually show. */
    if (sec->ceilingpic == skyflatnum) {
#ifdef R_FLATDUMP
        if ((r_flat_stamp & 127) == 0 && r_flat_stamp > 0)
            debugf("sk: flat sec=%d lo=%d hi=%d\n",
                   (int)(sec - sectors), lo, hi);
#endif
        r_sky_span_add(lo, hi);
    }
    const r_polypt_t *pts = &r_regionpts[rg->firstpt];

#if R_HALO
    /* A sky ceiling over a SMALL enclosed plan is a light well, and light
     * falls through it. r_shaft_add applies that test and rejects open
     * sky, where a visible beam would look wrong. */
    { int D_SectorIsSkyWell(int secnum);
      if (sec->ceilingpic == skyflatnum &&
          D_SectorIsSkyWell((int)(sec - sectors)))
        r_shaft_add(pts, rg->numpts, fx(sec->floorheight),
                    fx(sec->ceilingheight),
                    sec->lightlevel < 0 ? 0 :
                    sec->lightlevel > 255 ? 255 : sec->lightlevel); }
#endif

    const int light = sec->lightlevel < 0 ? 0 :
                      sec->lightlevel > 255 ? 255 : sec->lightlevel;

    const float fh = fx(sec->floorheight);
    const float ch = fx(sec->ceilingheight);

#if R_PVS == 2
    { extern int r_ss_emitted; r_ss_emitted = 1; }
#endif
    /* Sludge, lava and blood are lit like any other floor by default, which
     * leaves a nukage pool in a dim room as dark as the concrete beside it.
     * The flat lights itself; the walls it laps against are handled where the
     * wall is built. */
#if D_DYNLIGHT
    /* Base light and emissive term stay separate all the way to the vertex:
     * only the second one is coloured. */
    float fglow = D_FlatGlow(sec->floorpic);
    float ftint[3];
    D_FlatGlowRGB(sec->floorpic, ftint);
#else
    const float fglow = 0.0f;
    static const float ftint[3] = { 1.0f, 1.0f, 1.0f };
#endif
    if (cur_cam->z > fh) {
        r_flat_add(pts, rg->numpts, fh, light, p_level_texture(sec->floorpic),
                   fglow, ftint,
                   /* Sphere-vs-box reach test per surface: a rejected light
                    * adds exactly zero at every vertex, so the fan skips
                    * its per-vertex queries with identical output. */
                   r_light_reaches_box((float)rg->bx0, (float)rg->by0,
                                       (float)rg->bx1, (float)rg->by1, fh),
                   R_REFLECT && D_FlatReflective(sec->floorpic));
#if R_REFLECT
        /* The mirror pass clips its ghosts to these footprints. Low
         * graphic detail queues none, which alone kills every ghost at
         * the flush -- the wall and thing gates just save the queueing. */
        { extern int detailLevel;
          if (!detailLevel && D_FlatReflective(sec->floorpic))
              r_reflect_region(pts, rg->numpts, fh); }
#endif
        /* Glowing liquids grow their vapor layer: haze over the sludge,
         * smoke over the lava. Same polygon, a few units up, translucent. */
        if (fglow > 0.0f)
            r_vapor_add(pts, rg->numpts, fh,
                        D_FlatGlowClass(sec->floorpic), light);
    }
    /* A sky ceiling is an absence, not a surface. Doom marks it by the flat
     * itself rather than a flag: F_SKY1 resolves to skyflatnum at load. */
    if (cur_cam->z < ch && sec->ceilingpic != skyflatnum) {
        const int clit =
            r_light_reaches_box((float)rg->bx0, (float)rg->by0,
                                (float)rg->bx1, (float)rg->by1, ch);
        /* A ceiling can be emissive too -- a lava ceiling is rare but legal. */
#if D_DYNLIGHT
        float ctint[3];
        D_FlatGlowRGB(sec->ceilingpic, ctint);
        r_flat_add(pts, rg->numpts, ch, light, p_level_texture(sec->ceilingpic),
                   D_FlatGlow(sec->ceilingpic), ctint, clit, 0);
#else
        r_flat_add(pts, rg->numpts, ch, light, p_level_texture(sec->ceilingpic),
                   0.0f, ftint, clit, 0);
#endif
    }
#endif
}

#if R_PVS == 2
/* Set whenever this subsector queues something the frame will actually
 * draw. Reaching a subsector is not the same as drawing from it -- its
 * flats can still be span-culled and its segs backface-culled -- and only
 * the ones that contribute have to be in the PVS. */
int r_ss_emitted;
#endif

R_HOT static void render_subsector(int num)
{
    if (num >= numsubsectors) return;
#if R_PVS == 2
    r_ss_emitted = 0;
#endif
    const subsector_t *ss = &subsectors[num];

    render_flats(ss);

    /* Things in this cell. Reached only when the cell is visible, so sprites
     * inherit the BSP and occlusion culling the geometry already does. */
    /* Things, from the thinker list rather than a load-time table.
     *
     * Doom links every actor into its *sector's* list, not its subsector's, so
     * a sector spanning several cells would submit its things once per cell.
     * validcount guards that -- the same mechanism, and the same reason, as
     * R_AddSprites. */
    if (ss->sector->validcount != r_thing_vc) {
        ss->sector->validcount = r_thing_vc;

        const int   lit_ll = ss->sector->lightlevel < 0 ? 0 :
                             ss->sector->lightlevel > 255 ? 255 :
                             ss->sector->lightlevel;
        const float lit = (float)lit_ll * (1.0f / 255.0f);

        /* Camera position in Doom's fixed point, for the rotation formula. */
        const fixed_t camfx = (fixed_t)(cur_cam->x * (float)FRACUNIT);
        const fixed_t camfy = (fixed_t)(cur_cam->y * (float)FRACUNIT);

        extern boolean r_interpolate;      /* d_bridge.c; decl r_main.h */
        extern fixed_t fractionaltic;

        for (const mobj_t *mo = ss->sector->thinglist; mo; mo = mo->snext) {
            /* Lerp between the previous and current tic exactly as the view
             * does, so things and camera stay in sync at sub-tic frames.
             * p_mobj.c snapshots old*; teleports snap them to the exit. */
            fixed_t mx = mo->x, my = mo->y, mz = mo->z;
            angle_t mang = mo->angle;
            if (r_interpolate) {
                mx = mo->oldx + FixedMul(mo->x - mo->oldx, fractionaltic);
                my = mo->oldy + FixedMul(mo->y - mo->oldy, fractionaltic);
                mz = mo->oldz + FixedMul(mo->z - mo->oldz, fractionaltic);
                const int32_t adiff = (int32_t)(mo->angle - mo->oldangle);
                mang = mo->oldangle
                     + (angle_t)(((int64_t)adiff * fractionaltic) >> FRACBITS);
            }
            const float tx = fx(mx), ty = fx(my);

            /* Which of the eight rotations faces the viewer: Doom's own
             * integer formula, bit-exact with the original boundaries. The
             * previous float form went through newlib's atan2f -- several
             * hundred cycles per visible thing per frame -- where Doom's
             * tantoangle table does it in tens, and the table code is already
             * vendored in r_ssdata.c. */
            const angle_t ang = R_PointToAngle2(camfx, camfy, mx, my);
            const int rot =
                (int)((angle_t)(ang - mang + (angle_t)(ANG45 / 2) * 9) >> 29);

            r_thing_t t;
            t.x   = tx;
            t.y   = ty;
            t.z   = fx(mz);
            t.spr = R_SpriteFrame(mo->sprite, mo->frame, rot);
            if (!t.spr) continue;

            /* Doom marks frames that ignore sector light -- the lit end of a
             * torch, a plasma bolt -- with a bit in the frame word. */
#if R_PVS == 2
            { extern int r_ss_emitted; r_ss_emitted = 1; }
#endif
            /* Dynamic lights reach actors too -- a fireball crossing a dark
             * room lights the imp that threw it, in its own colour (the
             * sprite vertex always carried three channels, so this is
             * free at the RSP). Queried at mid-height, like the light
             * sources themselves; the empty-list case is a load and a
             * branch. Fullbright frames skip both the query and the
             * distance falloff, matching vanilla's colormap-0. */
            float sh[3];
            if (mo->frame & FF_FULLBRIGHT) {
                sh[0] = sh[1] = sh[2] = 1.0f;
            } else {
                float add[3];
                r_light_at_rgb(tx, ty, fx(mz) + fx(mo->height) * 0.5f, add);
                sh[0] = lit + add[0]; if (sh[0] > 1.0f) sh[0] = 1.0f;
                sh[1] = lit + add[1]; if (sh[1] > 1.0f) sh[1] = 1.0f;
                sh[2] = lit + add[2]; if (sh[2] > 1.0f) sh[2] = 1.0f;
            }
#if R_DOORMIRROR
            /* Every thing the walk sees is a candidate for a door's
             * mirror -- including the player, whose own sprite is queued
             * each frame and only ever rejected for sitting at depth
             * zero. Reflected across a door two paces away it lands four
             * paces out and draws like anything else. */
            { void r_mirror_thing(float x, float y, float z, void *spr,
                                  const float sh[3], unsigned ang);
              r_mirror_thing(t.x, t.y, t.z, t.spr, sh, (unsigned)mang); }
#endif
            r_sprite_add(cur_cam, &t, sh,
                         (mo->frame & FF_FULLBRIGHT) ? -1 : lit_ll,
                         /* Things that are vapour or energy rather than
                          * objects read better as haze than as cardboard:
                          * smoke and bullet puffs, and the teleport and
                          * item-respawn fogs -- the green flash at both
                          * ends of a teleport is light, and you should be
                          * able to see the room through it. Routed to the
                          * sprite pass's translucent tail. */
                         (mo->type == MT_SMOKE || mo->type == MT_PUFF)
                             ? 0.6f
                         : (mo->type == MT_TFOG || mo->type == MT_IFOG)
                             ? 0.7f : 1.0f);

#if R_REFLECT
            /* Over a glowing liquid: queue the mirror image. No polygon is
             * needed -- the flush's z-plane masking clips the image to the
             * pool's visible pixels -- so this is just the plane height and
             * the pool's hue. Sector-keyed: a thing on the bank does not
             * reflect, one wading or flying low over the surface does. */
            {
                /* The menu's Graphic Detail toggle owns the mirrors at
                 * runtime: a no-op setting since the software rasteriser
                 * left, now High = reflections, Low = none. */
                extern int detailLevel;
                if (!detailLevel &&
                    D_FlatReflective(ss->sector->floorpic)) {
                    const float ph = fx(ss->sector->floorheight);
                    if (t.z - ph < 128.0f) {
                        float prgb[3];
                        D_FlatGlowRGB(ss->sector->floorpic, prgb);
                        r_reflect_add(cur_cam, &t, sh,
                                      (mo->frame & FF_FULLBRIGHT) ? -1 : lit_ll,
                                      ph, prgb);
                    }
                }
            }
#endif
        }
    }

#if D_HWSTAT
    const uint32_t t_seg_ = TICKS_READ();
#endif
    for (int i = 0; i < ss->numlines; i++) {
        /* Everything static about the seg comes from the 32-byte float
         * mirror (r_ssdata.h): endpoints, length, texel start, and the
         * index links. Doom's seg_t and vertex_t are not touched by the
         * walk at all any more; sector heights, sidedef offsets and line
         * flags stay live reads through the links because doors move,
         * switches retexture and scrollers scroll. */
        const r_segf_t *sf = &r_segf[ss->firstline + i];
        STAT(stat_segs++);

        if (sf->lineidx == 0xFFFF) continue;   /* null linedef/sidedef/front */

#if R_SHOW == 2
        continue;
#endif
        const float ax = sf->ax, ay = sf->ay;
        const float bx = sf->bx, by = sf->by;

        /* Backface rejection: skip surfaces turned away from the camera before
         * doing any projection work. */
        if ((bx - ax) * (cur_cam->y - ay) - (by - ay) * (cur_cam->x - ax) > 0.0f)
            { STAT(stat_back++); continue; }

        const sector_t *front = &sectors[sf->frontsec];
        const side_t   *fs    = &sides[sf->sideidx];
        line_t         *ld    = &lines[sf->lineidx];
        const sector_t *back  =
            sf->backsec == 0xFFFF ? NULL : &sectors[sf->backsec];

        /* Pure trigger and sector-boundary lines -- two-sided, identical
         * heights both sides -- draw nothing and clip nothing, yet used to
         * pay the full three-plane projection and the wall struct build
         * before being noticed. Compared on the RAW fixed values, before
         * any conversion is paid: every reachable height is a whole number
         * of units (map data and every mover speed are integral), and
         * integers to 2^24 convert to float exactly, so fixed equality and
         * the float equality this replaces decide identically. The
         * degenerate same-AND-closed case (zero height both sides) still
         * falls through to the closed-door logic below, as before. */
        if (back &&
            back->ceilingheight == front->ceilingheight &&
            back->floorheight   == front->floorheight   &&
            fs->midtexture == 0 &&
            !(back->ceilingheight <= front->floorheight ||
              back->floorheight   >= front->ceilingheight))
            { STAT(stat_same++); continue; }

        const float ffloor = fx(front->floorheight);
        const float fceil  = fx(front->ceilingheight);
        /* Converted once and reused below the span test: the ML_MAPPED
         * store in between goes through a line_t*, which TBAA lets alias a
         * sector's fixed_t fields, so the compiler must otherwise reload
         * and reconvert both heights for every window/closed seg. */
        float bfloor = 0.0f, bceil = 0.0f;
        if (back) {
            bfloor = fx(back->floorheight);
            bceil  = fx(back->ceilingheight);
        }

        int x1, x2;
        float xf[8];
        if (!screen_span(ax, ay, bx, by, &x1, &x2, xf)) { STAT(stat_span++); continue; }

        /* Remember the line as seen, for the automap.
         *
         * Vanilla sets this in R_StoreWallRange as the software renderer
         * commits a seg. That renderer is gone, so nothing set it and every
         * line stayed unmapped however far the player walked. Here is the
         * same moment -- the seg has a real span on screen. Guarded: the
         * blind store dirtied the line's cache line on every visible seg of
         * every frame for a bit that sticks after the first. */
        if (!(ld->flags & ML_MAPPED)) ld->flags |= ML_MAPPED;

        const uint8_t light = (uint8_t)(front->lightlevel < 0   ? 0 :
                                        front->lightlevel > 255 ? 255 :
                                        front->lightlevel);

        /* Build the wall directly in cur_wall -- it is already the by-value
         * global the clippers read, and the struct copy that used to sit
         * before every clip call was a fifth of the seg loop's stores. The
         * shared fields are written once per seg here; each surface below
         * writes only tex/zbot/ztop/ztex (and the mid case, masked). Every
         * field the flush reads is covered: x1..y2, u0, len, light, cd/co,
         * has_xform here; tex/heights per surface; glow just below. */
        cur_wall.x1 = ax; cur_wall.y1 = ay;
        cur_wall.x2 = bx; cur_wall.y2 = by;
        /* Sidedef offset live (scrollers animate it), seg term from the
         * mirror -- both in texels, both fixed_t at source (the raw cast
         * this replaces fed u0 the seg offset times 65536: congruent to
         * zero for power-of-two texture widths, so multi-seg walls
         * restarted their texture at every seg boundary). */
        cur_wall.u0  = fx(fs->textureoffset) + sf->u0seg;
        cur_wall.len = sf->len;
        cur_wall.light = light;
        cur_wall.cd1 = xf[0]; cur_wall.co1 = xf[1];
        cur_wall.cd2 = xf[2]; cur_wall.co2 = xf[3];
        cur_wall.has_xform = 1;
        cur_wall.masked = 0;

#if D_DYNLIGHT
        /* The emissive floor is usually the BACK sector's, not the front's.
         *
         * A sludge pool is a floor depression, so the wall you actually see is
         * the lower texture of a two-sided line viewed from the walkway: front
         * is the concrete you are standing on and the nukage is behind the
         * line. Asking only the front sector lit four wall segs in an entire
         * demo route and none thereafter -- the handful being one-sided walls
         * standing in the pool itself.
         *
         * So take whichever adjacent floor actually glows, and light from that
         * floor's height. If both do, the brighter wins; they are the same
         * flat in practice. */
        {
            float gl = D_FlatGlow(front->floorpic);
            float gz = ffloor;                 /* already converted above */
            int   gpic = front->floorpic;
            if (back) {
                const float gb = D_FlatGlow(back->floorpic);
                if (gb > gl) { gl = gb; gz = bfloor; gpic = back->floorpic; }
            }
            cur_wall.glow  = gl;
            cur_wall.glowz = gz;
            cur_wall.reflect = gl > 0.0f;
            if (gl > 0.0f) {
                D_FlatGlowRGB(gpic, cur_wall.glow_rgb);
            } else {
                /* Not over a liquid -- but a polished floor (teleporter
                 * pad, marble, tech plate) still mirrors. Reuse glowz as
                 * the plane and glow_rgb as the tint; glow stays 0, so
                 * the light spill nulls itself. Same side preference as
                 * the glow: the reflective floor may be behind the line. */
                int rpic = -1;
                if (D_FlatReflective(front->floorpic)) {
                    rpic = front->floorpic; gz = ffloor;
                } else if (back && D_FlatReflective(back->floorpic)) {
                    rpic = back->floorpic;  gz = bfloor;
                }
                if (rpic >= 0) {
                    cur_wall.reflect = 1;
                    cur_wall.glowz   = gz;
                    D_FlatGlowRGB(rpic, cur_wall.glow_rgb);
                } else {
                    cur_wall.glow_rgb[0] = cur_wall.glow_rgb[1] =
                    cur_wall.glow_rgb[2] = 1.0f;
                }
            }
        }
#endif
        const float rowoff = fx(fs->rowoffset);

        if (!back) {
            STAT(stat_1s++);
            /* One-sided: a solid wall floor to ceiling, and it occludes.
             * Normally the texture hangs from the ceiling; lower-unpegged
             * stands it on the floor instead.
             *
             * A missing texture still has to occlude: the surface is opaque
             * whether or not it has art, and skipping the clip would leak
             * whatever lies behind it. */
            cur_wall.tex = p_level_texture(fs->midtexture);
            D_WALL_MIRROR(fs->midtexture);
            if (cur_wall.tex) {
                const float texh = (float)cur_wall.tex->height;
                cur_wall.zbot = ffloor; cur_wall.ztop = fceil;
                cur_wall.ztex = ((ld->flags & ML_DONTPEGBOTTOM)
                                 ? ffloor + texh : fceil) + rowoff;
                if (cur_wall.ztop > cur_wall.zbot) {
                    clip_solid_close(x1, x2);
                    continue;
                }
            }
            mark_solid(x1, x2);
            continue;
        }

        /* bfloor/bceil arrive from the hoisted conversion above. */

        /* A two-sided line still blocks sight when the sectors behind it do
         * not overlap vertically -- a closed door. Doom treats that as solid,
         * which matters: without it, every shut door leaks the room behind. */
        if (bceil <= ffloor || bfloor >= fceil) {
            STAT(stat_closed++);

            /* Solid for the purposes of occlusion, but still a surface that
             * has to be drawn -- and what is drawn is the *upper* texture, not
             * the middle one. A door is a sector whose ceiling has come down
             * to its floor, so the art the player faces hangs from the front
             * ceiling to the back one; a door line usually has no midtexture
             * at all, which is why reaching for it left every shut door and
             * fence invisible. */
            if (fceil > bceil) {
                cur_wall.tex = p_level_texture(fs->toptexture);
            D_WALL_MIRROR(fs->toptexture);
                if (cur_wall.tex) {
                    const float texh = (float)cur_wall.tex->height;
                    cur_wall.zbot = bceil; cur_wall.ztop = fceil;
                    cur_wall.ztex = ((ld->flags & ML_DONTPEGTOP)
                                     ? fceil : bceil + texh) + rowoff;
                    clip_solid_close(x1, x2);
                    continue;
                }
            }
            if (bfloor > ffloor) {
                cur_wall.tex = p_level_texture(fs->bottomtexture);
            D_WALL_MIRROR(fs->bottomtexture);
                if (cur_wall.tex) {
                    cur_wall.zbot = ffloor; cur_wall.ztop = bfloor;
                    cur_wall.ztex = ((ld->flags & ML_DONTPEGBOTTOM)
                                     ? fceil : bfloor) + rowoff;
                    clip_solid_close(x1, x2);
                    continue;
                }
            }

            /* Neither exists: still opaque, so it must occlude even with no
             * art to show for it. */
            mark_solid(x1, x2);
            continue;
        }

        /* Identical-heights lines were disposed of before projection, above. */

        /* A window. The strips above and below it are solid surfaces, but the
         * opening is not, so they clip as pass rather than solid. */
        STAT(stat_window++);
        /* Between two sky sectors there is no upper surface at all -- the
         * opening runs straight through to the backdrop. Drawing one here is
         * the classic "sky wall" artefact. */
        const bool sky_upper = front->ceilingpic == skyflatnum &&
                               back->ceilingpic  == skyflatnum;
        if (sky_upper) {
#ifdef R_FLATDUMP
            if ((r_flat_stamp & 127) == 0 && r_flat_stamp > 0)
                debugf("sk: seg fsec=%d bsec=%d x=%d..%d\n",
                       (int)(front - sectors), (int)(back - sectors), x1, x2);
#endif
            r_sky_span_add(x1, x2);
        }

        if (fceil > bceil && !sky_upper) {       /* upper */
            cur_wall.tex = p_level_texture(fs->toptexture);
            D_WALL_MIRROR(fs->toptexture);
            if (cur_wall.tex) {
                const float texh = (float)cur_wall.tex->height;
                cur_wall.zbot = bceil; cur_wall.ztop = fceil;
                /* Upper textures normally hang from the *lower* ceiling so a
                 * raising door reveals more of them; upper-unpegged pins them
                 * to the front ceiling instead. */
                cur_wall.ztex = ((ld->flags & ML_DONTPEGTOP)
                                 ? fceil : bceil + texh) + rowoff;
                clip_pass(x1, x2);
            }
        }
        if (bfloor > ffloor) {                   /* lower */
            cur_wall.tex = p_level_texture(fs->bottomtexture);
            D_WALL_MIRROR(fs->bottomtexture);
            if (cur_wall.tex) {
                cur_wall.zbot = ffloor; cur_wall.ztop = bfloor;
                /* Lower textures start at the far floor, unless lower-unpegged,
                 * which aligns them with the front ceiling so steps line up
                 * with the wall above them. */
                cur_wall.ztex = ((ld->flags & ML_DONTPEGBOTTOM)
                                 ? fceil : bfloor) + rowoff;
                clip_pass(x1, x2);
            }
        }

        /* Two-sided masked mid: the fence or grate hanging in the opening.
         * One texture height exactly, never tiled vertically -- lower-
         * unpegged stands it on the higher floor, otherwise it hangs from
         * the lower ceiling -- and clipped to the opening. It neither
         * occludes nor closes columns; the alpha cut happens per pixel. */
        if (fs->midtexture) {
            cur_wall.tex = p_level_texture(fs->midtexture);
            D_WALL_MIRROR(fs->midtexture);
            if (cur_wall.tex) {
                const float texh  = (float)cur_wall.tex->height;
                const float o_top = fminf(fceil, bceil);
                const float o_bot = fmaxf(ffloor, bfloor);
                float ztop, zbot;
                if (ld->flags & ML_DONTPEGBOTTOM) {
                    zbot = o_bot + rowoff;
                    ztop = zbot + texh;
                } else {
                    ztop = o_top + rowoff;
                    zbot = ztop - texh;
                }
                cur_wall.ztex = ztop;
                if (ztop > o_top) ztop = o_top;
                if (zbot < o_bot) zbot = o_bot;
                if (ztop > zbot) {
                    cur_wall.zbot = zbot;
                    cur_wall.ztop = ztop;
                    cur_wall.masked = 1;
                    clip_pass(x1, x2);
                    cur_wall.masked = 0;
                }
            }
        }

        /* Narrow each column to this opening -- AFTER the strips above,
         * which sit at the portal plane and must not be clipped by their own
         * opening. Both opening edges are straight lines in screen space, so
         * per-column values step linearly between the seg's projected ends,
         * which screen_span already produced. A sky joint's top edge still
         * narrows: everything above the opening in those columns is covered
         * by the depth-tested sky pass, whose span they joined above. */
        {
            const float zt  = fminf(fceil, bceil) - cur_cam->z;
            const float zb  = fmaxf(ffloor, bfloor) - cur_cam->z;
            const float cyf = SCREEN_H * 0.5f;
            const float fda = cur_cam->focal_y / xf[4];
            const float fdb = cur_cam->focal_y / xf[5];
            clip_narrow(x1, x2,
                        cyf - zt * fda, cyf - zt * fdb,
                        cyf - zb * fda, cyf - zb * fdb,
                        xf[6], xf[7]);
        }
    }
#if D_HWSTAT
    r_ph_seg_us += TICKS_TO_US(TICKS_SINCE(t_seg_));
#endif
#if R_PVS == 2
    if (r_ss_emitted) r_pvs_check_drawn(num);
#endif
}

/* Both halves of Doom's R_CheckBBox: the two-corner silhouette span for the
 * frustum, then the solid-segment list for occlusion -- a subtree whose
 * entire screen span is already covered by nearer solid walls contributes
 * nothing. This is where whole rooms behind a wall vanish. Reads the float
 * node mirror; runs twice per node per frame, which is where the BSP
 * traversal spends its time on larger maps. */

#if D_HWSTAT
static bool child_visible(int nodenum, int child)
{
    const uint32_t t_cull_ = TICKS_READ();
    bool r_ = child_visible_inner(nodenum, child);
    r_ph_cull_us += TICKS_TO_US(TICKS_SINCE(t_cull_));
    return r_;
}
#else
#define child_visible child_visible_inner
#endif

static bool child_visible_inner(int nodenum, int child)
{
#if R_PVS == 1
    /* One bit, before any projection: can anything under this child be seen
     * from the subsector the camera is standing in. */
    {
        const int c = nodes[nodenum].children[child];
        if (!((c & NF_SUBSECTOR) ? r_pvs_ss_visible(c & ~NF_SUBSECTOR)
                                 : r_pvs_node_visible(c)))
            return false;
    }
#endif
    const r_nodef_t *nf = &r_nodef[nodenum];

#ifdef R_FLATDUMP
    #define CDUMP(what, a, b) do { if ((r_flat_stamp & 127) == 0 && r_flat_stamp > 0)         debugf("cv: n=%d c=%d box=%d,%d..%d,%d %s %d %d\n", nodenum, child,                (int)nf->xl[child], (int)nf->yl[child],                (int)nf->xh[child], (int)nf->yh[child], what, a, b); } while (0)
#else
    #define CDUMP(what, a, b)
#endif

    int lo, hi;
    float dmin, dmax;
    if (!span_of_bbox(nf->xl[child], nf->yl[child],
                      nf->xh[child], nf->yh[child], &lo, &hi, &dmin, &dmax))
        { CDUMP("spanrej", 0, 0); return false; }

    if (range_covered(lo, hi)) { CDUMP("covered", lo, hi); return false; }

    /* Vertical half: every column closed means invisible whatever the
     * height. Otherwise, a subtree whose height range projects wholly
     * outside the loosest open window is hidden behind a doorway's lintel
     * or sill. Conservative in every direction: loosest window over the
     * span, nearest depth for the extremity away from eye level, farthest
     * for the one toward it. The depth range comes from span_of_bbox, which
     * already formed the corner products; the clamps below reproduce what
     * the old recompute made of the inside/wrap cases. */
    {
        float wtop, wbot;
        clip_window(lo, hi, &wtop, &wbot);
        if (wtop >= wbot) { CDUMP("closed", lo, hi); return false; }

        if (wtop > 0.0f || wbot < (float)SCREEN_H) {
            if (dmin < BSP_NEAR) dmin = BSP_NEAR;
            if (dmax < dmin)     dmax = dmin;

            const float cyf = SCREEN_H * 0.5f;
            const float f   = cur_cam->focal_y;
            const float zt  = nf->zhi[child] - cur_cam->z;
            const float zb  = nf->zlo[child] - cur_cam->z;
            const float ytop = cyf - f * (zt > 0.0f ? zt / dmin : zt / dmax);
            const float ybot = cyf - f * (zb < 0.0f ? zb / dmin : zb / dmax);

            if (ytop > wbot + 1.0f) { CDUMP("below", (int)ytop, (int)wbot); return false; }
            if (ybot < wtop - 1.0f) { CDUMP("above", (int)ybot, (int)wtop); return false; }
        }
    }
    return true;
}

static void render_node(int num)
{
    while (!(num & NF_SUBSECTOR)) {
        if (num >= numnodes) return;
        /* One u32 from the dense mirror gives both children; the walk no
         * longer touches Doom's node_t at all (point_on_side and the cull
         * read r_nodef). */
        const uint32_t kids = r_nodekids[num];
        STAT(stat_nodes++);

        const int side = point_on_side(cur_cam->x, cur_cam->y, num);

        /* Near side first. Recursing on the near child and looping on the far
         * one keeps recursion depth at the tree's depth rather than doubling
         * it, which matters on a console with a small stack. */
        if (child_visible(num, side)) render_node(side ? (int)(kids & 0xFFFF)
                                                       : (int)(kids >> 16));
        else                          STAT(stat_culled++);

        if (!child_visible(num, side ^ 1)) { STAT(stat_culled++); return; }
        num = side ? (int)(kids >> 16) : (int)(kids & 0xFFFF);
    }
    render_subsector(num & ~NF_SUBSECTOR);
}

void r_render_level(const r_camera_t *cam)
{
    cur_cam     = cam;
    r_thing_vc++;
    r_drop_bbox = r_drop_span = r_drop_cover = 0;
    stat_segs   = 0;
    stat_walls  = 0;
    stat_nodes  = 0;
    stat_culled = 0;
    stat_flatcull = 0;
    stat_1s = stat_closed = stat_same = stat_window = stat_back = stat_span = 0;
    stat_em_solid = stat_em_pass = 0;
    STAT(in_pass = 0);

    r_pvs_frame(cam->x, cam->y);
    r_set_view(cam);                       /* fills r_view_cs / r_view_sn */
    view_k = (SCREEN_W * 0.5f) / cam->focal_x;
    r_flat_stamp++;                        /* invalidate region stamps */
    clip_reset();

    /* Doors and lifts move sector heights, and the vertical cull reads
     * per-subtree height ranges derived from them. T_MovePlane -- the one
     * choke point every gradual height change passes through -- raises the
     * flag (savegame loads raise it in P_UnArchiveWorld), so quiet frames
     * no longer pay the all-sectors checksum walk this replaces. The flag
     * accumulates across multi-tic bursts by construction: nothing clears
     * it but this refresh, and a mover's final tic sets it, so the frame
     * after settle re-reads the resting heights. */
    {
        extern int r_nodez_dirty;
        if (r_nodez_dirty) {
            r_nodez_dirty = 0;
            R_NodeZConsume();     /* bubbles marked sectors; full walk only
                                     when everything is suspect */
        }
    }

    if (!numnodes) {
        /* A single-subsector map has no nodes at all. */
        render_subsector(0);
        return;
    }
    render_node(numnodes - 1);
}

int r_bsp_segs(void)   { return stat_segs; }
int r_bsp_walls(void)  { return stat_walls; }
int r_bsp_nodes(void)  { return stat_nodes; }
int r_bsp_culled(void) { return stat_culled; }
int r_bsp_flatcull(void) { return stat_flatcull; }

void r_bsp_dump(void)
{
    debugf("bsp: segs=%d back=%d offscreen=%d | 1sided=%d closed=%d same=%d window=%d | emit=%d\n",
           stat_segs, stat_back, stat_span, stat_1s, stat_closed, stat_same,
           stat_window, stat_walls);
    debugf("bsp: emit solid=%d pass=%d\n", stat_em_solid, stat_em_pass);
}
