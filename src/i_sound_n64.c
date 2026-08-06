/*
 * i_sound_n64 -- sound effects through libdragon's mixer.
 *
 * This stands in for Doom's s_sound.c rather than porting it. That file owns
 * channel allocation, priority, music and the sound-blocking logic for
 * networked games; most of it is scaffolding around two things that actually
 * matter to how Doom sounds -- attenuation with distance, and stealing a
 * channel from a quieter sound when they run out. Both are here.
 *
 * Effects are baked to wav64 by the build and streamed off the cartridge, so
 * 67 of them cost no RAM. The mixer runs on the RSP, which the renderer also
 * uses; the channel count is deliberately small for that reason.
 */
#include "doom/doomtype.h"
#include "doom/doomstat.h"
#include "doom/m_fixed.h"
#include "doom/p_mobj.h"
#include "doom/sounds.h"
#include "doom/tables.h"

angle_t R_PointToAngle2(fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2);

#include <libdragon.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

/* Eight voices is what Doom itself mixes, and enough that a firefight does not
 * cut itself off. Each costs RSP time the renderer is also asking for. */
#define SFX_CHANNELS 8

/* One for streamed music, plus one spare.
 *
 * The spare is not optional: libdragon reserves the highest channel for its
 * own use and asserts if you configure it (ch != Mixer.num_channels-1). With
 * nine channels the music would sit on channel 8 -- exactly the reserved one.
 * Ten leaves music on 8 and the reservation on 9. */
#define TOTAL_CHANNELS (SFX_CHANNELS + 2)

/* Doom's own constants: beyond S_CLIPPING_DIST nothing is audible, and inside
 * S_CLOSE_DIST there is no attenuation at all. */
#define S_CLIPPING_DIST (1200 * 0x10000)
#define S_CLOSE_DIST    (160  * 0x10000)
#define S_ATTENUATOR    ((S_CLIPPING_DIST - S_CLOSE_DIST) >> FRACBITS)

typedef struct {
    wav64_t  wav;
    bool     loaded;
    bool     missing;
} sfxcache_t;

static sfxcache_t cache[NUMSFX];
static bool       sound_ready;

/* What each channel is playing, so a new sound can take the least important
 * voice rather than being dropped. */
static struct {
    const mobj_t *origin;
    int           sfxid;
    float         vol;
} chan[SFX_CHANNELS];

void I_Sound_Init(void)
{
    /* 48 kHz output, to match the music.
     *
     * The effects are 11025 Hz and neither need nor benefit from this -- 22050
     * was ample for them. The music does: the tracks are 48 kHz stereo, and
     * libdragon's mixer will not play a source faster than its output rate,
     * resampling down only within about one percent rather than by a factor of
     * two. Running the output at the source rate avoids resampling entirely
     * and is the best quality this hardware offers.
     *
     * It costs RSP time, which the renderer also wants. Effects measured
     * 1.2 ms/frame at 22050, so expect roughly double. If that proves too
     * expensive the alternative is downsampling the tracks on the host, which
     * moves the cost off the console entirely at the price of fidelity. */
    /* 32 kHz output. 24 kHz -- equal to the music's source rate -- would cut
     * RSP mixing cost by a quarter and looked safe on paper, but on real
     * hardware the music channel came out as white noise: the mixer's
     * exactly-1:1 resampling path (source rate == output rate == channel
     * frequency limit) does not survive contact with the actual RSP ucode.
     * The ~0.4 ms saving is not worth chasing that edge case; the output
     * stays above every source rate instead. */
    audio_init(32000, 4);
    mixer_init(TOTAL_CHANNELS);
    sound_ready = true;
}

/* Open a sound on first use. Doom's sfxinfo carries the lump name without the
 * DS prefix; the build tool wrote each one lowercased. */
