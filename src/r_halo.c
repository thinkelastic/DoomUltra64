/* Halos and light shafts. See r_halo.h for the design. */

#include "r_halo.h"

#if R_HALO

#include "r_fastmath.h"
#include "r_flat.h"          /* R_FLAT_NEAR: the shared depth curve */
#include "r_light.h"
#include "r_tri.h"

#include <libdragon.h>
#include <math.h>
#include <string.h>

/* --- the two textures ----------------------------------------------------
 *
 * IA8, 32x32, baked at boot like the vapor noise. Intensity is flat and
 * high in both; all the SHAPE is in the alpha, because the colour comes
 * from the vertex shade -- a halo takes its light's own hue, a shaft the
 * sector's. Bilinear filtering smooths the 32-pixel grid into something
 * that reads as light rather than as a sprite.
 */
#define HALO_TEX 32

static uint8_t   halo_texels[HALO_TEX * HALO_TEX] __attribute__((aligned(16)));
static uint8_t   shaft_texels[HALO_TEX * HALO_TEX] __attribute__((aligned(16)));
static surface_t halo_surf, shaft_surf;

void r_halo_init(void)
{
    const float c = (HALO_TEX - 1) * 0.5f;

    for (int y = 0; y < HALO_TEX; y++)
        for (int x = 0; x < HALO_TEX; x++) {
            /* HALO: radial, falling off as a smoothstep on the squared
             * radius. Squared keeps it soft at the centre and quick at
             * the rim, which is what a glow around a point source looks
             * like; the edge reaches exactly zero so the quad's border
             * never shows. */
            const float dx = ((float)x - c) / c;
            const float dy = ((float)y - c) / c;
            float r2 = dx * dx + dy * dy;
            if (r2 > 1.0f) r2 = 1.0f;
            const float t = 1.0f - r2;
            const float fall = t * t;                  /* 0 at rim, 1 centre */
            int a = (int)(fall * 15.0f + 0.5f);
            if (a > 15) a = 15;
            halo_texels[y * HALO_TEX + x] = (uint8_t)(0xF0 | a);

            /* SHAFT: soft at the left and right edges, and fading from
             * top to bottom -- the light thins as it falls away from the
             * opening. The horizontal profile is the same smooth curve
             * so the beam has no hard sides. */
            const float hx = 1.0f - dx * dx;           /* 0 at sides */
            const float vy = 1.0f - ((float)y / (float)(HALO_TEX - 1));
            float s = hx * hx * (0.25f + 0.75f * vy);
            if (s < 0.0f) s = 0.0f;
            int sa = (int)(s * 15.0f + 0.5f);
            if (sa > 15) sa = 15;
            shaft_texels[y * HALO_TEX + x] = (uint8_t)(0xF0 | sa);
        }

    halo_surf  = surface_make(halo_texels,  FMT_IA8, HALO_TEX, HALO_TEX, HALO_TEX);
    shaft_surf = surface_make(shaft_texels, FMT_IA8, HALO_TEX, HALO_TEX, HALO_TEX);
    data_cache_hit_writeback(halo_texels,  sizeof halo_texels);
    data_cache_hit_writeback(shaft_texels, sizeof shaft_texels);
}

/* --- the shaft queue ----------------------------------------------------- */

/* A view rarely holds more than two or three wells; this is generous. */
#define SHAFT_MAX 8

/* Plan size above which a sky ceiling is open sky rather than a hole in a
 * roof. 512 units is four of Doom's 128-unit ceiling slabs across -- a
 * light well, a stairwell, a courtyard shaft. E1M1's outdoor yard is many
 * times this and is rejected, which is the point: a beam hanging in open
 * daylight looks wrong. */
#define SHAFT_MAX_SPAN 512.0f

/* Sector light below which a well is not lit enough to throw a beam. */
#define SHAFT_MIN_LIGHT 96

typedef struct {
    float cx, cy;        /* centre of the opening, in plan */
    float half_w;        /* half its smaller span: the beam's radius */
    float top, bot;      /* ceiling and floor of the well */
    float lum;           /* sector light, 0..1 */
} shaftjob_t;

