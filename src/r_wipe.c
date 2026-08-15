/*
 * The screen melt.
 *
 * Doom wipes between game states by sliding columns of the outgoing frame
 * down over the incoming one, each column starting at its own random
 * offset and accelerating. Vanilla does it by copying bytes between two
 * CPU-side screen buffers, which this port has none of: the frame is built
 * by the RDP straight into a framebuffer the CPU never reads.
 *
 * So the melt is drawn rather than copied. The outgoing frame is kept
 * (one memcpy out of the framebuffer, once, when the wipe starts) and each
 * frame after that the new scene is rendered normally and vertical strips
 * of the saved one are drawn back over it at descending offsets. A strip
 * is a texture like any other -- 16-bit colour straight out of the
 * framebuffer, no palette involved -- so this costs an upload and a
 * rectangle per strip and nothing else.
 *
 * Strips are WIPE_STRIP pixels wide rather than one, which is the whole
 * concession to the hardware: 320 single-column uploads a frame would be
 * absurd, and at four the melt still reads as Doom's. TMEM decides the
 * ceiling -- a strip is width * height * 2 bytes and must fit in 4 KB, so
 * at 4 texels wide that is 1920 bytes at 240 rows and 3840 at 480. Both fit
 * the full tile at either height.
 */
#include "r_wipe.h"

#include <libdragon.h>
#include <string.h>

#include "r_wall.h"          /* SCREEN_W / SCREEN_H */

/* Strip width scales with the horizontal sampling rate so the melt looks the
 * same on screen: at 640x240 a pixel is half as wide, so 8 framebuffer
 * columns cover the same distance 4 do at 320. That also keeps WIPE_COLS --
 * and the melt's random walk -- identical between modes. */
/* The multiplier is floored at 1: the width is a runtime value now, and a
 * screen narrower than the 320 frame would make this zero and turn the
 * WIPE_COLS division below into a divide by zero -- which on MIPS is a
 * trap, not a wrong number. */
#define WIPE_STRIP   (4 * (SCREEN_W > SCREEN_BASE_W ? SCREEN_W / SCREEN_BASE_W : 1))
#define WIPE_COLS    (SCREEN_W / WIPE_STRIP)
/* Most columns the melt can ever need: the widest screen divided by the
 * narrowest strip, which is what col_y has to be sized for now that the
 * width is chosen at runtime. */
#define WIPE_COLS_MAX (SCREEN_MAX_W / 4)

/* Rows per upload. The height this pass was written for and the only one it
 * is known to survive; see the note in r_wipe_draw. */
#define WIPE_CHUNK_H  240

/* Doom's melt, per strip instead of per column: a negative offset is a
 * delay before this strip starts moving, then it accelerates to 8 pixels
 * a tic. */
static int   col_y[WIPE_COLS_MAX];
static bool  wiping;
static surface_t saved;
static bool  saved_valid;

/* Deterministic, and deliberately not Doom's M_Random: drawing must not
 * disturb the game's random sequence, which demo playback depends on. */
static uint32_t wipe_seed = 1;

static int wipe_rand(void)
{
    wipe_seed = wipe_seed * 1103515245u + 12345u;
    return (int)((wipe_seed >> 16) & 0x7FFF);
}

bool r_wipe_active(void) { return wiping; }

