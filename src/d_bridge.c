/*
 * d_bridge -- what Doom's game code expects of the renderer and the host.
 *
 * Doom's level loader and game logic call outward in three directions: into
 * the renderer for texture lookup and precaching, into subsystems this port
 * does not have (sound, dehacked, networking), and into parts of the game
 * layer not yet ported. All three land here.
 *
 * The stubs are deliberate, not placeholders to be filled in later. Doom's
 * software renderer precomputes column and span data at level load because it
 * rasterises on the CPU; the RDP needs none of it, so those entry points do
 * nothing rather than build caches nobody reads. The distinction matters when
 * reading this file: a no-op here usually means "the hardware makes this
 * unnecessary", and only the ones marked STAGE mean "not written yet".
 */
/* Deliberately not including p_level.h: it declares vertex_t, sector_t,
 * line_t and fixed_t of its own, and this file speaks Doom's. The two sets are
 * being merged onto Doom's -- until that lands, the one function needed from
 * the old loader is declared here rather than dragging its headers in. */
int   p_level_resolve(const char *name, const char *prefix);
void *p_level_resolve_ptr(const char *name, const char *prefix);
void *p_level_thing_sprite(int type);

#include "doom/doomtype.h"
#include "doom/doomkeys.h"
#include "doom/d_mode.h"
#include "doom/doomstat.h"
#include "doom/i_system.h"
#include "doom/r_defs.h"
#include "doom/p_local.h"
#include "doom/p_tick.h"
#include "doom/p_mobj.h"
#include "doom/info.h"
#include "doom/tables.h"
#include "doom/p_pspr.h"
#include "doom/d_player.h"
#include "doom/r_state.h"
#include "doom/r_main.h"
#include "doom/g_game.h"
#include "doom/v_video.h"
#include "doom/d_main.h"
#include "doom/umapinfo.h"
#include "doom/sounds.h"
#include "doom/am_map.h"
#include "dt64.h"
#include "doom/w_wad.h"
#include "doom/z_zone.h"

#include "r_ssdata.h"

#include <libdragon.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* --- renderer: name lookup -------------------------------------------- */

/* Doom stores texture and flat *indices* in sidedefs and sectors, resolved
 * from names once at load. This port keeps its own table -- one entry per
 * texture actually referenced by the level, loaded from the cartridge -- so
 * these map Doom's names onto our indices and the rest of the loader is
 * unchanged. */
int R_TextureNumForName(const char *name)
{
    return p_level_resolve(name, "");
}

/* The index F_SKY1 resolves to. Doom's r_data.c sets this in R_InitFlats and
 * every "is this ceiling open to the sky" test compares against it; that file
 * is the software renderer's texture compositor and is not ported, so it is
 * established here instead.
 *
 * F_SKY1 is a marker rather than a real flat -- no sector ever draws it -- so
 * what matters is only that the value is distinct and stable, not that it
 * names loadable art. */
int skyflatnum = -1;

void R_InitSkyFlat(void)
{
    skyflatnum = p_level_resolve("F_SKY1", "f_");
}

int R_FlatNumForName(const char *name)
{
    /* Flats are converted to their own prefixed set on the host: they are
     * 64x64 in the WAD but downsampled here, and share no namespace with wall
     * textures despite Doom treating both as "pics". */
    return p_level_resolve(name, "f_");
}

int R_CheckTextureNumForName(const char *name)
{
    return p_level_resolve(name, "");
}

/* --- renderer: load-time precomputation the RDP does not need ---------- */

/* Doom builds per-seg and per-sector render caches at load because its
 * software renderer walks them every frame. Ours submits triangles to the RDP
 * and reads the level structures directly, so there is nothing to cache. */
void R_BuildSegRenderData(void) { }
void R_BuildBSPRenderData(void) { }

/* Sprite rotation tables and the texture precache are both load-time work for
 * a renderer that composites patches in RAM. Textures here are baked on the
 * host and streamed off the cartridge by p_level.c instead. */
void R_InitSprites(const char **namelist) { (void)namelist; }
void R_PrecacheLevel(void)          { }

/* --- host services not present on a cartridge ------------------------- */

int DEH_snprintf(char *buf, size_t len, const char *fmt, ...)
{
    /* No dehacked support: the string passes through untouched. */
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, len, fmt, ap);
    va_end(ap);
    return n;
}

/* Doom probes for available memory to size its zone. mem.c decides that here,
 * from a fixed budget chosen against the Expansion Pak's 8 MB. */
boolean I_GetMemoryValue(unsigned int offset, void *value, int size)
{
    (void)offset; (void)value; (void)size;
    return false;
}

void W_Reload(void) { }        /* no -file reloading from a cartridge */
void S_Start(void)  { }        /* STAGE: sound */


/* The reject matrix is an optional line-of-sight acceleration table. Some WADs
 * ship one too short for the sector count, which Doom's own loader pads. Doing
 * nothing is safe -- P_CheckSight falls back to tracing -- but a short read
 * would not be, so this is left explicit rather than absent. */
