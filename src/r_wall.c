#include "r_wall.h"
#include "r_fastmath.h"
#include "r_tri.h"
#include "r_light.h"

/* Bilinear filtering on paletted textures.
 *
 * This was point-sampled on the assumption that filtering a CI8 texture would
 * interpolate palette *indices* and land on unrelated colours. That is wrong
 * on this hardware: the RDP resolves each texel through the TLUT before the
 * filter unit sees it, so the blend happens in colour space and comes out
 * correct. Measured free -- the frame time does not move.
 *
 * Sprites stay point-sampled deliberately, and that reasoning does hold there:
 * their transparency is a reserved palette index, and blending it against its
 * neighbours would ring the cutout edges with halos. */
#ifndef R_BILINEAR
#define R_BILINEAR 1
#endif

#include <math.h>

/* Anything closer than this is clipped away. Projection divides by depth, so
 * the near plane exists to keep that division bounded, not to save fill rate. */
#define NEAR_PLANE 4.0f

/* Distance range over which sector light falls off to black. This stands in
 * for Doom's light-diminishing tables: rather than per-column lookups, the
 * falloff is evaluated per vertex and interpolated by the RDP for free. */
/* Tuned for real Doom sight lines rather than a 512-unit test room: E1M1
 * alone has views well past 2000 units. Doom's actual diminishing depends on
 * sector light level -- brighter sectors see further -- which this flat ramp
 * does not yet model. */
#define FOG_NEAR  512.0f
#define FOG_FAR   3500.0f

/* Vertex layout: {X,Y, R,G,B,A, S,T,INV_W, Z}.
 *
 * The Z component exists for the flats' sake. Walls alone need no depth
 * buffer -- BSP order plus solid-segment clipping sorts them exactly -- but
 * floors cannot be ordered that way, and once the depth buffer is on, walls
 * have to write consistent depth or they will be overdrawn by floors behind
 * them. */
/* Compact batch vertex: x, y, shade, s, t, 1/w. The RDP wants the rdpq
 * ten-float layout (rgb triplicated, alpha, z), but carrying that through
 * the batch tripled the arrays' cache footprint against an 8 KB D-cache.
 * The flush expands each quad into a stack-local ten-float block right at
 * submission; shade fans out to r=g=b there and z re-derives from 1/w with
 * the same 1 - NEAR/w mapping the flats use. */
#define VTX_STRIDE 6

static const rdpq_trifmt_t TRIFMT_WALL = {
    .pos_offset   = 0,
    .shade_offset = 2,
    .tex_offset   = 6,
    .tex_tile     = TILE0,
    .z_offset     = 9,
};

/* --- batch ---------------------------------------------------------------
 *
 * Wall quads are accumulated rather than drawn immediately, so that flushing
 * can group them by TMEM tile. Drawing as we go costs one texture upload per
 * quad; grouping costs one per *distinct tile actually used*, which is a small
 * fixed number (a 128x128 texture has 8 tiles) no matter how many walls sample
 * it. That ratio is the whole point: TMEM uploads, not triangles, are the
 * limiting resource. */

/* 384 quads is about 55 KB. Sized to cover a dense view of a real level
 * (roughly 100 visible segments x 2-8 tiles) while staying affordable in a
 * 4 MB machine. Overflow is counted, never silently ignored. */
#define R_MAX_QUADS    384
/* A real level view touches far more textures than a test room: E1M1 uses
 * 34 across the map and a dozen or more in a single frame. */
#define R_MAX_TEXTURES 64

#if R_TRI_QUANT
/* Quantized record: the RSP-format integers tri_vertex6 used to produce at
 * flush, produced at push instead -- where the depth and its reciprocal
 * are both already live, so the flush's per-quad divides die and its body
 * becomes a pure copy of command words. A wall quad's corners carry only
 * TWO distinct depths (0,3 share edge a; 1,2 share edge b), which is the
 * record's shape: per-corner xy/st, per-edge w/invw/z (and shade when it
 * cannot vary per corner). 80 bytes = 5 D-cache lines against the float
 * record's 7; the DYNLIGHT variant carries per-corner shade in 96 = 6. */
typedef struct {
    const dt64_tex_t *tex;
    uint16_t s0, t0;                 /* TMEM tile origin, in texture space */
    uint16_t s1, t1;                 /* exclusive, clamped to the texture */
    uint8_t  texidx;                 /* index into batch_tex, for bucketing */
    uint8_t  wrap;                   /* whole texture resident, sampled wrapped */
    uint8_t  masked;                 /* alpha-cut fence/grate quad */
    uint8_t  pad0;                   /* written, never read */
    uint32_t xy[4];                  /* (x16<<16)|(y16&0xFFFF), s11.2 */
    uint32_t st[4];                  /* (s16<<16)|(t16&0xFFFF), s10.5 */
#if D_DYNLIGHT
    uint32_t rgba[4];                /* per-corner, tint folded in at push */
    int32_t  w_a, invw_a; uint32_t zw_a, pad_a;
    int32_t  w_b, invw_b; uint32_t zw_b, pad_b;
#else
    int32_t  w_a, invw_a; uint32_t zw_a, rgba_a;
    int32_t  w_b, invw_b; uint32_t zw_b, rgba_b;
#endif
} r_quad_t;

#ifdef N64
#if D_DYNLIGHT
_Static_assert(sizeof(r_quad_t) == 96, "quantized quad must stay 6 D-cache lines");
#else
_Static_assert(sizeof(r_quad_t) == 80, "quantized quad must stay 5 D-cache lines");
#endif
#endif

#else /* !R_TRI_QUANT: float record, converted by the reference path */

typedef struct {
    const dt64_tex_t *tex;
    uint16_t s0, t0;                 /* TMEM tile origin, in texture space */
    uint16_t s1, t1;                 /* exclusive, clamped to the texture */
    uint8_t  texidx;                 /* index into batch_tex, for bucketing */
    uint8_t  wrap;                   /* whole texture resident, sampled wrapped */
    uint8_t  masked;                 /* alpha-cut fence/grate quad */
    float    v[4][VTX_STRIDE];
} r_quad_t;

#endif /* R_TRI_QUANT */

/* The Create-Dirty-Exclusive append path (batch_push) depends on both of
 * these: a whole number of 16-byte lines per record, starting on a line
 * boundary. A size change would make CDE on a quad's first line clobber
 * the previous quad's tail. (N64 only: host pointers are wider.) */
#if defined(N64) && !R_TRI_QUANT
_Static_assert(sizeof(r_quad_t) == 112, "r_quad_t must stay 7 D-cache lines");
#endif
#ifdef N64
_Static_assert((sizeof(r_quad_t) & 15) == 0, "quad record must be whole lines");
#endif

/* --- column spans --------------------------------------------------------
 *
 * A wall drawn as one texture rectangle per screen column, which is how Doom
 * has always drawn them and why it held its framerate on far weaker hardware.
 *
 * The reason it is not an approximation: every pixel in a vertical wall column
 * is at the same depth, so the texture mapping down that column is affine.
 * Perspective only enters *across* columns, and that is handled by solving for
 * the wall parameter per column. Doom's renderer is built on this identity.
 *
 * The reason it is fast: a texture rectangle is a fixed-function RDP primitive
 * with no edge slopes and no gradients -- measured at 3.8 us against 37.9 us
 * for a triangle. And because occlusion is resolved before drawing, the column
 * count is bounded by the screen width no matter how complex the level is.
 * Triangle count is not.
 *
 * Spans are batched by texture exactly as quads were: the whole (mip) texture
 * has to be resident for a column to sample an arbitrary texel of it, so one
 * upload has to serve every column that uses it.
 *
 * MEASURED AND REJECTED. A bare texture rectangle benchmarks at 3.8 us against
 * 37.9 us for a triangle, but a *one-pixel-wide* one costs 19 us: the RDP has
 * real per-primitive setup that a 1x240 rectangle cannot amortise, where the
 * benchmark's 24x24 could. 1100 columns came to 21 ms against 8.4 ms for the
 * 222 triangles they replaced.
 *
 * This is the difference between the RDP and the Jaguar's blitter, which is
 * what makes Doom's column renderer fast there and not here. Kept compiled out
 * because the negative result is worth more than the code. */
typedef struct {
    const dt64_tex_t *tex;
    uint8_t texidx;
    float   xa, xb;          /* screen column range */
    float   iza, izb;        /* 1/depth at each end -- linear in screen x */
    float   uoza, uozb;      /* parameter/depth at each end -- also linear */
    float   ztop, zbot;      /* world heights */
    float   ttop, tbot;      /* texel rows, constant down the wall */
    float   u0, wall_len;    /* to turn a parameter into a texel column */
    float   uscale;          /* world units to texels at the chosen mip */
    float   light;
} r_span_t;

#define R_MAX_SPANS 256

#if R_COLUMNS
static r_span_t spans[R_MAX_SPANS];
#endif
static int      num_spans;

/* Line-aligned by declaration, not linker accident: the CDE path in
 * batch_push must never see a quad straddling an extra line. */
static _Alignas(16) r_quad_t quads[R_MAX_QUADS];
static int               num_quads;
static const dt64_tex_t *batch_tex[R_MAX_TEXTURES];
static int               num_batch_tex;

/* Grouping key per quad, (texidx << 8) | tile ordinal, kept OUTSIDE the
 * 112-byte quad record on purpose: the flush's grouping passes touch only
 * this dense array (two bytes per quad, eight per D-cache line) instead of
 * dragging a 16-byte line of every quad header through the cache once per
 * tile probe. The ordinal is t-major (ty * tiles_x + tx), which is exactly
 * the (t0, s0) order the old grid scan visited, so sorting by this key
 * reproduces the old upload/draw sequence verbatim. */
