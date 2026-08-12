/*
 * r_tri -- quad and fan submission with RSP vertex-slot reuse.
 *
 * rdpq_triangle does the triangle *setup* on the RSP, but the per-vertex
 * conversion to fixed point happens on the CPU: two floorf, several
 * multiplies and a divide per vertex, then six command words. Measured on
 * the demo route, submission is around a third of all CPU time, and the
 * frame is CPU-bound in exactly this path on hardware.
 *
 * The protocol writes each vertex into one of three RSP slots, addressed
 * explicitly (RDPQ_CMD_TRIANGLE_DATA takes the slot offset), and a separate
 * command draws whatever the three slots currently hold. Consecutive
 * triangles that share vertices can therefore leave them in place instead
 * of converting and re-sending them:
 *
 *   quad  v0 v1 v2 v3 -> 4 vertex writes instead of 6
 *   fan   of m verts  -> m writes instead of 3*(m-2)
 *
 * Vertex order does not matter: the RSP sorts the three by Y and derives
 * the major-edge flag from the cross-product sign, and culling is disabled
 * in the overlay -- so the second triangle of a quad may be sent as
 * {v0,v3,v2} rather than {v0,v2,v3} and rasterises identically.
 *
 * The first triangle of each primitive goes through rdpq_triangle itself,
 * which is what registers the autosync requirements (pipe, tile, TMEM).
 * The extra triangles that follow use the same tile and the same TMEM, so
 * they add no requirement the first has not already recorded, and nothing
 * can come between them and it.
 */
#ifndef R_TRI_H
#define R_TRI_H

#include <libdragon.h>

#ifndef R_TRIFAST
#define R_TRIFAST 1
#endif
#ifndef R_NOTRI
#define R_NOTRI 0
#endif

#if R_NOTRI

/* Attribution build: keep every bit of CPU work up to submission, drop the
 * submissions themselves. Renders nothing; only for phase measurement. */
static inline void r_tri_quad(const rdpq_trifmt_t *fmt, const float *v0,
                              const float *v1, const float *v2, const float *v3)
{ (void)fmt; (void)v0; (void)v1; (void)v2; (void)v3; }
static inline void r_tri_fan(const rdpq_trifmt_t *fmt,
                             const float (*sv)[10], int m)
{ (void)fmt; (void)sv; (void)m; }
static inline void r_tri_group_begin(void) { }
static inline void r_tri_quad6(const rdpq_trifmt_t *fmt, const float v[4][6])
{ (void)fmt; (void)v; }
static inline void r_tri_quad6t(const rdpq_trifmt_t *fmt, const float v[4][6],
                                uint32_t tint)
{ (void)fmt; (void)v; (void)tint; }
/* Attribution build keeps the push-time conversion cost and drops the
 * submissions, which is truer to its charter than the float path was. */
#ifndef R_TRI_QUANT
#define R_TRI_QUANT 1
#endif
#define R_TRI_NEAR 4.0f
#include "r_fastmath.h"
static inline void r_tri_slot(int slot, int32_t xy, int32_t z16, int32_t rgba,
                              int32_t st, int32_t w, int32_t inv_w)
{ (void)slot; (void)xy; (void)z16; (void)rgba; (void)st; (void)w; (void)inv_w; }
static inline void r_tri_slots_issue(const rdpq_trifmt_t *fmt) { (void)fmt; }
static inline void r_tri_group_use(uint32_t bits) { (void)bits; }
static inline int32_t r_tri_s16_16(float f)
{
    if (f >= 32768.0f)  return 0x7FFFFFFF;
    if (f < -32768.0f)  return (int32_t)0x80000000;
    return rf_floor_i32(f * 65536.0f);
}

#elif defined(N64) && R_TRIFAST

/* The direct-submission machinery lives here rather than in r_tri.c so that
 * per-vertex emitters can inline it straight inside their loops -- r_flat's
 * fan writer converts each vertex in registers and writes the RSP slot
 * words with no staging array in between. r_tri.c uses the same inlines, so
 * there is exactly one copy of the conversion arithmetic. */

