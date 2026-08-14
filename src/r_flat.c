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

#ifndef R_LIQUIDRIPPLE
#define R_LIQUIDRIPPLE 0
#endif
#ifndef R_LIQUIDFLOW
#define R_LIQUIDFLOW 0
#endif

/* The swell rides ON the drift: same clock, same update, and its state
 * lives inside the drift's own guard. So it exists only where the drift
 * does -- LIQUIDRIPPLE=1 with LIQUIDFLOW=0 used to reference a ripple
 * that had not been compiled and broke the build. */
#if R_LIQUIDRIPPLE && R_LIQUIDFLOW && D_DYNLIGHT
#define R_RIPPLE_ON 1
#else
#define R_RIPPLE_ON 0
#endif
#include "r_fastmath.h"
#include "r_light.h"
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

#ifndef R_FLATDBG
#define R_FLATDBG 0
#endif
#if R_FLATDBG
/* Diagnostic: paint every depth band of every flat a different flat colour,
 * so the polygon structure a flat is actually built from is visible on the
 * television. A misplaced or short primitive shows as a wrongly-shaped
 * patch of one colour rather than as a subtle notch in a texture. Not a
 * shipping option; there is no texture and no shading under this. */
static int cur_banddbg;
static const float flatdbg_col[6][3] = {
    { 1.0f, 0.2f, 0.2f }, { 0.2f, 1.0f, 0.2f }, { 0.3f, 0.4f, 1.0f },
    { 1.0f, 1.0f, 0.2f }, { 1.0f, 0.3f, 1.0f }, { 0.2f, 1.0f, 1.0f },
};
#endif

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
/* The whole bias stack lives in r_flat.h, so walls, flats and sprites
 * cannot be moved out of order independently. */
#define FLAT_Z_NEAR R_FLAT_Z_NEAR
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
#if R_FUSESPLIT
static void split_depth(const fvtx_t *in, int n, fvtx_t *near_out, int *out_nn,
                        fvtx_t *far_out, int *out_fn, float d0);
#endif
static void emit_fan(const r_camera_t *cam, const fvtx_t *c, int m,
                     float dz, float shade, float sorg, float torg);

/* One queued surface. The polygon is referenced, not copied: subsector
 * polygons live in the level arena and do not move. */
typedef struct {
    const r_polypt_t *pts;
    float           height;
    /* Byte, not float: the flush rebuilds the shade with the callers' own
     * * (1/255.0f), bit-identically. The tex pointer that used to sit here
     * was written and never read (jobtex[] is what the flush binds), and
     * together the two cuts take the record from 24 to 16 bytes -- one
     * D-cache line per job instead of two. */
    uint8_t         shade_ll;
    uint8_t         npts;
    uint8_t         texidx;
    /* Nonzero when a light can reach the surface (caller's sphere-vs-box
     * test): the fan skips every per-vertex query otherwise. Occupies what
     * was the record's one pad byte. */
    uint8_t         lit;
#if D_DYNLIGHT
    /* Emissive contribution and its colour, as bytes: this array is 256 jobs
     * against an 8 KB D-cache, and four floats here would cost 4 KB of it for
     * a value that only ever drives a light. */
    uint8_t         glow;
    uint8_t         tint[3];
#endif
} flatjob_t;

/* A dense view of a real level queues well under this; E1M1 peaks near 90. */
#define FLAT_MAX_JOBS      256
#define FLAT_MAX_TEXTURES  32

static flatjob_t   jobs[FLAT_MAX_JOBS];
static int         numjobs;
static dt64_tex_t *jobtex[FLAT_MAX_TEXTURES];
static int         numjobtex;

#if D_DYNLIGHT
/* The job currently being emitted. emit_fan runs once per depth band of a
 * single job, so these are set per job rather than threaded through the band
 * macros -- the alternative is four extra arguments down two layers of macro
 * for a value that is constant across every band of the surface. */