static uint16_t q_key[R_MAX_QUADS];

#if D_DYNLIGHT && !R_TRI_QUANT
/* Per-quad light tint, 0x00RRGGBB, 0x00FFFFFF neutral -- a side array for
 * the same reason as q_key: the quad record is size-sacred (CDE).
 * Brightness stays per-corner in the shade float; only HUE is per-quad,
 * which bounds the approximation (see r_light_tint). num_tinted lets the
 * flush skip every read on frames nothing coloured touches. INVARIANT:
 * whenever DYNLIGHT is on, EVERY push writes its entry (neutral or not) --
 * a missed store would read a stale tint from a prior frame. (Quantized
 * builds fold the tint into the pushed shade words instead.) */
static uint32_t q_tint[R_MAX_QUADS];
static int      num_tinted;
#endif

/* One-entry texture memo for batch_push: quads arrive texture-batched per
 * wall, so the previous resolution almost always answers the next push
 * without the linear scan. Reset alongside the batch itself. */
static const dt64_tex_t *push_memo_tex;
static uint8_t           push_memo_ti;

int r_tri_wall, r_tri_flat, r_tri_spr, r_tri_sky;

static float cam_focal_y, cam_z;

/* Shared with r_flat, r_sprite and r_bsp: every per-frame consumer of the
 * camera basis reads these two floats rather than calling newlib's software
 * cosf/sinf again -- draw_one alone was paying that pair ~90 times a frame. */
float r_view_cs = 1.0f, r_view_sn = 0.0f;

void r_set_view(const r_camera_t *cam)
{
    r_view_cs = cosf(cam->angle);
    r_view_sn = sinf(cam->angle);
}

static int tmem_uploads;
static int quads_dropped;
static int quads_emitted;

static inline float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* Narrow a parametric range against a linear constraint c(u) = a + b*u >= 0.
 * Returns false when nothing survives. */
static inline bool clip_range(float *lo, float *hi, float a, float b)
{
    if (fabsf(b) < 1e-9f) return a >= 0.0f;     /* constraint independent of u */

    const float root = -a / b;
    if (b > 0.0f) { if (root > *lo) *lo = root; }
    else          { if (root < *hi) *hi = root; }
    return *lo < *hi;
}

/* Clamp one vertical edge to the screen, carrying the texture coordinate with
 * it. This is exact rather than approximate: at a fixed depth, screen Y is an
 * affine function of world Z, and texture T is affine in Z too, so T is affine
 * in Y and moving it proportionally introduces no distortion.
 *
 * Necessary because near geometry projects far off-screen vertically -- a wall
 * 4 units away puts its top edge at y = -3360 -- and RDP triangle coordinates
 * are s11.2, so anything beyond about +/-2047 wraps and rasterises garbage
 * across the frame. */
/* Half a pixel of vertical bleed where a wall meets a floor or ceiling.
 *
 * The two surfaces share an edge geometrically but not structurally: the wall
 * spans its seg's endpoints, while the floor polygon's edge along that same
 * seg comes out of BSP clipping and can carry an extra vertex from some other
 * partition. Collinear edges with different vertices are a T-junction, and the
 * rasteriser does not fill them identically -- the result is a hairline of
 * background along the join. Overlapping the wall slightly hides it, and costs
 * nothing now that a depth buffer decides what wins.
 *
 * Applied only at a wall's outer edges. Bleeding internal tile-row boundaries
 * would make two quads at equal depth fight over the same pixels. */
#define WALL_BLEED 0.5f

/* Clamp a quad's corners to the vertical bound without rotating any
 * visible edge.
 *
 * The previous form moved each corner independently to the tightest bound,
 * which is not clipping: a corner pulled to the bound leaves the straight
 * top or bottom edge drawn between a MOVED near corner and a TRUE far
 * corner. The border of a step rotates on screen, stops being parallel
 * with its neighbours and visibly intersects them at grazing angles, and
 * the texture rides along with the moved vertex. An edge is moved only
 * when BOTH its ends are past the same bound -- that strip of the quad is
 * fully hidden, so collapsing it is invisible; a mixed edge keeps its true
 * geometry, and the scissor discards the off-screen rows while the
 * z-buffer rejects the occluded ones. Correctness costs a little hidden
 * fill on the rare close-up quad.
 *
 * The RDP's coordinate range is the one bound that must always hold:
 * corners clamp per-edge against +/-RDP_Y_GUARD, safely inside the s11.2
 * limit, where the straight-line deformation lies far off-screen. T stays
 * affine in Y along each vertical edge for every move. */
#define RDP_Y_GUARD 1536.0f

static inline void clamp_quad(float *ya_t, float *ya_b, float *ta_t, float *ta_b,
                              float *yb_t, float *yb_b, float *tb_t, float *tb_b,
                              float ylo, float yhi)
{
    if (*ya_t >= ylo && *yb_t >= ylo && *ya_b <= yhi && *yb_b <= yhi) return;

    const float dya = *ya_b - *ya_t;
    const float dta = dya > 1e-6f ? (*ta_b - *ta_t) / dya : 0.0f;
    const float dyb = *yb_b - *yb_t;
    const float dtb = dyb > 1e-6f ? (*tb_b - *tb_t) / dyb : 0.0f;

    if (*ya_t < -RDP_Y_GUARD) { *ta_t += dta * (-RDP_Y_GUARD - *ya_t); *ya_t = -RDP_Y_GUARD; }
    if (*yb_t < -RDP_Y_GUARD) { *tb_t += dtb * (-RDP_Y_GUARD - *yb_t); *yb_t = -RDP_Y_GUARD; }
    if (*ya_b >  RDP_Y_GUARD) { *ta_b += dta * ( RDP_Y_GUARD - *ya_b); *ya_b =  RDP_Y_GUARD; }
    if (*yb_b >  RDP_Y_GUARD) { *tb_b += dtb * ( RDP_Y_GUARD - *yb_b); *yb_b =  RDP_Y_GUARD; }

    if (*ya_t < ylo && *yb_t < ylo) {
        *ta_t += dta * (ylo - *ya_t); *ya_t = ylo;
        *tb_t += dtb * (ylo - *yb_t); *yb_t = ylo;
    }
    if (*ya_b > yhi && *yb_b > yhi) {
        *ta_b += dta * (yhi - *ya_b); *ya_b = yhi;
        *tb_b += dtb * (yhi - *yb_b); *yb_b = yhi;
    }
}

/* Fraction of a surface's light that survives to the eye at a given depth:
 * 1 at R_FOG_NEAR, falling to 0 at the far distance the caller's LUT entry
 * encodes -- r_vis in r_wall.h, fed from r_fog_inv[light]. With FOGSCALE=1
 * the far distance scales with sector brightness the way vanilla's
 * diminishing tables did: bright rooms see far, dim rooms close in. The
 * LUT is built once below; entry 255 is bit-identical to the old fixed
 * 512..3500 ramp, so full-bright sectors cannot regress. */
float r_fog_inv[256];

static void fog_lut_build(void)
{
    static bool built;
    if (built) return;
    built = true;
    for (int i = 0; i < 256; i++) {
#if R_FOGSCALE
        /* i/255.0f: division makes 255/255 exactly 1.0f, so entry 255's
         * far is exactly FOG_FAR and its inverse is the legacy constant. */
        const float far = FOG_FAR * (0.25f + 0.75f * ((float)i / 255.0f));
#else
        const float far = FOG_FAR;
#endif
        r_fog_inv[i] = 1.0f / (far - R_FOG_NEAR);
    }
}

/* Legacy fixed-ramp form, kept only for the compiled-out R_COLUMNS span
 * path, which carries its light as a float and predates the LUT. */
static inline float visibility_at(float depth)
{
    const float fog = clamp01((depth - FOG_NEAR) * (1.0f / (FOG_FAR - FOG_NEAR)));
    return 1.0f - fog;
}

/* Sector light plus whatever dynamic lights reach this corner, then the
 * distance falloff.
 *
 * The dynamic term is added BEFORE the falloff, not after, because a moving
 * light illuminates the surface and that light then travels to the eye like
 * any other: a fireball across a large hall should not stay at full strength
 * just because it is bright at its source. Adding it afterwards would also let
 * it punch through fog, which reads as a glow floating in front of the wall.
 *
 * With DYNLIGHT=0 r_light_at is a constant zero, so this folds back to
 * light * visibility_at(depth) -- exactly the expression it replaced. */
/* How far up a wall an emissive floor throws light, in world units. About
 * waist height: far enough to read as a glow on the surface, short enough that
 * it does not flatten the whole wall into one brightness. */
#define GLOW_RISE 96.0f

static inline float lit_shade(float light, float depth, float finv,
                              float wx, float wy, float wz,
                              const r_wall_t *wall)
{
    float l = light + r_light_at(wx, wy, wz);

    /* Sludge and lava light the wall they lap against. Linear in height and
     * one-sided -- nothing below the floor, nothing above the reach -- which
     * costs a compare and a multiply-add on the rare wall that has it.
     *
     * Guarded because the fields themselves are: carrying two floats per wall
     * that nothing reads still costs the zero-stores on every wall built. */
#if D_DYNLIGHT
    if (wall->glow > 0.0f) {
        const float h = wz - wall->glowz;
        if (h >= 0.0f && h < GLOW_RISE)
            l += wall->glow * (1.0f - h * (1.0f / GLOW_RISE));
    }
#else
    (void)wall;
#endif

    if (l > 1.0f) l = 1.0f;
    return l * r_vis(depth, finv);
}

