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
#include "r_fastmath.h"
#include "r_light.h"     /* the registry the shine points at */   /* floor/ceil as one FPU op, not a libm call */

#ifndef R_REFLWOBBLE
#define R_REFLWOBBLE 0
#endif

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
    /* Depth value for the z buffer, from the PULLED depth -- see the bias
     * stack in r_flat.h. Carried on the job rather than recomputed in
     * draw_tile, which runs once per tile: the pull needs a divide the
     * texture's own 1/w cannot supply, and a billboard sits at one depth,
     * so it is one divide per sprite instead of one per tile. */
    float       z;
    uint8_t     mipped;
#if R_SPRSHINE
    /* Cylindrical specular pass: see spr_shine_flush. Carries the view
     * azimuth to the thing, which is what slides the highlight as the
     * player walks around it. */
    uint8_t     shine;
    float       vaz;
    float       laz;    /* azimuth TO the dominant light */
#endif
} sprjob_t;

/* 16-aligned for the same reason r_wall.c's quads[] is: the array landed at
 * 8 mod 16, so every job straddled a D-cache line boundary it did not need
 * to. Eight bytes of bss. */
static _Alignas(16) sprjob_t jobs[SPR_MAX_JOBS];
static int      numjobs;
static int      stat_drawn, stat_uploads, stat_dropped;
#if D_HWSTAT
/* Ghosts the size gate turned away, and ghosts that survived it. Extern
 * rather than file-static with an accessor because main.c's window report
 * is the only reader, which is the arrangement r_drop_ and r_ph_ use.
 *
 * These exist because whether the gate does anything is a question about
 * the ROUTE, not about the code, and two verification paths answered it
 * wrongly: abdiff's DEMO walker called the gate a no-op, and ares queues no
 * thing-ghosts at all. The standard attracts say 458 kept against 231
 * turned away -- a third of them. */
int r_refl_small, r_refl_kept;
#endif

/* As r_bsp's STAT, and for the same reason: stat_uploads sits on five
 * separate per-upload paths through the sprite flush and neither it nor
 * stat_drawn has a single caller anywhere in the tree (r_sprite_count and
 * r_sprite_uploads are declared in r_sprite.h and never used), so in a
 * shipping ROM they were counting for nobody. stat_dropped is left alone: it
 * only fires when a job is already being discarded, and main.c's drop
 * telemetry reads it. */
#if D_HWSTAT
#define STAT(x) (x)
#else
#define STAT(x) ((void)0)
#endif

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
    /* Whether the plane below is an actual LIQUID. The reflective set also
     * holds polished DRY surfaces -- teleporter pads, marble, the blue tech
     * plate -- registered with glow 0 for exactly this reason: they mirror,
     * but nothing about them ripples. Only liquids wobble. */
    uint8_t     liquid;
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

/* Each footprint PROJECTED, once per frame.
 *
 * The clip below runs per ghost QUAD, and it used to re-project every
 * footprint inside that loop -- a transform and a near-clip of up to 16
 * points, a reciprocal each, and a winding sum, all recomputed for a result
 * that depends only on the camera and the region. Neither changes during a
 * flush. On a nukage map that is (ghosts x tiles x regions) projections a
 * frame where (regions) would do: the profiling run put liquid reflections at
 * 36-42 ms of the worst E2M2 frames, larger than the BSP walk.
 *
 * Hoisted here, the per-quad work is the Sutherland-Hodgman clip alone. The
 * cached values are exactly what the inner loop computed, so the emitted
 * geometry is bit-identical -- this is a pure loop-invariant hoist, not an
 * approximation. */
typedef struct {
    float px[24], py[24];
    float h;
    float sgn;          /* screen winding: which side of an edge is inside */
    float bx0, bx1, by0, by1;   /* screen bounds of the projected footprint */
    int   n;            /* < 3 means the footprint projected to nothing */
} reflproj_t;
static reflproj_t reflproj[REFL_REGION_MAX];

#if R_REFLWOBBLE
/* A reflection on moving water is not a mirror -- it is a mirror that the
 * surface keeps bending. Now that the liquid itself drifts and swells, a
 * geometrically perfect ghost underneath it reads as glass.
 *
 * The bend is a horizontal displacement that varies with DEPTH BELOW THE
 * PLANE and with time, so the image shears in bands rather than sliding
 * as a block: the further under the surface a row of the image sits, the
 * further the water has carried it sideways. Amplitude is a fraction of
 * the image's own projected width, not a pixel count, so a distant
 * reflection wobbles by as much of itself as a near one does.
 *
 * Driven off the same clock as the liquid's own ripple, so the surface
 * and what it reflects move in sympathy rather than arguing. */
/* Throw in WORLD UNITS, not in fractions of the image.
 *
 * The amplitude used to be a fraction of each image's own projected
 * half-width, for a defensible reason -- it keeps a distant reflection
 * wobbling by as much of itself as a near one does. But the water is ONE
 * SURFACE. Scaling by the reflected object's size means a cacodemon and a
 * shotgun shell floating side by side in the same pool, at the same depth
 * below the same ripple, are displaced by different amounts. It was worse
 * for the wall ghosts, which passed half the projected span BETWEEN THEIR
 * ENDPOINTS: a long wall's reflection swung by a fraction of its whole
 * on-screen length, so the same water bent a 512-unit wall an order of
 * magnitude further than a barrel beside it.
 *
 * A world-space throw scaled by the projection keeps the perspective
 * behaviour the original wanted -- pixels per world unit already falls off
 * as 1/depth, so a distant ripple still shrinks correctly -- while making
 * the displacement a property of the surface rather than of what is being
 * reflected. 2.6 units reproduces the old throw for a typical 40-texel
 * monster, which is what the 0.13 fraction was tuned against. */
