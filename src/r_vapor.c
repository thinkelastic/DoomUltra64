/* Vapor over emissive liquids. See r_vapor.h for the design. */

#include "r_vapor.h"

#if R_VAPOR

#include "d_glow.h"
#include "r_fastmath.h"
#include "r_flat.h"          /* R_FLAT_NEAR family of constants */
#include "r_light.h"         /* the same per-vertex query the flats use */
#include "r_tri.h"

#include <libdragon.h>
#include <string.h>

/* --- the noise texture ---------------------------------------------------
 *
 * 32x32 IA8 (4-bit intensity, 4-bit alpha), value noise at two octaves,
 * tiling by construction so REPEAT_INFINITE never shows a seam. All the
 * cloud SHAPE lives in the alpha channel; the intensity carries a gentle
 * brightening of the densest wisps so the haze reads as volume rather
 * than a flat wash. Bilinear filtering does the rest. */
#define VAP_TEX 32

static uint8_t   vap_texels[VAP_TEX * VAP_TEX] __attribute__((aligned(16)));
static surface_t vap_surf;

/* Small integer hash, good enough for lattice noise. */
static inline int vap_hash(int x, int y)
{
    uint32_t h = (uint32_t)(x * 374761393 + y * 668265263);
    h = (h ^ (h >> 13)) * 1274126177u;
    return (int)((h >> 16) & 255);
}

/* Value noise on a lattice of `period` cells over the tile, bilinearly
 * smoothed; wraps at the tile edge so the result tiles. */
static int vap_noise(int x, int y, int period)
{
    const int cell = VAP_TEX / period;
    const int cx = x / cell, cy = y / cell;
    const int fx = x % cell,  fy = y % cell;

    const int n00 = vap_hash(cx % period, cy % period);
    const int n10 = vap_hash((cx + 1) % period, cy % period);
    const int n01 = vap_hash(cx % period, (cy + 1) % period);
    const int n11 = vap_hash((cx + 1) % period, (cy + 1) % period);

    const int top = n00 * (cell - fx) + n10 * fx;
    const int bot = n01 * (cell - fx) + n11 * fx;
    return (top * (cell - fy) + bot * fy) / (cell * cell);
}

void r_vapor_init(void)
{
    for (int y = 0; y < VAP_TEX; y++)
        for (int x = 0; x < VAP_TEX; x++) {
            /* Two octaves, the second at half weight. 0..255. */
            int v = (vap_noise(x, y, 4) * 2 + vap_noise(x, y, 8)) / 3;
            /* Push the low end down so the thin parts of the cloud go
             * fully clear instead of hazing the whole polygon evenly. */
            v = (v - 64) * 3 / 2;
            if (v < 0)   v = 0;
            if (v > 255) v = 255;

            const int a = v >> 4;                     /* 0..15 */
            const int i = 10 + (v >> 6);              /* 10..13: subtle */
            vap_texels[y * VAP_TEX + x] = (uint8_t)((i << 4) | a);
        }

    vap_surf = surface_make(vap_texels, FMT_IA8, VAP_TEX, VAP_TEX, VAP_TEX);
    data_cache_hit_writeback(vap_texels, sizeof vap_texels);
}

/* --- the frame's queue --------------------------------------------------- */

/* A dense view queues a handful of pools; this is generous. */
#define VAP_MAX 24

typedef struct {
    const r_polypt_t *pts;
    float   h;                 /* layer height (liquid + hover) */
    uint8_t npts;
    uint8_t cls;
    uint8_t light;
} vapjob_t;

static vapjob_t vap_jobs[VAP_MAX];
static int      vap_n;

/* Per-family look: hover height, tint, peak opacity, texel scale and
 * drift. The sludge haze hugs the surface, thin and green; lava smoke
 * rides higher, darker and denser, drifting a little faster. */
typedef struct {
    float   hover;
    uint8_t r, g, b, a;
    float   uvscale;           /* world units -> texture repeats */
    float   drift_u, drift_v;  /* repeats per tic */
} vapstyle_t;

static const vapstyle_t vap_style[] = {
    [D_GLOW_NUKAGE] = { 10.0f, 150, 230, 150,  72, 1.0f / 192.0f,
                        0.0044f,  0.0028f },
    [D_GLOW_SLIME]  = { 10.0f, 170, 205, 170,  60, 1.0f / 192.0f,
                        0.0036f,  0.0024f },
    [D_GLOW_LAVA]   = { 22.0f,  72,  62,  56, 116, 1.0f / 160.0f,
                        0.0072f, -0.0048f },
};
#define VAP_NSTYLE (int)(sizeof vap_style / sizeof vap_style[0])

void r_vapor_begin(void)
{
    vap_n = 0;
}

