/* See r_env.h for why this exists instead of the door mirror. */

#include "r_env.h"

#if R_ENVMAP

#include "r_tri.h"
#include "dt64.h"
#include "r_flat.h"   /* the shared depth scale: R_FLAT_NEAR */

#include <libdragon.h>
#include <math.h>

/* --- why this is cheap ---------------------------------------------------
 *
 * An environment map wants texture coordinates from the REFLECTED view
 * direction, which normally costs a normal and a reflect per vertex. Two
 * properties of this renderer collapse that to almost nothing:
 *
 *   1. A door is a VERTICAL plane, so its normal is horizontal and constant
 *      over the whole leaf, and reflection leaves the vertical component of
 *      the view ray untouched.
 *   2. The camera never PITCHES, so screen y maps to view elevation by a
 *      fixed scale and screen x maps to view azimuth by atan((x-cx)/focal_x).
 *
 * Reflecting a direction about a line negates its angle around that line:
 *
 *      theta_reflected = 2*theta_normal - theta_view
 *
 * and theta_view is the camera yaw plus the screen-x term. Over a door's
 * width that term is small, where atan(u) ~ u to well under a texel of a
 * 32-wide map, so the azimuth -- and therefore S -- is AFFINE IN SCREEN X.
 * The elevation, and therefore T, is affine in screen y for the same reason.
 *
 * Affine in screen space is exactly what a flat textured quad already
 * interpolates, so the sheen costs one quad, one small tile, and no
 * per-pixel work at all. It is drawn without perspective correction for the
 * same reason: there is none to correct. */

#define ENV_MAX      16      /* polished walls that can sheen in one frame */
#define ENV_TEX_W    32
#define ENV_TEX_H    32

typedef struct {
    float x1, y1, x2, y2;    /* the leaf, in world space */
    float zbot, ztop;
    uint8_t light;           /* sector light level, 0..255 -- NOT 0..1 */
} envwall_t;

static envwall_t envwall[ENV_MAX];
static int       numenv;

/* The environment itself, built once.
 *
 * A synthesised gradient rather than a baked asset, because what a polished
 * door actually reflects in a Doom room is not a scene -- it is a bright
 * band where the walls meet the ceiling lights, dark above and below. One
 * 32x32 RGBA16 tile is 2 KB, sits inside TMEM beside nothing else this pass
 * needs, and costs no cartridge space or wad2n64 changes. */
static surface_t env_tex;
static bool      env_ready;

static void env_build(void)
{
    env_tex = surface_alloc(FMT_RGBA16, ENV_TEX_W, ENV_TEX_H);
    if (!env_tex.buffer) return;

    for (int y = 0; y < ENV_TEX_H; y++) {
        uint16_t *row = (uint16_t *)((uint8_t *)env_tex.buffer +
                                     (size_t)y * env_tex.stride);
        /* Elevation across the map: 0 at the top (ceiling), 1 at the bottom
         * (floor), with the horizon band at the middle. */
        const float e = ((float)y + 0.5f) / (float)ENV_TEX_H;
        const float d = (e - 0.5f) * 2.0f;          /* -1..1 from the band */
        /* NARROW. At 3.0 the band covered most of the map's height and the
         * door read as uniformly lit -- washed out rather than polished. A
         * highlight is a highlight because most of the surface is NOT it. */
        float band = 1.0f - d * d * 16.0f;
        if (band < 0.0f) band = 0.0f;
        band *= band;                               /* sharpen the peak */

        for (int x = 0; x < ENV_TEX_W; x++) {
            /* A little azimuthal structure so turning past a door reads as
             * something moving over it rather than as a uniform wash. */
            const float a = ((float)x + 0.5f) / (float)ENV_TEX_W;
            /* Squared, so the glints are separated by genuinely dark metal
             * instead of riding on a bright pedestal. */
            float g = 0.5f + 0.5f * cosf(a * 4.0f * (float)M_PI);
            const float glint = 0.12f + 0.88f * g * g * g;
            float v = band * glint;
            /* Barely any floor. The old 0.10 put a tenth of the sheen over
             * EVERY texel of every door, which is most of what read as
             * washed out -- it is the flat term that kills a specular. */
            v = 0.015f + 0.985f * v;
            if (v > 1.0f) v = 1.0f;

            const int c = (int)(v * 31.0f + 0.5f);
            /* Very slightly cool, the way grey steel reads on a CRT. */
            const int r = c > 1 ? c - 1 : c;
            row[x] = (uint16_t)((r << 11) | (c << 6) | (c << 1) | 1);
        }
    }
    data_cache_hit_writeback_invalidate(env_tex.buffer,
                                        (size_t)env_tex.stride * ENV_TEX_H);
    env_ready = true;
}

