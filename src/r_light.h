/* Dynamic point lights.
 *
 * Doom has no lighting model in the 3D sense -- no normals, no light sources,
 * just a per-sector level and a distance falloff. This adds the one thing that
 * model cannot express: light that moves. A muzzle flash, a fireball crossing a
 * room, a rocket in flight.
 *
 * It costs no TMEM and no combiner stage, which is what makes it affordable
 * here when higher-resolution art is not. The RDP is already interpolating a
 * per-vertex shade across every surface; this only changes what value each
 * vertex is handed. Walls get one query per corner, flats one per polygon
 * vertex -- a few thousand multiply-adds a frame, against a submit path that
 * was until recently paying sixteen libm calls per wall quad.
 *
 * Scalar, not coloured. The batch vertex carries one shade float which the
 * flush fans out to r=g=b, so brightness is free and hue is not: colour would
 * mean widening the batch or carrying a per-quad tint. Worth doing, separately.
 *
 * DYNLIGHT=0 compiles every query to a constant zero, so the shade expression
 * folds back to exactly what it was and an ordinary ROM carries none of this.
 */
#ifndef R_LIGHT_H
#define R_LIGHT_H

#include <stdint.h>

#ifndef D_DYNLIGHT
#define D_DYNLIGHT 0
#endif

#if D_DYNLIGHT

/* Enough for a firefight without the per-vertex loop growing teeth. Once
 * it is full, the slot worth least HERE is the one that goes -- see
 * r_light_prio: the furthest contribute least and are most likely already
 * below a palette step. */
#define R_LIGHT_MAX 8

typedef struct {
    float x, y, z;
    float inv_r2;      /* 1 / radius^2, so the query needs no divide */
    float intensity;   /* added brightness at the centre, sector-light units */
    /* PREMULTIPLIED channel intensities: intensity * tint. CONVENTION: the
     * tint's max channel is 1.0, so intensity == max(ir,ig,ib) -- which is
     * what keeps the scalar query and the per-channel query in agreement
     * (the scalar add equals the coloured add's largest channel) and keeps
     * ranking slots by intensity meaningful. 32 bytes: two D-lines -- which
     * is why prio and the halo scale live in parallel arrays, not here. */
    float ir, ig, ib;
} r_light_t;

/* Exposed so the query below can inline. Written only by reset/add. */
extern r_light_t r_lights[R_LIGHT_MAX];

/* How big the visible glow IN THE AIR is, as a fraction of the light's
 * reach on surfaces. A parallel array rather than a field, deliberately:
 * r_light_t is two D-cache lines exactly and the per-vertex query reads
 * every byte of it, while this is read once per light per frame by the
 * halo pass alone. Explosions and projectiles want a tighter glow than
 * their reach suggests -- the light a fireball throws is wide, the
 * fireball itself is small. */
extern float r_light_halo[R_LIGHT_MAX];

/* What each slot is worth keeping, for eviction. A parallel array for the
 * same reason as the halo scales: r_light_t is two D-cache lines exactly
 * and the per-vertex query reads every byte of it, while this is read only
 * when a ninth light arrives in a frame. */
extern float r_light_prio[R_LIGHT_MAX];

/* Where the eye is, so a slot can be ranked by what it is worth HERE
 * rather than by its brightness at its own centre. Set once a frame,
 * before the walk; a light added without it still ranks, just from
 * wherever the eye last was. */
void r_light_eye(float x, float y);

/* Set the halo fraction for the NEXT r_light_add. Resets to 1.0 after,
 * so a caller that does not care never thinks about it. */
void r_light_halo_next(float frac);

/* How far the visible glow sits BELOW the light's own centre, in world
 * units, for the next r_light_add; resets to 0. They are not always the
 * same place. A flame lights a room from the top of its stand, which is
 * where the light belongs -- but the fire itself is a body just under
 * that, and a glow centred on the topmost pixel hangs half of itself in
 * the air above the prop. */
void r_light_halo_drop_next(float dz);
extern float r_light_halo_dz[R_LIGHT_MAX];

