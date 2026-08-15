/* Automap line rendering: chocolate's plotter draws palette-indexed pixels
 * into a CPU frame buffer; here each line becomes a screen-space thin quad
 * through the RDP, colored by looking the palette index up in the live
 * TLUT (so damage flashes tint the map exactly as they tint the world).
 *
 * The map's 320x200 coordinate space rides 20 rows down in the 240-row
 * frame, the same centring every full-screen UI state uses. */
#include <libdragon.h>
#include "dt64.h"
#include "r_wall.h"          /* UI_XSCALE */

#define AM_YSHIFT 20.0f
#define AM_HALF   0.6f      /* half-width: one-pixel lines with a little body */

static const rdpq_trifmt_t TRIFMT_AM = {
    .pos_offset   = 0,
    .shade_offset = -1,     /* -1 disables an attribute; zero would read   */
    .tex_offset   = -1,     /* the position floats as that attribute and   */
    .z_offset     = -1,     /* trip the FPU on denormals. */
};

static uint32_t am_cur_color = 0xFFFFFFFF;

void r_am_begin(void)
{
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER1((0, 0, 0, PRIM), (0, 0, 0, 1)));
    rdpq_mode_persp(false);
    /* No z attribute in the triangle format, so the z unit must be off
     * outright. The emulator tolerated the stale inherited z state; the
     * real RDP compared garbage depth and dropped every line. */
    rdpq_mode_zbuf(false, false);
    am_cur_color = 0xFFFFFFFF;
}

void r_am_line(int x0, int y0, int x1, int y1, int palette_index)
{
    const uint16_t c16 = dt64_tlut_color(palette_index);
    const uint32_t c = ((uint32_t)((c16 >> 11) & 31) << 27) |
                       ((uint32_t)((c16 >>  6) & 31) << 19) |
                       ((uint32_t)((c16 >>  1) & 31) << 11) | 0xFF;
    if (c != am_cur_color) {
        am_cur_color = c;
        rdpq_set_prim_color(color_from_packed32(c));
    }

    float ax = (float)x0, ay = (float)y0 + AM_YSHIFT;
    float bx = (float)x1, by = (float)y1 + AM_YSHIFT;
    float dx = bx - ax, dy = by - ay;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) { bx = ax + 1.0f; by = ay; dx = 1.0f; dy = 0.0f; len = 1.0f; }
    const float nx = -dy / len * AM_HALF;
    const float ny =  dx / len * AM_HALF;

    /* The quad is built entirely in Doom's 320x240 frame and scaled on the
     * way out. Widening the line in the scaled space instead would halve its
     * apparent thickness at 640x240, where a pixel is half as wide: this way
     * the line is the 320 line, stretched, and looks the same. At 480 both
     * axes scale, so the line thickens with everything else rather than
     * thinning to a hair across a screen with twice the rows. */
    const float kx = (float)UI_XSCALE, ky = (float)UI_YSCALE;
    const float v0[] = { (ax + nx) * kx, (ay + ny) * ky };
    const float v1[] = { (bx + nx) * kx, (by + ny) * ky };
    const float v2[] = { (bx - nx) * kx, (by - ny) * ky };
    const float v3[] = { (ax - nx) * kx, (ay - ny) * ky };
    rdpq_triangle(&TRIFMT_AM, v0, v1, v2);
    rdpq_triangle(&TRIFMT_AM, v0, v2, v3);
}
