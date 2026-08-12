/* RSP occupancy sampling. See rspidle.h for what this measures and why it is
 * done this way rather than with libdragon's own profiler. */

#include <libdragon.h>

#include "rspidle.h"

/* 4 kHz. At a 12 ms frame that is ~48 samples per frame and ~5700 over a
 * 120-frame reporting window, which pins a percentage to about +/-1.3%.
 *
 * The cost is one interrupt per period, so this trades directly against the
 * thing being measured; it is why RSPIDLE is its own flag and not part of
 * HWSTAT. Lower it if the sampling itself starts showing up in frame times.
 */
#define SAMPLE_PERIOD_US 250

static volatile uint32_t s_samples;    /* total samples taken */
static volatile uint32_t s_rsp_halted; /* ... with the RSP halted */
static volatile uint32_t s_rdp_busy;   /* ... with the RDP pipe busy */

static timer_link_t *s_timer;

/* Last reported window, for the HUD. Plain ints: written by rspidle_report at
 * main-loop level, never from the sampling interrupt. */
static unsigned s_last_idle, s_last_rdp;

/* Interrupt context: two MMIO reads and three increments, nothing that can
 * block. Everything the report needs is derived later. */
static void rspidle_sample(int ovfl)
{
    (void)ovfl;

    const uint32_t sp = *SP_STATUS;
    const uint32_t dp = *DP_STATUS;

    s_samples++;
    /* Halted means the queue ran dry and the ucode executed `break`. The RSP
     * is doing nothing and is waiting for the CPU to hand it more work. */
    if (sp & SP_STATUS_HALTED)      s_rsp_halted++;
    if (dp & DP_STATUS_PIPE_BUSY)   s_rdp_busy++;
}

void rspidle_init(void)
{
    if (s_timer) return;

    /* Refcounted inside libdragon, and nothing else here uses timers. */
    timer_init();
    s_timer = new_timer(TIMER_TICKS(SAMPLE_PERIOD_US), TF_CONTINUOUS,
                        rspidle_sample);
}

void rspidle_report(void)
{
    uint32_t samples, halted, rdp;

    /* Snapshot and clear together, so samples taken between the read and the
     * reset are not silently dropped from a window they were counted in. */
    disable_interrupts();
    samples = s_samples;
    halted  = s_rsp_halted;
    rdp     = s_rdp_busy;
    s_samples = s_rsp_halted = s_rdp_busy = 0;
    enable_interrupts();

    if (!samples) {
        debugf("rspidle: no samples -- timer not running\n");
        return;
    }

    /* Integer percentages: %f would drag newlib's float printf into the ROM
     * for a debug line, the same reason the HUD avoids it.
     *
     * rsp_idle is the number this exists for. rdp_busy is the cross-check:
     * "RSP not halted" counts real work AND the ucode's spin while it waits
     * on the RDP's DMA FIFO, so a low idle figure only means the RSP has no
     * slack if the RDP is not busy at the same time. */
    s_last_idle = (unsigned)((uint64_t)halted * 100 / samples);
    s_last_rdp  = (unsigned)((uint64_t)rdp    * 100 / samples);

    debugf("rspidle: n=%lu rsp_idle=%lu%% rdp_busy=%lu%%\n",
           (unsigned long)samples,
           (unsigned long)s_last_idle,
           (unsigned long)s_last_rdp);
}

void rspidle_last(unsigned *rsp_idle_pct, unsigned *rdp_busy_pct)
{
    if (rsp_idle_pct) *rsp_idle_pct = s_last_idle;
    if (rdp_busy_pct) *rdp_busy_pct = s_last_rdp;
}
