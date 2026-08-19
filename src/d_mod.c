#include "d_mod.h"
#include "wad.h"

#include <libdragon.h>
#include <dir.h>
#include <stdio.h>
#include <string.h>

const char *D_DataDir(void);

/* Where to look for the mods folder, in the order the IWAD search uses and
 * for the same reason: which spelling resolves through this FatFs build is
 * not predictable, so every one a player might have is tried. The folder the
 * game's own files came from goes first when it is known. */
static const char *const mod_dirs[] = {
    "sd:/Doom/", "sd:/DOOM/", "sd:/doom/", "sd:/",
};

bool D_ModDir(int i, char *out, unsigned cap)
{
    const char *dd = D_DataDir();
    if (dd && dd[0]) {
        if (i == 0) { snprintf(out, cap, "%smods", dd); return true; }
        i--;
    }
    if (i < 0 || i >= (int)(sizeof mod_dirs / sizeof mod_dirs[0])) return false;
    snprintf(out, cap, "%smods", mod_dirs[i]);
    return true;
}

static char mods[D_MOD_MAX][D_MOD_NAME_MAX];
static int  mod_count;
static char selected[D_MOD_NAME_MAX];

/* The IWAD that boot actually opened. Switching mods rebuilds the stack from
 * the bottom, and the bottom has to be the same file -- re-finding it by
 * searching the card again could land on a different spelling in a different
 * folder, and every lump index Doom is holding would shift underneath it. */
static char iwad_path[80];

void D_ModSetIwad(const char *path)
{
    if (path) snprintf(iwad_path, sizeof iwad_path, "%s", path);
}

/* Case-insensitive ".wad" test. The card presents names in whatever case its
 * long-filename records carry, and a player's mod is as likely to be
 * sigil.wad as SIGIL.WAD. */
static bool is_wad(const char *name)
{
    const size_t n = strlen(name);
    if (n < 5) return false;
    const char *e = name + n - 4;
    return (e[0] == '.') &&
           (e[1] == 'w' || e[1] == 'W') &&
           (e[2] == 'a' || e[2] == 'A') &&
           (e[3] == 'd' || e[3] == 'D');
}

void D_ModScan(void)
{
    mod_count = 0;

    char dir[64];
    for (int d = 0; D_ModDir(d, dir, sizeof dir) && !mod_count; d++) {
        dir_t ent;
        int r = dir_findfirst(dir, &ent);
        while (r == 0 && mod_count < D_MOD_MAX) {
            if (ent.d_type == DT_REG && is_wad(ent.d_name))
                snprintf(mods[mod_count++], D_MOD_NAME_MAX, "%s", ent.d_name);
            r = dir_findnext(dir, &ent);
        }
        if (mod_count)
            debugf("mod: %d in %s\n", mod_count, dir);
    }
}

int D_ModCount(void) { return mod_count; }

const char *D_ModName(int i)
{
    if (i < 0 || i >= mod_count) return NULL;
    return mods[i];
}

const char *D_ModSelected(void) { return selected; }

void D_ModSetSelected(const char *name)
{
    if (!name) name = "";
    snprintf(selected, sizeof selected, "%s", name);
}

bool D_ModStack(void)
{
    bool any = false;

    /* A mod baked in at build time is part of this cartridge's identity --
     * `./build.sh PWAD=mod.wad` puts its textures and sprites in the ROM
     * alongside its maps -- so it always loads, and loads first. Nothing to
     * select and nothing to go missing. */
    if (wad_add("/mod.wad")) {
        debugf("mod: baked-in /mod.wad\n");
        any = true;
    }

    if (!selected[0]) return any;

    char dir[64], path[128];
    for (int d = 0; D_ModDir(d, dir, sizeof dir); d++) {
        snprintf(path, sizeof path, "%s/%s", dir, selected);
        if (wad_add(path)) {
            debugf("mod: loaded %s\n", path);
            return true;
        }
    }

    /* A mod named in options.cfg that is no longer on the card. Saying so is
     * the whole of the handling: the game runs as the plain IWAD, which is
     * what the player will see anyway. */
    debugf("mod: '%s' not found, playing without it\n", selected);
    selected[0] = '\0';
    return any;
}

/* Switch mods with the game already up.
 *
 * Only safe because the stack is rebuilt from the same IWAD and a mod only
 * ever appends: lumps 0..n-1 keep their indices, so the patches the UI cached
 * at boot still resolve. See W_N64_Init, which relies on the same property.
 *
 * The caller must not be in a level -- the menu only offers this from the
 * title screen, where nothing holds level data.
 */
bool D_ModApply(const char *name)
{
    void W_N64_Init(void);
    void W_N64_DropCache(void);
    void D_IdentifyGame(void);
    int   W_CheckNumForName(const char *n);
    int   W_LumpLength(int lump);
    void *W_CacheLumpNum(int lump, int tag);
    void  dt64_build_tluts(const uint8_t *playpal, int nbytes);
    void  D_OptionsSave(void);

    if (!iwad_path[0]) {
        debugf("mod: no IWAD path recorded, cannot switch\n");
        return false;
    }

    D_ModSetSelected(name);

    wad_reset();
    if (!wad_add(iwad_path)) {
        /* The card was pulled, or the file went away between boot and now.
         * Nothing can be drawn from here, so say so loudly rather than
         * limping on with an empty directory. */
        debugf("mod: LOST the IWAD at '%s'\n", iwad_path);
        return false;
    }
    D_ModStack();

    /* The lump table, then everything read through it. */
    W_N64_Init();
    W_N64_DropCache();
    D_IdentifyGame();

    const int pp = W_CheckNumForName("PLAYPAL");
    if (pp >= 0)
        dt64_build_tluts(W_CacheLumpNum(pp, 1 /* PU_STATIC */), W_LumpLength(pp));

    /* Remember it for next boot, where the same stack is built the easy way. */
    D_OptionsSave();

    debugf("mod: now running '%s'\n", selected[0] ? selected : "(none)");
    return true;
}