static float cur_glow;
static float cur_tint[3] = { 1.0f, 1.0f, 1.0f };
static int   cur_lit;         /* job's light-reach flag; gates the queries */
/* Unlit AND unglowing -- which is nearly every flat outside a firefight
 * or a sludge pool. The three-channel shade chain then produces exactly
 * shade on every channel (zero light adds, zero glow adds, clamps
 * no-ops since shade <= 1), so one conversion replicated is
 * bit-identical to three, and ~20 FPU ops per vertex fold away. */
static int   cur_neutral;
#endif
#if R_FOGSCALE
/* Distance-falloff LUT entry for the job being emitted, same per-job
 * pattern as the glow above. Flats previously had NO diminishing at all --
 * a floor at 3000 units sat full-bright beside a near-black wall; with the
 * scaled fog they fall off exactly as the walls beside them do. */
static float cur_fog_inv;
#endif

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

void r_flat_add(const r_polypt_t *pts, int npts, float height, int lightlevel,
                dt64_tex_t *tex, float glow, const float tint[3], int lit,
                int reflective)
{
    /* The reflective flag routes to the reflection pass's region list --
     * the ghost clip polygons -- not to this queue; see r_reflect_region. */
    (void)reflective;
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
    j->pts      = pts;
    j->height   = height;
    j->shade_ll = (uint8_t)lightlevel;
    j->lit      = (uint8_t)(lit != 0);
#if D_DYNLIGHT
    j->glow    = (uint8_t)(glow < 0.0f ? 0.0f : (glow > 1.0f ? 255.0f : glow * 255.0f));
    j->tint[0] = (uint8_t)(tint[0] * 255.0f);
    j->tint[1] = (uint8_t)(tint[1] * 255.0f);
    j->tint[2] = (uint8_t)(tint[2] * 255.0f);
#else
    (void)glow; (void)tint;
#endif
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
    /* CI4 flats select their 16-entry bank of the resident 256-entry TLUT;
     * ignored for CI8, where palbank is zero. */
    p.palette = tex->palbank;
    rdpq_tex_upload(TILE0, &tex->surface, &p);
    stat_uploads++;
}

/* Texel density of the texture bound now. One flat always covers 64 world
 * units square, so texels-per-unit is width/64 -- exactly the legacy 0.5
 * for a 32x32 CI8 downsample (bit-identical to the old constant), 1.0 for
 * a full-resolution CI4 flat -- and the wrap period in texels is just the
 * width. Set per texture bucket in the flush. */
static float cur_tscale = 1.0f / FLAT_UNITS_PER_TEXEL;
static float cur_period = FLAT_PERIOD;
/* Reciprocal alongside, so the rebase keeps its multiply: both widths are
 * powers of two, so x * (1/w) is bit-identical to x / w -- exactly the
 * strength reduction the compiler did when the period was a constant. */
static float cur_inv_period = 1.0f / FLAT_PERIOD;

static void draw_one(const r_camera_t *cam, const r_polypt_t *pts, int npts,
                     float height, float shade);

#if R_LIQUIDFLOW && D_DYNLIGHT
/* How far the liquid has slid this frame, in texels.
 *
 * Vanilla nukage does not move: it swaps between three stills every eight
 * tics, which the eye reads as a slideshow rather than a current. Sliding
 * the texture coordinates continuously turns the same three frames into
 * flow -- and unlike the frame cycle it advances at DISPLAY rate, because
 * the sub-tic fraction rides along, so it glides where the frames step.
 *
 * Kept inside one wrap period: the texture repeats infinitely, so any
 * offset modulo the period looks identical, and staying bounded is what
 * keeps the s10.5 coordinates the rebase worked to establish. The two
 * axes run at different rates so the surface drifts diagonally and never
 * reveals the period by sliding along one axis. */
static float flow_u, flow_v;

#if R_RIPPLE_ON
/* A swell across the surface, on top of the drift.
 *
 * Each vertex's texture coordinate is displaced by a sine of its WORLD
 * position -- world, not screen or polygon, because adjacent subsectors
 * share their edge vertices and both must compute the identical offset
 * there or the pool tears along every internal boundary.
 *
 * Long wavelength and small amplitude, deliberately. It reads as a swell
 * rather than a wobble, and it is also the safe choice: Doom's subsectors
 * can meet at T-junctions, where one side carries a vertex mid-edge that
 * its neighbour lacks, and there the two interpolations cannot agree
 * exactly. A few texels of displacement keeps that disagreement under a
 * pixel; a large one would open a visible seam.
 *
 * The two axes are driven by the opposite coordinate and at different
 * rates, so the surface undulates instead of sliding as a block. */
