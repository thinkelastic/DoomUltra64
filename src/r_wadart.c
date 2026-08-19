#include "r_wadart.h"
#include "wad.h"
#include "mem.h"

#include <libdragon.h>
#include <stdlib.h>
#include <string.h>

/* Index 255 is the transparency key, as it was in the converter. It has to
 * be the same number on both sides: dt64_build_tluts gives that entry alpha
 * 0, so a texel carrying it is a hole rather than a colour. */
#define TRANSPARENT_INDEX 255

/* Doom's four namespaces are delimited by marker lumps rather than by name,
 * and flats and wall textures are allowed to share a name -- so a flat must
 * be looked up BETWEEN F_START and F_END, not with a bare wad_find that
 * might land on a wall. A merged stack has one region per WAD, so a handful
 * are kept and searched last-first, which is Doom's "later wins" again. */
#define REGION_MAX 8
typedef struct { int start, end; } region_t;

/* Mirrors dt64_last_absent, and for the same reason: a name the WAD simply
 * does not carry is a permanent answer worth remembering, while a failure to
 * allocate is a temporary one that must stay retryable -- or one momentarily
 * full arena hides that sprite for the rest of the level. */
int r_wadart_absent;

static region_t flat_rgn[REGION_MAX];  static int flat_rgn_n;
static region_t spr_rgn[REGION_MAX];   static int spr_rgn_n;

/* PNAMES and the TEXTURE directories stay resident. They are small -- 3 KB
 * and ~50 KB for Ultimate Doom -- and every wall in the level is looked up
 * through them, which is not a thing to re-read off a card each time. */
static uint8_t *pnames;   static int pnames_n;
static uint8_t *texdir[2]; static int texdir_sz[2];

static void cache_drop(void);      /* the composed-canvas cache, below */

/* --- lump plumbing ------------------------------------------------------ */

/* Read a whole lump into fresh malloc'd memory. Composition scratch only:
 * the arena is for what the RDP ends up reading, and these are freed as soon
 * as their pixels have been copied out. */
int r_wadart_io_us, r_wadart_io_n;    /* how much of composing is just reading */
int r_wadart_find_us, r_wadart_draw_us, r_wadart_mask_us, r_wadart_build_us;

static uint8_t *lump_load(int idx, int *size_out)
{
    const wad_lump_t *l = wad_lump(idx);
    if (!l || !l->size) return NULL;
    uint8_t *buf = malloc(l->size);
    if (!buf) return NULL;
    const uint32_t t0 = TICKS_READ();
    const bool ok = wad_read(idx, buf);
    r_wadart_io_us += (int)TICKS_TO_US(TICKS_DISTANCE(t0, TICKS_READ()));
    r_wadart_io_n++;
    if (!ok) { free(buf); return NULL; }
    if (size_out) *size_out = (int)l->size;
    return buf;
}

static void regions_find(const char *s1, const char *s2,
                         const char *e1, const char *e2,
                         region_t *out, int *count)
{
    *count = 0;
    int open = -1;
    for (int i = 0; i < wad_num_lumps(); i++) {
        const char *nm = wad_lump(i)->name;
        if (!strcasecmp(nm, s1) || !strcasecmp(nm, s2)) { open = i; continue; }
        if ((!strcasecmp(nm, e1) || !strcasecmp(nm, e2)) && open >= 0) {
            if (*count < REGION_MAX) {
                out[*count].start = open;
                out[*count].end   = i;
                (*count)++;
            }
            open = -1;
        }
    }
}

/* Last region first: a mod's copy of a name shadows the IWAD's. */
static int region_find(const region_t *rgn, int n, const char *name)
{
    for (int r = n - 1; r >= 0; r--)
        for (int i = rgn[r].end - 1; i > rgn[r].start; i--)
            if (!strcasecmp(wad_lump(i)->name, name)) return i;
    return -1;
}