#if D_DYNLIGHT
/* Hue for one batch quad, evaluated at its centre: sector light enters as
 * white, the emissive glow with its own colour at the centre height (same
 * ramp lit_shade applies per corner), and every reachable point light with
 * its own. Brightness stays per-corner in the shade floats; only the hue
 * is quad-constant, which is what a one-shade-channel batch can carry
 * without growing the size-sacred record. */
static inline uint32_t quad_tint(const r_wall_t *wall, float light,
                                 float wx, float wy, float wz)
{
    if (r_light_num == 0 && wall->glow <= 0.0f) return 0x00FFFFFFu;

    float gadd = 0.0f;
    if (wall->glow > 0.0f) {
        const float h = wz - wall->glowz;
        if (h >= 0.0f && h < GLOW_RISE)
            gadd = wall->glow * (1.0f - h * (1.0f / GLOW_RISE));
    }
    return r_light_tint(wx, wy, wz, light, gadd, wall->glow_rgb);
}
#endif

void r_setup_walls(void)
{
    fog_lut_build();
    tmem_uploads  = 0;
    quads_dropped = 0;
    r_tri_wall = r_tri_flat = r_tri_spr = r_tri_sky = 0;
    quads_emitted = 0;
    num_quads     = 0;
    num_spans     = 0;
    num_batch_tex = 0;
    push_memo_tex = NULL;
#if D_DYNLIGHT && !R_TRI_QUANT
    num_tinted    = 0;
#endif
}

/* RDP state for the wall pass. Separate from r_setup_walls so that the whole
 * BSP walk stays pure CPU work: the frame loop decides what to clear from
 * what the walk found (see the sky span), so nothing may reach the RDP before
 * the walk finishes. Called by r_flush_walls. */
static void r_wall_modes(void)
{
    rdpq_set_mode_standard();

    /* CI8 texels are looked up through the shared PLAYPAL palette. */
    rdpq_mode_tlut(TLUT_RGBA16);

    /* Perspective-correct texturing. This is NOT on by default:
     * rdpq_set_mode_standard leaves SOM_TEXTURE_PERSP clear, because most 2D
     * sprite work does not want it. Without it the RDP packs the 1/w this
     * renderer supplies and then never divides by it, giving affine mapping --
     * textures that swim on any surface at an angle, and that mismatch between
     * adjacent tile columns because each column gets its own linear
     * approximation. Tiling narrow enough hides it, which is exactly why it
     * survived a test room and two wrong diagnoses. It costs nothing. */
    rdpq_mode_persp(true);

#if R_PIPELINE == 1
    /* Bisect aid: raw texels, no shading at all. */
    rdpq_mode_combiner(RDPQ_COMBINER_TEX);
#else
    /* RGB = texture * shade, where shade already carries both sector light and
     * distance falloff. Alpha is left alone; nothing downstream consumes it.
     *
     * Deliberately NOT using rdpq_mode_fog here. The RDP's fog unit lives in
     * the blender, and enabling it forces 2-cycle mode -- where the RDP samples
     * both TILE0 and TILE1 every pixel, while rdpq_tex_upload_sub documents
     * that it may corrupt tile+1 to speed up uploads. Since this renderer
     * re-uploads a tile per wall segment, that hazard fires constantly and the
     * second cycle reads garbage.
     *
     * Folding the falloff into shade avoids the hazard entirely, keeps the RDP
     * in 1-cycle mode (roughly double the fill rate, which is the binding
     * constraint here), and is exact for Doom: its light diminishing fades to
     * black, which is what multiplying by a scalar does. Fog toward a non-black
     * colour, Doom 64 style, would need the blender and a TILE1 fix. */
    /* Alpha rides TEX0 so masked mids can cut; opaque textures carry alpha
     * one everywhere and the compare is off for them, so nothing changes. */
    rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, SHADE, 0), (0, 0, 0, TEX0)));
#endif

    /* Point sampling is mandatory for CI8, not a stylistic choice. Bilinear
     * filtering interpolates palette *indices* rather than colours, and Doom's
     * PLAYPAL is a set of unrelated ramps, so a blend between two indices
     * lands on an arbitrary third colour. Flat areas survive it; detailed ones
     * turn to mud. It also happens to be what Doom is supposed to look like. */
    rdpq_mode_filter(R_BILINEAR ? FILTER_BILINEAR : FILTER_POINT);

    /* Depth test and write. Walls would not need this on their own, but they
     * share the frame with depth-buffered floors. */
    rdpq_mode_zbuf(true, true);
    rdpq_mode_alphacompare(0);
    r_tri_group_begin();
}

int r_tmem_uploads(void)
{
    return tmem_uploads;
}

int r_quads_dropped(void)
{
    return quads_dropped;
}

int r_quads_emitted(void) { return quads_emitted; }

/* Append one quad to the batch, remembering its texture so the flush knows
 * which tile grids to walk. */
static r_quad_t *batch_push(const dt64_tex_t *tex,
                            int s0, int t0, int s1, int t1)
{
    if (num_quads >= R_MAX_QUADS) { quads_dropped++; return NULL; }

    int ti;
    if (tex == push_memo_tex) {
        ti = push_memo_ti;                   /* the common, batched case */
    } else {
        ti = -1;
        for (int i = 0; i < num_batch_tex; i++)
            if (batch_tex[i] == tex) { ti = i; break; }
        if (ti < 0) {
            if (num_batch_tex >= R_MAX_TEXTURES) { quads_dropped++; return NULL; }
            ti = num_batch_tex;
            batch_tex[num_batch_tex++] = tex;
        }
        push_memo_tex = tex;
        push_memo_ti  = (uint8_t)ti;
    }

    /* Tile ordinal straight from the origin: the grid only ever places
     * origins at multiples of the full 64x32 tile (narrower/shorter
     * textures collapse to column/row zero), so the shifts are exact. */
    const unsigned ord = (((unsigned)t0 >> 5) * tex->tiles_x)
                       + ((unsigned)s0 >> 6);
    assertf(ord < 256, "tile ordinal %u overflows the flush key", ord);

    quads_emitted++;
    q_key[num_quads] = (uint16_t)(((unsigned)ti << 8) | ord);
    r_quad_t *q = &quads[num_quads++];

#if defined(N64) && R_BATCH_CDE
    /* Create Dirty Exclusive on the record's lines before filling it: a
     * store miss otherwise fetches each line from RDRAM only to overwrite
     * every byte of it. CDE allocates the line dirty with no fetch
     * (writing back any dirty prior occupant first, per the manual), so
     * the append costs stores alone. Requires what the asserts above pin:
     * whole-line records on a 16-aligned array, and that every field (bar
     * named pads, which nothing reads) is written below or by the caller.
     * The "memory" clobber keeps the compiler from hoisting any of those
     * stores above the allocation. */
    __asm volatile (
        "cache 0xd, 0(%0);  cache 0xd, 16(%0); cache 0xd, 32(%0);"
        "cache 0xd, 48(%0); cache 0xd, 64(%0)"
        :: "r"(q) : "memory");
#if R_TRI_QUANT && D_DYNLIGHT
    __asm volatile ("cache 0xd, 80(%0)" :: "r"(q) : "memory");
#elif !R_TRI_QUANT
    __asm volatile ("cache 0xd, 80(%0); cache 0xd, 96(%0)" :: "r"(q) : "memory");
#endif
#endif

    q->texidx = (uint8_t)ti;
    q->wrap = 0;
    q->masked = 0;
    q->tex = tex;
    q->s0 = (uint16_t)s0; q->t0 = (uint16_t)t0;
    q->s1 = (uint16_t)s1; q->t1 = (uint16_t)t1;
    return q;
}

/* Fill one vertex. `s`/`t` are texel coordinates in the *source texture's*
 * space, which is what rdpq_tex_upload_sub expects even though only a
 * sub-rectangle is resident in TMEM. */
#if !R_TRI_QUANT
static inline void set_vtx(float *v, float sx, float sy,
                           float shade, float s, float t, float invw)
{
    v[0] = sx;    v[1] = sy;    v[2] = shade;
    v[3] = s;     v[4] = t;     v[5] = invw;
}
#else

/* --- push-time quantization helpers -------------------------------------
 * Each is the corresponding piece of tri_vertex6's arithmetic, verbatim,
 * so the integers match what the flush used to produce from the float
 * round-trip (a float survives store+load exactly, so converting here
 * yields the same bits the old flush conversion did). */

/* Everything shared by one column edge: both corners on it carry the same
 * x, depth pair and (without DYNLIGHT) shade. */
typedef struct { uint32_t xhi, zw, rgba; int32_t w, invw; } qedge_t;

static inline uint32_t q_rgba(float sh)
{
    const uint32_t cc = (uint32_t)(sh * 255.0f);
    return (cc << 24) | (cc << 16) | (cc << 8) | 255u;
}

#if D_DYNLIGHT
/* Tint folded in at push -- quad_tint is computed here anyway, and folding
 * it kills the flush's whole tinted-path dispatch. Neutral tint reduces to
 * q_rgba exactly (255/255 scaling of identical products). */
static inline uint32_t q_rgba_tint(float sh, uint32_t tint)
{
    if (tint == 0x00FFFFFFu) return q_rgba(sh);
    const uint32_t r = (uint32_t)(sh * (float)((tint >> 16) & 0xFF));
    const uint32_t g = (uint32_t)(sh * (float)((tint >> 8) & 0xFF));
    const uint32_t b = (uint32_t)(sh * (float)(tint & 0xFF));
    return (r << 24) | (g << 16) | (b << 8) | 255u;
}
#endif

