#include "p_level.h"

#include <libdragon.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"
#include "wad.h"

/* On-disk record sizes, from the Doom specs. Checked against the lump size so
 * a mismatch is caught at load rather than as corrupt geometry later. */
#define SZ_VERTEX     4
#define SZ_SECTOR    26
#define SZ_SIDEDEF   30
#define SZ_LINEDEF   14
#define SZ_SEG       12
#define SZ_SSECTOR    4
#define SZ_NODE      28

/* Map lumps follow their marker in a fixed order, and are found by position
 * rather than by name: every map has a lump called THINGS. */
static int map_lump(int marker, const char *name)
{
    /* The block is at most ~11 lumps; searching a short window avoids picking
     * up the next map's copy of the same name. */
    for (int i = marker + 1; i < marker + 12 && i < wad_num_lumps(); i++) {
        const wad_lump_t *l = wad_lump(i);
        if (!l) break;
        if (strncasecmp(l->name, name, 8) == 0) return i;
    }
    return -1;
}

/* Read a whole lump into a scratch buffer, check it divides evenly into
 * records, and report the count. */
static uint8_t *load_lump(int marker, const char *name, int recsize, int *count)
{
    const int idx = map_lump(marker, name);
    if (idx < 0) { debugf("level: missing lump %s\n", name); return NULL; }

    const wad_lump_t *l = wad_lump(idx);
    if (l->size % recsize) {
        debugf("level: %s is %lu bytes, not a multiple of %d\n",
               name, (unsigned long)l->size, recsize);
        return NULL;
    }

    uint8_t *raw = malloc(l->size ? l->size : 1);
    if (!raw) { debugf("level: out of scratch for %s\n", name); return NULL; }

    if (!wad_read(idx, raw)) {
        debugf("level: read failed for %s\n", name);
        free(raw);
        return NULL;
    }

    *count = (int)(l->size / recsize);
    return raw;
}

/* --- per-level texture working set ---------------------------------------
 *
 * A map names a small fraction of the IWAD's 287 wall textures -- 34 for E1M1,
 * about 449 KB -- so only those are pulled into RAM. Every texture is already
 * composed in ROM, keyed by name, so resolution is just a load on first use.
 *
 * Names are the key rather than an index because that is what sidedefs store,
 * and it keeps the format independent of the order wad2n64 happened to emit. */
/* Sprites made this much larger than it needs to be for geometry alone.
 *
 * A level's walls and flats come to about 150 entries. Animated actors add
 * every frame they pass through, and sprite lookup deliberately probes names
 * that do not exist -- a monster has no rotation-0 lump -- with each miss
 * cached as an entry so it is not retried. The table is pointers and names
 * only; the texels live in the texture arena and are budgeted there. */
#define MAX_LEVEL_TEX 1024

static dt64_tex_t lvl_tex[MAX_LEVEL_TEX];
static char       lvl_texname[MAX_LEVEL_TEX][12];
static int        lvl_numtex;

/* --- name lookup hash ----------------------------------------------------
 *
 * Resolution used to be a strcasecmp scan of the whole name table. That table
 * is 12 KB against an 8 KB D-cache and grows to several hundred entries once
 * sprites and cached misses pile in -- and it is probed from two per-frame
 * paths: every visible actor's sprite frame (up to three probes each) and
 * every UI patch. Tens of thousands of byte compares a frame, fully serial on
 * the CPU. The key is packed into three words, uppercased once, so a probe is
 * a handful of word compares; open addressing at 50% max load keeps chains
 * short. */
#define TEX_HASH_SIZE (MAX_LEVEL_TEX * 2)      /* power of two */

typedef struct { uint32_t w[3]; } texkey_t;

static int16_t  tex_hash[TEX_HASH_SIZE];
static texkey_t lvl_texkey[MAX_LEVEL_TEX];
static bool     tex_hash_ready;

static void tex_hash_reset(void)
{
    for (int i = 0; i < TEX_HASH_SIZE; i++) tex_hash[i] = -1;
    tex_hash_ready = true;
}

static inline uint32_t texkey_hash(const texkey_t *k)
{
    uint32_t h = k->w[0] * 2654435761u;
    h ^= k->w[1] * 2246822519u;
    h ^= k->w[2] * 3266489917u;
    return h ^ (h >> 15);
}

/* --- animated textures -------------------------------------------------
 *
 * Doom animates by indirection: every reference is looked up through a
 * translation table whose entries advance each tic, so nukage flows and
 * fire walls flicker without any geometry changing. The vanilla table is
 * indexed by texture number and relies on an animation's frames being
 * CONSECUTIVE numbers, which they are in TEXTURE1 order. Here texture
 * indices are handed out by a hash as names are first referenced, so they
 * are in no order at all -- hence a table of explicit frame lists rather
 * than a base and a count.
 *
 * The frame names come from texorder.bin, which the converter writes
 * because only it sees TEXTURE1 and the flat markers. Ranges cannot be
 * derived from the names: FIREWALL runs FIREWALA, FIREWALB, FIREWALL. */
#define ANIM_MAX        32
#define ANIM_MAX_FRAMES 16


typedef struct {
    int16_t frame[ANIM_MAX_FRAMES];   /* texture indices, in order */
    uint8_t numframes;
    uint8_t speed;
    uint8_t hold;                     /* never advance: see the liquid note */
} anim_t;

static anim_t   anims[ANIM_MAX];
static int      numanims;
static int16_t  anim_xlat[MAX_LEVEL_TEX];   /* index -> index to draw now */
static uint8_t *order_names;                /* flats then textures, 8 bytes each */
static int      order_nflats, order_ntex;

static void anim_load_order(void)
{
    if (order_names) return;

    int size = 0;
    uint8_t *raw = read_whole_file_public("/texorder.bin", &size);
    if (!raw || size < 4) return;

    order_nflats = (raw[0] << 8) | raw[1];
    const int flatbytes = order_nflats * 8;
    if (2 + flatbytes + 2 > size) { order_nflats = 0; return; }
    order_ntex = (raw[2 + flatbytes] << 8) | raw[3 + flatbytes];
    order_names = raw;
}

