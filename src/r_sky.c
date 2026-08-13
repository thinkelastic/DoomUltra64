/*
 * r_sky -- the sky backdrop.
 *
 * Doom's sky is not geometry. A sector whose ceiling flat is F_SKY1 simply
 * draws no ceiling, and whatever shows through is the sky texture painted in
 * screen space, scrolled by the view angle. Upper textures between two sky
 * sectors are skipped for the same reason: there is no surface there, just an
 * opening onto the backdrop.
 *
 * The horizontal mapping follows Doom's: a full turn spans four widths of the
 * sky texture, so a 90-degree view sees one width. That is why the sky in Doom
 * appears to rotate faster than the world -- it is a deliberate exaggeration,
 * not a projection.
 *
 * Drawn before the world with depth writes off, so everything else paints over
 * it. That costs a full-screen fill, which is why it is skipped entirely for
 * levels with no sky at all.
 */
#include "r_sky.h"

#ifndef R_BILINEAR
#define R_BILINEAR 1
#endif
#include "r_tri.h"

#include <math.h>

/* Doom maps 360 degrees onto four widths of the sky texture. */
#define SKY_TURNS 4.0f

static const rdpq_trifmt_t TRIFMT_SKY = {
    .pos_offset   = 0,
    .shade_offset = 2,
    .tex_offset   = 6,
    .tex_tile     = TILE0,
    .z_offset     = 9,
};

/* Just inside the far plane. Drawn last with the depth test on and writes
 * off, this passes only where nothing else was drawn -- so the sky costs the
 * pixels it actually occupies instead of a full screen the world then paints
 * over. Indoors it costs nothing at all.
 *
 * The margin has to clear the most distant geometry: depth d maps to
 * 1 - near/d, so 0.9999 sits beyond anything closer than 40000 units. */
#define SKY_DEPTH 0.9999f

static dt64_tex_t *sky_tex;

/* Columns where sky can actually be visible this frame, accumulated by the
 * BSP walk (sky-to-sky openings and sky-ceiling subsectors). Before this the
 * pass rasterised all 320x240 pixels depth-tested every frame in a sky level
 * -- even fully indoors, where the z-test failed every pixel but each still
 * cost a pipe cycle and a 16-bit z read, contradicting the "costs nothing"
 * note above. Empty span: the pass is skipped outright. */
static int sky_x0, sky_x1;

void r_sky_span_reset(void) { sky_x0 = SCREEN_W; sky_x1 = -1; }

void r_sky_span_add(int x0, int x1)
{
    if (x0 < sky_x0) sky_x0 = x0;
    if (x1 > sky_x1) sky_x1 = x1;
}

bool r_sky_would_draw(void) { return sky_tex != NULL && sky_x1 >= sky_x0; }

void r_sky_span(int *x0, int *x1) { *x0 = sky_x0; *x1 = sky_x1; }

void r_sky_set(dt64_tex_t *tex) { sky_tex = tex; }
bool r_sky_present(void)        { return sky_tex != NULL; }

