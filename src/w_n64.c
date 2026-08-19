/*
 * w_n64 -- Doom's W_ lump interface over the cartridge.
 *
 * Chocolate Doom's w_wad.c sits on stdio and mmap, neither of which exists
 * here: the IWAD is 14 MB against 8 MB of RDRAM, so it stays on the cartridge
 * and lumps are pulled across the PI bus on demand. src/wad.c already does
 * that; this presents it as the API Doom's own code expects, so p_setup.c and
 * everything downstream compile unmodified.
 *
 * The one real difference is caching. Doom's W_CacheLumpNum hands back a
 * pointer that stays valid until the lump is released, and callers rely on
 * that -- P_LoadSegs walks the returned block directly. A cartridge read
 * cannot return a pointer into ROM, so each cached lump is read into zone
 * memory once and the pointer remembered. PI DMA is roughly 5 MB/s, which is
 * fine at level load and ruinous inside a frame; the cache is what keeps it to
 * the former.
 */
#include "wad.h"

#include "doom/doomtype.h"
#include "doom/i_system.h"
#include "doom/w_wad.h"
#include "doom/z_zone.h"

#include <libdragon.h>
#include <string.h>

/* One entry per lump. E1M1's IWAD has ~2300, so this is a few KB of pointers
 * against the alternative of a hash table we would have to keep in step. */
/* Doom publishes lump metadata as an array of pointers, and the game code does
 * use it: P_SetupLevel takes maplumpinfo straight out of this table. It is
 * built once from our own directory rather than left null.
 *
 * The indirection is Doom's, not ours -- it lets w_wad.c splice PWAD entries
 * over IWAD ones by rewriting pointers. Nothing here does that, so the records
 * sit in one contiguous block with the pointer array alongside. */
lumpinfo_t **lumpinfo;
unsigned int numlumps;

boolean W_PointerInWadMapped(const void *ptr, char out_name[8], size_t *offset)
{
    /* Only meaningful when the WAD is memory-mapped. It never is here: the
     * IWAD stays on the cartridge and lumps are copied into the zone. */
    (void)ptr; (void)out_name; (void)offset;
    return false;
}

static void **lumpcache;
static int    numlumps_cached;

/* Pointer-to-lump records for immortal lumps.
 *
 * W_LumpForPointer identifies a patch by scanning lumpcache, but D_LoadLevel
 * wipes that table at every transition while the PU_STATIC blocks behind the
 * UI's patches live on -- so after the first level change every font and
 * status-bar patch became unidentifiable and the UI stopped resolving art.
 * PU_STATIC blocks are never freed, so their pointers are unique for the
 * program's lifetime and can be recorded once, immune to the cache wipe.
 * Only tags at or below PU_STATIC are recorded: purgeable blocks can be freed
 * and their addresses reused, which would make a stale record lie. */
#define W_STATIC_MAP_MAX 256
static struct { const void *ptr; lumpindex_t lump; } static_map[W_STATIC_MAP_MAX];
static int static_map_count;

/* Re-runnable, because the mod picker rebuilds the WAD stack while the game
 * is up. The three tables are freed and taken again at the new size.
 *
 * What makes this safe is that the IWAD is always re-opened FIRST and a mod
 * only ever appends: lump indices 0..n-1 name the same lumps before and
 * after, so every index Doom cached earlier -- the UI's patches above all --
 * still points where it did. Only the appended tail changes. */
static lumpinfo_t *lump_recs;

void W_N64_Init(void)
{
    if (lumpcache) { Z_Free(lumpcache); lumpcache = NULL; }
    if (lump_recs) { Z_Free(lump_recs); lump_recs = NULL; }
    if (lumpinfo)  { Z_Free(lumpinfo);  lumpinfo  = NULL; }

    numlumps_cached = wad_num_lumps();
    numlumps        = (unsigned int)numlumps_cached;

    lumpcache = Z_Malloc(numlumps_cached * sizeof *lumpcache, PU_STATIC, NULL);
    memset(lumpcache, 0, numlumps_cached * sizeof *lumpcache);

    lumpinfo_t  *recs = Z_Malloc(numlumps_cached * sizeof *recs, PU_STATIC, NULL);
    lump_recs = recs;
    lumpinfo = Z_Malloc(numlumps_cached * sizeof *lumpinfo, PU_STATIC, NULL);

    for (int i = 0; i < numlumps_cached; i++) {
        const wad_lump_t *l = wad_lump(i);

        /* Doom's name field is 8 bytes and not NUL-terminated; ours keeps a
         * terminator for convenience, so copy the significant bytes only. */
        memset(recs[i].name, 0, sizeof recs[i].name);
        for (int c = 0; c < 8 && l->name[c]; c++) recs[i].name[c] = l->name[c];

        recs[i].wad_file = NULL;          /* nothing memory-maps a cartridge */
        recs[i].position = (int)l->offset;
        recs[i].size     = (int)l->size;
        recs[i].cache    = NULL;
        recs[i].next     = -1;

        lumpinfo[i] = &recs[i];
    }
}

