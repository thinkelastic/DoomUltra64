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
#include "r_tri.h"

#include <math.h>
#include <string.h>

#define SPR_MAX_JOBS 128

/* Anything closer is skipped: the projection divides by depth. This is a
 * CULL threshold only -- the depth-buffer value must NOT use it, see
 * draw_tile. */
#define SPR_NEAR 8.0f

typedef struct {
    dt64_tex_t *tex;
    float       depth;
    float       sx, sy;      /* projected anchor */
    /* Screen pixels per world unit at this depth, per axis. Equal in a
     * square-pixel mode; scale_x is twice scale_y at 640x240. */
    float       scale_x, scale_y;
    /* Three channels: the RSP vertex always carried independent RGB, so a
     * coloured light on an actor costs nothing extra per vertex. Fog is
     * folded in here, once per job. */
    float       sr, sg, sb;
    /* 1.0 draws in the opaque alpha-cut groups; anything lower routes to
     * the translucent tail pass -- smoke and bullet puffs. */
    float       alpha;
    uint8_t     mipped;
} sprjob_t;

static sprjob_t jobs[SPR_MAX_JOBS];
static int      numjobs;
static int      stat_drawn, stat_uploads, stat_dropped;

#if R_REFLECT
/* Mirror images of things standing over glowing liquid. Few per frame --
 * only things whose own sector floor glows -- so no grouping: each job
 * uploads its own tiles. The image is the sprite reflected about the
 * horizontal plane z = H, which for a zero-pitch camera is another
 * billboard: same columns, rows reversed, hanging below the waterline. */
#define REFL_MAX 16
typedef struct {
    dt64_tex_t *tex;
    float       depth;
    float       sx;
    float       scale_x, scale_y;
    float       w_top;       /* sprite's world top; rows run down from it */
    float       plane_h;     /* the waterline */
    float       sr, sg, sb;  /* dimmed, pool-tinted shade */
} refljob_t;
static refljob_t refl[REFL_MAX];
static int       numrefl;
#if D_DYNLIGHT
static int       numrwrefl;      /* wall ghosts; queue at end of file */
#endif

/* Visible footprints of the reflective flats, queued by the BSP walk (the
 * same subsector polygons the vapor pass consumes; they live in the level
 * arena and do not move). Every ghost is clipped to the footprints of ITS
 * OWN plane in screen space -- the correction for everything the z-test
 * cannot see: same-height neighbours, other reflective floors, and sky
 * columns the world never wrote. */
#define REFL_REGION_MAX 24
typedef struct {
    const r_polypt_t *pts;
    float             h;
    uint8_t           n;
} reflregion_t;
static reflregion_t reflreg[REFL_REGION_MAX];
static int          numreflreg;

/* Self-report for the HWSTAT line: how many footprints the walk queued
 * and how many ghost triangles actually reached the RDP this frame. The
 * pair separates "floor never registered" from "ghosts all culled". */
int r_refl_dbg_regions, r_refl_dbg_tris;

void r_reflect_region(const r_polypt_t *pts, int npts, float h)
{
    if (npts < 3 || numreflreg >= REFL_REGION_MAX) return;
    reflregion_t *r = &reflreg[numreflreg++];
    r->pts = pts;
    r->h   = h;
    r->n   = (uint8_t)(npts > 16 ? 16 : npts);
}
#endif

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
#if R_REFLECT
    numrefl = 0;
    r_refl_dbg_regions = numreflreg;   /* last frame's count, now complete */
    numreflreg = 0;
    r_refl_dbg_tris = 0;
#if D_DYNLIGHT
    numrwrefl = 0;      /* a frame whose flush never ran must not leak */
#endif
#endif
}

int r_sprite_count(void)   { return stat_drawn; }
int r_sprite_uploads(void) { return stat_uploads; }
int r_sprite_dropped(void) { return stat_dropped; }

