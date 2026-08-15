/*
 * Minimal libdragon shim for host-side testing.
 *
 * Lets src/r_wall.c compile unmodified on the host against a mock RDP, so the
 * projection, near-clipping, TMEM tiling and fog maths can be verified in
 * milliseconds instead of via an emulator or a flashcart. Only the surface of
 * libdragon that r_wall.c actually touches is declared here.
 */
#ifndef HOST_LIBDRAGON_SHIM_H
#define HOST_LIBDRAGON_SHIM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------- surfaces */

typedef enum { FMT_CI8 = 1 } tex_format_t;

typedef struct {
    void        *buffer;
    tex_format_t format;
    uint16_t     width, height, stride;
} surface_t;

static inline surface_t surface_make_linear(void *buffer, tex_format_t format,
                                            uint16_t width, uint16_t height)
{
    return (surface_t){ buffer, format, width, height, width };
}

/* ------------------------------------------------------------ rdpq modes */
/* Render-state calls do not affect the geometry under test, so they are
 * inert here; the mock rasteriser hardcodes the equivalent pipeline. */

typedef enum { TILE0 = 0 } rdpq_tile_t;
typedef enum { TLUT_NONE = 0, TLUT_RGBA16 = 2 } rdpq_tlut_t;
typedef enum { FILTER_POINT = 0, FILTER_BILINEAR = 1 } rdpq_filter_t;

typedef struct { uint8_t r, g, b, a; } color_t;
#define RGBA32(r, g, b, a) ((color_t){ (r), (g), (b), (a) })

/* Combiner/blender descriptors carry token lists such as (TEX0,0,SHADE,0);
 * each is a single parenthesised macro argument, so they collapse cleanly. */
#define RDPQ_COMBINER1(rgb, alpha) 0
/* Blender formulas reduce to an opaque token on the host: the harness
 * checks geometry and TMEM discipline, not blending arithmetic. */
#define RDPQ_BLENDER(bl) 0
static inline void rdpq_mode_blender(int b) { (void)b; }
#define RDPQ_FOG_STANDARD          1

static inline void rdpq_set_mode_standard(void) {}
static inline void rdpq_set_mode_copy(bool transparency) { (void)transparency; }
static inline void rdpq_set_prim_color(color_t c) { (void)c; }
static inline void rdpq_mode_tlut(rdpq_tlut_t t) { (void)t; }
static inline void rdpq_mode_combiner(int c) { (void)c; }
static inline void rdpq_set_fog_color(color_t c) { (void)c; }
static inline void rdpq_mode_fog(int f) { (void)f; }
static inline void rdpq_mode_filter(rdpq_filter_t f) { (void)f; }

/* Antialiasing. The mock rasterises with a single sample per pixel, which
 * IS the AA_NONE behaviour r_wall_modes asserts, so the stub models the
 * only setting the port ever asks for. Missing entirely until now, which
 * silently broke the host harness the moment that assertion landed. */
typedef enum { AA_NONE = 0, AA_STANDARD = 1, AA_REDUCED = 2 } rdpq_antialias_t;
static inline void rdpq_mode_antialias(rdpq_antialias_t a) { (void)a; }

/* Mode bits the mock does not model. Worth naming explicitly rather than
 * omitting: SOM_TEXTURE_PERSP being off by default is exactly the kind of
 * state difference a software model silently hides, and it cost several
 * wrong diagnoses before being found on hardware. */
static inline void rdpq_mode_persp(bool p) { (void)p; }
static inline void rdpq_mode_zbuf(bool cmp, bool upd) { (void)cmp; (void)upd; }

/* ----------------------------------------------------------- rdpq draw */

typedef struct rdpq_trifmt_s {
    int         pos_offset;
    int         shade_offset;
    bool        shade_flat;
    int         tex_offset;
    rdpq_tile_t tex_tile;
    int         tex_mipmaps;
    int         z_offset;
} rdpq_trifmt_t;

/* Mirrors libdragon's sampling parameters closely enough for the renderer to
 * compile; the mock treats wrapping as clamping, which is why the resident
 * tile check in the harness still catches out-of-range sampling. */
#define REPEAT_INFINITE 2048.0f

typedef struct {
    int tmem_addr;
    int palette;
    struct { float translate; int scale_log; float repeats; bool mirror; } s, t;
} rdpq_texparms_t;

int rdpq_tex_upload(rdpq_tile_t tile, const surface_t *tex,
                    const rdpq_texparms_t *parms);

/* Implemented by tests/host_render.c. */
int  rdpq_tex_upload_sub(rdpq_tile_t tile, const surface_t *tex,
                         const rdpq_texparms_t *parms,
                         int s0, int t0, int s1, int t1);
void rdpq_triangle(const rdpq_trifmt_t *fmt,
                   const float *v1, const float *v2, const float *v3);
void rdpq_texture_rectangle(rdpq_tile_t tile, float x0, float y0,
                            float x1, float y1, float s, float t);
void rdpq_texture_rectangle_scaled(rdpq_tile_t tile, float x0, float y0,
                                   float x1, float y1,
                                   float s0, float t0, float s1, float t1);

/* Sprite/flat sources compile against these too. */
static inline void rdpq_mode_alphacompare(int threshold) { (void)threshold; }

/* Timer surface for the renderer's instrumentation; the host build does not
 * measure time, only behaviour. */
static inline uint32_t TICKS_READ(void) { return 0; }
#define TICKS_TO_US(t)       (t)
#define TICKS_SINCE(t)       (0u - (t))
#define TICKS_DISTANCE(a, b) ((b) - (a))

#define assertf(cond, ...) \
    do { if (!(cond)) { fprintf(stderr, "assert: " __VA_ARGS__); \
                        fputc('\n', stderr); abort(); } } while (0)

#endif /* HOST_LIBDRAGON_SHIM_H */
