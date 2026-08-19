#include "wad.h"

#include <libdragon.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

/* DFS rather than raw dma_read: PI DMA requires the RAM and ROM addresses to
 * share their low bit, and lump offsets are arbitrary. dfs_read falls back to
 * a CPU realign when needed, which is the right trade at load time -- this is
 * never on a frame path. */
/* The IWAD comes from one of two places.
 *
 * Baked into the ROM it is simple but it makes the cartridge image contain
 * id Software's game data, which cannot be handed to anyone. Read from the
 * flashcart's SD card instead and the ROM carries only code and the art
 * derived at build time, and the player supplies their own WAD -- which is
 * also how they swap one. Paths beginning "sd:/" take the card; anything
 * else is a path inside the ROM filesystem.
 *
 * Mods stack on top. Each lump remembers which WAD it came from, because a
 * merged directory is the only thing the layer above understands: Doom's
 * whole PWAD mechanism is "same name, later entry wins", and w_n64.c gets
 * that for free from a single array searched backwards. */
typedef struct {
    int   fd;      /* ROM filesystem handle, -1 when unused */
    FILE *fp;      /* SD card handle, NULL when unused */
} wad_src_t;

static wad_src_t   srcs[WAD_MAX_SRC];
static int         numsrcs;
static wad_lump_t *directory;
static int         numlumps;

static bool read_at(int src, uint32_t offset, void *dst, uint32_t len)
{
    if (src < 0 || src >= numsrcs) return false;

    if (srcs[src].fp) {
        if (fseek(srcs[src].fp, (long)offset, SEEK_SET) != 0) return false;
        return fread(dst, 1, len, srcs[src].fp) == len;
    }
    if (srcs[src].fd < 0) return false;
    if (dfs_seek(srcs[src].fd, (int)offset, SEEK_SET) != 0) return false;
    return dfs_read(dst, 1, (int)len, srcs[src].fd) == (int)len;
}

void wad_reset(void)
{
    for (int i = 0; i < numsrcs; i++)
        if (srcs[i].fp) fclose(srcs[i].fp);
    /* DFS handles are not closed: dfs_open has no counterpart in this
     * libdragon, and a ROM-resident WAD is opened once per boot anyway. */
    memset(srcs, 0, sizeof srcs);
    numsrcs = 0;

    free(directory);
    directory = NULL;
    numlumps  = 0;
}

bool wad_add(const char *path)
{
    if (numsrcs >= WAD_MAX_SRC) {
        debugf("wad: no room for '%s', %d already open\n", path, numsrcs);
        return false;
    }

    wad_src_t s = { .fd = -1, .fp = NULL };

    if (!strncmp(path, "sd:/", 4)) {
        /* Nothing about this card is reliable first time -- the music
         * streamer retries everything for the same reason -- so the open
         * gets a few attempts before it counts as absent. */
        for (int attempt = 0; attempt < 4 && !s.fp; attempt++) {
            s.fp = fopen(path, "rb");
            if (!s.fp) wait_ms(50);
        }
        if (!s.fp) {
            debugf("wad: cannot open '%s' on the card\n", path);
            return false;
        }
    } else {
        s.fd = dfs_open(path);
        if (s.fd < 0) {
            debugf("wad: cannot open '%s' (%d)\n", path, s.fd);
            return false;
        }
    }

    /* Published before the first read_at, which resolves through the table. */
    const int me = numsrcs;
    srcs[me] = s;
    numsrcs++;

    uint8_t header[12];
    if (!read_at(me, 0, header, sizeof header)) goto fail;

    if (memcmp(header, "IWAD", 4) != 0 && memcmp(header, "PWAD", 4) != 0) {
        debugf("wad: '%s' is not a WAD (magic %.4s)\n", path, (const char *)header);
        goto fail;
    }

    const int      added  = wad_le32(header + 4);
    const uint32_t dirofs = (uint32_t)wad_le32(header + 8);

    if (added <= 0 || added > 65536) {
        debugf("wad: implausible lump count %d -- byte order wrong?\n", added);
        goto fail;
    }

    /* The raw directory is read in bulk and then decoded in place into the
     * host-order form, so there is one PI transfer rather than one per lump. */
    const size_t rawsize = (size_t)added * 16;
    uint8_t *raw = malloc(rawsize);

    /* Grown rather than replaced: the lumps already stacked keep their
     * indices, which is what makes an index handed out earlier stay valid. */
    wad_lump_t *grown = realloc(directory,
                                (size_t)(numlumps + added) * sizeof *directory);
    if (!raw || !grown) {
        debugf("wad: out of memory for %d-lump directory\n", numlumps + added);
        free(raw);
        if (grown) directory = grown;
        goto fail;
    }
    directory = grown;

    if (!read_at(me, dirofs, raw, rawsize)) {
        debugf("wad: short read of directory at %lu\n", (unsigned long)dirofs);
        free(raw);
        goto fail;
    }

    for (int i = 0; i < added; i++) {
        const uint8_t *e = raw + (size_t)i * 16;
        wad_lump_t    *d = &directory[numlumps + i];
        d->offset = (uint32_t)wad_le32(e);
        d->size   = (uint32_t)wad_le32(e + 4);
        memcpy(d->name, e + 8, WAD_LUMP_NAME_LEN);
        d->name[WAD_LUMP_NAME_LEN] = '\0';
        d->src    = (uint8_t)me;
    }
    free(raw);
    numlumps += added;

    debugf("wad: %s %s, %d lumps (%d total, %u KB resident)\n",
           (const char *)header, path, added, numlumps,
           (unsigned)((numlumps * sizeof *directory) / 1024));
    return true;

fail:
    if (srcs[me].fp) fclose(srcs[me].fp);
    srcs[me] = (wad_src_t){ .fd = -1, .fp = NULL };
    numsrcs--;
    return false;
}

bool wad_open(const char *path)
{
    wad_reset();
    return wad_add(path);
}

int wad_num_lumps(void)   { return numlumps; }
int wad_num_sources(void) { return numsrcs; }

const wad_lump_t *wad_lump(int index)
{
    if (index < 0 || index >= numlumps) return NULL;
    return &directory[index];
}

/* Doom pads lump names with NUL only when shorter than 8, and compares
 * case-insensitively. */
static bool name_eq(const char *lump, const char *want)
{
    for (int i = 0; i < WAD_LUMP_NAME_LEN; i++) {
        char a = lump[i], b = want[i] ? want[i] : '\0';
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return false;
        if (!b) return true;
    }
    return true;
}

int wad_find(const char *name)
{
    for (int i = numlumps - 1; i >= 0; i--)
        if (name_eq(directory[i].name, name)) return i;
    return -1;
}

int wad_find_from(const char *name, int from)
{
    if (from < 0) from = 0;
    for (int i = from; i < numlumps; i++)
        if (name_eq(directory[i].name, name)) return i;
    return -1;
}

bool wad_read(int index, void *dst)
{
    const wad_lump_t *l = wad_lump(index);
    if (!l) return false;
    return read_at(l->src, l->offset, dst, l->size);
}

bool wad_read_at(int index, uint32_t offset, void *dst, uint32_t len)
{
    const wad_lump_t *l = wad_lump(index);
    if (!l || offset + len > l->size) return false;
    return read_at(l->src, l->offset + offset, dst, len);
}
