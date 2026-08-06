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

#elif defined(N64) && R_TRIFAST

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

#endif

#endif /* R_TRI_H */