#ifndef R_REFLWOB
#define R_REFLWOB     26        /* tenths of a world unit; 0 = no wobble */
#endif
#define WOBBLE_WORLD  ((float)R_REFLWOB * 0.1f)
#define WOBBLE_K      0.055f    /* per world unit of depth below the plane */

static float wobble_t;

static void wobble_update(void)
{
    extern int   leveltime;
    extern float d_subtic;
    /* Halved with the liquid's ripple: the two share a clock, and they
     * have to keep sharing it or the surface and its reflection start
     * moving to different rhythms. */
    wobble_t = ((float)leveltime + d_subtic) * 0.095f;
}

/* How far below the waterline the ripple reaches full throw. */
#define WOBBLE_ANCHOR 22.0f

/* Sideways shift, in the caller's own screen units.
 *
 * ANCHORED AT THE WATERLINE. The shift used to be a plain sine of depth,
 * which is non-zero at depth zero -- so the very row where the reflection
 * meets the thing it reflects slid from side to side, and the image came
 * unstuck from its own object at the pool's edge. A reflection is welded
 * to its source at the surface and free below it: the throw now ramps
 * from nothing at the contact line to full a short way under, which keeps
 * the join fixed while the body of the image still rides the swell. */
/* `px_per_unit` is the projection scale at the image -- focal_x/depth --
 * so the world-space throw above arrives in screen pixels and still falls
 * off with distance. Every image in a pool at a given depth below the
 * surface now shifts by the same amount, whatever its size. */
static inline float wobble_at(float depth_below, float px_per_unit)
{
    float grip = depth_below * (1.0f / WOBBLE_ANCHOR);
    if (grip < 0.0f) grip = 0.0f;
    if (grip > 1.0f) grip = 1.0f;
    return WOBBLE_WORLD * px_per_unit * grip *
           sinf(depth_below * WOBBLE_K + wobble_t);
}
#endif

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

#if R_SPRSHINE
/* Fallback bearing for the highlight, used only where no dynamic light
 * reaches the prop -- there, any direction is equally defensible. */
#define SHINE_LIGHT_AZ  0.9f

/* Armed by the walk for the next thing it queues; see r_sprite_shine_next.
 * CLEARED UNCONDITIONALLY at the top of r_sprite_add, before any early
 * return, because an armed latch that survives a rejected sprite lands on
 * whatever is queued next -- the exact bug the halo scale latch had. */
static bool spr_shine_next;
void r_sprite_shine_next(void) { spr_shine_next = true; }
#endif