void r_sprite_add(const r_camera_t *cam, const r_thing_t *t,
                  const float sh[3], int fog_ll, float alpha)
{
    if (!t->spr) return;
    if (numjobs >= SPR_MAX_JOBS) { stat_dropped++; return; }

    const float dx = t->x - cam->x, dy = t->y - cam->y;
    /* Same per-frame basis the walls and flats use; see r_set_view. */
    const float cs = r_view_cs, sn = r_view_sn;

    const float depth = dx * cs + dy * sn;
    if (depth < SPR_NEAR) return;

    const float offs = dx * sn - dy * cs;
    const float scale_x = cam->focal_x / depth;
    const float scale_y = cam->focal_y / depth;

    /* Reject anything whose billboard falls entirely outside the viewport
     * before it costs a texture upload. */
    const float sx = SCREEN_W * 0.5f + offs * scale_x;
    const dt64_tex_t *tspr = (const dt64_tex_t *)t->spr;
    const float halfw = (float)tspr->width * scale_x;
    if (sx + halfw < 0.0f || sx - halfw > (float)SCREEN_W) return;

    /* And vertically: an item the player stands over projects entirely below
     * the screen yet still queued, uploaded its tiles and drew nothing.
     * Common in E1M1's bonus clusters. */
    const float sy   = SCREEN_H * 0.5f - (t->z - cam->z) * scale_y;
    const float top  = sy - (float)tspr->topoffset * scale_y;
    if (top > (float)SCREEN_H + 2.0f ||
        top + (float)tspr->height * scale_y < -2.0f) return;

    sprjob_t *j = &jobs[numjobs++];

    /* Drop to the half-resolution frame once the sprite is drawn at less than
     * half size: same picture, a quarter of the TMEM tiles and quads. */
    j->tex = (dt64_tex_t *)t->spr;
    j->mipped = 0;
    /* Tested against the vertical scale, which is unchanged by wide mode, so
     * sprite LOD picks the same level as the 320 build. Same reasoning as the
     * wall mip test in r_wall.c: keep uploads fixed so a frame-time A/B
     * isolates fill rate. */
    if (tspr->mip && scale_y < 0.5f) { j->tex = tspr->mip; j->mipped = 1; }
    j->depth = depth;
    j->sx    = sx;
    j->sy    = sy;
    j->scale_x = scale_x;
    j->scale_y = scale_y;
    /* Distance falloff, once per job: sprites previously ignored it and
     * glowed full-bright beside fog-dimmed walls at the far end of a hall.
     * Vanilla diminishes them like everything else; fullbright frames
     * (fog_ll < 0) stay exempt. */
    {
#if R_FOGSCALE
        const float k = fog_ll >= 0 ? r_vis(depth, r_fog_inv[fog_ll]) : 1.0f;
#else
        const float k = 1.0f; (void)fog_ll;
#endif
        j->sr = sh[0] * k;
        j->sg = sh[1] * k;
        j->sb = sh[2] * k;
    }
    j->alpha = alpha;
}

/* Draw one tile of a sprite. The billboard sits at a single depth, so the
 * mapping across it is affine and the tile grid is a plain screen-space
 * subdivision -- no perspective subdivision needed, unlike walls and floors. */
