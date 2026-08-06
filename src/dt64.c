#include "dt64.h"

#include "mem.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

/* The TLUT lives in RAM for the lifetime of the program: Doom's palette
 * effects (damage flash, item pickup, invulnerability) are implemented by
 * re-uploading a different 512-byte palette, not by touching texture data. */
static uint16_t tlut[256] __attribute__((aligned(16)));

/* Read an entire DFS file, allocating from `arena` -- or from the system heap
 * when `arena` is DT64_HEAP, for short-lived reads the caller will free.
 *
 * dfs_read is PI DMA straight off the cartridge, so this is the mechanism the
 * whole asset pipeline rests on: ROM is the disk, RAM is the cache. Assets are
 * read once at load time, never during a frame -- the PI bus manages about
 * 5 MB/s, which is fine for a level transition and ruinous inside one. */
#define DT64_HEAP ((mem_arena_id_t)-1)

/* Why the last load failed. A missing file is a permanent fact and worth
 * remembering; a read that failed is not. The cartridge read path is not
 * perfectly reliable -- the music streamer carries retry logic for the same
 * reason -- and an arena can be full at one moment and not the next. If
 * those are recorded as "this texture does not exist", the sprite vanishes
 * for the rest of the level, which is what "sometimes sprites disappear"
 * looked like on hardware and never reproduced in the emulator. */
int dt64_last_absent;

static void *read_whole_file(const char *path, mem_arena_id_t arena, int *size_out)
{
    dt64_last_absent = 0;

    int fd = dfs_open(path);
    if (fd < 0) { dt64_last_absent = 1; return NULL; }

    int size = dfs_size(fd);
    if (size <= 0) { dfs_close(fd); dt64_last_absent = 1; return NULL; }

    /* 16-byte alignment keeps the RSP/RDP DMA paths happy and avoids a bounce
     * buffer when rdpq uploads out of this memory. */
    void *buf = (arena == DT64_HEAP) ? memalign(16, size)
                                     : mem_alloc(arena, (size_t)size);
    if (!buf) { dfs_close(fd); return NULL; }

    if (dfs_read(buf, 1, size, fd) != size) {
        if (arena == DT64_HEAP) free(buf);
        dfs_close(fd);
        return NULL;
    }
    dfs_close(fd);

    /* Push the data out to RDRAM before anything else looks at it.
     *
     * The RDP reads RDRAM directly and does not participate in the VR4300's
     * cache coherency, so any bytes still sitting dirty in the data cache are
     * simply invisible to it -- and a DMA that landed in RDRAM can later be
     * clobbered by a stale cache line writing back over it. Either way the
     * symptom is the same and easy to misread: geometry and palette look
     * correct, but the texture renders as scattered fragments of real data on
     * a background of whatever RDRAM happened to hold. */
    data_cache_hit_writeback_invalidate(buf, size);

    if (size_out) *size_out = size;
    return buf;
}

static uint16_t *tlut_banks;     /* all palettes, 256 entries each */
static int       tlut_nbanks = 1;
static int       tlut_bank   = 0;

void dt64_load_tlut(const char *path)
{
    int size = 0;
    uint16_t *data = read_whole_file(path, DT64_HEAP, &size);
    assertf(data, "cannot open TLUT '%s'", path);
    assertf(size >= 512 && (size % 512) == 0,
            "TLUT '%s' is %d bytes, expected a multiple of 512", path, size);

    tlut_banks  = data;              /* big-endian from the converter */
    tlut_nbanks = size / 512;
    tlut_bank   = 0;
    memcpy(tlut, data, 512);
}

/* Select which PLAYPAL palette the next bind uploads: 0 normal, 1-8 damage
 * reds, 9-12 pickup golds, 13 the radiation-suit green. (Invulnerability's
 * white-out is a colormap effect, not a palette, and is not covered here.)
 * st_stuff picks the bank; this just latches it for the next bind. */
void dt64_set_palette(int bank)
{
    if (!tlut_banks || bank < 0 || bank >= tlut_nbanks) bank = 0;
    if (bank == tlut_bank) return;
    tlut_bank = bank;
    memcpy(tlut, tlut_banks + bank * 256, 512);
}

/* The live palette entry for an index -- whatever bank is currently
 * latched, flashes included. RGBA5551 as uploaded. */
uint16_t dt64_tlut_color(int idx)
{
    return tlut[idx & 255];
}

void dt64_bind_tlut(void)
{
    /* Flushed here rather than at load time so that palette effects, which
     * rewrite this array between frames, stay correct without each having to
     * remember the cache. 512 bytes per frame is not worth optimising. */
    data_cache_hit_writeback_invalidate(tlut, sizeof tlut);
    rdpq_tex_upload_tlut(tlut, 0, 256);
}

/* Read a whole file into the texture arena. Exposed for the animation
 * table, which needs the converter's name ordering and has nowhere else
 * to get a file from. */
void *read_whole_file_public(const char *path, int *size_out)
{
    return read_whole_file(path, MEM_ARENA_TEXTURE, size_out);
}

bool dt64_load(dt64_tex_t *tex, const char *path)
{
    int size = 0;
    /* Textures live in the per-level arena: their lifetime is exactly one
     * level, so they are dropped wholesale at the next transition rather than
     * freed individually. */
    uint8_t *raw = read_whole_file(path, MEM_ARENA_TEXTURE, &size);
    if (!raw) return false;

    if (size < DT64_HEADER_SIZE || memcmp(raw, "DT64", 4) != 0) return false;

    uint16_t version = (uint16_t)((raw[4] << 8) | raw[5]);
    if (version != 3) return false;

    tex->width  = (uint16_t)((raw[6]  << 8) | raw[7]);
    tex->height = (uint16_t)((raw[8]  << 8) | raw[9]);
    tex->flags  = (uint16_t)((raw[10] << 8) | raw[11]);
    tex->leftoffset = (int16_t)((raw[12] << 8) | raw[13]);
    tex->topoffset  = (int16_t)((raw[14] << 8) | raw[15]);

    if ((int)(DT64_HEADER_SIZE + tex->width * tex->height) > size) return false;

    /* Point at the texel payload in place -- no second copy of a 16 KB
     * texture. The 16-byte header keeps this pointer 16-aligned, which matters
     * because the RDP reads texture data directly from here and demands
     * 8-byte alignment. */
    tex->texels = raw + DT64_HEADER_SIZE;
    assertf(((uintptr_t)tex->texels & 7) == 0,
            "texels for '%s' are misaligned (%p)", path, tex->texels);
    tex->surface = surface_make_linear(tex->texels, FMT_CI8, tex->width, tex->height);
    tex->tiles_x = (uint8_t)((tex->width  + DT64_TILE_W - 1) / DT64_TILE_W);
    tex->tiles_y = (uint8_t)((tex->height + DT64_TILE_H - 1) / DT64_TILE_H);
    tex->mip = NULL;
    return true;
}

void dt64_release_all(void)
{
    /* Textures are never freed individually. Doom's asset lifetime is exactly
     * one level, so the arena is reset wholesale at a transition -- which also
     * means the allocator can stay a bump pointer and never fragment. */
    mem_reset(MEM_ARENA_TEXTURE);
}