void r_sprite_add(const r_camera_t *cam, const r_thing_t *t,
                  const float sh[3], int fog_ll, float alpha)
{
#if R_SPRSHINE
    const bool want_shine = spr_shine_next;
    spr_shine_next = false;
#endif
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

#if R_SPROCCL
    /* Occlusion, not depth. A thing the walk has already walled off is
     * dropped here rather than queued and left to lose a z compare it
     * cannot reliably win -- see r_bsp_occluded for why the z bias alone
     * never could. This is what stops a distant monster drawing through a
     * wall; the bias stack only ever decided how far away it started.
     *
     * Called from inside the BSP walk (r_sprite_add's only caller) so the
     * occlusion state is the front-to-back one this thing is subject to. */
    {
        bool r_bsp_occluded(int x0, int x1, float y0, float y1);
        /* rf_*_i32, not floorf/ceilf: those are real jal libm calls that also
         * drag two cold cache lines into the walk, and the (int) cast then
         * adds a redundant trunc.w.s on the return. floor.w.s rounds toward
         * -inf and yields the integer directly, so this is bit-identical --
         * these are screen columns, bounded far inside int32, and
         * r_bsp_occluded clamps them anyway. */
        if (r_bsp_occluded(rf_floor_i32(sx - halfw), rf_ceil_i32(sx + halfw),
                           top, top + (float)tspr->height * scale_y))
            return;
    }
#endif

    sprjob_t *j = &jobs[numjobs++];

    /* Drop to the half-resolution frame once the sprite is drawn at less than
     * half size: same picture, a quarter of the TMEM tiles and quads. */
    j->tex = (dt64_tex_t *)t->spr;
    j->mipped = 0;
#if R_SPRSHINE
    j->shine = (uint8_t)want_shine;
    /* Azimuth from the eye to the thing. atan2 once per shiny thing, and
     * there are a handful in a room -- barrels and tech pillars. */
    j->vaz = want_shine ? atan2f(dy, dx) : 0.0f;
    /* POINT AT A REAL LIGHT when there is one.
     *
     * The highlight's whole claim is that it is a reflection of the room, so
     * keying it to a fixed compass bearing was a placeholder: it looked
     * plausible in isolation and wrong the moment a torch was visibly on the
     * other side of the barrel. The registry already carries every dynamic
     * light's position, so the dominant one at this thing is a short scan --
     * a handful of lights, and only for the few props that are armed.
     *
     * Ranked by the same falloff the shading query uses, so the light that
     * lights the barrel most is the one it mirrors. The fixed bearing stays
     * as the fallback for rooms with no dynamic light at all, where any
     * direction is equally defensible. */
    j->laz = SHINE_LIGHT_AZ;
#if D_DYNLIGHT
    if (want_shine) {
        float best = 0.0f;
        const int nl = r_light_count();
        for (int li = 0; li < nl; li++) {
            const r_light_t *l = &r_lights[li];
            const float lx = l->x - t->x, ly = l->y - t->y, lz = l->z - t->z;
            const float dd2 = lx * lx + ly * ly + lz * lz;
            float f = 1.0f - dd2 * l->inv_r2;
            if (f <= 0.0f) continue;               /* out of reach */
            f *= l->intensity;
            if (f > best) { best = f; j->laz = atan2f(ly, lx); }
        }
    }
#endif
#endif
    /* Tested against the vertical scale, which is unchanged by wide mode, so
     * sprite LOD picks the same level as the 320 build. Same reasoning as the
     * wall mip test in r_wall.c: keep uploads fixed so a frame-time A/B
     * isolates fill rate. */
    if (tspr->mip && scale_y < 0.5f) { j->tex = tspr->mip; j->mipped = 1; }
    j->depth = depth;
    /* Pulled toward the eye by whole world units, on the flats' own curve
     * -- the bias stack in r_flat.h explains why the pull is world-space
     * and the curve is shared. Clamped rather than allowed to cross the
     * eye: a sprite closer than the pull would otherwise invert through
     * infinity, and everything that near is already at depth 0. */
    {
        /* The near constant FADES with depth, from the sprites' own toward
         * the flats'. See the bias stack in r_flat.h for the shape; what it
         * buys is here.
         *
         * A near offset separates two surfaces by dNEAR/d, so the zone in
         * which a sprite wrongly beats a WALL is a fixed FRACTION of depth
         * -- (NEAR_spr/4.0 - 1) -- and a fixed fraction is a growing number
         * of world units. At the flat 4.5 that is 12.5%: 12 units at depth
         * 100, where the thing is touching the wall and nobody sees it, and
         * 250 units at 2000, where a whole room's depth of monsters draws
         * straight through. That is why it is only ever reported far away.
         *
         * The margin is only NEEDED up close. What it protects is the
         * contact with the floor the thing stands on -- clipped feet -- and
         * that contact is sub-pixel long before the through-wall zone gets
         * large. So the extra margin over the flats decays as D0/(D0 + d):
         * essentially all of it inside a couple of hundred units, nearly
         * none of it out where walls matter.
         *
         * IT CANNOT REACH ZERO, and the floor is not chosen here. Sprites
         * must stay ahead of the FLATS at every depth or the floor wins the
         * tie and takes the feet -- or, for something big and far like a
         * barrel explosion, the whole sprite. The flats themselves lead the
         * walls by (R_FLAT_Z_NEAR/4.0 - 1), so that fraction is the best any
         * sprite bias can do: 7.5% at FLATZ=43. This fade closes the gap
         * between 12.5% and that floor; going below it means lowering FLATZ,
         * which is a different contact and a different lever. */
        float nearc = R_SPR_Z_NEAR;
#if R_SPRFADE
        /* Decays to the WALLS' constant, not the flats'.
         *
         * It used to bottom out at R_FLAT_Z_NEAR, which is where the
         * through-wall artifact lived: the flats deliberately lead the walls
         * by (FLATZ/40 - 1), so a sprite that merely stops leading the FLATS
         * is still ahead of every WALL by 7.5% of its depth -- and a fixed
         * fraction is a growing number of world units, 75 at 1000 and 150 at
         * 2000. No amount of decay toward 4.3 could remove it, because 4.3
         * was already too high.
         *
         * Targeting 4.0 makes a distant sprite tie the walls instead of
         * beating them, which is what stops an enemy showing through one.
         * What it gives up is the floor contact at range: past roughly 180
         * units the constant falls under the flats' and the floor can take
         * the sprite's contact row. That is the right trade -- the audit put
         * the exposure at 0.056 of a screen row, since the floor's z moves
         * ~17.9 LSB per row at eye height, so it is a fraction of one row at
         * the feet of something already small. Up close, where a clipped
         * foot is actually visible, the full margin is still there. */
        nearc = R_FLAT_NEAR + (R_SPR_Z_NEAR - R_FLAT_NEAR) *
                ((float)R_SPRFADE / ((float)R_SPRFADE + depth));
#endif
        const float zd = depth - R_SPR_PULL;
        const float zv = zd > 1.0f ? 1.0f - nearc / zd : 0.0f;
        j->z = zv > 0.0f ? zv : 0.0f;
    }
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
        /* And the near half of it, on the same terms the floor under the
         * thing gets (r_nearlight): an imp two paces away in a dim hall
         * lit at lightlevel/255 while the floor it stands on is lit toward
         * full brightness reads as a cardboard cut-out. Fullbright frames
         * are already at 1.0 and the clamp leaves them there. */
        const float n = fog_ll >= 0 ? r_nearlight(depth) : 0.0f;
        float sr = sh[0] + n, sg = sh[1] + n, sb = sh[2] + n;
        if (sr > 1.0f) sr = 1.0f;
        if (sg > 1.0f) sg = 1.0f;
        if (sb > 1.0f) sb = 1.0f;
        j->sr = sr * k;
        j->sg = sg * k;
        j->sb = sb * k;
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
    /* Ahead of the floor it stands on -- see the bias stack in r_flat.h.
     * This used the shared wall constant, which worked only while the
     * flats sat behind it; once they moved ahead, the floor won the
     * contact and clipped the feet off every standing thing. It then used
     * a near constant of its own, which held the feet but leaked an
     * eighth of the depth over the walls; the pull that replaced it is
     * computed once per job, since every tile of a billboard shares the
     * one depth. */
    const float z  = j->z;

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

#if R_SPRSHINE
/* --- cylindrical shine --------------------------------------------------
 *
 * A barrel is a CYLINDER drawn as a billboard, and that is the whole trick.
 * A billboard has no surface normal to reflect -- every one of them faces the
 * camera, so a real environment map would return the same lookup for every
 * barrel everywhere and read as a flat wash sliding with the view. What a
 * cylinder does have is a KNOWN shape: horizontal position across the sprite
 * is angle around the barrel, u = sin(azimuth - view).
 *
 * So the specular highlight of a fixed environment light on a mirror-ish
 * cylinder lands where the surface normal bisects the view and light
 * directions, which puts it at
 *
 *      u_highlight = sin((theta_light - theta_view) / 2)
 *
 * across the sprite. Walk around a barrel and the band slides over it; stand
 * still and it stays put. That is the entire effect, and it is honest for
 * anything roughly cylindrical and wrong for anything else -- which is why it
 * is opt-in per thing type rather than applied to every sprite.
 *
 * Drawn as a second pass over the same tiles, so the sprite's own alpha does
 * the masking: the combiner takes RGB from SHADE and alpha from TEX0 * SHADE,
 * so a transparent texel contributes nothing and the silhouette costs no
 * extra work. No alpha compare needed -- alpha zero already leaves memory
 * alone under the standard blender.
 */

/* Where the room's light comes from, in world radians. Doom has no
 * directional light to ask, so this is a convention: pick one and let every
 * cylinder agree on it, which is what makes the highlights move together. */
/* Half-width of the band in u, and how hard it reaches its peak. */
#define SHINE_WIDTH     0.55f

#ifndef R_SPRSHINEAMT
#define R_SPRSHINEAMT 38            /* hundredths at the band's centre */
#endif
#define SHINE_AMT ((float)R_SPRSHINEAMT * 0.01f)

/* Shared with the door sheen: R_ENVKNEE is in Doom's 0..255 light units, and
 * a sprite's shade is the same quantity already normalised. */
#ifndef R_ENVKNEE
#define R_ENVKNEE 72
#endif
#ifndef R_ENVFULL
#define R_ENVFULL 176
#endif
#define SHINE_KNEE ((float)R_ENVKNEE / 255.0f)
#define SHINE_FULL ((float)R_ENVFULL / 255.0f)

static void spr_shine_flush(void)
{
    int any = 0;
    for (int i = 0; i < numjobs; i++) if (jobs[i].shine) { any = 1; break; }
    if (!any) return;

    /* RGB from SHADE (the highlight), alpha from TEX0 * SHADE (the sprite's
     * cutout times the band's strength). Same lerp blender the vapor and
     * halo passes use. */
    rdpq_mode_combiner(RDPQ_COMBINER1((0, 0, 0, SHADE), (TEX0, 0, SHADE, 0)));
    rdpq_mode_blender(RDPQ_BLENDER((IN_RGB, IN_ALPHA, MEMORY_RGB, INV_MUX_ALPHA)));
    rdpq_mode_alphacompare(0);
    rdpq_mode_zbuf(true, false);       /* the sprite already wrote its depth */
    rdpq_mode_filter(FILTER_POINT);

    for (int i = 0; i < numjobs; i++) {
        const sprjob_t *j = &jobs[i];
        if (!j->shine) continue;

        const dt64_tex_t *tex = j->tex;
        const int w = tex->width, h = tex->height;
        const int tw = w < DT64_TILE_W ? w : DT64_TILE_W;
        const int th = h < DT64_TILE_H ? h : DT64_TILE_H;
        if (w <= 0 || h <= 0) continue;

        /* Wrapped into (-pi, pi] before halving. vaz comes from atan2, so it
         * jumps by 2*pi as a thing crosses due west -- and the HALF angle has
         * period 4*pi, so that jump NEGATES u_hl instead of leaving it fixed.
         * The highlight swapped ends of the barrel in a single frame, at an
         * azimuth having nothing to do with the light. */
        float daz = j->laz - j->vaz;
        while (daz >  (float)M_PI) daz -= 2.0f * (float)M_PI;
        while (daz < -(float)M_PI) daz += 2.0f * (float)M_PI;
        /* The branch cut moved OFF the muzzle flash.
         *
         * Wrapping daz keeps it in (-pi, pi], but sin(daz/2) is still
         * discontinuous at the ends -- and daz = pi means the light is AT
         * THE EYE, which is exactly a muzzle flash: the brightest thing in
         * the registry, outranking every torch, so it wins the dominant-light
         * scan whenever the player fires. The highlight flipped end to end on
         * alternate frames of every shot.
         *
         * This form is continuous there and puts the cut at daz = 0 instead,
         * where the thing is backlit and the two competing answers are
         * symmetric rim highlights -- a flip nobody can see. */
        const float u_hl = -copysignf(cosf(0.5f * daz), daz);
        /* The opaque pass may have swapped in the half-size mip; its own draw
         * scales by 2 to compensate and this pass did not, so a distant barrel
         * got a half-scale highlight parked in its lower right. */
        const float m    = j->mipped ? 2.0f : 1.0f;
        const float scx  = j->scale_x * m, scy = j->scale_y * m;
        const float x0f  = j->sx - (float)tex->leftoffset * scx;
        const float y0f  = j->sy - (float)tex->topoffset  * scy;
        const float invw = 2.0f / (float)w;      /* texel -> u in [-1,1] */

        for (int t0 = 0; t0 < h; t0 += th) {
            const int t1 = t0 + th > h ? h : t0 + th;
            const float ya = y0f + (float)t0 * scy;
            const float yb = y0f + (float)t1 * scy;
            if (yb < 0.0f || ya > (float)SCREEN_H) continue;

            for (int s0 = 0; s0 < w; s0 += tw) {
                const int s1 = s0 + tw > w ? w : s0 + tw;
                const float xa = x0f + (float)s0 * scx;
                const float xb = x0f + (float)s1 * scx;
                if (xb < 0.0f || xa > (float)SCREEN_W) continue;

                /* Rides the thing's own lighting, on the same knee the door
                 * sheen uses. The shade is genuinely 0..1 here (r_bsp clamps
                 * it), so this needs the threshold and no rescue. It carries
                 * the fog term too, which is wanted: something far enough to
                 * be washed out should not hold a crisp specular. */
                const float lit = j->sr > j->sg ? j->sr : j->sg;
                if (lit <= SHINE_KNEE) continue;
                float klit = (lit - SHINE_KNEE) / (SHINE_FULL - SHINE_KNEE);
                if (klit > 1.0f) klit = 1.0f;
                klit *= SHINE_AMT;

                dt64_upload_tile(TILE0, (dt64_tex_t *)tex, NULL, s0, t0, s1, t1);
                STAT(stat_uploads++);

                /* SUBDIVIDED ACROSS THE SPRITE, not evaluated at the tile's
                 * two edges.
                 *
                 * The band is a quadratic in u and the RDP interpolates shade
                 * linearly, so sampling only at the ends can represent a ramp
                 * but never a peak. That was fatal rather than approximate
                 * here: every armed sprite is narrower than one 64-texel tile
                 * (a barrel is 23 wide), so the only samples were u = -1 and
                 * u = +1 -- the silhouette's edges. The highlight was
                 * therefore ABSENT whenever the model placed it near the
                 * middle of the barrel and full only when it placed it off
                 * the side, the exact inverse of the intent, with a dead arc
                 * of roughly 30% of azimuths. Strips cost vertices, not
                 * uploads: TMEM and the tile above are untouched. */
                enum { SHINE_STRIPS = 8 };
                const float fw = (float)(s1 - s0) / (float)SHINE_STRIPS;
                for (int q = 0; q < SHINE_STRIPS; q++) {
                    const float fa = (float)s0 + fw * (float)q;
                    const float fb = fa + fw;
                    const float ua = fa * invw - 1.0f;
                    const float ub = fb * invw - 1.0f;
                    float ka = 1.0f - (ua - u_hl) * (ua - u_hl) /
                                      (SHINE_WIDTH * SHINE_WIDTH);
                    float kb = 1.0f - (ub - u_hl) * (ub - u_hl) /
                                      (SHINE_WIDTH * SHINE_WIDTH);
                    if (ka < 0.0f) ka = 0.0f;
                    if (kb < 0.0f) kb = 0.0f;
                    if (ka <= 0.0f && kb <= 0.0f) continue;   /* off the band */
                    ka *= klit; kb *= klit;

                    const float qa = x0f + fa * scx;
                    const float qb = x0f + fb * scx;

                    float v[4][10];
                    const float xs[4] = { qa, qb, qb, qa };
                    const float ys[4] = { ya, ya, yb, yb };
                    const float ss[4] = { fa, fb, fb, fa };
                    const float ts[4] = { (float)t0, (float)t0,
                                          (float)t1, (float)t1 };
                    const float as[4] = { ka, kb, kb, ka };
                    for (int k = 0; k < 4; k++) {
                        v[k][0] = xs[k];
                        v[k][1] = ys[k];
                        v[k][2] = v[k][3] = v[k][4] = 1.0f;  /* white */
                        v[k][5] = as[k];
                        v[k][6] = ss[k];
                        v[k][7] = ts[k];
                        v[k][8] = 1.0f;
                        /* A hair nearer than the sprite: the opaque pass
                         * z-WRITES, so testing at an equal value fails at
                         * every pixel. 0.1/d is a near constant a tenth
                         * ahead, and the shine draws nowhere but on its own
                         * sprite so winning is all it can do. */
                        float zs = j->z - 0.1f / j->depth;
                        if (zs < 0.0f) zs = 0.0f;
                        v[k][9] = zs;
                    }
                    r_tri_quad(&TRIFMT_SPR, v[0], v[1], v[2], v[3]);
                    r_tri_spr += 2;
                }
            }
        }
    }
}
#endif /* R_SPRSHINE */

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
                        STAT(stat_uploads++);
                        bound = true;
                    }

                    draw_tile(j, s0, t0, s1, t1, scx, scy, x0, y0);
                }
            }
        }
    }