void PadRejectArray(byte *array, unsigned int len)
{
    (void)array; (void)len;
}

/* --- game layer not yet ported ---------------------------------------- */


/* P_SpawnMapThing now comes from Doom's p_mobj.c: things become real mobj_t
 * actors on the thinker list, with a state chain, rather than the position and
 * sprite the renderer previously recorded. */

/* --- collision and sound, pending their own stages -------------------- */

/* Collision now comes from Doom's own p_map.c. */


/* Sound effects live in i_sound_n64.c. */


/* --- sprite frames from actor state ----------------------------------- */

/* Resolve the baked texture for one frame of one sprite.
 *
 * Doom names sprite lumps <BASE><frame><rotation>: TROOA1 is the Imp's first
 * frame seen head-on. Rotation 0 means the sprite looks the same from every
 * angle, which is true of every pickup and decoration; monsters instead have
 * eight rotations and no 0 lump at all. The two forms are mutually exclusive
 * in a WAD, so trying the rotation-less name first and falling back is how a
 * caller finds out which kind it has without a sprite directory to consult.
 *
 * The build tool bakes these with an s_ prefix, one file per frame, so this is
 * a name lookup rather than the patch-and-rotation table Doom builds at load.
 */
/* (sprite, frame, rot) -> resolved texture memo.
 *
 * The full resolution below probes up to three name lookups per call, and it
 * runs for every visible actor every frame. The state-to-lump mapping cannot
 * change within a level, so one small direct-mapped table answers repeats --
 * including the misses, which monsters probe deliberately (no rotation-0
 * lump). Cleared at level transitions by p_level_reset_assets, since the
 * lvl_tex slots behind the pointers are reused by the next level. */
#define SPRCACHE_SIZE 512
static struct { uint32_t key; void *tex; } sprcache[SPRCACHE_SIZE];

void R_SpriteFrameReset(void)
{
    memset(sprcache, 0, sizeof sprcache);
}

void *R_SpriteFrame(int sprite, int frame, int rot)
{
    if (sprite < 0 || sprite >= NUMSPRITES) return NULL;

    const uint32_t key = 0x80000000u | ((uint32_t)sprite << 9) |
                         ((uint32_t)(frame & FF_FRAMEMASK) << 3) |
                         (uint32_t)(rot & 7);
    const uint32_t slot = (key * 2654435761u) >> (32 - 9);
    if (sprcache[slot].key == key) return sprcache[slot].tex;

    const char *base = sprnames[sprite];
    if (!base) return NULL;

    const char f = (char)('A' + (frame & FF_FRAMEMASK));
    const char r = (char)('1' + (rot & 7));

    char name[16];
    for (int i = 0; i < 4; i++) name[i] = base[i];
    name[4] = f;

    /* Rotation-less first. A lump ending in 0 means the thing looks the same
     * from every angle, which is true of every pickup and decoration, and such
     * a sprite has no numbered rotations at all. */
    name[5] = '0';
    name[6] = '\0';
    void *t = p_level_resolve_ptr(name, "s_");

    /* Then the plain rotation. Only 1 and 5 -- front and back -- normally
     * exist in this form, because they are the two views with no mirror. */
    if (!t) {
        name[5] = r;
        name[6] = '\0';
        t = p_level_resolve_ptr(name, "s_");
    }

    /* Finally the mirrored pair. Doom stores one lump for two opposing views
     * and flips it for the second: POSSA2A8 is rotation 2 and, mirrored,
     * rotation 8. The partner of R is 10-R, and the lump is named with the
     * lower rotation first.
     *
     * NOTE: the mirrored half should be drawn horizontally flipped. It is not
     * yet, so a monster seen from one side shows the other side's art. The
     * shape and animation are right; only the handedness is wrong. */
    if (!t) {
        const int partner = 10 - (int)(r - '0');
        if (partner >= 2 && partner <= 8) {
            const char pr = (char)('0' + partner);
            const char lo = r < pr ? r : pr;
            const char hi = r < pr ? pr : r;
            name[5] = lo; name[6] = f; name[7] = hi; name[8] = '\0';
            t = p_level_resolve_ptr(name, "s_");
        }
    }

    /* Do not memoise a failure that was only a failed read: the resolver
     * keeps those retryable, and caching the NULL here would defeat that
     * and hide the sprite for the rest of the level anyway. A frame that
     * genuinely has no art resolves to NULL with dt64_last_absent set, and
     * that is worth remembering -- sprite rotations are probed by name and
     * missing ones are the common case, not an error. */
    if (t || dt64_last_absent) {
        sprcache[slot].key = key;
        sprcache[slot].tex = t;
    }
    return t;
}

/* --- thinker-tick dependencies ---------------------------------------- */

/* P_Ticker calls these every tic. They belong to files not yet vendored, and
 * each is inert rather than absent so the thinker list can run on its own:
 * that is what makes actors animate before anything else works. */
