/*
 * wad -- lump access for a WAD living in cartridge ROM.
 *
 * The IWAD is 14 MB against 8 MB of RDRAM, so it is never resident. It is
 * appended to the ROM and lumps are pulled over the PI bus on demand, which
 * makes this the N64 equivalent of Chocolate Doom's wad_file_class_t: the
 * three operations it abstracts (open, close, read-at-offset) map exactly onto
 * what a cartridge can do.
 *
 * Everything here byte-swaps explicitly. WAD files are little-endian and the
 * VR4300 is big-endian, so every multi-byte field read from a lump has to be
 * converted -- the single most dangerous assumption in this port, because
 * getting it wrong yields a level that loads "successfully" and is garbage.
 */
#ifndef WAD_H
#define WAD_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define WAD_LUMP_NAME_LEN 8

typedef struct {
    char     name[WAD_LUMP_NAME_LEN + 1];   /* NUL-terminated for convenience */
    uint32_t offset;                        /* byte offset within its WAD */
    uint32_t size;
    uint8_t  src;                           /* which open WAD holds it */
} wad_lump_t;

/* How many WADs can be stacked at once. Doom's own -file takes a list; here
 * it is an IWAD plus a mod, and the spare slots cost 16 bytes each. */
#define WAD_MAX_SRC 8

/* Mount the WAD from the ROM filesystem and read its directory into RAM.
 * The directory is small (2305 lumps x 16 B = 36 KB for Ultimate Doom) and is
 * consulted constantly, so it stays resident; lump payloads do not.
 *
 * This closes anything already open and starts a fresh stack: it is the IWAD
 * call. Mods go on top of it with wad_add. */
bool wad_open(const char *path);

/* Stack another WAD on top of the ones already open, as Doom's -file does.
 *
 * The new directory is appended, and since wad_find searches backwards a
 * later WAD's lump shadows an earlier one of the same name -- which is the
 * whole of Doom's PWAD rule. Map lumps follow their marker, so a PWAD that
 * replaces MAP01 brings its own THINGS and LINEDEFS along with it.
 *
 * Must be called before W_N64_Init, which snapshots the directory. */
bool wad_add(const char *path);

/* Close every open WAD and drop the merged directory. The mod picker calls
 * this before rebuilding the stack; nothing else needs it. */
void wad_reset(void);

/* How many WADs are currently stacked, IWAD included. */
int  wad_num_sources(void);

int  wad_num_lumps(void);
const wad_lump_t *wad_lump(int index);

/* Find a lump by name, searching backwards so later (PWAD) entries win, as in
 * Doom. Returns -1 if absent. */
int  wad_find(const char *name);

/* Like wad_find, but only considers lumps at or after `from`. Map data lumps
 * are identified by position relative to their map marker, not by name alone:
 * every map has its own THINGS, LINEDEFS and so on. */
int  wad_find_from(const char *name, int from);

/* Read a lump into `dst`, which must be at least the lump's size. */
bool wad_read(int index, void *dst);

/* Read part of a lump. Used for the large geometry lumps, where the caller
 * streams directly into a typed array. */
bool wad_read_at(int index, uint32_t offset, void *dst, uint32_t len);

/* --- little-endian accessors -------------------------------------------
 *
 * Read from a raw lump buffer. Named for what they do rather than aliased to
 * Doom's SHORT()/LONG(), which are no-ops on little-endian hosts and are the
 * exact trap this port has to avoid. */

static inline int16_t wad_le16(const void *p)
{
    const uint8_t *b = p;
    return (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static inline int32_t wad_le32(const void *p)
{
    const uint8_t *b = p;
    return (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                     ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
}

#endif /* WAD_H */
