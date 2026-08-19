/*
 * wad2n64 -- host-side Doom WAD to N64 asset converter.
 *
 * Doom wall textures are stored as a composition recipe (TEXTURE1) over a set
 * of column-major patches (PNAMES). Assembling them costs both CPU and RAM,
 * neither of which the N64 has to spare, so all composition happens here at
 * build time. The ROM ships flat, ready-to-DMA CI8 texel data.
 *
 * Doom art is already 256-colour indexed, which maps onto the RDP's CI8 format
 * with a single shared TLUT taken from PLAYPAL. Keeping one global palette (as
 * opposed to per-texture palettes) means Doom's palette effects -- damage red,
 * item pickup, invulnerability -- become a 512-byte TLUT swap rather than a
 * re-upload of every texture in TMEM.
 *
 * Output is big-endian to match the VR4300.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

/* ---------------------------------------------------------------- WAD types */

typedef struct {
    char     magic[4];      /* "IWAD" or "PWAD" */
    int32_t  numlumps;
    int32_t  infotableofs;
} wadheader_t;

typedef struct {
    int32_t  filepos;
    int32_t  size;
    char     name[8];       /* not NUL-terminated when all 8 bytes are used */
} waddirent_t;

typedef struct {
    uint8_t *data;
    size_t   size;
    waddirent_t *dir;
    int32_t  numlumps;
} wad_t;

/* --------------------------------------------------------- little-endian in */
/* WAD files are little-endian regardless of host, so read them explicitly. */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static int16_t  rds16(const uint8_t *p) { return (int16_t)rd16(p); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* --------------------------------------------------------- big-endian out */
/* The N64 is big-endian; emit in target byte order so the ROM can DMA and use
 * the data directly with no runtime byte swapping. */

static void wr16be(FILE *f, uint16_t v) { fputc(v >> 8, f); fputc(v & 0xFF, f); }

/* ------------------------------------------------------------- WAD loading */

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "wad2n64: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static int wad_open(wad_t *w, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open '%s'", path);

    fseek(f, 0, SEEK_END);
    w->size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);

    w->data = malloc(w->size);
    if (!w->data) die("out of memory reading '%s'", path);
    if (fread(w->data, 1, w->size, f) != w->size) die("short read on '%s'", path);
    fclose(f);

    if (w->size < 12 || (memcmp(w->data, "IWAD", 4) && memcmp(w->data, "PWAD", 4)))
        die("'%s' is not a WAD file", path);

    w->numlumps = (int32_t)rd32(w->data + 4);
    uint32_t ofs = rd32(w->data + 8);
    if (ofs + (size_t)w->numlumps * 16 > w->size) die("WAD directory out of bounds");

    /* The on-disk directory is already the layout we want, but the integer
     * fields need host-order fixup, so build a decoded copy. */
    w->dir = calloc((size_t)w->numlumps, sizeof(waddirent_t));
    if (!w->dir) die("out of memory for WAD directory");
    for (int32_t i = 0; i < w->numlumps; i++) {
        const uint8_t *e = w->data + ofs + (size_t)i * 16;
        w->dir[i].filepos = (int32_t)rd32(e);
        w->dir[i].size    = (int32_t)rd32(e + 4);
        memcpy(w->dir[i].name, e + 8, 8);
    }
    return 0;
}

/* Stack a PWAD on top of an already-open WAD.
 *
 * The two files' bytes go in one buffer and the two directories in one array,
 * mod last -- which is all "later wins" needs, because wad_lump already
 * searches backwards and the emitters overwrite a file when they meet its
 * name a second time. It is the same merge src/wad.c performs on the console,
 * for the same reason, and it is what puts a mod's own textures and sprites
 * in the ROM instead of leaving its walls blank.
 */
static void wad_append(wad_t *w, const char *path) {
    wad_t p;
    wad_open(&p, path);

    const size_t base = w->size;
    uint8_t *grown = realloc(w->data, w->size + p.size);
    if (!grown) die("out of memory merging '%s'", path);
    memcpy(grown + base, p.data, p.size);
    w->data = grown;
    w->size += p.size;

    waddirent_t *dir = realloc(w->dir,
                               (size_t)(w->numlumps + p.numlumps) * sizeof *dir);
    if (!dir) die("out of memory merging '%s' directory", path);
    w->dir = dir;
    for (int32_t i = 0; i < p.numlumps; i++) {
        w->dir[w->numlumps + i] = p.dir[i];
        w->dir[w->numlumps + i].filepos += (int32_t)base;
    }
    w->numlumps += p.numlumps;

    printf("wad2n64: + %s (%d lumps, %d total)\n", path, p.numlumps, w->numlumps);
    free(p.data);
    free(p.dir);
}

