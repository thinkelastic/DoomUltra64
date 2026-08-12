/* Emissive-flat glow lookups, inline.
 *
 * The registry is tiny (a handful of NUKAGE/LAVA/SLIME entries per level)
 * and the queries sit in the BSP walk -- two per projected seg and up to
 * three per flat region. As out-of-line calls in d_bridge.c they paid call
 * overhead and an I-cache round trip per query for a loop of at most
 * D_GLOW_MAX compares; inlined, the empty-registry case folds to one load
 * and a branch inside the walk's own lines.
 *
 * The writers (registration at flat-name resolution, the lazy RGB bake, the
 * per-level reset) stay in d_bridge.c.
 */
#ifndef D_GLOW_H
#define D_GLOW_H

#include <stdint.h>

#ifndef D_DYNLIGHT
#define D_DYNLIGHT 0
#endif

/* Which liquid family a glowing flat belongs to -- the vapor layer picks
 * its look from this (green haze over sludge, smoke over lava). */
enum {
    D_GLOW_NONE = 0,
    D_GLOW_NUKAGE,
    D_GLOW_LAVA,
    D_GLOW_SLIME,
    D_GLOW_BLOOD,
    D_GLOW_WATER,
};

#if D_DYNLIGHT

#define D_GLOW_MAX 8

extern int16_t d_glow_pic[D_GLOW_MAX];
extern float   d_glow_amt[D_GLOW_MAX];
extern float   d_glow_rgb[D_GLOW_MAX][3];   /* light colour, from the art */
extern uint8_t d_glow_have_rgb[D_GLOW_MAX];
extern uint8_t d_glow_class[D_GLOW_MAX];
extern int     d_num_glow;

/* Lazily bakes d_glow_rgb[slot] from the flat's texels; d_bridge.c. */
void d_glow_bake_rgb(int slot, int picnum);

/* How much light this flat adds to itself, in the same 0..1 units as sector
 * light. 0 for everything that is not emissive. */
static inline float D_FlatGlow(int picnum)
{
    for (int i = 0; i < d_num_glow; i++)
        if (d_glow_pic[i] == (int16_t)picnum) return d_glow_amt[i];
    return 0.0f;
}

/* Liquid family of this flat; D_GLOW_NONE for anything not emissive. */
static inline int D_FlatGlowClass(int picnum)
{
    for (int i = 0; i < d_num_glow; i++)
        if (d_glow_pic[i] == (int16_t)picnum) return d_glow_class[i];
    return D_GLOW_NONE;
}

/* Colour of the light this flat casts. White for anything not emissive, so a
 * caller can multiply unconditionally. */
static inline void D_FlatGlowRGB(int picnum, float rgb[3])
{
    for (int i = 0; i < d_num_glow; i++)
        if (d_glow_pic[i] == (int16_t)picnum) {
            if (!d_glow_have_rgb[i]) d_glow_bake_rgb(i, picnum);
            rgb[0] = d_glow_rgb[i][0];
            rgb[1] = d_glow_rgb[i][1];
            rgb[2] = d_glow_rgb[i][2];
            return;
        }
    rgb[0] = rgb[1] = rgb[2] = 1.0f;
}

#else

static inline float D_FlatGlow(int picnum) { (void)picnum; return 0.0f; }
static inline int   D_FlatGlowClass(int picnum) { (void)picnum; return D_GLOW_NONE; }
static inline void  D_FlatGlowRGB(int picnum, float rgb[3])
{ (void)picnum; rgb[0] = rgb[1] = rgb[2] = 1.0f; }

#endif /* D_DYNLIGHT */

#endif /* D_GLOW_H */
