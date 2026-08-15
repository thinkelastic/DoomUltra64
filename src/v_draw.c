/*
 * v_draw -- Doom's patch drawing, on the RDP.
 *
 * Doom's menus, status bar and intermission screens all draw through
 * V_DrawPatch: a patch, a screen position, done. Its own implementation walks
 * the patch's column format and writes bytes into a framebuffer, which is
 * exactly what this port does not do.
 *
 * The operation is one the renderer already performs, though -- the player's
 * weapon is an alpha-tested screen-space quad, tiled through TMEM, drawn with
 * depth off. Menus are the same thing at a different position, so this is that
 * code generalised rather than anything new.
 *
 * The only awkward part is identifying the art. Doom caches a lump and passes
 * the resulting patch_t* onward, so the pointer is the only handle available;
 * it is turned back into a lump name and used to find the baked texture. The
 * patch header itself is still read for width, height and offsets, so the
 * ported code that inspects those keeps working.
 */
#include "doom/doomtype.h"
#include "doom/r_defs.h"
#include "doom/w_wad.h"
#include "doom/i_swap.h"

#include "dt64.h"
#include "r_wall.h"

#include <libdragon.h>
#include <stdio.h>
#include <string.h>

void       *p_level_resolve_ptr(const char *name, const char *prefix);
lumpindex_t W_LumpForPointer(const void *ptr);
const char *W_LumpName(lumpindex_t lump);

/* Whether a V_BeginUI/V_EndUI bracket is already open. The frame loop opens
 * one bracket around the weapon, status bar, messages and menu; V_DrawPatch
 * only pays for mode setup itself when called outside that bracket. Setting
 * the full block per patch was ~9 mode commands and a SYNC_PIPE hazard times
 * ~25 patches a frame, all programming identical state. */
static bool ui_active;

/* Vertical placement of Doom's 320x200 UI on this 320x240 screen.
 *
 * v_yshift is added to every patch draw; d_ui sets it per subsystem (the
 * status bar rides 40 rows down to sit flush with the bottom edge, like the
 * weapon already does; messages stay at the top).
 *
 * The buffer flag reproduces what Doom's backing-screen dance actually
 * achieves. ST_refreshBackground draws the bar into st_backing_screen at
 * y=0 and then copies the strip to ST_Y; with V_UseBuffer/V_CopyRect
 * stubbed out, the bar landed at the top-left of the real screen. While the
 * "buffer" is active, draws land at the bar's true destination instead:
 * ST_Y (168) plus the same 40-row shift, i.e. rows 208..240. */
static int  v_yshift;
static bool v_bufmode;

#define V_STBAR_DEST 208

void V_SetYShift(int shift) { v_yshift = shift; }

void V_UseBuffer(byte *buffer)
{
    (void)buffer;                 /* there is no buffer; only a destination */
    v_bufmode = true;
}

void V_RestoreBuffer(void)
{
    v_bufmode = false;
}

/* UI blits are unscaled, axis-aligned, depth-free CI8 screen rectangles with
 * 1-bit palette transparency -- exactly what the RDP's COPY mode exists for.
 * COPY moves 4 texels per clock instead of 1, the combiner is bypassed, and a
 * texture rectangle has no CPU-side edge or gradient setup at all. The
 * alpha-0 discard for the transparent index comes from set_mode_copy(true);
 * libdragon's rsp fixup multiplies DSDX by 4 and makes the coordinates
 * inclusive, so the same call works unchanged in this mode. */