void r_wadart_init(void)
{
    cache_drop();
    free(pnames);    pnames = NULL;    pnames_n = 0;
    free(texdir[0]); texdir[0] = NULL; texdir_sz[0] = 0;
    free(texdir[1]); texdir[1] = NULL; texdir_sz[1] = 0;

    regions_find("F_START", "FF_START", "F_END", "FF_END", flat_rgn, &flat_rgn_n);
    regions_find("S_START", "SS_START", "S_END", "SS_END", spr_rgn,  &spr_rgn_n);

    int sz = 0;
    const int pn = wad_find("PNAMES");
    if (pn >= 0 && (pnames = lump_load(pn, &sz)) && sz >= 4) {
        pnames_n = wad_le32(pnames);
        if (4 + pnames_n * 8 > sz) pnames_n = (sz - 4) / 8;
    }

    static const char *const dirs[2] = { "TEXTURE1", "TEXTURE2" };
    for (int d = 0; d < 2; d++) {
        const int t = wad_find(dirs[d]);
        if (t >= 0) texdir[d] = lump_load(t, &texdir_sz[d]);
    }

    debugf("wadart: %d flat region(s), %d sprite region(s), %d pnames, "
           "%d/%d texture dirs\n", flat_rgn_n, spr_rgn_n, pnames_n,
           texdir[0] ? 1 : 0, texdir[1] ? 1 : 0);
}

/* --- building a dt64_tex_t ---------------------------------------------- */

/* Copy composed pixels into the texture arena and describe them exactly as
 * dt64_load would, tile-major above one TMEM tile.
 *
 * Tile-major is not an optimisation to skip: a linear image wider than 64
 * makes each 64x32 upload 32 strided reads, the RDP's slow LOAD_TILE path.
 * Contiguous per tile, the same upload is one burst under LOAD_BLOCK. */
/* Verification composes a second copy of art the arena is already holding,
 * and the arena has peaked at 1914 of 2048 KB on the demo maps alone -- so
 * the copy goes on the heap, where it can be freed the moment it has been
 * compared. */
static bool use_heap;

static bool build(dt64_tex_t *t, const uint8_t *px, int w, int h, int masked,
                  int leftoffset, int topoffset)
{
    const uint32_t t_build = TICKS_READ();
    const int tilemajor = w * h > 2048;

    uint8_t *dst = use_heap ? malloc((size_t)w * h)
                            : mem_alloc(MEM_ARENA_TEXTURE, (size_t)w * h);
    if (!dst) return false;

    if (tilemajor) {
        size_t off = 0;
        for (int ty = 0; ty < h; ty += 32) {
            const int th = h - ty < 32 ? h - ty : 32;
            for (int tx = 0; tx < w; tx += 64) {
                const int tw = w - tx < 64 ? w - tx : 64;
                for (int row = 0; row < th; row++) {
                    memcpy(dst + off, px + (size_t)(ty + row) * w + tx, (size_t)tw);
                    off += (size_t)tw;
                }
            }
        }
    } else {
        memcpy(dst, px, (size_t)w * h);
    }

    t->width      = (uint16_t)w;
    t->height     = (uint16_t)h;
    t->flags      = (uint16_t)((masked ? 1 : 0) | (tilemajor ? 2 : 0));
    t->leftoffset = (int16_t)leftoffset;
    t->topoffset  = (int16_t)topoffset;
    t->texels     = dst;
    t->palbank    = 0;
    t->surface    = surface_make_linear(dst, FMT_CI8, (uint16_t)w, (uint16_t)h);
    t->tiles_x    = (uint8_t)((w + DT64_TILE_W - 1) / DT64_TILE_W);
    t->tiles_y    = (uint8_t)((h + DT64_TILE_H - 1) / DT64_TILE_H);
    t->mip        = NULL;
    r_wadart_build_us += (int)TICKS_TO_US(TICKS_DISTANCE(t_build, TICKS_READ()));
    return true;
}

/* --- the last full-size canvas ------------------------------------------
 *
 * The loader asks for a wall at full size and then immediately at half and
 * quarter, and a sprite at full size and then half. Composing each level from
 * the patches again re-reads every patch off the card and rebuilds the whole
 * canvas -- three times over for a wall, for two results that are just point
 * samples of the first. One entry is all it takes: the requests arrive back
 * to back, so the mip always finds the canvas its parent just left. */
static struct {
    char     name[9];
    int      kind;              /* 0 texture, 1 sprite */
    uint8_t *px;
    int      w, h, masked, lofs, tofs;
} canvas_cache;