#if R_SPRSHINE
    /* Cylindrical highlights over the opaque sprites, before the translucent
     * tail changes the mode block out from under them. */
    spr_shine_flush();
#endif

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
                        STAT(stat_uploads++);
                        last_tex = tex; last_s0 = s0; last_t0 = t0;
                    }
                    draw_tile(j, s0, t0, s1, t1, scx, scy, x0, y0);
                }
            }
        }
    }
    STAT(stat_drawn += numjobs);

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
 * bottom instead, leaving the pistol half-buried under the bar.
 *
 * Off the BASE height, not SCREEN_H. This is a coordinate in the 320x240
 * frame that UI_YSCALE magnifies on the way out, so measuring it against
 * the real screen would count the magnification twice: at 480 rows it came
 * to 248, doubled to 496, and put the whole weapon below the bottom edge of
 * a 480-line screen -- the gun and hand simply vanished. */
#define PSPRITE_YSHIFT (SCREEN_BASE_H - 200 - 32)

/* Muzzle-flash opacity. Bright saturated art over the weapon: high
 * enough to read as a flare through composite video, low enough that
 * the gun stays visible under it. */
#define PSPRITE_FLASH_ALPHA 0.65f

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

        /* The flash layer (ps_flash, index 1) is LIGHT, not an object:
         * every weapon's muzzle flash, from the pistol's white to the
         * plasma and BFG's green, is one sprite laid over the gun. Drawn
         * blended it reads as a flare with the weapon showing through
         * instead of a solid cutout pasted on top.
         *
         * COPY mode cannot blend -- it is a straight four-texels-per-clock
         * copy -- so this one layer steps out of the UI bracket's mode
         * into the 1-cycle pipeline and hands COPY back afterwards, for
         * the status bar that follows. Alpha rides PRIM, not SHADE: a
         * texture rectangle carries no shade coefficients. The cutout
         * compare stays on, so fully transparent texels are still
         * discarded before they reach the blender. */
        const int is_flash = (i == 1);
        if (is_flash) {
            rdpq_set_mode_standard();
            rdpq_mode_combiner(RDPQ_COMBINER1((0, 0, 0, TEX0),
                                              (TEX0, 0, PRIM, 0)));
            rdpq_mode_blender(RDPQ_BLENDER((IN_RGB, IN_ALPHA,
                                            MEMORY_RGB, INV_MUX_ALPHA)));
            rdpq_mode_alphacompare(1);
            rdpq_mode_filter(FILTER_POINT);
            rdpq_mode_zbuf(false, false);
            rdpq_mode_tlut(TLUT_RGBA16);
            rdpq_set_prim_color(RGBA32(255, 255, 255,
                                       (int)(PSPRITE_FLASH_ALPHA * 255.0f)));
        }

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
                const float y0 = (oy + (float)t0) * UI_YSCALE;
                const float y1 = (oy + (float)t1) * UI_YSCALE;
                if (x1 < 0.0f || x0 > (float)SCREEN_W) continue;
                if (y1 < 0.0f || y0 > (float)SCREEN_H) continue;

                dt64_upload_tile(TILE0, tex, NULL, s0, t0, s1, t1);
                rdpq_texture_rectangle_scaled(TILE0, x0, y0, x1, y1,
                                              (float)s0, (float)t0,
                                              (float)s1, (float)t1);
                r_tri_spr++;
            }
        }

        /* Hand the bracket's own mode back: D_UI_Draw follows. */
        if (is_flash) { void V_BeginUI(void); V_BeginUI(); }
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
                   float plane_h, const float pool_rgb[3], int liquid)
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