/* P_RespawnSpecials belongs to p_mobj.c, which is now vendored. */



/* --- pending stages --------------------------------------------------- */

/* Damage and pickups now come from p_inter.c. */

/* Line specials are Stage 5. p_spec.c currently handles doors on the old
 * structures; these are the hooks Doom's own collision calls instead. */


/* Monsters of the same species normally do not retaliate against each other.
 * Dehacked can lift that; there is no dehacked here. */
boolean deh_species_infighting = false;

char  **myargv;
/* Argument parsing and string helpers come from m_misc.c. */

/* Marks structures already visited in a traversal, so a line shared by two
 * blockmap cells is tested once. Doom keeps it in r_main.c. */
int validcount = 1;

/* --- player movement through Doom's collision ------------------------- */

/* Turn controller state into a Doom ticcmd.
 *
 * This is the whole interface between the console and the game. P_PlayerThink
 * reads nothing else: movement, turning, firing, weapon switching and using
 * doors are all expressed here and carried out by Doom's own code on the next
 * tic. Applying thrust directly -- which this did while p_user.c was missing --
 * bypassed the weapon state machine and the view bob along with it.
 *
 * The scale factors are Doom's own: forwardmove 25 and sidemove 24 at normal
 * speed, in units P_MovePlayer multiplies by 2048 to reach fixed point.
 */
void D_PlayerInput(float fwd, float strafe, float turn, boolean run,
                   boolean attack, boolean use)
{
    /* Into the local command, not players[0].cmd: G_Ticker owns that copy
     * and overwrites it from netcmds each tic (or from the demo file during
     * playback). D_TicStart publishes this one just before the tic runs. */
    extern ticcmd_t *d_localcmd(void);
    ticcmd_t *cmd = d_localcmd();
    memset(cmd, 0, sizeof *cmd);

    /* Doom's own speeds: walk 25/24, run 50/40 -- the same numbers holding
     * shift produces on the PC. */
    cmd->forwardmove = (signed char)(fwd    * (run ? 50.0f : 25.0f));
    cmd->sidemove    = (signed char)(strafe * (run ? 40.0f : 24.0f));

    /* angleturn is the top 16 bits of an angle_t, so a whole circle is 65536
     * and the wrap is free. */
    const float full = 4294967296.0f;      /* 2^32 == one full turn */
    cmd->angleturn = (short)(int)(turn * (full / (2.0f * (float)M_PI)) / 65536.0f);

    if (attack) cmd->buttons |= BT_ATTACK;
    if (use)    cmd->buttons |= BT_USE;

}

/* Where the camera sits. viewz comes from P_CalcHeight, so it carries the
 * view bob and the eye easing over steps that Doom applies. */
/* --- frame interpolation ------------------------------------------------
 *
 * The game simulates at a fixed 35 Hz; frames render faster. The vendored
 * game code already snapshots the previous tic (mobj old*, player oldviewz,
 * psprite oldsx/oldsy, per-sector old heights plus the changed-sector list
 * in p_tick.c) -- this side turns those pairs into a smooth picture by
 * lerping with fractionaltic, the sub-tic phase of the render moment.
 *
 * fractionaltic is anchored to the same accumulator that decides when tics
 * run (main.c), so the phase and the (old, current) pair can never disagree
 * -- the classic one-frame backward hiccup at tic boundaries comes from
 * anchoring the phase to a free-running clock instead. */
boolean r_interpolate = false;
fixed_t fractionaltic = 0;

void D_SetTicFrac(uint32_t into_tic_us)
{
#if D_INTERP
    if (into_tic_us > 28570u) into_tic_us = 28570u;  /* hold at the pair */
    fractionaltic = (fixed_t)(((uint64_t)into_tic_us << FRACBITS) / 28571u);
    r_interpolate = true;
#else
    (void)into_tic_us;
    fractionaltic  = 0;
    r_interpolate  = false;
#endif
}

/* Swap the heights of every sector that moved this tic to their lerped
 * values for the duration of the render, so doors and lifts glide. The
 * renderer reads sector_t directly during traversal; swapping in place
 * keeps culling, wall emission and flat queueing consistent with each
 * other. Restore before the next batch of game tics runs. */
void D_InterpBegin(void)
{
    if (!r_interpolate) return;
    for (int i = 0; i < numinterpolatedsectors; i++) {
        sector_t *sec = interpolatedsectors[i];
        sec->renderfloorheight   = sec->floorheight;
        sec->renderceilingheight = sec->ceilingheight;
        sec->floorheight = sec->oldfloorheight
            + FixedMul(sec->floorheight - sec->oldfloorheight, fractionaltic);
        sec->ceilingheight = sec->oldceilingheight
            + FixedMul(sec->ceilingheight - sec->oldceilingheight, fractionaltic);
    }
}