/* Doom lump names are 8 bytes, upper-case, NUL-padded only if shorter. */
static int lump_name_eq(const char *lump8, const char *want) {
    for (int i = 0; i < 8; i++) {
        char a = lump8[i];
        char b = want[i] ? want[i] : 0;
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return 0;
        if (!b) return 1;
    }
    return 1;
}

static const uint8_t *wad_lump(wad_t *w, const char *name, int32_t *size_out) {
    /* Search backwards: later lumps (from PWADs) override earlier ones. */
    for (int32_t i = w->numlumps - 1; i >= 0; i--) {
        if (lump_name_eq(w->dir[i].name, name)) {
            if (size_out) *size_out = w->dir[i].size;
            if ((size_t)(w->dir[i].filepos + w->dir[i].size) > w->size)
                die("lump '%s' extends past end of WAD", name);
            return w->data + w->dir[i].filepos;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------- PLAYPAL */

/* Emit palette 0 of PLAYPAL as an RDP TLUT in RGBA5551.
 *
 * The RDP samples CI8 through a 256-entry TLUT of 16-bit colours. We use
 * RGBA16 (RGBA5551) rather than IA16: one bit of alpha is all Doom needs,
 * since transparency is per-post, not per-texel. */
#define DT64_TRANSPARENT_INDEX 255

static void emit_tlut(wad_t *w, const char *outdir) {
    int32_t sz = 0;
    const uint8_t *pal = wad_lump(w, "PLAYPAL", &sz);
    if (!pal) die("WAD has no PLAYPAL lump");
    if (sz < 768) die("PLAYPAL too small (%d bytes)", sz);

    /* All 14 palettes, back to back: the runtime picks a 512-byte bank per
     * frame, which is how the damage/pickup/invulnerability flashes work --
     * a TLUT swap instead of any per-pixel recolouring. */
    const int npals = sz / 768 < 14 ? sz / 768 : 14;

    char path[1024];
    snprintf(path, sizeof path, "%s/playpal.tlut", outdir);
    FILE *f = fopen(path, "wb");
    if (!f) die("cannot write '%s'", path);

    for (int p = 0; p < npals; p++)
    for (int i = 0; i < 256; i++) {
        uint8_t r = pal[p * 768 + i * 3 + 0],
                g = pal[p * 768 + i * 3 + 1],
                b = pal[p * 768 + i * 3 + 2];
        /* 8-bit -> 5-bit by taking the high bits.
         *
         * Index 255 is the transparency key. Doom stores sprite transparency
         * as gaps between posts, which vanishes once a patch is flattened into
         * a rectangle, so one palette entry has to stand in for "nothing here".
         * 255 is safe: no sprite in the IWAD uses it. Walls and flats are
         * unaffected because their combiner never reads texture alpha. */
        uint16_t a = (i == DT64_TRANSPARENT_INDEX) ? 0 : 1;

        /* The key entry is forced BLACK as well as transparent, in every
         * palette bank. Nothing in the IWAD's art uses index 255 -- checked
         * across every patch and flat -- so no colour is lost, and the two
         * consumers both want black: a masked midtexture cuts these texels
         * away, while the same texture drawn as an ordinary wall has no
         * alpha compare and shows them, where PLAYPAL's own 255 would paint
         * pink across the uncovered top of half the textures in the game.
         * Black is also the better neighbour for the bilinear filter along
         * a fence's cut edges. */
        if (i == DT64_TRANSPARENT_INDEX) r = g = b = 0;

        uint16_t c = (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) |
                                ((b >> 3) << 1) | a);
        wr16be(f, c);
    }
    fclose(f);
    printf("  [TLUT ] %s/playpal.tlut (%d palettes, RGBA5551)\n", outdir, npals);
}

/* --------------------------------------------------------- patch decoding */

/* Draw one Doom patch into an 8-bit indexed canvas at (ox, oy).
 *
 * Patch layout: a header of width/height/offsets, then per-column a chain of
 * "posts" (topdelta, length, pad, texels[length], pad) ending at topdelta 0xFF.
 * Gaps between posts are transparent and left untouched, which is how Doom
 * represents masked textures such as grates and fences. */
static void draw_patch(uint8_t *canvas, uint8_t *mask, int cw, int ch,
                       const uint8_t *patch, int32_t patchsize, int ox, int oy) {
    if (patchsize < 8) die("patch lump too small");

    int pw = rds16(patch + 0);
    int ph = rds16(patch + 2);
    if (pw <= 0 || ph <= 0) die("patch has bad dimensions %dx%d", pw, ph);

    for (int x = 0; x < pw; x++) {
        int cx = ox + x;
        if (cx < 0 || cx >= cw) continue;                 /* clipped horizontally */

        uint32_t colofs = rd32(patch + 8 + (size_t)x * 4);
        if (colofs >= (uint32_t)patchsize) die("patch column %d out of bounds", x);

        const uint8_t *post = patch + colofs;
        int prev_top = -1;

        while (post < patch + patchsize && *post != 0xFF) {
            int topdelta = post[0];
            int length   = post[1];

            /* "Tall patch" convention: once topdelta stops increasing it is
             * relative to the previous post rather than to the column top.
             * Vanilla IWADs never need this, but DeHackEd-era PWADs do. */
            if (prev_top >= 0 && topdelta <= prev_top) topdelta += prev_top;
            prev_top = topdelta;

            if (post + 4 + length > patch + patchsize)
                die("patch post overruns lump at column %d", x);

            const uint8_t *src = post + 3;                /* skip the pad byte */
            for (int i = 0; i < length; i++) {
                int cy = oy + topdelta + i;
                if (cy < 0 || cy >= ch) continue;         /* clipped vertically */
                canvas[(size_t)cy * cw + cx] = src[i];
                mask[(size_t)cy * cw + cx] = 1;
            }
            post += 4 + length;                           /* topdelta,len,pad,data,pad */
        }
    }
}

/* ------------------------------------------------------ texture composition */

/* Resolve a texture by name through TEXTURE1/TEXTURE2 and compose it. */
static uint8_t *compose_texture(wad_t *w, const char *texname,
                                int *out_w, int *out_h, int *out_masked) {
    /* PNAMES maps patch indices used by TEXTUREx to actual lump names. */
    int32_t pnsize = 0;
    const uint8_t *pnames = wad_lump(w, "PNAMES", &pnsize);
    if (!pnames) die("WAD has no PNAMES lump");
    uint32_t numpnames = rd32(pnames);
    if (4 + numpnames * 8 > (uint32_t)pnsize) die("PNAMES truncated");

    const char *dirs[] = { "TEXTURE1", "TEXTURE2" };
    for (int d = 0; d < 2; d++) {
        int32_t tsize = 0;
        const uint8_t *tex = wad_lump(w, dirs[d], &tsize);
        if (!tex) continue;

        int32_t numtex = (int32_t)rd32(tex);
        for (int32_t i = 0; i < numtex; i++) {
            uint32_t tofs = rd32(tex + 4 + (size_t)i * 4);
            if (tofs + 22 > (uint32_t)tsize) die("%s entry %d out of bounds", dirs[d], i);
            const uint8_t *t = tex + tofs;
            if (!lump_name_eq((const char *)t, texname)) continue;

            int tw = rds16(t + 12);
            int th = rds16(t + 14);
            int npatch = rds16(t + 20);
            if (tw <= 0 || th <= 0) die("texture '%s' has bad size %dx%d", texname, tw, th);

            uint8_t *canvas = calloc((size_t)tw * th, 1);
            uint8_t *mask   = calloc((size_t)tw * th, 1);
            if (!canvas || !mask) die("out of memory composing '%s'", texname);

            for (int p = 0; p < npatch; p++) {
                const uint8_t *pd = t + 22 + (size_t)p * 10;
                int ox  = rds16(pd + 0);
                int oy  = rds16(pd + 2);
                int idx = rds16(pd + 4);
                if (idx < 0 || (uint32_t)idx >= numpnames)
                    die("texture '%s' references patch %d out of range", texname, idx);

                char pname[9] = {0};
                memcpy(pname, pnames + 4 + (size_t)idx * 8, 8);

                int32_t psize = 0;
                const uint8_t *patch = wad_lump(w, pname, &psize);
                if (!patch) die("texture '%s' needs missing patch '%s'", texname, pname);

                draw_patch(canvas, mask, tw, th, patch, psize, ox, oy);
            }

            /* A texture is masked if any texel was never written by a
             * patch -- and those texels must carry the transparent index,
             * not the zero calloc left behind. Index 0 is opaque black in
             * PLAYPAL, which is exactly how a fence renders when its gaps
             * are never marked: a solid black slab. The mask says which
             * texels no patch covered, so this is the honest answer rather
             * than a guess from the colour. */
            int masked = 0;
            for (size_t k = 0; k < (size_t)tw * th; k++)
                if (!mask[k]) { canvas[k] = DT64_TRANSPARENT_INDEX; masked = 1; }
            free(mask);

            *out_w = tw; *out_h = th; *out_masked = masked;
            return canvas;
        }
    }
    die("texture '%s' not found in TEXTURE1/TEXTURE2", texname);
    return NULL;
}

/* ------------------------------------------------------------------ output */

/*
 * .dt64 -- a flat CI8 texture.
 *
 *   char     magic[4]   "DT64"
 *   uint16   version    2
 *   uint16   width
 *   uint16   height
 *   uint16   flags      bit 0: masked (has transparent texels)
 *   uint32   reserved   0
 *   uint8    texels[width*height]     palette indices into the shared TLUT
 *
 * The header is padded to 16 bytes for a reason: the runtime keeps the file in
 * one allocation and points the RDP straight at the texels inside it, and the
 * RDP requires texture data to be 8-byte aligned. A 12-byte header would land
 * the texels at offset 12 and corrupt every upload.
 *
 * Texels are stored linearly. The runtime wraps this in a libdragon surface_t
 * and uploads 64x32 sub-rectangles, which is the largest CI8 tile that fits
 * the 4 KB TMEM alongside the 2 KB TLUT.
 */
#define DT64_HEADER_SIZE 16

/* Patch offsets for the texture currently being written. Sprites are drawn
 * relative to a point in the world, and Doom stores where that point sits
 * inside the image -- horizontally the centre, vertically the feet. */
static int g_leftoffset, g_topoffset;

static void emit_texture_impl(const char *outdir, const char *name,
                              const uint8_t *texels, int w, int h, int masked,
                              int verbose) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.dt64", outdir, name);
    FILE *f = fopen(path, "wb");
    if (!f) die("cannot write '%s'", path);

    /* Tile-major storage for textures larger than one TMEM tile.
     *
     * The runtime uploads 64x32 sub-rectangles. Cut from a linear image
     * wider than 64, each upload is 32 strided reads of 64 bytes -- the
     * RDP's slow LOAD_TILE path. Stored tile-major (each 64x32 tile
     * contiguous, tiles in row-major order), the same upload is one
     * contiguous burst and takes LOAD_BLOCK, roughly halving its RDRAM
     * time. Textures that fit TMEM whole stay linear: they are uploaded in
     * one piece by the wrap fast path, which needs the linear layout.
     * Offsets are uniform because only the last tile row/column can be
     * partial: tile (tx,ty) starts at w*ty*32 + tx*64*rowheight. */
    const int tilemajor = w * h > 2048;
    uint8_t *tiled = NULL;
    if (tilemajor) {
        tiled = malloc((size_t)w * h);
        if (!tiled) die("out of memory tiling '%s'", name);
        size_t off = 0;
        for (int ty = 0; ty < h; ty += 32) {
            const int th = h - ty < 32 ? h - ty : 32;
            for (int tx = 0; tx < w; tx += 64) {
                const int tw = w - tx < 64 ? w - tx : 64;
                for (int row = 0; row < th; row++) {
                    memcpy(tiled + off,
                           texels + (size_t)(ty + row) * w + tx,
                           (size_t)tw);
                    off += (size_t)tw;
                }
            }
        }
    }

    fwrite("DT64", 1, 4, f);
    wr16be(f, 3);
    wr16be(f, (uint16_t)w);
    wr16be(f, (uint16_t)h);
    wr16be(f, (uint16_t)((masked ? 1 : 0) | (tilemajor ? 2 : 0)));
    /* Sprite anchor, from the patch header. Zero for walls and flats, which
     * are positioned by geometry rather than hung off a point in the world. */
    wr16be(f, (uint16_t)(int16_t)g_leftoffset);
    wr16be(f, (uint16_t)(int16_t)g_topoffset);
    fwrite(tilemajor ? tiled : texels, 1, (size_t)w * h, f);
    free(tiled);
    fclose(f);

    if (verbose) {
        int tiles_x = (w + 63) / 64, tiles_y = (h + 31) / 32;
        printf("  [TEX  ] %s/%s.dt64  %dx%d CI8, %d bytes, %dx%d TMEM tiles%s\n",
               outdir, name, w, h, w * h, tiles_x, tiles_y, masked ? ", masked" : "");
    }
}

static void emit_texture(const char *outdir, const char *name,
                         const uint8_t *texels, int w, int h, int masked) {
    emit_texture_impl(outdir, name, texels, w, h, masked, 1);
}

static void emit_texture_quiet(const char *outdir, const char *name,
                               const uint8_t *texels, int w, int h, int masked) {
    emit_texture_impl(outdir, name, texels, w, h, masked, 0);
}

/* Emit every texture defined in TEXTUREx.
 *
 * The whole set is ~3.6 MB for Ultimate Doom, which is nothing against a
 * cartridge but far too much for RDRAM. Baking all of them keeps any map
 * loadable; the runtime pulls in only the ones a level's sidedefs actually
 * name, which is 34 textures (~449 KB) for E1M1. */
/* The order textures and flats appear in, which is the only thing that can
 * resolve an animation range.
 *
 * Doom animates by naming the first and last frame and walking everything
 * between them in WAD order -- and that really is order, not arithmetic on
 * the name: FIREWALL runs FIREWALA, FIREWALB, FIREWALL. The runtime cannot
 * work it out for itself because it never sees TEXTURE1 or the flat
 * markers; it only ever asks for baked art by name. So the order is
 * written out here, once, and the runtime resolves the ranges against it
 * using Doom's own animdefs table.
 *
 * Layout: uint16 count, then count 8-byte names, flats first then
 * textures, each section with its own count. */
static FILE *order_file;
static long  order_count_pos;
static int   order_count;

static void order_begin(const char *outdir, const char *what)
{
    if (!order_file) {
        char path[1024];
        snprintf(path, sizeof path, "%s/texorder.bin", outdir);
        order_file = fopen(path, "wb");
        if (!order_file) die("cannot write %s/texorder.bin", outdir);
    }
    (void)what;
    order_count_pos = ftell(order_file);
    wr16be(order_file, 0);
    order_count = 0;
}

static void order_add(const char *name)
{
    char buf[8] = {0};
    for (int i = 0; i < 8 && name[i]; i++) buf[i] = name[i];
    fwrite(buf, 1, 8, order_file);
    order_count++;
}

static void order_end(void)
{
    const long here = ftell(order_file);
    fseek(order_file, order_count_pos, SEEK_SET);
    wr16be(order_file, (uint16_t)order_count);
    fseek(order_file, here, SEEK_SET);
}

static void emit_all_textures(wad_t *w, const char *outdir) {
    int32_t pnsize = 0;
    if (!wad_lump(w, "PNAMES", &pnsize)) die("WAD has no PNAMES lump");

    int total = 0;
    const char *dirs[] = { "TEXTURE1", "TEXTURE2" };
    for (int d = 0; d < 2; d++) {
        int32_t tsize = 0;
        const uint8_t *tex = wad_lump(w, dirs[d], &tsize);
        if (!tex) continue;

        int32_t numtex = (int32_t)rd32(tex);
        for (int32_t i = 0; i < numtex; i++) {
            uint32_t tofs = rd32(tex + 4 + (size_t)i * 4);
            if (tofs + 22 > (uint32_t)tsize) continue;

            char name[9] = {0};
            memcpy(name, tex + tofs, 8);

            order_add(name);

            int tw, th, masked;
            uint8_t *texels = compose_texture(w, name, &tw, &th, &masked);
            emit_texture_quiet(outdir, name, texels, tw, th, masked);

            /* Half-resolution level.
             *
             * TMEM holds 2048 CI8 texels beside the palette, so a 128x128
             * texture is always eight tiles and eight quads -- even when the
             * wall is ten pixels tall on screen. At half size it is two. The
             * renderer picks this level once a wall is minified, which is both
             * cheaper and less aliased; the full-resolution level is still
             * there for surfaces close enough to magnify.
             *
             * Point sampling, not averaging: these are palette indices, and
             * the mean of two indices is an unrelated colour. */
            for (int level = 1; level <= 2; level++) {
                const int shift = level;
                const int mw = tw >> shift, mh = th >> shift;
                if (mw < 1 || mh < 1) break;

                uint8_t *mip = malloc((size_t)mw * mh);
                if (!mip) break;
                for (int y = 0; y < mh; y++)
                    for (int x = 0; x < mw; x++)
                        mip[y * mw + x] = texels[(y << shift) * tw + (x << shift)];

                char mname[32];
                snprintf(mname, sizeof mname, "%c_%s", level == 1 ? 'm' : 'q', name);
                emit_texture_quiet(outdir, mname, mip, mw, mh, masked);
                free(mip);
            }

            free(texels);
            total++;
        }
    }
    printf("  [TEX  ] %d wall textures composed into %s/\n", total, outdir);
}

/* Emit every flat between F_START and F_END: full 64x64 CI4 where the art
 * allows it, 32x32 CI8 downsample where it does not.
 *
 * Doom flats are 64x64 raw palette indices -- 4096 bytes, against the 2048
 * of TMEM left once a TLUT is resident. The old note here rejected CI4 on
 * the belief that its 16-entry sub-palettes could not coexist with the
 * 256-entry PLAYPAL the walls sample. That was wrong about the hardware:
 * a CI4 tile's palette field selects a 16-entry BANK of whatever TLUT is
 * resident -- bank N reads entries N*16..N*16+15 of the same 256-entry
 * table. So a flat whose every index shares one high nibble is CI4 with
 * bank = that nibble and texel = the low one: no new palette uploads, no
 * TLUT swap, and the runtime flash palettes recolour it for free. 59 of
 * the IWAD's 107 flats qualify losslessly (index runs in PLAYPAL are
 * 16-aligned by its construction); the rest keep the downsample:
 * box-filtering in palette space is meaningless, so it picks one of the
 * four source texels rather than averaging indices. */
static int g_no_ci4;

/* Packed two texels per byte, left texel in the high nibble -- the RDP's
 * CI4 order. 2048 bytes: exactly the TMEM budget beside the TLUT, one
 * upload, wraps whole like the CI8 flats do. */
static void emit_flat_ci4(const char *outdir, const char *name,
                          const uint8_t *src, int bank) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.dt64", outdir, name);
    FILE *f = fopen(path, "wb");
    if (!f) die("cannot write '%s'", path);

    fwrite("DT64", 1, 4, f);
    wr16be(f, 4);                              /* version 4: CI4 payload */
    wr16be(f, 64);
    wr16be(f, 64);
    wr16be(f, (uint16_t)(4u | ((unsigned)bank << 8)));  /* bit2 CI4; bank */
    wr16be(f, 0);
    wr16be(f, 0);
    for (int i = 0; i < 4096; i += 2)
        fputc(((src[i] & 15) << 4) | (src[i + 1] & 15), f);
    fclose(f);
}

