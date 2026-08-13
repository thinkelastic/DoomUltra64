/*
 * Host-side render harness.
 *
 * Compiles the real src/r_wall.c against a mock RDP and software-rasterises
 * what it emits. On-hardware iteration is slow, so this catches the whole
 * class of bugs that matter most -- wrong projection, bad near-clipping,
 * mis-addressed TMEM tiles, UVs outside the resident rectangle -- in the time
 * it takes to run a unit test.
 *
 * The rasteriser mirrors the RDP pipeline closely enough to be meaningful:
 * perspective-correct S/T, texel lookup through the shared TLUT, and
 * modulation by shade RGB.
 *
 * It also enforces the invariant the real hardware cannot check for us: every
 * sampled texel must lie inside the sub-rectangle most recently uploaded to
 * TMEM. A violation renders as garbage on console but is silent in a mock, so
 * it is checked explicitly.
 */

#include <math.h>
#include <string.h>

#include "../src/r_wall.h"
#include "../src/r_flat.h"
#include "../src/r_sprite.h"
#include "../src/scene.h"

#define FB_W SCREEN_W
#define FB_H SCREEN_H

static uint8_t  framebuffer[FB_H][FB_W][3];
static uint16_t tlut[256];

/* The texture rectangle currently "resident in TMEM". Wrapping is tracked
 * per axis, from the upload's texparms, exactly as the RDP's tile descriptor
 * would: a full-width power-of-two band uploaded with s.repeats folds S and
 * clamps T. */
static struct {
    const surface_t *tex;
    int s0, t0, s1, t1;
    bool wrap_s, wrap_t;
} tmem;

static int stat_uploads;
static int stat_upload_bytes;    /* texel bytes moved into TMEM */
static int stat_upload_block;    /* uploads eligible for LOAD_BLOCK */
static int stat_rects;           /* fixed-function texture rectangles */
static int stat_tris;
static int stat_pixels;
static int stat_oob;      /* samples outside the resident tile */
static int stat_overflow; /* vertices beyond the RDP coordinate range */
static int stat_deep;     /* quads spanning too wide a depth range */

/* Whether an upload can take the RDP's fast LOAD_BLOCK path: one contiguous
 * DMA burst instead of a strided read per row. Mirrors libdragon's check
 * (rdpq_tex.c): the rect's byte width must equal the surface stride, and the
 * stride must be a multiple of 8. For CI8, texels are bytes. */
static bool load_block_eligible(const surface_t *tex, int s0, int s1)
{
    return (s1 - s0) == (int)tex->stride && (tex->stride % 8) == 0;
}

/* ------------------------------------------------------------ mock rdpq */

int rdpq_tex_upload_sub(rdpq_tile_t tile, const surface_t *tex,
                        const rdpq_texparms_t *parms,
                        int s0, int t0, int s1, int t1)
{
    (void)tile;

    /* The real constraint: 4 KB TMEM, halved to 2 KB once a TLUT is resident.
     * At 1 byte per CI8 texel that is 2048 texels. Exceeding it silently
     * corrupts the palette on hardware, so fail loudly here instead. */
    const int bytes = (s1 - s0) * (t1 - t0);
    assertf(bytes <= 2048,
            "TMEM overflow: %dx%d CI8 = %d bytes, limit is 2048 with a TLUT",
            s1 - s0, t1 - t0, bytes);

    tmem.tex = tex;
    tmem.s0 = s0; tmem.t0 = t0; tmem.s1 = s1; tmem.t1 = t1;
    tmem.wrap_s = parms && parms->s.repeats > 1.5f;
    tmem.wrap_t = parms && parms->t.repeats > 1.5f;
    if (tmem.wrap_s)
        assertf((s0 == 0) && ((s1 & (s1 - 1)) == 0),
                "wrapping S upload must be a full power-of-two band, got [%d,%d)",
                s0, s1);
    if (tmem.wrap_t)
        assertf((t0 == 0) && ((t1 & (t1 - 1)) == 0),
                "wrapping T upload must be a full power-of-two band, got [%d,%d)",
                t0, t1);
    stat_uploads++;
    stat_upload_bytes += bytes;
    if (load_block_eligible(tex, s0, s1)) stat_upload_block++;
    return bytes;
}

