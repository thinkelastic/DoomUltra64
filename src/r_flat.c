/*
 * r_flat -- floors and ceilings.
 *
 * A subsector's floor is a convex, horizontal polygon, so a triangle fan is
 * all the geometry needed. Two things make it different from a wall:
 *
 * Texture coordinates come from world position, not from distance along a
 * surface: a flat is nailed to the map grid so adjoining subsectors line up.
 * Doom flats cover 64x64 world units; ours are downsampled to 32x32 texels,
 * so one texel spans two units.
 *
 * The texture wraps rather than clamps. A floor polygon is usually much larger
 * than 64 units, and letting the RDP wrap avoids splitting the geometry per
 * texture tile -- which is what walls have to do, and what makes them
 * expensive.
 *
 * Unlike walls, flats cannot be ordered by BSP traversal alone. Front-to-back
 * painter's order breaks wherever a distant floor projects into a nearer one
 * through a doorway, which is common. They are depth-buffered instead.
 */
#include "r_flat.h"
#include "r_tri.h"

#include <math.h>

#define FLAT_MAX_PTS 24

/* Upper bound on a clipped polygon, and the size of every scratch buffer here.
 *
 * Sutherland-Hodgman adds at most one vertex per clipping plane: three for the
 * view wedge (depth, left, right) and one per band peeled off afterwards. So
 * n+7 is the true worst case and n+8 is a safe bound.
 *
 * This was FLAT_MAX_PTS*2 across six separate arrays, which put draw_one's
 * stack frame at 4840 bytes -- against an 8 KB data cache, with 52 calls a
 * frame. Reserving worst-case space for polygons that are actually five or six
 * points is not free on this CPU: it is paid in evicted cache lines, and each
 * one costs a writeback to RDRAM. */
/* Widened from +8: the +1-vertex-per-plane bound that justified +8 holds
 * only for CONVEX input, and merged flat regions are convex only to within a
 * float tolerance. An epsilon-concave ring can cross a clip plane more than
 * twice, and overflowing this buffer silently DROPPED vertices -- a missing
 * wedge in the polygon, seen near the screen edge at close range where the
 * near clip cuts deepest. Overflow is counted now, never silent. */
#define FLAT_CLIP_MAX (FLAT_MAX_PTS + 16)

/* Near clip for flat GEOMETRY. Deliberately smaller than R_FLAT_NEAR (the
 * depth-curve constant): see the note at dmin_v. */
#define FLAT_CLIP_NEAR 1.0f

/* World units per texel after the 2:1 downsample in the build tool. */
#define FLAT_UNITS_PER_TEXEL 2.0f

/* Texture repeat period, in texels. Flats are 32x32. */
#define FLAT_PERIOD 32.0f

/* Largest depth ratio allowed within one triangle.
 *
 * A floor is not a wall: a wall quad is already cut into 64-texel columns, so
 * its depth range is naturally small, but a single floor polygon runs from the
 * near plane to the horizon. The RDP's perspective divide is fixed point and
 * rdpq normalises 1/w against the nearest vertex, so a triangle spanning a
 * huge depth range loses all precision at its far end -- visible as the
 * surface warping, worst when the camera is close to it. */
/* 2.0, matching R_MAX_DEPTH_RATIO for walls and the host harness's deep-
 * triangle check. This was 4.0, which ares renders fine -- but the real
 * RDP's fixed-point perspective divide visibly warps texture coordinates
 * across primitives spanning more than about 2:1 in depth, which reads as
 * smeared or "malformed" floors at stair steps and long strips. The
 * emulator cannot show this; keep the invariant tight enough for hardware. */
#define FLAT_MAX_DEPTH_RATIO 2.0f

/* Z-buffer near constant for flats ONLY -- deliberately smaller than the
 * shared R_FLAT_NEAR=4 walls and sprites use, which biases every flat
 * farther by 0.5/d. The RDP interpolates z linearly in screen space per
 * primitive, and the linear approximation of the convex 1/d curve sags
 * toward the camera mid-span by up to ~K*0.083/d for a 2:1-depth primitive.
 * Walls and flats approximate that curve between DIFFERENT vertex sets, so
 * at shared edges -- a stair riser meeting its tread, seen at a grazing
 * angle -- whichever sags lower steals the junction pixels: floor texture
 * wedges poking through walls. Biasing flats farther than the worst-case
 * divergence makes walls win every shared edge, which is always the right
 * outcome: a vertical surface at the same depth as a floor edge is in
 * front of it. Sprites keep the shared constant and thus beat the floor
 * they stand on. */
