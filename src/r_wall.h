/*
 * r_wall -- RDP wall rasterisation.
 *
 * Doom walls are vertical quads, which is a very friendly shape for the RDP:
 * the whole quad shares a single depth gradient, so near-plane clipping is a
 * 2D segment clip and the vertical extent needs no clipping at all.
 */
#ifndef R_WALL_H
#define R_WALL_H

#include "dt64.h"

#ifndef D_DYNLIGHT
#define D_DYNLIGHT 0
#endif

/* Doom's own frame width, and the reference for everything that is authored
 * in screen space rather than derived from the viewport: UI art, psprites,
 * and the vertical projection scale. */
#define SCREEN_BASE_W 320

/* R_WIDE=1 selects the N64's 640x240 mode: twice the horizontal sampling
 * rate, the same 240 scanlines, no interlace. See the note on focal_x/focal_y
 * below for why doubling width and not height is the cheap direction. */
#ifndef R_WIDE
#define R_WIDE 0
#endif

#if R_WIDE
#define SCREEN_W 640
#else
#define SCREEN_W SCREEN_BASE_W
#endif
#define SCREEN_H 240

/* Horizontal magnification for art authored in Doom's 320-wide frame: the UI,
 * the weapon, and the automap.
 *
 * Vertical is 1:1 in every mode because SCREEN_H never changes, so this is a
 * pure horizontal double -- which is exactly what makes the wide UI identical
 * in apparent size and shape to the 320 one, since a 640x240 pixel is half as
 * wide as it is tall.
 *
 * COPY mode survives this. It cannot filter, but it does step S by DSDX per
 * output pixel (libdragon's RSP fixup pre-multiplies by 4 for the copy
 * pipeline's 4-pixels-per-clock), so a destination twice as wide simply
 * point-doubles each texel and still moves four texels per clock. Scaled
 * rectangles are explicitly supported in copy mode; only the FLIP variant is
 * not. */
#define UI_XSCALE (SCREEN_W / SCREEN_BASE_W)

typedef struct {
    float x, y, z;     /* Doom convention: x/y is the floor plane, z is up */
    float angle;       /* radians; 0 looks along +x */

    /* Projection plane distance, in pixels, per axis.
     *
     * Square-pixel modes have focal_x == focal_y. 640x240 has pixels half as
     * wide as they are tall, so focal_x doubles while focal_y stays put: the
     * field of view is unchanged and only the horizontal sampling rate rises.
     *
     * Holding focal_y fixed is not just about aspect. Projected Y is never
     * clipped here -- only clamped against RDP_Y_GUARD (see r_wall.c), and
     * that clamp collapses quad edges, which is a known source of visible
     * artefacts on near geometry. Near walls already project to y = -3360 at
     * 320 wide; scaling Y too would double every such value and drive far
     * more geometry into the clamp. Wide mode leaves the entire vertical
     * pipeline bit-identical to the 320 build. */
    float focal_x;
    float focal_y;
} r_camera_t;

typedef struct {
    float x1, y1;      /* wall segment, viewed top-down */
    float x2, y2;
    float zbot, ztop;  /* vertical extent in world units */

    /* Texture alignment, in Doom's terms: one texel per world unit, never
     * stretched to fit. `u0` is the texel column at (x1,y1) -- the sidedef's
     * x offset plus the seg's distance along its linedef, which is what keeps
     * adjacent segs of one wall continuous. `ztex` is the world height at
     * which texture row 0 sits, so anchoring (pegged top or bottom) is just a
     * choice of ztex. */
    float u0;
    float ztex;
    float len;         /* wall length; precomputed, see r_setup_walls */

    dt64_tex_t *tex;
    uint8_t light;     /* sector light level, 0..255 */

    /* Camera-space endpoint transform {depth, offset} x2, filled by a caller
     * that already projected this segment (the BSP walk does, in
     * screen_span). Zero has_xform -- the default -- and the renderer
     * computes it itself; callers that set it must use the r_set_view basis
     * of the same frame. */
    float   cd1, co1, cd2, co2;
    uint8_t has_xform;
    /* Two-sided masked mid (fence, grate): alpha-cut against the texture's
     * transparent index, and never merged or tiled past one texture height. */
    uint8_t masked;

    /* Light spilling up from an emissive floor -- sludge, lava, blood. glow is
     * how much it adds at the waterline, glowz the world height of that floor.
     * Zero for the overwhelming majority of walls, and the whole term compiles
     * out with DYNLIGHT off. Carried per wall rather than looked up per corner
     * because the sector is known once, where the wall is built. */
#if D_DYNLIGHT
    float   glow;
    float   glowz;
    /* Colour of the emissive floor's light (max channel 1.0, from the
     * flat's own art via D_FlatGlowRGB); {1,1,1} when glow is zero. Feeds
     * the per-quad tint -- brightness stays per-corner via `glow`. */
    float   glow_rgb[3];
    /* The adjacent floor mirrors (every glowing liquid, plus the polished
     * surfaces registered reflective-only). glowz doubles as the mirror
     * plane and glow_rgb as its tint; glow stays 0 for the non-liquids,
     * which nulls the light spill without a second code path. */
    uint8_t reflect;
#endif
} r_wall_t;