/* Whole-texture upload. Wrapping follows the texparms per axis, as the sub-
 * rect path does. */
int rdpq_tex_upload(rdpq_tile_t tile, const surface_t *tex,
                    const rdpq_texparms_t *parms)
{
    (void)tile;
    tmem.tex = tex;
    tmem.s0 = 0; tmem.t0 = 0;
    tmem.s1 = tex->width; tmem.t1 = tex->height;
    tmem.wrap_s = parms && parms->s.repeats > 1.5f;
    tmem.wrap_t = parms && parms->t.repeats > 1.5f;
    stat_uploads++;
    stat_upload_bytes += tex->width * tex->height;
    if (load_block_eligible(tex, 0, tex->width)) stat_upload_block++;
    return tex->width * tex->height;
}

/* Fixed-function screen rectangle (COPY-mode UI blits, weapon tiles). The
 * mock only counts it: the paths under geometric test draw triangles. */
/* The weapon layer's mode bracket lives in v_draw.c, which the harness
 * does not compile: r_psprite_draw hands the bracket back after drawing
 * the muzzle flash blended, and the harness models neither UI modes nor
 * psprites. A no-op keeps the link honest without pretending to. */
void V_BeginUI(void) {}

void rdpq_texture_rectangle(rdpq_tile_t tile, float x0, float y0,
                            float x1, float y1, float s, float t)
{
    (void)tile; (void)x0; (void)y0; (void)x1; (void)y1; (void)s; (void)t;
    stat_rects++;
}

/* Counted, not rasterised, for the same reason as the 1:1 form: the only
 * callers here are the UI and weapon passes, and the harness has no game
 * layer to hand them anything to draw (D_PSpriteGet below returns none). */
void rdpq_texture_rectangle_scaled(rdpq_tile_t tile, float x0, float y0,
                                   float x1, float y1,
                                   float s0, float t0, float s1, float t1)
{
    (void)tile; (void)x0; (void)y0; (void)x1; (void)y1;
    (void)s0; (void)t0; (void)s1; (void)t1;
    stat_rects++;
}

/* r_sprite.c's weapon pass asks the game layer for psprites; the harness has
 * no game layer and draws none. */
int D_PSpriteGet(int i, void **tex, int *x, int *y)
{
    (void)i; (void)tex; (void)x; (void)y;
    return 0;
}

/* Sample the resident tile, converting RGBA5551 back to RGB888. */
static void sample_tmem(float s, float t, uint8_t out[3])
{
    int si = (int)floorf(s), ti = (int)floorf(t);

    bool oob = false;
    if (tmem.wrap_s) {
        const int w = tmem.s1;
        si %= w; if (si < 0) si += w;
    } else if (si < tmem.s0 || si >= tmem.s1) {
        oob = true;
    }
    if (tmem.wrap_t) {
        const int h = tmem.t1;
        ti %= h; if (ti < 0) ti += h;
    } else if (ti < tmem.t0 || ti >= tmem.t1) {
        oob = true;
    }
    if (oob) stat_oob++;

    /* Clamp into the resident rectangle, matching RDP tile clamping. */
    if (si < tmem.s0) si = tmem.s0;
    if (si >= tmem.s1) si = tmem.s1 - 1;
    if (ti < tmem.t0) ti = tmem.t0;
    if (ti >= tmem.t1) ti = tmem.t1 - 1;

    const uint8_t *texels = tmem.tex->buffer;
    const uint8_t idx = texels[(size_t)ti * tmem.tex->stride + si];

    /* TLUT entries are stored big-endian for the console. */
    const uint16_t raw = tlut[idx];
    const uint16_t c = (uint16_t)((raw >> 8) | (raw << 8));

    out[0] = (uint8_t)((((c >> 11) & 31) * 255) / 31);
    out[1] = (uint8_t)((((c >> 6)  & 31) * 255) / 31);
    out[2] = (uint8_t)((((c >> 1)  & 31) * 255) / 31);
}