static void emit_all_flats(wad_t *w, const char *outdir) {
    /* Every marker region, not just the last one. A merged stack has one per
     * WAD, and taking only the final pair -- which is what a "remember the
     * last F_START" scan does -- would emit the mod's flats and silently drop
     * the IWAD's. Later regions still win, because emitting a name twice
     * overwrites the first file. PWADs conventionally open theirs FF_START. */
    uint8_t small[32 * 32];
    int total = 0, ci4 = 0, inside = 0, seen = 0;
    for (int32_t i = 0; i < w->numlumps; i++) {
        if (lump_name_eq(w->dir[i].name, "F_START") ||
            lump_name_eq(w->dir[i].name, "FF_START")) { inside = 1; seen = 1; continue; }
        if (lump_name_eq(w->dir[i].name, "F_END") ||
            lump_name_eq(w->dir[i].name, "FF_END"))   { inside = 0; continue; }
        if (!inside) continue;
        if (w->dir[i].size != 4096) continue;          /* markers, oddities */

        order_add(w->dir[i].name);

        const uint8_t *src = w->data + w->dir[i].filepos;

        char name[9] = {0};
        memcpy(name, w->dir[i].name, 8);

        /* Flats and wall textures are separate namespaces in Doom and may
         * share a name, so keep them apart in the filesystem. */
        char fname[32];
        snprintf(fname, sizeof fname, "f_%s", name);

        if (!g_no_ci4) {
            const int bank = src[0] >> 4;
            int uniform = 1;
            for (int k = 1; k < 4096; k++)
                if ((src[k] >> 4) != bank) { uniform = 0; break; }
            if (uniform) {
                emit_flat_ci4(outdir, fname, src, bank);
                ci4++; total++;
                continue;
            }
        }

        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++)
                small[y * 32 + x] = src[(y * 2) * 64 + (x * 2)];
        emit_texture_quiet(outdir, fname, small, 32, 32, 0);
        total++;
    }
    if (!seen) {
        printf("  [FLAT ] no F_START/F_END markers, skipping flats\n");
        return;
    }
    printf("  [FLAT ] %d flats: %d full-res 64x64 CI4, %d downsampled to "
           "32x32 CI8 into %s/\n", total, ci4, total - ci4, outdir);
}