#define RIPPLE_WAVELEN 256.0f
#define RIPPLE_K       (6.2831853f / RIPPLE_WAVELEN)
#define RIPPLE_AMP     2.5f            /* texels */

static float ripple_t;
static int   cur_ripple;

static inline void ripple_at(float wx, float wy, float *rs, float *rt)
{
    *rs = RIPPLE_AMP * sinf(wy * RIPPLE_K + ripple_t);
    *rt = RIPPLE_AMP * sinf(wx * RIPPLE_K + ripple_t * 1.27f);
}
#endif

static void flow_update(void)
{
    extern int   leveltime;
    extern float d_subtic;
    const float t = (float)leveltime + d_subtic;
    const float u = t * 0.055f, v = t * 0.034f;
    flow_u = u - rf_floorf(u * cur_inv_period) * cur_period;
    flow_v = v - rf_floorf(v * cur_inv_period) * cur_period;
#if R_RIPPLE_ON
    ripple_t = t * 0.095f;      /* halved from play: a swell, not a chop */
#endif
}
#endif

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
        cur_tscale = (float)jobtex[t]->width * (1.0f / 64.0f);
        cur_period = (float)jobtex[t]->width;
        cur_inv_period = 1.0f / cur_period;
#if R_LIQUIDFLOW && D_DYNLIGHT
        flow_update();          /* the period just changed; rewrap */
#endif
        r_tri_group_begin();               /* TMEM changed: re-register */
#if D_HWSTAT
        stat_bind_us += TICKS_TO_US(TICKS_SINCE(t_));
#endif
        for (int i = head[t]; i >= 0; i = next[i]) {
#if R_FOGSCALE
            cur_fog_inv = r_fog_inv[jobs[i].shade_ll];
#endif
#if D_DYNLIGHT
            cur_glow    = jobs[i].glow    * (1.0f / 255.0f);
            cur_tint[0] = jobs[i].tint[0] * (1.0f / 255.0f);
            cur_tint[1] = jobs[i].tint[1] * (1.0f / 255.0f);
            cur_tint[2] = jobs[i].tint[2] * (1.0f / 255.0f);
            cur_lit     = jobs[i].lit;
            cur_neutral = !cur_lit && jobs[i].glow == 0;
#if R_RIPPLE_ON
            cur_ripple  = jobs[i].glow > 0;
#endif
#endif
            draw_one(cam, jobs[i].pts, jobs[i].npts, jobs[i].height,
                     (float)jobs[i].shade_ll * (1.0f / 255.0f));
        }
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
        const float dv = fabsf(dz) * cam->focal_y /
                         ((float)SCREEN_H * 0.5f + 16.0f);
        if (dv > dmin_v) dmin_v = dv;
    }

    const float kx = GUARD / cam->focal_x;

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
        const float sx = src[i].wx * cur_tscale;
        const float ty = src[i].wy * cur_tscale;
        if (sx < smin) smin = sx;
        if (ty < tmin) tmin = ty;
    }
    float sorg = rf_floorf(smin * cur_inv_period) * cur_period;
    float torg = rf_floorf(tmin * cur_inv_period) * cur_period;
#if R_LIQUIDFLOW && D_DYNLIGHT
    /* Only what glows: sludge, lava, blood, water. Concrete does not
     * flow, and the polished floors registered reflective-only carry no
     * glow, so they stay put too. */
    if (cur_glow > 0.0f) { sorg -= flow_u; torg -= flow_v; }
#endif

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
        const float extent = fabsf(dz) * cam->focal_y / dmin;

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
#if R_FUSESPLIT
    /* The first band reads the caller's polygon directly -- the up-front
     * copy existed only to seed the alternating buffers, and single-band
     * surfaces (most of them) never needed the buffers at all. */
    const fvtx_t *restp = src;