static inline void qedge_make(qedge_t *e, float x, float iw, float sh)
{
    /* The barrier is load-bearing for bit-exactness, not hygiene: this
     * GCC at -ffast-math provably folds 1/(1/d) back to d when producer
     * and consumer share a function (seen in emit_fan's disassembly), and
     * d differs from 1/(1/d) by an ulp -- enough to move a perspective
     * texel boundary. The old flush divided in another translation unit,
     * where no fold was possible; the barrier reproduces that. */
    float iw_q = iw;
    __asm volatile("" : "+f"(iw_q));
    e->w    = r_tri_s16_16(1.0f / iw_q);
    e->invw = r_tri_s16_16(iw_q);

    float zf = 1.0f - R_TRI_NEAR * iw_q;
    if (zf < 0.0f) zf = 0.0f;
    e->zw   = ((uint32_t)(uint16_t)(int16_t)(zf * (float)0x7FFF)) << 16;

    e->rgba = q_rgba(sh);
    e->xhi  = ((uint32_t)(uint16_t)(int16_t)rf_floor_i32(x * 4.0f)) << 16;
}

static inline uint32_t q_y16(float y)
{
    return (uint32_t)rf_floor_i32(y * 4.0f) & 0xFFFFu;
}

static inline uint32_t q_shi(float s)
{
    return ((uint32_t)(uint16_t)(int16_t)(s * 32.0f)) << 16;
}

static inline uint32_t q_t16(float t)
{
    return (uint32_t)(int32_t)(int16_t)(t * 32.0f) & 0xFFFFu;
}
#endif /* R_TRI_QUANT */


void r_draw_wall(const r_camera_t *cam, const r_wall_t *wall)
{
    r_draw_wall_clipped(cam, wall, 0.0f, (float)SCREEN_W);
}

static void wall_emit(const r_camera_t *cam, const r_wall_t *wall,
                      float ua, float ub,
                      float d1, float o1, float d2, float o2,
                      float ymin, float ymax);

void r_draw_wall_clipped(const r_camera_t *cam, const r_wall_t *wall,
                         float xmin, float xmax)
{
    r_draw_wall_win(cam, wall, xmin, xmax, 0.0f, (float)SCREEN_H);
}

R_HOT void r_draw_wall_win(const r_camera_t *cam, const r_wall_t *wall,
                     float xmin, float xmax, float ymin, float ymax)
{
    cam_focal_y = cam->focal_y;
    cam_z     = cam->z;

    dt64_tex_t *tex = wall->tex;
    if (!tex) return;

    /* --- view transform -------------------------------------------------
     * Rotate the segment into camera space. `depth` runs along the view
     * direction, `side` runs to the right of it. The BSP walk already did
     * this rotation for the same endpoints in screen_span and hands it over;
     * emit_range fires several times per seg where windows split the span,
     * and each used to redo it. */
    float d1, o1, d2, o2;
    if (wall->has_xform) {
        d1 = wall->cd1; o1 = wall->co1;
        d2 = wall->cd2; o2 = wall->co2;
    } else {
        const float cs = r_view_cs, sn = r_view_sn;
        const float dx1 = wall->x1 - cam->x, dy1 = wall->y1 - cam->y;
        const float dx2 = wall->x2 - cam->x, dy2 = wall->y2 - cam->y;
        d1 = dx1 * cs + dy1 * sn; o1 = dx1 * sn - dy1 * cs;
        d2 = dx2 * cs + dy2 * sn; o2 = dx2 * sn - dy2 * cs;
    }

    /* --- frustum clip ---------------------------------------------------
     * Clipping the near plane alone is not enough. A wall running past the
     * camera stays in front of the near plane while extending far to the
     * side, and projects to screen X in the thousands -- which overflows the
     * RDP's s11.2 triangle coordinates and corrupts the whole frame. So clip
     * against the side planes as well.
     *
     * The side planes come from the caller's column range rather than the
     * screen edges, so occlusion culling reuses this machinery: a screen
     * column x corresponds to the plane offset = depth * (x - cx) / focal. */
    const float cxm = SCREEN_W * 0.5f;
    const float kl = (xmin - cxm) / cam->focal_x;   /* left  bound  */
    const float kr = (xmax - cxm) / cam->focal_x;   /* right bound  */
    const float dd = d2 - d1, od = o2 - o1;

    float ua = 0.0f, ub = 1.0f;
    if (!clip_range(&ua, &ub, d1 - NEAR_PLANE, dd))         return;  /* near  */
    if (!clip_range(&ua, &ub, o1 - d1 * kl, od - dd * kl))  return;  /* left  */
    if (!clip_range(&ua, &ub, d1 * kr - o1, dd * kr - od))  return;  /* right */

    if (wall->len <= 0.0f) return;

    /* --- level-of-detail splits -----------------------------------------
     * The mip is chosen from a range's nearest point, so one choice for the
     * whole span keeps a long receding wall at full tile density all the way
     * to its far end -- the far three-quarters of a corridor wall minified
     * 2-8x was still being cut into full-resolution TMEM columns a few
     * pixels wide. Depth is affine in the wall parameter (d = d1 + dd*u), so
     * the depths where the choice changes map to exact parameter values:
     * split there, and each sub-range picks its own level below. Far
     * stretches then reach the whole-texture fast path and collapse to one
     * wrapped quad per mip band. The cut is a visible mip seam, but no more
     * so than the per-wall seams already present between adjacent segs. */
    int nmips = 0;
    for (const dt64_tex_t *t = tex; t->mip; t = t->mip) nmips++;

    if (nmips > 0 && fabsf(dd) > 1e-6f) {
        float cuts[8];
        int   ncuts = 0;
        float thresh = cam->focal_y;   /* must match the LOD test below */
        /* Divide only for thresholds that can actually land inside the
         * range. The depth interval spanned by [ua,ub] is [min,max] of the
         * two affine ends; a threshold outside it (widened by |dd|*2e-4 to
         * cover the u-space epsilons, plus a small absolute term so a tiny
         * |dd| cannot round the widening away) cannot pass the u test below.
         * The pre-test only ever skips divides whose u the existing test
         * would reject; accepted cuts still come from the same divide and
         * the same epsilon test, so the cut set is bit-identical. */
        const float d_ua = d1 + dd * ua, d_ub = d1 + dd * ub;
        const float d_min = fminf(d_ua, d_ub), d_max = fmaxf(d_ua, d_ub);
        const float widen = fabsf(dd) * 2e-4f;
        for (int k = 0; k < nmips && ncuts < 8; k++, thresh *= 2.0f) {
            if (thresh < d_min - widen - thresh * 1e-6f ||
                thresh > d_max + widen + thresh * 1e-6f) continue;
            const float u = (thresh - d1) / dd;
            if (u > ua + 1e-4f && u < ub - 1e-4f) cuts[ncuts++] = u;
        }
        for (int i = 1; i < ncuts; i++) {          /* ascending, dd either sign */
            const float key = cuts[i];
            int j = i - 1;
            while (j >= 0 && cuts[j] > key) { cuts[j + 1] = cuts[j]; j--; }
            cuts[j + 1] = key;
        }
        if (ncuts) {
            float lo = ua;
            for (int i = 0; i < ncuts; i++) {
                wall_emit(cam, wall, lo, cuts[i], d1, o1, d2, o2, ymin, ymax);
                lo = cuts[i];
            }
            wall_emit(cam, wall, lo, ub, d1, o1, d2, o2, ymin, ymax);
            return;
        }
    }
    wall_emit(cam, wall, ua, ub, d1, o1, d2, o2, ymin, ymax);
}

/* Per-wall invariants for the column/tile loops, plus the previous column's
 * far edge carried across columns: interior edges are shared, so column N's
 * far-edge transform, ~29-cycle reciprocal and fog lookup are column N+1's
 * near edge for free. */
typedef struct {
    const r_camera_t *cam;
    const r_wall_t   *wall;
    dt64_tex_t       *tex;
    float d1, o1, dd, od;
    float uscale, inv_uscale, inv_len;
    float light;
    float finv;                          /* fog LUT entry for this light */
    float t_lo, t_hi;
    float win_lo, win_hi;              /* vertical emission bounds */
    int   vrep_first, tile_h, texh;
    bool  merge_t;

    float prev_cb;                       /* carried far edge (see above) */
    float c_pb, c_ob, c_db, c_iwb, c_shb;
#if R_TRI_QUANT
    qedge_t c_qe;    /* the far edge's quantized form, divide included */
#endif
    bool  have_prev;
} wctx_t;

static bool wall_col(wctx_t *c, float ca, float cb, float s_a, float s_b,
                     int ts0, int ts1);

/* Emit one clipped parameter range of a wall with the mip level its own
 * nearest point selects. */