/* Emit every sprite frame between S_START and S_END.
 *
 * Unlike wall textures, a sprite is a single patch and needs no composition --
 * but it does need its transparent gaps made explicit, since the RDP samples a
 * rectangle. Untouched texels are left as the transparency key and discarded
 * at draw time by alpha compare. */
/* --- sound effects --------------------------------------------------------
 *
 * Doom stores each effect as a DS<name> lump: an 8-byte header giving format,
 * sample rate and length, then unsigned 8-bit PCM. Almost all are 11025 Hz.
 *
 * These come out as plain WAV, not as the final on-cart format. libdragon's
 * audioconv64 turns WAV into wav64, which the mixer streams straight off the
 * cartridge -- so 122 effects cost no RAM at all, which matters when textures
 * already hold half a megabyte. Emitting WAV here and converting in the build
 * keeps this tool free of that format's details.
 */
static void emit_sound(const char *outdir, const char *name,
                       const uint8_t *pcm, int rate, int nsamples) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.wav", outdir, name);
    FILE *f = fopen(path, "wb");
    if (!f) die("cannot write %s", path);

    /* Canonical 44-byte WAV header, mono, 8-bit unsigned -- which is exactly
     * how the lump already stores it, so the samples copy through untouched. */
    const uint32_t datasz = (uint32_t)nsamples;
    const uint32_t riffsz = 36 + datasz;
    const uint16_t chans = 1, bits = 8;
    const uint32_t byterate = (uint32_t)rate * chans * (bits / 8);
    const uint16_t align = chans * (bits / 8);

    fwrite("RIFF", 1, 4, f);  fwrite(&riffsz, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    const uint32_t fmtsz = 16; const uint16_t pcmfmt = 1;
    fwrite(&fmtsz, 4, 1, f);   fwrite(&pcmfmt, 2, 1, f);
    fwrite(&chans, 2, 1, f);   fwrite(&rate, 4, 1, f);
    fwrite(&byterate, 4, 1, f); fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);   fwrite(&datasz, 4, 1, f);
    fwrite(pcm, 1, datasz, f);
    fclose(f);
}