lumpindex_t W_CheckNumForName(const char *name)
{
    return wad_find(name);
}

lumpindex_t W_GetNumForName(const char *name)
{
    const lumpindex_t i = W_CheckNumForName(name);
    if (i < 0) I_Error("W_GetNumForName: %s not found!", name);
    return i;
}

int W_LumpLength(lumpindex_t lump)
{
    if (lump < 0 || lump >= wad_num_lumps())
        I_Error("W_LumpLength: %i >= numlumps", lump);
    return (int)wad_lump(lump)->size;
}

void W_ReadLump(lumpindex_t lump, void *dest)
{
    if (!wad_read(lump, dest))
        I_Error("W_ReadLump: error reading lump %i", lump);
}

void *W_CacheLumpNum(lumpindex_t lump, int tag)
{
    if (lump < 0 || lump >= numlumps_cached)
        I_Error("W_CacheLumpNum: %i >= numlumps", lump);

    if (!lumpcache[lump]) {
        /* A static lump may already be resident from a previous level or
         * attract cycle: the level-change drop wipes the table, not the
         * blocks. Re-link instead of re-reading -- re-reading allocated a
         * fresh block each time and leaked the old one, and a long title
         * session bled the zone dry a demo buffer and a font at a time. */
        for (int i = 0; i < static_map_count; i++)
            if (static_map[i].lump == lump)
                return lumpcache[lump] = (void *)static_map[i].ptr;

        const int len = W_LumpLength(lump);

        /* No user pointer.
         *
         * Passing &lumpcache[lump] hands the zone a reference it will write
         * NULL into when the block is freed or purged -- so a Z_FreeTags at
         * level load could empty an entry while this table still believed it
         * held one, and the next lookup returned NULL instead of re-reading.
         * The cache is invalidated explicitly by W_N64_DropCache instead.
         *
         * That choice makes purgeable tags unusable as-is: Z_Malloc asserts
         * when a purgeable block has no owner, and Doom's UI caches lumps
         * with PU_CACHE at draw time (ST_Stop's palette, M_Drawer's menu
         * patches). Clamp those to PU_LEVEL: they are freed by the explicit
         * Z_FreeTags at every level change, which is exactly when this cache
         * table is dropped anyway. */
        const int atag = tag >= PU_PURGELEVEL ? PU_LEVEL : tag;
        void *ptr = Z_Malloc(len ? len : 1, atag, NULL);
        if (len) W_ReadLump(lump, ptr);
        lumpcache[lump] = ptr;

        if (tag <= PU_STATIC && static_map_count < W_STATIC_MAP_MAX) {
            static_map[static_map_count].ptr  = ptr;
            static_map[static_map_count].lump = lump;
            static_map_count++;
        }
    }
    return lumpcache[lump];
}

void *W_CacheLumpName(const char *name, int tag)
{
    return W_CacheLumpNum(W_GetNumForName(name), tag);
}

void W_ReleaseLumpNum(lumpindex_t lump)
{
    /* Level lumps are torn down wholesale by the zone purge, so dropping the
     * pointer is enough for them. A released STATIC lump -- the demo buffer
     * between attract plays -- is freed for real, or the zone never gets it
     * back: that leak, times every demo of every attract cycle, was the
     * Z_Malloc failure after leaving the title screen running. */
    if (lump < 0 || lump >= numlumps_cached) return;

    void *ptr = lumpcache[lump];
    lumpcache[lump] = NULL;
    if (!ptr) return;

    for (int i = 0; i < static_map_count; i++)
        if (static_map[i].ptr == ptr) {
            static_map[i] = static_map[--static_map_count];
            Z_Free(ptr);
            return;
        }
}

void W_ReleaseLumpName(const char *name)
{
    W_ReleaseLumpNum(W_GetNumForName(name));
}

void W_N64_DropCache(void)
{
    if (lumpcache)
        memset(lumpcache, 0, numlumps_cached * sizeof *lumpcache);
}

/* Which lump a cached pointer came from.
 *
 * Doom's UI code caches a lump and hands the resulting patch_t* to
 * V_DrawPatch, which is the only clue the renderer gets about what it is being
 * asked to draw. The art here is baked on the host, so the pointer has to be
 * turned back into a name to find it -- the cache already knows, it just needs
 * asking in reverse.
 *
 * Linear, but over pointers already in cache rather than the whole directory,
 * and the UI draws a few dozen patches a frame at most.
 */
lumpindex_t W_LumpForPointer(const void *ptr)
{
    if (!ptr) return -1;

    /* Immortal lumps first: this map survives the level-change cache wipe,
     * and the UI's patches -- the only callers -- are all PU_STATIC. */
    for (int i = 0; i < static_map_count; i++)
        if (static_map[i].ptr == ptr) return static_map[i].lump;

    if (!lumpcache) return -1;
    for (int i = 0; i < numlumps_cached; i++)
        if (lumpcache[i] == ptr) return i;
    return -1;
}

const char *W_LumpName(lumpindex_t lump)
{
    if (lump < 0 || lump >= numlumps_cached) return NULL;
    return wad_lump(lump)->name;
}