/* Set the render mode up for wall drawing and start a new batch. Call once per
 * frame before any r_draw_wall, after rdpq_attach. */
void r_setup_walls(void);

/* Cache the camera basis for the frame. The wall clipper is called once per
 * uncovered column range -- several times per wall in a busy view -- and was
 * recomputing sin and cos of the view angle on every one of those calls.
 * newlib's are software routines costing hundreds of cycles each. */
void r_set_view(const r_camera_t *cam);

/* The cached basis itself, for the other per-frame consumers (flats, sprites,
 * BSP traversal). Valid after r_set_view until the camera moves. */
extern float r_view_cs, r_view_sn;

/* Members of the packed hot-code block (repo-local n64.ld places
 * .text.hot contiguously, first in .text). The per-seg walk's working set
 * exceeds the 16 KB direct-mapped I-cache, so SOME functions must alias;
 * this block chooses which by keeping the nine per-seg core functions
 * mutually conflict-free (~14.4 KB) and its layout deterministic across
 * relinks. Membership changes must keep the block under 16 KB and be
 * re-measured on hardware (ph_walk_us) -- every placement change re-rolls
 * every conflict outside the block. Host builds ignore the section. */
#define R_HOT __attribute__((section(".text.hot")))

/* Light-diminishing falloff, shared by walls, flats and sprites: fraction
 * of a surface's light that survives to the eye at `depth`. `inv` comes
 * from r_fog_inv[sector light]: with FOGSCALE=1 the far distance scales
 * with sector brightness (vanilla's diminishing behaviour); entry 255 is
 * bit-identical to the legacy fixed 512..3500 ramp. Built by
 * r_setup_walls; 1 KB of BSS. */
extern float r_fog_inv[256];
#define R_FOG_NEAR 512.0f

static inline float r_vis(float depth, float inv)
{
    float fog = (depth - R_FOG_NEAR) * inv;
    if (fog < 0.0f) fog = 0.0f;
    if (fog > 1.0f) fog = 1.0f;
    return 1.0f - fog;
}

/* Project and clip one wall, appending its tiles to the batch. Nothing reaches
 * the RDP until r_flush_walls. */
void r_draw_wall(const r_camera_t *cam, const r_wall_t *wall);

/* As r_draw_wall, but restricted to screen columns [xmin, xmax).
 *
 * This is how occlusion culling reaches the renderer: the BSP walk knows which
 * columns are already covered by nearer solid walls, and a column range is
 * expressible as two extra clip planes, so restricting a wall costs the same
 * as the frustum clip it already performs. */
void r_draw_wall_clipped(const r_camera_t *cam, const r_wall_t *wall,
                         float xmin, float xmax);

/* As r_draw_wall_clipped, additionally bounded to screen rows
 * [ymin, ymax) -- the conservative vertical window the BSP walk tracks
 * through doorways and window slits, so geometry beyond an opening is not
 * rasterised above and below it. */
void r_draw_wall_win(const r_camera_t *cam, const r_wall_t *wall,
                     float xmin, float xmax, float ymin, float ymax);

/* Emit the batch, grouped so each TMEM tile is uploaded once and then serves
 * every quad that samples it. Call once after the last r_draw_wall.
 *
 * IMPORTANT: grouping reorders drawing, which is only sound when the batched
 * geometry does not overlap in screen space -- there is no Z-buffer, so with
 * overlapping quads whatever draws last wins. A convex room satisfies this
 * trivially. Real levels will satisfy it via Doom's solid-segment clipping,
 * which trims occluded columns on the CPU during front-to-back BSP traversal
 * and leaves no overdraw to order. Adding overlapping geometry (sprites,
 * masked midtextures) means drawing it in a separate, ordered pass. */
void r_flush_walls(void);

/* TMEM uploads issued by the last r_flush_walls. This, not triangle count, is
 * what governs wall throughput on real hardware. */
int r_tmem_uploads(void);

/* Quads dropped because the batch was full, so silent truncation is visible. */
int r_quads_dropped(void);

/* Triangles submitted this frame, by subsystem. rdpq_triangle computes every
 * setup coefficient on the CPU in floating point, so this is a direct proxy
 * for CPU cost -- more so than pixels are. */
extern int r_tri_wall, r_tri_flat, r_tri_spr, r_tri_sky;

/* Triangle setup: RSP (default) or CPU.
 *
 * rdpq_triangle() dispatches to rdpq_triangle_rsp(), so the RSP already
 * computes every edge slope and attribute gradient -- the CPU only ships raw
 * vertices. The alternative entry point does that setup on the VR4300 instead
 * and emits the pre-assembled passthrough commands.
 *
 * Both are handled by the same RSP overlay, so this switches cleanly at link
 * time. It exists to answer which unit is actually the constraint: if moving
 * setup onto the CPU makes the frame *faster*, the RSP was the bottleneck. */
#if R_TRI_CPU
void rdpq_triangle_cpu(const rdpq_trifmt_t *fmt,
                       const float *v1, const float *v2, const float *v3);
#define rdpq_triangle(f, a, b, c) rdpq_triangle_cpu((f), (a), (b), (c))
#endif
int r_quads_emitted(void);

#endif /* R_WALL_H */