/* The RSP keeps three vertex slots, each ROUND_UP((2+1+1+3)*4, 16) bytes
 * apart. Mirrors TRI_DATA_LEN in libdragon's rdpq_tri.c; a mismatch would
 * scatter vertices across the wrong slots, so it is checked against the
 * same expression rather than pasted as a bare 32. */
#define R_TRI_DATA_LEN (((2 + 1 + 1 + 3) * 4 + 15) & ~15)
_Static_assert(R_TRI_DATA_LEN == 32, "RSP triangle slot stride changed");

/* Near constant for the depth value. Walls and flats share it (NEAR_PLANE
 * and R_FLAT_NEAR are the same number); clamping at zero matches the flat
 * path. Lives here because push-time quantization computes z outside
 * r_tri.c. */
#define R_TRI_NEAR 4.0f

#include "r_fastmath.h"

/* The wall batch stores RSP-format integers at push time and the flush is
 * a pure copy (see r_wall.c). QUANT=0 restores the float record and the
 * flush-time converters -- the A/B lever for proving the two produce the
 * same command stream. Forced 0 in the reference/host branch below. */
#ifndef R_TRI_QUANT
#define R_TRI_QUANT 1
#endif

/* rdpq_triangle's ONLY side effect beyond the slot writes the flush already
 * replicates is one __rdpq_autosync_use(bits) recording that a triangle
 * consumes the pipe, TILE0 and TMEM -- pure CPU-side bookkeeping (the bits
 * are consumed by the next mode/upload change, which emits the matching
 * SYNCs). Registering the same bits directly lets every triangle of a
 * group go through the slot path; the guard reproduces the old group
 * leader's timing exactly (registration fires with the FIRST triangle
 * after each group_begin, after that iteration's upload and alphacompare
 * calls). Internal libdragon API, stable because libdragon is vendored --
 * see rdpq_tri.c's autosync block for the source of truth. */
extern void __rdpq_autosync_use(uint32_t);
#define R_TRI_USE_WALL (AUTOSYNC_PIPE | AUTOSYNC_TILE(0) | AUTOSYNC_TMEM(0))

static inline void r_tri_group_use(uint32_t bits)
{
    extern int r_tri_group_reg;
    if (!r_tri_group_reg) {
        r_tri_group_reg = 1;
        __rdpq_autosync_use(bits);
    }
}

/* Float to s16.16, clamped exactly as rdpq does: the conversion instruction
 * raises an unimplemented-operation exception on overflow, which would land
 * in the exception handler rather than merely producing a wrong pixel. */
static inline int32_t r_tri_s16_16(float f)
{
    if (f >= 32768.0f)  return 0x7FFFFFFF;
    if (f < -32768.0f)  return (int32_t)0x80000000;
    return rf_floor_i32(f * 65536.0f);
}

/* Nonzero once the current group's autosync is on record; cleared by
 * r_tri_group_begin. Only the first primitive after a tile or TMEM change
 * has to go through rdpq_triangle to register it. */
extern int r_tri_group_reg;

/* Write one converted vertex into an RSP slot. */
static inline void r_tri_slot(int slot, int32_t xy, int32_t z16, int32_t rgba,
                              int32_t st, int32_t w, int32_t inv_w)
{
    rspq_write(RDPQ_OVL_ID, RDPQ_CMD_TRIANGLE_DATA,
               R_TRI_DATA_LEN * slot, xy, z16, rgba, st, w, inv_w);
}

/* Draw whatever the three slots currently hold. */
static inline void r_tri_slots_issue(const rdpq_trifmt_t *fmt)
{
    uint32_t cmd_id = RDPQ_CMD_TRI;
    if (fmt->shade_offset >= 0) cmd_id |= 0x4;
    if (fmt->tex_offset   >= 0) cmd_id |= 0x2;
    if (fmt->z_offset     >= 0) cmd_id |= 0x1;

    rspq_write(RDPQ_OVL_ID, RDPQ_CMD_TRIANGLE,
               0xC000 | (cmd_id << 8) |
               (fmt->tex_mipmaps ? (fmt->tex_mipmaps - 1) << 3 : 0) |
               (fmt->tex_tile & 7));
}

/* Nonzero when emitters may take the direct slot path for follow-on
 * primitives of the current group. */
#define R_TRI_DIRECT 1

