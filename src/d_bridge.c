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

#include "r_light.h"
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
#if R_DOORMIRROR
/* Polished metal walls -- the grey doors and panels. Recorded by
 * resolved index, as the emissive flats are, because the name is gone by
 * the time anything draws: the loader resolves once and the renderer only
 * ever sees the index. A level uses a handful, so the lookup is a scan of
 * a table that is nearly always length 0 to 3. */
#define MIRROR_MAX 12
static int16_t mirror_pic[MIRROR_MAX];
static int     num_mirror;

int D_TextureIsMirror(int picnum)
{
    for (int i = 0; i < num_mirror; i++)
        if (mirror_pic[i] == (int16_t)picnum) return 1;
    return 0;
}

void D_MirrorReset(void) { num_mirror = 0; }

static int mirror_for_name(const char *n)
{
    /* Exact names, not prefixes: DOORTRAK and DOORSTOP are the dirty
     * track and jamb around a door, not the polished leaf, and BIGDOOR3
     * is rusted brown. */
    static const char *const shiny[] = {
        "DOOR1", "DOOR3", "BIGDOOR2", "BIGDOOR4", "SHAWN2", "METAL1"
    };
    /* Compared over eight bytes, not as C strings: Doom's sidedef name
     * fields are char[8] and carry NO terminator when the name fills
     * them -- BIGDOOR2 is exactly eight. strcasecmp ran off the end of
     * the field and never matched the very textures most worth
     * mirroring. */
    char nm[9];
    for (int i = 0; i < 8; i++) nm[i] = n[i] ? n[i] : '\0';
    nm[8] = '\0';
    for (unsigned i = 0; i < sizeof shiny / sizeof shiny[0]; i++)
        if (!strncasecmp(nm, shiny[i], 8) &&
            strlen(shiny[i]) == strlen(nm)) return 1;
    return 0;
}
#endif