static const char *order_name(int section_flats, int i)
{
    if (!order_names) return NULL;
    if (section_flats)
        return i < order_nflats ? (const char *)(order_names + 2 + i * 8) : NULL;
    return i < order_ntex
        ? (const char *)(order_names + 4 + order_nflats * 8 + i * 8) : NULL;
}

static int order_find(int section_flats, const char *name)
{
    const int n = section_flats ? order_nflats : order_ntex;
    for (int i = 0; i < n; i++) {
        const char *c = order_name(section_flats, i);
        int eq = 1;
        for (int k = 0; k < 8; k++) {
            const char a = c[k], b = name[k] ? name[k] : 0;
            if ((a >= 'a' && a <= 'z' ? a - 32 : a) !=
                (b >= 'a' && b <= 'z' ? b - 32 : b)) { eq = 0; break; }
            if (!a && !b) break;
        }
        if (eq) return i;
    }
    return -1;
}

/* Build the frame lists for this level. Only animations whose art the
 * level actually references are registered -- resolving the rest would
 * pull their frames into the texture arena for nothing. */
void p_level_anim_init(void)
{
    /* Doom's own table, declared here rather than by including p_spec.h,
     * which drags in the whole game header chain. boolean is int-sized, so
     * the layout matches; the list ends with istexture == -1. */
    typedef struct {
        int  istexture;
        char endname[9];
        char startname[9];
        int  speed;
    } animdef_t;
    extern animdef_t animdefs[];

    numanims = 0;
    for (int i = 0; i < lvl_numtex; i++) anim_xlat[i] = (int16_t)i;
    anim_load_order();
    if (!order_names) return;

    for (int i = 0; animdefs[i].istexture != -1 && numanims < ANIM_MAX; i++) {
        const int flats = !animdefs[i].istexture;
        const char *pfx = flats ? "f_" : "";

        const int s = order_find(flats, animdefs[i].startname);
        const int e = order_find(flats, animdefs[i].endname);
        if (s < 0 || e < 0 || e < s || e - s + 1 > ANIM_MAX_FRAMES) continue;

        /* Skip the whole animation unless the level uses one of its
         * frames; p_level_lookup answers without loading anything. */
        int used = 0;
        for (int f = s; f <= e && !used; f++)
            if (p_level_lookup(order_name(flats, f), pfx) >= 0) used = 1;
        if (!used) continue;

        anim_t *a = &anims[numanims];
        a->numframes = 0;
        a->hold = 0;
        /* Vanilla runs every animation at 8 tics a frame, and for a liquid
         * that frame swap IS the motion -- three or four hand-painted
         * cells cycled forever. It is not motion here any more: the
         * surface drifts and swells continuously underneath it (see
         * r_flat.c), and the swap on top of that reads as a flicker
         * fighting the flow rather than adding to it. Halving its rate
         * only halved the argument, so the liquids now hold a single
         * cell and let the drift do all of it. Fire, switches, the
         * teleport pad and the rest keep Doom's timing, which is the
         * whole of their animation.
         *
         * Held rather than pinned to frame 0: anim_xlat starts as the
         * identity, so a sector keeps whatever cell the mapper laid down
         * instead of every pool in the game snapping to the same one. */
        {
            static const char *const liquid[] = {
                "NUKAGE", "LAVA", "SLIME", "BLOOD", "FWATER", "SWATER"
            };
            a->speed = animdefs[i].speed ? animdefs[i].speed : 8;
#if R_LIQUIDFLOW
            for (unsigned q = 0; q < sizeof liquid / sizeof liquid[0]; q++)
                if (!strncmp(animdefs[i].startname, liquid[q],
                             strlen(liquid[q]))) { a->hold = 1; break; }
#else
            (void)liquid;
#endif
        }
        for (int f = s; f <= e; f++) {
            const int idx = p_level_resolve(order_name(flats, f), pfx);
            if (idx >= 0) a->frame[a->numframes++] = (int16_t)idx;
        }
        if (a->numframes > 1) numanims++;
    }

    debugf("anims: %d animations active\n", numanims);
}

/* Advance every animation one tic and republish the translation. */
void p_level_anim_tick(void)
{
    static int tics;
    tics++;
    for (int i = 0; i < numanims; i++) {
        const anim_t *a = &anims[i];
        if (a->hold) continue;              /* liquids: the drift is the motion */
        const int step = (tics / a->speed) % a->numframes;
        for (int f = 0; f < a->numframes; f++)
            anim_xlat[a->frame[f]] = a->frame[(f + step) % a->numframes];
    }
}

/* Hot-block member: called ~8x per visited seg from the walk. See the
 * R_HOT note in r_wall.h; kept as a local define to spare this file the
 * renderer's headers. */
#ifndef R_HOT
#define R_HOT __attribute__((section(".text.hot")))
#endif
R_HOT dt64_tex_t *p_level_texture(int index)
{
    if (index < 0 || index >= lvl_numtex) return NULL;
    index = anim_xlat[index];                 /* animation indirection */
    if (index < 0 || index >= lvl_numtex) return NULL;
    /* A zero-width entry is a name known to have no asset -- cached so the
     * lookup is not retried, and reported once rather than per reference. */
    if (!lvl_tex[index].width) return NULL;
    return &lvl_tex[index];
}

int p_level_numtextures(void) { return lvl_numtex; }

/* Resolve an 8-byte sidedef texture name, loading it on first reference. */
static int tex_resolve_pfx(const uint8_t *name8, const char *prefix);
static int tex_lookup_pfx(const uint8_t *name8, const char *prefix);

/* Name-to-index lookup for Doom's own level loader.
 *
 * p_setup.c resolves texture names as it reads sidedefs and sectors, exactly
 * as this file does, but it works in NUL-terminated strings while the WAD
 * stores fixed 8-byte fields. Padding here rather than at each call site keeps
 * the vendored code unmodified. */
/* Per-subsector polygon size from the old loader, for cross-checking the
 * rebuilt version against Doom's structures. Declared at its call site rather
 * than in a header: that file speaks Doom's types and this one does not. */
int p_level_ss_numpts(int i);
static level_t *verify_level;
int p_level_ss_numpts(int i)
{
    if (!verify_level || i < 0 || i >= verify_level->numsubsectors) return -1;
    return verify_level->subsectors[i].numpts;
}