void V_BeginUI(void)
{
    ui_active = true;
/* A RUNTIME test now, not a #if. The question is the same -- is the UI
 * drawn 1:1, in which case COPY mode's four texels a clock apply -- but
 * the answer changes when the detail menu does, so it cannot be decided
 * at compile time. COPY mode cannot magnify (see below), so any scaled
 * mode has to take the standard path. */
    if (SCREEN_W == SCREEN_BASE_W && SCREEN_H == SCREEN_BASE_H) {
    rdpq_set_mode_copy(true);
    } else {
    /* COPY mode cannot magnify.
     *
     * It moves four texels per clock, and DSDX steps S once per four-pixel
     * GROUP -- which is why libdragon's RSP fixup multiplies it by 4. At 1:1
     * that is exactly four consecutive texels per four pixels and the blit is
     * perfect. Ask for a 2x destination and the group boundaries quantise the
     * sampling instead of doubling each texel: every one-pixel feature ghosts
     * and gaps, which on the status bar turns "BULL 40/200" into a row of
     * split glyphs. Verified in ares (paraLLEl-RDP).
     *
     * Standard mode scales correctly, at one pixel per clock instead of four.
     * The UI is a few tens of thousands of pixels a frame, so that trade buys
     * a correct full-width HUD for a cost that stays in the noise -- and it
     * is paid only in wide mode. */
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER1((0, 0, 0, TEX0), (0, 0, 0, TEX0)));
    rdpq_mode_alphacompare(1);        /* same transparent-index cutout */
    rdpq_mode_filter(FILTER_POINT);   /* palette art: never interpolate */
    rdpq_mode_zbuf(false, false);     /* overlay; modes persist, so be explicit */
    }
    rdpq_mode_tlut(TLUT_RGBA16);
}

void V_EndUI(void)
{
    ui_active = false;
    /* Back to a known standard state so whatever draws next -- usually the
     * next frame's clears -- never inherits COPY mode by surprise. */
    rdpq_set_mode_standard();
}

bool V_UIActive(void) { return ui_active; }

/* Draw a baked texture at a screen position, tiling through TMEM. */
/* Horizontal safe-area inset for overlays anchored to the left edge.
 * Doom draws messages at x=0, which is fine on a monitor and not on a
 * television: the outermost columns of the framebuffer fall outside the
 * tube's visible area, so the first character loses a few pixels. Only the
 * layers that hug the edge use this; the status bar is designed to run
 * edge to edge and insetting it would just open a gap. */
static int v_xshift;

void V_SetXShift(int shift) { v_xshift = shift; }

/* --- status-bar compositing ---------------------------------------------
 *
 * The bar redrew ~40 patch upload+rectangle pairs every frame though its
 * content changes on well under a third of frames even in combat (the idle
 * face rerolls at ~2 Hz). Instead: the vendored ST code still "draws"
 * every frame, but into a capture list of (texture, x, y) commands; when
 * the list -- or the palette bank, which an RGBA16 composite bakes in --
 * differs from the committed one, the list is replayed once into a
 * persistent offscreen surface, and every frame re-blits that surface as a
 * handful of full-TMEM tiles. Correct by construction: any visual change
 * in the bar necessarily changes the command list or the bank. */
typedef struct { const dt64_tex_t *tex; int16_t x, y; } v_barcmd_t;
_Static_assert(sizeof(v_barcmd_t) == 8, "capture entry packs to 8 bytes");
#define V_BARCMD_MAX 64          /* worst real case ~47 (netgame+deathmatch) */
#define V_BAR_H      32          /* == ST_HEIGHT, in the 320x240 frame */

/* The same two quantities in REAL PIXELS.
 *
 * The offscreen strip is a piece of framebuffer, so it is sized and placed
 * in pixels -- while everything composited INTO it arrives in the 320x240
 * frame and is magnified by v_blit_at on the way. Keeping the two spaces
 * apart matters the moment they differ: at 480 rows the strip stayed 32 px
 * tall while the art inside doubled to 64 and was clipped to its top half,
 * and the blit put it at row 208 of a 480-line screen -- a half-height
 * status bar across the middle of the picture. */
#define V_BAR_H_PX      ((int)(V_BAR_H * UI_YSCALE))
#define V_STBAR_DEST_PX ((int)(V_STBAR_DEST * UI_YSCALE))
static v_barcmd_t bar_cur[V_BARCMD_MAX], bar_prev[V_BARCMD_MAX];
static int  bar_cur_n, bar_prev_n = -1;   /* -1 forces the first composite */
static int  bar_prev_bank = -1;
static bool bar_capture, bar_surf_valid, bar_legacy;
static surface_t bar_surf;