void r_wipe_start(const surface_t *from)
{
    if (wiping || !from) return;

    /* The SOURCE has to be the live screen size, and this is not paranoia:
     * the capture below walks `from` to SCREEN_H, and screen_apply() changes
     * SCREEN_H at the top of a frame while prev_fb still points at the frame
     * BEFORE it -- one 240-row buffer. surface_make_sub does pointer
     * arithmetic and does not bounds-check, so rows 240..479 come out of
     * whatever follows that buffer in RDRAM, which is the next framebuffer
     * of the display's set of three, holding a nearly identical picture.
     * The melt then shows the frame twice, stacked: the reported duplicate
     * of the screen top and bottom in 480i.
     *
     * Refused outright rather than clamped. A melt is a transition, and
     * skipping one is invisible next to half a screen of the wrong frame;
     * there is no partial capture here that is better than none. */
    if ((int)from->width != SCREEN_W || (int)from->height != SCREEN_H) return;

    /* A capture from a DIFFERENT screen size is worse than none: the melt
     * would draw a 240-row image down a 480-row screen, leaving the frame
     * it captured -- status bar and all -- sitting across the middle of the
     * picture. The detail menu frees this on the way through, but the
     * surface outlives any single switch, so it checks its own shape rather
     * than trusting that. */
    if (saved_valid &&
        (saved.width != SCREEN_W || saved.height != SCREEN_H)) {
        surface_free(&saved);
        saved_valid = false;
    }

    if (!saved_valid) {
        saved = surface_alloc(FMT_RGBA16, SCREEN_W, SCREEN_H);
        if (!saved.buffer) return;                  /* no memory: no wipe */
        saved_valid = true;
        /* The allocation can arrive with stale dirty lines whose eventual
         * writeback would land on top of RDP-written pixels. Once, here:
         * after this the CPU never touches the buffer again. */
        data_cache_hit_writeback_invalidate(saved.buffer,
                                            (size_t)saved.stride * SCREEN_H);
    }

    /* Copy the outgoing frame with the RDP, not the CPU. The memcpy this
     * replaces sat behind a full rspq_wait and dragged 150 KB through the
     * D-cache -- a guaranteed ~10 ms hitch frame at every game-state
     * transition, the longest stall left in the game. Enqueued COPY-mode
     * blits need no wait at all: queue order already puts them after the
     * commands that finished drawing `from`, and scanout reading the same
     * finished buffer sees consistent pixels. 64x32 RGBA16 tiles fill TMEM
     * exactly (4 KB, no TLUT) and DMA as 32 rows of 128 bytes -- far
     * kinder to RDRAM than the melt's own tall thin strips. */
    rdpq_attach(&saved, NULL);
    rdpq_set_mode_copy(false);
    /* RGBA16 MUST NOT sample through the TLUT -- V_BarBlit takes the same
     * precaution for the same tiles. Without it these blits read palette
     * entries instead of pixels, and worse, an enabled TLUT reserves the
     * upper half of TMEM: the tile then has 2048 bytes rather than 4096.
     * That is exactly the 240-vs-480 threshold this went wrong at. */
    rdpq_mode_tlut(TLUT_NONE);
    for (int ty = 0; ty < SCREEN_H; ty += 32)
        for (int tx = 0; tx < SCREEN_W; tx += 64) {
            const int w = tx + 64 > SCREEN_W ? SCREEN_W - tx : 64;
            const int h = ty + 32 > SCREEN_H ? SCREEN_H - ty : 32;
            surface_t tile =
                surface_make_sub((surface_t *)from, tx, ty, w, h);
            rdpq_tex_upload(TILE0, &tile, NULL);
            rdpq_texture_rectangle(TILE0, tx, ty, tx + w, ty + h, 0, 0);
        }
    rdpq_detach();

    col_y[0] = -(wipe_rand() % 16);
    for (int i = 1; i < WIPE_COLS; i++) {
        int r = (wipe_rand() % 3) - 1;
        col_y[i] = col_y[i - 1] + r;
        if (col_y[i] > 0)   col_y[i] = 0;
        if (col_y[i] < -15) col_y[i] = -15;
    }
    wiping = true;
}