static void draw_tile(const sprjob_t *j, int s0, int t0, int s1, int t1,
                      float scx, float scy, float x0, float y0)
{
    const float x1 = x0 + (float)(s1 - s0) * scx;
    const float y1 = y0 + (float)(t1 - t0) * scy;

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
        v[i][2] = j->sr; v[i][3] = j->sg; v[i][4] = j->sb;
        v[i][5] = j->alpha;   /* opaque groups never read it; the tail does */
        v[i][6] = ss[i];
        v[i][7] = ts[i];
        v[i][8] = iw;
        v[i][9] = z;
    }

    /* Slot reuse, as walls and flats: the second triangle shares v0 and v2
     * with the first, so it is one vertex write and an issue word instead
     * of a full second rdpq_triangle (which reconverts all three). The RSP
     * Y-sorts each triangle independently, so reordering cannot change
     * coverage beyond the same ±1-step tie-break class TRIFAST documented. */
    r_tri_quad(&TRIFMT_SPR, v[0], v[1], v[2], v[3]);
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
    int have_tl = 0;
    for (int i = 0; i < numjobs; i++) {
        if (jobs[i].alpha < 1.0f) { have_tl = 1; continue; }
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
                    const float m   = j->mipped ? 2.0f : 1.0f;
                    const float scx = j->scale_x * m;
                    const float scy = j->scale_y * m;
                    const float left = j->sx - (float)tex->leftoffset * scx;
                    const float top  = j->sy - (float)tex->topoffset  * scy;
                    const float x0 = left + (float)s0 * scx;
                    const float y0 = top  + (float)t0 * scy;

                    /* Visibility BEFORE the upload: a (texture, tile) pair
                     * whose every instance is off-screen used to upload 2 KB
                     * and then draw nothing. */
                    if (x0 + (float)(s1 - s0) * scx < 0.0f ||
                        x0 > (float)SCREEN_W ||
                        y0 + (float)(t1 - t0) * scy < 0.0f ||
                        y0 > (float)SCREEN_H) continue;

                    if (!bound) {
                        dt64_upload_tile(TILE0, tex, NULL, s0, t0, s1, t1);
                        stat_uploads++;
                        bound = true;
                    }

                    draw_tile(j, s0, t0, s1, t1, scx, scy, x0, y0);
                }
            }
        }
    }
    /* --- translucent tail: smoke and puffs -----------------------------
     * Far to near (the array is depth-ascending, so walk it backward),
     * blended against what is already there, z-tested so the world
     * occludes them but never z-written so they cannot carve holes in
     * each other. The combiner must multiply texture alpha by shade
     * alpha here -- the opaque pass deliberately passes texture alpha
     * alone for its cutout compare. */
    if (have_tl) {
        rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, SHADE, 0),
                                          (TEX0, 0, SHADE, 0)));
        rdpq_mode_blender(RDPQ_BLENDER((IN_RGB, IN_ALPHA,
                                        MEMORY_RGB, INV_MUX_ALPHA)));
        rdpq_mode_alphacompare(0);
        rdpq_mode_zbuf(true, false);

        dt64_tex_t *last_tex = NULL;
        int last_s0 = -1, last_t0 = -1;
        for (int i = numjobs - 1; i >= 0; i--) {
            const sprjob_t *j = &jobs[i];
            if (j->alpha >= 1.0f) continue;
            dt64_tex_t *tex = j->tex;
            const int w = tex->width, h = tex->height;
            const int tw = w < DT64_TILE_W ? w : DT64_TILE_W;
            const int th = h < DT64_TILE_H ? h : DT64_TILE_H;
            const float m   = j->mipped ? 2.0f : 1.0f;
            const float scx = j->scale_x * m;
            const float scy = j->scale_y * m;
            const float left = j->sx - (float)tex->leftoffset * scx;
            const float top  = j->sy - (float)tex->topoffset  * scy;
            for (int t0 = 0; t0 < h; t0 += th) {
                const int t1 = t0 + th > h ? h : t0 + th;
                for (int s0 = 0; s0 < w; s0 += tw) {
                    const int s1 = s0 + tw > w ? w : s0 + tw;
                    const float x0 = left + (float)s0 * scx;
                    const float y0 = top  + (float)t0 * scy;
                    if (x0 + (float)(s1 - s0) * scx < 0.0f ||
                        x0 > (float)SCREEN_W ||
                        y0 + (float)(t1 - t0) * scy < 0.0f ||
                        y0 > (float)SCREEN_H) continue;
                    /* Puff frames repeat down a trail: one upload serves
                     * a run of the same frame. */
                    if (tex != last_tex || s0 != last_s0 || t0 != last_t0) {
                        dt64_upload_tile(TILE0, tex, NULL, s0, t0, s1, t1);
                        stat_uploads++;
                        last_tex = tex; last_s0 = s0; last_t0 = t0;
                    }
                    draw_tile(j, s0, t0, s1, t1, scx, scy, x0, y0);
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

        /* Doom's 320-wide frame; scaled to the viewport at emit, exactly as
         * v_blit does for menu and status-bar art. */
        const float ox = (float)px;
        const float oy = (float)(py + PSPRITE_YSHIFT);

        for (int t0 = 0; t0 < h; t0 += th) {
            const int t1 = t0 + th > h ? h : t0 + th;
            for (int s0 = 0; s0 < w; s0 += tw) {
                const int s1 = s0 + tw > w ? w : s0 + tw;

                /* Cull before the upload, not after it. */
                const float x0 = (ox + (float)s0) * UI_XSCALE;
                const float x1 = (ox + (float)s1) * UI_XSCALE;
                const float y0 = oy + (float)t0, y1 = oy + (float)t1;
                if (x1 < 0.0f || x0 > (float)SCREEN_W) continue;
                if (y1 < 0.0f || y0 > (float)SCREEN_H) continue;

                dt64_upload_tile(TILE0, tex, NULL, s0, t0, s1, t1);
                rdpq_texture_rectangle_scaled(TILE0, x0, y0, x1, y1,
                                              (float)s0, (float)t0,
                                              (float)s1, (float)t1);
                r_tri_spr++;
            }
        }
    }
}

#if R_REFLECT
/* --- liquid reflections -------------------------------------------------
 *
 * Mirror images of things over glowing liquid. For a camera that never
 * pitches, the reflection of a billboard about the horizontal plane z = H
 * is another billboard: same columns, rows reversed, hanging below the
 * waterline. So the whole feature is one more quad per nearby thing, and
 * the masking costs nothing at all -- see r_reflect_flush.
 */

/* Queue the mirror image. Culls mirror r_sprite_add's, but against the
 * image's own extent: a sprite on screen can have its image off screen and
 * vice versa. */
void r_reflect_add(const r_camera_t *cam, const r_thing_t *t,
                   const float sh[3], int fog_ll,
                   float plane_h, const float pool_rgb[3])
{
    if (!t->spr || numrefl >= REFL_MAX) return;
    if (cam->z <= plane_h + 1.0f) return;   /* eye at the waterline: no image */

    const float dx = t->x - cam->x, dy = t->y - cam->y;
    const float cs = r_view_cs, sn = r_view_sn;
    const float depth = dx * cs + dy * sn;
    if (depth < SPR_NEAR) return;

    const float offs    = dx * sn - dy * cs;
    const float scale_x = cam->focal_x / depth;
    const float scale_y = cam->focal_y / depth;

    const dt64_tex_t *tspr = (const dt64_tex_t *)t->spr;
    const float sx    = SCREEN_W * 0.5f + offs * scale_x;
    const float halfw = (float)tspr->width * scale_x;
    if (sx + halfw < 0.0f || sx - halfw > (float)SCREEN_W) return;

    const float w_top  = t->z + (float)tspr->topoffset;
    const float w_bot  = w_top - (float)tspr->height;
    const float cy     = SCREEN_H * 0.5f;
    /* Image extent, mirrored: z' = 2H - z. */
    const float y_top  = cy - (2.0f * plane_h - w_bot - cam->z) * scale_y;
    if (y_top > (float)SCREEN_H + 2.0f) return;
    const float y_bot  = cy - (2.0f * plane_h - w_top - cam->z) * scale_y;
    if (y_bot < -2.0f) return;

    refljob_t *j = &refl[numrefl++];
    j->tex     = (dt64_tex_t *)t->spr;   /* no mip: images are dim and short */
    j->depth   = depth;
    j->sx      = sx;
    j->scale_x = scale_x;
    j->scale_y = scale_y;
    j->w_top   = w_top;
    j->plane_h = plane_h;

    /* Fogged like the caster, then dimmed toward the pool's own hue: a
     * reflection off rippling emissive liquid is darker than what casts it
     * and picks the liquid's colour up. Fullbright casters (fireballs, a
     * lost soul) keep their exemption and stay bright in the water. */
    float k = 1.0f;
#if R_FOGSCALE
    if (fog_ll >= 0) k = r_vis(depth, r_fog_inv[fog_ll]);
#else
    (void)fog_ll;
#endif
    /* Tuned on a real TV, not the emulator: composite video and CRT
     * gamma eat dim blends, so the ghost runs brighter than a monitor
     * would suggest. Console diagnostic R3/17 proved the geometry was
     * rendering while the player saw nothing. */
    j->sr = sh[0] * k * (0.46f + 0.30f * pool_rgb[0]);
    j->sg = sh[1] * k * (0.46f + 0.30f * pool_rgb[1]);
    j->sb = sh[2] * k * (0.46f + 0.30f * pool_rgb[2]);
}

#if D_DYNLIGHT
static void reflect_walls_emit(const r_camera_t *cam);
#endif

/* Screen-project one footprint at its own height; 0 if it vanishes. */
static int refl_project_region(const r_camera_t *cam, const reflregion_t *rg,
                               float px[24], float py[24])
{
    float d[16], o[16];
    const int n = rg->n;
    const float cs = r_view_cs, sn = r_view_sn;
    for (int i = 0; i < n; i++) {
        const float dx = rg->pts[i].x - cam->x;
        const float dy = rg->pts[i].y - cam->y;
        d[i] = dx * cs + dy * sn;
        o[i] = dx * sn - dy * cs;
    }
    /* Near-clip the polygon so the projection below cannot divide by a
     * vanishing depth. */
    float d2[20], o2[20];
    int m = 0;
    for (int i = 0; i < n; i++) {
        const int j = i + 1 < n ? i + 1 : 0;
        const int ina = d[i] >= SPR_NEAR, inb = d[j] >= SPR_NEAR;
        if (ina) { d2[m] = d[i]; o2[m] = o[i]; m++; }
        if (ina != inb) {
            const float t = (SPR_NEAR - d[i]) / (d[j] - d[i]);
            d2[m] = SPR_NEAR; o2[m] = o[i] + (o[j] - o[i]) * t; m++;
        }
        if (m >= 18) break;
    }
    if (m < 3) return 0;
    const float cx = SCREEN_W * 0.5f, cy = SCREEN_H * 0.5f;
    const float hz = rg->h - cam->z;
    for (int i = 0; i < m; i++) {
        const float iw = 1.0f / d2[i];
        px[i] = cx + o2[i] * cam->focal_x * iw;
        py[i] = cy - hz * cam->focal_y * iw;
    }
    return m;
}

/* One Sutherland-Hodgman step over 10-float rows. Rows carry s,t already
 * multiplied by iw, so every attribute is screen-linear and the plain lerp
 * at the cut is perspective-exact. */
static int refl_clip_edge(float in[][10], int n, float out[][10],
                          float ax, float ay, float bx, float by, float sgn)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        const int j = i + 1 < n ? i + 1 : 0;
        const float ex = bx - ax, ey = by - ay;
        const float da = sgn * (ex * (in[i][1] - ay) - ey * (in[i][0] - ax));
        const float db = sgn * (ex * (in[j][1] - ay) - ey * (in[j][0] - ax));
        if (da >= 0.0f) { memcpy(out[m], in[i], sizeof(float) * 10); m++; }
        if ((da >= 0.0f) != (db >= 0.0f) && m < 13) {
            const float t = da / (da - db);
            for (int k = 0; k < 10; k++)
                out[m][k] = in[i][k] + (in[j][k] - in[i][k]) * t;
            m++;
        }
        if (m >= 13) break;
    }
    return m;
}