void D_InterpEnd(void)
{
    if (!r_interpolate) return;
    for (int i = 0; i < numinterpolatedsectors; i++) {
        sector_t *sec = interpolatedsectors[i];
        sec->floorheight   = sec->renderfloorheight;
        sec->ceilingheight = sec->renderceilingheight;
    }
}

void D_PlayerView(float *x, float *y, float *z, float *angle)
{
    const player_t *p  = &players[0];
    const mobj_t   *mo = p->mo;
    if (!mo) return;

    fixed_t vx = mo->x, vy = mo->y, vz = p->viewz;
    angle_t va = mo->angle;

    if (r_interpolate) {
        vx = mo->oldx + FixedMul(mo->x - mo->oldx, fractionaltic);
        vy = mo->oldy + FixedMul(mo->y - mo->oldy, fractionaltic);
        vz = p->oldviewz + FixedMul(p->viewz - p->oldviewz, fractionaltic);

        /* Unsigned subtraction then signed cast takes the short arc
         * (359->1 degrees is +2, not -358); the multiply needs 64 bits
         * because a half-turn delta times FRACUNIT overflows 32. */
        const int32_t adiff = (int32_t)(mo->angle - mo->oldangle);
        va = mo->oldangle
           + (angle_t)(((int64_t)adiff * fractionaltic) >> FRACBITS);
    }

    *x = (float)vx / 65536.0f;
    *y = (float)vy / 65536.0f;
    *z = (float)vz / 65536.0f;
    *angle = (float)va * ((float)M_PI / 2147483648.0f);
}

boolean D_PlayerAlive(void) { return players[0].mo != NULL; }

int D_PlayerHealth(void) { return players[0].health; }
int D_PlayerAmmo(void)
{
    const player_t *p = &players[0];
    const ammotype_t a = weaponinfo[p->readyweapon].ammo;
    return a == am_noammo ? -1 : p->ammo[a];
}

/* Dehacked can retune the BFG's cost per shot. There is no dehacked here, so
 * this stays at the value the weapon was designed around. */
int deh_bfg_cells_per_shot = 40;

/* Dehacked-tunable gameplay constants. Doom exposes these so a patch can
 * rebalance pickups; without dehacked they stay at the shipped values. */
int deh_max_health       = 100;
int deh_initial_health   = 100;
int deh_initial_bullets  = 50;
int deh_max_armor        = 200;
int deh_green_armor_class = 1;
int deh_blue_armor_class  = 2;
int deh_max_soulsphere   = 200;
int deh_soulsphere_health = 100;
int deh_megasphere_health = 200;

/* Dehacked string substitution. Without a patch loaded every string passes
 * through unchanged, which is what the pickup messages expect. */
const char *DEH_String(const char *s) { return s; }

/* Damaging floors, secret sectors and the end-of-level trigger all live in
 * p_spec.c, which is Stage 5. */

/* The automap is not ported. */

/* --- player weapon sprites -------------------------------------------- */

/* Hand one of the player's weapon sprites to the renderer.
 *
 * Doom positions these in a 320x200 space whose bottom 32 rows are the status
 * bar, with the sprite's own offsets applied: x is psp->sx less the sprite's
 * left offset, y is psp->sy less its top offset. Returns 0 when that slot is
 * empty -- the muzzle flash occupies its own slot and is absent most of the
 * time.
 */
int D_PSpriteGet(int i, void **tex, int *x, int *y)
{
    if (i < 0 || i >= NUMPSPRITES) return 0;

    const pspdef_t *psp = &players[0].psprites[i];
    if (!psp->state) return 0;

    void *R_SpriteFrame(int sprite, int frame, int rot);
    void *t = R_SpriteFrame(psp->state->sprite, psp->state->frame, 0);
    if (!t) return 0;

    const dt64_tex_t *dt = (const dt64_tex_t *)t;
    *tex = t;
    fixed_t sx = psp->sx, sy = psp->sy;
    if (r_interpolate) {
        sx = psp->oldsx + FixedMul(psp->sx - psp->oldsx, fractionaltic);
        sy = psp->oldsy + FixedMul(psp->sy - psp->oldsy, fractionaltic);
    }
    *x = (sx >> FRACBITS) - dt->leftoffset;
    *y = (sy >> FRACBITS) - dt->topoffset;
    return 1;
}

/* --- texture animation tables ----------------------------------------- */

/* Doom animates textures by indirection: every reference goes through
 * texturetranslation[], and P_InitPicAnims advances the entries of an animated
 * set once a tic so a switch flips or nukage flows without touching geometry.
 *
 * The renderer here resolves textures by name into its own table at load, so
 * it does not read these. They exist because p_spec.c writes them, and they
 * are identity-initialised so a lookup through them is harmless.
 *
 * The cost is that animated flats do not animate yet. Wiring it up means
 * having the renderer index through texturetranslation[] instead of holding a
 * resolved pointer -- worth doing, and not required to make the level work.
 */
int     *texturetranslation;
int     *flattranslation;
fixed_t *textureheight;
int      numflats;

