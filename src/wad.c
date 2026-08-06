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
 * else is a path inside the ROM filesystem. */
static int         wad_fd = -1;      /* ROM filesystem */
static FILE       *wad_fp;           /* SD card */
static wad_lump_t *directory;
static int         numlumps;

static bool read_at(uint32_t offset, void *dst, uint32_t len)
{
    if (wad_fp) {
        if (fseek(wad_fp, (long)offset, SEEK_SET) != 0) return false;
        return fread(dst, 1, len, wad_fp) == len;
    }
    if (wad_fd < 0) return false;
    if (dfs_seek(wad_fd, (int)offset, SEEK_SET) != 0) return false;
    return dfs_read(dst, 1, (int)len, wad_fd) == (int)len;
}

bool wad_open(const char *path)
{
    wad_fd = -1;
    wad_fp = NULL;

    if (!strncmp(path, "sd:/", 4)) {
        /* Nothing about this card is reliable first time -- the music
         * streamer retries everything for the same reason -- so the open
         * gets a few attempts before it counts as absent. */
        for (int attempt = 0; attempt < 4 && !wad_fp; attempt++) {
            wad_fp = fopen(path, "rb");
            if (!wad_fp) wait_ms(50);
        }
        if (!wad_fp) {
            debugf("wad: cannot open '%s' on the card\n", path);
            return false;
        }
    } else {
        wad_fd = dfs_open(path);
        if (wad_fd < 0) {
            debugf("wad: cannot open '%s' (%d)\n", path, wad_fd);
            return false;
        }
    }

    uint8_t header[12];
    if (!read_at(0, header, sizeof header)) return false;

    if (memcmp(header, "IWAD", 4) != 0 && memcmp(header, "PWAD", 4) != 0) {
        debugf("wad: '%s' is not a WAD (magic %.4s)\n", path, (const char *)header);
        if (wad_fp) { fclose(wad_fp); wad_fp = NULL; }
        return false;
    }

    numlumps = wad_le32(header + 4);
    const uint32_t dirofs = (uint32_t)wad_le32(header + 8);

    if (numlumps <= 0 || numlumps > 65536) {
        debugf("wad: implausible lump count %d -- byte order wrong?\n", numlumps);
        return false;
    }

    /* The raw directory is read in bulk and then decoded in place into the
     * host-order form, so there is one PI transfer rather than one per lump. */
    const size_t rawsize = (size_t)numlumps * 16;
    uint8_t *raw = malloc(rawsize);
    directory = malloc((size_t)numlumps * sizeof *directory);
    if (!raw || !directory) {
        debugf("wad: out of memory for %d-lump directory\n", numlumps);
        free(raw);
        return false;
    }

    if (!read_at(dirofs, raw, rawsize)) {
        debugf("wad: short read of directory at %lu\n", (unsigned long)dirofs);
        free(raw);
        return false;
    }

    for (int i = 0; i < numlumps; i++) {
        const uint8_t *e = raw + (size_t)i * 16;
        directory[i].offset = (uint32_t)wad_le32(e);
        directory[i].size   = (uint32_t)wad_le32(e + 4);
        memcpy(directory[i].name, e + 8, WAD_LUMP_NAME_LEN);
        directory[i].name[WAD_LUMP_NAME_LEN] = '\0';
    }
    free(raw);

    debugf("wad: %s, %d lumps, directory at 0x%lx (%u KB resident)\n",
           (const char *)header, numlumps, (unsigned long)dirofs,
           (unsigned)((numlumps * sizeof *directory) / 1024));
    return true;
}

int wad_num_lumps(void) { return numlumps; }

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
    return read_at(l->offset, dst, l->size);
}

bool wad_read_at(int index, uint32_t offset, void *dst, uint32_t len)
{
    const wad_lump_t *l = wad_lump(index);
    if (!l || offset + len > l->size) return false;
    return read_at(l->offset + offset, dst, len);
}
