/* The seam microscope. See d_seam.h for what this is and why it exists. */

#include "d_seam.h"

uint32_t r_seam_break;
int r_seam_pose[4];

#if D_SEAMPROBE

/* Dump latch: 0 = watching, 1 = the next frame's commands are being
 * dumped. Arms on a STABLE sighting -- the same hole count several frames
 * running, which is what a player standing at a seam produces and a
 * player walking past one does not (the first cut of this armed on any
 * sighting and spent itself on the title screen). A small budget of
 * dumps per boot, spaced well apart, so a session can visit more than
 * one seam without burying the log. */
static uint32_t     seam_frame;
static int          seam_dumps;         /* dumps burned this boot */
static uint32_t     seam_last_dump;     /* frame of the last one */
static int          seam_stable;        /* consecutive frames, same count */
static int          seam_last_n;

/* The RDP command hook. Called by rdpq_debug's trace engine for every
 * command the RDP consumes, with the raw big-endian words -- including the
 * triangles the RSP assembled, which no CPU-side code ever sees. That is
 * the whole reason this probe exists: the coefficients are taken from the
 * stream the hardware read, not recomputed by arithmetic that might not
 * share the bug.
 *
 * EVERYTHING is kept now, whole commands. The first cut kept only the
 * four edge words on the theory that coverage decides existence -- and
 * the walk of a real dump then proved every hole pixel COVERED, which
 * moves the suspect list to the stages the edge words cannot see: the
 * z compare (a triangle's own z block), the alpha cut (SET_OTHER_MODES,
 * SET_BLEND_COLOR), and whatever the fills painted. So: triangles in
 * full, and every mode-bearing command beside them, in stream order.
 *
 * Buffered, not printed, from the hook. The trace engine can run close
 * to the RSP's buffer switching, and dump #2 of the first full-word cut
 * rebooted the console mid-print -- a quarter-second of blocking USB
 * writes from that context is the prime suspect. The hook only copies
 * words; d_seam_frame prints them from the main loop, where blocking is
 * merely slow. */
#define SEAM_MAX_QW   12288            /* ~96 KB: a frame is ~7k qwords */
#define SEAM_MAX_CMD  2048
static uint64_t seam_qw[SEAM_MAX_QW];
static struct { uint16_t off; uint8_t n; uint8_t cmd; } seam_cmd[SEAM_MAX_CMD];
static volatile int seam_nqw, seam_ncmd, seam_lost;

/* Set once the probe is initialised. The hook buffers every frame from then
 * on, rather than only while a dump is latched.
 *
 * The latched form could only ever dump the frame AFTER a sighting, which is
 * fine for its original use -- a player standing still, where frame N+1 has
 * the same hole -- and useless on a demo route, where the hole is transient.
 * Measured: three dumps taken that way all landed on frames whose scan found
 * ZERO holes, so the decode described a hole-free frame. Buffering always and
 * printing retroactively means the words belong to the very frame the scan
 * found the hole in. */
static volatile int seam_watching;

static void seam_hook(void *ctx, uint64_t *buf, int sz)
{
    (void)ctx;
    if (!seam_watching) return;

    const int cmd = (int)((buf[0] >> 56) & 0x3F);
    const int keep = (cmd >= 0x08 && cmd <= 0x0F) ||   /* triangles     */
                     cmd == 0x2D ||                    /* SET_SCISSOR   */
                     cmd == 0x2F ||                    /* SET_OTHER_MODES */
                     cmd == 0x39 ||                    /* SET_BLEND_COLOR */
                     cmd == 0x37 ||                    /* SET_FILL_COLOR  */
                     cmd == 0x36 ||                    /* FILL_RECT       */
                     cmd == 0x3F;                      /* SET_COLOR_IMAGE */
    if (!keep) return;
    if (seam_ncmd >= SEAM_MAX_CMD || seam_nqw + sz > SEAM_MAX_QW) {
        seam_lost++;
        return;
    }
    seam_cmd[seam_ncmd].off = (uint16_t)seam_nqw;
    seam_cmd[seam_ncmd].n   = (uint8_t)sz;
    seam_cmd[seam_ncmd].cmd = (uint8_t)cmd;
    for (int i = 0; i < sz; i++) seam_qw[seam_nqw + i] = buf[i];
    seam_nqw += sz;
    seam_ncmd++;
}

void d_seam_init(void)
{
    /* The trace engine is what walks the RDP's buffers on the CPU and
     * calls the hooks; without it the hook never fires. It also validates,
     * which this probe gets for free -- a stream error would print itself
     * next to the dump it invalidates. */
    rdpq_debug_start();
    rdpq_debug_install_hook(seam_hook, NULL);
    debugf("seamprobe: armed, clear is the probe colour\n");
    seam_watching = 1;
    debugf("seamprobe: watching wall column continuity\n");
}