/* Clip one ghost quad (rows with s,t premultiplied by iw) to every
 * footprint of its plane; emit the surviving fans. A ghost whose plane
 * has no visible footprint emits nothing -- which is also the correct
 * fate of a mirror over a pool the frame never drew. */
static void refl_emit_clipped(const r_camera_t *cam, float q[4][10],
                              float plane_h)
{
    for (int rgi = 0; rgi < numreflreg; rgi++) {
        const reflregion_t *rg = &reflreg[rgi];
        if (rg->h > plane_h + 0.5f || rg->h < plane_h - 0.5f) continue;

        float px[24], py[24];
        const int pn = refl_project_region(cam, rg, px, py);
        if (pn < 3) continue;

        /* The footprint's winding on screen decides which side is inside. */
        float area = 0.0f;
        for (int i = 0; i < pn; i++) {
            const int j = i + 1 < pn ? i + 1 : 0;
            area += px[i] * py[j] - px[j] * py[i];
        }
        const float sgn = area >= 0.0f ? 1.0f : -1.0f;

        float a[14][10], b[14][10];
        memcpy(a, q, sizeof(float) * 40);
        int n = 4, flip = 0;
        for (int e = 0; e < pn && n >= 3; e++) {
            const int j = e + 1 < pn ? e + 1 : 0;
            n = refl_clip_edge(flip ? b : a, n, flip ? a : b,
                               px[e], py[e], px[j], py[j], sgn);
            flip ^= 1;
        }
        if (n < 3) continue;

        float (*p)[10] = flip ? b : a;
        float v[14][10];
        for (int i = 0; i < n; i++) {
            memcpy(v[i], p[i], sizeof(float) * 10);
            v[i][6] = p[i][6] / p[i][8];
            v[i][7] = p[i][7] / p[i][8];
        }
        for (int i = 1; i + 1 < n; i++) {
            rdpq_triangle(&TRIFMT_SPR, v[0], v[i], v[i + 1]);
            r_tri_spr++;
            r_refl_dbg_tris++;
        }
    }
}