void r_sky_draw(const r_camera_t *cam)
{
    if (!sky_tex) return;
    if (sky_x1 < sky_x0) return;           /* no sky visible this frame */

    /* Full resolution. The half-res mip was cheaper when the pass covered
     * the whole screen every frame, but the span clamp already pays that
     * bill, and the blur it cost turned Freedoom's mountain silhouettes
     * into a featureless smear that read as missing geometry. */
    const dt64_tex_t *tex = sky_tex;
    const int texw = tex->width, texh = tex->height;
    if (texw <= 0 || texh <= 0) return;

    /* Flat shading, no depth interaction: this is a backdrop, and the world
     * that follows must be free to overwrite every pixel of it. */
    rdpq_set_mode_standard();
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, SHADE, 0), (0, 0, 0, SHADE)));
    /* Filtered with the world, under the same lever. A sky is mostly
     * gradient, which is where point sampling shows its stair-steps
     * worst. The hazard is the tile grid: each quad samples its own
     * 64x32 TMEM tile uploaded with clamping parameters, so at a tile's
     * edge the filter asks for a neighbour texel it does not have and
     * clamps to the edge instead -- half a texel of smear along every
     * boundary. Checked against a point-sampled capture of the same
     * sky-heavy pose before this was left on. */
    rdpq_mode_filter(R_BILINEAR ? FILTER_BILINEAR : FILTER_POINT);
    rdpq_mode_persp(false);        /* screen-space quad: no perspective to correct */
    rdpq_mode_zbuf(true, false);   /* test against the world, never write */

    /* Scroll by view angle. Negated so the sky moves opposite the turn, as the
     * world does. */
    float turn = -cam->angle / (2.0f * (float)M_PI);
    turn -= floorf(turn);
    const float s_left = turn * SKY_TURNS * (float)texw;

    /* One screen width sees a quarter turn, hence one texture width. */
    const float s_per_px = (float)texw / (float)SCREEN_W;

    const int tile_w = texw < DT64_TILE_W ? texw : DT64_TILE_W;
    const int tile_h = texh < DT64_TILE_H ? texh : DT64_TILE_H;

    /* Vanilla's vertical mapping, not a stretch-to-fit. Doom draws sky at
     * one texel per pixel on a 200-row screen and lets it wrap below; this
     * pass was stretching 128 rows over all 240, which pushed the pale top
     * band of the texture across twice as much screen as the PC shows and
     * squashed the mountains low. Scaled to this screen's height, a texel
     * row covers SCREEN_H/200 pixels, and rows past the texture's end wrap
     * -- they sit below the horizon and are almost always behind world
     * geometry, exactly as on the PC. */
    const float px_per_row = (float)SCREEN_H / 200.0f;

    const float x_lo = (float)(sky_x0 > 0 ? sky_x0 - 1 : 0);
    const float x_hi = (float)(sky_x1 + 2 < SCREEN_W ? sky_x1 + 2 : SCREEN_W);
    const float s_lo = s_left + x_lo * s_per_px;
    const float s_hi = s_left + x_hi * s_per_px;

    const int first = (int)floorf(s_lo / (float)tile_w) * tile_w;

    for (int col = first; (float)col < s_hi; col += tile_w) {
        const float ca = fmaxf((float)col, s_lo);
        const float cb = fminf((float)(col + tile_w), s_hi);
        if (cb <= ca) continue;

        /* Wrap into the texture. Widths are powers of two in practice, but
         * the modulo keeps it honest for odd sky textures. */
        int src_s = col % texw;
        if (src_s < 0) src_s += texw;

        const float x0 = (ca - s_left) / s_per_px;
        const float x1 = (cb - s_left) / s_per_px;
        const float sa = src_s + (ca - (float)col);
        const float sb = src_s + (cb - (float)col);

        /* Tile row OUTER, vertical repeat INNER: the repeat loop re-drew the
         * same TMEM tile at each wrapped band, so iterating it inside means
         * one upload per (column, tile row) instead of one per band drawn --
         * a wide sky repeated twice was uploading everything twice. The
         * quads tile the screen without overlap at one shared depth, so
         * draw order cannot change a pixel. r_tri_quad sends the second
         * triangle as one slot write instead of a full second conversion. */
        const float band_px = (float)texh * px_per_row;
        for (int t0 = 0; t0 < texh; t0 += tile_h) {
            const int t1 = t0 + tile_h > texh ? texh : t0 + tile_h;
            bool resident = false;

            for (float ybase = 0.0f; ybase < (float)SCREEN_H; ybase += band_px) {
                const float y0 = ybase + (float)t0 * px_per_row;
                const float y1 = ybase + (float)t1 * px_per_row;
                if (y0 >= (float)SCREEN_H) continue;

                if (!resident) {
                    dt64_upload_tile(TILE0, tex, NULL,
                                     src_s, t0,
                                     src_s + tile_w > texw ? texw
                                                           : src_s + tile_w,
                                     t1);
                    resident = true;
                }

                /* Screen-space quad. Every vertex shares 1/w, so the mapping
                 * is affine whatever the perspective bit says. */
                float v[4][10];
                const float xs[4] = { x0, x1, x1, x0 };
                const float ys[4] = { y0, y0, y1, y1 };
                const float ss[4] = { sa, sb, sb, sa };
                const float ts[4] = { (float)t0, (float)t0,
                                      (float)t1, (float)t1 };
                for (int i = 0; i < 4; i++) {
                    v[i][0] = xs[i];
                    v[i][1] = ys[i];
                    v[i][2] = v[i][3] = v[i][4] = 1.0f;
                    v[i][5] = 1.0f;
                    v[i][6] = ss[i];
                    v[i][7] = ts[i];
                    v[i][8] = 1.0f;
                    v[i][9] = SKY_DEPTH;
                }
                r_tri_quad(&TRIFMT_SKY, v[0], v[1], v[2], v[3]);
                r_tri_sky += 2;
            }
        }
    }

    rdpq_mode_persp(true);
}

/* Set the backdrop from code that speaks Doom's headers and cannot include
 * dt64.h's neighbours without a type clash. */
void r_sky_set_raw(void *tex) { r_sky_set((dt64_tex_t *)tex); }