#if R_REFLMINPX > 0
    /* Ghosts too small to read as a reflection.
     *
     * The pass had a reach gate (128 units above the plane) and an
     * off-screen gate, but no SIZE gate -- so an imp across the hall queued
     * a full mirror job, with its own blended quad and its share of the
     * flush, to put a smudge a few pixels tall on the water.
     *
     * The cost this trims is a TAIL, not a steady bill: measured on
     * hardware the reflect submit is ~200 us in the median frame and spikes
     * past 4 ms on the worst ones. That is worth doing because the frame is
     * work-bound rather than display-capped -- the f histogram is unimodal
     * at 14-16 ms, not paired at 16.67/33.3 -- so the long frames are real
     * time and not a wait for the field. It is NOT a median-frame win, and
     * should not be sold as one.
     *
     * A height threshold and not a distance one: what matters is how many
     * pixels the image gets, and a cacodemon at a distance that would hide
     * an imp is still worth mirroring. scale_y is already in hand. */
    if ((float)tspr->height * scale_y < (float)R_REFLMINPX) {
#if D_HWSTAT
        r_refl_small++;
#endif
        return;
    }
#endif

    const float w_top  = t->z + (float)tspr->topoffset;
    const float w_bot  = w_top - (float)tspr->height;
    const float cy     = SCREEN_H * 0.5f;
    /* Image extent, mirrored: z' = 2H - z. */
    const float y_top  = cy - (2.0f * plane_h - w_bot - cam->z) * scale_y;
    if (y_top > (float)SCREEN_H + 2.0f) return;
    const float y_bot  = cy - (2.0f * plane_h - w_top - cam->z) * scale_y;
    if (y_bot < -2.0f) return;