/* Halo alpha, when the light's own intensity is the wrong measure of how
 * brightly the thing should GLOW. The two are usually the same question --
 * a fireball that lights a room hard should blaze -- but not always: a
 * barrel's ooze is a small emissive detail that must be clearly visible
 * while lighting almost nothing, and it has to stay dim as a LIGHT because
 * barrels come in clusters of eight and the registry evicts by intensity.
 * 0 means "derive it from intensity", which is what everything else does. */
void r_light_halo_alpha_next(float a);
extern float r_light_halo_a[R_LIGHT_MAX];
extern int       r_light_num;

/* Clear the frame's list. Called once per frame before the BSP walk, since the
 * walk is what emits the geometry that reads it. */
void r_light_reset(void);

/* Add a light. radius is where its contribution reaches zero; intensity is
 * what it adds at the centre, in the same 0..1 units as sector light. r/g/b
 * tint the light: a fireball warms what it lights, plasma cools it. */
void r_light_add(float x, float y, float z, float radius, float intensity,
                 float r, float g, float b);

/* Added brightness at a world point, 0 when nothing reaches it.
 *
 * An inline, and the header carries the light array to make that possible,
 * because of where this sits: inside emit_fan's vertex loop and lit_shade's
 * per-corner path. As an out-of-line function it did two harms there: on
 * quiet frames (no lights -- most frames) every vertex still paid a call
 * whose body is a loop over nothing, and at the addresses the August build
 * linked at, that body sat exactly 16 KB from emit_fan's loop in the
 * direct-mapped I-cache, so each call evicted the caller's own lines and the
 * return refetched them -- the single hottest avoidable cost the audit found.
 * Inlined, the empty-list case folds to one load and a branch, and the
 * aliasing cannot exist because there is no jump.
 *
 * Falloff is 1 - d^2/r^2: cheap (no square root) and bounded, unlike an
 * inverse-square curve which would need clamping at the centre. This curve
 * is FLAT near the source and falls steeply near the edge -- at half the
 * radius it still gives 0.75, at 0.9r only 0.19. That is the opposite of
 * physical falloff, and it is the right choice here: it spends most of the
 * light budget filling the near field, which is the part the player can see
 * change. */
static inline float r_light_at(float x, float y, float z)
{
    float add = 0.0f;

    for (int i = 0; i < r_light_num; i++) {
        const r_light_t *l = &r_lights[i];
        const float dx = x - l->x;
        const float dy = y - l->y;
        const float dz = z - l->z;
        const float d2 = dx * dx + dy * dy + dz * dz;

        const float a = 1.0f - d2 * l->inv_r2;
        if (a > 0.0f) add += a * l->intensity;
    }

    return add;
}

/* The same query with the colour carried through, for surfaces that keep
 * three shade channels (flats and sprites do; walls approximate with a
 * per-quad tint). out[] receives the per-channel ADDITIONS -- all zero when
 * nothing reaches the point, so `shade + out[k]` degrades to exactly the
 * scalar path's result for white lights. */
static inline void r_light_at_rgb(float x, float y, float z, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;

    for (int i = 0; i < r_light_num; i++) {
        const r_light_t *l = &r_lights[i];
        const float dx = x - l->x;
        const float dy = y - l->y;
        const float dz = z - l->z;
        const float d2 = dx * dx + dy * dy + dz * dz;

        const float a = 1.0f - d2 * l->inv_r2;
        if (a > 0.0f) {
            out[0] += a * l->ir;
            out[1] += a * l->ig;
            out[2] += a * l->ib;
        }
    }
}

/* Chromaticity of the TOTAL light at a point, packed 0x00RRGGBB with the
 * max channel scaled to 255; 0x00FFFFFF means neutral (take the fast,
 * untinted path). `base` is the sector light, entering as a white
 * contributor, and (gadd, grgb) an optional emissive-glow contribution --
 * so a wall's hue is the blend of everything lighting it, in proportion,
 * and decays to pure white as base light dominates. Why a per-quad tint is
 * sound against the scalar corner shades: with the max-channel==1.0
 * convention the scalar add equals the coloured add's largest channel, so
 * shade * tint_c reproduces the flats' additive per-channel result at the
 * sample point. All-white lights accumulate identical summands per channel
 * and return exactly neutral. */