R_HOT static void wall_emit(const r_camera_t *cam, const r_wall_t *wall,
                      float ua, float ub,
                      float d1, float o1, float d2, float o2,
                      float ymin, float ymax)
{
    dt64_tex_t *tex = wall->tex;
    const float dd = d2 - d1, od = o2 - o1;

    /* --- texture mapping ------------------------------------------------
     * Doom maps one texel to one world unit, so wall length in units is also
     * its length in texels; longer walls simply repeat the texture. */
    /* Length comes precomputed: a square root per column range was pure
     * repetition, since the wall does not change between them. */
    const float wall_len = wall->len;

    /* Level of detail.
     *
     * A surface is minified once it covers fewer screen pixels than it has
     * texels, which for a 1:1 texel-per-unit wall is simply depth > focal.
     * Past that the half-resolution level carries the same information in a
     * quarter of the TMEM tiles -- and therefore a quarter of the quads --
     * while also aliasing less. Chosen from the nearest visible point of
     * THIS range; the caller has already split the wall at the depths where
     * the choice changes.
     *
     * `uscale` converts world units to texels for whichever level is used. */
    float uscale = 1.0f, inv_uscale = 1.0f;
    {
        const float dnear = fminf(d1 + dd * ua, d1 + dd * ub);
        /* Descend while the surface is still minified at the current level. A
         * wall of height H covers H*focal/d pixels and holds H*uscale texels,
         * so it is minified whenever d exceeds focal/uscale.
         *
         * focal_y, deliberately, even though the density this bounds is the
         * horizontal one. It keeps LOD selection bit-identical to the 320
         * build, so wide mode moves exactly one variable -- fill rate -- and
         * a frame-time A/B measures that and nothing else. Switching this to
         * focal_x is the physically correct choice at 640 wide and yields
         * sharper texture on near walls, but it holds every surface one mip
         * level higher for twice the distance, which costs TMEM uploads --
         * the resource this renderer is built to conserve. Measure the cheap
         * version first; see the WIDE notes in README. */
        /* One divide up front instead of one per evaluation, and the
         * reciprocal ridden along instead of divided for below. All
         * bit-identical: uscale only ever halves, halving a float is exact,
         * and IEEE division by an exactly-halved divisor doubles the
         * quotient exactly -- so focal_y / (uscale/2^k) ==
         * (focal_y/uscale) * 2^k, which is what the running threshold
         * accumulates, and 1/uscale likewise doubles exactly. */
        float mip_thresh = cam->focal_y;
        while (tex->mip && dnear > mip_thresh) {
            tex = tex->mip;
            uscale     *= 0.5f;
            inv_uscale *= 2.0f;
            mip_thresh *= 2.0f;
        }
    }

    const int texw = tex->width, texh = tex->height;

    /* Texel-space span actually visible. Offset by u0 so that segs cut from
     * the same linedef continue each other's texture instead of each
     * restarting it at zero. */
    const float sa = (wall->u0 + ua * wall_len) * uscale;
    const float sb = (wall->u0 + ub * wall_len) * uscale;

    const float light = wall->light * (1.0f / 255.0f);
    const float finv  = r_fog_inv[wall->light];
    const float cx = SCREEN_W * 0.5f, cy = SCREEN_H * 0.5f;


/* Columns are DISABLED. See the note above r_span_t: measured slower than
 * triangles on this hardware, because RDP per-primitive setup does not
 * amortise over a one-pixel-wide rectangle. Kept for reference. */
#ifndef R_COLUMNS
#define R_COLUMNS 0
#endif

#if R_COLUMNS
    /* --- column path ----------------------------------------------------
     *
     * Requires the whole texture resident, since consecutive screen columns
     * sample arbitrary and different texel columns. Descend the mip chain
     * until it fits; if even the smallest level does not, fall through to the
     * tiled quad path, which still works. */
    {
        const dt64_tex_t *ct = tex;
        float cs_scale = uscale;
        while (ct->width * ct->height > 2048 && ct->mip) {
            ct = ct->mip;
            cs_scale *= 0.5f;
        }

        if (ct->width * ct->height <= 2048 && num_spans < R_MAX_SPANS) {
            const float da = d1 + dd * ua, db = d1 + dd * ub;
            const float oa = o1 + od * ua, ob = o1 + od * ub;

            if (da > 0.0f && db > 0.0f) {
                const float iza = 1.0f / da, izb = 1.0f / db;
                const float xa = cx + oa * cam->focal_x * iza;
                const float xb = cx + ob * cam->focal_x * izb;

                if (xb - xa > 0.01f) {
                    int ti = -1;
                    for (int i = 0; i < num_batch_tex; i++)
                        if (batch_tex[i] == ct) { ti = i; break; }
                    if (ti < 0 && num_batch_tex < R_MAX_TEXTURES) {
                        ti = num_batch_tex;
                        batch_tex[num_batch_tex++] = ct;
                    }

                    if (ti >= 0) {
                        r_span_t *sp = &spans[num_spans++];
                        sp->tex    = ct;
                        sp->texidx = (uint8_t)ti;
                        sp->xa = xa;  sp->xb = xb;
                        sp->iza = iza; sp->izb = izb;
                        /* parameter/depth, the other quantity linear in x */
                        sp->uoza = ua * iza;
                        sp->uozb = ub * izb;
                        sp->ztop = wall->ztop;
                        sp->zbot = wall->zbot;
                        sp->ttop = (wall->ztex - wall->ztop) * cs_scale;
                        sp->tbot = (wall->ztex - wall->zbot) * cs_scale;
                        sp->u0   = wall->u0;
                        sp->wall_len = wall_len;
                        sp->uscale   = cs_scale;
                        sp->light    = light;
                        return;
                    }
                }
            }
        }
    }

#endif /* R_COLUMNS */

    /* --- whole-texture fast path ----------------------------------------
     *
     * Once a mip level is small enough to sit in TMEM entire, the tiling can
     * go away completely: load it once with wrapping and a single quad covers
     * the wall however many times the texture repeats along it. That is worth
     * far more than it sounds. Tiling costs a quad per texture repeat *per*
     * TMEM tile, so a long wall seen from a distance -- the common case in a
     * corridor -- was paying eight or more quads to draw a few hundred pixels.
     *
     * Only reachable for minified surfaces, since the full-resolution level of
     * a 128x128 texture is eight times too large to be resident. */
    /* Whether each axis can wrap, decided independently.
     *
     * The RDP only wraps power-of-two dimensions. Doom widths almost always
     * are -- 64, 128, 256 -- while heights are whatever the artist needed:
     * 72, 56, 128. Requiring both cost the fast path on most textures for the
     * sake of an axis that usually does not need to repeat at all: a wall
     * shorter than its texture shows part of it and never tiles vertically.
     *
     * So an axis qualifies if it can wrap, or if the wall simply does not
     * leave the texture along it. */
    const bool pot_w = (texw & (texw - 1)) == 0;
    const bool pot_h = (texh & (texh - 1)) == 0;

    const float t_lo_all = (wall->ztex - wall->ztop) * uscale;
    const float t_hi_all = (wall->ztex - wall->zbot) * uscale;

    const bool s_ok = pot_w || (sa >= 0.0f && sb <= (float)texw);
    const bool t_ok = pot_h || (t_lo_all >= 0.0f && t_hi_all <= (float)texh);

    if (s_ok && t_ok && texw * texh <= 2048) {
        const float pa = ua, pb = ub;
        const float oa = o1 + od * pa, da = d1 + dd * pa;
        const float ob = o1 + od * pb, db = d1 + dd * pb;

        if (da > 0.0f && db > 0.0f) {
            const float iwa = 1.0f / da, iwb = 1.0f / db;
            const float xa = cx + oa * cam->focal_x * iwa;
            const float xb = cx + ob * cam->focal_x * iwb;

            /* Per-corner rather than per-edge: a light near the floor has to
             * be able to brighten the bottom of a wall and leave the top dark,
             * which one value per edge cannot express. The world position of
             * each end comes back from the parametric coordinate the clipper
             * already produced. Kept behind the flag so the ordinary build
             * keeps using the two edge values above unchanged. */
#if D_DYNLIGHT
            const float wdx = wall->x2 - wall->x1, wdy = wall->y2 - wall->y1;
            const float wxa = wall->x1 + wdx * pa, wya = wall->y1 + wdy * pa;
            const float wxb = wall->x1 + wdx * pb, wyb = wall->y1 + wdy * pb;
            const float sha_t = lit_shade(light, da, finv, wxa, wya, wall->ztop, wall);
            const float sha_b = lit_shade(light, da, finv, wxa, wya, wall->zbot, wall);
            const float shb_t = lit_shade(light, db, finv, wxb, wyb, wall->ztop, wall);
            const float shb_b = lit_shade(light, db, finv, wxb, wyb, wall->zbot, wall);
#else
            const float sha = light * r_vis(da, finv);
            const float shb = light * r_vis(db, finv);
            const float sha_t = sha, sha_b = sha;
            const float shb_t = shb, shb_b = shb;
#endif

            const float t_lo = (wall->ztex - wall->ztop) * uscale;
            const float t_hi = (wall->ztex - wall->zbot) * uscale;

            float ya_t = cy - (wall->ztop - cam->z) * cam->focal_y * iwa;
            float ya_b = cy - (wall->zbot - cam->z) * cam->focal_y * iwa;
            float yb_t = cy - (wall->ztop - cam->z) * cam->focal_y * iwb;
            float yb_b = cy - (wall->zbot - cam->z) * cam->focal_y * iwb;

            if (!((ya_b <= ymin && yb_b <= ymin) ||
                  (ya_t >= ymax && yb_t >= ymax))) {
                float ta_t = t_lo, ta_b = t_hi;
                float tb_t = t_lo, tb_b = t_hi;
                clamp_quad(&ya_t, &ya_b, &ta_t, &ta_b,
                           &yb_t, &yb_b, &tb_t, &tb_b, ymin, ymax);

                ya_t -= WALL_BLEED; yb_t -= WALL_BLEED;
                ya_b += WALL_BLEED; yb_b += WALL_BLEED;

                r_quad_t *q = batch_push(tex, 0, 0, texw, texh);
                if (q) {
                    q->masked = wall->masked;
                    q->wrap = (uint8_t)(1 | (pot_w ? 2 : 0) | (pot_h ? 4 : 0));
#if R_TRI_QUANT
                    qedge_t ea, eb;
                    qedge_make(&ea, xa, iwa, sha_t);
                    qedge_make(&eb, xb, iwb, shb_t);
                    q->xy[0] = ea.xhi | q_y16(ya_t);
                    q->xy[1] = eb.xhi | q_y16(yb_t);
                    q->xy[2] = eb.xhi | q_y16(yb_b);
                    q->xy[3] = ea.xhi | q_y16(ya_b);
                    const uint32_t sa_hi = q_shi(sa), sb_hi = q_shi(sb);
                    q->st[0] = sa_hi | q_t16(ta_t);
                    q->st[1] = sb_hi | q_t16(tb_t);
                    q->st[2] = sb_hi | q_t16(tb_b);
                    q->st[3] = sa_hi | q_t16(ta_b);
                    q->w_a = ea.w; q->invw_a = ea.invw; q->zw_a = ea.zw;
                    q->w_b = eb.w; q->invw_b = eb.invw; q->zw_b = eb.zw;
#if D_DYNLIGHT
                    const uint32_t tnt = quad_tint(wall, light,
                        (wxa + wxb) * 0.5f, (wya + wyb) * 0.5f,
                        (wall->ztop + wall->zbot) * 0.5f);
                    q->rgba[0] = q_rgba_tint(sha_t, tnt);
                    q->rgba[1] = q_rgba_tint(shb_t, tnt);
                    q->rgba[2] = q_rgba_tint(shb_b, tnt);
                    q->rgba[3] = q_rgba_tint(sha_b, tnt);
#endif
#else /* !R_TRI_QUANT */
                    set_vtx(q->v[0], xa, ya_t, sha_t, sa, ta_t, iwa);
                    set_vtx(q->v[1], xb, yb_t, shb_t, sb, tb_t, iwb);
                    set_vtx(q->v[2], xb, yb_b, shb_b, sb, tb_b, iwb);
                    set_vtx(q->v[3], xa, ya_b, sha_b, sa, ta_b, iwa);
#if D_DYNLIGHT
                    q_tint[num_quads - 1] = quad_tint(wall, light,
                        (wxa + wxb) * 0.5f, (wya + wyb) * 0.5f,
                        (wall->ztop + wall->zbot) * 0.5f);
                    if (q_tint[num_quads - 1] != 0x00FFFFFFu) num_tinted++;
#endif
#endif /* R_TRI_QUANT */
                }
            }
        }
        return;
    }

    /* --- tile loop ------------------------------------------------------
     * Iterate repeats of the texture along the wall, and within each repeat
     * the TMEM-sized columns. Nesting it this way rather than stepping a
     * global counter by the tile width keeps it correct for textures whose
     * width is not a multiple of 64 -- there, a fixed stride would eventually
     * straddle a repeat boundary and sample outside the resident tile. */
    const int tile_w = texw < DT64_TILE_W ? texw : DT64_TILE_W;
    const int tile_h = texh < DT64_TILE_H ? texh : DT64_TILE_H;

    /* Wall-invariant vertical mapping, hoisted out of the column loop: it
     * depends only on the wall and the level, but computed inside the loop
     * the compiler had to redo the divide and floorf per column, because
     * set_vtx's float stores may alias the wall's float fields under TBAA. */
    const float t_lo = (wall->ztex - wall->ztop) * uscale;
    const float t_hi = (wall->ztex - wall->zbot) * uscale;

    wctx_t c = {
        .cam    = cam,
        .wall   = wall,
        .tex    = tex,
        .d1 = d1, .o1 = o1, .dd = dd, .od = od,
        .uscale     = uscale,
        .inv_uscale = inv_uscale,   /* ridden along the descent; same bits */
        .inv_len    = 1.0f / wall_len,
        .light  = light,
        .finv   = finv,
        .t_lo   = t_lo,
        .t_hi   = t_hi,
        .win_lo = ymin,
        .win_hi = ymax,
        .vrep_first = rf_floor_i32(t_lo / (float)texh),
        .tile_h = tile_h,
        .texh   = texh,
        /* One wrapped band vertically when the tile already spans the full
         * texture height; symmetric to the horizontal merge below. */
        .merge_t = (tile_h == texh) && pot_h && (t_hi - t_lo) < 1023.0f,
        .have_prev = false,
    };

    /* Horizontal repeat merge. When the resident tile spans the full texture
     * width, every repeat of the texture along the wall samples the SAME
     * tile: the flush uploads it once and drew N quads where one wrapped
     * quad does -- a 512-unit wall with a 64-wide door texture was 8 quads
     * per tile row for identical pixels. Emit wide columns covering the
     * whole visible span, rebased near zero to respect the RDP's s10.5
     * coordinate limit; the flush uploads such textures with S wrapping
     * (power-of-two only, hence pot_w).
     *
     * Merged spans are still cut where the depth ratio would exceed 2:1,
     * the same bound the flats band at: the real RDP's fixed-point
     * perspective divide visibly warps texture coordinates past ~2:1 (the
     * emulator does not show this), and the screen-linear z the RDP
     * interpolates diverges from the true 1/d curve by ~K*0.083/d per 2:1
     * primitive -- the junction z-bias in r_flat.c is sized against exactly
     * this bound, so widening the ratio here reopens wall/floor leaks.
     * Depth is affine in S along the wall, so the cuts are direct. */
    if (!wall->masked && (tile_w == texw) && pot_w && (sb - sa) < 1023.0f) {
        const float rep_base = rf_floorf(sa / (float)texw) * (float)texw;
        const float sB = dd * c.inv_len * c.inv_uscale;
        const float sA = d1 - dd * wall->u0 * c.inv_len;

        float ca = sa;
        while (ca < sb - 1e-4f) {
            const float dca = sA + sB * ca;
            float cb = sb;
            if (sB > 1e-9f) {
                const float s_lim = (dca * 2.0f - sA) / sB;
                if (s_lim < cb) cb = s_lim;
            } else if (sB < -1e-9f) {
                const float s_lim = (dca * 0.5f - sA) / sB;
                if (s_lim < cb) cb = s_lim;
            }
            if (cb <= ca + 1e-4f) cb = fminf(sb, ca + 1.0f);   /* safety */
            if (!wall_col(&c, ca, cb, ca - rep_base, cb - rep_base, 0, texw))
                return;
            ca = cb;
        }
        return;
    }

    const int rep_first = rf_floor_i32(sa / (float)texw);

    for (int rep = rep_first; (float)(rep * texw) < sb; rep++) {
        const float rep_base = (float)(rep * texw);

        for (int src_s = 0; src_s < texw; src_s += tile_w) {
            const int src_s1 = src_s + tile_w > texw ? texw : src_s + tile_w;

            const float col_a = fmaxf(rep_base + src_s,  sa);
            const float col_b = fminf(rep_base + src_s1, sb);
            if (col_b <= col_a) continue;

            if (!wall_col(&c, col_a, col_b,
                          col_a - rep_base, col_b - rep_base,
                          src_s, src_s1))
                return;                      /* batch full; stop this wall */
        }
    }
}

