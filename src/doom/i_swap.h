/*
 * i_swap -- endian conversion for on-disk data.
 *
 * Chocolate Doom delegates this to SDL. There is no SDL here, and the
 * substitution is not cosmetic: the N64 is big-endian while every WAD is
 * little-endian, so these macros actually do work on this target where on x86
 * they compile away to nothing. Getting them wrong does not fail to build --
 * it loads a level whose vertices are scattered to the far corners of the
 * coordinate space.
 */
#ifndef __I_SWAP__
#define __I_SWAP__

#include <stdint.h>

static inline int16_t i_swap16(int16_t x)
{
    const uint16_t u = (uint16_t)x;
    return (int16_t)(((u & 0x00FFu) << 8) | ((u & 0xFF00u) >> 8));
}

static inline int32_t i_swap32(int32_t x)
{
    const uint32_t u = (uint32_t)x;
    return (int32_t)(((u & 0x000000FFu) << 24) | ((u & 0x0000FF00u) <<  8) |
                     ((u & 0x00FF0000u) >>  8) | ((u & 0xFF000000u) >> 24));
}

/* Doom's names. Both directions are the same operation on a fixed-endian
 * target, so the LE and BE forms differ only in which one is a no-op. */
#define SHORT(x)    i_swap16(x)
#define LONG(x)     i_swap32(x)
#define SHORTLE(x)  i_swap16(x)
#define LONGLE(x)   i_swap32(x)
#define SHORTBE(x)  (x)
#define LONGBE(x)   (x)

#endif
