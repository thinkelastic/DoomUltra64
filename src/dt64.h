/*
 * dt64 -- runtime loader for build-time-composed Doom textures.
 *
 * See tools/wad2n64.c for the producer side and the on-disk layout.
 */
#ifndef DT64_H
#define DT64_H

#include <libdragon.h>
#include <stdint.h>

/* The largest CI8 tile that fits TMEM.
 *
 * TMEM is 4 KB total. When sampling through a TLUT the palette occupies the
 * upper half (2 KB), leaving 2048 bytes for texels. At one byte per CI8 texel
 * that is exactly 64x32. Every wall is drawn as a grid of tiles this size,
 * which is the central constraint the whole renderer is built around. */
#define DT64_TILE_W 64
#define DT64_TILE_H 32

/* Padded to 16 bytes so texels stay 8-byte aligned for the RDP when the file
 * is kept as a single allocation. See tools/wad2n64.c for the full layout. */
#define DT64_HEADER_SIZE 16

/* flags bits */
#define DT64_FLAG_MASKED    1
#define DT64_FLAG_TILEMAJOR 2   /* texels stored per 64x32 tile, contiguous */

typedef struct dt64_tex_s {
    uint16_t   width;
    uint16_t   height;
    uint16_t   flags;        /* bit 0: masked */
    int16_t    leftoffset;   /* sprite anchor: horizontal centre */
    int16_t    topoffset;    /* sprite anchor: distance down to the feet */
    uint8_t   *texels;       /* CI8 indices into the shared TLUT */
    surface_t  surface;      /* wraps texels for rdpq_tex_upload_sub */
    uint8_t    tiles_x;      /* width  in TMEM tiles */
    uint8_t    tiles_y;      /* height in TMEM tiles */

    /* Half-resolution level, or NULL. Selected when a surface is minified:
     * a 128x128 texture costs eight TMEM tiles and eight quads, its mip two. */
    struct dt64_tex_s *mip;
} dt64_tex_t;

/* Load the shared 256-entry PLAYPAL TLUT into RAM. */
void dt64_load_tlut(const char *path);

/* Rebuild every palette bank from a runtime PLAYPAL lump (14 x 768 bytes of
 * RGB): EXTWAD players can swap IWADs, and flash palettes belong to the WAD
 * actually playing. Bit-identical to the baked banks for identical bytes. */
void dt64_build_tluts(const uint8_t *playpal, int nbytes);

/* Upload the current palette into the upper half of TMEM. Call once per frame,
 * before drawing: it is a single 512-byte command, and re-binding per frame is
 * what makes Doom's palette effects (damage red, item pickup, invulnerability)
 * a palette swap rather than a re-upload of every texture. */
void dt64_bind_tlut(void);

/* Select which PLAYPAL palette the next bind uploads (0 = normal). */
void dt64_set_palette(int bank);
int  dt64_palette(void);      /* the bank currently selected */

/* Live palette entry (current bank), RGBA5551. */
uint16_t dt64_tlut_color(int idx);

/* Load a .dt64 texture from the ROM filesystem into the texture arena.
 * Returns false on failure, including when the arena is exhausted. */
bool dt64_load(dt64_tex_t *tex, const char *path);

/* After a failed dt64_load: nonzero if the file is genuinely absent, zero
 * if the load merely failed (bad read, arena full) and may yet succeed. */
extern int dt64_last_absent;

/* Read a whole file into the texture arena (NULL on failure). */
void *read_whole_file_public(const char *path, int *size_out);

/* Drop every loaded texture at once. Call at a level transition; individual
 * textures are never freed, since their lifetime is exactly one level. */
void dt64_release_all(void);

/* Upload one TMEM tile of `tex` covering the texel rect [s0,t0)..(s1,t1),
 * hiding the storage layout. Tile-major textures hand the RDP one contiguous
 * block whose stride equals its width, which is the condition for the fast
 * LOAD_BLOCK path -- one DMA burst instead of a strided read per row. The
 * tile descriptor is translated so callers keep absolute texture
 * coordinates either way. The rect must lie on the DT64 tile grid, which is
 * how every emitter already iterates. */
static inline int dt64_upload_tile(rdpq_tile_t tile, const dt64_tex_t *t,
                                   const rdpq_texparms_t *parms,
                                   int s0, int t0, int s1, int t1)
{
    if (t->flags & DT64_FLAG_TILEMAJOR) {
        const int ty = t0 / DT64_TILE_H;
        const int tx = s0 / DT64_TILE_W;
        const int tw = s1 - s0, th = t1 - t0;
        surface_t ts = surface_make_linear(
            t->texels + (size_t)t->width * (size_t)(ty * DT64_TILE_H)
                      + (size_t)tx * DT64_TILE_W * (size_t)th,
            FMT_CI8, (uint16_t)tw, (uint16_t)th);

        rdpq_texparms_t p;
        if (parms) p = *parms;
        else       p = (rdpq_texparms_t){ 0 };
        p.s.translate += (float)s0;
        p.t.translate += (float)t0;
        return rdpq_tex_upload(tile, &ts, &p);
    }
    return rdpq_tex_upload_sub(tile, &t->surface, parms, s0, t0, s1, t1);
}

#endif /* DT64_H */
