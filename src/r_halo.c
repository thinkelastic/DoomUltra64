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
            /* Fade IN at the head as well as out at the foot. The beam
             * used to reach full strength on its very first row, which
             * drew a hard horizontal line across the top and read as a
             * pasted card rather than as light entering the room -- the
             * more so wherever the geometry starts the beam below the
             * ceiling it comes through. Light arriving through a hole has
             * no edge; it accumulates over the first stretch of air. */
            const float head = vy > 0.82f ? (1.0f - vy) * (1.0f / 0.18f)
                                          : 1.0f;
            float s = edge * head * (0.12f + 0.88f * vy);
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

/* How much of the opening's outline the beam keeps. A sky well is a
 * subsector polygon and those are convex but not always tidy; eight is
 * more corners than any real skylight has and bounds the per-shaft work.
 * A well with more is kept as its bounding rectangle, which is what a
 * merged multi-subsector well degrades to anyway. */
#define SHAFT_PTS_MAX 8

typedef struct {
    float cx, cy;        /* centre of the opening, in plan */
    float half_w;        /* half its smaller span: the merge radius */
    float top, bot;      /* ceiling and floor of the well */
    float lum;           /* sector light, 0..1 */
    /* The opening's outline in plan, which is what the beam is extruded
     * from. Wound as the source polygon was; the draw works out which way
     * that is from the signed area. */
    float px[SHAFT_PTS_MAX], py[SHAFT_PTS_MAX];
    int   npts;
} shaftjob_t;

static shaftjob_t shafts[SHAFT_MAX];
static int        num_shafts;

void r_halo_begin(void) { num_shafts = 0; }

/* Replace an outline with its own bounding rectangle, wound the same way
 * whatever came in. Used when a well is too many-cornered to carry and
 * when two subsectors merge into one well -- in both cases the true
 * outline is either unavailable or is a union this pass will not compute,
 * and a box is the honest approximation: still the opening's shape and
 * still a single non-overlapping silhouette. */
/* An outline's bounding box, for the merge test. */
static void shaft_bbox(const shaftjob_t *s, float *x0, float *y0,
                       float *x1, float *y1)
{
    *x0 = *x1 = s->px[0];
    *y0 = *y1 = s->py[0];
    for (int k = 1; k < s->npts; k++) {
        if (s->px[k] < *x0) *x0 = s->px[k];
        if (s->px[k] > *x1) *x1 = s->px[k];
        if (s->py[k] < *y0) *y0 = s->py[k];
        if (s->py[k] > *y1) *y1 = s->py[k];
    }
}

static void shaft_boxify(shaftjob_t *s, float x0, float y0,
                         float x1, float y1)
{
    s->px[0] = x0; s->py[0] = y0;
    s->px[1] = x1; s->py[1] = y0;
    s->px[2] = x1; s->py[2] = y1;
    s->px[3] = x0; s->py[3] = y1;
    s->npts = 4;
}