#define FLAT_Z_NEAR 3.5f
/* Eight, not four. Bands step geometrically by FLAT_MAX_DEPTH_RATIO, so
 * the count sets how much depth the invariant actually covers: at the old
 * 4.0 ratio four bands reached 256:1 and spanned any real floor, but
 * tightening the ratio to 2.0 for the hardware's perspective divide
 * silently shrank that to 16:1. Past it the final band is unbounded and
 * breaks the very rule the tighter ratio exists to enforce -- and a
 * corridor floor passes 16:1 easily. Eight bands restore 256:1. Only
 * surfaces that actually span that depth pay for the extra bands; the
 * small-extent early-out below still skips banding entirely for the rest. */
#define FLAT_MAX_BANDS 8

static const rdpq_trifmt_t TRIFMT_FLAT = {
    .pos_offset   = 0,
    .shade_offset = 2,
    .tex_offset   = 6,
    .tex_tile     = TILE0,
    .z_offset     = 9,
};

/* View-space polygon vertex: depth and offset for projection, world position
 * retained because flat texture coordinates come from the map grid. */
typedef struct { float d, o, wx, wy; } fvtx_t;

static int clip_depth(const fvtx_t *in, int n, fvtx_t *out, float d0, bool keep_far);
static int clip_side(const fvtx_t *in, int n, fvtx_t *out, float k, bool keep_left);
static void emit_fan(const r_camera_t *cam, const fvtx_t *c, int m,
                     float dz, float shade, float sorg, float torg);

/* One queued surface. The polygon is referenced, not copied: subsector
 * polygons live in the level arena and do not move. */
typedef struct {
    const r_polypt_t *pts;
    dt64_tex_t     *tex;
    float           height;
    float           shade;
    uint8_t         npts;
    uint8_t         texidx;
} flatjob_t;

/* A dense view of a real level queues well under this; E1M1 peaks near 90. */
#define FLAT_MAX_JOBS      256
#define FLAT_MAX_TEXTURES  32

static flatjob_t   jobs[FLAT_MAX_JOBS];
static int         numjobs;
static dt64_tex_t *jobtex[FLAT_MAX_TEXTURES];
static int         numjobtex;

static int stat_flats;
static int stat_uploads;
static int stat_dropped;
static int stat_calls, stat_bands;

/* Counted for the same reason as r_bsp's: a missing floor should be
 * attributable, not guessed at. */
int r_drop_npts, r_drop_depth, r_drop_side, r_drop_pool, r_drop_clipofl;
static uint32_t stat_emit_us, stat_bind_us;

void r_flat_begin(void)
{
    stat_flats   = 0;
    stat_uploads = 0;
    stat_dropped = 0;
    stat_calls = stat_bands = 0;
    r_drop_npts = r_drop_depth = r_drop_side = r_drop_pool = 0;
    stat_emit_us = stat_bind_us = 0;
    numjobs      = 0;
    numjobtex    = 0;
}

int r_flat_count(void)   { return stat_flats; }
int r_flat_uploads(void) { return stat_uploads; }
int r_flat_calls(void)   { return stat_calls; }
int r_flat_bands(void)   { return stat_bands; }
int r_flat_emit_us(void) { return (int)stat_emit_us; }
int r_flat_bind_us(void) { return (int)stat_bind_us; }
int r_flat_dropped(void) { return stat_dropped; }

void r_flat_add(const r_polypt_t *pts, int npts, float height, float shade,
                dt64_tex_t *tex)
{
    if (!tex || npts < 3 || npts > FLAT_MAX_PTS) return;
    if (numjobs >= FLAT_MAX_JOBS) { stat_dropped++; return; }

    int ti = -1;
    for (int i = 0; i < numjobtex; i++)
        if (jobtex[i] == tex) { ti = i; break; }
    if (ti < 0) {
        if (numjobtex >= FLAT_MAX_TEXTURES) { stat_dropped++; return; }
        ti = numjobtex;
        jobtex[numjobtex++] = tex;
    }

    flatjob_t *j = &jobs[numjobs++];
    j->pts    = pts;
    j->tex    = tex;
    j->height = height;
    j->shade  = shade;
    j->npts   = (uint8_t)npts;
    j->texidx = (uint8_t)ti;
}