int R_TextureNumForName(const char *name)
{
    const int idx = p_level_resolve(name, "");
#if R_DOORMIRROR
    if (idx >= 0 && num_mirror < MIRROR_MAX && mirror_for_name(name)) {
        for (int i = 0; i < num_mirror; i++)
            if (mirror_pic[i] == (int16_t)idx) return idx;
        mirror_pic[num_mirror++] = (int16_t)idx;
    }
#endif
    return idx;
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

/* --- emissive flats ---------------------------------------------------- */

/* Doom's sludge, lava and blood read as light sources but are lit like any
 * other floor: a nukage pool in a dim room comes out as dark as the concrete
 * around it. Nothing in the WAD marks them, so they are recognised by name --
 * the same way Doom itself recognises F_SKY1.
 *
 * Recorded by resolved index rather than by name, because the name is gone by
 * the time anything draws: the loader resolves flats to indices once and the
 * renderer only ever sees the index. A level uses one or two of these, so the
 * lookup is a scan of a table that is nearly always length 0, 1 or 2.
 */
#if D_DYNLIGHT
/* Storage for the inline queries in d_glow.h; the writers below own it. */
#include "d_glow.h"
#include "d_rumble.h"
#define GLOW_MAX D_GLOW_MAX
int16_t d_glow_pic[D_GLOW_MAX];
float   d_glow_amt[D_GLOW_MAX];
float   d_glow_rgb[D_GLOW_MAX][3];      /* light colour, from the art */
uint8_t d_glow_have_rgb[D_GLOW_MAX];
uint8_t d_glow_class[D_GLOW_MAX];
uint8_t d_glow_reflect[D_GLOW_MAX];
int     d_num_glow;
#define glow_pic      d_glow_pic
#define glow_amt      d_glow_amt
#define glow_rgb      d_glow_rgb
#define glow_have_rgb d_glow_have_rgb
#define num_glow      d_num_glow

dt64_tex_t *p_level_texture(int index);

/* The light's colour, averaged from the flat's own texels.
 *
 * Hand-picked tints would be a guess about art I cannot see; the average of
 * what the surface actually shows is the same information the player is
 * looking at. Computed once per flat per level, off the first frame that draws
 * it -- the texture is demand-loaded, so it does not exist when the name is
 * resolved.
 *
 * NORMALISED so the brightest channel is full, which is the point worth being
 * careful about: the tint multiplies an intensity, so what is wanted is the
 * HUE and not the brightness. Averaged raw, a dark blood flat would come out
 * near-black and dim the very light it is supposed to be casting. Scaling to
 * the brightest channel keeps green nukage green and red blood red while
 * leaving the intensity to glow_amt, where it belongs. */
void d_glow_bake_rgb(int slot, int picnum)
{
    glow_have_rgb[slot] = 1;
    glow_rgb[slot][0] = glow_rgb[slot][1] = glow_rgb[slot][2] = 1.0f;

    const dt64_tex_t *t = p_level_texture(picnum);
    if (!t || !t->texels || t->width <= 0 || t->height <= 0) return;

    uint32_t ar = 0, ag = 0, ab = 0;
    const int n = t->width * t->height;
    for (int i = 0; i < n; i++) {
        const uint16_t c = dt64_tlut_color(t->texels[i]);   /* RGBA5551 */
        ar += (c >> 11) & 31;
        ag += (c >>  6) & 31;
        ab += (c >>  1) & 31;
    }

    const uint32_t mx = ar > ag ? (ar > ab ? ar : ab) : (ag > ab ? ag : ab);
    if (!mx) return;                       /* pure black flat: leave it white */
    glow_rgb[slot][0] = (float)ar / (float)mx;
    glow_rgb[slot][1] = (float)ag / (float)mx;
    glow_rgb[slot][2] = (float)ab / (float)mx;
}

/* D_FlatGlow / D_FlatGlowRGB are inline in d_glow.h now -- the queries sit
 * in the BSP walk and the registry is a handful of entries. */

void D_FlatGlowReset(void)
{
    num_glow = 0;
    for (int i = 0; i < GLOW_MAX; i++) glow_have_rgb[i] = 0;
}


static float glow_for_name(const char *n, uint8_t *cls)
{
    /* Prefix matches: the WAD numbers the animation frames (NUKAGE1..3,
     * LAVA1..4), and every frame of one animation glows the same. */
    if (!strncasecmp(n, "NUKAGE", 6)) { *cls = D_GLOW_NUKAGE; return 0.55f; }
    if (!strncasecmp(n, "LAVA",   4)) { *cls = D_GLOW_LAVA;   return 0.70f; }
    if (!strncasecmp(n, "SLIME",  5)) { *cls = D_GLOW_SLIME;  return 0.45f; }
    if (!strncasecmp(n, "BLOOD",  5)) { *cls = D_GLOW_BLOOD;  return 0.30f; }
    if (!strncasecmp(n, "FWATER", 6)) { *cls = D_GLOW_WATER;  return 0.25f; }
    *cls = D_GLOW_NONE;
    return 0.0f;
}

/* Polished surfaces that mirror without glowing: no vapor, no light spill,
 * no self-illumination -- registered with glow 0, which nulls every glow
 * consumer -- but things and walls over them queue reflections, tinted by
 * the surface's own baked colour like the liquids are. The list is the
 * shiny art: teleporter pads, the temples' green marble, and the polished
 * blue and silver tech plates. Exact names where a prefix would catch
 * matte cousins (FLAT2, FLAT17..19, DEM1_1..4). */
static int reflect_for_name(const char *n)
{
    if (!strncasecmp(n, "GATE",   4)) return 1;   /* GATE1..4  */
    if (!strncasecmp(n, "CEIL4_", 6)) return 1;   /* CEIL4_1..3 */
    if (!strcasecmp (n, "DEM1_5"))    return 1;
    if (!strcasecmp (n, "DEM1_6"))    return 1;
    if (!strcasecmp (n, "FLAT14"))    return 1;
    if (!strcasecmp (n, "FLAT22"))    return 1;
    if (!strcasecmp (n, "FLAT23"))    return 1;
    return 0;
}
#endif /* D_DYNLIGHT */

int R_FlatNumForName(const char *name)
{
    /* Flats are converted to their own prefixed set on the host: they are
     * 64x64 in the WAD but downsampled here, and share no namespace with wall
     * textures despite Doom treating both as "pics". */
    const int idx = p_level_resolve(name, "f_");

#if D_DYNLIGHT
    uint8_t cls;
    const float g   = glow_for_name(name, &cls);
    const int   rfl = g > 0.0f ? 1 : reflect_for_name(name);
    if ((g > 0.0f || rfl) && idx >= 0 && num_glow < GLOW_MAX) {
        bool seen = false;
        for (int i = 0; i < num_glow; i++)
            if (glow_pic[i] == (int16_t)idx) { seen = true; break; }
        if (!seen) {
            glow_pic[num_glow]       = (int16_t)idx;
            glow_amt[num_glow]       = g;
            d_glow_class[num_glow]   = cls;
            d_glow_reflect[num_glow] = (uint8_t)rfl;
            num_glow++;
        }
    }
#endif
    return idx;
}

int R_CheckTextureNumForName(const char *name)
{
    return p_level_resolve(name, "");
}

#if R_HALO
/* Which sectors are light WELLS: a sky ceiling that is a hole in a roof,
 * rather than part of the open air.
 *
 * Span alone cannot tell them apart. E1M2 has a small sky sector sitting
 * inside a large one -- a raised platform in an open courtyard -- and by
 * size it looks exactly like a chimney. The difference is what surrounds
 * it: a well's neighbours are roofed rooms, open sky's neighbours are
 * more sky. So a sector qualifies only if no two-sided line joins it to
 * another sky-ceilinged sector.
 *
 * One pass over the linedefs at load, and the frame just reads a byte. */
static uint8_t *sky_well;

int D_SectorIsSkyWell(int secnum)
{
    return sky_well && secnum >= 0 && secnum < numsectors && sky_well[secnum];
}

void D_BuildSkyWells(void)
{
    sky_well = Z_Malloc(numsectors, PU_LEVEL, NULL);
    if (!sky_well) return;

    for (int i = 0; i < numsectors; i++)
        sky_well[i] = sectors[i].ceilingpic == skyflatnum;

    for (int i = 0; i < numlines; i++) {
        const line_t *ld = &lines[i];
        if (!ld->frontsector || !ld->backsector) continue;
        if (ld->frontsector->ceilingpic != skyflatnum) continue;
        if (ld->backsector->ceilingpic  != skyflatnum) continue;
        /* Sky on both sides: neither is a hole in anything. */
        sky_well[ld->frontsector - sectors] = 0;
        sky_well[ld->backsector  - sectors] = 0;
    }
}
#endif

#if D_KEYLIGHT
/* Rooms lit by a standing light are dimmed, so the lamp does the lighting.
 *
 * A torch in vanilla is decoration standing in a room the mapper already
 * lit to taste. Give it a real light and the room is lit twice: once by
 * the sector level that was standing in for the flame, and again by the
 * flame. Everything near a lamp washed out. Taking 30% off the sector
 * level where a lamp stands hands that job to the light itself -- the
 * pool around the flame comes back brighter than the ambient it replaced,
 * and the corners the flame does not reach fall away, which is the whole
 * point of putting a torch there.
 *
 * Marked once at load from the thing list, so the frame reads a byte. */
static uint8_t *lamp_sector;

int D_SectorHasLamp(int secnum)
{
    return lamp_sector && secnum >= 0 && secnum < numsectors
        && lamp_sector[secnum];
}

void D_BuildLampSectors(void)
{
    lamp_sector = Z_Malloc(numsectors, PU_LEVEL, NULL);
    if (!lamp_sector) return;
    memset(lamp_sector, 0, numsectors);

    for (int i = 0; i < numsectors; i++) {
        for (const mobj_t *mo = sectors[i].thinglist; mo; mo = mo->snext) {
            switch (mo->type) {
                case MT_MISC41: case MT_MISC42: case MT_MISC43:
                case MT_MISC44: case MT_MISC45: case MT_MISC46:
                case MT_MISC29: case MT_MISC31:
                case MT_MISC49: case MT_MISC50:
                    lamp_sector[i] = 1;
                    break;
                default: break;
            }
            if (lamp_sector[i]) break;
        }
    }
}
#endif

/* --- renderer: load-time precomputation the RDP does not need ---------- */

/* Doom builds per-seg and per-sector render caches at load because its
 * software renderer walks them every frame. This port has its own version
 * of the same idea: the per-seg float mirror the BSP walk reads instead of
 * chasing vertex/sector pointers and reconverting fixed-point every frame.
 * P_LoadSegs calls this at the exact moment segs/vertexes/sides/lines are
 * all resolved. */
void R_BuildSegRenderData(void)
{
    void R_BuildSegFloatMirror(void);
    R_BuildSegFloatMirror();
}
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

/* Stamped by main.c before every G_Ticker; only a P_Ticker that actually
 * ran advances leveltime past it. So leveltime > oldleveltime is precisely
 * "the world moved since the last tic attempt" -- false while the menu or
 * pause holds P_Ticker off. */
int oldleveltime = 0;

/* fractionaltic as a plain float in [0,1), zero whenever interpolation is
 * off -- for renderer consumers (vapor) that animate on leveltime and
 * should glide with the world without pulling Doom's types in. */
float d_subtic = 0.0f;

void D_SetTicFrac(uint32_t into_tic_us)
{
#if D_INTERP
    if (into_tic_us > 28570u) into_tic_us = 28570u;  /* hold at the pair */
    fractionaltic = (fixed_t)(((uint64_t)into_tic_us << FRACBITS) / 28571u);
    /* Interpolate only while the simulation advances. When P_Ticker
     * early-returns -- menu open in single player, pause -- the (old,
     * current) pairs freeze mid-move but the fraction would keep cycling
     * at 35 Hz, and every mid-move door and monster sawtooths between its
     * last two positions behind the menu. Presenting current state,
     * uninterpolated, is exact for a world that is not moving. */
    r_interpolate = (leveltime > oldleveltime);
    d_subtic = r_interpolate
             ? (float)fractionaltic * (1.0f / (float)FRACUNIT) : 0.0f;
#else
    (void)into_tic_us;
    fractionaltic  = 0;
    r_interpolate  = false;
    d_subtic       = 0.0f;
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
    /* Re-arm the node height ranges for every sector whose PRESENTED
     * height this frame lerped: the next frame presents a different
     * sub-tic height, and when the mover settles the last lerped frame's
     * mark is what buys the final refresh at true resting heights. Without
     * this, interp frames rendered with sub-tic-stale cull ranges and the
     * settle frame stayed one tic of travel stale -- the flag-only design
     * had exactly that hole. */
    {
        extern void R_NodeZMarkSector(int);
        for (int i = 0; i < numinterpolatedsectors; i++)
            R_NodeZMarkSector((int)(interpolatedsectors[i] - sectors));
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
 * Wired end to end: the build tool emits all fourteen palettes, dt64 keeps
 * them as selectable banks (rebuilt from the runtime WAD's PLAYPAL at boot,
 * so EXTWAD IWAD swaps flash with their own colours), and ST_doPaletteStuff
 * drives the selection below every frame. A flash is a 512-byte bank copy
 * plus the TLUT upload the frame was paying anyway.
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

/* Collect this frame's moving lights. Called from the frame loop before the
 * BSP walk, because the walk is what emits the geometry that queries them.
 *
 * Doom has no light entities, so the sources are inferred from what the world
 * already contains: the weapon's flash psprite, and the projectile mobjs that
 * are lit objects by their nature. Nothing is added to the game state and
 * nothing here can affect it -- this reads the simulation and writes only to
 * the renderer's list, so a build with DYNLIGHT off plays identically. */
#if D_DYNLIGHT
/* Lights are tic-quantized by construction -- everything the scan below
 * reads is simulation state that only a game tic can change -- so frames
 * within one tic rebuild an identical list. Cache by gametic and skip the
 * whole thinker walk on the repeat frames (at 60 fps and 35 Hz tics, nearly
 * every other frame). Invalidated on level (re)load, when the thinker list
 * is rebuilt while gametic keeps counting. */
static int lights_gametic = -1;

void D_LightsInvalidate(void)
{
    lights_gametic = -1;
}
#else
void D_LightsInvalidate(void) { }
#endif

void D_LightsUpdate(void)
{
#if D_DYNLIGHT
    {
        extern int gametic;
        if (gametic == lights_gametic) return;
        lights_gametic = gametic;
    }

    r_light_reset();

    const player_t *pl = &players[consoleplayer];
    if (!pl->mo) return;

    const float px = (float)pl->mo->x / 65536.0f;
    const float py = (float)pl->mo->y / 65536.0f;

    /* Before anything registers: the registry ranks slots by what a light
     * is worth from here, and a lit room now offers it more than eight. */
    r_light_eye(px, py);

#if D_RUMBLE
    /* The chainsaw is the one weapon you feel the whole time it is out.
     * Idle it idles: a low floor the duty-cycle dither turns into a slow
     * irregular tick, the motor catching every few frames. Biting, it
     * runs hard and continuous. The saw's attack states are the tell --
     * A_Saw's own frames -- so the change lands the moment the blade
     * engages rather than when the button goes down. */
    {
        float saw = 0.0f;
        if (pl->readyweapon == wp_chainsaw && pl->health > 0) {
            const state_t *ws = pl->psprites[ps_weapon].state;
            const int sn = ws ? (int)(ws - states) : -1;
            const int biting = (sn == S_SAW1 || sn == S_SAW2 || sn == S_SAW3);
            saw = biting ? 0.55f : 0.11f;
        }
        if (saw > 0.0f) D_RumbleSustain(saw);
    }

    /* The damage edge: damagecount jumps by the damage taken and decays
     * one per tic, so a rise is a fresh hit -- the same signal the red
     * flash keys on. Runs once per gametic behind this function's cache. */
    {
        static int last_dmg, last_health;
        const int dc = pl->damagecount;
        const int hp = pl->health;
        /* Two edges, strongest wins. damagecount is the red flash's
         * signal but CAPS AT 100: under sustained battering it pins and
         * the rise-edge goes silent exactly when the thumping should be
         * hardest. Health drops on every damaging attack in the game --
         * the universal edge -- and covers the saturated case. */
        int hurt = 0;
        if (dc > last_dmg)     hurt = dc - last_dmg;
        if (hp < last_health && last_health - hp > hurt)
            hurt = last_health - hp;
        if (hurt > 0) {
            /* Getting hit is one flat, solid thump -- no scaling, no
             * attenuation of any kind, by request from the couch. Only
             * the explosion concussion below varies (with distance),
             * and a blast that also wounds you stacks both. */
            D_RumbleAdd(0.6f);
        }
        last_dmg    = dc;
        last_health = hp;
    }
#endif

    /* Anything this far from the eye cannot reach geometry the player is
     * looking at closely enough to matter, and the list is small. */
    const float CULL = 1400.0f;

    /* Keys glow. They are one to three per level, static until taken, and
     * removed from the thinker list the moment they are -- so this costs
     * one switch case and nothing else. Deliberately the DIMMEST entry in
     * the registry: with eight slots and weakest-first eviction, a
     * fireball must always be able to take a key's slot in a firefight.
     * One breath shared by every key on the level, one sine per gametic. */
#if D_KEYLIGHT
    const float key_lum = 0.34f + 0.06f * fm_sinf((float)leveltime * 0.10f);

    /* Flame, not breath: torches and candles flicker rather than pulse.
     * Two sines well off each other's period, so the sum never repeats
     * on a beat the eye can catch -- the same trick the vapor uses for
     * its churn. One evaluation a tic, shared by every flame in the
     * level; they are all the same fire. */
    const float torch_lum = 0.46f
                          + 0.05f * fm_sinf((float)leveltime * 0.41f)
                          + 0.03f * fm_sinf((float)leveltime * 0.97f);
#endif

    /* The weapon's flash. ps_flash carries a state only while the frame that
     * shows the muzzle is up, which is exactly the window the light wants --
     * no timer of our own, and it stays in step with the weapon's animation
     * however the player's fire rate changes. */
    if (pl->psprites[ps_flash].state)
        r_light_add(px, py, (float)pl->mo->z / 65536.0f + 32.0f, 512.0f, 1.0f,
                    1.00f, 0.93f, 0.78f);          /* warm gunpowder white */

#if D_RUMBLE
    /* Weapon kick: a short light pulse each time the muzzle flash state
     * changes -- one per shot, including each round of a chaingun volley
     * (its flash states alternate). Scaled per weapon and deliberately
     * lighter than the flat 0.6 hit thump, so recoil and pain stay
     * distinct in the hand. Melee has no flash and no kick. */
    {
        static const void *last_flash;
        const void *fs = pl->psprites[ps_flash].state;
        if (fs && fs != last_flash) {
            float k = 0.18f;
            switch (pl->readyweapon) {
                case wp_pistol:        k = 0.18f; break;
                case wp_shotgun:       k = 0.35f; break;
                case wp_supershotgun:  k = 0.45f; break;
                case wp_chaingun:      k = 0.15f; break;
                case wp_missile:       k = 0.50f; break;
                case wp_plasma:        k = 0.12f; break;
                case wp_bfg:           k = 0.70f; break;
                default: break;
            }
            D_RumbleAdd(k);
        }
        last_flash = fs;
    }
#endif

    for (thinker_t *th = thinkercap.next; th != &thinkercap; th = th->next) {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
        const mobj_t *mo = (const mobj_t *)th;

        /* Each projectile lights in its own colour -- the art's own hue,
         * saturated enough to read on grey stone but shy of pure primaries,
         * which posterize against the 16-bit framebuffer. */
        float radius, intensity, cr, cg, cb;
        /* Set by anything whose light comes from the top of its art
         * rather than from the middle of its collision box. */
        int flame_top = 0;
#if D_RUMBLE
        float boom = 0.0f;   /* set by the explosion cases; fed after the cull */
#endif
        switch (mo->type) {
            case MT_ROCKET:
                if ((mo->frame & FF_FRAMEMASK) > 0) {
                    /* Past frame A the rocket is its explosion: a wider,
                     * hotter flash for the blast's few tics instead of the
                     * small in-flight glow riding through it. */
                    radius = 560.0f; intensity = 1.15f;
                    cr = 1.00f; cg = 0.66f; cb = 0.34f;
#if D_RUMBLE
                    boom = 0.9f;
#endif
                } else {
                    radius = 384.0f; intensity = 0.90f;
                    cr = 1.00f; cg = 0.62f; cb = 0.30f;
                }
                break;
            case MT_BARREL:
                /* A standing barrel glows at the rim: the art is a can of
                 * something green and lit, and the ooze on top is the one
                 * part of it that gives light. Small and dim on purpose --
                 * barrels come in clusters of six and eight, and this must
                 * never be what fills the registry when one of them goes
                 * up. A dying one is the game's biggest practical fireball:
                 * flash hard, then decay with the explosion animation's own
                 * frames -- monotonic with the art, no state-table
                 * spelunking. */
#if D_KEYLIGHT
                /* Under KEYLIGHT with the other standing lights: it is
                 * scenery that glows, not an event. */
                if (mo->health > 0) {
                    radius = 112.0f; intensity = 0.20f;
                    cr = 0.42f; cg = 1.00f; cb = 0.38f;
                    flame_top = 1;
                    break;
                }
#else
                if (mo->health > 0) continue;
#endif
                {
                    const int fr = (int)(mo->frame & FF_FRAMEMASK);
                    intensity = 1.30f - 0.25f * (float)fr;
                    if (intensity < 0.30f) intensity = 0.30f;
#if D_RUMBLE
                    /* Only the first blast frames thump; the tail glow
                     * of the animation is light, not concussion. */
                    if (fr <= 6) boom = 1.0f;
#endif
                }
                radius = 560.0f;
                cr = 1.00f; cg = 0.64f; cb = 0.30f;
                break;
            case MT_PLASMA:      radius = 288.0f; intensity = 0.75f;
                                 cr = 0.45f; cg = 0.65f; cb = 1.00f; break;
            case MT_BFG:         radius = 576.0f; intensity = 1.00f;
                                 cr = 0.50f; cg = 1.00f; cb = 0.45f; break;
            case MT_TROOPSHOT:   radius = 320.0f; intensity = 0.80f;
                                 cr = 1.00f; cg = 0.52f; cb = 0.28f; break;
            case MT_HEADSHOT:    radius = 320.0f; intensity = 0.80f;
                                 cr = 1.00f; cg = 0.55f; cb = 0.30f; break;
            case MT_BRUISERSHOT: radius = 320.0f; intensity = 0.80f;
                                 cr = 0.55f; cg = 1.00f; cb = 0.40f; break;
#if D_KEYLIGHT
            /* Armour sits in the same class as the keys: a pickup worth
             * spotting across a dark room, glowing its own colour --
             * green for the jacket (MISC0), blue for the megaarmour
             * (MISC1). The bonus helmets are deliberately NOT here: they
             * come in dozens (E1M1 alone places twenty-five), and eight
             * of them would fill the light registry with things nobody
             * came looking for. */
            /* Standing lights: the props Doom draws as light sources but
             * never lights anything with. Colours from the art -- the
             * torch families are literally blue, green and red, the
             * lamps and candles a warm white. Deliberately below the
             * projectiles in the registry, above nothing: a room full of
             * torches must still yield its slots to a fireball, but a
             * torch outranks nothing else and should hold its slot in a
             * quiet room. Radius scales with the prop: a candle pools
             * light at its own feet, a tall lamp fills a corner. */
            case MT_MISC41:                          /* tall blue torch */
                radius = 352.0f; intensity = torch_lum;
                flame_top = 1;
                cr = 0.42f; cg = 0.55f; cb = 1.00f; break;
            case MT_MISC44:                          /* short blue torch */
                radius = 256.0f; intensity = torch_lum * 0.85f;
                flame_top = 1;
                cr = 0.42f; cg = 0.55f; cb = 1.00f; break;
            case MT_MISC42:                          /* tall green torch */
                radius = 352.0f; intensity = torch_lum;
                flame_top = 1;
                cr = 0.45f; cg = 1.00f; cb = 0.48f; break;
            case MT_MISC45:                          /* short green torch */
                radius = 256.0f; intensity = torch_lum * 0.85f;
                flame_top = 1;
                cr = 0.45f; cg = 1.00f; cb = 0.48f; break;
            case MT_MISC43:                          /* tall red torch */
                radius = 352.0f; intensity = torch_lum;
                flame_top = 1;
                cr = 1.00f; cg = 0.42f; cb = 0.30f; break;
            case MT_MISC46:                          /* short red torch */
                radius = 256.0f; intensity = torch_lum * 0.85f;
                flame_top = 1;
                cr = 1.00f; cg = 0.42f; cb = 0.30f; break;
            case MT_MISC29:                          /* techno floor lamp */
            case MT_MISC31:                          /* floor lamp       */
                radius = 320.0f; intensity = torch_lum * 0.95f;
                flame_top = 1;
                cr = 1.00f; cg = 0.95f; cb = 0.82f; break;
            case MT_MISC50:                          /* candelabra */
                radius = 224.0f; intensity = torch_lum * 0.80f;
                flame_top = 1;
                cr = 1.00f; cg = 0.88f; cb = 0.62f; break;
            case MT_MISC49:                          /* candle */
                radius = 128.0f; intensity = torch_lum * 0.55f;
                flame_top = 1;
                cr = 1.00f; cg = 0.86f; cb = 0.58f; break;
            case MT_MISC0:                           /* green armour  */
                radius = 192.0f; intensity = key_lum;
                cr = 0.38f; cg = 1.00f; cb = 0.42f; break;
            case MT_MISC1:                           /* blue armour   */
                radius = 192.0f; intensity = key_lum;
                cr = 0.40f; cg = 0.58f; cb = 1.00f; break;
            /* Key cards and skulls: MISC4/5/6 are blue/red/yellow
             * cards, MISC7/8/9 the yellow/red/blue skulls. 192 units,
             * not 256: a sphere twice a wall's height washed a whole bay
             * rather than pooling, which is not what "a key glows a
             * little" looks like. This reaches roughly one wall height
             * around the key and no further. */
            case MT_MISC4: case MT_MISC9:            /* blue  */
                radius = 192.0f; intensity = key_lum;
                cr = 0.35f; cg = 0.55f; cb = 1.00f; break;
            case MT_MISC5: case MT_MISC8:            /* red   */
                radius = 192.0f; intensity = key_lum;
                cr = 1.00f; cg = 0.32f; cb = 0.30f; break;
            case MT_MISC6: case MT_MISC7:            /* yellow */
                radius = 192.0f; intensity = key_lum;
                cr = 1.00f; cg = 0.85f; cb = 0.35f; break;
#endif
            case MT_SKULL: {
                /* A lost soul is a burning skull; it carries its fire
                 * everywhere. Kept modest while it floats -- with 8 slots,
                 * weakest-light eviction must let any real fireball beat an
                 * ambient skull -- rising when it ignites for the charge,
                 * and popping like a small barrel when it dies, since its
                 * death frames are an explosion. */
                cr = 1.00f; cg = 0.60f; cb = 0.26f;
                if (mo->health <= 0) {
                    /* Death frames F..K are 5..10; fade with the blast. */
                    const int fr = (int)(mo->frame & FF_FRAMEMASK);
                    radius    = 480.0f;
                    intensity = 1.10f - 0.18f * (float)(fr - 5);
                    if (intensity < 0.25f) intensity = 0.25f;
                } else {
                    const int sn = (int)(mo->state - states);
                    const int charging =
                        sn >= S_SKULL_ATK1 && sn <= S_SKULL_ATK4;
                    radius    = charging ? 420.0f : 320.0f;
                    intensity = charging ? 0.95f  : 0.55f;
                }
                break;
            }
            default: continue;
        }

        /* The glow IN THE AIR is a QUARTER of the reach for anything
         * that flies or detonates: the light a fireball throws is wide,
         * the fireball itself is small, and anything larger reads as a
         * fog bank around it rather than a burning thing. Tightened
         * twice from play. Keys, armour, muzzle flashes and the
         * emissive floors keep their full size. */
        switch (mo->type) {
            /* A barrel going up is the biggest fireball in the game and
             * carries a wider glow than the things that merely fly. */
            case MT_BARREL:
                r_light_halo_next(0.325f);
                break;
            case MT_ROCKET:
            case MT_TROOPSHOT: case MT_HEADSHOT: case MT_BRUISERSHOT:
            case MT_PLASMA: case MT_BFG:
                r_light_halo_next(0.25f);
                break;
#if D_KEYLIGHT
            /* A flame is a few inches across; its REACH is thirty feet.
             * At the default full fraction the glow came out half the
             * reach tall -- 176 units for a tall torch, so it hung 176
             * units BELOW the wick as well as above, swallowing the whole
             * prop and the floor around it. Centred on the flame it still
             * read as a haze over the furniture rather than as the flame
             * burning. These fractions put every one of them at roughly
             * 20-25 units, which is the size of the fire in the art. */
            case MT_MISC41: case MT_MISC42: case MT_MISC43:  /* tall torch  */
                r_light_halo_next(0.13f); break;
            case MT_MISC44: case MT_MISC45: case MT_MISC46:  /* short torch */
                r_light_halo_next(0.18f); break;
            case MT_MISC29: case MT_MISC31:                  /* lamps       */
                r_light_halo_next(0.15f); break;
            case MT_MISC50:                                  /* candelabra  */
                r_light_halo_next(0.20f); break;
            case MT_MISC49:                                  /* candle      */
                r_light_halo_next(0.28f); break;
#endif
            default: break;
        }

        const float mx = (float)mo->x / 65536.0f;
        const float my = (float)mo->y / 65536.0f;
        const float dx = mx - px, dy = my - py;
        if (dx * dx + dy * dy > CULL * CULL) continue;

#if D_RUMBLE
        /* Concussion falls off linearly to nothing at 600 units. Fed per
         * gametic (this walk is cached), so a blast pumps the level for
         * its few animation tics and the motor rides it down. */
        if (boom > 0.0f) {
            const float d2 = dx * dx + dy * dy;
            if (d2 < 600.0f * 600.0f) {
                /* sqrt.s is a native VR4300 instruction; no libm here. */
                const float k = 1.0f - sqrtf(d2) * (1.0f / 600.0f);
                D_RumbleAdd(boom * k * 0.6f);
            }
        }
#endif

        /* Height of the projectile's middle, so a fireball lights the floor
         * under it and the ceiling above rather than the plane it sits on. */
        float mz = (float)mo->z / 65536.0f
                 + (float)mo->height / 65536.0f * 0.5f;

        /* Anything that burns burns at the TOP of the prop, not at the
         * middle of its collision box. Those boxes are 16 units tall
         * whatever the thing is, so a candelabra lit from its middle put
         * the pool of light under the stand and a tall torch's flame sat
         * at knee height. The art knows better: topoffset is the distance
         * from a thing's feet to the top of its sprite, and the flame is
         * the top of these sprites -- 15 units up a candle, 92 up a tall
         * torch. The light's CENTRE goes there, at the top of the art and
         * not just below it: the flame is what is lit, and putting the
         * centre under it only spread the pool back down the stand. The
         * barrel's ooze is the same idea upside down -- it glows at the
         * rim, which is the top of the can.
         *
         * The sprite comes from R_SpriteFrame, the same call the renderer
         * draws the thing with. It must: the only other lookup here keys
         * on DOOMEDNUM, the number in the WAD's THINGS lump, and mo->type
         * is an mobjtype_t -- a different numbering entirely. Asking it
         * for MT_MISC31 got a miss and a zero, so every flame quietly
         * stayed at the middle of its 16-unit collision box and none of
         * this did anything at all. */
        if (flame_top) {
            const dt64_tex_t *spr =
                (const dt64_tex_t *)R_SpriteFrame(mo->sprite, mo->frame, 0);
            if (spr && spr->topoffset > 0) {
                mz = (float)mo->z / 65536.0f + (float)spr->topoffset;
                /* The light radiates from the top of the stand; the fire
                 * is the body just under it. An eighth of the art's height
                 * puts the glow on the flame instead of straddling the
                 * topmost pixel with half of itself in the air above. */
                r_light_halo_drop_next((float)spr->topoffset * 0.125f);
            }
        }
        r_light_add(mx, my, mz, radius, intensity, cr, cg, cb);
    }

#if D_HWSTAT
    /* Harvest line for the capture rig: the route is tic-keyed, so any tic
     * printed here can be replayed exactly with FREEZE=<tic> to pin a
     * frame with that light live -- how lit poses are found for pixel
     * validation. Once per lit stretch, not per frame. */
    {
        extern int leveltime;
        static int was_lit;
        const int lit = r_light_count() > 0;
        if (lit && !was_lit)
            debugf("lights: lit from leveltime=%d n=%d\n",
                   leveltime, r_light_count());
        was_lit = lit;
    }
#endif
#endif
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