/* Draw the queued images. Runs after the sprite pass -- nearer actors
 * occlude images through the z-buffer -- and before sky and vapor: sky
 * repaints any spill into never-written columns, and the haze drifts over
 * the mirror, which is exactly where haze belongs.
 *
 * The masking trick: each vertex's Z is the depth at which the ray through
 * it crosses the water plane. On pool pixels that is the pool's own
 * depth-buffer value (same plane), so a small bias wins; on any pixel where
 * nearer geometry was drawn -- the pool's banks, walls, things -- the test
 * fails. The image clips itself to the pool's visible pixels with no
 * polygon math at all. */
void r_reflect_flush(const r_camera_t *cam)
{
#if D_DYNLIGHT
    if (!numrefl && !numrwrefl) return;
#else
    if (!numrefl) return;
#endif

    /* Blended, z-tested, never z-written. Texture alpha rides through the
     * combiner's alpha channel, so transparent texels leave the memory
     * pixel alone and solid ones mix at the vertex alpha. Point sampling
     * for the same cutout-halo reason as the sprite pass. */
    rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, SHADE, 0), (TEX0, 0, SHADE, 0)));
    rdpq_mode_blender(RDPQ_BLENDER((IN_RGB, IN_ALPHA, MEMORY_RGB, INV_MUX_ALPHA)));
    rdpq_mode_alphacompare(0);
    rdpq_mode_zbuf(true, false);
    rdpq_mode_filter(FILTER_POINT);

    const float cy = SCREEN_H * 0.5f;

    for (int jn = 0; jn < numrefl; jn++) {
        const refljob_t *j = &refl[jn];
        dt64_tex_t *tex = j->tex;
        const int w  = tex->width, h = tex->height;
        const int tw = w < DT64_TILE_W ? w : DT64_TILE_W;
        const int th = h < DT64_TILE_H ? h : DT64_TILE_H;

        const float iw     = 1.0f / j->depth;   /* affine across a billboard */
        const float x_left = j->sx - (float)tex->leftoffset * j->scale_x;
        const float eh     = cam->z - j->plane_h;

        for (int t0 = 0; t0 < h; t0 += th) {
            const int t1 = t0 + th > h ? h : t0 + th;

            /* Texel rows t0..t1 sit at world z [w_top-t1, w_top-t0]; the
             * mirror sends them to [2H-(w_top-t0), 2H-(w_top-t1)] -- lower
             * rows nearer the waterline. That reversal is the flip: the
             * quad below samples t1 at its top edge and t0 at its bottom. */
            const float zi_lo = 2.0f * j->plane_h - (j->w_top - (float)t0);
            const float zi_hi = 2.0f * j->plane_h - (j->w_top - (float)t1);
            const float y_top = cy - (zi_hi - cam->z) * j->scale_y;
            const float y_bot = cy - (zi_lo - cam->z) * j->scale_y;
            if (y_bot < 0.0f || y_top > (float)SCREEN_H) continue;

            /* Ray-plane crossing depth per edge; one tile spans little
             * enough screen that the hyperbola-vs-linear error stays
             * inside the margins. The bias is +2^-6: reflective flats
             * draw pushed 2^-5 deeper (r_flat.c REFL_PLANE_PUSH), so a
             * ghost lands between the true plane and the pushed surface
             * -- it wins exactly the pixels the mirroring flat wrote and
             * loses same-height non-reflective neighbours, which the
             * crossing depth alone cannot tell apart. Both margins are
             * double the flat pass's worst banding sag (~2^-7). */
            const float dc_hi = j->depth * eh / (cam->z - zi_hi);
            const float dc_lo = j->depth * eh / (cam->z - zi_lo);
            /* MINUS: nearer than the plane, so the ghost wins the pool's
             * own pixels (including its banding sag) and loses to true
             * occluders. The brief +2^-6 era was the z-push design's
             * leftover -- with the push reverted it put every ghost
             * BEHIND its pool and the real RDP culled them all, R7/42
             * submitted and zero visible. Neighbour bleed, the reason
             * the sign once flipped, is the region clip's job now. */
            float z_hi = 1.0f - R_FLAT_NEAR / dc_hi - (1.0f / 64.0f);
            float z_lo = 1.0f - R_FLAT_NEAR / dc_lo - (1.0f / 64.0f);
            if (z_hi > 1.0f) z_hi = 1.0f;
            if (z_lo > 1.0f) z_lo = 1.0f;
            if (z_hi < 0.0f) z_hi = 0.0f;
            if (z_lo < 0.0f) z_lo = 0.0f;

            /* Depth fade, like the walls: the image dims as it reaches
             * down, dying REFL_FADE_T units below the plane -- water's
             * own look, and the soft cap on how far anything ghosts. */
            #define REFL_FADE_T 64.0f
            float a_hi = 0.72f * (zi_hi - (j->plane_h - REFL_FADE_T))
                               * (1.0f / REFL_FADE_T);
            float a_lo = 0.72f * (zi_lo - (j->plane_h - REFL_FADE_T))
                               * (1.0f / REFL_FADE_T);
            if (a_hi > 0.72f) a_hi = 0.72f;
            if (a_lo > 0.72f) a_lo = 0.72f;
            if (a_hi <= 0.0f) continue;
            if (a_lo < 0.0f) a_lo = 0.0f;

            for (int s0 = 0; s0 < w; s0 += tw) {
                const int s1 = s0 + tw > w ? w : s0 + tw;
                const float x0 = x_left + (float)s0 * j->scale_x;
                const float x1 = x_left + (float)s1 * j->scale_x;
                if (x1 < 0.0f || x0 > (float)SCREEN_W) continue;

                dt64_upload_tile(TILE0, tex, NULL, s0, t0, s1, t1);
                stat_uploads++;

                float v[4][10];
                const float xs[4] = { x0, x1, x1, x0 };
                const float ys[4] = { y_top, y_top, y_bot, y_bot };
                const float ss[4] = { (float)s0, (float)s1, (float)s1, (float)s0 };
                const float ts[4] = { (float)t1, (float)t1, (float)t0, (float)t0 };
                const float zs[4] = { z_hi, z_hi, z_lo, z_lo };
                const float as[4] = { a_hi, a_hi, a_lo, a_lo };
                for (int i = 0; i < 4; i++) {
                    v[i][0] = xs[i]; v[i][1] = ys[i];
                    v[i][2] = j->sr; v[i][3] = j->sg; v[i][4] = j->sb;
                    v[i][5] = as[i];
                    v[i][6] = ss[i] * iw; v[i][7] = ts[i] * iw;
                    v[i][8] = iw;
                    v[i][9] = zs[i];
                }
                refl_emit_clipped(cam, v, j->plane_h);
            }
        }
    }
    numrefl = 0;
