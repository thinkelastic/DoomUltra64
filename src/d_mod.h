/*
 * d_mod -- add-on WADs from the card.
 *
 * Doom's PWAD rule is "same name, later entry wins", and src/wad.c already
 * implements it: wad_add appends a directory and wad_find searches backwards.
 * This is the layer above -- finding what is on the card, remembering which
 * one the player chose, and stacking it before the lump table is built.
 *
 * WHAT A MOD CAN AND CANNOT CHANGE. Levels are read from the WAD at run time,
 * so new maps work. Textures, flats, sprites, sounds and menu art are baked
 * into the cartridge at build time and resolved by NAME (see p_level.c and
 * tools/wad2n64.c), so a mod's custom art is not in the ROM and cannot be
 * conjured at run time -- those walls come up missing. Bake it in with
 * `./build.sh PWAD=mod.wad` when a mod brings its own art.
 */
#ifndef D_MOD_H
#define D_MOD_H

#include <stdbool.h>

/* A FAT name is 8.3 or a long-filename record; 64 covers both without
 * making the table big enough to care about. */
#define D_MOD_NAME_MAX 64

/* Enough to list a card full of mods without a scrollbar's worth of work. */
#define D_MOD_MAX 32

/* Enumerate the mods folder. Cheap enough to call whenever the menu opens,
 * which is what keeps the list honest after the card is changed. */
void        D_ModScan(void);

int         D_ModCount(void);
const char *D_ModName(int i);          /* file name as it sits on the card */

/* The chosen mod, "" when the player is running the plain game. Read from
 * options.cfg at boot, so a mod survives a power cycle. */
const char *D_ModSelected(void);
void        D_ModSetSelected(const char *name);   /* "" or NULL clears it */

/* Stack the selection onto the already-open IWAD. Must run before
 * W_N64_Init, which snapshots the merged directory into Doom's lump table.
 * False when nothing was loaded, including the ordinary "none selected". */
bool        D_ModStack(void);

/* The folder mods live in, resolved against wherever the game's files were
 * found. Written into `out`; false when no candidate exists. */
bool        D_ModDir(int i, char *out, unsigned cap);

/* Remember which IWAD boot opened, so a later switch can rebuild the stack
 * on the same foundation. */
void        D_ModSetIwad(const char *path);

/* Switch mods with the game running, from the title screen only. Rebuilds
 * the WAD stack and Doom's lump table, and persists the choice. */
bool        D_ModApply(const char *name);

#endif /* D_MOD_H */
