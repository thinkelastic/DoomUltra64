/* RSP occupancy sampling.
 *
 * Answers one question: is the RSP idle enough that moving submit work onto it
 * would pay? The frame is CPU-bound in the submit path on hardware, but that
 * only makes an RSP overlay worth writing if the RSP is actually waiting for
 * us. If it is already saturated, an overlay moves the queue, not the frame.
 *
 * libdragon ships a profiler that would answer this directly. It cannot be
 * used here: enabling it overflows the RSP's IMEM budget for rdpq's microcode
 * by 112 bytes. See the RSPQPROF comment in the Makefile.
 *
 * This measures the same thing from the CPU side instead, and can do so
 * because of how rspq idles: when the queue runs dry, RSPQCmd_WaitNewInput in
 * rsp_queue.inc executes `break`, which HALTS the RSP until the CPU signals
 * more work. So SP_STATUS's halted bit is a true "RSP has nothing to do"
 * signal, not a guess, and sampling it over a frame gives the duty cycle.
 *
 * Enabled by RSPIDLE=1, which is deliberately separate from HWSTAT: the
 * sampler adds a periodic interrupt, and HWSTAT builds are what the demo-route
 * frame-time medians come from. Do not read absolute frame times from a build
 * that has this on.
 */
#ifndef RSPIDLE_H
#define RSPIDLE_H

/* Start sampling. Safe to call once, after the RCP subsystems are up. */
void rspidle_init(void);

/* Emit the window's percentages over the debug channel and start a new one. */
void rspidle_report(void);

/* The last window's percentages, for the on-screen line. Zero until the first
 * report. Reading them off the television is the point: it needs no USB cable
 * and no host, so the number is available wherever the console is. */
void rspidle_last(unsigned *rsp_idle_pct, unsigned *rdp_busy_pct);

#endif