#if D_HWSTAT
    r_refl_kept++;
#endif
    refljob_t *j = &refl[numrefl++];
    j->tex     = (dt64_tex_t *)t->spr;   /* no mip: images are dim and short */
    j->depth   = depth;
    j->sx      = sx;
    j->scale_x = scale_x;
    j->scale_y = scale_y;
    j->w_top   = w_top;
    j->plane_h = plane_h;
    /* Written, not merely declared. The wobble gate reads this and the field
     * sat at its static zero, so thing ghosts stopped rippling over nukage
     * entirely while the wall ghosts behind them still did. */
    j->liquid  = (uint8_t)(liquid != 0);

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

/* Project every queued footprint and take its winding, once for the frame.
 * Called at the top of the flush, before any ghost is clipped. */
static void refl_project_all(const r_camera_t *cam)
{
    for (int i = 0; i < numreflreg; i++) {
        reflproj_t *p = &reflproj[i];
        p->h = reflreg[i].h;
        p->n = refl_project_region(cam, &reflreg[i], p->px, p->py);
        if (p->n < 3) continue;

        /* The footprint's winding on screen decides which side is inside. */
        float area = 0.0f;
        for (int k = 0; k < p->n; k++) {
            const int j = k + 1 < p->n ? k + 1 : 0;
            area += p->px[k] * p->py[j] - p->px[j] * p->py[k];
        }
        p->sgn = area >= 0.0f ? 1.0f : -1.0f;

        /* Screen bounds, so a ghost that lands nowhere near this pool can be
         * rejected before the clip rather than by it. A frame carries up to
         * REFL_REGION_MAX footprints and a given ghost overlaps one or two of
         * them, but every quad was being walked through the full
         * Sutherland-Hodgman against all of them -- one pass per footprint
         * EDGE, each copying 10-float rows. Rejecting on bounds cannot change
         * what is drawn: a quad outside the bounds is outside the polygon, so
         * the clip it skips would have returned nothing. */
        p->bx0 = p->bx1 = p->px[0];
        p->by0 = p->by1 = p->py[0];
        for (int k = 1; k < p->n; k++) {
            if (p->px[k] < p->bx0) p->bx0 = p->px[k];
            if (p->px[k] > p->bx1) p->bx1 = p->px[k];
            if (p->py[k] < p->by0) p->by0 = p->py[k];
            if (p->py[k] > p->by1) p->by1 = p->py[k];
        }
    }
}