void r_shaft_add(const r_polypt_t *pts, int npts,
                 float floor_h, float ceil_h, int lightlevel)
{
#if !R_SHAFT
    (void)pts; (void)npts; (void)floor_h; (void)ceil_h; (void)lightlevel;
    return;
#else
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

    /* One shaft per well, not per subsector.
     *
     * A ceiling arrives here in pieces -- the BSP splits one hole into
     * several subsector regions -- and they must become a single beam.
     * Stacking is what must not happen: the blender composites each over
     * the last and the overlap reads as a brighter core the opening does
     * not have.
     *
     * Merge on the BOXES TOUCHING. The test used to compare centre
     * distance against half the SMALLER span of each piece, and that is
     * exactly wrong for the shape the BSP produces: a long thin slice has
     * a tiny smaller-span, so two neighbouring slices of one hole sat
     * further apart than the test could reach and never merged. A 256-unit
     * well then lit a beam over one 98-unit strip of itself -- a narrow
     * slab that did not span its own opening, which is what made it read
     * as hanging in front of the hole rather than falling through it.
     * Regions of one ceiling abut exactly, so a few units of slack is all
     * the test needs; and two DIFFERENT wells cannot touch, because sky on
     * both sides of a line disqualifies both sectors (D_BuildSkyWells). */
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const float half = (sx < sy ? sx : sy) * 0.5f;
    const float GAP = 8.0f;
    for (int i = 0; i < num_shafts; i++) {
        shaftjob_t *m = &shafts[i];
        float bx0, by0, bx1, by1;
        shaft_bbox(m, &bx0, &by0, &bx1, &by1);
        if (x0 > bx1 + GAP || x1 < bx0 - GAP ||
            y0 > by1 + GAP || y1 < by0 - GAP) continue;

        float ux0 = x0 < bx0 ? x0 : bx0, ux1 = x1 > bx1 ? x1 : bx1;
        float uy0 = y0 < by0 ? y0 : by0, uy1 = y1 > by1 ? y1 : by1;
        /* Never let merging grow a well into open daylight. */
        const float uw = ux1 - ux0, uh = uy1 - uy0;
        if ((uw > uh ? uw : uh) > SHAFT_MAX_SPAN) return;

        if (ceil_h  > m->top) m->top = ceil_h;
        if (floor_h < m->bot) m->bot = floor_h;
        shaft_boxify(m, ux0, uy0, ux1, uy1);
        m->cx = (ux0 + ux1) * 0.5f;
        m->cy = (uy0 + uy1) * 0.5f;
        m->half_w = (uw < uh ? uw : uh) * 0.5f;
        if (m->half_w < 24.0f) m->half_w = 24.0f;
        return;
    }

    shaftjob_t *s = &shafts[num_shafts++];
    s->cx = cx; s->cy = cy;
    s->half_w = half < 24.0f ? 24.0f : half;
    s->top = ceil_h;
    s->bot = floor_h;
    s->lum = (float)lightlevel * (1.0f / 255.0f);
    if (npts <= SHAFT_PTS_MAX) {
        for (int i = 0; i < npts; i++) { s->px[i] = pts[i].x;
                                         s->py[i] = pts[i].y; }
        s->npts = npts;
    } else {
        shaft_boxify(s, x0, y0, x1, y1);
    }
#endif /* R_SHAFT */
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
/* One beam, as the SHAPE OF ITS OPENING rather than a fixed-width slab.
 *
 * The beam is the opening's plan outline extruded from ceiling to floor:
 * a vertical prism. For a camera that never pitches, that prism's screen
 * silhouette is easy to say exactly. Take the edges of the outline that
 * face the camera -- the near chain. Each one is a vertical wall of the
 * prism, and its projection is a quad whose top and bottom edges come
 * from projecting the ceiling and the floor at each END's own depth. The
 * chain of those quads IS the silhouette: its top edge rises and falls
 * with the opening's perspective instead of sitting flat, which is what
 * makes a long skylight read as long rather than as a post.
 *
 * Why the near chain and nothing else, and why that is the whole answer
 * to overlap: the outline is convex, so its near chain projects
 * monotonically across the screen and consecutive quads meet exactly at
 * a shared projected vertex -- they tile the silhouette, edge to edge,
 * covering every pixel once. Any pixel covered twice would be composited
 * twice by the lerp blender and show as a bright seam, which is the one
 * artifact a beam of light cannot have. Drawing the far chain as well, or
 * slabs on a fixed screen grid, would both do exactly that.
 *
 * The texture spans the whole silhouette rather than repeating per quad,
 * for the same reason: its soft side edges must appear once, at the two
 * outer ends, not at every internal seam.
 */
static void shaft_prism(const r_camera_t *cam, const shaftjob_t *s, float k)
{
    const float cs = r_view_cs, sn = r_view_sn;
    const float cxs = SCREEN_W * 0.5f, cys = SCREEN_H * 0.5f;
    const int n = s->npts;
    const float NEARP = 12.0f;

    /* Winding, so "the eye is inside" has a sign. */
    float area = 0.0f;
    for (int i = 0; i < n; i++) {
        const int j = i + 1 < n ? i + 1 : 0;
        area += s->px[i] * s->py[j] - s->px[j] * s->py[i];
    }
    const float wind = area >= 0.0f ? 1.0f : -1.0f;

    /* Project every corner once. */
    float depth[SHAFT_PTS_MAX], sx[SHAFT_PTS_MAX];
    float dmin = 1e9f, dmax = -1e9f;
    for (int i = 0; i < n; i++) {
        const float dx = s->px[i] - cam->x, dy = s->py[i] - cam->y;
        depth[i] = dx * cs + dy * sn;
        sx[i]    = dx * sn - dy * cs;
        if (depth[i] < dmin) dmin = depth[i];
        if (depth[i] > dmax) dmax = depth[i];
    }

    /* Standing IN the well there is no silhouette to trace, and nothing
     * sensible to draw from inside a column of light. */
    {
        int inside = 1;
        for (int i = 0; i < n && inside; i++) {
            const int j = i + 1 < n ? i + 1 : 0;
            const float ex = s->px[j] - s->px[i], ey = s->py[j] - s->py[i];
            const float rx = cam->x - s->px[i],   ry = cam->y - s->py[i];
            if ((ex * ry - ey * rx) * wind < 0.0f) inside = 0;
        }
        if (inside) return;
    }

    /* THE WHOLE FALL OF LIGHT, not a slice of it.
     *
     * A well is a volume: the same plan shape at the ceiling and on the
     * floor, with the light between. Two earlier tries drew a single
     * surface of it -- the rim nearest the eye, which put the fall a
     * quarter of a room in front of the opening, and then the cut at mid
     * depth, which is worse: a lone quad hanging in the middle of the
     * shaft with air on both sides, reading as a square panel rather than
     * as a column of light.
     *
     * What the eye should see is the volume's whole screen footprint. For
     * a plan polygon extruded straight down, that footprint is bounded by
     * the widest projection of the outline (every corner, each at its own
     * depth), by the opening above, and by the pool of light on the floor
     * below -- and the floor pool is the part that gives the beam its
     * height, because a 257-deep footprint of lit floor covers a lot of
     * screen. One quad over that footprint fills the shaft, and being one
     * quad it cannot composite over itself anywhere.
     *
     * The top is the opening's LOWER rim: the rim projects over a range,
     * and the light is only in the room once it is past all of it. The
     * bottom is the NEAREST floor, which is the lowest the pool reaches. */
    float X0 = 1e9f, X1 = -1e9f;
    float y_top = -1e9f, y_bot = -1e9f;
    int seen = 0;
    for (int i = 0; i < n; i++) {
        if (depth[i] < NEARP) continue;
        const float iwi = 1.0f / depth[i];
        const float x = cxs + sx[i] * cam->focal_x * iwi;
        if (x < X0) X0 = x;
        if (x > X1) X1 = x;
        const float yt = cys - (s->top - cam->z) * cam->focal_y * iwi;
        const float yb = cys - (s->bot - cam->z) * cam->focal_y * iwi;
        if (yt > y_top) y_top = yt;      /* lowest rim: the far one */
        if (yb > y_bot) y_bot = yb;      /* lowest floor: the near one */
        seen++;
    }
    if (seen < 2) return;
    if (X1 - X0 < 1.0f || y_bot - y_top < 1.0f) return;
    if (X1 < 0.0f || X0 > (float)SCREEN_W) return;
    if (y_bot < 0.0f || y_top > (float)SCREEN_H) return;

    /* Depth for the z test: the middle of the well. The footprint spans
     * the whole of it, so neither end is right, and the near rim loses to
     * nothing while the far rim ties with the wall behind the shaft. */
    const float iw = 2.0f / (dmin + dmax);

    /* Depth-proportional bias, as the billboards use. */
    float z = 1.0f - (R_FLAT_NEAR + 0.6f) * iw;
    if (z < 0.0f) z = 0.0f;

    const float xs[4] = { X0, X1, X1, X0 };
    const float ys[4] = { y_top, y_top, y_bot, y_bot };
    const float ss[4] = { 0.0f, (float)HALO_TEX, (float)HALO_TEX, 0.0f };
    const float ts[4] = { 0.0f, 0.0f, (float)HALO_TEX, (float)HALO_TEX };
    const float as[4] = { k, k, 0.0f, 0.0f };

    float v[4][10];
    for (int i = 0; i < 4; i++) {
        v[i][0] = xs[i]; v[i][1] = ys[i];
        v[i][2] = 1.00f; v[i][3] = 0.97f; v[i][4] = 0.86f;
        v[i][5] = as[i];
        v[i][6] = ss[i]; v[i][7] = ts[i];
        v[i][8] = iw;
        v[i][9] = z;
    }
    r_tri_quad(&TRIFMT_HALO, v[0], v[1], v[2], v[3]);
    r_tri_spr += 2;
}

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
         * the opening to the floor; the vertex alpha sets its scale. Half
         * what it was -- at full strength the beam read as a solid pale
         * slab rather than as light in the air. */
        shaft_prism(cam, s, 0.21f * s->lum);
    }
    num_shafts = 0;
}

/* Halos are glows around DYNAMIC LIGHTS, so with the registry compiled out
 * there is nothing to draw one around -- and the loop below reads the light
 * array directly, which does not exist then. Shafts are unaffected: they
 * come from sky wells, not from lights, and still draw. */
#if !D_DYNLIGHT
void r_halo_flush(const r_camera_t *cam) { (void)cam; }
#else
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
#endif /* D_DYNLIGHT */

#endif /* R_HALO */