int p_level_resolve(const char *name, const char *prefix)
{
    uint8_t key[8];
    for (int i = 0; i < 8; i++) key[i] = name[i] ? (uint8_t)name[i] : 0;
    return tex_resolve_pfx(key, prefix);
}

/* Is this name already in the level's table? Answers without loading, so a
 * caller can ask "does this level use it" without dragging the art in.
 * P_InitSwitchList needs exactly that: it walks every switch pair Doom
 * knows, and resolving all of them would pull in eighty wall textures for
 * the handful of switches a map actually has. */
int p_level_lookup(const char *name, const char *prefix)
{
    uint8_t key[8];
    for (int i = 0; i < 8; i++) key[i] = name[i] ? (uint8_t)name[i] : 0;
    return tex_lookup_pfx(key, prefix);
}

static int tex_resolve(const uint8_t *name8) { return tex_resolve_pfx(name8, ""); }

/* Shared by the resolver and the no-load lookup: build the key and probe.
 * Returns the slot in *slot so the resolver can fill it on a miss. */
static int tex_probe(const uint8_t *name8, const char *prefix,
                     char nm_out[9], texkey_t *key_out, uint32_t *slot)
{
    char nm[9];
    memcpy(nm, name8, 8);
    nm[8] = '\0';
    for (int i = 7; i >= 0 && (nm[i] == ' ' || nm[i] == '\0'); i--) nm[i] = '\0';
    for (int i = 0; i < 8 && nm[i]; i++)
        if (nm[i] >= 'a' && nm[i] <= 'z') nm[i] -= 32;
    if (!nm[0] || nm[0] == '-') return -2;

    union { char b[12]; texkey_t k; } kb = { .b = { 0 } };
    kb.b[0] = prefix[0] ? prefix[0] : ' ';
    for (int i = 0; i < 8 && nm[i]; i++) kb.b[1 + i] = nm[i];
    const texkey_t key = kb.k;

    if (!tex_hash_ready) tex_hash_reset();
    uint32_t h = texkey_hash(&key) & (TEX_HASH_SIZE - 1);
    while (tex_hash[h] >= 0) {
        const int i = tex_hash[h];
        if (lvl_texkey[i].w[0] == key.w[0] && lvl_texkey[i].w[1] == key.w[1] &&
            lvl_texkey[i].w[2] == key.w[2])
            return i;
        h = (h + 1) & (TEX_HASH_SIZE - 1);
    }
    memcpy(nm_out, nm, 9);
    *key_out = key;
    *slot = h;
    return -1;
}

static int tex_lookup_pfx(const uint8_t *name8, const char *prefix)
{
    char nm[9]; texkey_t key; uint32_t slot;
    const int r = tex_probe(name8, prefix, nm, &key, &slot);
    /* A cached miss counts as absent: the name was asked for once and has
     * no art, so nothing should be registered against it. */
    return (r >= 0 && lvl_tex[r].width) ? r : -1;
}

static int tex_resolve_pfx(const uint8_t *name8, const char *prefix)
{
    /* Key building and the hash probe are shared with the no-load lookup so
     * the two can never disagree about what a name means. Early IWADs store
     * sidedef names in mixed case ("Crate1", "metal") while the converted
     * files are uppercase, and DFS compares exactly -- that mismatch made
     * every such wall invisible, E2M2's crate maze most famously -- so
     * tex_probe uppercases once, for both the key and the path below. */
    char nm[9];
    texkey_t key;
    uint32_t h;
    const int hit = tex_probe(name8, prefix, nm, &key, &h);
    if (hit == -2) return -1;                 /* empty name, or Doom's "-" */
    if (hit >= 0) return lvl_tex[hit].width ? hit : -1;

    if (lvl_numtex >= MAX_LEVEL_TEX) {
        debugf("level: texture table full at %d, dropping '%s'\n", lvl_numtex, nm);
        return -1;
    }

    char path[32];
    snprintf(path, sizeof path, "/%s%s.dt64", prefix, nm);
    if (!dt64_load(&lvl_tex[lvl_numtex], path)) {
        /* Wall textures always exist in a converted set; a miss here is a
         * real fault worth naming. Sprite probes miss by design and stay
         * quiet. */
        if (!prefix[0])
            debugf("tex miss: %s\n", path);
#if DEBUG_RDP
        /* UI misses were silent, and silence is how an unpainted
         * intermission looks exactly like a working one that drew
         * nothing. Under DEBUG they name themselves. */
        else if (prefix[0] == 'u')
            debugf("ui miss: %s (slots %d/%d)\n", path, lvl_numtex,
                   MAX_LEVEL_TEX);
#endif
        /* Only a genuinely absent file is remembered. A failed read or a
         * momentarily full arena must stay retryable, or one bad cartridge
         * read hides that sprite for the whole level. */
        if (!dt64_last_absent) {
            debugf("tex load failed (retryable): %s\n", path);
            return -1;
        }

        /* Record the miss so repeated references neither retry the load nor
         * repeat the message. Sprite lookups deliberately probe a name that
         * often does not exist, so this is the common path, not an error. */
        memset(&lvl_tex[lvl_numtex], 0, sizeof lvl_tex[0]);
        memcpy(lvl_texname[lvl_numtex], &key, 12);
        lvl_texkey[lvl_numtex] = key;
        tex_hash[h] = (int16_t)lvl_numtex;
        anim_xlat[lvl_numtex] = (int16_t)lvl_numtex;
        lvl_numtex++;
        return -1;
    }
    memcpy(lvl_texname[lvl_numtex], &key, 12);
    lvl_texkey[lvl_numtex] = key;
    tex_hash[h] = (int16_t)lvl_numtex;
    /* Identity from the moment the index exists. Textures resolve lazily as
     * the renderer first references them, so anything left to the
     * level-start pass would keep a zeroed entry and draw texture 0. */
    anim_xlat[lvl_numtex] = (int16_t)lvl_numtex;
    const int idx = lvl_numtex++;

    /* Wall textures carry a half-resolution level; flats and sprites do not
     * need one (flats are already downsampled, sprites are never minified
     * enough to matter). Loading it here keeps the choice invisible to the
     * renderer beyond a pointer. */
    if (prefix[0] == 's' && lvl_numtex < MAX_LEVEL_TEX) {
        /* Sprites carry one half-resolution level, used once a frame is drawn
         * at less than half size. */
        char mpath[36];
        snprintf(mpath, sizeof mpath, "/n_%s.dt64", nm);
        if (dt64_load(&lvl_tex[lvl_numtex], mpath)) {
            lvl_tex[idx].mip = &lvl_tex[lvl_numtex];
            snprintf(lvl_texname[lvl_numtex], sizeof lvl_texname[0], "^%s", nm);
            lvl_numtex++;
        }
    }

    if (!prefix[0]) {
        /* Chain the levels: each points at the next one down, so the renderer
         * walks the chain rather than indexing a fixed set. */
        int prev = idx;
        static const char lvlpfx[] = { 'm', 'q' };
        for (unsigned l = 0; l < sizeof lvlpfx && lvl_numtex < MAX_LEVEL_TEX; l++) {
            char mpath[32];
            snprintf(mpath, sizeof mpath, "/%c_%s.dt64", lvlpfx[l], nm);
            if (!dt64_load(&lvl_tex[lvl_numtex], mpath)) break;
            lvl_tex[prev].mip = &lvl_tex[lvl_numtex];
            snprintf(lvl_texname[lvl_numtex], sizeof lvl_texname[0], "~%u%s",
                     l, nm);
            prev = lvl_numtex++;
        }
    }
    return idx;
}