void D_InitPicTables(int count)
{
    numflats = count;

    texturetranslation = Z_Malloc((count + 1) * sizeof *texturetranslation,
                                  PU_STATIC, NULL);
    flattranslation    = Z_Malloc((count + 1) * sizeof *flattranslation,
                                  PU_STATIC, NULL);
    textureheight      = Z_Malloc((count + 1) * sizeof *textureheight,
                                  PU_STATIC, NULL);

    for (int i = 0; i <= count; i++) {
        texturetranslation[i] = i;
        flattranslation[i]    = i;
        textureheight[i]      = 128 * FRACUNIT;
    }
}


/* --- level progression ------------------------------------------------- */

/* Doom's sky is per-episode, not per-level: SKY1 for Knee-Deep in the Dead,
 * SKY2 for The Shores of Hell, SKY3 for Inferno. */
void D_SetSky(int episode)
{
    void r_sky_set_raw(void *tex);
    char name[8];

    /* Doom picks the backdrop by episode; Doom II has one episode of thirty-two
     * maps and picks by map instead, in the same three bands R_SetupLevel
     * uses -- city through 11, hell-adjacent through 20, hell after that. */
    int n;
    if (gamemode == commercial)
        n = gamemap < 12 ? 1 : gamemap < 21 ? 2 : 3;
    else
        n = episode < 1 ? 1 : episode > 3 ? 3 : episode;

    name[0] = 'S'; name[1] = 'K'; name[2] = 'Y';
    name[3] = (char)('0' + n);
    name[4] = '\0';
    r_sky_set_raw(p_level_resolve_ptr(name, ""));
}


/* Palette effects: the red flash when hurt, the gold one on a pickup, the
 * inverted tint under invulnerability.
 *
 * Doom implements these by swapping the display palette -- the framebuffer is
 * paletted, so recolouring the whole screen costs one 768-byte upload. The
 * same trick applies here almost unchanged, because every texture is CI8
 * against a shared TLUT: a flash is a 512-byte TLUT swap rather than anything
 * per-pixel.
 *
 * Not wired up yet. The build tool converts only PLAYPAL's first palette, so
 * there is nothing to switch to; emitting all fourteen and selecting between
 * them here is the remaining work, and it is small.
 */
void I_SetPalette(byte *palette)
{
    /* ST_doPaletteStuff hands a pointer into the PLAYPAL lump; the offset
     * from its base is the palette index. The baked TLUT carries all 14
     * banks, so a flash is a 512-byte selection, exactly as the load-time
     * design intended. */
    const byte *base = W_CacheLumpName("PLAYPAL", PU_STATIC);
    int bank = (int)((palette - base) / 768);
    dt64_set_palette(bank);
}

/* --- the chocolate game shell ------------------------------------------ */

/* d_loop.c owns this in chocolate: the per-player commands for the tic being
 * run. Single console, no netplay -- one live slot, filled from the local
 * command just before each G_Ticker. */
static ticcmd_t netcmds_store[MAXPLAYERS];
ticcmd_t *netcmds = netcmds_store;
static ticcmd_t localcmd;
ticcmd_t *d_localcmd(void) { return &localcmd; }

/* LEVELTEST's flow driver: synthesize attack presses so intermission and
 * finale acceleration advance without a controller. */
static int test_fire_hold;
void D_TestPressFire(int on) { test_fire_hold = on; }

void D_TicStart(void)
{
    netcmds[0] = localcmd;

    /* Whatever the menu has asked for -- save, load, pause -- overrides the
     * buttons, exactly as G_BuildTiccmd does it on the PC.
     *
     * Here rather than in the per-frame input build, because this runs once
     * per tic and the request must be consumed by exactly one. Folded in per
     * frame it was published to every tic that frame covered: two tics in one
     * frame ran the save twice, and Doom's handler renames the slot "NET GAME"
     * on the second pass, because G_DoSaveGame clears the description once it
     * has written it. A frame covering no tic at all would have lost it. */
    { void G_PutPendingSpecials(ticcmd_t *cmd);
      G_PutPendingSpecials(&netcmds[0]); }

    /* One queued weapon step per tic. G_NextWeapon starts from
     * pendingweapon when a switch is already animating, so consecutive
     * taps walk the weapon list instead of fighting over one slot. */
    if (!(netcmds[0].buttons & BT_CHANGE) && gamestate == GS_LEVEL && !menuactive) {
        int G_PopWeaponChange(void);
        const int w = G_PopWeaponChange();
        if (w >= 0) {
            netcmds[0].buttons |= BT_CHANGE;
            netcmds[0].buttons |= w << BT_WEAPONSHIFT;
        }
    }
    /* The flow driver's synthetic press must be applied here: the per-frame
     * input rebuild memsets localcmd, so a flag set outside the tic loop
     * would never survive into a command otherwise. */
    if (test_fire_hold)
        netcmds[0].buttons |= BT_ATTACK;
}