/* Wrapping is the point of the upload parameters: REPEAT_INFINITE lets one
 * polygon cover any area without being cut along texture boundaries. */
static void bind_flat(dt64_tex_t *tex)
{
    rdpq_texparms_t p = {0};
    p.s.repeats = REPEAT_INFINITE;
    p.t.repeats = REPEAT_INFINITE;
    rdpq_tex_upload(TILE0, &tex->surface, &p);
    stat_uploads++;
}

static void draw_one(const r_camera_t *cam, const r_polypt_t *pts, int npts,
                     float height, float shade);

void r_flat_flush(const r_camera_t *cam)
{
    r_tri_group_begin();
    /* Group the queue by texture in one pass.
     *
     * This scanned the whole job list once per texture, so a frame with 32
     * textures and 90 queued surfaces walked 2880 entries to issue 90 draws.
     * A singly-linked bucket per texture gets the same ordering in O(jobs). */
    int16_t head[FLAT_MAX_TEXTURES];
    static int16_t next[FLAT_MAX_JOBS];
    for (int t = 0; t < numjobtex; t++) head[t] = -1;
    for (int i = numjobs - 1; i >= 0; i--) {
        next[i] = head[jobs[i].texidx];
        head[jobs[i].texidx] = (int16_t)i;
    }

    for (int t = 0; t < numjobtex; t++) {
        if (head[t] < 0) continue;
#if D_HWSTAT
        const uint32_t t_ = TICKS_READ();
#endif
        bind_flat(jobtex[t]);
        r_tri_group_begin();               /* TMEM changed: re-register */
#if D_HWSTAT
        stat_bind_us += TICKS_TO_US(TICKS_SINCE(t_));
#endif
        for (int i = head[t]; i >= 0; i = next[i])
            draw_one(cam, jobs[i].pts, jobs[i].npts, jobs[i].height,
                     jobs[i].shade);
    }
    numjobs   = 0;
    numjobtex = 0;
}

/* The emit timer is instrumentation, and instrumentation in this loop is
 * not free: two clock reads per band per flat, every frame. Only HWSTAT
 * builds pay for it. */
#if D_HWSTAT
#define EMIT_TIMED(pts_, n_) do {                                   \
        const uint32_t t_ = TICKS_READ();                           \
        emit_fan(cam, (pts_), (n_), dz, shade, sorg, torg);          \
        stat_emit_us += TICKS_TO_US(TICKS_SINCE(t_));               \
    } while (0)
#else
#define EMIT_TIMED(pts_, n_) \
        emit_fan(cam, (pts_), (n_), dz, shade, sorg, torg)
#endif

