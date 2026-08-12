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

void r_light_add(float x, float y, float z, float radius, float intensity,
                 float r, float g, float b)
{
    if (radius <= 0.0f || intensity <= 0.0f) return;

    if (r_light_num == R_LIGHT_MAX) {
        /* Full: keep the brighter set rather than whichever arrived first.
         * Peak intensity is the proxy -- the caller has already scaled it by
         * distance to the eye, so a dim distant flash loses to a near one. */
        int weakest = 0;
        for (int i = 1; i < R_LIGHT_MAX; i++)
            if (r_lights[i].intensity < r_lights[weakest].intensity) weakest = i;
        if (r_lights[weakest].intensity >= intensity) return;
        r_light_num = weakest;         /* overwrite it, restored below */
    }

    r_light_t *l = &r_lights[r_light_num];
    l->x = x; l->y = y; l->z = z;
    l->inv_r2    = 1.0f / (radius * radius);
    l->intensity = intensity;
    /* Premultiplied; callers keep the max tint channel at 1.0 so intensity
     * remains the max channel (see the header's convention note). */
    l->ir = intensity * r;
    l->ig = intensity * g;
    l->ib = intensity * b;

    if (r_light_num < R_LIGHT_MAX) r_light_num++;
    else                           r_light_num = R_LIGHT_MAX;
}

#endif /* D_DYNLIGHT */
