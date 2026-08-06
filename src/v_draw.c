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
    rdpq_set_mode_copy(true);
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

static void v_blit(const dt64_tex_t *tex, float ox, float oy)
{
    ox += (float)v_xshift;
    oy += (float)(v_bufmode ? V_STBAR_DEST : v_yshift);
    const int w = tex->width, h = tex->height;
    const int tw = w < DT64_TILE_W ? w : DT64_TILE_W;
    const int th = h < DT64_TILE_H ? h : DT64_TILE_H;

    for (int t0 = 0; t0 < h; t0 += th) {
        const int t1 = t0 + th > h ? h : t0 + th;
        for (int s0 = 0; s0 < w; s0 += tw) {
            const int s1 = s0 + tw > w ? w : s0 + tw;

            const float x0 = ox + (float)s0, y0 = oy + (float)t0;
            const float x1 = ox + (float)s1, y1 = oy + (float)t1;
            if (x1 < 0.0f || x0 > (float)SCREEN_W) continue;
            if (y1 < 0.0f || y0 > (float)SCREEN_H) continue;

            dt64_upload_tile(TILE0, tex, NULL, s0, t0, s1, t1);
            rdpq_texture_rectangle(TILE0, x0, y0, x1, y1,
                                   (float)s0, (float)t0);
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

    for (int y = 0; y < 200; y += tex->height)
        for (int x = 0; x < SCREEN_W; x += tex->width)
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