static void draw_one(const r_camera_t *cam, const r_polypt_t *pts, int npts,
                     float height, float shade)
{
    stat_calls++;
#ifdef R_FLATDUMP
    #define DDUMP(what) do { extern uint32_t r_flat_stamp; if ((r_flat_stamp & 127) == 0 && r_flat_stamp > 0)         debugf("dd: h=%d n=%d %s\n", (int)height, npts, what); } while (0)
#else
    #define DDUMP(what)
#endif
    if (npts < 3 || npts > FLAT_MAX_PTS) { DDUMP("npts"); r_drop_npts++; return; }

    /* Camera basis cached once per frame by r_set_view. Calling newlib's
     * software cosf/sinf here cost hundreds of cycles per queued flat, ~90
     * times a frame, for a value that cannot change mid-flush. */
    const float cs = r_view_cs, sn = r_view_sn;
    const float dz = height - cam->z;

    /* View-space polygon: depth along the view axis, offset across it, plus
     * the world position kept for texture lookup.
     *
     * The transform depends only on the polygon and the camera, not on the
     * surface height -- so a region drawn as both floor and ceiling would
     * do this rotation twice per frame over identical vertices. A small
     * direct-mapped cache keyed on the point array and the frame stamp lets
     * the second surface reuse the first's transform in place; draw_one is
     * single-threaded, so pointing src at the cache entry is free. */
    extern uint32_t r_flat_stamp;
    #define XFORM_SLOTS 8
    static struct {
        const r_polypt_t *pts;
        uint32_t stamp;
        int      n;
        fvtx_t   v[FLAT_MAX_PTS];
    } xcache[XFORM_SLOTS];

    const unsigned slot = ((uintptr_t)pts >> 4) & (XFORM_SLOTS - 1);
    fvtx_t *v = xcache[slot].v;
    int n = npts;

    if (xcache[slot].pts != pts || xcache[slot].stamp != r_flat_stamp ||
        xcache[slot].n != npts) {
        for (int i = 0; i < npts; i++) {
            const float dx = pts[i].x - cam->x, dy = pts[i].y - cam->y;
            v[i].d  = dx * cs + dy * sn;
            v[i].o  = dx * sn - dy * cs;
            v[i].wx = pts[i].x;
            v[i].wy = pts[i].y;
        }
        xcache[slot].pts   = pts;
        xcache[slot].stamp = r_flat_stamp;
        xcache[slot].n     = npts;
    }

    /* Clip the polygon into the region that projects to sane coordinates.
     *
     * Clamping projected vertices instead -- which this did -- is not a
     * weaker version of clipping, it is wrong: it moves a vertex off the line
     * it belongs to and deforms the polygon. Standing close to a surface, its
     * near vertices project enormously far out, get yanked to the clamp box,
     * and the deformed shape stops reaching the corners of the screen. That
     * is the floor vanishing beneath you.
     *
     * The bounds are generous -- far outside the 320x240 viewport -- so
     * nothing visible is trimmed; they exist only to keep coordinates inside
     * the RDP's s11.2 range. */
    const float GUARD = 900.0f;

    fvtx_t c[FLAT_CLIP_MAX], tmpc[FLAT_CLIP_MAX];

    /* Vertical: y = cy - dz*focal/d, so bounding |y-cy| bounds d from below.
     * Bounded by the viewport edge (half the screen height from centre) plus
     * a margin for WALL_BLEED and rounding, not by the guard box: against
     * the +/-900px guard the floor underfoot survived down to d~7 while the
     * screen bottom is d~55, and the first depth band then lay entirely
     * below the viewport -- fanned, submitted and scissored away every
     * frame. A floor closer than this bound genuinely projects off the
     * screen, so nothing visible is lost.
     *
     * The floor of the bound is FLAT_CLIP_NEAR, not the shared depth-curve
     * constant. A surface within a few units of eye level -- a stair tread
     * four or five steps up -- is nearly edge-on, and clipping it at depth 4
     * put the clipped edge ON screen: a black band across the steps under
     * the weapon, treads visibly chopped. At one unit, the edge is on
     * screen only while the eye passes within ~0.75 units of the plane --
     * a one-frame flash mid-climb instead of a persistent hole. The depth
     * VALUE written to the z-buffer still uses R_FLAT_NEAR so the scene's
     * depth distribution is untouched; see emit_fan. */
    float dmin_v = FLAT_CLIP_NEAR;
    if (fabsf(dz) > 1e-3f) {
        const float dv = fabsf(dz) * cam->focal /
                         ((float)SCREEN_H * 0.5f + 16.0f);
        if (dv > dmin_v) dmin_v = dv;
    }

    const float kx = GUARD / cam->focal;

    /* Most polygons lie entirely inside the guard region, and running three
     * Sutherland-Hodgman passes over them just copies every vertex unchanged.
     * One cheap test up front skips all of it.
     *
     * This matters more than it looks: the clip/band/project pipeline is the
     * largest single cost in the frame, larger than every triangle and upload
     * put together, and the overwhelming majority of it is this. */
    bool inside = true;
    for (int i = 0; i < n; i++) {
        if (v[i].d < dmin_v || v[i].o > v[i].d * kx || v[i].o < -v[i].d * kx) {
            inside = false;
            break;
        }
    }

    /* On the fast path the polygon is used where it already is, rather than
     * copied into the clip buffer: the copy touched cache lines to no end. */
    const fvtx_t *src;
    int m;
    if (inside) {
        m = n;
        src = v;
    } else {
        src = c;
        m = clip_depth(v, n, c, dmin_v, true);
        if (m < 3) { DDUMP("depth"); r_drop_depth++; return; }
        m = clip_side(c, m, tmpc, kx, true);
        m = clip_side(tmpc, m, c, kx, false);
        if (m < 3) { DDUMP("side"); r_drop_side++; return; }
    }

    /* --- texture coordinate origin -------------------------------------
     *
     * RDP texture coordinates are s10.5, so they saturate past +/-1024.
     * Doom map coordinates blow through that easily -- E1M1 alone reaches
     * 2432 texels -- and the overflow wraps, which reads as a moire pattern
     * across the floor.
     *
     * Shifting by a whole number of texture periods costs nothing visually,
     * because the texture wraps anyway, and brings the coordinates back to
     * the polygon's own size. */
    float smin = 1e9f, tmin = 1e9f;
    for (int i = 0; i < m; i++) {
        const float sx = src[i].wx * (1.0f / FLAT_UNITS_PER_TEXEL);
        const float ty = src[i].wy * (1.0f / FLAT_UNITS_PER_TEXEL);
        if (sx < smin) smin = sx;
        if (ty < tmin) tmin = ty;
    }
    const float sorg = floorf(smin / FLAT_PERIOD) * FLAT_PERIOD;
    const float torg = floorf(tmin / FLAT_PERIOD) * FLAT_PERIOD;

    /* --- depth banding --------------------------------------------------
     * Split the polygon into bands of bounded depth ratio before projecting,
     * so no single triangle spans more range than the fixed-point divide can
     * carry. Bands are geometrically spaced, giving each the same ratio. */
    float dmin = 1e9f, dmax = -1e9f;
    for (int i = 0; i < m; i++) {
        if (src[i].d < dmin) dmin = src[i].d;
        if (src[i].d > dmax) dmax = src[i].d;
    }

    float edge[FLAT_MAX_BANDS + 1];
    int bands = 0;
    {
        /* Banding exists to bound perspective error in *pixels*, so a surface
         * that covers few pixels needs none however deep it runs. The vertical
         * extent at the near end is the bound: dz*focal/dmin. Without this a
         * distant floor strip a few pixels tall still paid for four bands. */
        const float extent = fabsf(dz) * cam->focal / dmin;

        if (extent < 24.0f && dmax <= dmin * FLAT_MAX_DEPTH_RATIO) {
            edge[bands++] = dmin;
        } else {
            float d = dmin;
            while (bands < FLAT_MAX_BANDS && d < dmax) {
                edge[bands++] = d;
                d *= FLAT_MAX_DEPTH_RATIO;
            }
            if (bands == 0) edge[bands++] = dmin;
        }
        edge[bands] = dmax * 1.001f;      /* keep the far edge inclusive */
    }

    /* Peel one band at a time off the remaining polygon.
     *
     * The near and far halves must be cut from the *same* source polygon by
     * the same plane, or their shared edge is interpolated between different
     * vertex pairs and the two bands disagree by a fraction of a pixel --
     * which shows up as hairline gaps along the seam. Clipping each band
     * independently out of the original polygon looks equivalent and is not:
     * a band clipped twice has already had vertices inserted, so the second
     * cut interpolates between different endpoints than its neighbour does. */
    /* Two buffers alternating, not three plus a copy: the far half of each cut
     * becomes the source for the next band in place. */
    fvtx_t bandbuf[2][FLAT_CLIP_MAX], near[FLAT_CLIP_MAX];
    int cur = 0, rn = m;
    for (int i = 0; i < m; i++) bandbuf[0][i] = src[i];

    for (int b = 0; b < bands && rn >= 3; b++) {
        const fvtx_t *rest = bandbuf[cur];
        if (b == bands - 1) {                       /* last band: all of it */
            EMIT_TIMED(rest, rn);
            break;
        }

        const int nn = clip_depth(rest, rn, near, edge[b + 1], false);
        const int fn = clip_depth(rest, rn, bandbuf[cur ^ 1], edge[b + 1], true);

        if (nn >= 3) EMIT_TIMED(near, nn);

        rn = fn;
        cur ^= 1;
    }
    stat_flats++;
}