/* True only when a level's data is resident AND the game is in it. The
 * gamestate alone is not enough: it is zero (GS_LEVEL) before the first
 * tic ever runs, and the renderer must not walk an empty BSP. */
static boolean d_level_resident;
boolean d_level_resident_set(boolean v) { return d_level_resident = v; }
int D_InLevel(void) { return gamestate == GS_LEVEL && d_level_resident; }

int D_FlowState(int *ga) { *ga = (int)gameaction; return (int)gamestate; }


/* Title/demo attract cycle, transcribed from d_main.c. The page flips and
 * demo lumps alternate exactly as on the PC: TITLEPIC, DEMO1, CREDIT, DEMO2,
 * HELP2 (shareware's second page), DEMO3, round again. */
boolean advancedemo;
static int  demosequence = -1;
static int  pagetic;
const char *pagename = "TITLEPIC";

void D_AdvanceDemo(void) { advancedemo = true; }

void D_DoAdvanceDemo(void)
{
    players[consoleplayer].playerstate = PST_LIVE;  /* not reborn */
    advancedemo = false;
    usergame = false;               /* no save/end game here */
    paused = false;
    gameaction = ga_nothing;

    demosequence = (demosequence + 1) % 6;

    switch (demosequence) {
    case 0:
        pagetic = 170;
        gamestate = GS_DEMOSCREEN;
        pagename = "TITLEPIC";
        break;
    case 1:
        G_DeferedPlayDemo("demo1");
        break;
    case 2:
        pagetic = 200;
        gamestate = GS_DEMOSCREEN;
        pagename = "CREDIT";
        break;
    case 3:
        G_DeferedPlayDemo("demo2");
        break;
    case 4:
        pagetic = 200;
        gamestate = GS_DEMOSCREEN;
        pagename = gamemode == shareware ? "HELP2" : "CREDIT";
        break;
    case 5:
        G_DeferedPlayDemo("demo3");
        break;
    }
}

void D_PageTicker(void)
{
    if (--pagetic < 0)
        D_AdvanceDemo();
}

void D_PageDrawer(void)
{
    V_DrawPatch(0, 0, W_CacheLumpName(pagename, PU_CACHE));
}

void D_StartTitle(void)
{
    gameaction = ga_nothing;
    demosequence = -1;
    /* Synchronously: the first frame renders before the first tic runs,
     * and it must find GS_DEMOSCREEN, not the zero-initialised GS_LEVEL. */
    D_DoAdvanceDemo();
}

/* Software-renderer hooks G_ code calls around view changes; the RDP
 * renderer has no view-size machinery or border to redraw. */
void R_ExecuteSetViewSize(void)  { }
void R_FillBackScreen(void)      { }
void R_UpdateSegRenderData(void) { }

/* Strict-vanilla demo arbitration from chocolate's d_loop: with no command
 * line there is no -strict, so a feature is allowed iff its condition is. */
boolean D_NonVanillaRecord(boolean conditional, const char *feature)
{
    (void)feature; return conditional;
}
boolean D_NonVanillaPlayback(boolean conditional, int lumpnum,
                             const char *feature)
{
    (void)lumpnum; (void)feature; return conditional;
}

/* --- controls, cheats and shell globals chocolate expects --------------- */

/* m_controls.c equivalents. The controller mapping happens in d_ui.c; these
 * exist so the vendored responders compile, bound to values d_ui can send. */
int key_pause           = 'p';
int key_demo_quit       = 'q';
int key_message_refresh = KEY_ENTER;
int key_multi_msg       = 't';
int key_multi_msgplayer[8];
int key_nextweapon      = 0;
int key_prevweapon      = 0;
int key_spy             = KEY_F12;
int joybnextweapon      = -1;
int joybprevweapon      = -1;
int mousebnextweapon    = -1;
int mousebprevweapon    = -1;

/* Dehacked-tunable cheat rewards, at their shipped values. */
int deh_god_mode_health   = 100;
int deh_idfa_armor        = 200;
int deh_idfa_armor_class  = 2;
int deh_idkfa_armor       = 200;
int deh_idkfa_armor_class = 2;

/* The wipe machinery is not ported (the melt needs CPU access to both
 * frames, which the pipelined RDP path deliberately never has). g_game
 * still tracks the variable to force redraws; nothing reads it here. */
gamestate_t wipegamestate = GS_DEMOSCREEN;
boolean modifiedgame = false;
/* Screen-size machinery from the software renderer; loading a game asks
 * for a view resize it never needs here. */
boolean setsizeneeded = false;
int ticdup = 1;
void StatCopy(wbstartstruct_t *stats) { (void)stats; }
/* Nonzero means "canaries intact". This build plants none, so there is
 * nothing that can trip; returning 0 here reads as corruption and turns
 * every Z_CheckHeap into an instant I_Error. */
int I_CheckZoneCanaries(void) { return 1; }

/* Automap bindings and the draw hook. am_map.c is the real chocolate
 * automap now; its lines render through r_am.c. */