void d_seam_frame(const surface_t *fb, uint16_t clear16)
{
    seam_frame++;

    /* The dump ran while this frame rendered; the blocking detach means
     * every buffer is consumed by now. Print the buffered commands from
     * here -- main loop context, where a slow USB write is only slow --
     * close the dump, and force this frame's hole report out next to it,
     * whatever the rate limiter thinks. */

    /* Only frames that drew the world. The title page and the menus leave
     * most of the screen legitimately clear-coloured -- the very first
     * boot frame spent the one-shot latch on sixteen solid magenta rows of
     * title screen, which is what this guard is the memorial of. The melt
     * covers the screen with the previous scene while it runs, so wait out
     * a second of level time past it. */
    {
        int D_InLevel(void), D_AutomapActive(void);
        extern int leveltime;
        if (!D_InLevel() || D_AutomapActive() || leveltime < 40) return;
    }

    /* The RDP wrote straight to RDRAM; drop any lines the CPU still holds
     * for this buffer. Same idiom, same reasoning as posehash_frame. */
    data_cache_hit_invalidate(fb->buffer, fb->stride * fb->height);

    /* World viewport only. The status bar owns the bottom of the screen
     * and legitimately leaves nothing clear-coloured; the top rows catch
     * sky, which covers. Runs, not pixels: a seam is a run of one to three
     * pixels, and runs keep a frame's report to a few lines. */
    enum { Y0 = 4, Y1 = 196, MAXRUN = 48 };
    struct { uint16_t y, x0, x1; } run[MAXRUN];
    int nrun = 0, holes = 0;

    for (int y = Y0; y < Y1 && y < fb->height; y++) {
        const uint16_t *row =
            (const uint16_t *)((const uint8_t *)fb->buffer +
                               (size_t)y * fb->stride);
        for (int x = 0; x < fb->width; x++) {
            if (row[x] != clear16) continue;
            holes++;
            if (nrun > 0 && run[nrun - 1].y == y &&
                run[nrun - 1].x1 + 1 == x) {
                run[nrun - 1].x1 = (uint16_t)x;
            } else if (nrun < MAXRUN) {
                run[nrun].y  = (uint16_t)y;
                run[nrun].x0 = run[nrun].x1 = (uint16_t)x;
                nrun++;
            }
        }
    }

    if (holes == 0) {
        seam_stable = 0;
        seam_last_n = 0;
        return;
    }

    /* Report continuously but not torrentially: the player navigates by
     * these lines. A dump prints its own hole list, so the two sit side by
     * side in the log whatever the rate limiter is doing. */
    static uint32_t last_report;
    if (seam_frame - last_report >= 8) {
        last_report = seam_frame;
        { extern uint32_t r_seam_break;
          if (r_seam_break) debugf("colbreak %lu\n",
                                   (unsigned long)r_seam_break); }
        debugf("seamhole f=%lu n=%d pose=%d,%d,%d,%d:",
               (unsigned long)seam_frame, holes,
               r_seam_pose[0], r_seam_pose[1], r_seam_pose[2], r_seam_pose[3]);
        for (int i = 0; i < nrun; i++)
            debugf(" %d:%d-%d", run[i].y, run[i].x0, run[i].x1);
        debugf(nrun == MAXRUN ? " ...\n" : "\n");
    }

    /* Stability: the same count several frames running means the player
     * is holding a pose at a seam, which is the frame worth spending a
     * dump on. Walking shows holes too, but a different count every
     * frame. */
    if (holes == seam_last_n) seam_stable++;
    else { seam_stable = 0; seam_last_n = holes; }

    /* Fire on the SHAPE of a sighting, not on the player holding still.
     *
     * The stability test assumed a human standing at a seam; on a demo route
     * the count changes every frame and it never armed. And the sighting
     * cannot be reproduced by pinning the pose afterwards, because FREEZE
     * restarts the level and its doors and lifts are then in a different
     * state than they were mid-route. So trigger on what the artifact looks
     * like instead: many runs, each one or two pixels wide, which is a tall
     * thin column and exactly the thing under investigation. */
    int thin = 0;
    for (int i = 0; i < nrun; i++)
        if (run[i].x1 - run[i].x0 <= 1) thin++;
    const int tallthin = (nrun >= 20 && thin >= nrun - 2);

    if ((seam_stable >= 8 || tallthin) &&
        holes >= 3 && holes <= 600 &&
        seam_dumps < 3 &&
        (seam_dumps == 0 || seam_frame - seam_last_dump > 600)) {
        seam_dumps++;
        seam_last_dump = seam_frame;
        seam_stable = 0;
        /* THIS frame's words, printed now that its holes are known. */
        debugf("seamdump begin f=%lu holes=%d\n",
               (unsigned long)seam_frame, holes);
        for (int i = 0; i < nrun; i++)
            debugf("  hole %d:%d-%d\n", run[i].y, run[i].x0, run[i].x1);
        for (int cc = 0; cc < seam_ncmd; cc++) {
            const uint64_t *q = &seam_qw[seam_cmd[cc].off];
            const int n = seam_cmd[cc].n;
            debugf("st %d %d", seam_cmd[cc].cmd, n);
            for (int i = 0; i < n; i++)
                debugf(" %016llx", (unsigned long long)q[i]);
            debugf("\n");
        }
        debugf("seamdump end f=%lu cmds=%d lost=%d\n",
               (unsigned long)seam_frame, seam_ncmd, seam_lost);
    }

    /* Clear for the next frame, whatever happened this one. */
    seam_nqw = seam_ncmd = seam_lost = 0;
}

#endif /* D_SEAMPROBE */