/* Vertex layout is TRIFMT_SHADE_TEX: {X,Y, R,G,B,A, S,T,INV_W}. */
#define VX(v) ((v)[0])
#define VY(v) ((v)[1])

static inline float edge(const float *a, const float *b, float px, float py)
{
    return (px - VX(a)) * (VY(b) - VY(a)) - (py - VY(a)) * (VX(b) - VX(a));
}

/* RDP triangle coordinates are s11.2 fixed point. Anything beyond this wraps
 * during triangle setup and rasterises garbage across the frame, so geometry
 * must be frustum-clipped before submission -- clipping only the near plane
 * still lets a wall passing beside the camera project to x in the thousands.
 * A software rasteriser happily draws such a triangle, so the limit has to be
 * asserted rather than observed. */
#define RDP_COORD_LIMIT 2047.0f

void rdpq_triangle(const rdpq_trifmt_t *fmt,
                   const float *v1, const float *v2, const float *v3)
{
    (void)fmt;
    stat_tris++;

    /* The RDP's fixed-point perspective divide warps textures when one
     * primitive spans a wide depth range. A float rasteriser cannot reproduce
     * that -- it only appears on real hardware -- so the *invariant* is
     * checked here instead: keep the depth ratio per triangle bounded and the
     * artefact cannot occur. Mirrors R_MAX_DEPTH_RATIO in r_wall.c. */
    {
        const float iw1 = v1[8], iw2 = v2[8], iw3 = v3[8];
        const float lo = fminf(iw1, fminf(iw2, iw3));
        const float hi = fmaxf(iw1, fmaxf(iw2, iw3));
        if (lo > 0.0f && hi / lo > 2.05f) stat_deep++;   /* 2.0 + epsilon */
    }

    const float *vs[3] = { v1, v2, v3 };
    for (int i = 0; i < 3; i++) {
        if (fabsf(VX(vs[i])) > RDP_COORD_LIMIT ||
            fabsf(VY(vs[i])) > RDP_COORD_LIMIT) {
            if (stat_overflow++ == 0)
                printf("      first overflow: vertex (%.1f, %.1f)\n",
                       VX(vs[i]), VY(vs[i]));
        }
    }

    float minx = fminf(VX(v1), fminf(VX(v2), VX(v3)));
    float maxx = fmaxf(VX(v1), fmaxf(VX(v2), VX(v3)));
    float miny = fminf(VY(v1), fminf(VY(v2), VY(v3)));
    float maxy = fmaxf(VY(v1), fmaxf(VY(v2), VY(v3)));

    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx);
    int y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > FB_W) x1 = FB_W;
    if (y1 > FB_H) y1 = FB_H;

    const float area = edge(v1, v2, VX(v3), VY(v3));
    if (fabsf(area) < 1e-6f) return;
    const float inv_area = 1.0f / area;

    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            const float px = x + 0.5f, py = y + 0.5f;

            /* Barycentric coverage; accept either winding. */
            float w1 = edge(v2, v3, px, py) * inv_area;
            float w2 = edge(v3, v1, px, py) * inv_area;
            float w3 = edge(v1, v2, px, py) * inv_area;
            if (w1 < 0.0f || w2 < 0.0f || w3 < 0.0f) continue;

            /* Perspective-correct S/T: interpolate S*invW and invW linearly in
             * screen space, then divide -- exactly what the RDP does with the
             * INV_W the renderer supplies. */
            const float iw = w1 * v1[8] + w2 * v2[8] + w3 * v3[8];
            if (iw <= 0.0f) continue;

            const float s = (w1 * v1[6] * v1[8] + w2 * v2[6] * v2[8] +
                             w3 * v3[6] * v3[8]) / iw;
            const float t = (w1 * v1[7] * v1[8] + w2 * v2[7] * v2[8] +
                             w3 * v3[7] * v3[8]) / iw;

            uint8_t texel[3];
            sample_tmem(s, t, texel);

            /* Combiner: RGB = TEX0 * SHADE, with no blender pass. Shade
             * already carries sector light times distance falloff, so this is
             * the whole shading pipeline -- see r_setup_walls for why the RDP
             * fog unit is deliberately not used. */
            const float lr = w1 * v1[2] + w2 * v2[2] + w3 * v3[2];
            const float lg = w1 * v1[3] + w2 * v2[3] + w3 * v3[3];
            const float lb = w1 * v1[4] + w2 * v2[4] + w3 * v3[4];

            framebuffer[y][x][0] = (uint8_t)(texel[0] * lr);
            framebuffer[y][x][1] = (uint8_t)(texel[1] * lg);
            framebuffer[y][x][2] = (uint8_t)(texel[2] * lb);
            stat_pixels++;
        }
    }
}

