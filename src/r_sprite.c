/*
 * r_sprite -- things, drawn as screen-aligned billboards.
 *
 * Doom sprites are not world-space quads that rotate to face the viewer: they
 * are axis-aligned in screen space, scaled by distance. That is why a Doom
 * monster never appears to lean when you look up, and matching it matters for
 * the look.
 *
 * Two things separate sprites from every surface drawn so far:
 *
 * They have holes. Doom stores transparency as gaps between column posts,
 * which disappears once a patch is flattened into a rectangle, so the build
 * tool leaves untouched texels at a reserved palette index whose TLUT entry
 * has alpha zero. Alpha compare then discards them. Walls and flats never
 * needed this and their combiner does not even read texture alpha.
 *
 * They overlap each other and the world arbitrarily, so they rely on the depth
 * buffer the floors already required. Alpha-tested pixels are discarded before
 * the depth write, so a sprite's transparent margin does not punch a hole in
 * anything behind it.
 */
#include "r_sprite.h"
#include "r_flat.h"     /* R_FLAT_NEAR: the depth scale walls and flats share */

#include <math.h>

#define SPR_MAX_JOBS 128

/* Anything closer is skipped: the projection divides by depth. This is a
 * CULL threshold only -- the depth-buffer value must NOT use it, see
 * draw_tile. */
#define SPR_NEAR 8.0f

typedef struct {
    dt64_tex_t *tex;
    float       depth;
    float       sx, sy;      /* projected anchor */
    float       scale;       /* screen pixels per world unit at this depth */
    float       shade;
    uint8_t     mipped;
} sprjob_t;

static sprjob_t jobs[SPR_MAX_JOBS];
static int      numjobs;
static int      stat_drawn, stat_uploads, stat_dropped;

static const rdpq_trifmt_t TRIFMT_SPR = {
    .pos_offset   = 0,
    .shade_offset = 2,
    .tex_offset   = 6,
    .tex_tile     = TILE0,
    .z_offset     = 9,
};

void r_sprite_begin(void)
{
    numjobs = stat_drawn = stat_uploads = stat_dropped = 0;
}

int r_sprite_count(void)   { return stat_drawn; }
int r_sprite_uploads(void) { return stat_uploads; }
int r_sprite_dropped(void) { return stat_dropped; }

void r_sprite_add(const r_camera_t *cam, const r_thing_t *t, float shade)
{
    if (!t->spr) return;
    if (numjobs >= SPR_MAX_JOBS) { stat_dropped++; return; }

    const float dx = t->x - cam->x, dy = t->y - cam->y;
    /* Same per-frame basis the walls and flats use; see r_set_view. */
    const float cs = r_view_cs, sn = r_view_sn;

    const float depth = dx * cs + dy * sn;
    if (depth < SPR_NEAR) return;

    const float offs = dx * sn - dy * cs;
    const float scale = cam->focal / depth;

    /* Reject anything whose billboard falls entirely outside the viewport
     * before it costs a texture upload. */
    const float sx = SCREEN_W * 0.5f + offs * scale;
    const dt64_tex_t *tspr = (const dt64_tex_t *)t->spr;
    const float halfw = (float)tspr->width * scale;
    if (sx + halfw < 0.0f || sx - halfw > (float)SCREEN_W) return;

    /* And vertically: an item the player stands over projects entirely below
     * the screen yet still queued, uploaded its tiles and drew nothing.
     * Common in E1M1's bonus clusters. */
    const float sy   = SCREEN_H * 0.5f - (t->z - cam->z) * scale;
    const float top  = sy - (float)tspr->topoffset * scale;
    if (top > (float)SCREEN_H + 2.0f ||
        top + (float)tspr->height * scale < -2.0f) return;

    sprjob_t *j = &jobs[numjobs++];

    /* Drop to the half-resolution frame once the sprite is drawn at less than
     * half size: same picture, a quarter of the TMEM tiles and quads. */
    j->tex = (dt64_tex_t *)t->spr;
    j->mipped = 0;
    if (tspr->mip && scale < 0.5f) { j->tex = tspr->mip; j->mipped = 1; }
    j->depth = depth;
    j->sx    = sx;
    j->sy    = sy;
    j->scale = scale;
    j->shade = shade;
}

/* Draw one tile of a sprite. The billboard sits at a single depth, so the
 * mapping across it is affine and the tile grid is a plain screen-space
 * subdivision -- no perspective subdivision needed, unlike walls and floors. */