/* Clip by one side plane of the view wedge.
 *
 * `right` selects which: keep offset <= depth*k, or keep offset >= -depth*k.
 * The two are not negations of each other -- negating the first gives
 * offset >= depth*k, which combined with the first leaves only the plane
 * itself and collapses the polygon to nothing. */
static int clip_side(const fvtx_t *in, int n, fvtx_t *out, float k, bool right)
{
    if (n < 3) return 0;

    int m = 0;
    for (int i = 0; i < n; i++) {
        const fvtx_t a = in[i], b = in[(i + 1) % n];
        const float sa = right ? (a.d * k - a.o) : (a.o + a.d * k);
        const float sb = right ? (b.d * k - b.o) : (b.o + b.d * k);

        if (sa >= 0.0f) {
            if (m < FLAT_CLIP_MAX) out[m++] = a; else r_drop_clipofl++;
        }
        if ((sa > 0.0f) != (sb > 0.0f) && m < FLAT_CLIP_MAX) {
            const float t = sa / (sa - sb);
            out[m].d  = a.d  + (b.d  - a.d ) * t;
            out[m].o  = a.o  + (b.o  - a.o ) * t;
            out[m].wx = a.wx + (b.wx - a.wx) * t;
            out[m].wy = a.wy + (b.wy - a.wy) * t;
            m++;
        }
    }
    return m;
}