static void emit_all_sounds(wad_t *w, const char *outdir) {
    int total = 0;
    size_t bytes = 0;

    for (int32_t i = 0; i < w->numlumps; i++) {
        if (w->dir[i].name[0] != 'D' || w->dir[i].name[1] != 'S') continue;
        if (w->dir[i].size < 8) continue;

        const uint8_t *l = w->data + w->dir[i].filepos;
        const int fmt   = l[0] | (l[1] << 8);
        const int rate  = l[2] | (l[3] << 8);
        const int count = l[4] | (l[5] << 8) | (l[6] << 16) | (l[7] << 24);
        if (fmt != 3 || rate <= 0) continue;

        /* The declared length can exceed the lump; trust whichever is smaller.
         * Some IWADs pad, and reading past the lump would splice in whatever
         * follows it in the WAD. */
        int n = count;
        if (n > (int)w->dir[i].size - 8) n = (int)w->dir[i].size - 8;
        if (n <= 0) continue;

        char name[16];
        int k = 0;
        for (; k < 8 && w->dir[i].name[k]; k++) name[k] = (char)tolower((unsigned char)w->dir[i].name[k]);
        name[k] = 0;

        emit_sound(outdir, name, l + 8, rate, n);
        total++;
        bytes += (size_t)n;
    }
    printf("  [SFX  ] %d effects (%zu KB of PCM)\n", total, bytes / 1024);
}