static void v_blit_at(const dt64_tex_t *tex, float ox, float oy);

static void v_blit(const dt64_tex_t *tex, float ox, float oy)
{
    /* ox/oy stay in Doom's 320-wide frame right through the tile loop; only
     * the destination rectangle is scaled, at the point it is emitted. The
     * safe-area inset is a 320-frame quantity too, so it is applied here,
     * before scaling. */
    ox += (float)v_xshift;
    oy += (float)(v_bufmode ? V_STBAR_DEST : v_yshift);

    if (bar_capture) {
        /* Coordinates are integral here (integer math upstream), so the
         * int16 record is lossless. A draw poking above the bar strip
         * cannot be composited; vanilla art never does, but fail safe. */
        if (bar_cur_n < V_BARCMD_MAX && oy >= (float)V_STBAR_DEST) {
            bar_cur[bar_cur_n].tex = tex;
            bar_cur[bar_cur_n].x   = (int16_t)ox;
            bar_cur[bar_cur_n].y   = (int16_t)oy;
            bar_cur_n++;
        } else {
            bar_legacy = true;
        }
        return;
    }
    v_blit_at(tex, ox, oy);
}

static void v_blit_at(const dt64_tex_t *tex, float ox, float oy)
{
    const int w = tex->width, h = tex->height;
    const int tw = w < DT64_TILE_W ? w : DT64_TILE_W;
    const int th = h < DT64_TILE_H ? h : DT64_TILE_H;

    for (int t0 = 0; t0 < h; t0 += th) {
        const int t1 = t0 + th > h ? h : t0 + th;
        for (int s0 = 0; s0 < w; s0 += tw) {
            const int s1 = s0 + tw > w ? w : s0 + tw;

            const float x0 = (ox + (float)s0) * UI_XSCALE;
            const float x1 = (ox + (float)s1) * UI_XSCALE;
            const float y0 = (oy + (float)t0) * UI_YSCALE;
            const float y1 = (oy + (float)t1) * UI_YSCALE;
            if (x1 < 0.0f || x0 > (float)SCREEN_W) continue;
            if (y1 < 0.0f || y0 > (float)SCREEN_H) continue;

            dt64_upload_tile(TILE0, tex, NULL, s0, t0, s1, t1);
            /* Scaled form even at 1x, where it is the same rectangle: one
             * code path, and the S range is stated explicitly rather than
             * implied by the destination width. */
            rdpq_texture_rectangle_scaled(TILE0, x0, y0, x1, y1,
                                          (float)s0, (float)t0,
                                          (float)s1, (float)t1);
            r_tri_spr++;
        }
    }
}

/* patch-pointer -> baked-texture memo.
 *
 * v_tex_for ran a linear scan of the ~2300-entry lump cache pointer table (a
 * 9 KB walk against an 8 KB data cache) plus a name-table lookup for EVERY
 * patch of EVERY frame -- and the status bar alone is ~25 patches a frame,
 * all recurring. The same few dozen pointers resolve to the same textures
 * until a level change swaps the texture arena, so one direct-mapped table
 * removes the entire cost. Cleared by p_level_reset_assets via
 * V_DrawResetCache. Failed resolutions are cached too: a patch with no baked
 * art stays that way for the whole level. */
#define V_TEXCACHE 128
static struct {
    const patch_t    *patch;
    const dt64_tex_t *tex;
} v_texcache[V_TEXCACHE];

void V_DrawResetCache(void)
{
    for (int i = 0; i < V_TEXCACHE; i++) v_texcache[i].patch = NULL;
    /* The bar composite is invalid across a level swap for the same reason
     * this cache is: the next level can rebake different art at the same
     * arena addresses, so pointer-equal capture entries must not count as
     * clean. The surface allocation itself survives. */
    bar_prev_n = -1;
    bar_prev_bank = -1;
    bar_surf_valid = false;
    bar_legacy = false;
}