void r_vapor_add(const r_polypt_t *pts, int npts, float liquid_h, int cls,
                 int light)
{
    if (cls < 0 || cls >= VAP_NSTYLE || vap_style[cls].a == 0) return;
    if (npts < 3 || vap_n >= VAP_MAX) return;

    vapjob_t *j = &vap_jobs[vap_n++];
    j->pts   = pts;
    j->h     = liquid_h + vap_style[cls].hover;
    j->npts  = (uint8_t)npts;
    j->cls   = (uint8_t)cls;
    j->light = (uint8_t)light;
}

/* --- drawing -------------------------------------------------------------
 *
 * The transform and clip mirror the flat pipeline's shapes (the vertex is
 * the same {depth, offset, wx, wy}); only the banding is absent, because
 * noise cannot show the perspective warp banding exists to hide. */

typedef struct { float d, o, wx, wy; } vvtx_t;

#define VAP_CLIP_MAX 40
#define VAP_NEAR     1.0f

static int vap_clip(const vvtx_t *in, int n, vvtx_t *out,
                    float ka, float kb, float kc)
{
    /* One Sutherland-Hodgman pass against ka*d + kb*o + kc >= 0, which
     * expresses the near plane (1,0,-near) and both view sides (k,±1,0). */
    if (n < 3) return 0;
    int m = 0;
    vvtx_t a = in[n - 1];
    float  sa = ka * a.d + kb * a.o + kc;
    for (int i = 0; i < n; i++) {
        const vvtx_t b  = in[i];
        const float  sb = ka * b.d + kb * b.o + kc;
        if (sa >= 0.0f && m < VAP_CLIP_MAX) out[m++] = a;
        if ((sa > 0.0f) != (sb > 0.0f) && m < VAP_CLIP_MAX) {
            const float t = sa / (sa - sb);
            out[m].d  = a.d  + (b.d  - a.d ) * t;
            out[m].o  = a.o  + (b.o  - a.o ) * t;
            out[m].wx = a.wx + (b.wx - a.wx) * t;
            out[m].wy = a.wy + (b.wy - a.wy) * t;
            m++;
        }
        a = b; sa = sb;
    }
    return m;
}

static const rdpq_trifmt_t TRIFMT_VAPOR = {
    .pos_offset   = 0,
    .shade_offset = 2,
    .tex_offset   = 6,
    .tex_tile     = TILE0,
    .z_offset     = 9,
};