/* --- menu, status bar and intermission graphics ---------------------------
 *
 * These are patches like sprites, but they are drawn in screen space at a
 * fixed size, so they need neither a half-resolution level nor the anchor
 * offsets a billboard uses. What they do need is the transparency key: menu
 * art is full of cutouts, and the status bar overlays the view.
 *
 * They live outside the S_START/S_END and F_START/F_END markers, so they are
 * found by name instead. The prefixes cover the menu (M_), the status bar and
 * its font (ST), the intermission (WI), the ammo digits and the screen border.
 */
static int is_ui_lump(const char *name) {
    static const char *const pfx[] = {
        "M_", "ST", "WI", "AMMNUM", "BRDR", "CWILV", "HELP",
        "TITLEPIC", "INTERPIC", "CREDIT", "VICTORY", "PFUB", "END",
    };
    for (size_t k = 0; k < sizeof pfx / sizeof pfx[0]; k++) {
        const size_t n = strlen(pfx[k]);
        if (strncmp(name, pfx[k], n) == 0) return 1;
    }
    return 0;
}

static void emit_all_ui(wad_t *w, const char *outdir) {
    int total = 0;
    size_t bytes = 0;

    for (int32_t i = 0; i < w->numlumps; i++) {
        if (w->dir[i].size < 12) continue;

        char name[9] = {0};
        memcpy(name, w->dir[i].name, 8);
        if (!is_ui_lump(name)) continue;

        const uint8_t *patch = w->data + w->dir[i].filepos;
        const int pw = rds16(patch), ph = rds16(patch + 2);
        if (pw <= 0 || ph <= 0 || pw > 512 || ph > 512) continue;

        uint8_t *canvas = malloc((size_t)pw * ph);
        uint8_t *mask   = calloc((size_t)pw * ph, 1);
        if (!canvas || !mask) die("out of memory for ui lump");
        memset(canvas, DT64_TRANSPARENT_INDEX, (size_t)pw * ph);

        draw_patch(canvas, mask, pw, ph, patch, w->dir[i].size, 0, 0);

        char fname[32];
        snprintf(fname, sizeof fname, "u_%s", name);

        /* Offsets are kept: the status bar and some menu pieces position
         * themselves by them exactly as sprites do. */
        g_leftoffset = rds16(patch + 4);
        g_topoffset  = rds16(patch + 6);
        emit_texture_quiet(outdir, fname, canvas, pw, ph, 1);
        g_leftoffset = g_topoffset = 0;

        total++;
        bytes += (size_t)pw * ph;
        free(canvas);
        free(mask);
    }
    printf("  [UI   ] %d menu/status graphics (%zu KB)\n", total, bytes / 1024);
}