#if D_DYNLIGHT
    /* The walls over the same pools, in the same mode block. */
    reflect_walls_emit(cam);
#endif
}
#endif /* R_REFLECT */

#if R_REFLECT && D_DYNLIGHT
/* --- wall reflections ---------------------------------------------------
 *
 * The walls standing over a glowing pool mirror in it too. A wall is a
 * vertical quad, so its reflection about z = H is another vertical quad:
 * same endpoints, heights mirrored, rows sampled in reverse from the
 * waterline down. Only the band just under the surface is drawn -- alpha
 * fades to nothing REFL_WALL_FADE units below the waterline, which is how
 * water actually reads, bounds the cost to one TMEM tile per wall, and
 * hides the one deliberate approximation: the tile holds texture columns
 * 0..63 with S wrapping at 64, so art wider than 64 ghosts its first 64
 * columns. A dim ripple-covered reflection does not tell.
 */
#define REFL_WALL_MAX  12
#define REFL_WALL_FADE 40.0f

typedef struct {
    dt64_tex_t *tex;
    float x1, y1, x2, y2;
    float zbot, ztop;        /* wall extent; the image is its mirror */
    float ztex;              /* world height of texture row 0 */
    float u0, len;
    float plane_h;
    float sr, sg, sb;        /* dimmed, pool-tinted, light-scaled */
    uint8_t light;
} rwrefl_t;
static rwrefl_t rwrefl[REFL_WALL_MAX];
/* numrwrefl is declared beside numrefl at the top of the file: the frame
 * reset lives in r_sprite_begin, far above this block. */