static wav64_t *sfx_get(int id)
{
    if (id <= 0 || id >= NUMSFX) return NULL;

    sfxcache_t *c = &cache[id];
    if (c->missing) return NULL;
    if (c->loaded)  return &c->wav;

    const char *nm = S_sfx[id].name;
    if (!nm) { c->missing = true; return NULL; }

    char path[40];
    snprintf(path, sizeof path, "/ds%s.wav64", nm);
    for (char *p = path; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;

    /* wav64_open asserts rather than reporting a miss, and not every sfxinfo
     * entry has art in every IWAD, so check first. */
    const int fd = dfs_open(path);
    if (fd < 0) { c->missing = true; return NULL; }
    dfs_close(fd);

    char rompath[48];
    snprintf(rompath, sizeof rompath, "rom:%s", path);
    wav64_open(&c->wav, rompath);
    c->loaded = true;
    return &c->wav;
}

/* Volume and stereo position for a sound at `origin`, heard by `listener`.
 * Returns false when it is too far away to bother playing. */
static bool sfx_place(const mobj_t *listener, const mobj_t *origin,
                      float *vol, float *pan)
{
    *vol = 1.0f;
    *pan = 0.5f;

    if (!origin || !listener || origin == listener) return true;

    const fixed_t adx = abs(listener->x - origin->x);
    const fixed_t ady = abs(listener->y - origin->y);
    fixed_t dist = adx + ady - (adx > ady ? ady : adx) / 2;   /* P_AproxDistance */

    if (dist > S_CLIPPING_DIST) return false;

    if (dist > S_CLOSE_DIST) {
        const int d = (dist - S_CLOSE_DIST) >> FRACBITS;
        *vol = 1.0f - (float)d / (float)S_ATTENUATOR;
        if (*vol <= 0.0f) return false;
    }

    /* Pan by which side of the listener's facing the sound is on. */
    const angle_t ang  = R_PointToAngle2(listener->x, listener->y,
                                         origin->x, origin->y);
    const angle_t rel  = ang - listener->angle;
    const float   frel = (float)rel * (2.0f * (float)M_PI / 4294967296.0f);
    *pan = 0.5f - 0.45f * sinf(frel);
    return true;
}

/* Master SFX level from the options slider, 0..120 as Doom's menu scales
 * it. Applied to every placement, including live re-placements below. */
static float sfx_master = 1.0f;

void S_SetSfxVolume(int volume)
{
    sfx_master = volume <= 0 ? 0.0f : (volume >= 120 ? 1.0f : volume / 120.0f);
}

/* Re-place every active channel against the listener's current position --
 * walking away from a barrel fire fades it, turning pans it. Called once
 * per tic, chocolate's cadence. Sounds that have left the audible radius
 * stop outright and free their channel. */
void S_UpdateSounds(mobj_t *listener)
{
    if (!sound_ready) return;

    for (int i = 0; i < SFX_CHANNELS; i++) {
        if (!chan[i].origin) continue;
        if (!mixer_ch_playing(i)) { chan[i].origin = NULL; continue; }

        float vol, pan;
        if (!sfx_place(listener, chan[i].origin, &vol, &pan)) {
            mixer_ch_stop(i);
            chan[i].origin = NULL;
            continue;
        }
        chan[i].vol = vol;
        mixer_ch_set_vol_pan(i, vol * sfx_master, pan);
    }
}

void S_StartSound(void *origin_p, int sfx_id)
{
    if (!sound_ready || sfx_id <= 0 || sfx_id >= NUMSFX) return;

    const mobj_t *origin   = (const mobj_t *)origin_p;
    const mobj_t *listener = players[consoleplayer].mo;

    float vol, pan;
    if (!sfx_place(listener, origin, &vol, &pan)) return;

    wav64_t *w = sfx_get(sfx_id);
    if (!w) return;

    /* A free channel, else the quietest one. Doom drops the new sound instead;
     * taking the quietest keeps the loudest -- nearest -- events audible,
     * which is what the attenuation is trying to express anyway. */
    int pick = -1;
    for (int i = 0; i < SFX_CHANNELS; i++)
        if (!mixer_ch_playing(i)) { pick = i; break; }

    if (pick < 0) {
        float worst = vol;
        for (int i = 0; i < SFX_CHANNELS; i++)
            if (chan[i].vol < worst) { worst = chan[i].vol; pick = i; }
        if (pick < 0) return;            /* everything playing is louder */
    }

    wav64_play(w, pick);
    mixer_ch_set_vol_pan(pick, vol * sfx_master, pan);
    chan[pick].origin = origin;
    chan[pick].sfxid  = sfx_id;
    chan[pick].vol    = vol;
}

void S_StopSound(void *origin_p)
{
    if (!sound_ready) return;
    for (int i = 0; i < SFX_CHANNELS; i++)
        if (chan[i].origin == (const mobj_t *)origin_p) {
            mixer_ch_stop(i);
            chan[i].origin = NULL;
        }
}

/* Push mixed audio out. Called once a frame: the mixer fills whatever the
 * audio buffers have room for, so this self-regulates against frame rate. */
void I_Sound_Update(void)
{
    if (!sound_ready) return;

    /* Follow moving sources -- a monster that walks past should pan across. */
    const mobj_t *listener = players[consoleplayer].mo;
    for (int i = 0; i < SFX_CHANNELS; i++) {
        if (!mixer_ch_playing(i)) { chan[i].origin = NULL; continue; }
        if (!chan[i].origin) continue;

        float vol, pan;
        if (sfx_place(listener, chan[i].origin, &vol, &pan)) {
            mixer_ch_set_vol_pan(i, vol, pan);
            chan[i].vol = vol;
        }
    }

    mixer_try_play();
}