int joybautomap       = -1;
int key_map_toggle    = KEY_TAB;
int key_map_east      = KEY_RIGHTARROW;
int key_map_west      = KEY_LEFTARROW;
int key_map_north     = KEY_UPARROW;
int key_map_south     = KEY_DOWNARROW;
int key_map_zoomin    = '=';
int key_map_zoomout   = '-';
int key_map_maxzoom   = '0';
int key_map_follow    = 'f';
int key_map_grid      = 'g';
int key_map_mark      = 'm';
int key_map_clearmark = 'c';

int D_AutomapActive(void) { return automapactive; }

/* LEVELTEST's flow driver: toggle the map through the real responder. */
void D_TestToggleMap(void)
{
    boolean G_Responder(event_t *ev);
    event_t ev;
    ev.type = ev_keydown; ev.data1 = key_map_toggle; ev.data2 = ev.data3 = 0;
    G_Responder(&ev);
    ev.type = ev_keyup;
    G_Responder(&ev);
}

void D_AutomapDraw(void)
{
    void r_am_begin(void);
    void AM_Drawer(void);
    if (!automapactive || gamestate != GS_LEVEL) return;
    r_am_begin();
    AM_Drawer();
}

/* The software renderer's sprite tables, referenced by the Doom II cast
 * call. Shareware never reaches it; these exist for the linker. */
spritedef_t *sprites;
int firstspritelump;

/* G_InitNew resolves the sky texture number for the software renderer;
 * this port picks its sky in D_SetSky from the episode instead. */
int skytexture;

/* UMAPINFO is the fork's WAD-driven flow override; without the lumps the
 * queries all answer "not present" and the vanilla flow runs. */
void UMAPINFO_LoadLumps(void) { }
const umapinfo_map_t *UMAPINFO_GetMap(int episode, int map)
{ (void)episode; (void)map; return NULL; }
const umapinfo_episode_t *UMAPINFO_GetEpisode(int episode)
{ (void)episode; return NULL; }
boolean UMAPINFO_HasEpisode(int episode) { (void)episode; return false; }
int UMAPINFO_MaxEpisode(void) { return 0; }
boolean UMAPINFO_ResolveMap(const char *marker, int *episode, int *map)
{ (void)marker; (void)episode; (void)map; return false; }
boolean UMAPINFO_GetNextMap(int episode, int map, boolean secret,
                            int *next_episode, int *next_map)
{ (void)episode; (void)map; (void)secret;
  (void)next_episode; (void)next_map; return false; }
const char *UMAPINFO_LevelName(int episode, int map)
{ (void)episode; (void)map; return NULL; }
const char *UMAPINFO_LevelPic(int episode, int map)
{ (void)episode; (void)map; return NULL; }
const char *UMAPINFO_Music(int episode, int map)
{ (void)episode; (void)map; return NULL; }
const char *UMAPINFO_SkyTexture(int episode, int map)
{ (void)episode; (void)map; return NULL; }
const char *UMAPINFO_ExitPic(int episode, int map)
{ (void)episode; (void)map; return NULL; }
const char *UMAPINFO_EndPic(int episode, int map)
{ (void)episode; (void)map; return NULL; }
const char *UMAPINFO_InterText(int episode, int map)
{ (void)episode; (void)map; return NULL; }
int UMAPINFO_ParTime(int episode, int map)
{ (void)episode; (void)map; return -1; }

/* Music control. The card carries a track per state alongside the level
 * tracks: PINTRO for the title page, PINTER for the intermission, PVICTOR
 * for the finale text, PBUNNY for the episode 3 scroller. Doom names music
 * by musicenum; levels resolve to their episode/map track, everything else
 * to those four. Unknown numbers keep whatever is playing. */
void S_ChangeMusic(int musicnum, int looping)
{
    void mus_play_track(const char *track);
    (void)looping;                 /* the card player always loops */

    if (musicnum <= mus_None || musicnum >= NUMMUSIC) return;

    /* The track's filename is its Doom lump name with the D_ swapped for a P
     * -- D_RUNNIN becomes PRUNNIN -- and S_music already holds those names,
     * so one rule covers both games. This used to be a chain of comparisons
     * against the Doom music numbers, which named the four non-level tracks
     * literally and would have needed thirty-five more for Doom II. */
    const char *n = S_music[musicnum].name;
    char track[9];
    int k = 0;
    track[k++] = 'P';
    while (k < 8 && n[k - 1]) {
        const char c = n[k - 1];
        track[k] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        k++;
    }
    track[k] = '\0';
    mus_play_track(track);
}
void S_StartMusic(int musicnum) { S_ChangeMusic(musicnum, true); }
void S_PauseSound(void)  { }
void S_ResumeSound(void) { }

/* --- what the menu expects of the rest of Doom ------------------------- */

/* Menu key and button bindings. Doom makes these configurable; here the
 * controller is mapped in d_ui.c, so these only have to exist and be
 * consistent with what that sends. */