#else
    for (int i = 0; i < m; i++) bandbuf[0][i] = src[i];
#endif

    for (int b = 0; b < bands && rn >= 3; b++) {
#if R_FUSESPLIT
        const fvtx_t *rest = restp;
#else
        const fvtx_t *rest = bandbuf[cur];
#endif
        if (b == bands - 1) {                       /* last band: all of it */
            EMIT_TIMED(rest, rn);
            break;
        }

#if R_FUSESPLIT
        int nn, fn;
        split_depth(rest, rn, near, &nn, bandbuf[cur ^ 1], &fn, edge[b + 1]);
#else
        const int nn = clip_depth(rest, rn, near, edge[b + 1], false);
        const int fn = clip_depth(rest, rn, bandbuf[cur ^ 1], edge[b + 1], true);
#endif

#if R_FLATDBG
        cur_banddbg = b;
#endif
        if (nn >= 3) EMIT_TIMED(near, nn);

        rn = fn;
        cur ^= 1;
#if R_FUSESPLIT
        restp = bandbuf[cur];
#endif
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
    /* Rolling pair rather than in[(i+1) % n].
     *
     * n is a runtime value, so the modulo is a real VR4300 integer divide --
     * 37 cycles with mfhi interlocks -- executed once per edge of every
     * clipped polygon, which is a couple of thousand iterations a frame across
     * the flat pipeline. Carrying the previous vertex forward also halves the
     * loads: each vertex is read once instead of twice. */
    fvtx_t a = in[0];
    for (int i = 0; i < n; i++) {
        const fvtx_t b = (i + 1 < n) ? in[i + 1] : in[0];
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
        a = b;
    }
    return m;
}

/* Clip a view-space polygon by a plane of constant depth. */
#if R_FUSESPLIT
/* Both halves of a depth cut in ONE edge walk. Each side keeps clip_depth's
 * boundary predicates and interpolant operands VERBATIM (near still tests
 * and divides on the negated distances), so every emitted vertex -- and
 * every >=0 boundary duplication -- is bit-identical to the two-pass form;
 * only the duplicated vertex loads, distance computations and loop overhead
 * are gone. Proven route-identical by the FUSESPLIT abdiff lever. */
static void split_depth(const fvtx_t *in, int n,
                        fvtx_t *near_out, int *out_nn,
                        fvtx_t *far_out, int *out_fn, float d0)
{
    int mn = 0, mf = 0;

    if (n >= 3) {
        fvtx_t a  = in[0];
        float  ua = a.d - d0;
        for (int i = 0; i < n; i++) {
            const fvtx_t b  = (i + 1 < n) ? in[i + 1] : in[0];
            const float  ub = b.d - d0;

            /* far side (clip_depth with sgn = +1, verbatim) */
            if (ua >= 0.0f) {
                if (mf < FLAT_CLIP_MAX) far_out[mf++] = a;
                else r_drop_clipofl++;
            }
            if ((ua > 0.0f) != (ub > 0.0f) && mf < FLAT_CLIP_MAX) {
                const float t = ua / (ua - ub);
                far_out[mf].d  = a.d  + (b.d  - a.d ) * t;
                far_out[mf].o  = a.o  + (b.o  - a.o ) * t;
                far_out[mf].wx = a.wx + (b.wx - a.wx) * t;
                far_out[mf].wy = a.wy + (b.wy - a.wy) * t;
                mf++;
            }

            /* near side (sgn = -1, verbatim) */
            const float na = -ua, nb = -ub;
            if (na >= 0.0f) {
                if (mn < FLAT_CLIP_MAX) near_out[mn++] = a;
                else r_drop_clipofl++;
            }
            if ((na > 0.0f) != (nb > 0.0f) && mn < FLAT_CLIP_MAX) {
                const float t = na / (na - nb);
                near_out[mn].d  = a.d  + (b.d  - a.d ) * t;
                near_out[mn].o  = a.o  + (b.o  - a.o ) * t;
                near_out[mn].wx = a.wx + (b.wx - a.wx) * t;
                near_out[mn].wy = a.wy + (b.wy - a.wy) * t;
                mn++;
            }

            a = b; ua = ub;
        }
    }

    *out_nn = mn;
    *out_fn = mf;
}
#endif /* R_FUSESPLIT */