/* Allocate the in-RAM array for a lump from the level arena. */
static void *alloc_level(size_t n, size_t elem, size_t *total)
{
    void *p = mem_alloc(MEM_ARENA_LEVEL, n * elem);
    if (p) *total += n * elem;
    return p;
}

/* --- subsector polygons ---------------------------------------------------
 *
 * Doom stores no polygon for a subsector, only the segs along its walls. The
 * edges where the BSP split open space have no seg at all, so a fan over the
 * segs leaves gaps -- which is why GL ports historically needed a nodebuilder
 * to add the missing edges.
 *
 * Reconstructing them is cheap if done the other way round: start with the
 * whole map as one convex polygon and clip it by every partition line on the
 * path from the root down to the subsector, then by the subsector's own segs.
 * A convex region intersected with half-planes stays convex, so the result is
 * exactly the subsector and a triangle fan over it is valid.
 *
 * Doing this at load time rather than in the build tool avoids a second asset
 * format for something that takes a few milliseconds and ~20 KB. */
#define MAX_POLY_PTS 24

typedef struct { float x, y; } vec2_t;

/* Signed side of the directed line (px,py)+(dx,dy). Matches r_bsp's
 * point_on_side: positive is Doom's "front". */
static inline float side_of(float x, float y,
                            float px, float py, float dx, float dy)
{
    return dy * (x - px) - dx * (y - py);
}

/* Sutherland-Hodgman clip of a convex polygon to the front half-plane.
 * `keep_front` false clips to the back instead. */
static int clip_half(const vec2_t *in, int n, vec2_t *out,
                     float px, float py, float dx, float dy, bool keep_front)
{
    if (n == 0) return 0;
    const float sgn = keep_front ? 1.0f : -1.0f;

    int m = 0;
    for (int i = 0; i < n; i++) {
        const vec2_t a = in[i], b = in[(i + 1) % n];
        const float sa = sgn * side_of(a.x, a.y, px, py, dx, dy);
        const float sb = sgn * side_of(b.x, b.y, px, py, dx, dy);

        if (sa >= 0.0f) { if (m < MAX_POLY_PTS) out[m++] = a; }
        if ((sa > 0.0f && sb < 0.0f) || (sa < 0.0f && sb > 0.0f)) {
            const float t = sa / (sa - sb);
            if (m < MAX_POLY_PTS) {
                out[m].x = a.x + (b.x - a.x) * t;
                out[m].y = a.y + (b.y - a.y) * t;
                m++;
            }
        }
    }
    return m;
}

static void store_poly(level_t *lev, int ssnum, vec2_t *poly, int n,
                       int *ptcursor)
{
    subsector_t *ss = &lev->subsectors[ssnum];
    ss->numpts = 0;
    if (n < 3) return;                     /* degenerate: nothing to draw */

    if (*ptcursor + n > lev->numpolypts) return;   /* out of room */

    ss->firstpt = (uint16_t)*ptcursor;
    ss->numpts  = (uint16_t)n;

    /* Nudge the polygon outward from its own centre.
     *
     * Neighbouring BSP cells tile the floor exactly, but a vertex of one can
     * land partway along another's edge -- a T-junction, which the rasteriser
     * does not fill identically on both sides and which shows as a hairline
     * between floor or ceiling patches. A fraction of a unit of overlap closes
     * it. Cells at different heights overlap by the same negligible amount,
     * and the depth buffer settles which is in front.
     *
     * Done once at load rather than per frame: the geometry never moves. */
    float cxp = 0.0f, cyp = 0.0f;
    for (int i = 0; i < n; i++) { cxp += poly[i].x; cyp += poly[i].y; }
    cxp /= (float)n; cyp /= (float)n;

    float xl = 1e9f, xh = -1e9f, yl = 1e9f, yh = -1e9f;
    for (int i = 0; i < n; i++) {
        const float ox = poly[i].x - cxp, oy = poly[i].y - cyp;
        const float len = sqrtf(ox * ox + oy * oy);
        const float k = len > 1e-3f ? (1.0f + 0.75f / len) : 1.0f;
        poly[i].x = cxp + ox * k;
        poly[i].y = cyp + oy * k;

        lev->polypts[*ptcursor].x = poly[i].x;
        lev->polypts[*ptcursor].y = poly[i].y;
        if (poly[i].x < xl) xl = poly[i].x;
        if (poly[i].x > xh) xh = poly[i].x;
        if (poly[i].y < yl) yl = poly[i].y;
        if (poly[i].y > yh) yh = poly[i].y;
        (*ptcursor)++;
    }

    /* Cached so the renderer can reject an off-screen cell with four
     * transforms instead of clipping every edge. */
    ss->bx0 = (int16_t)xl; ss->bx1 = (int16_t)xh;
    ss->by0 = (int16_t)yl; ss->by1 = (int16_t)yh;
}