void r_env_begin(void) { numenv = 0; }

void r_env_wall_add(const r_wall_t *w)
{
    if (numenv >= ENV_MAX || w->ztop <= w->zbot) return;

    /* One leaf arrives once per uncovered column range; keep one sheen, or
     * the blend doubles along the join. Same guard the pool ghosts use. */
    for (int i = 0; i < numenv; i++)
        if (envwall[i].x1 == w->x1 && envwall[i].y1 == w->y1 &&
            envwall[i].x2 == w->x2 && envwall[i].y2 == w->y2) return;

    envwall_t *e = &envwall[numenv++];
    e->x1 = w->x1; e->y1 = w->y1;
    e->x2 = w->x2; e->y2 = w->y2;
    e->zbot = w->zbot; e->ztop = w->ztop;
    e->light = w->light;
}

/* How much of the map reaches the screen. Tuned as a sheen, not a mirror:
 * the door's own texture must still read through it. */
#ifndef R_ENVAMT
#define R_ENVAMT 45                 /* hundredths of full strength */
#endif
#define ENV_ALPHA ((float)R_ENVAMT * 0.01f)

/* Sector light level below which a door gets no sheen at all, and the level
 * at which it reaches full. Doom's own 0..255 units, so both can be read
 * straight off a sector's light.
 *
 * The knee is deliberately LOW -- only a genuinely dark room switches the
 * effect off -- and full is reached well before 255, because ramping all the
 * way to fullbright left every ordinary room at a third strength and the
 * sheen never really arrived anywhere. Off in the dark, on everywhere else,
 * with the ramp just wide enough that it is not a hard edge you can walk
 * across. */
#ifndef R_ENVKNEE
#define R_ENVKNEE 72
#endif
#ifndef R_ENVFULL
#define R_ENVFULL 176
#endif

/* Must match NEAR_PLANE in r_wall.c: the sheen has to be clipped exactly
 * where the leaf it sits on is, or it goes missing where the wall does not. */
#define NEAR_PLANE_ENV R_FLAT_NEAR

/* A tenth in front of the wall's own 4.0 -- see the note in r_env_flush. */
#define ENV_Z_NEAR (R_FLAT_NEAR + 0.1f)

/* The wall pass's parametric clip, kept locally so this file does not reach
 * into r_wall.c's internals. One constraint at a time over the segment's own
 * parameter u in [0,1]; returns false when the range closes. */
static inline bool env_clip_range(float *lo, float *hi, float a, float b)
{
    if (fabsf(b) < 1e-9f) return a >= 0.0f;   /* independent of u */
    const float root = -a / b;
    if (b > 0.0f) { if (root > *lo) *lo = root; }
    else          { if (root < *hi) *hi = root; }
    return *lo < *hi;
}

static inline float env_t_clamp(float t)
{
    if (t < 0.5f) return 0.5f;
    if (t > (float)ENV_TEX_H - 0.5f) return (float)ENV_TEX_H - 0.5f;
    return t;
}

static const rdpq_trifmt_t TRIFMT_ENV = {
    .pos_offset   = 0,
    .shade_offset = 2,
    .tex_offset   = 6,
    .tex_tile     = TILE0,
    .z_offset     = 9,
};