/* Clip a view-space polygon by a plane of constant depth. */
static int clip_depth(const fvtx_t *in, int n, fvtx_t *out, float d0, bool keep_far)
{
    if (n < 3) return 0;
    const float sgn = keep_far ? 1.0f : -1.0f;

    int m = 0;
    for (int i = 0; i < n; i++) {
        const fvtx_t a = in[i], b = in[(i + 1) % n];
        const float sa = sgn * (a.d - d0), sb = sgn * (b.d - d0);

        if (sa >= 0.0f) {
            if (m < FLAT_CLIP_MAX) out[m++] = a; else r_drop_clipofl++;
        }
        if ((sa > 0.0f) != (sb > 0.0f) && m < FLAT_CLIP_MAX) {
            const float t = sa / (sa - sb);
            out[m].d  = a.d  + (b.d  - a.d ) * t;
            out[m].o  = a.o  + (b.o  - a.o ) * t;
            out[m].wx = a.wx + (b.wx - a.wx) * t;
            out[m].wy = a.wy + (b.wy - a.wy) * t;
            m++;
        }
    }
    return m;
}

static void emit_fan(const r_camera_t *cam, const fvtx_t *c, int m,
                     float dz, float shade, float sorg, float torg)
{
    stat_bands++;
    const float cx = SCREEN_W * 0.5f, cy = SCREEN_H * 0.5f;

    /* Project. The polygon is already clipped to a bounded region, so the
     * RDP's scissor handles what is off-screen. */
    /* Everything that does not vary per vertex, lifted out of the loop. The
     * camera focal length was being fetched through a pointer and multiplied
     * by dz on every vertex of every band, and the texel scale recomputed
     * twice each time. */
    const float focal = cam->focal;
    const float dzf   = dz * focal;
    const float tscale = 1.0f / FLAT_UNITS_PER_TEXEL;

    float sv[FLAT_CLIP_MAX][10];
    for (int i = 0; i < m; i++) {
        const float iw = 1.0f / c[i].d;
        /* Bounded by the clipping above, so no clamping is needed -- and
         * none may be applied, since that is what deformed the polygon. */
        sv[i][0] = cx + c[i].o * focal * iw;
        sv[i][1] = cy - dzf * iw;
        sv[i][2] = sv[i][3] = sv[i][4] = shade;
        sv[i][5] = 1.0f;
        sv[i][6] = c[i].wx * tscale - sorg;
        sv[i][7] = c[i].wy * tscale - torg;
        sv[i][8] = iw;
        /* Depth for the Z-buffer, 0 at the near plane rising to 1 far away.
         * Geometry may now live nearer than the curve's near constant
         * (FLAT_CLIP_NEAR < R_FLAT_NEAR); everything in that margin clamps
         * to depth 0 -- nothing can sort in front of it anyway. */
        float zv = 1.0f - FLAT_Z_NEAR * iw;
        sv[i][9] = zv < 0.0f ? 0.0f : zv;
    }

    r_tri_fan(&TRIFMT_FLAT, (const float (*)[10])sv, m);
    r_tri_flat += m - 2;
}
