/*
 * Quad and fan submission with RSP vertex-slot reuse. See r_tri.h for why
 * this exists and why reordering the triangles is safe.
 */
#include "r_tri.h"

#if defined(N64) && R_TRIFAST && !R_NOTRI

#include <math.h>

/* The RSP keeps three vertex slots, each ROUND_UP((2+1+1+3)*4, 16) bytes
 * apart. Mirrors TRI_DATA_LEN in libdragon's rdpq_tri.c; a mismatch would
 * scatter vertices across the wrong slots, so it is checked below against
 * the same expression rather than pasted as a bare 32. */
#define TRI_DATA_LEN (((2 + 1 + 1 + 3) * 4 + 15) & ~15)

/* Near constant for the depth value. Walls and flats share it (NEAR_PLANE
 * and R_FLAT_NEAR are the same number), and clamping at zero matches what
 * the flat path already did; a wall nearer than the near plane is clipped
 * long before it reaches here. */
#define R_TRI_NEAR 4.0f
_Static_assert(TRI_DATA_LEN == 32, "RSP triangle slot stride changed");

/* Float to s16.16, clamped exactly as rdpq does: trunc.w.s raises an
 * unimplemented-operation exception on overflow, which would land in the
 * exception handler rather than merely producing a wrong pixel. */
static inline int32_t f_to_s16_16(float f)
{
    if (f >= 32768.0f)  return 0x7FFFFFFF;
    if (f < -32768.0f)  return (int32_t)0x80000000;
    return (int32_t)floorf(f * 65536.0f);
}

/* Convert one vertex and place it in an RSP slot. The conversion is a
 * transcription of rdpq_triangle_rsp's inner loop -- deliberately, so the
 * two paths produce bit-identical vertex data and can be A/B'd. */
static inline void tri_vertex(const rdpq_trifmt_t *fmt, int slot, const float *v)
{
    const int16_t x = floorf(v[fmt->pos_offset + 0] * 4.0f);
    const int16_t y = floorf(v[fmt->pos_offset + 1] * 4.0f);

    int16_t z = 0;
    if (fmt->z_offset >= 0)
        z = v[fmt->z_offset] * 0x7FFF;

    int32_t rgba = 0;
    if (fmt->shade_offset >= 0) {
        const uint32_t r = v[fmt->shade_offset + 0] * 255.0f;
        const uint32_t g = v[fmt->shade_offset + 1] * 255.0f;
        const uint32_t b = v[fmt->shade_offset + 2] * 255.0f;
        const uint32_t a = v[fmt->shade_offset + 3] * 255.0f;
        rgba = (int32_t)((r << 24) | (g << 16) | (b << 8) | a);
    }

    int16_t s = 0, t = 0;
    int32_t w = 0, inv_w = 0;
    if (fmt->tex_offset >= 0) {
        s     = v[fmt->tex_offset + 0] * 32.0f;
        t     = v[fmt->tex_offset + 1] * 32.0f;
        w     = f_to_s16_16(1.0f / v[fmt->tex_offset + 2]);
        inv_w = f_to_s16_16(       v[fmt->tex_offset + 2]);
    }

    rspq_write(RDPQ_OVL_ID, RDPQ_CMD_TRIANGLE_DATA,
               TRI_DATA_LEN * slot,
               (x << 16) | (y & 0xFFFF),
               (z << 16),
               rgba,
               (s << 16) | (t & 0xFFFF),
               w,
               inv_w);
}

/* Draw whatever the three slots currently hold. */
static inline void tri_issue(const rdpq_trifmt_t *fmt)
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

/* Cleared by r_tri_group_begin, set once the group's autosync is on
 * record. Only the first primitive after a tile or TMEM change has to go
 * through rdpq_triangle to register it. */
static int group_registered;

void r_tri_group_begin(void) { group_registered = 0; }

/* One vertex straight from the batch's six floats. Same arithmetic
 * rdpq performs, minus the ten-float layout it would have been copied
 * into and read back out of. */
static inline void tri_vertex6(const rdpq_trifmt_t *fmt, int slot,
                               const float *v)
{
    const int16_t x = floorf(v[0] * 4.0f);
    const int16_t y = floorf(v[1] * 4.0f);

    const float iw = v[5];
    float zf = 1.0f - R_TRI_NEAR * iw;
    if (zf < 0.0f) zf = 0.0f;
    const int16_t z = zf * 0x7FFF;

    const uint32_t c = v[2] * 255.0f;
    const int32_t rgba = (int32_t)((c << 24) | (c << 16) | (c << 8) | 255);

    const int16_t s = v[3] * 32.0f;
    const int16_t t = v[4] * 32.0f;

    rspq_write(RDPQ_OVL_ID, RDPQ_CMD_TRIANGLE_DATA,
               TRI_DATA_LEN * slot,
               (x << 16) | (y & 0xFFFF),
               (z << 16),
               rgba,
               (s << 16) | (t & 0xFFFF),
               f_to_s16_16(1.0f / iw),
               f_to_s16_16(iw));
    (void)fmt;
}