/* Emit the quads of one horizontal column span [ca,cb] of a wall.
 * `s_a`/`s_b` are the texel S coordinates at the two edges, `ts0`/`ts1` the
 * resident tile's S extent for the batcher. Returns false when the quad
 * batch filled up. */
static bool wall_col(wctx_t *c, float ca, float cb, float s_a, float s_b,
                     int ts0, int ts1)
{
    const r_camera_t *cam  = c->cam;
    const r_wall_t   *wall = c->wall;
    const float cy = SCREEN_H * 0.5f;
    const float cx = SCREEN_W * 0.5f;

    /* Interpolate the two column edges back along the unclipped segment --
     * except the near edge, which is usually the previous column's far edge
     * verbatim (the fmax/fmin clamps only fire at the span's ends, so
     * interior edges compare bit-equal). */
    float pa, oa, da, iwa, sha;
    const bool reuse_a = c->have_prev && ca == c->prev_cb;
    if (reuse_a) {
        pa = c->c_pb; oa = c->c_ob; da = c->c_db;
        iwa = c->c_iwb; sha = c->c_shb;
    } else {
        pa = (ca * c->inv_uscale - wall->u0) * c->inv_len;
        oa = c->o1 + c->od * pa;
        da = c->d1 + c->dd * pa;
        if (da <= 0.0f) { c->have_prev = false; return true; }
        iwa = 1.0f / da;
        sha = c->light * r_vis(da, c->finv);
    }

    const float pb = (cb * c->inv_uscale - wall->u0) * c->inv_len;
    const float ob = c->o1 + c->od * pb;
    const float db = c->d1 + c->dd * pb;
    if (db <= 0.0f) { c->have_prev = false; return true; }
    const float iwb = 1.0f / db;
    const float shb = c->light * r_vis(db, c->finv);

    c->prev_cb = cb;
    c->c_pb = pb; c->c_ob = ob; c->c_db = db;
    c->c_iwb = iwb; c->c_shb = shb;
    c->have_prev = true;

    const float xa = cx + oa * cam->focal_x * iwa;
    const float xb = cx + ob * cam->focal_x * iwb;

#if R_TRI_QUANT
    /* Quantized edges, once per COLUMN: every band of it shares them, and
     * the near edge usually arrives ready-made from the previous column's
     * far edge -- carrying the divide with it. The reconstructed xa here
     * is bit-equal to the previous call's xb (same expression, the exact
     * carried operands), so a cached edge's xhi still matches. */
    qedge_t qa, qb;
    if (reuse_a) qa = c->c_qe;
    else         qedge_make(&qa, xa, iwa, sha);
    qedge_make(&qb, xb, iwb, shb);
    c->c_qe = qb;
    const uint32_t sa_hi = q_shi(s_a), sb_hi = q_shi(s_b);
#endif

#if D_DYNLIGHT
    /* World position of the two column edges, from the parametric coordinate
     * the column stepper already carries. Hoisted out of the band loop: it is
     * the same point at every height. */
    const float wdx = wall->x2 - wall->x1, wdy = wall->y2 - wall->y1;
    const float wxa = wall->x1 + wdx * pa, wya = wall->y1 + wdy * pa;
    const float wxb = wall->x1 + wdx * pb, wyb = wall->y1 + wdy * pb;
#endif

    const int   texh   = c->texh;
    const int   tile_h = c->tile_h;
    const float t_lo   = c->t_lo, t_hi = c->t_hi;

    /* Texel rows this wall covers, one texel per world unit. Doom never
     * scales a wall texture to the wall: a short wall shows part of the
     * texture and a tall one repeats it. */
    for (int vrep = c->vrep_first; ; vrep++) {
        if (c->merge_t) { if (vrep != c->vrep_first) break; }
        else if (!((float)(vrep * texh) < t_hi)) break;

        const float vbase = (float)(vrep * texh);

        for (int src_t = 0; src_t < texh; src_t += tile_h) {
            float trow_a, trow_b;
            int   t0, t1;

            if (c->merge_t) {
                /* Single wrapped band covering the whole vertical span. */
                trow_a = t_lo; trow_b = t_hi;
                t0 = 0; t1 = texh;
            } else {
                t1 = src_t + tile_h > texh ? texh : src_t + tile_h;
                t0 = src_t;
                trow_a = fmaxf(vbase + t0, t_lo);
                trow_b = fminf(vbase + t1, t_hi);
                if (trow_b <= trow_a) continue;
            }

            /* World heights of this texel band, and the band's texel rows
             * within the resident tile. */
            const float ztop = wall->ztex - trow_a * c->inv_uscale;
            const float zbot = wall->ztex - trow_b * c->inv_uscale;
            const float trel_a = trow_a - vbase;
            const float trel_b = trow_b - vbase;

            float ya_t = cy - (ztop - cam->z) * cam->focal_y * iwa;
            float ya_b = cy - (zbot - cam->z) * cam->focal_y * iwa;
            float yb_t = cy - (ztop - cam->z) * cam->focal_y * iwb;
            float yb_b = cy - (zbot - cam->z) * cam->focal_y * iwb;

            /* This band's own top and bottom, so a light picks out one course
             * of a tiled wall rather than washing the whole column. Recomputes
             * the falloff from the cached depth instead of reusing the cached
             * shade -- same input, so adjacent bands still meet bit-equal at a
             * shared edge, which is what the caching upstream is protecting. */
#if D_DYNLIGHT
            /* sha stays live for the band-continuity cache above even though
             * the corners no longer read it; shb is what gets cached. */
            (void)sha;
            const float sha_t = lit_shade(c->light, da, c->finv, wxa, wya, ztop, wall);
            const float sha_b = lit_shade(c->light, da, c->finv, wxa, wya, zbot, wall);
            const float shb_t = lit_shade(c->light, db, c->finv, wxb, wyb, ztop, wall);
            const float shb_b = lit_shade(c->light, db, c->finv, wxb, wyb, zbot, wall);
#else
            const float sha_t = sha, sha_b = sha;
            const float shb_t = shb, shb_b = shb;
#endif

            /* Skip tiles entirely outside the vertical bound (viewport, or
             * the open window when seen through a doorway). */
            if ((ya_b <= c->win_lo && yb_b <= c->win_lo) ||
                (ya_t >= c->win_hi && yb_t >= c->win_hi))
                continue;

            float ta_t = trel_a, ta_b = trel_b;
            float tb_t = trel_a, tb_b = trel_b;
            clamp_quad(&ya_t, &ya_b, &ta_t, &ta_b,
                       &yb_t, &yb_b, &tb_t, &tb_b, c->win_lo, c->win_hi);

            if (trow_a <= t_lo + 1e-4f) { ya_t -= WALL_BLEED; yb_t -= WALL_BLEED; }
            if (trow_b >= t_hi - 1e-4f) { ya_b += WALL_BLEED; yb_b += WALL_BLEED; }

            r_quad_t *q = batch_push(c->tex, ts0, t0, ts1, t1);
            if (q) q->masked = c->wall->masked;
            if (!q) return false;            /* batch full; stop this wall */

#if R_TRI_QUANT
            q->xy[0] = qa.xhi | q_y16(ya_t);        /* top-left  */
            q->xy[1] = qb.xhi | q_y16(yb_t);        /* top-right */
            q->xy[2] = qb.xhi | q_y16(yb_b);        /* bot-right */
            q->xy[3] = qa.xhi | q_y16(ya_b);        /* bot-left  */
            q->st[0] = sa_hi | q_t16(ta_t);
            q->st[1] = sb_hi | q_t16(tb_t);
            q->st[2] = sb_hi | q_t16(tb_b);
            q->st[3] = sa_hi | q_t16(ta_b);
            q->w_a = qa.w; q->invw_a = qa.invw; q->zw_a = qa.zw;
            q->w_b = qb.w; q->invw_b = qb.invw; q->zw_b = qb.zw;
#if D_DYNLIGHT
            /* Tint per BAND, folded straight into the corner shades: the
             * centre height localizes a floor-hugging glow or light to the
             * lower courses of a tall wall instead of washing the column. */
            {
                const uint32_t tnt = quad_tint(wall, c->light,
                    (wxa + wxb) * 0.5f, (wya + wyb) * 0.5f,
                    (ztop + zbot) * 0.5f);
                q->rgba[0] = q_rgba_tint(sha_t, tnt);
                q->rgba[1] = q_rgba_tint(shb_t, tnt);
                q->rgba[2] = q_rgba_tint(shb_b, tnt);
                q->rgba[3] = q_rgba_tint(sha_b, tnt);
            }
#else
            q->rgba_a = qa.rgba;
            q->rgba_b = qb.rgba;
#endif
#else /* !R_TRI_QUANT */
            set_vtx(q->v[0], xa, ya_t, sha_t, s_a, ta_t, iwa);  /* top-left  */
            set_vtx(q->v[1], xb, yb_t, shb_t, s_b, tb_t, iwb);  /* top-right */
            set_vtx(q->v[2], xb, yb_b, shb_b, s_b, tb_b, iwb);  /* bot-right */
            set_vtx(q->v[3], xa, ya_b, sha_b, s_a, ta_b, iwa);  /* bot-left  */
#if D_DYNLIGHT
            /* Per BAND, not per column: the centre height localizes a
             * floor-hugging glow or light to the lower courses of a tall
             * wall instead of washing the whole column. */
            q_tint[num_quads - 1] = quad_tint(wall, c->light,
                (wxa + wxb) * 0.5f, (wya + wyb) * 0.5f,
                (ztop + zbot) * 0.5f);
            if (q_tint[num_quads - 1] != 0x00FFFFFFu) num_tinted++;
#endif
#endif /* R_TRI_QUANT */

#if DEBUG_RDP
            /* One-shot dump of what the tile loop actually emits, so a blank
             * or truncated wall can be attributed to geometry or to RDP state
             * rather than guessed at. */
            extern int r_debug_dump;
            if (r_debug_dump > 0) {
                r_debug_dump--;
                debugf("quad s=[%d,%d] t=[%d,%d] su=[%.1f,%.1f] "
                       "x=[%.1f,%.1f] y=[%.1f,%.1f] shade=[%.2f,%.2f] iw=[%.4f,%.4f]\n",
                       ts0, ts1, t0, t1, s_a, s_b, xa, xb, ya_t, ya_b,
                       sha, shb, iwa, iwb);
            }
#endif
        }
    }
    return true;
}