/* One Sutherland-Hodgman step over 10-float rows. Rows carry s,t already
 * multiplied by iw, so every attribute is screen-linear and the plain lerp
 * at the cut is perspective-exact. */
int refl_clip_edge(float in[][10], int n, float out[][10],
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

int refl_clip_edge(float in[][10], int n, float out[][10],
                   float ax, float ay, float bx, float by, float sgn);

/* Clip one ghost quad (rows with s,t premultiplied by iw) to every
 * footprint of its plane; emit the surviving fans. A ghost whose plane
 * has no visible footprint emits nothing -- which is also the correct
 * fate of a mirror over a pool the frame never drew. */
static void refl_emit_clipped(float q[4][10], float plane_h)
{
    /* The quad's own bounds, once for all footprints. */
    float qx0 = q[0][0], qx1 = q[0][0], qy0 = q[0][1], qy1 = q[0][1];
    for (int i = 1; i < 4; i++) {
        if (q[i][0] < qx0) qx0 = q[i][0];
        if (q[i][0] > qx1) qx1 = q[i][0];
        if (q[i][1] < qy0) qy0 = q[i][1];
        if (q[i][1] > qy1) qy1 = q[i][1];
    }

    for (int rgi = 0; rgi < numreflreg; rgi++) {
        /* Read the frame's projection instead of redoing it; refl_project_all
         * ran once at the top of the flush. */
        const reflproj_t *rp = &reflproj[rgi];
        if (rp->n < 3) continue;
        if (rp->h > plane_h + 0.5f || rp->h < plane_h - 0.5f) continue;
        /* Disjoint on screen: the clip would return nothing. */
        if (qx1 < rp->bx0 || qx0 > rp->bx1 ||
            qy1 < rp->by0 || qy0 > rp->by1) continue;

        const float *px = rp->px, *py = rp->py;
        const int    pn = rp->n;
        const float  sgn = rp->sgn;

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
/* The blend the door mirror uses: texture alpha times vertex alpha, mixed
 * with what is there, z-tested and never written. */

void r_reflect_flush(const r_camera_t *cam)
{
#if D_DYNLIGHT
    if (!numrefl && !numrwrefl) return;
#else
    if (!numrefl) return;
#endif

#if R_REFLWOBBLE
    wobble_update();
#endif

    /* Every footprint projected ONCE, before any ghost is clipped against
     * them. This is the whole of the reflection optimisation: the clip below
     * runs per ghost quad and used to redo this work inside that loop. */
    refl_project_all(cam);

    /* Blended, z-tested, never z-written. Texture alpha rides through the
     * combiner's alpha channel, so transparent texels leave the memory
     * pixel alone and solid ones mix at the vertex alpha. Point sampling
     * for the same cutout-halo reason as the sprite pass. */
    rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, SHADE, 0), (TEX0, 0, SHADE, 0)));
    rdpq_mode_blender(RDPQ_BLENDER((IN_RGB, IN_ALPHA, MEMORY_RGB, INV_MUX_ALPHA)));
    rdpq_mode_alphacompare(0);
    rdpq_mode_zbuf(true, false);
    rdpq_mode_filter(FILTER_POINT);

#if R_REFLDECAL
    /* DECAL, not a bias. The ghost's z is the depth at which the eye ray
     * meets the water, so it is COPLANAR with the pool by construction --
     * exactly the case this z-mode exists for.
     *
     * The old scheme pushed the ghost a constant in FRONT of the pool so it
     * would win the tie against it. That works, and it also wins against
     * anything else inside the same margin: since the margin is a near
     * offset it covers a fixed FRACTION of depth, so a monster standing in
     * the outer quarter of the distance to a pool was drawn through by its
     * own reflection. A one-sided nearer-than test cannot tell "the pool"
     * from "just in front of the pool"; decal mode tests for coplanarity
     * instead, so the ghost lands on the pool and is occluded by everything
     * genuinely nearer, which is the whole requirement.
     *
     * libdragon warns this is not bulletproof against z-fighting. Here the
     * ghost and the pool are the same plane through the same near constant,
     * which is the best case for it -- but REFLDECAL=0 restores the biased
     * form if a reflection ever flickers. */
    rdpq_mode_zmode(ZMODE_DECAL);
#endif

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

            /* Ray-plane crossing depth per edge -- the depth at which the
             * eye ray to the mirrored point meets the pool, which is where
             * the image belongs. One tile spans little enough screen that
             * the hyperbola-vs-linear error stays inside the margins.
             *
             * (The 2^-5/2^-6 constant-bias scheme this comment used to
             * describe, and r_flat.c's REFL_PLANE_PUSH with it, are gone --
             * see the depth-proportional form below. No such symbol exists
             * in the tree.) */
            const float dc_hi = j->depth * eh / (cam->z - zi_hi);
            const float dc_lo = j->depth * eh / (cam->z - zi_lo);
            /* The ghost must sit NEARER than the pool's own z but by an
             * amount that shrinks with depth, like everything else in
             * z-space. A constant 2^-6 bias swallowed hundreds of world
             * units at distance -- monsters plainly in front of a far
             * pool lost the z-fight and the mirror drew through them.
             *
             * Depth-proportional instead: the pool draws at 1 - FLAT_Z_NEAR/d
             * sagging at most ~0.67/d under its banding, and the ghost at
             * 1 - REFL_Z_NEAR/dc with REFL_Z_NEAR one whole unit ahead --
             * so it undercuts the sag with margin at every distance. That
             * part is right by construction and tracks FLATZ automatically.
             *
             * WHAT DOES NOT TRACK is the margin over everything ELSE, and
             * these numbers were written when FLATZ was 3.5 and the ghost
             * 4.5. At 4.3 and 5.3 the ghost's lead over the WALLS' 4.0 grew
             * from 1.125x to 1.325x, so an occluder must now be nearer than
             * 4.0/5.3 = 75% of the pool's crossing depth to win, where it
             * used to be 8/9 = 89%. The zone in which the mirror wrongly
             * draws over something standing in front of it went from 11% of
             * the pool's depth to 25% -- the very artifact the paragraph
             * above says was fixed.
             *
             * It cannot be tuned out here: the +1.0 is load-bearing against
             * the banding sag, so the ghost is pinned to FLAT_Z_NEAR + 1 and
             * its lead over the walls is whatever FLATZ makes it. Lowering
             * FLATZ is the lever, and it pays three ways at once -- flats,
             * sprites (see the bias stack in r_flat.h) and this. */
            float z_hi = 1.0f - R_REFL_Z_NEAR / dc_hi;
            float z_lo = 1.0f - R_REFL_Z_NEAR / dc_lo;
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

#if R_REFLWOBBLE
            /* One shift per edge of the band, so the row is not merely
             * moved but sheared -- which is what carries the eye from one
             * band to the next as a continuous wave rather than a stack
             * of offset strips. */
            /* Liquids only. A polished floor reflects without rippling --
             * see the note on `liquid` in refljob_t. */
            /* scale_x IS pixels per world unit at this billboard's depth. */
            const float wob_hi = j->liquid
                ? wobble_at(j->plane_h - zi_hi, j->scale_x) : 0.0f;
            const float wob_lo = j->liquid
                ? wobble_at(j->plane_h - zi_lo, j->scale_x) : 0.0f;
#else
            const float wob_hi = 0.0f, wob_lo = 0.0f;
#endif

            for (int s0 = 0; s0 < w; s0 += tw) {
                const int s1 = s0 + tw > w ? w : s0 + tw;
                const float x0 = x_left + (float)s0 * j->scale_x;
                const float x1 = x_left + (float)s1 * j->scale_x;
                if (x1 + wob_hi < 0.0f && x1 + wob_lo < 0.0f) continue;
                if (x0 + wob_hi > (float)SCREEN_W &&
                    x0 + wob_lo > (float)SCREEN_W) continue;

                dt64_upload_tile(TILE0, tex, NULL, s0, t0, s1, t1);
                STAT(stat_uploads++);

                float v[4][10];
                const float xs[4] = { x0 + wob_hi, x1 + wob_hi,
                                      x1 + wob_lo, x0 + wob_lo };
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
                refl_emit_clipped(v, j->plane_h);
            }
        }
    }
    numrefl = 0;