/* --------------------------------------------------------- asset loading */

static void load_tlut(const char *path)
{
    FILE *f = fopen(path, "rb");
    assertf(f, "cannot open %s", path);
    assertf(fread(tlut, 1, sizeof tlut, f) == sizeof tlut, "short TLUT read");
    fclose(f);
}

static void load_tex(dt64_tex_t *tex, const char *path)
{
    /* Zeroed first: dt64_tex_t gained a mip pointer, and a stale one here
     * would be followed straight into the level-of-detail path. */
    memset(tex, 0, sizeof *tex);

    FILE *f = fopen(path, "rb");
    assertf(f, "cannot open %s", path);

    uint8_t hdr[DT64_HEADER_SIZE];
    assertf(fread(hdr, 1, sizeof hdr, f) == sizeof hdr, "short header in %s", path);
    assertf(memcmp(hdr, "DT64", 4) == 0, "bad magic in %s", path);
    assertf(((hdr[4] << 8) | hdr[5]) == 3, "unsupported .dt64 version in %s", path);

    tex->width  = (uint16_t)((hdr[6]  << 8) | hdr[7]);
    tex->height = (uint16_t)((hdr[8]  << 8) | hdr[9]);
    tex->flags      = (uint16_t)((hdr[10] << 8) | hdr[11]);
    tex->leftoffset = (int16_t)((hdr[12] << 8) | hdr[13]);
    tex->topoffset  = (int16_t)((hdr[14] << 8) | hdr[15]);

    const size_t n = (size_t)tex->width * tex->height;
    tex->texels = malloc(n);
    assertf(tex->texels, "out of memory");
    assertf(fread(tex->texels, 1, n, f) == n, "short texel read in %s", path);
    fclose(f);

    /* Tile-major files exist for the RDP's LOAD_BLOCK path; the mock samples
     * through a linear surface, so convert back. The real console keeps the
     * tiled layout and uploads per-tile via dt64_upload_tile. */
    if (tex->flags & DT64_FLAG_TILEMAJOR) {
        uint8_t *lin = malloc(n);
        assertf(lin, "out of memory");
        const uint8_t *src = tex->texels;
        for (int ty = 0; ty < tex->height; ty += 32) {
            const int th = tex->height - ty < 32 ? tex->height - ty : 32;
            for (int tx = 0; tx < tex->width; tx += 64) {
                const int tw = tex->width - tx < 64 ? tex->width - tx : 64;
                for (int row = 0; row < th; row++) {
                    memcpy(lin + (size_t)(ty + row) * tex->width + tx,
                           src, (size_t)tw);
                    src += tw;
                }
            }
        }
        free(tex->texels);
        tex->texels = lin;
        tex->flags &= (uint16_t)~DT64_FLAG_TILEMAJOR;
    }

    tex->surface = surface_make_linear(tex->texels, FMT_CI8, tex->width, tex->height);
    tex->tiles_x = (uint8_t)((tex->width  + DT64_TILE_W - 1) / DT64_TILE_W);
    tex->tiles_y = (uint8_t)((tex->height + DT64_TILE_H - 1) / DT64_TILE_H);
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    assertf(f, "cannot write %s", path);
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    fwrite(framebuffer, 1, sizeof framebuffer, f);
    fclose(f);
}