#if R_COLUMNS
/* Draw one span as a run of single-column texture rectangles.
 *
 * Per column: 1/z and parameter/z are both linear in screen x, so one divide
 * recovers the wall parameter and with it the texel column and the vertical
 * scale. Everything else is a multiply-add. That is Doom's inner loop, with
 * the RDP doing the texel work instead of the CPU. */
static void emit_span_columns(const r_span_t *sp)
{
    const float cy = SCREEN_H * 0.5f;
    const int texw = sp->tex->width;

    int x0 = (int)floorf(sp->xa), x1 = (int)ceilf(sp->xb);
    if (x0 < 0) x0 = 0;
    if (x1 > SCREEN_W) x1 = SCREEN_W;
    if (x0 >= x1) return;

    const float span = sp->xb - sp->xa;
    if (span < 0.001f) return;

    const float d_iz  = (sp->izb  - sp->iza)  / span;
    const float d_uoz = (sp->uozb - sp->uoza) / span;

    for (int x = x0; x < x1; x++) {
        const float dx = ((float)x + 0.5f) - sp->xa;
        const float iz  = sp->iza  + d_iz  * dx;
        if (iz <= 0.0f) continue;

        const float u = (sp->uoza + d_uoz * dx) / iz;    /* the one divide */

        /* Texel column, wrapped in software so the texture need not be a
         * power of two -- the RDP can only wrap those, and Doom's are often
         * not. */
        float sx = (sp->u0 + u * sp->wall_len) * sp->uscale;
        sx = fmodf(sx, (float)texw);
        if (sx < 0.0f) sx += (float)texw;

        const float scale = cam_focal_y * iz;
        float ytop = cy - (sp->ztop - cam_z) * scale;
        float ybot = cy - (sp->zbot - cam_z) * scale;
        if (ybot <= 0.0f || ytop >= (float)SCREEN_H) continue;

        /* Clamp to the viewport, carrying the texel row with it: t is affine
         * in y at constant depth, which is exactly the case here. */
        float ttop = sp->ttop, tbot = sp->tbot;
        const float dy = ybot - ytop;
        if (dy > 1e-4f) {
            const float dtdy = (tbot - ttop) / dy;
            if (ytop < 0.0f)                { ttop += dtdy * (0.0f - ytop);                ytop = 0.0f; }
            if (ybot > (float)SCREEN_H)     { tbot += dtdy * ((float)SCREEN_H - ybot);     ybot = (float)SCREEN_H; }
        }
        if (ybot - ytop < 0.01f) continue;

        rdpq_texture_rectangle_scaled(TILE0, x, ytop, x + 1, ybot,
                                      sx, ttop, sx + 1.0f, tbot);
        r_tri_wall++;      /* counted as one primitive */
    }
}