static void draw_tile(const sprjob_t *j, int s0, int t0, int s1, int t1,
                      float sc, float x0, float y0)
{
    const float x1 = x0 + (float)(s1 - s0) * sc;
    const float y1 = y0 + (float)(t1 - t0) * sc;

    if (x1 < 0.0f || x0 > (float)SCREEN_W) return;
    if (y1 < 0.0f || y0 > (float)SCREEN_H) return;

    const float iw = 1.0f / j->depth;
    /* Depth mapped with the SAME near constant walls and flats use. Mapping
     * it with SPR_NEAR (8) put sprites on a different depth curve than the
     * world (4): a sprite up to twice a wall's distance still compared in
     * front of it and drew straight through -- monsters visible in the void
     * behind walls. One constant, one curve. */
    const float z  = 1.0f - R_FLAT_NEAR * iw;

    float v[4][10];
    const float xs[4] = { x0, x1, x1, x0 };
    const float ys[4] = { y0, y0, y1, y1 };
    const float ss[4] = { (float)s0, (float)s1, (float)s1, (float)s0 };
    const float ts[4] = { (float)t0, (float)t0, (float)t1, (float)t1 };

    for (int i = 0; i < 4; i++) {
        v[i][0] = xs[i];
        v[i][1] = ys[i];
        v[i][2] = v[i][3] = v[i][4] = j->shade;
        v[i][5] = 1.0f;
        v[i][6] = ss[i];
        v[i][7] = ts[i];
        v[i][8] = iw;
        v[i][9] = z;
    }

    rdpq_triangle(&TRIFMT_SPR, v[0], v[1], v[2]);
    rdpq_triangle(&TRIFMT_SPR, v[0], v[2], v[3]);
    r_tri_spr += 2;
}

/* Distinct textures in the current batch, so each is uploaded once per tile
 * rather than once per instance. E1M1 places 25 identical armour bonuses; they
 * should cost one upload between them, not 25. */
#define SPR_MAX_TEXTURES 48

void r_sprite_flush(void)
{
    if (!numjobs) return;

    /* Alpha compare against the transparency key. The combiner must pass
     * texture alpha through for this to see anything -- walls deliberately
     * pass shade alpha instead, so the mode is set here rather than shared. */
    rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, SHADE, 0), (0, 0, 0, TEX0)));
    rdpq_mode_alphacompare(1);

    /* Depth on, explicitly. The wall pass turns it off for its own reasons and
     * modes persist, so inheriting it left sprites drawing over geometry that
     * was in front of them -- monsters visible through walls. */
    rdpq_mode_zbuf(true, true);

    /* Point sampling, also explicitly, and for a reason that does not apply to
     * walls: a sprite's transparency is a reserved palette index, and blending
     * it against its neighbours rings every cutout edge with a halo of that
     * colour. Walls have no such index and take bilinear happily, so the
     * inherited mode was wrong here specifically. */
    rdpq_mode_filter(FILTER_POINT);

    /* Ascending depth. The BSP delivers sprites roughly near-to-far, so this
     * is the direction in which insertion sort really does run in near-linear
     * time -- sorting descending (as this used to) made the natural arrival
     * order the provable worst case: every new job shifted the entire sorted
     * prefix, ~8100 struct copies at the job cap, precisely in fight frames.
     * Far-to-near drawing order is recovered below by walking each bucket
     * from its head, which the construction leaves farthest-first. */
    for (int a = 1; a < numjobs; a++) {
        const sprjob_t key = jobs[a];
        int b = a - 1;
        while (b >= 0 && jobs[b].depth > key.depth) { jobs[b + 1] = jobs[b]; b--; }
        jobs[b + 1] = key;
    }

    /* Group by texture with one O(jobs) bucket pass, the same fix r_wall's
     * counting sort and r_flat's linked buckets made: the previous form
     * rescanned all jobs once per (texture, tile) -- ~15k iterations in a
     * fight frame purely to skip entries. Pushing jobs in ascending depth
     * order onto each head leaves every bucket farthest-first. */
    dt64_tex_t *texes[SPR_MAX_TEXTURES];
    int16_t     head[SPR_MAX_TEXTURES];
    static int16_t nextj[SPR_MAX_JOBS];
    int ntex = 0;
    for (int i = 0; i < numjobs; i++) {
        int k = -1;
        for (int c = 0; c < ntex; c++)
            if (texes[c] == jobs[i].tex) { k = c; break; }
        if (k < 0) {
            if (ntex >= SPR_MAX_TEXTURES) { stat_dropped++; continue; }
            k = ntex;
            texes[ntex] = jobs[i].tex;
            head[ntex++] = -1;
        }
        nextj[i] = head[k];
        head[k]  = (int16_t)i;
    }

    for (int k = 0; k < ntex; k++) {
        dt64_tex_t *tex = texes[k];
        const int w = tex->width, h = tex->height;

        /* Sprites are frequently larger than the 2 KB of TMEM left beside the
         * palette, so they tile like walls do. Iterating tiles outside the
         * instance loop is what makes the batching pay: one upload serves
         * every copy of the sprite on screen. */
        const int tw = w < DT64_TILE_W ? w : DT64_TILE_W;
        const int th = h < DT64_TILE_H ? h : DT64_TILE_H;

        for (int t0 = 0; t0 < h; t0 += th) {
            const int t1 = t0 + th > h ? h : t0 + th;
            for (int s0 = 0; s0 < w; s0 += tw) {
                const int s1 = s0 + tw > w ? w : s0 + tw;

                bool bound = false;
                for (int i = head[k]; i >= 0; i = nextj[i]) {
                    const sprjob_t *j = &jobs[i];

                    /* Anchor: horizontally the sprite's centre marker,
                     * vertically the feet. Both from the patch header. */
                    const float sc = j->mipped ? j->scale * 2.0f : j->scale;
                    const float left = j->sx - (float)tex->leftoffset * sc;
                    const float top  = j->sy - (float)tex->topoffset  * sc;
                    const float x0 = left + (float)s0 * sc;
                    const float y0 = top  + (float)t0 * sc;

                    /* Visibility BEFORE the upload: a (texture, tile) pair
                     * whose every instance is off-screen used to upload 2 KB
                     * and then draw nothing. */
                    if (x0 + (float)(s1 - s0) * sc < 0.0f ||
                        x0 > (float)SCREEN_W ||
                        y0 + (float)(t1 - t0) * sc < 0.0f ||
                        y0 > (float)SCREEN_H) continue;

                    if (!bound) {
                        dt64_upload_tile(TILE0, tex, NULL, s0, t0, s1, t1);
                        stat_uploads++;
                        bound = true;
                    }

                    draw_tile(j, s0, t0, s1, t1, sc, x0, y0);
                }
            }
        }
    }
    stat_drawn += numjobs;

    rdpq_mode_alphacompare(0);
    numjobs = 0;
}