/* Drop the composited strip's SURFACE, not just its contents.
 *
 * V_DrawResetCache invalidates what the strip holds and keeps the
 * allocation, which is right across a level swap because the screen has
 * not changed shape. A resolution change is the case where the allocation
 * itself is wrong: the strip is SCREEN_W wide and V_BAR_H_PX tall, and
 * both of those just moved. Freed here and lazily reallocated at the new
 * size by the next V_BarComposite. */
void V_BarFreeSurface(void)
{
    if (bar_surf.buffer) surface_free(&bar_surf);
    bar_surf = (surface_t){0};
    bar_prev_n = -1;
    bar_prev_bank = -1;
    bar_surf_valid = false;
    bar_legacy = false;
}

/* Begin recording the bar's draws instead of emitting them. */
void V_BarCaptureBegin(void)
{
    bar_capture = true;
    bar_cur_n = 0;
}

/* Stop recording; returns true when the composite must be rebuilt (the
 * command list or the palette bank -- which an RGBA16 composite bakes in --
 * changed). Commits the captured list either way. */
int V_BarCaptureEnd(void)
{
    bar_capture = false;
    const int bank = dt64_palette();
    const bool clean = bar_surf_valid && !bar_legacy &&
                       bar_cur_n == bar_prev_n && bank == bar_prev_bank &&
                       memcmp(bar_cur, bar_prev,
                              (size_t)bar_cur_n * sizeof *bar_cur) == 0;
    memcpy(bar_prev, bar_cur, (size_t)bar_cur_n * sizeof *bar_cur);
    bar_prev_n    = bar_cur_n;
    bar_prev_bank = bank;
    return !clean;
}

int V_BarValid(void) { return bar_surf_valid && !bar_legacy; }

/* Replay the committed list once into the persistent offscreen strip.
 * Runs at the frame loop's no-attach point (the same slot r_wipe_start's
 * blit proved out); rdpq_detach enqueues the SYNC_FULL that orders these
 * writes before the frame's re-blit reads. */
void V_BarComposite(void)
{
    if (bar_legacy) return;

    /* Same self-check as the melt's capture: a strip allocated for another
     * screen size is the wrong shape for this one, and reusing it puts the
     * bar somewhere other than the bottom of the picture. */
    if (bar_surf.buffer &&
        (bar_surf.width != SCREEN_W || bar_surf.height != V_BAR_H_PX)) {
        surface_free(&bar_surf);
        bar_surf = (surface_t){0};
        bar_surf_valid = false;
    }

    if (!bar_surf.buffer) {
        bar_surf = surface_alloc(FMT_RGBA16, SCREEN_W, V_BAR_H_PX);
        if (!bar_surf.buffer) { bar_legacy = true; return; }
        /* Stale dirty lines from allocation would write back over RDP
         * pixels; once, here -- the CPU never touches the buffer again. */
        data_cache_hit_writeback_invalidate(bar_surf.buffer,
                                            (size_t)bar_surf.stride * V_BAR_H_PX);
    }

    rdpq_attach(&bar_surf, NULL);
    rdpq_clear(RGBA32(0, 0, 0, 255));   /* no uninitialized texel survives */
    dt64_bind_tlut();
    V_BeginUI();
    for (int i = 0; i < bar_prev_n; i++)
        v_blit_at(bar_prev[i].tex,
                  (float)bar_prev[i].x,
                  (float)(bar_prev[i].y - V_STBAR_DEST));
    V_EndUI();
    rdpq_detach();
    bar_surf_valid = true;
}

/* Re-blit the composited strip: a handful of full-TMEM RGBA16 tiles in
 * place of the ~40 per-patch pairs the bar used to cost every frame. */