static shaftjob_t shafts[SHAFT_MAX];
static int        num_shafts;

void r_halo_begin(void) { num_shafts = 0; }

void r_shaft_add(const r_polypt_t *pts, int npts,
                 float floor_h, float ceil_h, int lightlevel)
{
    if (npts < 3 || num_shafts >= SHAFT_MAX) return;
    if (ceil_h - floor_h < 32.0f) return;          /* no room to fall */
    if (lightlevel < SHAFT_MIN_LIGHT) return;      /* a dark hole, not a beam */

    float x0 = pts[0].x, x1 = pts[0].x;
    float y0 = pts[0].y, y1 = pts[0].y;
    for (int i = 1; i < npts; i++) {
        if (pts[i].x < x0) x0 = pts[i].x;
        if (pts[i].x > x1) x1 = pts[i].x;
        if (pts[i].y < y0) y0 = pts[i].y;
        if (pts[i].y > y1) y1 = pts[i].y;
    }
    const float sx = x1 - x0, sy = y1 - y0;
    const float span = sx > sy ? sx : sy;
    if (span > SHAFT_MAX_SPAN) return;             /* open sky, not a well */

    /* One shaft per well, not per subsector: a well split across several
     * subsectors would otherwise stack beams and wash out. Merge into an
     * existing shaft whose centre is within its own radius. */
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const float half = (sx < sy ? sx : sy) * 0.5f;
    for (int i = 0; i < num_shafts; i++) {
        const float ddx = shafts[i].cx - cx, ddy = shafts[i].cy - cy;
        const float reach = shafts[i].half_w + half;
        if (ddx * ddx + ddy * ddy < reach * reach) {
            if (ceil_h  > shafts[i].top) shafts[i].top = ceil_h;
            if (floor_h < shafts[i].bot) shafts[i].bot = floor_h;
            return;
        }
    }

    shaftjob_t *s = &shafts[num_shafts++];
    s->cx = cx; s->cy = cy;
    s->half_w = half < 24.0f ? 24.0f : half;
    s->top = ceil_h;
    s->bot = floor_h;
    s->lum = (float)lightlevel * (1.0f / 255.0f);
}

/* --- drawing ------------------------------------------------------------- */

static const rdpq_trifmt_t TRIFMT_HALO = {
    .pos_offset   = 0,
    .shade_offset = 2,
    .tex_offset   = 6,
    .tex_tile     = TILE0,
    .z_offset     = 9,
};

/* One view-facing quad. The camera never pitches, so "facing the viewer"
 * is a rotation about the vertical axis only: the quad's horizontal axis
 * is the camera's right vector, and its vertical axis is world up. Both
 * effects are that shape; only the corner heights and the shade differ.
 *
 * Depth is the billboard's own, mapped on the curve walls and flats
 * share, so the world occludes it exactly. Never z-written: two halos
 * overlapping must both contribute.
 */
static void billboard(const r_camera_t *cam, float wx, float wy,
                      float half_w, float z_top, float z_bot,
                      float r, float g, float b, float a_top, float a_bot)
{
    const float cs = r_view_cs, sn = r_view_sn;
    const float dx = wx - cam->x, dy = wy - cam->y;
    const float depth = dx * cs + dy * sn;
    if (depth < 12.0f) return;                     /* behind or on the eye */

    const float offs = dx * sn - dy * cs;
    const float iw = 1.0f / depth;
    const float sx = SCREEN_W * 0.5f + offs * cam->focal_x * iw;
    const float wpx = half_w * cam->focal_x * iw;  /* half width in pixels */
    if (sx + wpx < 0.0f || sx - wpx > (float)SCREEN_W) return;

    const float cy = SCREEN_H * 0.5f;
    const float y_top = cy - (z_top - cam->z) * cam->focal_y * iw;
    const float y_bot = cy - (z_bot - cam->z) * cam->focal_y * iw;
    if (y_bot < 0.0f || y_top > (float)SCREEN_H) return;

    float z = 1.0f - R_FLAT_NEAR * iw;
    if (z < 0.0f) z = 0.0f;

    const float xs[4] = { sx - wpx, sx + wpx, sx + wpx, sx - wpx };
    const float ys[4] = { y_top, y_top, y_bot, y_bot };
    const float ss[4] = { 0.0f, (float)HALO_TEX, (float)HALO_TEX, 0.0f };
    const float ts[4] = { 0.0f, 0.0f, (float)HALO_TEX, (float)HALO_TEX };
    const float as[4] = { a_top, a_top, a_bot, a_bot };

    float v[4][10];
    for (int i = 0; i < 4; i++) {
        v[i][0] = xs[i]; v[i][1] = ys[i];
        v[i][2] = r; v[i][3] = g; v[i][4] = b;
        v[i][5] = as[i];
        v[i][6] = ss[i]; v[i][7] = ts[i];
        v[i][8] = iw;
        v[i][9] = z;
    }
    r_tri_quad(&TRIFMT_HALO, v[0], v[1], v[2], v[3]);
    r_tri_spr += 2;
}