static int clip_depth(const fvtx_t *in, int n, fvtx_t *out, float d0, bool keep_far)
{
    if (n < 3) return 0;
    const float sgn = keep_far ? 1.0f : -1.0f;

    int m = 0;
    /* Rolling pair, for the reason given in clip_side: the modulo on a
     * runtime n is a 37-cycle integer divide per edge. */
    fvtx_t a = in[0];
    for (int i = 0; i < n; i++) {
        const fvtx_t b = (i + 1 < n) ? in[i + 1] : in[0];
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
        a = b;
    }
    return m;
}

/* Ripple offsets in TEXELS, so the direct path (which works in s10.5)
 * and the registration path (which works in texels) apply the same
 * number and stay bit-identical where the ripple is off.
 *
 * FILE SCOPE on purpose: the reference path lives in the #else of
 * R_TRI_DIRECT, and defining this inside the direct branch left the host
 * harness -- the only build that compiles that path -- without it. */
#if R_RIPPLE_ON
#define FAN_RIPPLE_DECL(p)                                                  \
        float rs_ = 0.0f, rt_ = 0.0f;                                       \
        if (cur_ripple) ripple_at((p)->wx, (p)->wy, &rs_, &rt_);
#else
#define FAN_RIPPLE_DECL(p) const float rs_ = 0.0f, rt_ = 0.0f; (void)rs_; (void)rt_;
#endif

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
    const float focal_x = cam->focal_x;
    const float dzf     = dz * cam->focal_y;
    const float tscale = cur_tscale;
#if D_DYNLIGHT
    /* Loop-invariant pieces of the lit path, hoisted by hand: the glow
     * products and the query height do not vary per vertex, and spelling
     * that out here does not depend on the compiler proving that the sv
     * stores cannot alias the statics they read. The height keeps the SAME
     * expression (dz + cam->z) the per-vertex code used, so the sum is
     * bit-identical. */
    const float lz = dz + cam->z;
    const float glow_r = cur_glow * cur_tint[0];
    const float glow_g = cur_glow * cur_tint[1];
    const float glow_b = cur_glow * cur_tint[2];
#endif

#ifdef R_TRI_DIRECT
    /* Direct submission: each vertex is converted in registers and written
     * straight into an RSP slot -- no sv[][10] staging array built only to
     * be re-read and converted a second time by the generic fmt-driven
     * path. The arithmetic below is tri_vertex's, expression for
     * expression, so the pixels are identical; only the staging is gone.
     * The group-leading fan still routes its first triangle through
     * rdpq_triangle (see r_tri.h on autosync), and that path consumes the
     * SAME per-vertex expressions via the 10-float rows. */
    if (m >= 3) {
        /* One vertex: compute, convert exactly as tri_vertex does, write. */
#define FAN_VTX(slot_, p_) do {                                             \
        const fvtx_t *p = (p_);                                             \
        const float iw = 1.0f / p->d;                                       \
        const float sx = cx + p->o * focal_x * iw;                          \
        const float sy = cy - dzf * iw;                                     \
        FAN_SHADE_DECL(p)                                                   \
        const int16_t xi = (int16_t)rf_floor_i32(sx * 4.0f);                \
        const int16_t yi = (int16_t)rf_floor_i32(sy * 4.0f);                \
        FAN_RIPPLE_DECL(p)                                                  \
        const int16_t si = (int16_t)(p->wx * tsc32 - sorg32 + rs_ * 32.0f); \
        const int16_t ti = (int16_t)(p->wy * tsc32 - torg32 + rt_ * 32.0f); \
        float zv = 1.0f - FLAT_Z_NEAR * iw;                                 \
        if (zv < 0.0f) zv = 0.0f;                                           \
        const int16_t zi = (int16_t)(zv * 0x7FFF);                          \
        r_tri_slot((slot_), (xi << 16) | (yi & 0xFFFF), (zi << 16), rgba,   \
                   (si << 16) | (ti & 0xFFFF),                              \
                   r_tri_s16_16(1.0f / iw), r_tri_s16_16(iw));              \
    } while (0)