static void build_polys(level_t *lev, int num, vec2_t *poly, int n, int *cursor)
{
    if (num & NF_SUBSECTOR) {
        const int ssnum = num & ~NF_SUBSECTOR;
        if (ssnum >= lev->numsubsectors) return;
        const subsector_t *ss = &lev->subsectors[ssnum];

        /* Finally trim by the subsector's own walls. */
        vec2_t a[MAX_POLY_PTS], b[MAX_POLY_PTS];
        int cn = n;
        for (int i = 0; i < cn; i++) a[i] = poly[i];

        for (int i = 0; i < ss->numsegs && cn >= 3; i++) {
            const int sn = ss->firstseg + i;
            if (sn < 0 || sn >= lev->numsegs) continue;
            const seg_t *sg = &lev->segs[sn];
            if (sg->v1 >= lev->numvertexes || sg->v2 >= lev->numvertexes) continue;

            const float x1 = (float)(lev->vertexes[sg->v1].x >> FRACBITS);
            const float y1 = (float)(lev->vertexes[sg->v1].y >> FRACBITS);
            const float x2 = (float)(lev->vertexes[sg->v2].x >> FRACBITS);
            const float y2 = (float)(lev->vertexes[sg->v2].y >> FRACBITS);

            cn = clip_half(a, cn, b, x1, y1, x2 - x1, y2 - y1, true);
            for (int k = 0; k < cn; k++) a[k] = b[k];
        }

        store_poly(lev, ssnum, a, cn, cursor);
        return;
    }

    if (num >= lev->numnodes) return;
    const node_t *nd = &lev->nodes[num];
    const float px = (float)(nd->x >> FRACBITS), py = (float)(nd->y >> FRACBITS);
    const float dx = (float)(nd->dx >> FRACBITS), dy = (float)(nd->dy >> FRACBITS);

    vec2_t child[MAX_POLY_PTS];
    int cn;

    cn = clip_half(poly, n, child, px, py, dx, dy, true);
    build_polys(lev, nd->children[0], child, cn, cursor);

    cn = clip_half(poly, n, child, px, py, dx, dy, false);
    build_polys(lev, nd->children[1], child, cn, cursor);
}

/* Resolve which sector each subsector belongs to, via its first seg. */
static void assign_ss_sectors(level_t *lev)
{
    for (int i = 0; i < lev->numsubsectors; i++) {
        subsector_t *ss = &lev->subsectors[i];
        ss->sector = 0xFFFF;
        if (!ss->numsegs) continue;
        const int sn = ss->firstseg;
        if (sn < 0 || sn >= lev->numsegs) continue;
        const seg_t *sg = &lev->segs[sn];
        if (sg->linedef >= lev->numlines) continue;
        const line_t *ld = &lev->lines[sg->linedef];
        const uint16_t sd = ld->sidenum[sg->side ? 1 : 0];
        if (sd == 0xFFFF || sd >= lev->numsides) continue;
        ss->sector = lev->sides[sd].sector;
    }
}

/* --- things --------------------------------------------------------------
 *
 * Doom maps a thing type to a sprite through mobjinfo and the state table in
 * info.c -- some two thousand lines of tables driving animation, AI and
 * pickup behaviour. None of that exists yet, so this is a direct
 * type-to-sprite-name table covering what Doom 1 actually places. It renders
 * the first frame only; animation arrives with the state machine.
 *
 * A type absent from this table is simply not drawn, which is correct for the
 * ones that have no sprite anyway (player and deathmatch starts, teleport
 * destinations). */
typedef struct { int16_t type; const char *sprite; } thingspr_t;

static const thingspr_t thing_sprites[] = {
    /* monsters */
    { 3004, "POSS" }, {    9, "SPOS" }, { 3001, "TROO" }, { 3002, "SARG" },
    { 3005, "HEAD" }, { 3003, "BOSS" }, {   58, "SARG" }, {   16, "CYBR" },
    {    7, "SPID" }, {   68, "BSPI" }, {   65, "CPOS" }, {   69, "HKNI" },
    {   71, "PAIN" }, {   66, "SKEL" }, {   67, "FATT" }, {   64, "VILE" },
    /* weapons */
    { 2001, "SHOT" }, { 2002, "MGUN" }, { 2003, "LAUN" }, { 2004, "PLAS" },
    { 2005, "CSAW" }, { 2006, "BFUG" }, {   82, "SGN2" },
    /* ammo */
    { 2007, "CLIP" }, { 2008, "SHEL" }, { 2010, "ROCK" }, { 2047, "CELL" },
    { 2048, "AMMO" }, { 2049, "SBOX" }, { 2046, "BROK" }, {   17, "CELP" },
    /* health and armour */
    { 2011, "STIM" }, { 2012, "MEDI" }, { 2014, "BON1" }, { 2015, "BON2" },
    { 2018, "ARM1" }, { 2019, "ARM2" }, { 2013, "SOUL" }, {   83, "MEGA" },
    /* powerups */
    { 2022, "PINV" }, { 2023, "PSTR" }, { 2024, "PINS" }, { 2025, "SUIT" },
    { 2026, "PMAP" }, { 2045, "PVIS" }, {    8, "BPAK" },
    /* keys */
    {    5, "BKEY" }, {   13, "RKEY" }, {    6, "YKEY" },
    {   40, "BSKU" }, {   38, "RSKU" }, {   39, "YSKU" },
    /* decorations and obstacles */
    { 2035, "BAR1" }, { 2028, "COLU" }, {   85, "TLMP" }, {   86, "TLP2" },
    {   34, "CAND" }, {   35, "CBRA" }, {   31, "COL2" }, {   32, "COL3" },
    {   33, "COL4" }, {   30, "COL1" }, {   37, "COL6" }, {   36, "COL5" },
    {   41, "CEYE" }, {   42, "FSKU" }, {   43, "TRE1" }, {   54, "TRE2" },
    {   47, "SMIT" }, {   48, "ELEC" }, {   26, "POL6" }, {   10, "PLAY" },
    {   12, "PLAY" }, {   15, "PLAY" }, {   24, "POL5" }, {   28, "POL2" },
    {   27, "POL4" }, {   29, "POL3" }, {   25, "POL1" }, {   44, "TBLU" },
    {   45, "TGRN" }, {   46, "TRED" }, {   55, "SMBT" }, {   56, "SMGT" },
    {   57, "SMRT" }, {   70, "FCAN" },
};

