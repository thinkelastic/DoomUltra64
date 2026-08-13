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
#include <string.h>
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
    /* RAW probe. joypad_set_rumble_active may itself be gated behind the
     * library's detection, which would make a "forced" pulse through it
     * vacuous -- so this speaks the Rumble Pak wire protocol directly:
     * 32 bytes of 0x01 to pak address 0xC000 turns the motor on, zeros
     * turn it off, one second on every four. No detection anywhere in
     * the path. NEVER a default: these writes land inside a real
     * Controller Pak's memory if one is seated. */
    {
        static int fr;
        uint8_t block[32];
        if (++fr >= 240) fr = 0;
        if (fr == 1) {
            memset(block, 0x01, sizeof block);
            joybus_accessory_write(0, 0xC000, block);
        } else if (fr == 60) {
            memset(block, 0x00, sizeof block);
            joybus_accessory_write(0, 0xC000, block);
        }
    }
    return;
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