/* Distance falloff per vertex -- the depth is already in hand (it is what
 * the projection inverts), so this is one multiply-clamp against the job's
 * LUT entry. Clamp-then-fog, matching lit_shade's order on walls. */
#if R_FOGSCALE
#define FAN_VIS_DECL(p)  const float vis = r_vis((p)->d, cur_fog_inv);
#else
#define FAN_VIS_DECL(p)  const float vis = 1.0f;
#endif
#if D_DYNLIGHT
        /* Base light and white point lights stay neutral; only the emissive
         * term carries the surface's own colour (see the note at the old
         * staging loop's home: tinting the whole vertex would recolour
         * neighbouring concrete and double-tint the sludge). Shade fans out
         * per channel; alpha is 255 as sv[5]=1.0f always produced. The
         * neutral branch is bit-identical to the chain it skips (see
         * cur_neutral) and is what most vertices take. */
#define FAN_SHADE_DECL(p)                                                   \
        uint32_t rgba;                                                      \
        FAN_VIS_DECL(p)                                                     \
        if (cur_neutral) {                                                  \
            const uint32_t cs = (uint32_t)(shade * vis * 255.0f);           \
            rgba = (cs << 24) | (cs << 16) | (cs << 8) | 255u;              \
        } else {                                                            \
            float la[3];                                                    \
            if (cur_lit) r_light_at_rgb((p)->wx, (p)->wy, lz, la);          \
            else         la[0] = la[1] = la[2] = 0.0f;                      \
            float rr = shade + la[0] + glow_r;                              \
            float gg = shade + la[1] + glow_g;                              \
            float bb = shade + la[2] + glow_b;                              \
            if (rr > 1.0f) rr = 1.0f;                                       \
            if (gg > 1.0f) gg = 1.0f;                                       \
            if (bb > 1.0f) bb = 1.0f;                                       \
            rr *= vis; gg *= vis; bb *= vis;                                \
            rgba = ((uint32_t)(rr * 255.0f) << 24) |                        \
                   ((uint32_t)(gg * 255.0f) << 16) |                        \
                   ((uint32_t)(bb * 255.0f) << 8) | 255u;                   \
        }
#else
#define FAN_SHADE_DECL(p)                                                   \
        FAN_VIS_DECL(p)                                                     \
        const uint32_t cs = (uint32_t)(shade * vis * 255.0f);               \
        const uint32_t rgba = (cs << 24) | (cs << 16) | (cs << 8) | 255u;
