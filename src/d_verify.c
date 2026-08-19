/*
 * d_verify -- run Doom's own level loader and check it against ours.
 *
 * Before the renderer is migrated onto Doom's structures, the loader that
 * produces them has to be known good on this target. That is not a given: it
 * is C written for little-endian hosts, and every field in a WAD goes through
 * the byte-swap layer added for this port.
 *
 * The check is deliberately not "does it load without crashing". A level whose
 * bytes are swapped wrongly still loads: the counts come out plausible and the
 * structures are well-formed, but the geometry is scattered across the
 * coordinate space. Comparing counts and a known vertex against the loader
 * already proven on hardware catches that, which a self-consistency check
 * never would.
 *
 * This file speaks Doom's headers exclusively. p_level.h declares its own
 * vertex_t and sector_t, and the two cannot meet in one translation unit until
 * the migration is finished.
 */
#include "doom/doomtype.h"
#include "doom/doomdef.h"
#include "doom/doomstat.h"
#include "doom/p_local.h"
#include "doom/p_setup.h"
#include "doom/r_state.h"
#include "doom/sounds.h"           /* the music numbers, for the level track */
#include "doom/info.h"
#include "doom/p_mobj.h"
#include "doom/z_zone.h"

#include "r_ssdata.h"
#include "r_pvs.h"

#include <libdragon.h>

void W_N64_Init(void);

/* Load a level, replacing whatever was resident.
 *
 * Ordering is the whole content of this function. The texture arena and the
 * zone's PU_LEVEL blocks both belong to the outgoing level and must go first;
 * the lump cache holds pointers into those blocks, so it is dropped with them
 * or it hands back freed memory. R_InitSkyFlat has to run before P_SetupLevel
 * because P_LoadSectors resolves ceiling flats as it reads them and sky
 * ceilings must be recognisable by then.
 */
/* The two halves of a level swap, split so g_game.c's G_DoLoadLevel can
 * bracket its own P_SetupLevel call with them. Pre tears down everything
 * the outgoing level owns -- the ordering rationale is above. Post rebuilds
 * the renderer's static data and starts the level's music. */
/* See d_bridge.c: gates the 3D renderer and the level-only UI. */
extern boolean d_level_resident_set(boolean v);

void D_LevelPreSetup(void)
{
    void p_level_reset_assets(void);
    void D_LightsInvalidate(void);
    d_level_resident_set(false);
    void W_N64_DropCache(void);
    void R_InitSkyFlat(void);

    /* The dynamic-light cache keys on gametic, which keeps counting across
     * a level change while the thinker list it was built from is destroyed. */
    D_LightsInvalidate();

    /* The frame tail is pipelined: the RDP may still be rasterising from
     * textures in the arena this teardown resets. Drain it first. */
    rspq_wait();

    r_pvs_reset();
    p_level_reset_assets();
    Z_FreeTags(PU_LEVEL, PU_CACHE);
    W_N64_DropCache();

    R_InitSkyFlat();
}

void D_LevelPostSetup(void)
{
    void D_SetSky(int episode);
    void mus_play(int episode, int map);

    /* Switch textures resolve to indices in this level's texture table, so
     * the list has to be rebuilt after P_SetupLevel rather than once at
     * startup the way P_Init does it -- the table is reset per level, and
     * the port never called P_Init at all, which is why switches could
     * never flip: switchlist[] stayed zeroed and matched nothing. */
    {
        void P_InitSwitchList(void);
        P_InitSwitchList();
    }
    { void p_level_anim_init(void); p_level_anim_init(); }

#if R_HALO
    { void D_BuildSkyWells(void); D_BuildSkyWells(); }
#endif
#if D_KEYLIGHT
    /* After the things are spawned: this reads the sector thing lists. */
    { void D_BuildLampSectors(void); D_BuildLampSectors(); }
#endif
    R_BuildSubsectorData();
    r_pvs_load(gameepisode, gamemap);
    D_SetSky(gameepisode);

    /* Doom's own choice of level track, from G_DoLoadLevel: one track per map
     * in Doom II, episode-major in Doom. Routed through S_ChangeMusic rather
     * than named directly so both games resolve the same way. */
    {
        void S_ChangeMusic(int musicnum, int looping);
        const int mnum = gamemode == commercial
            ? mus_runnin + gamemap - 1
            : mus_e1m1 + (gameepisode - 1) * 9 + gamemap - 1;
        S_ChangeMusic(mnum, true);
    }
    d_level_resident_set(true);

#if R_RUNTIMEART
    { extern int p_art_us, p_art_count;
      extern int r_wadart_io_us;
      extern int r_wadart_find_us, r_wadart_draw_us, r_wadart_mask_us, r_wadart_build_us;
      debugf("art: %d composed, %d ms (io %d, find %d, draw %d, mask %d, build %d)\n",
             p_art_count, p_art_us / 1000, r_wadart_io_us / 1000,
             r_wadart_find_us / 1000, r_wadart_draw_us / 1000,
             r_wadart_mask_us / 1000, r_wadart_build_us / 1000); }
#endif
#if R_WADART_VERIFY
    /* Cumulative: sprites resolve lazily as they are first drawn, so a
     * level's count keeps climbing after its geometry is up. */
    { extern int r_wadart_verify_ok, r_wadart_verify_bad;
      debugf("wadart verify: %d identical, %d MISMATCHED\n",
             r_wadart_verify_ok, r_wadart_verify_bad); }
#endif

    if (gamemode == commercial)
        debugf("level MAP%02d: %d sectors, %d segs, zone free %d KB\n",
               gamemap, numsectors, numsegs, Z_FreeMemory() / 1024);
    else
        debugf("level E%dM%d: %d sectors, %d segs, zone free %d KB\n",
               gameepisode, gamemap, numsectors, numsegs, Z_FreeMemory() / 1024);
}