static void emit_all_sprites(wad_t *w, const char *outdir) {
    /* Every marker region -- see emit_all_flats for why one pair is not
     * enough once a mod is stacked on top. */
    int total = 0, inside = 0, seen = 0;
    size_t bytes = 0;
    for (int32_t i = 0; i < w->numlumps; i++) {
        if (lump_name_eq(w->dir[i].name, "S_START") ||
            lump_name_eq(w->dir[i].name, "SS_START")) { inside = 1; seen = 1; continue; }
        if (lump_name_eq(w->dir[i].name, "S_END") ||
            lump_name_eq(w->dir[i].name, "SS_END"))   { inside = 0; continue; }
        if (!inside) continue;
        if (w->dir[i].size < 12) continue;

        const uint8_t *patch = w->data + w->dir[i].filepos;
        int pw = rds16(patch), ph = rds16(patch + 2);
        if (pw <= 0 || ph <= 0 || pw > 512 || ph > 512) continue;

        uint8_t *canvas = malloc((size_t)pw * ph);
        uint8_t *mask   = calloc((size_t)pw * ph, 1);
        if (!canvas || !mask) die("out of memory for sprite");
        memset(canvas, DT64_TRANSPARENT_INDEX, (size_t)pw * ph);

        draw_patch(canvas, mask, pw, ph, patch, w->dir[i].size, 0, 0);

        char name[9] = {0};
        memcpy(name, w->dir[i].name, 8);
        char fname[32];
        snprintf(fname, sizeof fname, "s_%s", name);

        const int lofs = rds16(patch + 4), tofs = rds16(patch + 6);
        g_leftoffset = lofs;
        g_topoffset  = tofs;
        emit_texture_quiet(outdir, fname, canvas, pw, ph, 1);

        /* Half-resolution frame. A sprite larger than 2 KB of texels tiles
         * exactly as a wall does, and a monster across the room is drawn at a
         * fraction of its size while still paying for every tile. Offsets
         * halve with the image so the anchor still lands on its feet. */
        if (pw >= 2 && ph >= 2) {
            const int mw = pw / 2, mh = ph / 2;
            uint8_t *mip = malloc((size_t)mw * mh);
            if (mip) {
                for (int y = 0; y < mh; y++)
                    for (int x = 0; x < mw; x++)
                        mip[y * mw + x] = canvas[(y * 2) * pw + (x * 2)];
                char mn[36];
                snprintf(mn, sizeof mn, "n_%s", name);
                g_leftoffset = lofs / 2;
                g_topoffset  = tofs / 2;
                emit_texture_quiet(outdir, mn, mip, mw, mh, 1);
                free(mip);
            }
        }
        g_leftoffset = g_topoffset = 0;

        bytes += (size_t)pw * ph;
        free(canvas);
        free(mask);
        total++;
    }
    if (!seen) {
        printf("  [SPR  ] no S_START/S_END markers, skipping sprites\n");
        return;
    }
    printf("  [SPR  ] %d sprite frames, %.1f MB into %s/\n",
           total, bytes / 1e6, outdir);
}