int key_menu_activate  = KEY_ESCAPE;
int key_menu_back      = KEY_BACKSPACE;
int key_menu_abort     = KEY_ESCAPE;
int key_menu_confirm   = 'y';
int key_menu_forward   = KEY_ENTER;
int key_menu_up        = KEY_UPARROW;
int key_menu_down      = KEY_DOWNARROW;
int key_menu_left      = KEY_LEFTARROW;
int key_menu_right     = KEY_RIGHTARROW;
int key_menu_help      = KEY_F1;
int key_menu_save      = KEY_F2;
int key_menu_load      = KEY_F3;
int key_menu_volume    = KEY_F4;
int key_menu_detail    = KEY_F5;
int key_menu_qsave     = KEY_F6;
int key_menu_endgame   = KEY_F7;
int key_menu_messages  = KEY_F8;
int key_menu_qload     = KEY_F9;
int key_menu_quit      = KEY_F10;
int key_menu_gamma     = KEY_F11;
int key_menu_incscreen = KEY_EQUALS;
int key_menu_decscreen = KEY_MINUS;
int key_menu_screenshot = 0;

int joybfire = 0, joybuse = 1, joybmenu = -1, joybstrafe = 2, joybspeed = 3;

/* Whether the game is at a level, the intermission or the finale. Nothing here
 * leaves GS_LEVEL: there is no title screen or intermission yet. */

boolean devparm = false;

/* The quit prompt picks one of these at random. Doom ships a list of its own
 * jokes; this port does not reproduce them, and a plain prompt does the job. */
const char *doom1_endmsg[] = { "are you sure you want to quit?" };
const char *doom2_endmsg[] = { "are you sure you want to quit?" };

/* The menu measures its own animation in tics. libdragon's timer counts at the
 * CPU's rate, so this converts to Doom's 35 Hz. */
int I_GetTime(void)
{
    return (int)(timer_ticks() / (TICKS_PER_SECOND / TICRATE));
}

void I_WaitVBL(int count) { (void)count; }

/* Text entry for savegame names, and the OPL synth's diagnostics. Neither
 * applies: there is no keyboard and no OPL emulation. */
void I_StartTextInput(int x1, int y1, int x2, int y2)
{
    (void)x1; (void)y1; (void)x2; (void)y2;
}
void I_StopTextInput(void) { }
void I_OPL_DevMessages(char *buf, size_t len) { if (len) buf[0] = 0; }

/* Volume levels and view size are configuration the menu edits. There is no
 * config file to persist them to, so they live here at their defaults. */
int musicVolume = 8, sfxVolume = 8, usegamma = 0;
unsigned int joywait = 0;

char *savegamedir = "";

void S_SetMusicVolume(int volume)
{
    void mus_set_volume(int volume);
    mus_set_volume(volume);
}
/* S_SetSfxVolume lives in i_sound_n64.c with the channel state. */

/* Chocolate updates sound placement once per tic from the game loop; the
 * listener is the console player's actor. */
void D_SoundTic(void)
{
    void S_UpdateSounds(mobj_t *listener);
    if (players[consoleplayer].mo)
        S_UpdateSounds(players[consoleplayer].mo);
}

/* The view is always the full screen here: there is no status bar inset to
 * shrink it into, and the renderer has no scaled modes. */
void R_SetViewSize(int blocks, int detail) { (void)blocks; (void)detail; }

/* Doom's video layer keeps a back buffer it can copy regions of, so a menu
 * can be drawn over a frozen frame and lifted off again. The RDP redraws
 * every frame from scratch, so the copy itself has nothing to do --
 * V_UseBuffer/V_RestoreBuffer live in v_draw.c, where they place the
 * status-bar background at its real destination instead of a buffer. */
void V_CopyRect(int srcx, int srcy, pixel_t *source, int width, int height,
                int destx, int desty)
{
    (void)srcx; (void)srcy; (void)source;
    (void)width; (void)height; (void)destx; (void)desty;
}

/* Which WAD a lump came from -- only meaningful with PWADs loaded over an
 * IWAD, which a cartridge does not do. */
boolean W_IsIWADLump(const lumpinfo_t *lump) { (void)lump; return true; }

/* Set when a game is in progress, so the menu knows whether to offer "end
 * game" and whether Escape should pause. This port is always in a level. */

/* Doom can remap keys for non-US keyboards. There is no keyboard. */
boolean vanilla_keyboard_mapping = true;

/* Command-line arguments. A cartridge is launched with none, so every query
 * reports absent -- which is also what gives the game its default behaviour
 * rather than any of the developer modes these flags select. */
int M_CheckParm(const char *check) { (void)check; return 0; }
int M_CheckParmWithArgs(const char *check, int num_args)
{
    (void)check; (void)num_args;
    return 0;
}

/* Which file a lump came from, for the "modified game" notice. One WAD here. */
const char *W_WadNameForLump(const lumpinfo_t *lump)
{
    (void)lump;
    return "doom.wad";
}