static const char *sprite_for_type(int16_t type)
{
    for (size_t i = 0; i < sizeof thing_sprites / sizeof thing_sprites[0]; i++)
        if (thing_sprites[i].type == type) return thing_sprites[i].sprite;
    return NULL;
}

/* Resolve a sprite frame, trying the rotation-less form first.
 *
 * Doom names frames <BASE><frame><rot>: rotation 0 means the sprite looks the
 * same from every angle, which is true of every pickup and decoration.
 * Monsters instead have eight rotations, so fall back to the one facing the
 * viewer head-on. */
static dt64_tex_t *resolve_sprite(const char *base)
{
    char nm[16];
    uint8_t key[8];

    snprintf(nm, sizeof nm, "%sA0", base);
    memset(key, 0, sizeof key);
    memcpy(key, nm, strlen(nm) < 8 ? strlen(nm) : 8);
    int idx = tex_resolve_pfx(key, "s_");
    if (idx < 0) {
        snprintf(nm, sizeof nm, "%sA1", base);
        memset(key, 0, sizeof key);
        memcpy(key, nm, strlen(nm) < 8 ? strlen(nm) : 8);
        idx = tex_resolve_pfx(key, "s_");
    }
    return p_level_texture(idx);
}

/* Which sector a point is in, by descending the BSP. Needed to stand a thing
 * on its floor, and by the player to follow it. */
int p_subsector_at(const level_t *lev, float x, float y)
{
    if (!lev->numnodes) return lev->numsubsectors ? 0 : -1;

    int num = lev->numnodes - 1;
    while (!(num & NF_SUBSECTOR)) {
        if (num >= lev->numnodes) return -1;
        const node_t *n = &lev->nodes[num];
        const float dx = x - (float)(n->x >> FRACBITS);
        const float dy = y - (float)(n->y >> FRACBITS);
        const float sd = (float)(n->dy >> FRACBITS) * dx -
                         (float)(n->dx >> FRACBITS) * dy;
        num = n->children[sd > 0.0f ? 0 : 1];
    }
    const int ss = num & ~NF_SUBSECTOR;
    return ss < lev->numsubsectors ? ss : -1;
}

const sector_t *p_sector_at(const level_t *lev, float x, float y)
{
    const int ss = p_subsector_at(lev, x, y);
    if (ss < 0) return NULL;
    const subsector_t *sub = &lev->subsectors[ss];
    if (sub->sector >= lev->numsectors) return NULL;
    return &lev->sectors[sub->sector];
}

static void load_things(level_t *lev, int marker)
{
    int n = 0;
    uint8_t *raw = load_lump(marker, "THINGS", 10, &n);
    if (!raw) return;

    lev->things = alloc_level((size_t)n, sizeof *lev->things, &lev->bytes);
    if (!lev->things) { free(raw); return; }

    int drawn = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *r = raw + i * 10;
        mapthing_t *t = &lev->things[lev->numthings];
        t->x     = (float)wad_le16(r);
        t->y     = (float)wad_le16(r + 2);
        t->angle = wad_le16(r + 4);
        t->type  = wad_le16(r + 6);
        t->spr   = NULL;

        t->ssector = (int16_t)p_subsector_at(lev, t->x, t->y);
        const sector_t *sec = p_sector_at(lev, t->x, t->y);
        t->z = sec ? (float)(sec->floorheight >> FRACBITS) : 0.0f;

        const char *base = sprite_for_type(t->type);
        if (base) { t->spr = resolve_sprite(base); if (t->spr) drawn++; }

        lev->numthings++;
    }
    free(raw);

    /* Sort things by subsector so the traversal can take a contiguous range.
     *
     * Without this the renderer has to consider every thing in the map each
     * frame and reject most of them by frustum test -- fine for E1M1's 143,
     * wasteful for a map with thousands. Linking them to BSP cells means a
     * thing is only looked at when the cell it stands in is visible, which is
     * the same culling the geometry already gets for free.
     *
     * Insertion sort: the array is small and already close to sorted, since
     * mappers place things room by room. */
    for (int i = 0; i < lev->numsubsectors; i++)
        lev->subsectors[i].firstthing = lev->subsectors[i].numthings = 0;

    for (int i = 1; i < lev->numthings; i++) {
        const mapthing_t key = lev->things[i];
        int j = i - 1;
        while (j >= 0 && lev->things[j].ssector > key.ssector) {
            lev->things[j + 1] = lev->things[j];
            j--;
        }
        lev->things[j + 1] = key;
    }

    for (int i = 0; i < lev->numthings; i++) {
        const int ss = lev->things[i].ssector;
        if (ss < 0 || ss >= lev->numsubsectors) continue;
        if (!lev->subsectors[ss].numthings)
            lev->subsectors[ss].firstthing = (uint16_t)i;
        lev->subsectors[ss].numthings++;
    }

    debugf("level: %d things, %d with sprites\n", lev->numthings, drawn);
}