static void cache_drop(void)
{
    free(canvas_cache.px);
    canvas_cache.px = NULL;
    canvas_cache.name[0] = '\0';
}

/* Takes ownership of `px`. */
static void cache_put(int kind, const char *name, uint8_t *px,
                      int w, int h, int masked, int lofs, int tofs)
{
    cache_drop();
    snprintf(canvas_cache.name, sizeof canvas_cache.name, "%s", name);
    canvas_cache.kind = kind;
    canvas_cache.px = px;
    canvas_cache.w = w; canvas_cache.h = h;
    canvas_cache.masked = masked;
    canvas_cache.lofs = lofs; canvas_cache.tofs = tofs;
}

static bool cache_hit(int kind, const char *name)
{
    return canvas_cache.px && canvas_cache.kind == kind &&
           !strcasecmp(canvas_cache.name, name);
}

/* Point sampling, not averaging: these are palette indices, and the mean of
 * two indices is an unrelated colour. */
static bool mip_from(dt64_tex_t *t, const uint8_t *px, int w, int h,
                     int level, int masked, int lofs, int tofs)
{
    if (level <= 0) return build(t, px, w, h, masked, lofs, tofs);

    const int mw = w >> level, mh = h >> level;
    if (mw < 1 || mh < 1) return false;

    uint8_t *mip = malloc((size_t)mw * mh);
    if (!mip) return false;
    for (int y = 0; y < mh; y++)
        for (int x = 0; x < mw; x++)
            mip[y * mw + x] = px[(y << level) * w + (x << level)];
    const bool ok = build(t, mip, mw, mh, masked, lofs >> level, tofs >> level);
    free(mip);
    return ok;
}

/* --- patches ------------------------------------------------------------ */

/* Doom stores a patch as columns of vertical runs, which is what made it
 * cheap to draw a masked column and awkward to hand to anything that wants a
 * rectangle. Unpack one into a canvas, marking which texels it covered. */
static void draw_patch(uint8_t *canvas, uint8_t *mask, int cw, int ch,
                       const uint8_t *patch, int patchsize, int ox, int oy)
{
    const uint32_t t_draw = TICKS_READ();
    if (patchsize < 8) return;

    const int pw = wad_le16(patch + 0);
    const int ph = wad_le16(patch + 2);
    if (pw <= 0 || ph <= 0) return;

    for (int x = 0; x < pw; x++) {
        const int cx = ox + x;
        if (cx < 0 || cx >= cw) continue;
        if (8 + (size_t)x * 4 + 4 > (size_t)patchsize) break;

        const uint32_t colofs = (uint32_t)wad_le32(patch + 8 + (size_t)x * 4);
        if (colofs >= (uint32_t)patchsize) continue;

        const uint8_t *post = patch + colofs;
        int prev_top = -1;

        while (post + 1 < patch + patchsize && *post != 0xFF) {
            int topdelta = post[0];
            const int length = post[1];

            /* "Tall patch" convention: once topdelta stops increasing it is
             * relative to the previous post rather than to the column top.
             * Vanilla IWADs never need it; DeHackEd-era PWADs do, and this
             * now has to survive whatever a player puts on the card. */
            if (prev_top >= 0 && topdelta <= prev_top) topdelta += prev_top;
            prev_top = topdelta;

            if (post + 4 + length > patch + patchsize) break;

            /* Clip the run once and then step by the row stride, rather
             * than recomputing cy * cw and re-testing the bounds for every
             * texel. A post is a COLUMN, so these writes walk down the
             * canvas a whole row apart and each one touches its own cache
             * line -- there is no fixing that here, but there is no reason
             * to pay for a multiply and a branch on top of the miss. */
            const uint8_t *src = post + 3;      /* skip the pad byte */
            int cy = oy + topdelta, i0 = 0, n = length;
            if (cy < 0)      { i0 = -cy; n -= i0; cy = 0; }
            if (cy + n > ch) { n = ch - cy; }
            if (n > 0) {
                uint8_t *cp = canvas + (size_t)cy * cw + cx;
                uint8_t *mp = mask   + (size_t)cy * cw + cx;
                const uint8_t *sp = src + i0;
                for (int i = 0; i < n; i++) {
                    *cp = sp[i]; *mp = 1;
                    cp += cw; mp += cw;
                }
            }
            post += 4 + length;
        }
    }
    r_wadart_draw_us += (int)TICKS_TO_US(TICKS_DISTANCE(t_draw, TICKS_READ()));
}

