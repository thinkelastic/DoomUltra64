/*
 * d_rumble -- Rumble Pak feedback.
 *
 * The pak is a binary motor: on or off, no strength register. Perceived
 * strength is duty cycle, so this keeps a decaying energy level in [0,1]
 * and dithers it into per-frame on/off with an error accumulator -- a
 * level of 0.4 runs the motor exactly 40% of frames, evenly spread, which
 * is how N64 games always faked analog rumble. Joybus writes are not
 * free, so the command goes out only when the state actually changes.
 *
 * Sources feed D_RumbleAdd: the player-damage edge (the same damagecount
 * rise that drives the red flash) and nearby explosions (the dynamic
 * light walk already knows every blast's position and intensity). Both
 * live in d_bridge.c.
 */
#include <libdragon.h>
#include "d_rumble.h"

static float level;      /* current energy; decays per frame */
static float acc;        /* dither accumulator: duty == level, exactly */
static int   motor_on;   /* last state sent, so redundant sends are skipped */

void D_RumbleAdd(float amount)
{
    if (amount <= 0.0f) return;
    level += amount;
    if (level > 1.0f) level = 1.0f;
}

void D_RumbleFrame(void)
{
#if D_FORCERUMBLE
    /* Diagnostic build: drive the motor without asking the controller
     * whether it has one, and self-pulse every four seconds so no game
     * event is needed. For third-party pads with built-in rumble that
     * skip the accessory handshake. NEVER a default: the motor command
     * writes into pak address space, and a Controller Pak sitting there
     * would be corrupted. */
    { static int fr; if (++fr >= 240) fr = 0; if (fr == 1) D_RumbleAdd(0.9f); }
#else
    if (!joypad_get_rumble_supported(JOYPAD_PORT_1)) {
        /* Pak pulled mid-game: make sure the state machine lets go. */
        if (motor_on) {
            joypad_set_rumble_active(JOYPAD_PORT_1, false);
            motor_on = 0;
        }
        level = acc = 0.0f;
        return;
    }
#endif

    /* ~180 ms half-life at 60 fps: a single hit thumps and releases, a
     * rocket volley sustains. */
    level *= 0.94f;
    if (level < 0.02f) level = 0.0f;

    acc += level;
    int want = 0;
    if (acc >= 1.0f) { want = 1; acc -= 1.0f; }

    if (want != motor_on) {
        joypad_set_rumble_active(JOYPAD_PORT_1, want != 0);
        motor_on = want;
    }
}