bool p_load_level(level_t *lev, const char *mapname)
{
    memset(lev, 0, sizeof *lev);
    strncpy(lev->name, mapname, sizeof lev->name - 1);

    const int marker = wad_find(mapname);
    if (marker < 0) { debugf("level: no map '%s' in WAD\n", mapname); return false; }

    /* Both arenas belong to the level being replaced. */
    mem_reset(MEM_ARENA_LEVEL);
    dt64_release_all();
    lvl_numtex = 0;
    tex_hash_reset();

    /* The animation table indexes into the texture slots being dropped,
     * and the name ordering lives in the texture arena that goes with
     * them, so both are stale from here. */
    numanims = 0;
    order_names = NULL;

    uint8_t *raw = NULL;
    int n = 0;

    /* --- vertexes: whole units on disk, fixed-point in memory ----------- */
    if (!(raw = load_lump(marker, "VERTEXES", SZ_VERTEX, &n))) return false;
    lev->vertexes = alloc_level(n, sizeof *lev->vertexes, &lev->bytes);
    if (!lev->vertexes) { free(raw); return false; }
    lev->numvertexes = n;
    for (int i = 0; i < n; i++) {
        lev->vertexes[i].x = (fixed_t)wad_le16(raw + i * SZ_VERTEX)     << FRACBITS;
        lev->vertexes[i].y = (fixed_t)wad_le16(raw + i * SZ_VERTEX + 2) << FRACBITS;
    }
    free(raw);

    /* --- sectors -------------------------------------------------------- */
    if (!(raw = load_lump(marker, "SECTORS", SZ_SECTOR, &n))) return false;
    lev->sectors = alloc_level(n, sizeof *lev->sectors, &lev->bytes);
    if (!lev->sectors) { free(raw); return false; }
    lev->numsectors = n;
    for (int i = 0; i < n; i++) {
        const uint8_t *r = raw + i * SZ_SECTOR;
        sector_t *s = &lev->sectors[i];
        s->floorheight   = (fixed_t)wad_le16(r)     << FRACBITS;
        s->ceilingheight = (fixed_t)wad_le16(r + 2) << FRACBITS;
        /* Flat names live at r+4 and r+12; texture resolution comes with the
         * flat pipeline, so only the light and specials are taken for now. */
        s->floorpic   = (int16_t)tex_resolve_pfx(r + 4,  "f_");

        /* F_SKY1 is not a flat but a marker: the ceiling is open to the sky,
         * so nothing is drawn there and the backdrop shows through. */
        s->skyceiling = (strncasecmp((const char *)r + 12, "F_SKY1", 6) == 0);
        s->ceilingpic = s->skyceiling
                      ? -1
                      : (int16_t)tex_resolve_pfx(r + 12, "f_");
        s->lightlevel = wad_le16(r + 20);
        s->special    = wad_le16(r + 22);
        s->tag        = wad_le16(r + 24);
    }
    free(raw);

    /* --- sidedefs ------------------------------------------------------- */
    if (!(raw = load_lump(marker, "SIDEDEFS", SZ_SIDEDEF, &n))) return false;
    lev->sides = alloc_level(n, sizeof *lev->sides, &lev->bytes);
    if (!lev->sides) { free(raw); return false; }
    lev->numsides = n;
    for (int i = 0; i < n; i++) {
        const uint8_t *r = raw + i * SZ_SIDEDEF;
        side_t *sd = &lev->sides[i];
        sd->textureoffset = (fixed_t)wad_le16(r)     << FRACBITS;
        sd->rowoffset     = (fixed_t)wad_le16(r + 2) << FRACBITS;
        sd->toptexture    = (int16_t)tex_resolve(r + 4);
        sd->bottomtexture = (int16_t)tex_resolve(r + 12);
        sd->midtexture    = (int16_t)tex_resolve(r + 20);
        sd->sector = (uint16_t)wad_le16(r + 28);
    }
    free(raw);

    /* --- linedefs ------------------------------------------------------- */
    if (!(raw = load_lump(marker, "LINEDEFS", SZ_LINEDEF, &n))) return false;
    lev->lines = alloc_level(n, sizeof *lev->lines, &lev->bytes);
    if (!lev->lines) { free(raw); return false; }
    lev->numlines = n;
    for (int i = 0; i < n; i++) {
        const uint8_t *r = raw + i * SZ_LINEDEF;
        line_t *ld = &lev->lines[i];
        ld->v1         = (uint16_t)wad_le16(r);
        ld->v2         = (uint16_t)wad_le16(r + 2);
        ld->flags      = wad_le16(r + 4);
        ld->special    = wad_le16(r + 6);
        ld->tag        = wad_le16(r + 8);
        ld->sidenum[0] = (uint16_t)wad_le16(r + 10);
        ld->sidenum[1] = (uint16_t)wad_le16(r + 12);

        if (ld->v1 < lev->numvertexes && ld->v2 < lev->numvertexes) {
            const int ax = lev->vertexes[ld->v1].x >> FRACBITS;
            const int ay = lev->vertexes[ld->v1].y >> FRACBITS;
            const int bx = lev->vertexes[ld->v2].x >> FRACBITS;
            const int by = lev->vertexes[ld->v2].y >> FRACBITS;
            ld->bx0 = (int16_t)(ax < bx ? ax : bx);
            ld->bx1 = (int16_t)(ax > bx ? ax : bx);
            ld->by0 = (int16_t)(ay < by ? ay : by);
            ld->by1 = (int16_t)(ay > by ? ay : by);
        }
    }
    free(raw);

    /* --- segs ----------------------------------------------------------- */
    if (!(raw = load_lump(marker, "SEGS", SZ_SEG, &n))) return false;
    lev->segs = alloc_level(n, sizeof *lev->segs, &lev->bytes);
    if (!lev->segs) { free(raw); return false; }
    lev->numsegs = n;
    for (int i = 0; i < n; i++) {
        const uint8_t *r = raw + i * SZ_SEG;
        seg_t *sg = &lev->segs[i];
        sg->v1      = (uint16_t)wad_le16(r);
        sg->v2      = (uint16_t)wad_le16(r + 2);
        sg->angle   = wad_le16(r + 4);
        sg->linedef = (uint16_t)wad_le16(r + 6);
        sg->side    = wad_le16(r + 8);
        sg->offset  = wad_le16(r + 10);
    }
    free(raw);

    /* --- subsectors ----------------------------------------------------- */
    if (!(raw = load_lump(marker, "SSECTORS", SZ_SSECTOR, &n))) return false;
    lev->subsectors = alloc_level(n, sizeof *lev->subsectors, &lev->bytes);
    if (!lev->subsectors) { free(raw); return false; }
    lev->numsubsectors = n;
    for (int i = 0; i < n; i++) {
        lev->subsectors[i].numsegs  = (uint16_t)wad_le16(raw + i * SZ_SSECTOR);
        lev->subsectors[i].firstseg = (uint16_t)wad_le16(raw + i * SZ_SSECTOR + 2);
    }
    free(raw);

    /* --- nodes ---------------------------------------------------------- */
    if (!(raw = load_lump(marker, "NODES", SZ_NODE, &n))) return false;
    lev->nodes = alloc_level(n, sizeof *lev->nodes, &lev->bytes);
    if (!lev->nodes) { free(raw); return false; }
    lev->numnodes = n;
    for (int i = 0; i < n; i++) {
        const uint8_t *r = raw + i * SZ_NODE;
        node_t *nd = &lev->nodes[i];
        nd->x  = (fixed_t)wad_le16(r)     << FRACBITS;
        nd->y  = (fixed_t)wad_le16(r + 2) << FRACBITS;
        nd->dx = (fixed_t)wad_le16(r + 4) << FRACBITS;
        nd->dy = (fixed_t)wad_le16(r + 6) << FRACBITS;
        for (int c = 0; c < 2; c++)
            for (int b = 0; b < 4; b++)
                nd->bbox[c][b] = wad_le16(r + 8 + c * 8 + b * 2);
        nd->children[0] = (uint16_t)wad_le16(r + 24);
        nd->children[1] = (uint16_t)wad_le16(r + 26);
    }
    free(raw);

    assign_ss_sectors(lev);

    /* Reconstruct the floor/ceiling polygons. Budgeted generously: a convex
     * cell rarely exceeds a handful of vertices, but clipping can add one per
     * partition crossed. */
    lev->numpolypts = lev->numsubsectors * 10;
    lev->polypts = alloc_level((size_t)lev->numpolypts, sizeof *lev->polypts,
                               &lev->bytes);
    if (lev->polypts) {
        float xl = 1e9f, xh = -1e9f, yl = 1e9f, yh = -1e9f;
        for (int i = 0; i < lev->numvertexes; i++) {
            const float vx = (float)(lev->vertexes[i].x >> FRACBITS);
            const float vy = (float)(lev->vertexes[i].y >> FRACBITS);
            if (vx < xl) xl = vx;
            if (vx > xh) xh = vx;
            if (vy < yl) yl = vy;
            if (vy > yh) yh = vy;
        }
        xl -= 64.0f; yl -= 64.0f; xh += 64.0f; yh += 64.0f;

        vec2_t world[MAX_POLY_PTS] = {
            { xl, yl }, { xh, yl }, { xh, yh }, { xl, yh }
        };
        int cursor = 0;
        if (lev->numnodes)
            build_polys(lev, lev->numnodes - 1, world, 4, &cursor);
        else
            store_poly(lev, 0, world, 4, &cursor);

        int built = 0;
        for (int i = 0; i < lev->numsubsectors; i++)
            if (lev->subsectors[i].numpts >= 3) built++;
        verify_level = lev;
        debugf("level: %d/%d subsector polygons, %d points (%u KB)\n",
               built, lev->numsubsectors, cursor,
               (unsigned)(lev->numpolypts * sizeof *lev->polypts / 1024));
    }

    load_things(lev, marker);

    /* The sky texture is a wall texture by name, shared across a whole
     * episode. Loaded only when some sector actually opens onto it, so an
     * indoor map pays nothing. */
    lev->sky = NULL;
    for (int i = 0; i < lev->numsectors; i++) {
        if (!lev->sectors[i].skyceiling) continue;
        static const uint8_t skyname[8] = { 'S','K','Y','1',0,0,0,0 };
        lev->sky = p_level_texture(tex_resolve_pfx(skyname, ""));
        break;
    }

    return true;
}

