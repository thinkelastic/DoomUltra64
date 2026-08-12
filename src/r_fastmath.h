/* Inline floor/ceil for the render paths.
 *
 * floorf/ceilf are libm CALLS on this toolchain: no GCC pattern lowers them
 * to the VR4300's floor.w.s/ceil.w.s (and n64.mk's -ftrapping-math pins
 * that). But the instructions exist, and libdragon's fmath.h wraps them in
 * inline asm -- so on N64 these helpers are one FPU op plus a register move,
 * cheaper even than truncate-and-correct. (Named rf_, because libdragon
 * already owns the fm_ prefix in <fmath.h>.)
 *
 * On the host (the mock-RDP render test) the same values come from
 * truncate-and-correct, which equals floorf/ceilf for every finite input in
 * int32 range: above 2^23 a float's ULP is >= 1 so the value is already
 * integral and the correction is correctly false; below 2^23 the integer
 * fits the 24-bit significand and converts back exactly. So both builds
 * produce bit-identical integers over the reachable domain.
 *
 * Callers must keep inputs inside int32 range, exactly as with the
 * trunc.w.s casts already throughout the renderer; floor.w.s traps the same
 * way on overflow rather than silently wrapping.
 *
 * R_FASTFLOOR=0 restores the libm calls, same contract as r_tri.c.
 */
#ifndef R_FASTMATH_RENDER_H
#define R_FASTMATH_RENDER_H

#include <math.h>
#include <stdint.h>

#ifndef R_FASTFLOOR
#define R_FASTFLOOR 1
#endif

#if R_FASTFLOOR && defined(N64)

static inline int32_t rf_floor_i32(float f)
{
    float   y;
    int32_t r;
    __asm ("floor.w.s %0,%1" : "=f"(y) : "f"(f));
    __asm ("mfc1 %0,%1"      : "=r"(r) : "f"(y));
    return r;
}

static inline int32_t rf_ceil_i32(float f)
{
    float   y;
    int32_t r;
    __asm ("ceil.w.s %0,%1" : "=f"(y) : "f"(f));
    __asm ("mfc1 %0,%1"     : "=r"(r) : "f"(y));
    return r;
}

static inline float rf_floorf(float f)
{
    float yint, y;
    __asm ("floor.w.s %0,%1" : "=f"(yint) : "f"(f));
    __asm ("cvt.s.w %0,%1"   : "=f"(y) : "f"(yint));
    return y;
}

#elif R_FASTFLOOR

/* Host build: same integers via truncate-and-correct (see header comment). */
static inline int32_t rf_floor_i32(float f)
{
    const int32_t i = (int32_t)f;
    return i - (f < (float)i);
}

static inline int32_t rf_ceil_i32(float f)
{
    const int32_t i = (int32_t)f;
    return i + (f > (float)i);
}

static inline float rf_floorf(float f)
{
    return (float)rf_floor_i32(f);
}

#else

static inline int32_t rf_floor_i32(float f) { return (int32_t)floorf(f); }
static inline int32_t rf_ceil_i32(float f)  { return (int32_t)ceilf(f); }
static inline float   rf_floorf(float f)    { return floorf(f); }

#endif /* R_FASTFLOOR */

#endif /* R_FASTMATH_RENDER_H */