/* A lone patch -- a sprite frame or a piece of menu art -- as a texture.
 * Anything no post covered becomes the transparency key. */
static bool patch_tex(dt64_tex_t *t, int lump, int level, const char *name)
{
    if (level > 0 && cache_hit(1, name))
        return mip_from(t, canvas_cache.px, canvas_cache.w, canvas_cache.h,
                        level, canvas_cache.masked,
                        canvas_cache.lofs, canvas_cache.tofs);

    int psize = 0;
    uint8_t *patch = lump_load(lump, &psize);
    if (!patch) return false;
    if (psize < 8) { free(patch); return false; }

    const int pw = wad_le16(patch + 0), ph = wad_le16(patch + 2);
    const int lofs = wad_le16(patch + 4), tofs = wad_le16(patch + 6);
    if (pw <= 0 || ph <= 0 || pw > 512 || ph > 512) { free(patch); return false; }

    uint8_t *canvas = malloc((size_t)pw * ph);
    uint8_t *mask   = calloc((size_t)pw * ph, 1);
    if (!canvas || !mask) { free(canvas); free(mask); free(patch); return false; }
    memset(canvas, TRANSPARENT_INDEX, (size_t)pw * ph);

    draw_patch(canvas, mask, pw, ph, patch, psize, 0, 0);
    free(patch);
    free(mask);

    const bool ok = mip_from(t, canvas, pw, ph, level, 1, lofs, tofs);
    cache_put(1, name, canvas, pw, ph, 1, lofs, tofs);   /* keeps `canvas` */
    return ok;
}

bool r_wadart_sprite(dt64_tex_t *t, const char *name, int level)
{
    const int lump = region_find(spr_rgn, spr_rgn_n, name);
    if (lump < 0) { r_wadart_absent = 1; return false; }
    r_wadart_absent = 0;
    return patch_tex(t, lump, level, name);
}

bool r_wadart_ui(dt64_tex_t *t, const char *name)
{
    /* Menu and status-bar art sits outside every marker region, so a plain
     * backwards search is both correct and what "later wins" wants. */
    const int lump = wad_find(name);
    const wad_lump_t *l = lump >= 0 ? wad_lump(lump) : NULL;
    if (!l || l->size < 12) { r_wadart_absent = 1; return false; }
    r_wadart_absent = 0;
    return patch_tex(t, lump, 0, name);
}

/* --- flats -------------------------------------------------------------- */

bool r_wadart_flat(dt64_tex_t *t, const char *name)
{
    const int lump = region_find(flat_rgn, flat_rgn_n, name);
    const wad_lump_t *l = lump >= 0 ? wad_lump(lump) : NULL;
    if (!l || l->size != 4096) { r_wadart_absent = 1; return false; }
    r_wadart_absent = 0;

    uint8_t *src = lump_load(lump, NULL);
    if (!src) return false;

    /* When every texel shares a high nibble the flat fits TMEM whole as CI4,
     * at full 64x64 rather than downsampled -- the palette bank supplies the
     * nibble the texels no longer carry. */
    const int bank = src[0] >> 4;
    int uniform = 1;
    for (int k = 1; k < 4096; k++)
        if ((src[k] >> 4) != bank) { uniform = 0; break; }

    if (uniform) {
        uint8_t *dst = use_heap ? malloc(2048) : mem_alloc(MEM_ARENA_TEXTURE, 2048);
        if (!dst) { free(src); return false; }
        for (int i = 0; i < 4096; i += 2)
            dst[i >> 1] = (uint8_t)(((src[i] & 15) << 4) | (src[i + 1] & 15));
        free(src);

        t->width = t->height = 64;
        t->flags = (uint16_t)(4u | ((unsigned)bank << 8));
        t->leftoffset = t->topoffset = 0;
        t->texels  = dst;
        t->palbank = (uint8_t)bank;
        t->surface = surface_make_linear(dst, FMT_CI4, 64, 64);
        t->tiles_x = t->tiles_y = 1;
        t->mip     = NULL;
        return true;
    }

    uint8_t small[32 * 32];
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            small[y * 32 + x] = src[(y * 2) * 64 + (x * 2)];
    free(src);
    return build(t, small, 32, 32, 0, 0, 0);
}

