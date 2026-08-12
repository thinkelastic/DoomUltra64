/* Frame fingerprinting. See posehash.h for what this is for. */

#include <libdragon.h>

#include "posehash.h"

/* One line per hashed frame, tagged with the tic so two runs can be diffed
 * even if one of them dropped frames or started a level a moment later. */
void posehash_frame(const surface_t *fb)
{
    /* leveltime, not gametic.
     *
     * gametic counts from boot, and tics only advance while the game loop is
     * running -- so the tic the demo happens to START on depends on how long
     * level loading took, which is wall-clock work that differs run to run.
     * Keyed to gametic, the same picture lands on a different tic in each
     * build and every frame reads as changed. The symptom that gave it away
     * was two runs reporting the identical pair of hashes at one tic with the
     * sides swapped: both builds drew both pictures, just offset.
     *
     * leveltime restarts at zero with the level and advances one per tic, so
     * it names a position along the route rather than a moment in the session.
     */
    extern int leveltime;
    int D_InLevel(void);

    /* Several frames can render within one tic. With INTERP=0 they are the
     * same picture, so hashing the first is enough and keeps the cost off the
     * rest. */
    static int last_tic = -1;

    if (D_POSEHASH <= 0) return;
    /* Menus and the title page are not on the route and have no leveltime. */
    if (!D_InLevel()) return;
    if (leveltime == last_tic) return;
    if (leveltime % D_POSEHASH) return;
    last_tic = leveltime;

    /* Belt and braces: main.c takes the blocking rdpq_detach_wait path when
     * POSEHASH is on, so the frame is already complete by here. rspq_wait is
     * not sufficient on its own -- it drains the RSP, not the RDP behind it --
     * and relying on it was what made this harness report 141 of 144 frames
     * changed by an edit that provably changes no arithmetic. */
    rspq_wait();

    /* The RDP wrote these pixels straight to RDRAM, so any lines the CPU still
     * holds for this buffer are stale from an earlier frame that used it.
     * Invalidate rather than read uncached: 150 KB of uncached reads costs
     * tens of milliseconds, and this runs inside the emulator. */
    const int bytes = fb->stride * fb->height;
    data_cache_hit_invalidate(fb->buffer, bytes);

    /* FNV-1a over the visible pixels. Not a checksum with any pedigree -- it
     * only has to change when a pixel changes, and be cheap. Read 32 bits at a
     * time; a 16 bpp row is a whole number of words at every width used here. */
    uint32_t h = 2166136261u;
    for (int y = 0; y < fb->height; y++) {
        const uint32_t *row =
            (const uint32_t *)((const uint8_t *)fb->buffer + (size_t)y * fb->stride);
        for (int x = 0; x < fb->width / 2; x++) {
            h ^= row[x];
            h *= 16777619u;
        }
    }

    debugf("posehash: tic=%d h=%08lx\n", leveltime, (unsigned long)h);
}