void r_vapor_flush(const r_camera_t *cam)
{
    if (!vap_n) return;

    extern int leveltime;
    const float cs = r_view_cs, sn = r_view_sn;
    const float view_k = (SCREEN_W * 0.5f) / cam->focal_x;
    const float cx = SCREEN_W * 0.5f, cy = SCREEN_H * 0.5f;

    /* One mode block for the whole pass: standard cycle, shade-tinted
     * texture alpha through the blender, z probed but never written --
     * the world hides the haze, the haze never hides the world. The IA8
     * noise needs no TLUT; the frame's next consumer rebinds its own
     * lookup state (V_BeginUI), and TMEM below the TLUT is free real
     * estate here. */
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER1((0, 0, 0, SHADE), (TEX0, 0, SHADE, 0)));
    rdpq_mode_blender(RDPQ_BLENDER((IN_RGB, IN_ALPHA, MEMORY_RGB, INV_MUX_ALPHA)));
    rdpq_mode_zbuf(true, false);
    rdpq_mode_filter(FILTER_BILINEAR);
    rdpq_mode_tlut(TLUT_NONE);

    rdpq_texparms_t tp = {0};
    tp.s.repeats = REPEAT_INFINITE;
    tp.t.repeats = REPEAT_INFINITE;
    rdpq_tex_upload(TILE0, &vap_surf, &tp);
    r_tri_group_begin();

    for (int jn = 0; jn < vap_n; jn++) {
        const vapjob_t   *j  = &vap_jobs[jn];
        const vapstyle_t *st = &vap_style[j->cls];
        const float dz = j->h - cam->z;
        if (dz >= 0.0f) continue;          /* at or below eye level only */

        /* Transform, then clip to the view wedge. */
        vvtx_t poly[VAP_CLIP_MAX], clip[VAP_CLIP_MAX];
        int n = j->npts;
        if (n > VAP_CLIP_MAX) n = VAP_CLIP_MAX;
        for (int i = 0; i < n; i++) {
            const float dx = j->pts[i].x - cam->x;
            const float dy = j->pts[i].y - cam->y;
            poly[i].d  = dx * cs + dy * sn;
            poly[i].o  = dx * sn - dy * cs;
            poly[i].wx = j->pts[i].x;
            poly[i].wy = j->pts[i].y;
        }
        n = vap_clip(poly, n, clip, 1.0f, 0.0f, -VAP_NEAR);
        n = vap_clip(clip, n, poly, view_k, -1.0f, 0.0f);
        n = vap_clip(poly, n, clip, view_k, 1.0f, 0.0f);
        if (n < 3) continue;

        /* Three motions stack, all per-frame constants so the per-vertex
         * cost is zero: the linear drift, a slow circular SWIRL over it --
         * the pattern churns in place instead of running like a conveyor,
         * which is what makes the movement legible at a glance -- and a
         * breathing opacity, phased per pool so neighbouring hazes never
         * pulse in step. fm_sinf is libdragon's ~50-tick sine.
         *
         * The phase seed is the pool's own world position, NOT its heap
         * address: zone addresses move whenever code size changes, so a
         * pointer-seeded phase made the haze differ between two builds of
         * the same scene -- poison for A/B pixel comparison. Vertex
         * coordinates come from the WAD and cannot vary per build. */
        /* d_subtic glides the drift between tics; the vapor was the one
         * moving thing in the world stepping at 35 Hz while everything
         * else interpolated. Zero with interpolation off, so INTERP=0
         * builds animate exactly on the tic as before. */
        extern float d_subtic;
        const float t = (float)leveltime + d_subtic;
        const float phase =
            (float)(((int)j->pts[0].x + (int)j->pts[0].y) & 7) * 0.8f;
        const float du = t * st->drift_u
                       + 0.16f * fm_sinf(t * 0.021f + phase);
        const float dv = t * st->drift_v
                       + 0.16f * fm_sinf(t * 0.015f + phase * 1.7f);
        const float breathe = 0.82f + 0.18f * fm_sinf(t * 0.05f + phase);

        /* Rebase the texture coordinates near zero, exactly why the flats
         * carry sorg/torg: world X times the repeat scale times 32 texels
         * overflows the RDP's s10.5 range on a big map. The layer's first
         * vertex anchors the origin; the drift rides in before flooring so
         * motion survives the rebase. */
        const float uorg = rf_floorf(j->pts[0].x * st->uvscale + du);
        const float vorg = rf_floorf(j->pts[0].y * st->uvscale + dv);
        const float rr = (float)st->r * (1.0f / 255.0f);
        const float gg = (float)st->g * (1.0f / 255.0f);
        const float bb = (float)st->b * (1.0f / 255.0f);
        /* The room's own light level, which the layer used to ignore
         * entirely: haze in an unlit cellar was as bright as haze in a
         * floodlit hall, because the style colour went to every corner
         * unchanged. */
        const float amb = (float)j->light * (1.0f / 255.0f);
        const float aa = (float)st->a * (1.0f / 255.0f) * breathe;
        const float finv = r_fog_inv[j->light];
        const float dzf  = dz * cam->focal_y;
        const float vz   = j->h;          /* where the layer hangs, for the light query */

        float sv[VAP_CLIP_MAX][10];
        for (int i = 0; i < n; i++) {
            const float iw = 1.0f / clip[i].d;
            sv[i][0] = cx + clip[i].o * cam->focal_x * iw;
            sv[i][1] = cy - dzf * iw;
            /* Lit per corner, not one flat wash over the whole bank.
             *
             * Every vertex carried the style's colour, so a layer was
             * uniformly bright from end to end whatever was happening
             * underneath it, and read as a painted sheet lying on the
             * pool rather than as something suspended in the room. Each
             * corner now takes the light where it actually is -- the
             * same query the flat below it uses -- so the haze brightens
             * under a torch, and a fireball crossing the pool sweeps a
             * gradient across the smoke instead of leaving it flat. */
            float la[3] = { 0.0f, 0.0f, 0.0f };
            r_light_at_rgb(clip[i].wx, clip[i].wy, vz, la);
            float lr = amb + la[0], lg = amb + la[1], lb = amb + la[2];
            if (lr > 1.0f) lr = 1.0f;
            if (lg > 1.0f) lg = 1.0f;
            if (lb > 1.0f) lb = 1.0f;
            sv[i][2] = rr * lr;
            sv[i][3] = gg * lg;
            sv[i][4] = bb * lb;
            /* The haze thins with the same falloff that dims the world
             * around it, so a distant pool's layer fades with its room. */
            sv[i][5] = aa * r_vis(clip[i].d, finv);
            sv[i][6] = (clip[i].wx * st->uvscale + du - uorg) * (float)VAP_TEX;
            sv[i][7] = (clip[i].wy * st->uvscale + dv - vorg) * (float)VAP_TEX;
            sv[i][8] = iw;
            float zv = 1.0f - R_TRI_NEAR * iw;
            sv[i][9] = zv < 0.0f ? 0.0f : zv;
        }
        r_tri_fan(&TRIFMT_VAPOR, (const float (*)[10])sv, n);
        r_tri_flat += n - 2;
    }

    /* Leave a clean standard state, as every pass here does. */
    rdpq_mode_zbuf(true, true);
    rdpq_mode_blender(0);
}

#endif /* R_VAPOR */