static inline uint32_t r_light_tint(float x, float y, float z, float base,
                                    float gadd, const float grgb[3])
{
    float ar = base, ag = base, ab = base;
    int hit = 0;

    if (gadd > 0.0f) {
        ar += gadd * grgb[0];
        ag += gadd * grgb[1];
        ab += gadd * grgb[2];
        hit = 1;
    }
    for (int i = 0; i < r_light_num; i++) {
        const r_light_t *l = &r_lights[i];
        const float dx = x - l->x;
        const float dy = y - l->y;
        const float dz = z - l->z;
        const float a = 1.0f - (dx * dx + dy * dy + dz * dz) * l->inv_r2;
        if (a > 0.0f) {
            ar += a * l->ir;
            ag += a * l->ig;
            ab += a * l->ib;
            hit = 1;
        }
    }
    if (!hit) return 0x00FFFFFFu;          /* no divide on the common miss */

    const float m = ar > ag ? (ar > ab ? ar : ab) : (ag > ab ? ag : ab);
    if (m <= 0.0f) return 0x00FFFFFFu;
    const float s = 255.0f / m;
    return ((uint32_t)(ar * s) << 16) |
           ((uint32_t)(ag * s) << 8)  |
            (uint32_t)(ab * s);
}

/* Can ANY light reach a horizontal surface at height z whose xy extent is
 * the given box? Closest-point test per light, strictly conservative: a
 * light this rejects contributes exactly zero to every point of the
 * surface (its falloff is non-positive at the box's nearest point, and
 * distance only grows from there), so skipping the per-vertex queries for
 * such a surface is output-identical. */
static inline int r_light_reaches_box(float bx0, float by0,
                                      float bx1, float by1, float z)
{
    for (int i = 0; i < r_light_num; i++) {
        const r_light_t *l = &r_lights[i];
        const float cx = l->x < bx0 ? bx0 : (l->x > bx1 ? bx1 : l->x);
        const float cy = l->y < by0 ? by0 : (l->y > by1 ? by1 : l->y);
        const float dx = cx - l->x;
        const float dy = cy - l->y;
        const float dz = z - l->z;
        if (1.0f - (dx * dx + dy * dy + dz * dz) * l->inv_r2 > 0.0f)
            return 1;
    }
    return 0;
}

/* For telemetry: how many lights survived culling this frame. */
static inline int r_light_count(void) { return r_light_num; }

#else

static inline void  r_light_reset(void) { }
static inline void  r_light_eye(float x, float y) { (void)x; (void)y; }
static inline void  r_light_halo_drop_next(float dz) { (void)dz; }
static inline void  r_light_halo_alpha_next(float a) { (void)a; }
static inline void  r_light_add(float x, float y, float z, float rad, float i,
                                float r, float g, float b)
                    { (void)x; (void)y; (void)z; (void)rad; (void)i;
                      (void)r; (void)g; (void)b; }
static inline float r_light_at(float x, float y, float z)
                    { (void)x; (void)y; (void)z; return 0.0f; }
static inline void  r_light_at_rgb(float x, float y, float z, float out[3])
                    { (void)x; (void)y; (void)z;
                      out[0] = out[1] = out[2] = 0.0f; }
static inline uint32_t r_light_tint(float x, float y, float z, float base,
                                    float gadd, const float grgb[3])
                    { (void)x; (void)y; (void)z; (void)base; (void)gadd;
                      (void)grgb; return 0x00FFFFFFu; }
static inline int   r_light_reaches_box(float bx0, float by0,
                                        float bx1, float by1, float z)
                    { (void)bx0; (void)by0; (void)bx1; (void)by1; (void)z;
                      return 0; }
static inline int   r_light_count(void) { return 0; }

#endif /* D_DYNLIGHT */

#endif /* R_LIGHT_H */