void r_tri_quad6(const rdpq_trifmt_t *fmt, const float v[4][6])
{
    if (!group_registered) {
        /* First primitive since the tile changed: through rdpq, so the
         * autosync requirement is recorded for everything that follows. */
        float x[4][10];
        for (int k = 0; k < 4; k++) {
            x[k][0] = v[k][0]; x[k][1] = v[k][1];
            x[k][2] = x[k][3] = x[k][4] = v[k][2];
            x[k][5] = 1.0f;
            x[k][6] = v[k][3]; x[k][7] = v[k][4]; x[k][8] = v[k][5];
            float z = 1.0f - R_TRI_NEAR * v[k][5];
            x[k][9] = z < 0.0f ? 0.0f : z;
        }
        rdpq_triangle(fmt, x[0], x[1], x[2]);
        group_registered = 1;
        tri_vertex6(fmt, 1, v[3]);
        tri_issue(fmt);
        return;
    }

    tri_vertex6(fmt, 0, v[0]);
    tri_vertex6(fmt, 1, v[1]);
    tri_vertex6(fmt, 2, v[2]);
    tri_issue(fmt);
    tri_vertex6(fmt, 1, v[3]);      /* slots read {v0,v3,v2} */
    tri_issue(fmt);
}

void r_tri_quad(const rdpq_trifmt_t *fmt, const float *v0, const float *v1,
                const float *v2, const float *v3)
{
    /* First triangle through rdpq: it records the autosync requirements and
     * fills slots 0,1,2 with v0,v1,v2. */
    rdpq_triangle(fmt, v0, v1, v2);

    /* Second triangle is {v0,v2,v3}. v0 is already in slot 0 and v2 in
     * slot 2, so only v3 has to be sent -- into slot 1, which makes the
     * slots read {v0,v3,v2}: the same triangle wound the other way, which
     * the RSP resolves when it sorts. */
    tri_vertex(fmt, 1, v3);
    tri_issue(fmt);
}

void r_tri_fan(const rdpq_trifmt_t *fmt, const float (*sv)[10], int m)
{
    if (m < 3) return;

    if (!group_registered) {
        rdpq_triangle(fmt, sv[0], sv[1], sv[2]);
        group_registered = 1;
    } else {
        tri_vertex(fmt, 0, sv[0]);
        tri_vertex(fmt, 1, sv[1]);
        tri_vertex(fmt, 2, sv[2]);
        tri_issue(fmt);
    }

    /* sv[0] stays in slot 0 for the whole fan. Each further triangle shares
     * its previous neighbour, so it needs one new vertex, alternating into
     * the slot the triangle before last used:
     *
     *   slots after start : [v0 v1 v2]
     *   i=3 -> slot1=v3   : [v0 v3 v2]  = {v0,v2,v3}
     *   i=4 -> slot2=v4   : [v0 v3 v4]  = {v0,v3,v4}
     *   i=5 -> slot1=v5   : [v0 v5 v4]  = {v0,v4,v5}
     */
    for (int i = 3; i < m; i++) {
        tri_vertex(fmt, (i & 1) ? 1 : 2, sv[i]);
        tri_issue(fmt);
    }
}

#endif /* N64 && R_TRIFAST && !R_NOTRI */

#if D_TRIBENCH
/* Submit `iters` textured/shaded/z quads through either path and return the
 * elapsed microseconds. Vertices vary per iteration so nothing can be
 * hoisted or cached away; the caller scissors the output to one pixel. */
uint32_t r_tri_bench(int iters, int fast)
{
    static const rdpq_trifmt_t FMT = {
        .pos_offset = 0, .shade_offset = 2, .tex_offset = 6,
        .tex_tile = TILE0, .z_offset = 9,
    };
    float v[4][10];
    if (fast == 2) r_tri_group_begin();
    const uint32_t t0 = TICKS_READ();

    for (int i = 0; i < iters; i++) {
        const float o = (float)(i & 31);
        for (int k = 0; k < 4; k++) {
            v[k][0] = o + (k == 1 || k == 2 ? 16.0f : 0.0f);
            v[k][1] = o + (k >= 2 ? 16.0f : 0.0f);
            v[k][2] = v[k][3] = v[k][4] = 0.75f;
            v[k][5] = 1.0f;
            v[k][6] = (k == 1 || k == 2) ? 16.0f : 0.0f;
            v[k][7] = (k >= 2) ? 16.0f : 0.0f;
            v[k][8] = 1.0f / (32.0f + o);
            v[k][9] = 0.5f;
        }
        if (fast == 2) {
            /* Compact submission: the batch's own six floats, no expansion,
             * and no rdpq call once the group's autosync is registered. */
            float c[4][6];
            for (int k = 0; k < 4; k++) {
                c[k][0] = v[k][0]; c[k][1] = v[k][1]; c[k][2] = v[k][2];
                c[k][3] = v[k][6]; c[k][4] = v[k][7]; c[k][5] = v[k][8];
            }
            r_tri_quad6(&FMT, (const float (*)[6])c);
        } else if (fast) {
            r_tri_quad(&FMT, v[0], v[1], v[2], v[3]);
        } else {
            rdpq_triangle(&FMT, v[0], v[1], v[2]);
            rdpq_triangle(&FMT, v[0], v[2], v[3]);
        }
    }
    return TICKS_TO_US(TICKS_SINCE(t0));
}
#endif