/* --- wall textures ------------------------------------------------------ */

/* Find a texture's entry in TEXTURE1/TEXTURE2. Later directory first, so a
 * mod's TEXTURE1 shadows the IWAD's. */
static const uint8_t *texentry(const char *name, int *size_left)
{
    const uint32_t t0 = TICKS_READ();
    #define TEXENTRY_RET(v) do { \
        r_wadart_find_us += (int)TICKS_TO_US(TICKS_DISTANCE(t0, TICKS_READ())); \
        return (v); } while (0)
    for (int d = 1; d >= 0; d--) {
        if (!texdir[d]) continue;
        const uint8_t *tex = texdir[d];
        const int      tsz = texdir_sz[d];
        if (tsz < 4) continue;

        const int numtex = wad_le32(tex);
        for (int i = 0; i < numtex; i++) {
            if (4 + (size_t)i * 4 + 4 > (size_t)tsz) break;
            const uint32_t tofs = (uint32_t)wad_le32(tex + 4 + (size_t)i * 4);
            if (tofs + 22 > (uint32_t)tsz) continue;
            if (!strncasecmp((const char *)tex + tofs, name, 8)) {
                /* An 8-byte field is not NUL-terminated when full, so a
                 * shorter query must not match a longer name by prefix. */
                const char *tn = (const char *)tex + tofs;
                int n = 0; while (n < 8 && tn[n]) n++;
                if ((int)strlen(name) != n) continue;
                *size_left = tsz - (int)tofs;
                TEXENTRY_RET(tex + tofs);
            }
        }
    }
    TEXENTRY_RET(NULL);
    #undef TEXENTRY_RET
}

bool r_wadart_texture(dt64_tex_t *t, const char *name, int level)
{
    if (level > 0 && cache_hit(0, name))
        return mip_from(t, canvas_cache.px, canvas_cache.w, canvas_cache.h,
                        level, canvas_cache.masked, 0, 0);

    int left = 0;
    const uint8_t *e = texentry(name, &left);
    if (!e || !pnames) { r_wadart_absent = 1; return false; }
    r_wadart_absent = 0;

    const int tw = wad_le16(e + 12), th = wad_le16(e + 14);
    const int npatch = wad_le16(e + 20);
    if (tw <= 0 || th <= 0 || tw > 1024 || th > 1024) return false;

    uint8_t *canvas = calloc((size_t)tw * th, 1);
    uint8_t *mask   = calloc((size_t)tw * th, 1);
    if (!canvas || !mask) { free(canvas); free(mask); return false; }

    for (int p = 0; p < npatch; p++) {
        if (22 + (size_t)p * 10 + 10 > (size_t)left) break;
        const uint8_t *pd = e + 22 + (size_t)p * 10;
        const int ox  = wad_le16(pd + 0);
        const int oy  = wad_le16(pd + 2);
        const int idx = wad_le16(pd + 4);
        if (idx < 0 || idx >= pnames_n) continue;

        char pname[9] = {0};
        memcpy(pname, pnames + 4 + (size_t)idx * 8, 8);

        const int pl = wad_find(pname);
        if (pl < 0) continue;
        int psize = 0;
        uint8_t *patch = lump_load(pl, &psize);
        if (!patch) continue;
        draw_patch(canvas, mask, tw, th, patch, psize, ox, oy);
        free(patch);
    }

    /* A texture is masked if any texel was never written by a patch -- and
     * those must carry the transparency key, not the zero calloc left. Index
     * 0 is opaque black in PLAYPAL, which is how a fence renders as a solid
     * slab when its gaps are never marked. */
    const uint32_t t_mask = TICKS_READ();
    int masked = 0;
    for (size_t k = 0; k < (size_t)tw * th; k++)
        if (!mask[k]) { canvas[k] = TRANSPARENT_INDEX; masked = 1; }
    free(mask);
    r_wadart_mask_us += (int)TICKS_TO_US(TICKS_DISTANCE(t_mask, TICKS_READ()));

    const bool ok = mip_from(t, canvas, tw, th, level, masked, 0, 0);
    cache_put(0, name, canvas, tw, th, masked, 0, 0);    /* keeps `canvas` */
    return ok;
}