void r_env_flush(const r_camera_t *cam)
{
    if (!numenv) return;
    if (!env_ready) { env_build(); if (!env_ready) { numenv = 0; return; } }

    /* Added over the door, z-tested and never written: the leaf is already
     * in the depth buffer from the wall pass, so the sheen lands on it and
     * anything nearer hides it, with no bias of its own. */
    rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, SHADE, 0), (0, 0, 0, SHADE)));
    rdpq_mode_blender(RDPQ_BLENDER((IN_RGB, IN_ALPHA, MEMORY_RGB, INV_MUX_ALPHA)));
    rdpq_mode_alphacompare(0);
    rdpq_mode_zbuf(true, false);
    rdpq_mode_filter(FILTER_BILINEAR);
    rdpq_mode_persp(false);          /* affine by construction -- see above */
    rdpq_mode_tlut(TLUT_NONE);
    /* A SMALL BIAS, not decal. Decal was the right instinct -- the sheen is
     * genuinely coplanar with the leaf -- but its tolerance is derived from
     * the primitive's depth slope, and walking up to a door makes that slope
     * enormous: the window stopped matching the wall's and the sheen flashed
     * and dropped out at close range.
     *
     * The pool ghosts cannot use a bias, because a mirror lands on pixels
     * other things legitimately occupy and any margin wrongly claims them.
     * A door sheen is different: it is drawn only over the door's own quad,
     * and the door is the nearest thing at those pixels. So the margin only
     * has to beat the leaf itself, and a tenth is three z LSBs at any real
     * distance while claiming under 3% of depth from anything in front. */

    /* S REPEATS. The azimuth runs off both ends of the map and is meant to
     * wrap round the room; uploaded with the default clamp it smeared the
     * edge column across the whole leaf instead. T clamps, because elevation
     * genuinely ends at the ceiling and the floor. */
    rdpq_texparms_t tp = {0};
    tp.s.repeats = REPEAT_INFINITE;
    rdpq_tex_upload(TILE0, &env_tex, &tp);

    const float cs = r_view_cs, sn = r_view_sn;
    const float cx = SCREEN_W * 0.5f, cy = SCREEN_H * 0.5f;
    const float inv2pi = 1.0f / (2.0f * (float)M_PI);

    for (int i = 0; i < numenv; i++) {
        const envwall_t *e = &envwall[i];

        float d1 = (e->x1 - cam->x) * cs + (e->y1 - cam->y) * sn;
        float d2 = (e->x2 - cam->x) * cs + (e->y2 - cam->y) * sn;
        float o1 = (e->x1 - cam->x) * sn - (e->y1 - cam->y) * cs;
        float o2 = (e->x2 - cam->x) * sn - (e->y2 - cam->y) * cs;

        /* CLIPPED TO THE VIEW WEDGE, not merely to the near plane.
         *
         * Near-clipping alone bounds depth but leaves the projected X
         * unbounded: a leaf a few units away throws its corners tens of
         * thousands of pixels sideways, and the quad then spans most of the
         * screen. Its z is only meaningful over the door, so across the rest
         * it interpolates to whatever and passes the depth test wherever it
         * happens to land -- and because r_tri_quad splits the quad into two
         * triangles, the two halves disagree and the join shows as a hard
         * DIAGONAL across the picture. That is the reported artifact.
         *
         * So the same three-plane parametric clip the wall pass uses: near,
         * left and right, solved over the segment's own parameter and the
         * endpoints rebuilt from it, which keeps d and o consistent so S, T
         * and z all follow without special cases. */
        const float NEARP = NEAR_PLANE_ENV;
        {
            const float kl = (0.0f - cx) / cam->focal_x;
            const float kr = ((float)SCREEN_W - cx) / cam->focal_x;
            const float dd = d2 - d1, od = o2 - o1;
            float ua = 0.0f, ub = 1.0f;
            if (!env_clip_range(&ua, &ub, d1 - NEARP, dd))        continue;
            if (!env_clip_range(&ua, &ub, o1 - d1 * kl, od - dd * kl)) continue;
            if (!env_clip_range(&ua, &ub, d1 * kr - o1, dd * kr - od)) continue;

            const float nd1 = d1 + dd * ua, no1 = o1 + od * ua;
            const float nd2 = d1 + dd * ub, no2 = o1 + od * ub;
            d1 = nd1; o1 = no1; d2 = nd2; o2 = no2;
        }
        if (d1 < NEARP || d2 < NEARP) continue;

        /* Either projected order is legitimate -- which end of a leaf lands
         * left on screen depends on the side it is seen from, and rejecting
         * one of them silently dropped half the doors in the level. Swap
         * into left-to-right instead, carrying the depths with the ends so
         * the s and z terms below stay attached to the right corner. */
        if (o1 * d2 > o2 * d1) {
            float t;
            t = d1; d1 = d2; d2 = t;
            t = o1; o1 = o2; o2 = t;
        }

        const float iw1 = 1.0f / d1, iw2 = 1.0f / d2;
        const float xl = cx + o1 * cam->focal_x * iw1;
        const float xr = cx + o2 * cam->focal_x * iw2;
        if (xr - xl < 0.5f) continue;              /* edge-on: nothing to light */
        if (xr < 0.0f || xl > (float)SCREEN_W) continue;

        /* CLAMPED. A leaf near the near plane projects its corners hundreds
         * of thousands of pixels off screen, and the RDP's screen Y is s11.2
         * -- past a couple of thousand it WRAPS, and the quad reappears
         * across the middle of the picture instead of clipping away. ENV_T
         * below is a pure affine function of y, so taking it from the
         * clamped value stays exact. */
        #define ENV_Y_GUARD 1536.0f
        #define ENV_YC(v) ((v) < -ENV_Y_GUARD ? -ENV_Y_GUARD \
                                              : ((v) > ENV_Y_GUARD ? ENV_Y_GUARD : (v)))
        const float yt1 = ENV_YC(cy - (e->ztop - cam->z) * cam->focal_y * iw1);
        const float yb1 = ENV_YC(cy - (e->zbot - cam->z) * cam->focal_y * iw1);
        const float yt2 = ENV_YC(cy - (e->ztop - cam->z) * cam->focal_y * iw2);
        const float yb2 = ENV_YC(cy - (e->zbot - cam->z) * cam->focal_y * iw2);

        /* theta_reflected = 2*theta_normal - theta_view. The normal's angle
         * is constant over the leaf; the view term is the camera yaw plus
         * the screen-x offset, linearised as described at the top. */
        const float ex = e->x2 - e->x1, ey = e->y2 - e->y1;
        const float th_n = atan2f(-ex, ey);        /* normal, not the edge */
        /* Wrapped into one turn before it becomes a texture coordinate. The
         * camera yaw is unbounded, so 2*th_n - angle can be arbitrarily
         * large -- and the RDP's S is s10.5, which loses its fraction and
         * then overflows outright long before that. Wrapping the ANGLE (not
         * the final S) keeps both ends of a leaf on the same turn, so the
         * quad never straddles a seam. */
        float base = 2.0f * th_n - cam->angle;
        base -= floorf(base * inv2pi) * (2.0f * (float)M_PI);

        /* PLUS the screen-x term. In this basis screen x SUBTRACTS from the
         * view azimuth, so the reflected azimuth (2*theta_n - theta_view)
         * adds it back. With the sign wrong the map swept at twice the yaw
         * rate instead of staying pinned to the wall. */
        const float sl = (base + (xl - cx) / cam->focal_x) * inv2pi
                       * (float)ENV_TEX_W;
        const float sr = (base + (xr - cx) / cam->focal_x) * inv2pi
                       * (float)ENV_TEX_W;

        /* Elevation to T. Half a map per focal length puts the horizon band
         * on the door at eye height, which is where a real one sits. */
        /* Clamped to the map: a tall leaf close to the eye projects well past
         * both ends, and an out-of-range T on a clamped tile is a stretched
         * edge row rather than the band it should be showing. */
        #define ENV_T(yy) env_t_clamp((0.5f + ((yy) - cy) / \
                                       (cam->focal_y * 2.0f)) * (float)ENV_TEX_H)
        const float tt1 = ENV_T(yt1), tb1 = ENV_T(yb1);
        const float tt2 = ENV_T(yt2), tb2 = ENV_T(yb2);

        /* ONLY IN BRIGHT ROOMS, and with a knee rather than a slope.
         *
         * This used to read `e->light < 1.0f ? e->light : 1.0f`, which was
         * simply wrong: r_wall_t's light is 0..255, not 0..1, so every room
         * above light level 1 clamped to full strength. The sheen never rode
         * the lighting at all -- it was at maximum in pitch-dark corridors.
         *
         * A plain proportional scale would not be right either. Polished
         * metal catches a room that has something to catch; a dim room does
         * not give a weak highlight so much as none, and a faint sheen in
         * the dark reads as a rendering error rather than as a material. So
         * it is zero below the knee and ramps to full over what remains. */
        const float lv = (float)e->light;
        if (lv <= (float)R_ENVKNEE) continue;
        float lit = (lv - (float)R_ENVKNEE) /
                    (float)(R_ENVFULL - R_ENVKNEE);
        if (lit > 1.0f) lit = 1.0f;

        const float a = ENV_ALPHA * lit;
        if (a <= 0.0f) continue;

        float v[4][10];
        const float xs[4] = { xl,  xr,  xr,  xl  };
        const float ys[4] = { yt1, yt2, yb2, yb1 };
        const float ss[4] = { sl,  sr,  sr,  sl  };
        const float ts[4] = { tt1, tt2, tb2, tb1 };
        for (int k = 0; k < 4; k++) {
            v[k][0] = xs[k];
            v[k][1] = ys[k];
            v[k][2] = v[k][3] = v[k][4] = 1.0f;
            v[k][5] = a;
            v[k][6] = ss[k];
            v[k][7] = ts[k];
            v[k][8] = 1.0f;      /* affine: one 1/w for the whole quad */
            /* Coplanar with the leaf the wall pass already wrote, so the
             * decal z-mode the reflections use would suit it -- but the
             * sheen is additive and never written, and the leaf is the
             * nearest thing at these pixels anyway. Standard test, at the
             * leaf's own depth. */
            v[k][9] = 1.0f - ENV_Z_NEAR * iw1;
        }
        /* Depth per side, so a door seen at an angle is tested along its
         * real slant rather than at one end's depth. */
        v[1][9] = 1.0f - ENV_Z_NEAR * iw2;
        v[2][9] = 1.0f - ENV_Z_NEAR * iw2;

        r_tri_quad(&TRIFMT_ENV, v[0], v[1], v[2], v[3]);
        r_tri_spr += 2;
    }

    /* Perspective back on: modes persist and the backdrop is not affine. */
    rdpq_mode_persp(true);
    numenv = 0;
}

#endif /* R_ENVMAP */
