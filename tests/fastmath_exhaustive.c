/* Exhaustive equivalence check for src/r_fastmath.c.
 *
 * Walks all 2^32 float bit patterns and compares the replacement against the
 * host libm bit-for-bit -- every finite value, every NaN payload, both zeroes,
 * both infinities. Not sampled: for a single-argument float function the input
 * space is small enough to enumerate, so there is no reason to argue about
 * coverage.
 *
 *   gcc -O2 -fno-builtin -o /tmp/fm tests/fastmath_exhaustive.c -lm && /tmp/fm
 *
 * -fno-builtin matters here too: without it the compiler constant-folds the
 * reference calls and the test compares the candidate against itself.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* The implementations under test, kept in sync with src/r_fastmath.c by
 * generation: this file is regenerated from it, never edited by hand. */
#include <stdint.h>

/* Union rather than memcpy: with -fno-builtin, memcpy is a real call. */
static inline uint32_t bits_of(float f)
{
    union { float f; uint32_t u; } c;
    c.f = f;
    return c.u;
}

#define EXP_MASK   0x7F800000u
/* Exponent at which a float's ULP reaches 1: everything at or above is
 * already integral, and both functions are the identity there. It is also the
 * bound that keeps the int32 conversion below inside its range. */
#define EXP_INTEGRAL ((127u + 23u) << 23)

/* NaN and infinity are returned as x + x, which is what newlib does and is not
 * merely cosmetic: adding a signalling NaN to itself QUIETS it, and returning
 * the input unchanged does not. The exhaustive check caught exactly that --
 * 8388606 sNaN payloads diverging, and nothing else. Infinity is unaffected by
 * the addition, so one expression covers both. */
static float cand_floorf(float x)
{
    const uint32_t e = bits_of(x) & EXP_MASK;

    if (e == EXP_MASK)     return x + x;   /* NaN (quieted) or infinity */
    if (e >= EXP_INTEGRAL) return x;       /* already integral */
    if (x == 0.0f)         return x;       /* preserve -0.0 */

    const int32_t i = (int32_t)x;          /* trunc.w.s: toward zero */
    const float   f = (float)i;
    return (x < f) ? (float)(i - 1) : f;
}

static float cand_ceilf(float x)
{
    const uint32_t e = bits_of(x) & EXP_MASK;

    if (e == EXP_MASK)     return x + x;
    if (e >= EXP_INTEGRAL) return x;
    if (x == 0.0f)         return x;

    const int32_t i = (int32_t)x;
    const float   f = (float)i;
    float r = (x > f) ? (float)(i + 1) : f;

    /* ceil of a negative fraction is negative zero, not positive zero. */
    if (r == 0.0f && x < 0.0f) r = -0.0f;
    return r;
}

int main(void)
{
    long long bad_floor = 0, bad_ceil = 0;
    long long shown = 0;

    uint32_t u = 0;
    do {
        float x;
        memcpy(&x, &u, 4);

        /* Compare RESULT BITS, not values: that catches -0.0 vs +0.0, which
         * == would call equal, and treats NaN payloads as data rather than
         * as a comparison that is false by definition. */
        float a = cand_floorf(x), b = floorf(x);
        uint32_t ua, ub;
        memcpy(&ua, &a, 4); memcpy(&ub, &b, 4);
        if (ua != ub) {
            if (shown++ < 8)
                printf("  floorf mismatch: x=%08x (%g) cand=%08x ref=%08x\n", u, (double)x, ua, ub);
            bad_floor++;
        }

        a = cand_ceilf(x); b = ceilf(x);
        memcpy(&ua, &a, 4); memcpy(&ub, &b, 4);
        if (ua != ub) {
            if (shown++ < 16)
                printf("  ceilf  mismatch: x=%08x (%g) cand=%08x ref=%08x\n", u, (double)x, ua, ub);
            bad_ceil++;
        }
    } while (++u != 0);

    printf("checked 2^32 patterns: floorf mismatches=%lld  ceilf mismatches=%lld\n",
           bad_floor, bad_ceil);
    return (bad_floor || bad_ceil) ? 1 : 0;
}