/* ------------------------------------------------------------------ main */

/* Frame stamp normally owned by r_ssdata.c, which the host build omits;
 * the transform cache in r_flat.c keys on it. */
uint32_t r_flat_stamp = 1;

int main(int argc, char **argv)
{
    const char *outdir = argc > 1 ? argv[1] : ".";
    const char *fsdir  = argc > 2 ? argv[2] : "filesystem";

    char path[512];
    snprintf(path, sizeof path, "%s/playpal.tlut", fsdir);
    load_tlut(path);

    dt64_tex_t startan, brown, compute;
    snprintf(path, sizeof path, "%s/STARTAN3.dt64", fsdir); load_tex(&startan, path);
    snprintf(path, sizeof path, "%s/BROWN1.dt64",   fsdir); load_tex(&brown,   path);
    snprintf(path, sizeof path, "%s/COMPUTE1.dt64", fsdir); load_tex(&compute, path);

    r_wall_t walls[SCENE_NUM_WALLS];
    scene_build(walls, &startan, &compute, &brown);

    /* Viewpoints chosen to exercise distinct paths: centred (all four walls
     * visible), a diagonal corner view (steep depth gradients), and one hard
     * against a wall (exercises the near-plane clip). */
    const struct { const char *name; r_camera_t cam; } views[] = {
        /* Focals derived exactly as main.c derives them, so `make test
         * WIDE=1` exercises the real wide-mode projection -- in particular
         * the s11.2 vertex-range check, which is what doubling screen X
         * could break. */
#define CAM_FX (SCREEN_W * 0.5f)
#define CAM_FY (SCREEN_BASE_W * 0.5f)
        { "front",  { 0.0f,    0.0f,   EYE_Z, 0.0f,       CAM_FX, CAM_FY } },
        { "corner", { -190.0f, -190.0f, EYE_Z, 0.7853982f, CAM_FX, CAM_FY } },
        { "close",  { 0.0f,   -220.0f, EYE_Z, 1.5707963f, CAM_FX, CAM_FY } },
    };

    int failures = 0;
    for (size_t i = 0; i < sizeof views / sizeof views[0]; i++) {
        memset(framebuffer, 0, sizeof framebuffer);
        stat_uploads = stat_tris = stat_pixels = stat_oob = stat_overflow = stat_deep = 0;

        r_setup_walls();
        r_set_view(&views[i].cam);
        for (int w = 0; w < SCENE_NUM_WALLS; w++)
            r_draw_wall(&views[i].cam, &walls[w]);
        r_flush_walls();

        snprintf(path, sizeof path, "%s/%s.ppm", outdir, views[i].name);
        write_ppm(path);

        const float coverage = 100.0f * stat_pixels / (FB_W * FB_H);
        printf("  %-7s uploads=%-4d tris=%-4d coverage=%5.1f%% oob=%d overflow=%d "
               "dropped=%d deep=%d%s\n",
               views[i].name, stat_uploads, stat_tris, coverage, stat_oob,
               stat_overflow, r_quads_dropped(), stat_deep,
               (stat_oob || stat_overflow || r_quads_dropped() || stat_deep)
                   ? "  <-- FAIL" : "");

        if (stat_oob) failures++;
        if (stat_overflow) failures++;
        if (r_quads_dropped()) failures++;
        if (stat_deep) failures++;
        if (stat_pixels == 0) {
            printf("      <-- FAIL: nothing rasterised\n");
            failures++;
        }
        /* r_tmem_uploads must agree with what the mock actually saw. */
        if (r_tmem_uploads() != stat_uploads) {
            printf("      <-- FAIL: upload counter %d != actual %d\n",
                   r_tmem_uploads(), stat_uploads);
            failures++;
        }
    }

    /* --- batching scale check ------------------------------------------
     *
     * The claim batching rests on is that TMEM uploads track the number of
     * distinct tiles on screen, not the amount of geometry. Verify it by
     * subdividing the same room into progressively more segments: the walls
     * stay collinear so nothing overlaps and the picture is unchanged in
     * extent, but the renderer sees many times more draw calls.
     *
     * Triangles should climb and uploads should not. If a future change
     * reintroduces per-quad uploads, this is where it shows up. */
    printf("\nbatching scale (same room, subdivided):\n");

    int upload_at_1 = 0, bytes_at_1 = 0;
    for (int split = 1; split <= 8; split *= 2) {
        r_wall_t seg[SCENE_NUM_WALLS * 8];
        int n = 0;
        for (int w = 0; w < SCENE_NUM_WALLS; w++) {
            for (int k = 0; k < split; k++) {
                const float f0 = (float)k / split, f1 = (float)(k + 1) / split;
                seg[n] = walls[w];
                seg[n].x1 = walls[w].x1 + (walls[w].x2 - walls[w].x1) * f0;
                seg[n].y1 = walls[w].y1 + (walls[w].y2 - walls[w].y1) * f0;
                seg[n].x2 = walls[w].x1 + (walls[w].x2 - walls[w].x1) * f1;
                seg[n].y2 = walls[w].y1 + (walls[w].y2 - walls[w].y1) * f1;
                /* len is precomputed and must follow the geometry: copying a
                 * wall and shortening it leaves a stale length, which shows up
                 * as wildly wrong texture repeats. */
                seg[n].len = sqrtf((seg[n].x2 - seg[n].x1) * (seg[n].x2 - seg[n].x1) +
                                   (seg[n].y2 - seg[n].y1) * (seg[n].y2 - seg[n].y1));
                n++;
            }
        }

        stat_uploads = stat_upload_bytes = stat_upload_block = 0;
        stat_tris = stat_oob = stat_overflow = stat_deep = 0;
        r_setup_walls();
        r_set_view(&views[0].cam);
        for (int w = 0; w < n; w++) r_draw_wall(&views[0].cam, &seg[w]);
        r_flush_walls();

        printf("  %3d walls -> uploads=%-4d tris=%-4d bytes=%-6d block=%d/%d\n",
               n, stat_uploads, stat_tris, stat_upload_bytes,
               stat_upload_block, stat_uploads);

        if (split == 1) { upload_at_1 = stat_uploads; bytes_at_1 = stat_upload_bytes; }

        /* Uploads may rise a little, since subdividing lets more of each wall
         * survive clipping, but must not scale with geometry. Bytes are the
         * honest version of the same property: a change that keeps the count
         * flat while re-uploading bigger rectangles would sail through a
         * count-only check. */
        if (stat_uploads > upload_at_1 * 3) {
            printf("      <-- FAIL: uploads scaling with geometry\n");
            failures++;
        }
        if (stat_upload_bytes > bytes_at_1 * 3) {
            printf("      <-- FAIL: upload bytes scaling with geometry\n");
            failures++;
        }
        if (r_quads_dropped()) {
            printf("      <-- FAIL: batch overflowed (%d dropped)\n", r_quads_dropped());
            failures++;
        }
    }

    /* --- flat batching --------------------------------------------------
     *
     * Four abutting floor cells sharing one flat must cost ONE upload: the
     * whole point of queueing flats instead of drawing them as the BSP walk
     * finds them. This pins the flat path's upload behaviour the way the
     * subdivision test pins the walls'. */
    {
        static uint8_t flatpx[32 * 32];
        for (unsigned i = 0; i < sizeof flatpx; i++) flatpx[i] = (uint8_t)i;
        dt64_tex_t flat;
        memset(&flat, 0, sizeof flat);
        flat.width = 32; flat.height = 32;
        flat.texels  = flatpx;
        flat.surface = surface_make_linear(flatpx, FMT_CI8, 32, 32);

        static const r_polypt_t cells[4][4] = {
            { {  32, -64 }, {  96, -64 }, {  96,   0 }, {  32,   0 } },
            { {  32,   0 }, {  96,   0 }, {  96,  64 }, {  32,  64 } },
            { {  96, -64 }, { 160, -64 }, { 160,   0 }, {  96,   0 } },
            { {  96,   0 }, { 160,   0 }, { 160,  64 }, {  96,  64 } },
        };

        stat_uploads = stat_tris = stat_oob = stat_overflow = stat_deep = 0;
        r_set_view(&views[0].cam);
        r_flat_begin();
        for (int i = 0; i < 4; i++)
            { static const float white[3] = {1.0f,1.0f,1.0f};
              r_flat_add(cells[i], 4, 0.0f, 255, &flat, 0.0f, white, 0, 0); }
        r_flat_flush(&views[0].cam);

        printf("\nflats: 4 cells, 1 texture -> uploads=%d tris=%d oob=%d\n",
               stat_uploads, stat_tris, stat_oob);
        if (stat_uploads != 1) {
            printf("      <-- FAIL: expected exactly 1 flat upload\n");
            failures++;
        }
        if (stat_tris == 0 || stat_oob) {
            printf("      <-- FAIL: flat rasterisation wrong\n");
            failures++;
        }
    }

    /* --- sprite batching ------------------------------------------------
     *
     * Three instances of one two-tile sprite must cost two uploads (one per
     * distinct tile), and an instance projecting entirely below the screen
     * must cost none -- the cull runs before the upload, not after. */
    {
        static uint8_t sprpx[64 * 64];
        for (unsigned i = 0; i < sizeof sprpx; i++) sprpx[i] = (uint8_t)(i * 7);
        dt64_tex_t spr;
        memset(&spr, 0, sizeof spr);
        spr.width = 64; spr.height = 64;
        spr.leftoffset = 32;         /* anchored centre-bottom, like Doom art */
        spr.topoffset  = 64;
        spr.texels  = sprpx;
        spr.surface = surface_make_linear(sprpx, FMT_CI8, 64, 64);

        stat_uploads = stat_tris = stat_oob = stat_overflow = stat_deep = 0;
        r_set_view(&views[0].cam);
        r_sprite_begin();
        for (int i = 0; i < 3; i++) {
            r_thing_t t = { .x = 100.0f + 40.0f * i, .y = -20.0f + 20.0f * i,
                            .z = 0.0f, .spr = &spr };
            { const float sh[3] = { 1.0f, 1.0f, 1.0f };
              r_sprite_add(&views[0].cam, &t, sh, -1, 1.0f); }
        }
        r_sprite_flush();

        printf("sprites: 3 instances, 2 tiles -> uploads=%d tris=%d oob=%d\n",
               stat_uploads, stat_tris, stat_oob);
        if (stat_uploads != 2) {
            printf("      <-- FAIL: expected exactly 2 sprite uploads\n");
            failures++;
        }
        if (stat_tris != 12 || stat_oob) {
            printf("      <-- FAIL: expected 12 sprite triangles\n");
            failures++;
        }

        /* Fully below the screen: culled at add time, zero uploads. */
        stat_uploads = stat_tris = 0;
        r_sprite_begin();
        {
            r_thing_t t = { .x = 100.0f, .y = 0.0f, .z = -2000.0f, .spr = &spr };
            { const float sh[3] = { 1.0f, 1.0f, 1.0f };
              r_sprite_add(&views[0].cam, &t, sh, -1, 1.0f); }
        }
        r_sprite_flush();
        printf("sprites: off-screen instance     -> uploads=%d tris=%d\n",
               stat_uploads, stat_tris);
        if (stat_uploads != 0 || stat_tris != 0) {
            printf("      <-- FAIL: off-screen sprite still cost work\n");
            failures++;
        }
    }

    printf("\n%s\n", failures ? "FAILED" : "all views OK");
    return failures ? 1 : 0;
}
