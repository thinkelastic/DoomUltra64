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

/* Boot-time only, so clarity beats cleverness. */
static inline float rf_sqrtf_approx(float v) { return sqrtf(v); }

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

            /* A disc with a softened core, not a ring.
             *
             * This was a true annulus -- zero at the centre -- to keep
             * any blend off the source's own pixels while the blender
             * was still darkening what it touched. With that fixed the
             * hole is no longer needed, and it was WIDER than the sprite
             * it protected: a fireball showed a visible gap between the
             * ball and its glow. The core is now merely damped rather
             * than empty, so the glow runs continuously outward from the
             * sprite's edge, and the falloff still carries most of the
             * light into the air around it where it belongs. */
            const float r = rf_sqrtf_approx(r2);
            const float t = 1.0f - r;
            float fall = t * t;                        /* 1 centre .. 0 rim */
            const float damp = r < 0.30f
                             ? 0.62f + 0.38f * (r * (1.0f / 0.30f))
                             : 1.0f;
            fall *= damp;
            if (fall < 0.0f) fall = 0.0f;
            int a = (int)(fall * 15.0f + 0.5f);
            if (a > 15) a = 15;
            halo_texels[y * HALO_TEX + x] = (uint8_t)(0xF0 | a);

            /* SHAFT: a column, not a blob. Alpha is flat across the
             * width apart from a soften at each side so the edge is not
             * a hard line, and falls from full at the opening to nothing
             * at the floor. Square-sided the whole way down -- a ray,
             * which is what light through a hole in a roof looks like;
             * the earlier radial profile bulged and read as a cloud. */
            const float ax = dx < 0.0f ? -dx : dx;     /* 0 centre, 1 side */
            float edge = ax > 0.84f ? (1.0f - ax) * (1.0f / 0.16f) : 1.0f;
            if (edge < 0.0f) edge = 0.0f;
            const float vy = 1.0f - ((float)y / (float)(HALO_TEX - 1));
            float s = edge * (0.12f + 0.88f * vy);
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

    /* Slightly NEARER than the thing it belongs to. A fireball's sprite
     * is drawn first and writes depth at exactly this distance, so a
     * halo sharing that depth z-fights with the very object it wraps --
     * a speckled, ragged core instead of a glow. The bias is
     * depth-proportional, folded into the near constant, for the same
     * reason the reflections' is: a constant in z-space is hundreds of
     * world units out at range and none up close. A larger near
     * constant means a smaller z, and smaller is nearer. */
    float z = 1.0f - (R_FLAT_NEAR + 0.6f) * iw;
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

/* Shared mode block.
 *
 * The blender is the plain lerp the vapor and the reflections already
 * use, NOT the additive form this started with. The RDP's blender is
 * not an adder: it computes (P*A + M*B) / (A + B), so a memory factor
 * of ONE does not add light, it averages the pixel with the halo's own
 * colour -- and over anything brighter than that colour the result is
 * DARKER than what was there. On a fireball's white-hot core, where the
 * halo's alpha peaks, that read as the centre saturating to black. It
 * dirtied the sky the same way; drawing shafts behind the sky hid that
 * symptom without addressing this cause.
 *
 * With INV_MUX_ALPHA the factors sum to one, the divide is a no-op, and
 * the result is an honest mix. Z-tested, never written. */
static void halo_mode(void)
{
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER1((0, 0, 0, SHADE), (TEX0, 0, SHADE, 0)));
    rdpq_mode_blender(RDPQ_BLENDER((IN_RGB, IN_ALPHA, MEMORY_RGB, INV_MUX_ALPHA)));
    rdpq_mode_alphacompare(0);
    rdpq_mode_zbuf(true, false);
    rdpq_mode_filter(FILTER_BILINEAR);
    rdpq_mode_tlut(TLUT_NONE);
}

/* Shafts draw BEFORE the sky, and that placement is the whole fix for
 * the smear the first version left across the backdrop: the sky pass
 * tests depth but never writes it, so a beam drawn afterwards passed
 * the test everywhere sky showed and hung over it. Drawn first, the sky
 * covers exactly the part that is behind it and the beam survives only
 * where it belongs -- falling through the room below the opening. */
void r_shaft_flush(const r_camera_t *cam)
{
    if (!num_shafts) return;

    halo_mode();
    rdpq_texparms_t tp = {0};
    rdpq_tex_upload(TILE0, &shaft_surf, &tp);
    r_tri_group_begin();

    for (int i = 0; i < num_shafts; i++) {
        const shaftjob_t *s = &shafts[i];
        /* Daylight through a hole: warm white, scaled by how bright the
         * sector under it actually is. The texture carries the fall from
         * the opening to the floor; the vertex alpha sets its scale. */
        const float k = 0.42f * s->lum;
        billboard(cam, s->cx, s->cy, s->half_w, s->top, s->bot,
                  1.00f, 0.97f, 0.86f, k, 0.0f);
    }
    num_shafts = 0;
}

void r_halo_flush(const r_camera_t *cam)
{
    const int nlights = r_light_count();
    if (!nlights) return;

    halo_mode();

    /* Halos: one per live light, in its own colour. */
    {
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
            /* Carried toward white rather than used as the raw hue. The
             * blender mixes toward this colour, so a saturated one pulls
             * the channels it lacks DOWNWARDS -- a red light over grey
             * stone dimmed the green and blue instead of adding red, and
             * the pixel came out darker than it started. Glow is bright
             * first and coloured second; mixing toward white makes the
             * blend brighten every channel and merely tint the hue. */
            const float hr = l->ir * m, hg = l->ig * m, hb = l->ib * m;
            const float r = 0.45f + 0.55f * hr;
            const float g = 0.45f + 0.55f * hg;
            const float b = 0.45f + 0.55f * hb;

            /* Half the light's reach: the glow in the air is tighter than
             * the light it throws on the walls, which is what keeps a
             * muzzle flash a flash rather than a fog bank. Alpha rides
             * the light's own intensity, capped so a barrel's 1.3 does
             * not blow the screen out. */
            float a = 0.30f * l->intensity;
            if (a > 0.42f) a = 0.42f;

            /* Half the reach, times the light's own halo fraction --
             * a half again for anything that flies or detonates. */
            const float half_h = radius * 0.5f * r_light_halo[i];
            billboard(cam, l->x, l->y, half_h,
                      l->z + half_h, l->z - half_h, r, g, b, a, a);
        }
    }

}

#endif /* R_HALO */