void r_tri_quad(const rdpq_trifmt_t *fmt, const float *v0, const float *v1,
                const float *v2, const float *v3);
void r_tri_fan(const rdpq_trifmt_t *fmt, const float (*sv)[10], int m);

/* Call immediately after anything that changes the tile or TMEM.
 *
 * Only the first primitive after such a change has to go through
 * rdpq_triangle, which is what registers the autosync requirement; every
 * primitive after it uses the same tile and the same TMEM and adds no
 * requirement of its own, so it can be written straight into the RSP's
 * slots. Getting this boundary wrong does not produce a wrong pixel, it
 * produces a missing sync -- so it is placed at every upload site rather
 * than once per texture group. */
void r_tri_group_begin(void);

/* Submit a quad straight from the batch's own six-float vertices --
 * x, y, shade, s, t, 1/w -- with no expansion into rdpq's ten-float
 * layout in between. The expansion existed only to be read back and
 * converted a second time. */
void r_tri_quad6(const rdpq_trifmt_t *fmt, const float v[4][6]);

/* Same, with a per-quad hue: tint is 0x00RRGGBB, and each vertex's scalar
 * shade multiplies the three channels instead of replicating. Only quads a
 * coloured light or glow actually reaches come through here. */
void r_tri_quad6t(const rdpq_trifmt_t *fmt, const float v[4][6],
                  uint32_t tint);

#else

/* Reference path: one rdpq_triangle per triangle. This is what the host
 * harness rasterises, so the tests check the same call sites either way. */
static inline void r_tri_quad(const rdpq_trifmt_t *fmt, const float *v0,
                              const float *v1, const float *v2, const float *v3)
{
    rdpq_triangle(fmt, v0, v1, v2);
    rdpq_triangle(fmt, v0, v2, v3);
}

static inline void r_tri_fan(const rdpq_trifmt_t *fmt,
                             const float (*sv)[10], int m)
{
    for (int i = 1; i + 1 < m; i++)
        rdpq_triangle(fmt, sv[0], sv[i], sv[i + 1]);
}

static inline void r_tri_group_begin(void) { }

/* Reference/host path keeps the float record end to end -- no slot
 * machinery exists here, so the quantized record cannot compile. */
#undef R_TRI_QUANT
#define R_TRI_QUANT 0

/* Reference expansion, so the host harness rasterises the same geometry. */
static inline void r_tri_quad6(const rdpq_trifmt_t *fmt, const float v[4][6])
{
    float x[4][10];
    for (int k = 0; k < 4; k++) {
        x[k][0] = v[k][0]; x[k][1] = v[k][1];
        x[k][2] = x[k][3] = x[k][4] = v[k][2];
        x[k][5] = 1.0f;
        x[k][6] = v[k][3]; x[k][7] = v[k][4]; x[k][8] = v[k][5];
        float z = 1.0f - 4.0f * v[k][5];
        x[k][9] = z < 0.0f ? 0.0f : z;
    }
    rdpq_triangle(fmt, x[0], x[1], x[2]);
    rdpq_triangle(fmt, x[0], x[2], x[3]);
}

static inline void r_tri_quad6t(const rdpq_trifmt_t *fmt, const float v[4][6],
                                uint32_t tint)
{
    const float trf = (float)((tint >> 16) & 0xFF) * (1.0f / 255.0f);
    const float tgf = (float)((tint >> 8) & 0xFF) * (1.0f / 255.0f);
    const float tbf = (float)(tint & 0xFF) * (1.0f / 255.0f);
    float x[4][10];
    for (int k = 0; k < 4; k++) {
        x[k][0] = v[k][0]; x[k][1] = v[k][1];
        x[k][2] = v[k][2] * trf;
        x[k][3] = v[k][2] * tgf;
        x[k][4] = v[k][2] * tbf;
        x[k][5] = 1.0f;
        x[k][6] = v[k][3]; x[k][7] = v[k][4]; x[k][8] = v[k][5];
        float z = 1.0f - 4.0f * v[k][5];
        x[k][9] = z < 0.0f ? 0.0f : z;
    }
    rdpq_triangle(fmt, x[0], x[1], x[2]);
    rdpq_triangle(fmt, x[0], x[2], x[3]);
}

#endif

#endif /* R_TRI_H */