#endif /* R_COLUMNS */

void r_flush_walls(void)
{
    r_wall_modes();

#if R_COLUMNS
    /* Column spans first: one upload per texture, then every column that
     * samples it. */
    if (num_spans) {
        /* A texture rectangle carries no shade component, so sector light
         * rides on the primitive colour instead. */
        rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, PRIM, 0), (0, 0, 0, PRIM)));

        /* Nor does it carry per-pixel depth. Walls need none among
         * themselves -- occlusion already resolved that -- but they must
         * write depth so floors and sprites behind them are rejected. The
         * source is switched to the primitive depth register, set per column
         * since every column is at its own constant depth. */
        rdpq_mode_zbuf(false, false);
    }

    for (int i = 0; i < num_batch_tex && num_spans; i++) {
        bool bound = false;
        for (int k = 0; k < num_spans; k++) {
            if (spans[k].texidx != i) continue;
            if (!bound) {
                rdpq_tex_upload(TILE0, &spans[k].tex->surface, NULL);
                tmem_uploads++;
                bound = true;
            }
            {
                const float lit = spans[k].light *
                    visibility_at(2.0f / (spans[k].iza + spans[k].izb));
                const uint8_t l = (uint8_t)(lit < 0.0f ? 0.0f :
                                            (lit > 1.0f ? 255.0f : lit * 255.0f));
                rdpq_set_prim_color(RGBA32(l, l, l, 255));
            }
            emit_span_columns(&spans[k]);
        }
    }
    if (num_spans) {
        rdpq_mode_zbuf(true, true);
        /* Alpha rides TEX0 so masked mids can cut; opaque textures carry alpha
     * one everywhere and the compare is off for them, so nothing changes. */
    rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, SHADE, 0), (0, 0, 0, TEX0)));
    }
    num_spans = 0;
#endif /* R_COLUMNS */

    /* Alpha-compare tracking, reset per flush: r_wall_modes above leaves the
     * unit off, so a frame that ended mid-fence must not convince the next
     * one that compare is still enabled. */
    int cur_masked = 0;

    /* Group quads by (texture, tile) in two stable counting passes over the
     * dense key array, then draw in ONE linear walk of the result.
     *
     * The previous form grouped by texture and then, for every tile of the
     * grid, re-scanned that texture's whole quad list for origin matches --
     * each probe dragging a 16-byte header line of a 112-byte record
     * through the D-cache, once per tile. The two radix passes below read
     * only q_key (two bytes per quad) and the walk touches each quad
     * exactly once, in the same order the grid scan produced: the low byte
     * is the t-major tile ordinal, the high byte the texture index, and
     * counting sorts are stable, so within a (texture, tile) group the
     * original push order survives. Same uploads, same command stream. */
    static uint16_t order[R_MAX_QUADS];
    static uint16_t order_tmp[R_MAX_QUADS];

    {
        uint16_t cnt[257] = {0};
        for (int k = 0; k < num_quads; k++) cnt[(q_key[k] & 0xFF) + 1]++;
        for (int i = 0; i < 256; i++) cnt[i + 1] += cnt[i];
        for (int k = 0; k < num_quads; k++)
            order_tmp[cnt[q_key[k] & 0xFF]++] = (uint16_t)k;
    }
    {
        uint16_t cnt[R_MAX_TEXTURES + 1] = {0};
        for (int k = 0; k < num_quads; k++) cnt[(q_key[k] >> 8) + 1]++;
        for (int i = 0; i < num_batch_tex; i++) cnt[i + 1] += cnt[i];
        for (int n = 0; n < num_quads; n++) {
            const uint16_t k = order_tmp[n];
            order[cnt[q_key[k] >> 8]++] = k;
        }
    }

    {
        const dt64_tex_t *tex = NULL;
        bool band_s = false, band_t = false;
        uint16_t prev_key = 0xFFFF;        /* no real key: texidx < 64 */

        for (int n = 0; n < num_quads; n++) {
            const uint16_t  qi = order[n];
            const r_quad_t *q  = &quads[qi];
            const uint16_t  key = q_key[qi];

            if (key != prev_key) {
                if (tex == NULL || (prev_key >> 8) != (key >> 8)) {
                    tex = batch_tex[key >> 8];

                    const int tile_w = tex->width  < DT64_TILE_W
                                     ? tex->width  : DT64_TILE_W;
                    const int tile_h = tex->height < DT64_TILE_H
                                     ? tex->height : DT64_TILE_H;
                    /* Band wrapping is a property of the texture's shape,
                     * not of any one quad: when the tile spans the full
                     * (power-of-two) texture along an axis, the emitter
                     * merges repeats into single wrapped quads along it,
                     * and every upload of such a texture wraps that axis.
                     * Quads that happen to stay inside one repeat sample
                     * identically either way. */
                    band_s = (tile_w == tex->width) &&
                             (tex->width  & (tex->width  - 1)) == 0;
                    band_t = (tile_h == tex->height) &&
                             (tex->height & (tex->height - 1)) == 0;
                }
                prev_key = key;

                /* First quad of the (texture, tile) group decides the
                 * upload, exactly as the first origin match used to. */
                if (q->wrap) {
                    /* Only the axes that are power-of-two may repeat; the
                     * others clamp, which is correct because the fast path
                     * is only taken when the wall stays inside the texture
                     * along them. */
                    rdpq_texparms_t p = {0};
                    p.s.repeats = (q->wrap & 2) ? REPEAT_INFINITE : 1;
                    p.t.repeats = (q->wrap & 4) ? REPEAT_INFINITE : 1;
                    rdpq_tex_upload(TILE0, &tex->surface, &p);
                } else if (band_s || band_t) {
                    rdpq_texparms_t p = {0};
                    p.s.repeats = band_s ? REPEAT_INFINITE : 1;
                    p.t.repeats = band_t ? REPEAT_INFINITE : 1;
                    dt64_upload_tile(TILE0, tex, &p,
                                     q->s0, q->t0, q->s1, q->t1);
                } else {
                    dt64_upload_tile(TILE0, tex, NULL,
                                     q->s0, q->t0, q->s1, q->t1);
                }
                tmem_uploads++;
                r_tri_group_begin();       /* TMEM changed: re-register */
            }

            /* Fences and grates alpha-cut; everything else keeps
             * the compare off so bilinear edges stay untouched. */
            if (q->masked != cur_masked) {
                cur_masked = q->masked;
                rdpq_mode_alphacompare(cur_masked);
                r_tri_group_begin();       /* mode changed */
            }

#if R_TRI_QUANT
            /* Pure copy: the record already holds the RSP command words --
             * the conversions, divides and the tint all happened at push,
             * where every input was live in registers. The autosync
             * registration replaces the old group-leader rdpq_triangle,
             * firing with the first triangle after each group_begin (same
             * bits, same timing, no float detour). Slot pattern is
             * r_tri_quad6's: 0,1,2, issue, slot1 = corner 3, issue. */
            r_tri_group_use(R_TRI_USE_WALL);
#if D_DYNLIGHT
#define QRGBA(k) (int32_t)q->rgba[k]
#else
#define QRGBA(k) (int32_t)((k) == 0 || (k) == 3 ? q->rgba_a : q->rgba_b)
#endif
            r_tri_slot(0, (int32_t)q->xy[0], (int32_t)q->zw_a, QRGBA(0),
                       (int32_t)q->st[0], q->w_a, q->invw_a);
            r_tri_slot(1, (int32_t)q->xy[1], (int32_t)q->zw_b, QRGBA(1),
                       (int32_t)q->st[1], q->w_b, q->invw_b);
            r_tri_slot(2, (int32_t)q->xy[2], (int32_t)q->zw_b, QRGBA(2),
                       (int32_t)q->st[2], q->w_b, q->invw_b);
            r_tri_slots_issue(&TRIFMT_WALL);
            r_tri_slot(1, (int32_t)q->xy[3], (int32_t)q->zw_a, QRGBA(3),
                       (int32_t)q->st[3], q->w_a, q->invw_a);
            r_tri_slots_issue(&TRIFMT_WALL);
#undef QRGBA
#else /* !R_TRI_QUANT */
#if D_DYNLIGHT
            /* num_tinted == 0 on quiet frames keeps this byte-identical to
             * the plain path -- q_tint is not even read. */
            if (num_tinted) {
                const uint32_t tnt = q_tint[qi];
                if (tnt != 0x00FFFFFFu)
                    r_tri_quad6t(&TRIFMT_WALL, (const float (*)[6])q->v, tnt);
                else
                    r_tri_quad6(&TRIFMT_WALL, (const float (*)[6])q->v);
            } else
#endif
            r_tri_quad6(&TRIFMT_WALL, (const float (*)[6])q->v);
#endif /* R_TRI_QUANT */
            r_tri_wall += 2;
        }
    }

    num_quads     = 0;
    num_batch_tex = 0;
}