void r_reflect_wall_add(const r_wall_t *w)
{
    if (numrwrefl >= REFL_WALL_MAX || !w->tex) return;
    if (w->ztop <= w->glowz) return;          /* nothing above the water */

    /* One wall arrives as several column ranges; keep one ghost. */
    if (numrwrefl) {
        const rwrefl_t *p = &rwrefl[numrwrefl - 1];
        if (p->x1 == w->x1 && p->y1 == w->y1 &&
            p->x2 == w->x2 && p->y2 == w->y2) return;
    }

    rwrefl_t *r = &rwrefl[numrwrefl++];
    r->tex  = w->tex;
    r->x1 = w->x1; r->y1 = w->y1;
    r->x2 = w->x2; r->y2 = w->y2;
    r->zbot    = w->zbot;
    r->ztop    = w->ztop;
    r->ztex    = w->ztex;
    r->u0      = w->u0;
    r->len     = w->len;
    r->plane_h = w->glowz;
    r->light   = w->light;

    const float l = (float)w->light * (1.0f / 255.0f);
    r->sr = l * (0.46f + 0.30f * w->glow_rgb[0]);
    r->sg = l * (0.46f + 0.30f * w->glow_rgb[1]);
    r->sb = l * (0.46f + 0.30f * w->glow_rgb[2]);
}

/* Emit the queued wall ghosts; called from r_reflect_flush inside its mode
 * block. Same z-plane masking as the thing images. */
