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

#define SCREEN_W 320
#define SCREEN_H 240

typedef struct {
    float x, y, z;     /* Doom convention: x/y is the floor plane, z is up */
    float angle;       /* radians; 0 looks along +x */
    float focal;       /* projection plane distance, in pixels */
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