#endif
        /* The texel origins are loop constants, carried pre-scaled into
         * s10.5: (wx*tscale - sorg)*32 == wx*(tscale*32) - sorg*32
         * bit-exactly, because every factor is a power of two -- scaling
         * commutes with the one rounding (the subtract) -- and it saves
         * two multiplies per vertex. s/t stay well inside the int16 texel
         * range by the sorg/torg rebase. */
        const float tsc32  = tscale * 32.0f;
        const float sorg32 = sorg * 32.0f, torg32 = torg * 32.0f;

        if (!r_tri_group_reg) {
            /* Group-leading fan: expand only the three vertices
             * rdpq_triangle reads, with the exact expressions the staging
             * loop used, so registration and direct paths stay
             * bit-identical. */
            float x3[3][10];
            for (int i = 0; i < 3; i++) {
                const float iw = 1.0f / c[i].d;
                x3[i][0] = cx + c[i].o * focal_x * iw;
                x3[i][1] = cy - dzf * iw;
                FAN_VIS_DECL(&c[i])
#if D_DYNLIGHT
                float la[3];
                if (cur_lit) r_light_at_rgb(c[i].wx, c[i].wy, lz, la);
                else         la[0] = la[1] = la[2] = 0.0f;
                float r = shade + la[0] + glow_r;
                float g = shade + la[1] + glow_g;
                float b = shade + la[2] + glow_b;
                x3[i][2] = (r > 1.0f ? 1.0f : r) * vis;
                x3[i][3] = (g > 1.0f ? 1.0f : g) * vis;
                x3[i][4] = (b > 1.0f ? 1.0f : b) * vis;
#else
                x3[i][2] = x3[i][3] = x3[i][4] = shade * vis;
#endif
#if R_FLATDBG
                {
                    const float *dc = flatdbg_col[cur_banddbg % 6];
                    x3[i][2] = dc[0]; x3[i][3] = dc[1]; x3[i][4] = dc[2];
                }
#endif
                x3[i][5] = 1.0f;
                FAN_RIPPLE_DECL(&c[i])
                x3[i][6] = c[i].wx * tscale - sorg + rs_;
                x3[i][7] = c[i].wy * tscale - torg + rt_;
                x3[i][8] = iw;
                float zv = 1.0f - FLAT_Z_NEAR * iw;
                x3[i][9] = zv < 0.0f ? 0.0f : zv;
            }
            rdpq_triangle(&TRIFMT_FLAT, x3[0], x3[1], x3[2]);
            r_tri_group_reg = 1;
        } else {
            FAN_VTX(0, &c[0]);
            FAN_VTX(1, &c[1]);
            FAN_VTX(2, &c[2]);
            r_tri_slots_issue(&TRIFMT_FLAT);
        }

        /* c[0] stays in slot 0 for the whole fan; each further triangle
         * shares its previous neighbour, alternating into the slot the
         * triangle before last used -- the r_tri_fan pattern. */
        for (int i = 3; i < m; i++) {
            FAN_VTX((i & 1) ? 1 : 2, &c[i]);
            r_tri_slots_issue(&TRIFMT_FLAT);
        }
#undef FAN_VTX
#undef FAN_SHADE_DECL
#undef FAN_VIS_DECL
    }
    r_tri_flat += m - 2;
#else /* !R_TRI_DIRECT: reference/host path, staged exactly as before */
    float sv[FLAT_CLIP_MAX][10];
    for (int i = 0; i < m; i++) {
        const float iw = 1.0f / c[i].d;
        /* Bounded by the clipping above, so no clamping is needed -- and
         * none may be applied, since that is what deformed the polygon. */
        sv[i][0] = cx + c[i].o * focal_x * iw;
        sv[i][1] = cy - dzf * iw;
        /* Per vertex, which costs nothing structural: the fan already carries
         * a colour per vertex and was simply being handed the same value at
         * each. The world position is right here -- wx/wy are retained for the
         * flat's texture coordinates, and the plane's height is dz above the
         * eye. Without this a floor lights uniformly across a whole subsector,
         * which reads as the room changing brightness rather than a light in
         * it. */
#if R_FOGSCALE
        const float vis = r_vis(c[i].d, cur_fog_inv);
#else
        const float vis = 1.0f;
#endif
#if D_DYNLIGHT
        /* Point lights carry their own colour per channel; the emissive
         * term still carries the surface's own tint. The sector base stays
         * neutral so a pool's neighbour keeps its concrete grey. */
        float la[3];
        if (cur_lit) r_light_at_rgb(c[i].wx, c[i].wy, lz, la);
        else         la[0] = la[1] = la[2] = 0.0f;
        float r = shade + la[0] + glow_r;
        float g = shade + la[1] + glow_g;
        float b = shade + la[2] + glow_b;
        sv[i][2] = (r > 1.0f ? 1.0f : r) * vis;
        sv[i][3] = (g > 1.0f ? 1.0f : g) * vis;
        sv[i][4] = (b > 1.0f ? 1.0f : b) * vis;
#else
        sv[i][2] = sv[i][3] = sv[i][4] = shade * vis;
#endif
#if R_FLATDBG
        {
            const float *dc = flatdbg_col[cur_banddbg % 6];
            sv[i][2] = dc[0]; sv[i][3] = dc[1]; sv[i][4] = dc[2];
        }
#endif
        sv[i][5] = 1.0f;
        FAN_RIPPLE_DECL(&c[i])
        sv[i][6] = c[i].wx * tscale - sorg + rs_;
        sv[i][7] = c[i].wy * tscale - torg + rt_;
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
#endif /* R_TRI_DIRECT */
}