void V_BarBlit(void)
{
    /* RGBA16 must not sample through the TLUT, and the 4 KB tiles clobber
     * the TLUT half of TMEM -- rebind for the CI8 text that follows. */
    rdpq_mode_tlut(TLUT_NONE);
    /* 64x32 tiles, and the 32 is not decoration: RGBA16 at that size is
     * 4096 bytes, which is exactly the TMEM available beside the TLUT. The
     * strip was one tile tall while it was 32 rows, so the y loop did not
     * exist; at 480 the strip is 64 rows and a single 64x64 upload asks for
     * 8 KB, which the RDP refuses outright. Tiled on both axes it stays at
     * the same 4 KB per upload whatever the strip's height. */
    for (int ty = 0; ty < V_BAR_H_PX; ty += 32) {
        const int th = ty + 32 > V_BAR_H_PX ? V_BAR_H_PX - ty : 32;
        for (int tx = 0; tx < SCREEN_W; tx += 64) {
            const int tw = tx + 64 > SCREEN_W ? SCREEN_W - tx : 64;
            surface_t t = surface_make_sub(&bar_surf, tx, ty, tw, th);
            rdpq_tex_upload(TILE0, &t, NULL);
            rdpq_texture_rectangle(TILE0, tx, V_STBAR_DEST_PX + ty,
                                   tx + tw, V_STBAR_DEST_PX + ty + th, 0, 0);
            r_tri_spr++;
        }
    }
    dt64_bind_tlut();
    rdpq_mode_tlut(TLUT_RGBA16);
}

/* Find the baked art for a cached patch. */
static const dt64_tex_t *v_tex_for(const patch_t *patch)
{
    const unsigned slot = (unsigned)(((uintptr_t)patch) >> 4) & (V_TEXCACHE - 1);
    if (v_texcache[slot].patch == patch) return v_texcache[slot].tex;

    const dt64_tex_t *tex = NULL;
    const lumpindex_t lump = W_LumpForPointer(patch);
    if (lump >= 0) {
        const char *raw = W_LumpName(lump);
        if (raw) {
            char name[9];
            int n = 0;
            for (; n < 8 && raw[n]; n++) name[n] = raw[n];
            name[n] = 0;
            tex = (const dt64_tex_t *)p_level_resolve_ptr(name, "u_");
        }
    }

    v_texcache[slot].patch = patch;
    v_texcache[slot].tex   = tex;
    return tex;
}

/* Tile a 64x64 flat across the full 320x200 frame -- the finale text
 * backdrop. Flats are baked at 32x32 (see wad2n64), so sixty-odd COPY-mode
 * rectangles cover the screen; this draws once per finale frame, not in any
 * hot path. */
void V_DrawFlatFill(const char *flatname)
{
    const dt64_tex_t *tex =
        (const dt64_tex_t *)p_level_resolve_ptr(flatname, "f_");
    if (!tex) return;

    /* In UI coordinates: v_blit scales the destination, so tiling the 320
     * frame covers the whole viewport at any width. */
    for (int y = 0; y < 200; y += tex->height)
        for (int x = 0; x < SCREEN_BASE_W; x += tex->width)
            v_blit(tex, (float)x, (float)y);
}

void V_DrawPatch(int x, int y, patch_t *patch)
{
    if (!patch) return;

    const dt64_tex_t *tex = v_tex_for(patch);
    if (!tex) return;

    /* Doom positions by the patch's own offsets, as it does for sprites. */
    const bool standalone = !ui_active;
    if (standalone) V_BeginUI();
    v_blit(tex, (float)(x - SHORT(patch->leftoffset)),
                (float)(y - SHORT(patch->topoffset)));
    if (standalone) V_EndUI();
}

void V_DrawPatchDirect(int x, int y, patch_t *patch) { V_DrawPatch(x, y, patch); }

/* Doom tracks dirty rectangles so it can update only what changed. The RDP
 * redraws the whole frame regardless, so there is nothing to mark. */
void V_MarkRect(int x, int y, int width, int height)
{
    (void)x; (void)y; (void)width; (void)height;
}