/* THINGS records are 10 bytes: x, y, angle, type, flags. Only the player 1
 * start (type 1) is needed to place the camera. */
bool p_player_start(const char *mapname, float *x, float *y, float *angle_deg)
{
    const int marker = wad_find(mapname);
    if (marker < 0) return false;

    int n = 0;
    uint8_t *raw = load_lump(marker, "THINGS", 10, &n);
    if (!raw) return false;

    bool found = false;
    for (int i = 0; i < n; i++) {
        const uint8_t *r = raw + i * 10;
        if (wad_le16(r + 6) != 1) continue;      /* type 1 = player 1 start */
        *x = (float)wad_le16(r);
        *y = (float)wad_le16(r + 2);
        *angle_deg = (float)wad_le16(r + 4);
        found = true;
        break;
    }
    free(raw);
    return found;
}

/* Sprite for a thing type, for Doom's P_LoadThings. This lookup table is
 * a stand-in for info.c's mobjinfo[], which carries doomednum and spawnstate
 * properly; it goes away when the state tables land in Stage 2. */
dt64_tex_t *p_level_thing_sprite(int type)
{
    const char *base = sprite_for_type((int16_t)type);
    return base ? resolve_sprite(base) : NULL;
}

/* As p_level_resolve, but yields the texture itself and NULL when the name is
 * not in the ROM. Sprite lookup needs to distinguish "no such frame" from
 * "index 0", which the index form cannot express. */
void *p_level_resolve_ptr(const char *name, const char *prefix)
{
    const int idx = p_level_resolve(name, prefix);
    if (idx < 0) return NULL;
    dt64_tex_t *t = p_level_texture(idx);
    return t;
}

/* Drop every asset belonging to the level being replaced.
 *
 * Doom's own loader does not do this: it owns its geometry through the zone's
 * PU_LEVEL tag, but the baked textures here live in a separate arena that the
 * zone knows nothing about. A level change has to release both.
 */
void p_level_reset_assets(void)
{
    /* Everything that caches resolved dt64_tex_t pointers must drop them
     * with the arena: the lvl_tex slots get reused by the next level. */
    void V_DrawResetCache(void);
    void R_SpriteFrameReset(void);
    /* Emissive flats are recorded by resolved INDEX, which is exactly the kind
     * of stale reference the comment above warns about: keep it and the next
     * level's flat that lands in slot 7 inherits the last one's glow. */
    void D_FlatGlowReset(void);
#if R_ENVMAP
    /* Polished-wall textures are recorded by resolved INDEX, exactly as the
     * emissive flats are, so a stale table lets the next level's texture in
     * the same slot inherit the sheen. Reset with the table it mirrors. */
    void D_MirrorReset(void);
#endif

    mem_reset(MEM_ARENA_LEVEL);
    dt64_release_all();
    lvl_numtex = 0;
    tex_hash_reset();

    /* The animation table indexes into the texture slots being dropped,
     * and the name ordering lives in the texture arena that goes with
     * them, so both are stale from here. */
    numanims = 0;
    order_names = NULL;
    V_DrawResetCache();
    R_SpriteFrameReset();
#if D_DYNLIGHT
    D_FlatGlowReset();
#if R_ENVMAP
    D_MirrorReset();
#endif
#endif
}