/* The single-player setup D_CheckNetGame does, which this port never calls.
 *
 * Without it playeringame[0] is false, and P_SpawnPlayer returns before
 * spawning anything: no player mobj, and no ST_Start to give the status bar
 * its player pointer. The next tic dereferences that null pointer inside
 * ST_updateWidgets and the console faults.
 *
 * It went unnoticed because everything that loaded a level happened to set
 * the flag on the way past. Demo playback reads playeringame straight out of
 * the demo header, and the port's own D_LoadLevel sets it -- so the attract
 * loop and the LEVELTEST boot both worked. Starting a game from the menu goes
 * through Doom's G_DoNewGame instead, which clears players 1 to 3 and assumes
 * player 0 was established at startup. Once a demo had run, a new game
 * inherited its flag and worked; started before the first demo, it crashed. */
void D_SinglePlayerSetup(void)
{
    consoleplayer = displayplayer = 0;
    playeringame[0] = true;
    for (int i = 1; i < MAXPLAYERS; i++)
        playeringame[i] = false;
    netgame    = false;
    deathmatch = false;
}

/* Which game this IWAD is, from the lumps it contains.
 *
 * gamemode was left at `indetermined`, which behaves like registered Doom
 * because that is the fallback branch everywhere it is tested -- fine while
 * there was only ever one IWAD. Doom II needs `commercial` or nothing works:
 * P_SetupLevel builds "E1M1" instead of "MAP01" and cannot find the level,
 * the intermission draws an episode map that does not exist, and the menu
 * offers an episode list for a game with one episode.
 *
 * Identified the way chocolate doom's IdentifyIWADByName does, by asking for
 * the first level of each possibility rather than trusting a filename. */
void D_IdentifyGame(void)
{
    if (W_CheckNumForName("MAP01") >= 0) {
        gamemode    = commercial;
        gamemission = doom2;
    } else if (W_CheckNumForName("E4M1") >= 0) {
        gamemode = retail;
    } else if (W_CheckNumForName("E2M1") >= 0) {
        gamemode = registered;
    } else if (W_CheckNumForName("E1M1") >= 0) {
        gamemode = shareware;
    }

    /* Name the save files after the game, so the two cartridges do not share
     * slots. The prefix was left at its "doom" default and never set, which
     * meant Doom II wrote doom_0.sav over Doom's -- and then loaded it, since
     * a savegame carries no record of which IWAD it belongs to. */
    {
        void P_SetSavePrefix(const char *prefix);
        P_SetSavePrefix(gamemode == commercial ? "doom2" : "doom");
    }

    debugf("game: %s\n",
           gamemode == commercial ? "Doom II (commercial)" :
           gamemode == retail     ? "Ultimate Doom (retail)" :
           gamemode == registered ? "Doom (registered)" :
           gamemode == shareware  ? "Doom (shareware)" : "unidentified");
}

void D_LoadLevel(int episode, int map)
{
    D_LevelPreSetup();

    playeringame[0] = true;
    players[0].playerstate = PST_REBORN;
    consoleplayer = displayplayer = 0;

    gameepisode = episode;
    gamemap     = map;

    P_SetupLevel(episode, map, 0, gameskill);
    D_LevelPostSetup();
}