static void reflect_walls_emit(const r_camera_t *cam)
{
    const float cs = r_view_cs, sn = r_view_sn;
    const float cx = SCREEN_W * 0.5f, cy = SCREEN_H * 0.5f;

    for (int jn = 0; jn < numrwrefl; jn++) {
        const rwrefl_t *r = &rwrefl[jn];
        if (cam->z <= r->plane_h + 1.0f) continue;

        /* Endpoints to camera space, near-clipping the segment; the u
         * coordinate interpolates with the clip so the art stays put. */
        float d1 = (r->x1 - cam->x) * cs + (r->y1 - cam->y) * sn;
        float d2 = (r->x2 - cam->x) * cs + (r->y2 - cam->y) * sn;
        float o1 = (r->x1 - cam->x) * sn - (r->y1 - cam->y) * cs;
        float o2 = (r->x2 - cam->x) * sn - (r->y2 - cam->y) * cs;
        float u1 = r->u0, u2 = r->u0 + r->len;
        const float NEARP = 8.0f;
        if (d1 < NEARP && d2 < NEARP) continue;
        if (d1 < NEARP) {
            const float t = (NEARP - d1) / (d2 - d1);
            d1 += (d2 - d1) * t; o1 += (o2 - o1) * t; u1 += (u2 - u1) * t;
        } else if (d2 < NEARP) {
            const float t = (NEARP - d2) / (d1 - d2);
            d2 += (d1 - d2) * t; o2 += (o1 - o2) * t; u2 += (u1 - u2) * t;
        }

        /* The mirrored band starts at the image of the wall's own BOTTOM
         * -- the waterline only for a wall standing in the surface. An
         * elevated step riser images deeper, with a gap at the plane;
         * anchoring every ghost at the waterline drew staircases at the
         * wrong height with rows sampled below their own bottom edge --
         * the stacked "occluded surface" artifact. With the depth fade
         * this also approximates mirror-world occlusion: a riser 16
         * units up ghosts faint and deep, one past the fade not at all. */
        const float zi_raw = 2.0f * r->plane_h - r->zbot;
        const float zi_top = zi_raw < r->plane_h ? zi_raw : r->plane_h;
        float zi_bot = 2.0f * r->plane_h - r->ztop;
        if (zi_bot < r->plane_h - REFL_WALL_FADE)
            zi_bot = r->plane_h - REFL_WALL_FADE;
        if (zi_bot >= zi_top) continue;

        /* Texture rows, mirrored: the image of world z samples row
         * ztex - (2H - z'), so a wall base continues seamlessly into
         * its reflection. One 32-row tile from there down. */
        const int texh = r->tex->height;
        float t_top = r->ztex - 2.0f * r->plane_h + zi_top;
        float t_bot = r->ztex - 2.0f * r->plane_h + zi_bot;
        /* Rebase into the texture so the upload window is meaningful. */
        {
            const float wrap = rf_floorf(t_top / (float)texh) * (float)texh;
            t_top -= wrap; t_bot -= wrap;
        }
        int win0 = (int)t_top;
        if (win0 < 0) win0 = 0;
        if (win0 > texh - 1) win0 = texh - 1;
        int win1 = win0 + 32 > texh ? texh : win0 + 32;

        const int tw = r->tex->width < 64 ? r->tex->width : 64;
        rdpq_texparms_t tp = {0};
        tp.s.repeats = REPEAT_INFINITE;
        dt64_upload_tile(TILE0, r->tex, &tp, 0, win0, tw, win1);
        stat_uploads++;

        const float eh  = cam->z - r->plane_h;
        const float iw1 = 1.0f / d1, iw2 = 1.0f / d2;
        const float xl  = cx + o1 * cam->focal_x * iw1;
        const float xr  = cx + o2 * cam->focal_x * iw2;

        /* Corner rows: [x,y, r,g,b,a, s,t, invw, z]. Perspective-correct
         * s via invw; the alpha fade to zero at the band's foot is what
         * reads as water. */
        float v[4][10];
        const float za[2] = { zi_top, zi_bot };
        const float ta[2] = { t_top,  t_bot  };
        /* Both edges fade with image depth, not just the foot: an
         * elevated wall's whole band sits deep and starts already dim. */
        const float aa[2] = {
            0.62f * (zi_top - (r->plane_h - REFL_WALL_FADE))
                  * (1.0f / REFL_WALL_FADE),
            0.62f * (zi_bot - (r->plane_h - REFL_WALL_FADE))
                  * (1.0f / REFL_WALL_FADE) };
        for (int c = 0; c < 4; c++) {
            const int side = (c == 1 || c == 2);      /* right column */
            const int row  = (c >= 2);                /* bottom edge  */
            const float d  = side ? d2 : d1;
            const float o_ = side ? o2 : o1;  (void)o_;
            const float iw = side ? iw2 : iw1;
            const float zi = za[row];
            v[c][0] = side ? xr : xl;
            v[c][1] = cy - (zi - cam->z) * cam->focal_y * iw;
            v[c][2] = r->sr; v[c][3] = r->sg; v[c][4] = r->sb;
            v[c][5] = aa[row];
            v[c][6] = (side ? u2 : u1) * iw;
            v[c][7] = ta[row] * iw;
            v[c][8] = iw;
            {
                /* -2^-6, nearer than the plane: see the thing emitter. */
                const float dc = d * eh / (cam->z - zi);
                float z = 1.0f - R_FLAT_NEAR / dc - (1.0f / 64.0f);
                if (z > 1.0f) z = 1.0f;
                if (z < 0.0f) z = 0.0f;
                v[c][9] = z;
            }
        }
        refl_emit_clipped(cam, v, r->plane_h);
    }
    numrwrefl = 0;
}
#endif /* R_REFLECT && D_DYNLIGHT */