/* --- enumeration, for the animation tables ------------------------------ */

int r_wadart_numtextures(void)
{
    int n = 0;
    for (int d = 0; d < 2; d++)
        if (texdir[d] && texdir_sz[d] >= 4) n += wad_le32(texdir[d]);
    return n;
}

const char *r_wadart_texname(int i)
{
    static char nm[9];
    for (int d = 0; d < 2; d++) {
        if (!texdir[d] || texdir_sz[d] < 4) continue;
        const int numtex = wad_le32(texdir[d]);
        if (i >= numtex) { i -= numtex; continue; }
        const uint32_t tofs = (uint32_t)wad_le32(texdir[d] + 4 + (size_t)i * 4);
        if (tofs + 8 > (uint32_t)texdir_sz[d]) return NULL;
        memcpy(nm, texdir[d] + tofs, 8);
        nm[8] = '\0';
        return nm;
    }
    return NULL;
}

int r_wadart_numflats(void)
{
    int n = 0;
    for (int r = 0; r < flat_rgn_n; r++)
        n += flat_rgn[r].end - flat_rgn[r].start - 1;
    return n;
}

const char *r_wadart_flatname(int i)
{
    for (int r = 0; r < flat_rgn_n; r++) {
        const int n = flat_rgn[r].end - flat_rgn[r].start - 1;
        if (i < n) return wad_lump(flat_rgn[r].start + 1 + i)->name;
        i -= n;
    }
    return NULL;
}

/* --- verification -------------------------------------------------------
 *
 * Compose what the converter already baked and compare the bytes. Two
 * independent implementations agreeing on every texel of a level's whole art
 * set is a far stronger claim than either one looking right on screen, and
 * it is the only way to be sure the runtime path can replace the build one.
 */
int r_wadart_verify_ok, r_wadart_verify_bad;

void r_wadart_verify(const char *prefix, const char *name,
                     const dt64_tex_t *baked)
{
    if (!baked || !baked->texels) return;

    dt64_tex_t mine;
    memset(&mine, 0, sizeof mine);

    use_heap = true;
    bool got;
    if      (!prefix[0])        got = r_wadart_texture(&mine, name, 0);
    else if (prefix[0] == 'f')  got = r_wadart_flat(&mine, name);
    else if (prefix[0] == 's')  got = r_wadart_sprite(&mine, name, 0);
    else if (prefix[0] == 'n')  got = r_wadart_sprite(&mine, name, 1);
    else if (prefix[0] == 'm')  got = r_wadart_texture(&mine, name, 1);
    else if (prefix[0] == 'q')  got = r_wadart_texture(&mine, name, 2);
    else if (prefix[0] == 'u')  got = r_wadart_ui(&mine, name);
    else                        got = false;
    use_heap = false;

    if (!got) {
        debugf("VERIFY %s%s: composed nothing (baked %dx%d)\n",
               prefix, name, baked->width, baked->height);
        r_wadart_verify_bad++;
        return;
    }

    const char *why = NULL;
    if (mine.width  != baked->width)  why = "width";
    else if (mine.height != baked->height) why = "height";
    else if (mine.flags  != baked->flags)  why = "flags";
    else if (mine.leftoffset != baked->leftoffset) why = "leftoffset";
    else if (mine.topoffset  != baked->topoffset)  why = "topoffset";

    if (!why) {
        const size_t n = (mine.flags & 4) ? 2048u
                                          : (size_t)mine.width * mine.height;
        for (size_t k = 0; k < n; k++)
            if (mine.texels[k] != baked->texels[k]) {
                debugf("VERIFY %s%s: texel %u differs, mine %02x baked %02x\n",
                       prefix, name, (unsigned)k, mine.texels[k], baked->texels[k]);
                why = "texels";
                break;
            }
    }

    if (why) {
        debugf("VERIFY %s%s: %s MISMATCH (mine %dx%d f%04x, baked %dx%d f%04x)\n",
               prefix, name, why, mine.width, mine.height, mine.flags,
               baked->width, baked->height, baked->flags);
        r_wadart_verify_bad++;
    } else {
        r_wadart_verify_ok++;
    }
    free(mine.texels);
}
