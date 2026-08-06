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
 * ceiling -- a strip is width * 240 * 2 bytes and must fit in 4 KB.
 */
#include "r_wipe.h"

#include <libdragon.h>
#include <string.h>

#include "r_wall.h"          /* SCREEN_W / SCREEN_H */

#define WIPE_STRIP   4
#define WIPE_COLS    (SCREEN_W / WIPE_STRIP)

/* Doom's melt, per strip instead of per column: a negative offset is a
 * delay before this strip starts moving, then it accelerates to 8 pixels
 * a tic. */
static int   col_y[WIPE_COLS];
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

    if (!saved_valid) {
        saved = surface_alloc(FMT_RGBA16, SCREEN_W, SCREEN_H);
        if (!saved.buffer) return;                  /* no memory: no wipe */
        saved_valid = true;
    }

    /* One copy of the outgoing frame. The framebuffer is written by the
     * RDP, so make sure what is in memory is what was drawn before it is
     * read back through the CPU's cache. */
    rspq_wait();
    for (int y = 0; y < SCREEN_H; y++)
        memcpy((uint8_t *)saved.buffer + (size_t)y * saved.stride,
               (const uint8_t *)from->buffer + (size_t)y * from->stride,
               (size_t)SCREEN_W * 2);
    data_cache_hit_writeback_invalidate(saved.buffer,
                                        (size_t)saved.stride * SCREEN_H);

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

    for (int i = 0; i < WIPE_COLS; i++) {
        const int y = col_y[i];
        if (y >= SCREEN_H) continue;              /* fully melted away */

        const int x0 = i * WIPE_STRIP;
        const int h  = SCREEN_H - (y > 0 ? y : 0);
        if (h <= 0) continue;

        surface_t strip = surface_make_sub(&saved, x0, 0, WIPE_STRIP, h);
        rdpq_tex_upload(TILE0, &strip, NULL);
        rdpq_texture_rectangle(TILE0, x0, y > 0 ? y : 0,
                               x0 + WIPE_STRIP, (y > 0 ? y : 0) + h, 0, 0);
    }
}

void r_wipe_free(void)
{
    if (saved_valid) {
        surface_free(&saved);
        saved_valid = false;
    }
    wiping = false;
}