#if D_DYNLIGHT
    /* The walls over the same pools, in the same mode block -- and so under
     * the same decal z-mode, which they want for the same reason. */
    reflect_walls_emit(cam);
#endif
#if R_REFLDECAL
    /* Hand the standard z-mode back. Render modes persist across passes, and
     * the sky and vapor that follow are NOT coplanar decals -- leaving decal
     * set would have them fight the geometry they are meant to sit behind. */
    rdpq_mode_zmode(ZMODE_STANDARD);
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
    uint8_t liquid;          /* ripples only over an actual liquid */
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
    /* glow is 0 for the polished non-liquids and non-zero for sludge, lava
     * and blood -- the wall struct already carries the distinction. */
    r->liquid  = (uint8_t)(w->glow > 0.0f);
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
        STAT(stat_uploads++);

        const float eh  = cam->z - r->plane_h;
        const float iw1 = 1.0f / d1, iw2 = 1.0f / d2;
        const float xl  = cx + o1 * cam->focal_x * iw1;
        const float xr  = cx + o2 * cam->focal_x * iw2;

        /* Corner rows: [x,y, r,g,b,a, s,t, invw, z]. Perspective-correct
         * s via invw; the alpha fade to zero at the band's foot is what
         * reads as water. */
#if R_REFLWOBBLE
        /* The wall's ghost bends with the same water, and by the same
         * amount as anything else floating in it -- the projection scale
         * across the band, not the wall's own on-screen length. */
        const float pxu_w = cam->focal_x * (iw1 + iw2) * 0.5f;
        const float wob_t_ = r->liquid
            ? wobble_at(r->plane_h - zi_top, pxu_w) : 0.0f;
        const float wob_b_ = r->liquid
            ? wobble_at(r->plane_h - zi_bot, pxu_w) : 0.0f;
#else
        const float wob_t_ = 0.0f, wob_b_ = 0.0f;
#endif

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
            v[c][0] = (side ? xr : xl) + (row ? wob_b_ : wob_t_);
            v[c][1] = cy - (zi - cam->z) * cam->focal_y * iw;
            v[c][2] = r->sr; v[c][3] = r->sg; v[c][4] = r->sb;
            v[c][5] = aa[row];
            v[c][6] = (side ? u2 : u1) * iw;
            v[c][7] = ta[row] * iw;
            v[c][8] = iw;
            {
                /* Depth-proportional bias: see the thing emitter. */
                const float dc = d * eh / (cam->z - zi);
                float z = 1.0f - R_REFL_Z_NEAR / dc;
                if (z > 1.0f) z = 1.0f;
                if (z < 0.0f) z = 0.0f;
                v[c][9] = z;
            }
        }
        refl_emit_clipped(v, r->plane_h);
    }
    numrwrefl = 0;
}
#endif /* R_REFLECT && D_DYNLIGHT */