/* Advance one tic. Returns true while the melt still has somewhere to go. */
bool r_wipe_tick(void)
{
    if (!wiping) return false;

    bool moving = false;
    for (int i = 0; i < WIPE_COLS; i++) {
        if (col_y[i] < 0) {
            col_y[i]++;
            moving = true;
        } else if (col_y[i] < SCREEN_H) {
            const int dy = col_y[i] < 16 ? col_y[i] + 1 : 8;
            col_y[i] += dy;
            if (col_y[i] > SCREEN_H) col_y[i] = SCREEN_H;
            moving = true;
        }
    }
    if (!moving) wiping = false;
    return wiping;
}

void r_wipe_draw(void)
{
    if (!wiping || !saved_valid) return;

    /* Straight texel copy: no shading, no depth, no filtering. The saved
     * frame is already the finished picture. */
    rdpq_set_mode_copy(false);
    /* Same reason as the capture: RGBA16 must not sample through the TLUT,
     * or these blits read palette entries instead of pixels.
     *
     * It is NOT a TMEM size limit, which an earlier version of this comment
     * claimed. libdragon budgets the tile by FORMAT alone -- 4096 bytes for
     * RGBA16 whether or not a TLUT is bound (rdpq_tex.c) -- so a 4-texel
     * strip at 8 bytes a row fits 480 rows either way. The duplicated frame
     * had a different cause; see scr_settle in main.c. */
    rdpq_mode_tlut(TLUT_NONE);

    for (int i = 0; i < WIPE_COLS; i++) {
        const int y = col_y[i];
        if (y >= SCREEN_H) continue;              /* fully melted away */

        const int x0 = i * WIPE_STRIP;
        const int h  = SCREEN_H - (y > 0 ? y : 0);
        /* Two rows, not one.
         *
         * A single-scanline strip is drawn correctly -- tile and rectangle
         * are both 4x1 and the texel it samples is in range -- but COPY mode
         * makes the RDP's rectangle coordinates inclusive, so YL == YH, and
         * the command validator reads that as a zero-height rect and reports
         * an access at t = -1 on every one. The strip is the last row of an
         * already-melted column and is about to vanish; dropping it costs
         * nothing visible and keeps DEBUG=1 logs free of an error that is
         * not one. */
        if (h <= 1) continue;

        /* CHUNKED at the height this melt was designed around.
         *
         * The geometry is not the problem -- instrumented on a real 480i run,
         * the capture, the saved surface and the strip are all exactly
         * 4 x 480 with a 640-byte stride. What does not survive is the tall
         * upload itself: this pass was written when a strip was at most 240
         * rows ("width * 240 * 2 bytes" in the header note above), and at
         * 480 the frame comes back duplicated down the column. Rather than
         * keep guessing at which RDP limit that is -- three theories have
         * been wrong, including a TMEM one that libdragon's own source
         * refutes -- the strip is split into pieces of the height that has
         * always worked. Two uploads per column during a melt, which lasts
         * under a second and is not a frame-rate concern.
         *
         * Each piece takes its source rows from the same offset it draws to,
         * so the melt is unchanged: the saved frame still slides down as one
         * image, it is simply delivered in bands. */
        for (int off = 0; off < h; off += WIPE_CHUNK_H) {
            int ch = h - off;
            if (ch > WIPE_CHUNK_H) ch = WIPE_CHUNK_H;
            if (ch <= 1) break;         /* same zero-height guard as above */

            surface_t strip =
                surface_make_sub(&saved, x0, off, WIPE_STRIP, ch);
            rdpq_tex_upload(TILE0, &strip, NULL);
            const int dy = (y > 0 ? y : 0) + off;
            rdpq_texture_rectangle(TILE0, x0, dy,
                                   x0 + WIPE_STRIP, dy + ch, 0, 0);
        }
    }

    /* Hand the TLUT back: the UI bracket that follows draws CI8 text and
     * would otherwise sample it as raw colour. */
    dt64_bind_tlut();
    rdpq_mode_tlut(TLUT_RGBA16);
}

void r_wipe_free(void)
{
    if (saved_valid) {
        surface_free(&saved);
        saved_valid = false;
    }
    wiping = false;
}
