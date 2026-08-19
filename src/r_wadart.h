/*
 * r_wadart -- Doom's own art, composed on the console instead of in the build.
 *
 * The cartridge used to carry every texture, flat and sprite pre-composed by
 * tools/wad2n64.c, which is what tied a ROM to the one IWAD it was built
 * against. This does the same work at level load, so a ROM plays whatever WAD
 * the card offers -- Doom, Doom II, Freedoom, a total conversion -- and a
 * mod's own art appears without anyone rebuilding anything.
 *
 * It is affordable for one reason: Doom's art is ALREADY 8-bit palettised, so
 * none of this is colour work. It is layout -- patch columns and posts into a
 * flat canvas, then into the 64x32 tiles TMEM wants. Vanilla Doom composes
 * its textures at runtime on a 486 for the same reason.
 *
 * Every routine fills a dt64_tex_t exactly as dt64_load would, out of the
 * same texture arena, so nothing downstream can tell the difference. That is
 * also how it is tested: compose with the baked files still present and
 * compare the bytes (R_WADART_VERIFY).
 */
#ifndef R_WADART_H
#define R_WADART_H

#include "dt64.h"
#include <stdbool.h>

/* Read PNAMES and TEXTURE1/TEXTURE2 into a lookup. Call once after the WAD
 * stack is built and before any texture is asked for; calling it again after
 * a mod switch is how the new stack's textures become visible. */
void r_wadart_init(void);

/* Doom's four art namespaces. Each fills `t` and returns false if the WAD
 * has no such thing -- which is not an error: sprite lookups probe names
 * that often do not exist, exactly as the baked path did.
 *
 * `level` selects a mip: 0 is full size, 1 half, 2 quarter. Walls carry two
 * levels and sprites one, matching what the converter used to emit. */
bool r_wadart_texture(dt64_tex_t *t, const char *name, int level);
bool r_wadart_flat(dt64_tex_t *t, const char *name);
bool r_wadart_sprite(dt64_tex_t *t, const char *name, int level);
bool r_wadart_ui(dt64_tex_t *t, const char *name);

/* How many textures TEXTURE1/TEXTURE2 declare, and the i'th name, in the
 * order the WAD lists them. Doom's animations rely on that order and the
 * converter used to hand it over in texorder.bin. */
int         r_wadart_numtextures(void);
const char *r_wadart_texname(int i);

/* Flat names in F_START/F_END order, for the same reason. */
int         r_wadart_numflats(void);
const char *r_wadart_flatname(int i);

/* After a false return: nonzero if the WAD genuinely has no such lump, zero
 * if composition merely failed and may yet succeed. Mirrors dt64_last_absent. */
extern int r_wadart_absent;

/* Compose what the build already baked and compare every byte. Counts land
 * in these; a mismatch names the texture and the offset. */
extern int r_wadart_verify_ok, r_wadart_verify_bad;
void r_wadart_verify(const char *prefix, const char *name,
                     const dt64_tex_t *baked);

#endif /* R_WADART_H */