void r_halo_flush(const r_camera_t *cam)
{
    const int nlights = r_light_count();
    if (!num_shafts && !nlights) return;

    /* Texture alpha times vertex alpha, added over what is already there
     * rather than mixed with it: light adds to a scene, it does not
     * replace it, and adding keeps two overlapping glows brighter
     * instead of averaging them back down. Z-tested, never written. */
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER1((0, 0, 0, SHADE), (TEX0, 0, SHADE, 0)));
    rdpq_mode_blender(RDPQ_BLENDER((IN_RGB, IN_ALPHA, MEMORY_RGB, ONE)));
    rdpq_mode_alphacompare(0);
    rdpq_mode_zbuf(true, false);
    rdpq_mode_filter(FILTER_BILINEAR);
    rdpq_mode_tlut(TLUT_NONE);

    /* Shafts first: they are the larger, dimmer thing, and a halo inside
     * one should add on top of it. */
    if (num_shafts) {
        rdpq_texparms_t tp = {0};
        rdpq_tex_upload(TILE0, &shaft_surf, &tp);
        r_tri_group_begin();

        for (int i = 0; i < num_shafts; i++) {
            const shaftjob_t *s = &shafts[i];
            /* Daylight through a hole: warm white, scaled by how bright
             * the sector under it actually is. Strongest at the opening,
             * gone by the floor -- the texture carries that gradient, and
             * the vertex alpha sets its scale. */
            const float k = 0.42f * s->lum;
            billboard(cam, s->cx, s->cy, s->half_w, s->top, s->bot,
                      1.00f, 0.97f, 0.86f, k, 0.0f);
        }
    }

    /* Halos: one per live light, in its own colour. */
    if (nlights) {
        rdpq_texparms_t tp = {0};
        rdpq_tex_upload(TILE0, &halo_surf, &tp);
        r_tri_group_begin();

        for (int i = 0; i < nlights; i++) {
            const r_light_t *l = &r_lights[i];
            /* The registry stores 1/r^2 and premultiplied channels; the
             * halo wants the radius back, and the hue with its brightest
             * channel at full so a dim light still shows its colour. */
            /* sqrt.s is a native VR4300 instruction; no libm here. */
            const float radius = 1.0f / sqrtf(l->inv_r2);
            const float m = l->intensity > 0.0f ? 1.0f / l->intensity : 0.0f;
            const float r = l->ir * m, g = l->ig * m, b = l->ib * m;

            /* Half the light's reach: the glow in the air is tighter than
             * the light it throws on the walls, which is what keeps a
             * muzzle flash a flash rather than a fog bank. Alpha rides
             * the light's own intensity, capped so a barrel's 1.3 does
             * not blow the screen out. */
            float a = 0.30f * l->intensity;
            if (a > 0.42f) a = 0.42f;

            const float half_h = radius * 0.5f;
            billboard(cam, l->x, l->y, half_h,
                      l->z + half_h, l->z - half_h, r, g, b, a, a);
        }
    }

    num_shafts = 0;
}

#endif /* R_HALO */