/* Draw the player's weapon.
 *
 * Unlike world sprites these are pure screen space: no projection, no depth,
 * fixed scale. Doom lays them out in a 320x200 frame whose bottom 32 rows are
 * the status bar; this target is 320x240 with no status bar yet, so the whole
 * thing shifts down by the difference and the weapon still sits against the
 * bottom edge.
 *
 * Drawn after the world with the depth test off, because the weapon is in
 * front of everything by definition.
 */
/* Doom lays psprites out in a 320x200 frame whose bottom 32 rows are the
 * status bar; the weapon's bottom edge lands on the bar's top. This screen
 * is 240 rows with the bar overlaid on the bottom 32 (rows 208..240), so the
 * weapon shifts down by 240-200-32 = 8: its bottom edge meets the bar's top
 * exactly as on the PC. The previous 40-row shift pinned it to the screen
 * bottom instead, leaving the pistol half-buried under the bar. */
#define PSPRITE_YSHIFT (SCREEN_H - 200 - 32)

void r_psprite_draw(void)
{
    int D_PSpriteGet(int i, void **tex, int *x, int *y);

    /* Runs inside the frame's V_BeginUI bracket: COPY mode, TLUT bound,
     * transparent index discarded by the alpha threshold. The weapon is an
     * unscaled 1:1 screen blit exactly like a menu patch, so it shares that
     * state and each tile is one fixed-function rectangle -- no CPU edge
     * setup, four texels per clock -- instead of two shaded triangles. */
    for (int i = 0; i < 2; i++) {
        void *raw; int px, py;
        if (!D_PSpriteGet(i, &raw, &px, &py)) continue;

        const dt64_tex_t *tex = (const dt64_tex_t *)raw;
        const int w = tex->width, h = tex->height;
        const int tw = w < DT64_TILE_W ? w : DT64_TILE_W;
        const int th = h < DT64_TILE_H ? h : DT64_TILE_H;

        const float ox = (float)px, oy = (float)(py + PSPRITE_YSHIFT);

        for (int t0 = 0; t0 < h; t0 += th) {
            const int t1 = t0 + th > h ? h : t0 + th;
            for (int s0 = 0; s0 < w; s0 += tw) {
                const int s1 = s0 + tw > w ? w : s0 + tw;

                /* Cull before the upload, not after it. */
                const float x0 = ox + (float)s0, y0 = oy + (float)t0;
                const float x1 = ox + (float)s1, y1 = oy + (float)t1;
                if (x1 < 0.0f || x0 > (float)SCREEN_W) continue;
                if (y1 < 0.0f || y0 > (float)SCREEN_H) continue;

                dt64_upload_tile(TILE0, tex, NULL, s0, t0, s1, t1);
                rdpq_texture_rectangle(TILE0, x0, y0, x1, y1,
                                       (float)s0, (float)t0);
                r_tri_spr++;
            }
        }
    }
}
