/* Dynamic point lights. See r_light.h for what this is and what it is not.
 *
 * Only the writers live here; the query is an inline in the header (with the
 * array exported to make that possible) -- see the note on r_light_at. */

#include "r_light.h"

#if D_DYNLIGHT

_Alignas(16) r_light_t r_lights[R_LIGHT_MAX];
int       r_light_num;

void r_light_reset(void)
{
    r_light_num = 0;
}

float r_light_halo[R_LIGHT_MAX];
float r_light_prio[R_LIGHT_MAX];
float r_light_halo_dz[R_LIGHT_MAX];
static float halo_next = 1.0f;
static float halo_dz_next;

void r_light_halo_next(float frac) { halo_next = frac; }
void r_light_halo_drop_next(float dz) { halo_dz_next = dz; }

/* Where the eye is, for ranking slots. Set once a frame before the walk. */
static float eye_x, eye_y;

void r_light_eye(float x, float y) { eye_x = x; eye_y = y; }

void r_light_add(float x, float y, float z, float radius, float intensity,
                 float r, float g, float b)
{
    /* Taken and cleared at entry, so a rejected light cannot leave its
     * scale latched for whoever comes next. */
    const float halo = halo_next;
    const float halo_dz = halo_dz_next;
    halo_next = 1.0f;
    halo_dz_next = 0.0f;

    if (radius <= 0.0f || intensity <= 0.0f) return;

    /* What a slot is worth keeping FOR, which is not the same as how
     * bright it is at its own centre: a torch across the hall and a torch
     * at your elbow register identically, and only one of them is worth a
     * slot. Inverse-square once the eye is outside the light's reach,
     * flat inside it -- so anything actually lighting you ranks by its own
     * brightness, and everything beyond falls away with distance. */
    const float ex = x - eye_x, ey = y - eye_y;
    const float d2 = ex * ex + ey * ey, r2 = radius * radius;
    const float prio = d2 <= r2 ? intensity : intensity * r2 / d2;

    int slot;
    if (r_light_num < R_LIGHT_MAX) {
        slot = r_light_num++;
    } else {
        /* Full: keep the set worth the most, not whichever arrived first.
         *
         * The count must NOT be rewound to the evicted slot to do this.
         * It used to be, and since every query loops i < r_light_num, the
         * first eviction in a frame silently dropped every light above
         * the evicted index out of the render AND let the next arrival
         * overwrite a live slot without any comparison at all. Nothing
         * showed while the registry never filled -- lights were transient,
         * a flash here and a fireball there. The standing lamps and
         * torches are the first that are simply always on, so a lit room
         * now fills all eight slots and holds them. */
        int weakest = 0;
        for (int i = 1; i < R_LIGHT_MAX; i++)
            if (r_light_prio[i] < r_light_prio[weakest]) weakest = i;
        if (r_light_prio[weakest] >= prio) return;
        slot = weakest;
    }

    r_light_t *l = &r_lights[slot];
    l->x = x; l->y = y; l->z = z;
    l->inv_r2    = 1.0f / r2;
    l->intensity = intensity;
    r_light_halo[slot] = halo;
    r_light_halo_dz[slot] = halo_dz;
    r_light_prio[slot] = prio;
    /* Premultiplied; callers keep the max tint channel at 1.0 so intensity
     * remains the max channel (see the header's convention note). */
    l->ir = intensity * r;
    l->ig = intensity * g;
    l->ib = intensity * b;
}

#endif /* D_DYNLIGHT */