/* -------------------------------------------------------------------- main */

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr,
            "usage: wad2n64 <doom.wad> <outdir> <TEXTURE> [TEXTURE...]\n"
            "       wad2n64 <doom.wad> <outdir> --all [--pwad mod.wad]\n"
            "\n"
            "Composes Doom wall textures into flat CI8 data for the RDP and\n"
            "emits the shared PLAYPAL TLUT.\n"
            "\n"
            "--pwad stacks a mod on top, so its textures, flats, sprites and\n"
            "sounds are baked in place of the ones it replaces.\n");
        return 1;
    }

    const char *wadpath = argv[1];
    const char *outdir  = argv[2];

    wad_t wad;
    wad_open(&wad, wadpath);
    printf("wad2n64: %s (%d lumps)\n", wadpath, wad.numlumps);

    /* Before any emitter runs: they all read through the merged directory. */
    for (int a = 3; a + 1 < argc; a++)
        if (strcmp(argv[a], "--pwad") == 0) wad_append(&wad, argv[a + 1]);

    emit_tlut(&wad, outdir);

    for (int a = 3; a < argc; a++)
        if (strcmp(argv[a], "--no-ci4") == 0) g_no_ci4 = 1;

    /* RUNTIMEART builds compose their own art as levels load, so baking it
     * would put nine megabytes in the cartridge for nothing. Sounds are not
     * art: they are still wav64 and still baked. */
    int no_art = 0;
    for (int a = 3; a < argc; a++)
        if (strcmp(argv[a], "--no-art") == 0) no_art = 1;

    if (argc >= 4 && strcmp(argv[3], "--all") == 0) {
        if (!no_art) {
            order_begin(outdir, "flats");
            emit_all_flats(&wad, outdir);
            order_end();
            order_begin(outdir, "textures");
            emit_all_textures(&wad, outdir);
            order_end();
            if (order_file) fclose(order_file);
            emit_all_sprites(&wad, outdir);
        }
        emit_all_sounds(&wad, outdir);
        if (!no_art) emit_all_ui(&wad, outdir);
        return 0;
    }

    for (int i = 3; i < argc; i++) {
        /* Options and their operands are not texture names. Without this a
         * stray --pwad on the single-texture path would be looked up as a
         * texture and die on a name no WAD contains. */
        if (strncmp(argv[i], "--", 2) == 0) {
            if (strcmp(argv[i], "--pwad") == 0) i++;
            continue;
        }
        int tw, th, masked;
        uint8_t *texels = compose_texture(&wad, argv[i], &tw, &th, &masked);
        emit_texture(outdir, argv[i], texels, tw, th, masked);
        free(texels);
    }
    return 0;
}
