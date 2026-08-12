/* floorf and ceilf, as instructions rather than calls.
 *
 * On this toolchain both are libm CALLS: GCC's MIPS backend has no pattern
 * lowering them to the VR4300's floor.w.s, and libdragon's n64.mk sets
 * -ftrapping-math, so -fno-trapping-math does not unlock one either. Newlib's
 * floorf is 68 instructions reached through a jal, around bit manipulation
 * that the FPU can do in one.
 *
 * This file replaces them for the whole link rather than at each call site.
 * The project's objects are listed explicitly and so are resolved before
 * -lm is searched, which means the archive members are never pulled in and
 * every caller rebinds -- including libdragon's own rdpq, whose triangle
 * setup pays two of these per vertex and which no edit of ours could reach
 * otherwise. The Makefile already relies on this exact linker behaviour for
 * the patched flashcart driver; see the comment above the src list.
 *
 * MUST be compiled -fno-builtin. Without it GCC recognises the body as floorf
 * and replaces it with a call to floorf, which is this function: it compiles
 * cleanly and recurses until the stack is gone.
 *
 * Verified exhaustively against newlib over all 2^32 float bit patterns --
 * every value, every NaN payload, both zeroes -- not sampled. See
 * tests/fastmath_exhaustive.c.
 */

#include <stdint.h>
#include <math.h>

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
float floorf(float x)
{
    const uint32_t e = bits_of(x) & EXP_MASK;

    if (e == EXP_MASK)     return x + x;   /* NaN (quieted) or infinity */
    if (e >= EXP_INTEGRAL) return x;       /* already integral */
    if (x == 0.0f)         return x;       /* preserve -0.0 */

    const int32_t i = (int32_t)x;          /* trunc.w.s: toward zero */
    const float   f = (float)i;
    return (x < f) ? (float)(i - 1) : f;
}

float ceilf(float x)
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
