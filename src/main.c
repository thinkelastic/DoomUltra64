/*
 * DoomUltra64 -- the frame loop.
 *
 * Owns the display, the frame's structure and the order the renderer's passes
 * run in; the passes themselves live in r_*.c and the game logic behind
 * d_bridge.c. Two things here are load-bearing and easy to disturb:
 *
 *   - the RESOLUTION is a runtime choice (the options menu's Graphic Detail),
 *     so SCREEN_W/SCREEN_H are variables and the switch may only happen at
 *     the top of a frame, where nothing is in flight;
 *   - the frame tail is PIPELINED -- rdpq_detach_show returns while the RDP
 *     is still rasterising, and the work after it is deliberately placed in
 *     that shadow. Blocking there gives the time back.
 */

#include <libdragon.h>
#include <math.h>
#include <stdio.h>

#include "dt64.h"
#include "mem.h"
#include "mus_n64.h"
#include "p_level.h"
#include "r_bsp.h"
#include "r_flat.h"
#include "r_halo.h"
#include "r_sky.h"
#include "r_env.h"
#include "r_sprite.h"
#include "r_vapor.h"
#include "wad.h"
#include "r_wall.h"
#include "r_wipe.h"
#include "scene.h"
#include "d_seam.h"

#if D_RSPQPROF
/* Not reached from libdragon.h. Brings RSPQ_PROFILE in with it. */
#include <rspq_profile.h>
#if !RSPQ_PROFILE
#error "RSPQPROF=1 needs a libdragon built with RSPQ_PROFILE=1. Build with \
./build.sh RSPQPROF=1, which selects the profiling toolchain image. Against \
the stock one every rspq_profile_* call is an empty stub: the ROM would link, \
run, and report a confident zero for every counter."
#endif
/* RCP ticks, not the CPU's: the profiler counts in the RCP's clock domain and
 * RCP_TICKS_TO_USECS is private to libdragon's own dump. */
#define RSPQPROF_US(t) ((unsigned long)(((uint64_t)(t) * 1000000ULL) / RCP_FREQUENCY))
#endif

#if D_RSPIDLE
#include "rspidle.h"
#endif

#if D_POSEHASH
#include "posehash.h"
#endif

#define MAPNAME "E1M1"

/* The IWAD this cartridge was baked from, for the message shown when it
 * cannot be found. Naming the right file matters: a Doom II cartridge asking
 * for DOOM.WAD would send the player looking for the wrong one. */
#if D_DOOM2
#define D_IWAD_WANTED "DOOM2.WAD"
#else
#define D_IWAD_WANTED "DOOM.WAD"
#endif

/* Doom's own subsystems, declared here rather than through their headers:
 * those speak Doom's types and this file speaks the renderer's. */
void D_LoadLevel(int episode, int map);
void D_InitPicTables(int count);
void W_N64_Init(void);
void    D_UI_Init(void);
void    D_UI_LevelStart(void);
void    D_UI_Ticker(void);
void    D_UI_Draw(void);
int     D_UI_HandleInput(joypad_buttons_t held);

void Z_Init(void);
void I_Sound_Init(void);
void I_Sound_Update(void);

static bool       level_loaded;
static r_camera_t cam;

/* --- runtime resolution ---------------------------------------------------
 *
 * The options menu's GRAPHIC DETAIL picks between HIGH (320x480 interlaced)
 * and LOW (320x240). Doom's own M_ChangeDetail already routes through
 * R_SetViewSize, so the menu needed no new item; d_bridge turns that call
 * into a request here.
 *
 * REQUEST, not an immediate switch. R_SetViewSize is reached from game
 * logic, which this loop runs after handing the frame to the RDP -- tearing
 * the framebuffers down at that moment would free memory the RDP is still
 * rasterising into. The request is honoured at the top of the next frame,
 * before display_get, which is the one point where nothing is in flight. */
/* Seeded with the BOOT mode, emphatically not left at zero. Zero is a
 * request in this scheme -- it differs from the live size -- so a default
 * of 0,0 made the very first frame switch the screen to 0x0 before the
 * game had asked for anything, and the first integer division by a screen
 * dimension trapped: MIPS divides are followed by a teq, so it surfaced as
 * a divide-by-zero exception rather than as a blank picture. */
static int pend_scr_w = SCREEN_BOOT_W, pend_scr_h = SCREEN_BOOT_H;

/* Whether the boot allocation was large enough for the HIGH mode. When it
 * was not, the detail menu still toggles but cannot reach 480i -- better a
 * setting that does nothing than an assert in the middle of a game. */
static bool hires_ok;

/* Frames left before every framebuffer has been redrawn at the current size.
 * Set by screen_apply, counted down in the loop; the melt refuses to start
 * while it is non-zero. */
static int scr_settle;

/* 320x480 interlaced. libdragon ships constants for 320x480 and 640x480 but
 * not this one, and it is the cheapest 480i this renderer can draw: the
 * column count -- which is what the occlusion machinery costs scale with --
 * does not change from 320x240 at all. display_init's own rules are met
 * (width divisible by 4 for 16bpp, height inside 720). */
static const resolution_t RES_320x480 = {
    .width = 320, .height = 480, .interlaced = INTERLACE_HALF
};

void D_RequestScreen(int w, int h) { pend_scr_w = w; pend_scr_h = h; }

/* The VI's resample filter decision, in ONE place.
 *
 * VIFILTER=0 was DEAD, and silently so. It reached only the display_init in
 * main(), and the boot-mode drop immediately after it passed FILTERS_RESAMPLE
 * unconditionally -- a branch taken in every default build, because
 * SCREEN_BOOT_H is 240 whenever HIRES=0. So the lever could be set to 0, the
 * build could come up, and the filter was on the whole time.
 *
 * That is worse than a missing optimisation. The filter reads the coverage
 * bits the RDP writes at polygon edges and blends across them during scanout,
 * which is exactly the kind of thing the seam investigation was hunting -- and
 * a0603c9 lists "the VI resample filter" among the causes ELIMINATED on
 * evidence. Any such elimination done with a default build tested nothing.
 *
 * Keyed on the framebuffer's WIDTH, not on which lever selected it: the VI
 * always scales to a 640-wide virtual output, so what decides this is how much
 * horizontal resize is left. 640 has none and the filter only smears 1:1
 * content; 320 and 512 both upscale and want it. */
static filter_options_t vi_filter_for(int w)
{
#if defined(D_VIFILTER) && D_VIFILTER == 0
    (void)w;
    return FILTERS_DISABLED;
#else
    return w == 640 ? FILTERS_DISABLED : FILTERS_RESAMPLE;
#endif
}

static void screen_apply(void)
{
    void V_BarFreeSurface(void);

    /* A degenerate request is a bug in the caller, not something to render:
     * refuse it rather than divide by it later. */
    /* A 480-line request the boot allocation cannot back is refused here,
     * where it is a no-op, rather than inside display_change, where the
     * size assertion would end the session. */
    if (!hires_ok && pend_scr_h >= 480) { pend_scr_h = r_scr_h; pend_scr_w = r_scr_w; }

    if (pend_scr_w < SCREEN_BASE_W || pend_scr_h < SCREEN_BASE_H ||
        pend_scr_w > SCREEN_MAX_W  || pend_scr_h > SCREEN_MAX_H) {
        pend_scr_w = r_scr_w;
        pend_scr_h = r_scr_h;
        return;
    }
    if (pend_scr_w == r_scr_w && pend_scr_h == r_scr_h) return;

    /* DRAIN FIRST. The two surfaces freed below are RDP texture SOURCES for
     * the frame submitted at the bottom of the previous loop iteration, which
     * is deliberately not waited on -- the melt samples `saved` and the status
     * bar samples `bar_surf`. display_change drains nothing (it only stages VI
     * state), and surface_free hands the block back to newlib, which writes
     * its own free-chunk bookkeeping into it. Freeing while the RDP still
     * reads is a torn status bar at best and a wild DMA at worst.
     *
     * Costs one drain per detail toggle and nothing during play: the
     * unchanged-size early-out above returns on every other frame. The tree
     * already takes this precaution in d_verify.c. */
    rspq_wait();

    /* Everything sized to the old screen goes next: the melt's saved frame
     * and the status bar's composited strip are both framebuffers in their
     * own right, and both are lazily reallocated at the new size. */
    r_wipe_free();
    V_BarFreeSurface();

    /* display_change, NOT close-then-init.
     *
     * It allocates nothing: it asserts the requested mode fits the buffers
     * already allocated and then just reconfigures the VI. Tearing the
     * display down and building it back up did allocate -- three ~480 KB
     * blocks -- and by the time a level has carved up the heap those may
     * not be contiguous even when the free total is ample. libdragon
     * asserts "Out of memory" inside display_init, which cannot be caught
     * or retried, so the switch has to be incapable of failing rather than
     * merely likely to succeed. Claiming the largest footprint once at
     * boot (see display_init) makes every later switch free. */
    display_change(pend_scr_h >= 480 ? RES_320x480 : RESOLUTION_320x240,
                   DEPTH_16_BPP, 3, GAMMA_NONE,
                   vi_filter_for(pend_scr_w));

    /* Every framebuffer now holds content of the WRONG SIZE until it has
     * been drawn at the new one, and there are three of them. A melt started
     * before they have all come round captures whichever buffer prev_fb
     * happens to point at, and the part of it below the old height is
     * pre-switch content -- the frame duplicated top and bottom. It needs a
     * screen change and a wipe close together, which is exactly what booting
     * into HIGH from saved options produces: the mode switches on frame one
     * and the title screen follows immediately. */
    scr_settle = 3;

    r_scr_w = pend_scr_w;
    r_scr_h = pend_scr_h;
    r_scr_uix = (float)r_scr_w / (float)SCREEN_BASE_W;
    r_scr_uiy = (float)r_scr_h / (float)SCREEN_BASE_H;

    /* The projection follows the frame, on the same terms as at boot. */
    cam.focal_x = SCREEN_W * 0.5f;
    cam.focal_y = SCREEN_BASE_W * 0.5f * r_scr_uiy;

    debugf("screen: %dx%d, ui %.2fx%.2f\n",
           r_scr_w, r_scr_h, (double)r_scr_uix, (double)r_scr_uiy);
}
#if D_TESTROOM
static dt64_tex_t tex_startan, tex_brown, tex_compute;
static r_wall_t walls[SCENE_NUM_WALLS];
#endif

/* --- stress control ------------------------------------------------------
 *
 * 60 fps on the base scene is a floor, not a margin: the VI caps the frame
 * rate, so a frame finishing in 2 ms and one finishing in 16 ms both report
 * 60. What matters for level scale is how much of the ~16.7 ms budget a frame
 * actually costs, and how that scales with geometry.
 *
 * A/B subdivide each wall into more collinear segments. The picture is
 * unchanged and screen coverage is identical, so this isolates the cost of
 * geometry and TMEM uploads from the cost of filling pixels. */
#define MAX_SPLIT 16

#if D_TESTROOM
static r_wall_t stress_walls[SCENE_NUM_WALLS * MAX_SPLIT];
static int stress_split = 1;
static int stress_count = SCENE_NUM_WALLS;

static void stress_rebuild(void)
{
    stress_count = 0;
    for (int w = 0; w < SCENE_NUM_WALLS; w++) {
        for (int k = 0; k < stress_split; k++) {
            const float f0 = (float)k / stress_split;
            const float f1 = (float)(k + 1) / stress_split;
            r_wall_t *s = &stress_walls[stress_count++];
            *s = walls[w];
            s->x1 = walls[w].x1 + (walls[w].x2 - walls[w].x1) * f0;
            s->y1 = walls[w].y1 + (walls[w].y2 - walls[w].y1) * f0;
            s->x2 = walls[w].x1 + (walls[w].x2 - walls[w].x1) * f1;
            s->y2 = walls[w].y1 + (walls[w].y2 - walls[w].y1) * f1;
        }
    }
}
#endif /* D_TESTROOM */

static void scene_init(void)
{
    /* DragonFS-native paths: the "rom:/" prefix belongs to the stdio wrapper,
     * not to dfs_open, which resolves against the filesystem root directly. */
    dt64_load_tlut("/playpal.tlut");

    cam.x = 0.0f;
    cam.y = 0.0f;
    cam.z = EYE_Z;
    cam.angle = 0.0f;
    /* 90 degree horizontal FOV, as in Doom. focal_y is pinned to the 320-wide
     * frame so WIDE changes the horizontal sampling rate only -- and then
     * scaled by the vertical magnification, so HIRES changes the sampling
     * rate on both axes and the view stays the same shape. Without that
     * factor a 480-line frame would show the same pixel count of world
     * over twice the scanlines: a half-height picture in a letterbox.
     * See the note in r_wall.h. */
    cam.focal_x = SCREEN_W * 0.5f;
    cam.focal_y = SCREEN_BASE_W * 0.5f * (float)UI_YSCALE;

    /* --- real level geometry ------------------------------------------
     * Path B: keep Doom's data and BSP, replace its renderer. This proves the
     * first half -- the WAD streaming off the cartridge and the little-endian
     * decode -- before any of it reaches the RDP. */
    /* The level, loaded entirely by Doom's own p_setup.c.
     *
     * Our loader is gone: Doom's resolves textures through R_TextureNumForName
     * into the same asset table, D_SetSky picks the episode's backdrop, and
     * P_SpawnPlayer places the player from the THINGS lump. Keeping a second
     * copy of the geometry only cost RAM and cache. */
    /* The card first, so a player can drop in their own IWAD and so a ROM
     * built without one still runs; the embedded copy is the fallback.
     * Only root-level names resolve through this driver, and the case that
     * works is not predictable -- the music streamer learned the same
     * lesson -- so the usual spellings are each tried. */
    static const char *const iwad_names[] = {
#if D_DOOM2
        /* This cartridge's textures, sprites and sounds were baked from Doom
         * II, so it wants Doom II's IWAD and no other: loading Doom's here
         * would resolve every name against the wrong art. */
        "DOOM2.WAD", "doom2.wad", "DOOM2.wad",
#else
        "DOOM.WAD", "doom.wad", "DOOM.wad",
        "DOOM1.WAD", "doom1.wad",
#endif
    };

    /* Where to look, in order. The card root first, then a folder beside the
     * ROM, since keeping the set together in one directory is the tidier
     * arrangement and the one a player will reach for.
     *
     * Every spelling of both is tried because neither is predictable through
     * this driver. FAT stores short entries upper-case and needs a
     * long-filename record or the NT case bits to present anything else, and
     * which of those this FatFs build honours decides what resolves -- for
     * the directory as much as for the file. */
    static const char *const iwad_dirs[] = {
        "sd:/", "sd:/Doom/", "sd:/DOOM/", "sd:/doom/",
    };

    bool wad_ok = false;
    for (unsigned d = 0; d < sizeof iwad_dirs / sizeof iwad_dirs[0] && !wad_ok; d++)
        for (unsigned i = 0; i < sizeof iwad_names / sizeof iwad_names[0]; i++) {
            char path[64];
            snprintf(path, sizeof path, "%s%s", iwad_dirs[d], iwad_names[i]);
            if ((wad_ok = wad_open(path))) {
                debugf("wad: using %s\n", path);
                /* Remember WHERE, so saves can go beside the game rather
                 * than at a path compiled in. This is the only thing the
                 * console actually knows about where it was launched from:
                 * there is no "my ROM's directory" to ask for, but the
                 * folder that yielded the IWAD is the same folder in every
                 * arrangement a player is likely to have. */
                { void D_SetDataDir(const char *dir);
                  D_SetDataDir(iwad_dirs[d]); }
                break;
            }
        }

    /* The copy inside the cartridge, when one was built in. */
    if (!wad_ok && (wad_ok = wad_open("/doom.wad")))
        debugf("wad: using the embedded /doom.wad\n");

    if (wad_ok) {
        void D_LoadLevel(int episode, int map);
        void D_PlayerView(float *x, float *y, float *z, float *angle);

        Z_Init();
        W_N64_Init();

        /* Before anything reads a level or draws a menu: Doom's own code
         * branches on gamemode in a dozen places, and every one of them was
         * taking the Doom branch regardless of which IWAD had been opened. */
        { void D_IdentifyGame(void); D_IdentifyGame(); }

        /* Flash palettes from the WAD actually playing, not the one the ROM
         * was built against -- an EXTWAD player can drop in any IWAD, and
         * its PLAYPAL owns the damage reds and pickup golds. Bit-identical
         * to the baked banks when the WADs match. */
        {
            int W_CheckNumForName(const char *name);
            int W_LumpLength(int lump);
            void *W_CacheLumpNum(int lump, int tag);
            const int pp = W_CheckNumForName("PLAYPAL");
            if (pp >= 0)
                dt64_build_tluts(W_CacheLumpNum(pp, 1 /* PU_STATIC */),
                                 W_LumpLength(pp));
        }

        /* And establish player one, which D_CheckNetGame would have done. */
        { void D_SinglePlayerSetup(void); D_SinglePlayerSetup(); }

#if D_SAVETEST
        { void I_SaveSelfTest(void); I_SaveSelfTest(); }
#endif

        D_InitPicTables(1024);
        r_vapor_init();
        r_halo_init();

        /* Menus, status bar and messages BEFORE the first level load: Doom's
         * own P_SpawnPlayer calls ST_Start and HU_Start for the console
         * player, and those index hu_font[] -- fonts that only exist once
         * D_UI_Init has cached them. Initialising the UI after D_LoadLevel
         * was a null deref inside the very first P_SetupLevel. */
        D_UI_Init();

#if R_DEMO || D_LEVELTEST || defined(R_VIEWLOCK)
        /* The measurement harnesses want a level immediately, not a title
         * cycle: the attract AI and the teardown test drive E1M1 directly. */
#ifdef R_BOOT_MAP
        { static const int bm[2] = { R_BOOT_MAP };
          D_LoadLevel(bm[0], bm[1]); }
#else
        D_LoadLevel(1, 1);
#endif
        D_PlayerView(&cam.x, &cam.y, &cam.z, &cam.angle);
        D_UI_LevelStart();
#else
        /* Settings from the shared file beside the ROMs, before the title
         * screen so the resolution the player chose is the one they boot
         * into rather than something that snaps a frame later. m_config.c
         * is not compiled here, so this is the only thing that restores
         * them. */
        { void D_OptionsLoad(void); D_OptionsLoad(); }

        /* Boot into the title screen with the attract demos cycling behind
         * the menu, exactly as chocolate doom's D_DoomMain ends. */
        {
            void D_StartTitle(void);
            D_StartTitle();
        }
#endif
#if D_MENUTEST
        /* Walk the menu the way a player does, as far as MENUTEST says:
         * 1 opens the main menu, 2 goes on to the episode list, 3 carries
         * through skill and actually starts the game. Starting a game is the
         * one path that has to be driven from here, since no demo reaches
         * it -- which is how it went so long with a null player pointer. */
        {
            void M_StartControlPanel(void);
            void D_TestMenuKey(int key);
            M_StartControlPanel();
#if D_MENUTEST >= 2
            D_TestMenuKey(13);          /* ENTER on New Game */
#endif
#if D_MENUTEST >= 3
            D_TestMenuKey(13);          /* ENTER on the first episode */
            D_TestMenuKey(13);          /* ENTER on the default skill */
#endif
#if D_MENUTEST == 7
            /* Back into the menu -- the skill press above closed it -- and
             * down one to Options, to capture the slider rows. */
            M_StartControlPanel();
            D_TestMenuKey(0xaf);        /* KEY_DOWNARROW */
            D_TestMenuKey(13);          /* ENTER on Options */
#endif
#if D_MENUTEST == 4
            /* Back to the menu and into Load, to capture the slot list. */
            M_StartControlPanel();
            D_TestMenuKey(0xaf);        /* KEY_DOWNARROW */
            D_TestMenuKey(0xaf);        /* KEY_DOWNARROW */
            D_TestMenuKey(13);          /* ENTER on Load Game */
#endif
        }
#endif
        level_loaded = true;
    }

#if D_TRIBENCH
    /* Submission micro-benchmark. The scissor is set to a single pixel so
     * the RDP fills nothing and cannot become the bottleneck: what is left
     * is the CPU's per-vertex conversion plus the RSP's setup, which is
     * exactly what vertex-slot reuse changes. Runs once at boot. */
    {
        extern uint32_t r_tri_bench(int iters, int fast);
        surface_t *bfb = display_get();
        rdpq_attach(bfb, display_get_zbuf());
        rdpq_set_mode_standard();
        rdpq_set_scissor(0, 0, 1, 1);

        const int N = 4000;
        r_tri_bench(64, 0);                    /* warm caches */
        const uint32_t ref_us  = r_tri_bench(N, 0);
        const uint32_t fast_us = r_tri_bench(N, 1);
        const uint32_t cmp_us  = r_tri_bench(N, 2);
        debugf("tribench: %d quads  reference=%lu  slotreuse=%lu (%ld%%)  "
               "compact=%lu (%ld%%)\n",
               N, (unsigned long)ref_us,
               (unsigned long)fast_us,
               ref_us ? (long)(100 - (int64_t)fast_us * 100 / ref_us) : 0,
               (unsigned long)cmp_us,
               ref_us ? (long)(100 - (int64_t)cmp_us * 100 / ref_us) : 0);

        rdpq_set_scissor(0, 0, SCREEN_W, SCREEN_H);
        rdpq_detach_show();
        rspq_wait();
    }
#endif

    mem_report("after scene load");

    if (!level_loaded) {
        /* Fallback test room. Loaded only on failure, and only after the
         * level attempt: p_load_level resets the texture arena, so anything
         * loaded before it would be left pointing at freed memory.
         *
         * These three textures are Doom's, and Doom II has none of them, so
         * asserting on them turned "the IWAD was not found" into "missing
         * COMPUTE1" on a Doom II cartridge -- a texture the player has no
         * reason to have heard of, naming a file that is correctly absent.
         * The room is a diagnostic aid, so its own absence must not be fatal:
         * without it the loop below draws nothing and the message stands on
         * its own. */
        debugf("no IWAD found -- put %s beside the ROM or at the card root\n",
               D_IWAD_WANTED);

#if D_TESTROOM
        /* Bring-up aid, off by default. It predates the game running at all,
         * and its textures are Doom's -- a cartridge baked from a WAD without
         * them cannot draw it. */
        assertf(dt64_load(&tex_startan, "/STARTAN3.dt64"), "missing STARTAN3");
        assertf(dt64_load(&tex_brown,   "/BROWN1.dt64"),   "missing BROWN1");
        assertf(dt64_load(&tex_compute, "/COMPUTE1.dt64"), "missing COMPUTE1");
        scene_build(walls, &tex_startan, &tex_compute, &tex_brown);
        stress_rebuild();
#else
        /* Say what is wrong, rather than drawing something.
         *
         * Both failures the player actually saw came from here. Doom drew the
         * test room -- a couple of untextured walls and no game, with nothing
         * to suggest the IWAD was the problem -- and Doom II died on
         * "missing COMPUTE1", naming a texture that is correctly absent from
         * its WAD. Neither pointed at the real fault.
         *
         * There is no gentler way to report it: the message font lives in the
         * IWAD too, so with none opened there is nothing to draw text with.
         * libdragon's failure screen carries its own font, which makes it the
         * only thing on this console that can say this. */
        assertf(0, "No %s found.\n\n"
                   "Put it in the folder beside the ROM,\n"
                   "or at the root of the SD card.",
                D_IWAD_WANTED);
#endif
    }
}

/* Set by attract mode when it wants to try a door; folded into the ticcmd
 * alongside the real controller. */
static bool demo_use_pending;

#if R_DEMO
/* Attract mode: walks the level unattended.
 *
 * A single vantage point says nothing about the frames that actually hurt, and
 * ares has no input bindings to drive from outside, so the route is generated
 * in the ROM instead. It runs identically in the emulator and on hardware,
 * which makes it a repeatable benchmark rather than a demo.
 *
 * The route is deliberately not a list of map coordinates: it walks forward,
 * notices when it has stopped making progress, and turns away. That explores a
 * level it knows nothing about and stays valid when geometry changes. */
/* EVERY piece of walker state below is keyed to leveltime, never to call
 * count. This function runs once per RENDERED FRAME, and the frame rate is
 * exactly what differs between two builds being A/B'd -- the warmup, door
 * timer and turn duration used to be frame-counted, so a faster ROM exited
 * warmup at an earlier tic, turned for less game time, and walked a
 * DIFFERENT ROUTE. That divergence is what made whole-route comparisons
 * (POSEHASH, the abdiff harness) report differences for provably identical
 * renderers: the two ROMs were not looking at the same scenes. Tic-keyed,
 * the route is a pure function of simulation time and identical across
 * builds of any speed. (With interpolation on, the progress probe below
 * reads a sub-tic view position, which can flip a knife-edge stuck test
 * between builds -- measurement runs use INTERP=0, where it is exact.) */
static void demo_drive(float *fwd, float *turn, bool *use)
{
    static float px, py, dir = 1.0f;
    static int   stuck, turn_until = -1, last_check;
    extern int leveltime;

    *use = false;
    if (leveltime < 15) { *fwd = 0.0f; *turn = 0.0f; return; }   /* settle */

    /* Try a door every ~2.7 s: E1M1 is gated by one, and an attract mode
     * that never opens it only ever benchmarks the first room. The whole
     * tic sees *use high; the tic builder samples it once. */
    if (leveltime % 96 == 0) *use = true;

    /* Progress is measured on the actor, not the old player struct: movement
     * goes through Doom's collision now and that struct no longer moves. */
    void D_PlayerView(float *x, float *y, float *z, float *angle);
    float ax = px, ay = py, az, aang;
    D_PlayerView(&ax, &ay, &az, &aang);

    if (leveltime < turn_until) {
        *fwd  = 0.15f;            /* keep creeping so corners unstick */
        *turn = 0.045f * dir;
        return;
    }

    /* Progress judged over game TIME, not frames. Per-frame displacement
     * halves whenever the frame rate doubles, so at a real 60 fps a
     * per-frame test read normal walking as "stuck" on every frame and the
     * route spun into the nearest corner -- on hardware, every time. Eight
     * tics is ~230 ms: a walker covers ~60 units in that; well under 20
     * means something is in the way. */
    if (leveltime - last_check >= 8) {
        const float dx = ax - px, dy = ay - py;
        const float moved2 = dx * dx + dy * dy;
        px = ax; py = ay;
        last_check = leveltime;

        if (moved2 < 20.0f * 20.0f) {
            if (++stuck > 1) {
                stuck      = 0;
                /* 12-27 tics of turning, the duration itself a function of
                 * simulation time only. */
                turn_until = leveltime + 12 + (leveltime & 15);
                dir        = -dir;
            }
        } else {
            stuck = 0;
        }
    }

    *fwd  = 1.0f;
    *turn = 0.0f;
}
#endif

/* Doom's logic runs at a fixed 35 Hz however fast the frame renders.
 *
 * Every speed constant in the game -- how far a monster steps, how fast a door
 * opens, how long a frame of animation lasts -- is expressed per tic, so the
 * rate has to stay fixed. Scaling it to the frame rate would change behaviour
 * the ported code is supposed to reproduce exactly. Accumulating the remainder
 * keeps the average right when the frame time is not a whole number of tics. */
extern int r_drop_bbox, r_drop_span, r_drop_cover;
extern int r_drop_npts, r_drop_depth, r_drop_side, r_drop_pool, r_drop_clipofl;

int D_PlayerHealth(void);
int D_PlayerAmmo(void);

static surface_t *prev_fb;      /* the frame a melt would start from */

/* Doom's boolean is an enum of int size; main.c stays free of doom headers. */
typedef int boolean_stub_t;

/* Per-phase CPU accumulators, reported with the periodic telemetry. The
 * frame is CPU-bound in the submit path on hardware, so knowing the split
 * between the BSP walk and each flush is what decides where to optimise. */
#if D_HWSTAT
static uint32_t ph_walk_us, ph_wallflush_us, ph_flatflush_us, ph_sprite_us;
/* The buffer-acquire block, outside frame_total_us by construction. */
static uint32_t ph_acq_us;
/* The loop TAIL, which is 42% of the frame and had never been profiled:
 * everything after the submit stamp. sim is Doom's own simulation, audio is
 * the mixer and the music stream; what is left over is the UI, the melt and
 * the interpolation bookkeeping. */
static uint32_t ph_sim_us, ph_audio_us;
static uint32_t ph_snd_us, ph_mus_us;
/* The passes nobody ever timed. They turned out to be the largest single
 * block on a nukage-heavy map -- see the profiling notes -- which is exactly
 * what an unmeasured phase is for. refl is the liquid mirrors; fx is the
 * shafts, sky, halos and vapor together. */
static uint32_t ph_refl_us, ph_fx_us;
#endif

/* Sub-tic phase for frame interpolation: how far into the next tic the
 * moment of rendering falls. carry_us is that phase at the instant the tic
 * loop last ran; the render adds the wall-clock time elapsed since. Both
 * come from the same accumulator that schedules the tics themselves, so the
 * phase can never disagree with the (previous, current) state pair. */
static uint32_t tic_carry_us;
static uint32_t tic_stamp;

static void run_game_tics(uint32_t frame_us)
{
    static uint32_t carry_us;
    carry_us += frame_us;

    /* 1000000/35 = 28571 us per tic. Capped so a long frame -- a level load,
     * a stall -- cannot spiral into a burst of catch-up ticks. */
    void D_TicStart(void);
    void M_Ticker(void);
    void G_Ticker(void);
    void D_DoAdvanceDemo(void);
    extern boolean_stub_t advancedemo;

#if D_FREEZE
    /* A/B harness: stop the world after a fixed number of tics so two
     * builds settle into identical state and any pixel difference is the
     * renderer's doing rather than animation phase. */
    {
        extern int gametic;
        if (gametic >= D_FREEZE) { carry_us = 0; tic_stamp = TICKS_READ(); return; }
    }
#endif
#if D_EXITAT
    /* Reach the INTERMISSION unattended: finish the level at a fixed tic,
     * exactly as touching the exit switch would. The intermission loads
     * its background and fonts into the same texture arena the level's
     * own art is still holding -- which is a pure memory-budget question
     * and therefore reproducible here, not a hardware one. Fires once. */
    {
        extern int gametic;
        static int exited;
        void G_ExitLevel(void);
        int  D_InLevel(void);
        if (!exited && gametic >= D_EXITAT && D_InLevel()) {
            exited = 1;
            G_ExitLevel();
        }
    }
#endif
    int guard = 0;
    while (carry_us >= 28571u && guard < 4) {
        carry_us -= 28571u;
        guard++;
#if R_DEMO
        /* The walker is sampled once PER TIC, here, not only once per
         * rendered frame: a frame covering several tics used to feed one
         * frame-sampled input to all of them, so where the tic boundaries
         * fell -- a pure function of build speed -- steered the route, and
         * two builds of different speed still walked apart even after the
         * walker's own state went tic-keyed. Evaluated at the leveltime of
         * the tic about to run, the route is a function of simulation state
         * alone. demo_drive's state transitions all key on leveltime, so
         * the per-frame evaluation in handle_input (kept for the pad and
         * menu paths) re-deriving the same values is idempotent. */
        if (level_loaded) {
            void D_PlayerInput(float fwd, float strafe, float turn, int run,
                               int attack, int use);
            float dfwd = 0.0f, dturn = 0.0f;
            bool  duse = false;
            demo_drive(&dfwd, &dturn, &duse);
            D_PlayerInput(dfwd, 0.0f, dturn, 0, 0, duse);
        }
#endif
        /* Chocolate's RunTic, in order: a pending page/demo advance first,
         * then the local command is published for this tic, the menu ticks,
         * and G_Ticker runs the gameaction machine plus whichever gamestate
         * ticker applies (P_Ticker inside GS_LEVEL). */
        if (advancedemo)
            D_DoAdvanceDemo();
        /* Interpolation guard (d_bridge.c): only a P_Ticker that actually
         * runs moves leveltime past this stamp, so a menu- or pause-held
         * world renders its current state instead of lerping a frozen
         * pair with a still-cycling fraction. */
        {
            extern int leveltime, oldleveltime;
            oldleveltime = leveltime;
        }
        D_TicStart();
        M_Ticker();
        G_Ticker();
        /* d_loop.c owns this in chocolate; without it the status bar's
         * gametic guard never opens and every gametic-paced animation --
         * menu cursor, doomguy's face -- freezes at zero. */
        {
            extern int gametic;
            gametic++;
        }
        {
            void D_SoundTic(void);
            void p_level_anim_tick(void);
            D_SoundTic();
            p_level_anim_tick();
            r_wipe_tick();
        }
    }
    if (guard == 4) carry_us = 0;

    tic_carry_us = carry_us;
    tic_stamp    = TICKS_READ();
}

static void handle_input(void)
{
    joypad_poll();
    joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);

    /* Free-camera speed for the no-level fallback scene only; the game
     * path expresses movement in Doom's own per-tic units below. */
    const float move_speed = 5.0f;

    /* Stick: forward/back on Y, rotate on X -- Doom's native scheme.
     * C-left/C-right sidestep, R held is run, Z or A fires, B uses, L is
     * the automap. The d-pad doubles the stick for pad-only play.
     *
     * Turn rate is anchored to the PC's: vanilla's angleturn is 640 units
     * per tic walking and 1280 running, and D_PlayerInput converts radians
     * at 10430 units per radian -- so full deflection maps to 640/10430,
     * not to a made-up feel constant. The first cut here was ten times
     * vanilla and unplayable. */
    /* A runs. Either trigger toggles the automap. */
    const int run = in.btn.a;

    /* Scaled by the CONTROLLER slider in the options menu: 0.5x at 0 up to
     * 1.4x at 9, in tenths. The default 3 is 0.8 -- deliberately BELOW the
     * rate this shipped with, which came from vanilla's 640 angleturn units
     * a tic. That number suits a keyboard, where turning is instant on and
     * instant off; on a stick held at partial deflection it overshoots.
     * The deadzone below is a fraction of TURN_FULL, so it scales with the
     * setting instead of eating a fixed slice of a slower turn. */
    extern int joySensitivity;
    const float sens = 0.5f + (float)joySensitivity * 0.1f;
    const float TURN_FULL =
        (640.0f / 10430.0f) * (run ? 2.0f : 1.0f) * sens;

    float fwd  = in.stick_y / 80.0f;
    float turn = -in.stick_x / 80.0f * TURN_FULL;
    if (fabsf(fwd)  < 0.08f) fwd = 0.0f;        /* stick deadzone */
    if (fabsf(turn) < TURN_FULL * 0.08f) turn = 0.0f;

    if (in.btn.d_up)    fwd =  1.0f;
    if (in.btn.d_down)  fwd = -1.0f;
    if (in.btn.d_left)  turn += TURN_FULL;
    if (in.btn.d_right) turn -= TURN_FULL;

    float strafe = 0.0f;
    if (in.btn.c_left)  strafe = -1.0f;
    if (in.btn.c_right) strafe =  1.0f;

#if R_DEMO
    if (level_loaded) {
        bool demo_use = false;
        demo_drive(&fwd, &turn, &demo_use);
        strafe = 0.0f;
        /* Using doors is BT_USE in the ticcmd now. */
        demo_use_pending = demo_use;
    }
#endif

    if (level_loaded) {
        /* Movement goes through Doom's own collision now: input becomes thrust
         * on the player's actor, and P_XYMovement carries it out on the next
         * tic via P_TryMove and P_SlideMove -- the same path every monster
         * takes. p_move.c's parallel implementation is gone with it. */
        void D_PlayerInput(float fwd, float strafe, float turn, int run,
                           int attack, int use);
        void D_PlayerView(float *x, float *y, float *z, float *angle);
        /* The menu gets the buttons before the game does, and swallows
         * movement while it is open -- otherwise the player keeps walking
         * underneath it. */
        if (D_UI_HandleInput(in.btn)) {
            fwd = strafe = turn = 0.0f;
            D_PlayerInput(0.0f, 0.0f, 0.0f, 0, 0, 0);
        } else {
            /* Z fires, B opens doors and works switches, A runs. */
            D_PlayerInput(fwd, strafe, turn, run,
                          in.btn.z,
                          in.btn.b || demo_use_pending);
        }
        demo_use_pending = false;
        D_PlayerView(&cam.x, &cam.y, &cam.z, &cam.angle);
    } else {
        cam.angle += turn;
        const float cs = cosf(cam.angle), sn = sinf(cam.angle);
        cam.x += (cs * fwd + sn * strafe) * move_speed;
        cam.y += (sn * fwd - cs * strafe) * move_speed;
    }
}

/* Screen-space overlay pass, all drawn through the RDP inside one mode
 * bracket. Declared here rather than through headers for the usual reason:
 * d_ui.c speaks Doom's types. */
void V_BeginUI(void);
void V_EndUI(void);
void D_UI_DrawText(int x, int y, const char *s);

/* Previous frame's attach-to-RDP-idle time, stamped from the SYNC_FULL
 * interrupt. This is the pipelined replacement for the number the old
 * blocking rdpq_detach_wait produced -- same span, measured without making
 * the CPU sit through the RDP's tail. */
static volatile uint32_t frame_total_us;
#if D_HWSTAT
static uint32_t          submit_us;    /* CPU submit span; HWSTAT telemetry only */
#endif

static void frame_done_cb(void *arg)
{
    const uint32_t t_start = (uint32_t)(uintptr_t)arg;
    frame_total_us = TICKS_TO_US(TICKS_DISTANCE(t_start, TICKS_READ()));
}

int main(void)
{
    /* Wide mode costs one extra framebuffer's worth of RDRAM per buffer:
     * 640x240x2 is 300 KB against 150, so three buffers plus the z-buffer go
     * from ~600 KB to ~1.2 MB. That is affordable on the 8 MB this build
     * already requires, and keeping three buffers preserves the frame
     * pipelining that rdpq_detach_show gives us. */
    /* FILTERS_RESAMPLE only at 320. The VI always scales the framebuffer to a
     * 640-wide virtual output, so at 320 the resample filter is what smooths
     * that 2x upscale -- worth having. At 640 there is no horizontal resize
     * left to do, and running a bilinear kernel over 1:1 content only smears
     * it: the extra columns we just paid fill rate for get averaged away.
     * Disabled there so wide mode actually shows the resolution. */
    /* The USB channel BEFORE the display, not after it.
     *
     * display_init asserts on its own arguments and allocates the largest
     * single block the program ever asks for, and it used to run fifty
     * lines ahead of the first debug_init_usblog -- so any failure in it
     * printed to a channel that did not exist yet and the console simply
     * went quiet. That is exactly what a first 640x480 build did: the ROM
     * transferred, the cart ran it, and not one byte came back to say
     * why. Opening the channel first costs nothing when nothing is
     * listening and turns a silent hang into a message. */
    debug_init_usblog();

    /* Two buffers at 480, three below it. A 320x480 buffer is 480 KB
     * against 150 KB at 320x240, and the heap has about 1730 KB free
     * after the arenas: two buffers plus the z buffer take 1440 KB and
     * leave room for the melt's saved frame, three would take 1920 KB and
     * not fit at all. The cost is one frame of the pipelining the comment
     * above describes -- display_get at the top of the loop can now block
     * on the buffer the VI is still scanning out, so a frame that
     * finishes early waits instead of running ahead. At 480 the frame is
     * not the thing finishing early. */
    /* Allocated at the LARGEST mode the detail menu can reach, whatever
     * mode the game boots in, because this is the one moment the heap is
     * pristine: nothing else has been allocated yet, so the three big
     * blocks are certain to be contiguous. Every later switch reuses them
     * through display_change and cannot fail.
     *
     * 320x480 covers all three modes -- 640x240 is 307 KB a buffer and
     * 320x240 is 154 KB, both inside 320x480's 480 KB -- so the reservation
     * is the price of the toggle existing at all, not of using it. Two
     * buffers rather than three: three at this size do not fit beside the
     * arenas, and display_change can drop to any smaller count but never
     * exceed what was allocated here. */
    /* PROBED, not assumed. display_init asserts "Out of memory" rather than
     * returning a failure, so asking for buffers that will not fit takes
     * the console down before the debug channel has said anything useful.
     * Three blocks of the largest mode's size is exactly what display_init
     * is about to ask malloc for; if they come back, they will come back
     * for it too. */
    {
        const size_t fb = (size_t)320 * 480 * 2;   /* 300 KB a buffer */
        void *p0 = malloc(fb), *p1 = malloc(fb),
             *p2 = malloc(fb), *p3 = malloc(fb);   /* three plus the z */
        hires_ok = p0 && p1 && p2 && p3;
        free(p0); free(p1); free(p2); free(p3);
        debugf("screen: %lu KB free, hires-capable=%d\n",
               (unsigned long)(get_memory_size() / 1024), hires_ok);
    }

    display_init(hires_ok ? RES_320x480 : RESOLUTION_320x240,
                 DEPTH_16_BPP, 3, GAMMA_NONE, vi_filter_for(SCREEN_W));

    /* The allocation above is always 320x480; the BOOT mode may not be.
     * Drop to it now that the memory is claimed -- this reconfigures the VI
     * and allocates nothing, exactly as the detail menu's later switches
     * do. */
    if (!hires_ok || SCREEN_BOOT_H != 480)
        display_change(SCREEN_BOOT_W == 640 ? RESOLUTION_640x240
                                            : RESOLUTION_320x240,
                       DEPTH_16_BPP, 3, GAMMA_NONE,
                       vi_filter_for(SCREEN_BOOT_W));

    /* Audio before the arenas claim the heap. audio_init reserves DMA buffers
     * the AI reads directly; starting it with the heap nearly full does not
     * fail loudly, it corrupts the display. */
    /* Open the USB debug channel if a flashcart with a cable is present.
     * debugf output then reaches the host, which is worth far more than the
     * few words that fit on screen. Harmless when nothing is listening. */
    debug_init_usblog();

#if D_SOUND
    I_Sound_Init();

    /* Music lives on the flashcart's SD card, not in the ROM -- the set is
     * about a gigabyte of 48 kHz stereo. Absent on a bare cartridge, which is
     * not an error. */
    mus_init("sd:/DOOMMUS.wad");
#endif

    joypad_init();
    rdpq_init();
    dfs_init(DFS_DEFAULT_LOCATION);

    /* Debug channel first: mem_init reports through it, and its Expansion Pak
     * assertion is the one message most worth not losing. */
    debug_init_isviewer();
    debug_init_usblog();

    /* Reserve the arenas after display/RDP init, so their allocations are
     * already out of the system heap and the report reflects reality. */
    mem_init();

#if DEBUG_RDP
    /* Validates every RDP command and reports illegal state combinations,
     * which is otherwise invisible: bad state renders as a blank screen. */
    rdpq_debug_start();
#endif
#if D_SEAMPROBE
    /* Starts the same trace engine DEBUG_RDP does -- a second start just
     * re-clears its state before the hook lands -- and hangs the command
     * hook on it. See d_seam.h. */
    d_seam_init();
#endif

    scene_init();

#if D_RSPQPROF
    /* Started here, not at rdpq_init, for two reasons. The profiler only
     * accumulates slots it has a name for, and it collects those names when it
     * starts -- from the overlays registered by then, which means every
     * subsystem's ucode has to be up first (rdpq's, and the mixer's from
     * I_Sound_Init) or its time silently vanishes from the totals and reads
     * back as RSP idle: precisely the number being measured. And starting
     * after scene_init keeps the level load out of the first window. */
    rspq_profile_start();
#endif

#if D_RSPIDLE
    /* Same reasoning about placement: start sampling once the level is up, so
     * the first window measures the game rather than the load. */
    rspidle_init();
#endif

#if DEBUG_RDP
    /* Dump the first frame's worth of wall tiles, then go quiet. */
    int r_debug_dump_init = 16;
    extern int r_debug_dump;
    r_debug_dump = r_debug_dump_init;
    uint32_t frame = 0;
#endif

    while (1) {
        /* The level's art dies with the level. Release it the moment the
         * game leaves GS_LEVEL -- for the intermission, the finale, or the
         * attract screen -- instead of holding it until the NEXT level
         * loads, which is when p_load_level used to do it.
         *
         * That delay was a real bug: the intermission loads its own
         * background and fonts while the finished level's working set is
         * still resident, so on a heavy map it was bidding for the last
         * hundred kilobytes of the arena. When it lost, dt64_load failed,
         * V_DrawPatch drew nothing, and the screen came up black with only
         * the stats on it -- silently, because a UI miss prints nothing.
         * Reproduced with TEXARENA=1630 on E2M2 ("ui miss: /u_WIMAP1.dt64",
         * "arena exhausted (need 62 KB, 29 KB free)"), which is the same
         * squeeze a 1914 KB peak leaves on the console's 2 MB.
         *
         * p_level_reset_assets is the function p_load_level already used;
         * it drops the arena and every cache that holds resolved pointers
         * into it. Nothing between here and the next level renders the
         * world, and the wipe melts a saved framebuffer, not textures. */
        {
            int  D_InLevel(void);
            void p_level_reset_assets(void);
            static int was_in_level;
            const int in_level = D_InLevel();
            if (was_in_level && !in_level) p_level_reset_assets();
            was_in_level = in_level;
        }
        {
            void D_SetTicFrac(uint32_t into_tic_us);
            void D_InterpBegin(void);
            int  D_InLevel(void);
            if (D_InLevel()) {
                D_SetTicFrac(tic_carry_us + TICKS_TO_US(TICKS_SINCE(tic_stamp)));
                D_InterpBegin();
            }
        }

        handle_input();

#if D_RUMBLE
        /* Decay and dither the rumble level into the pak, once per frame.
         * Output-only: no pixels, no sim state, no demo risk. */
        { void D_RumbleFrame(void); D_RumbleFrame(); }
#endif

#ifdef R_VIEWLOCK
        {   /* Reproduction rig: pin the camera to a pose reported by the
             * hwstat telemetry (build with VIEWLOCK=x,y,z,milliangle), so a
             * rendering fault seen on a television can be recreated
             * pixel-for-pixel in the emulator. The game keeps running
             * underneath; only the renderer's viewpoint is frozen. */
            static const float vl[4] = { R_VIEWLOCK };
            cam.x = vl[0]; cam.y = vl[1]; cam.z = vl[2];
            cam.angle = vl[3] / 1000.0f;
        }
#endif

        /* Before display_get, which is the only point in the loop where no
         * frame is in flight -- see the note on D_RequestScreen. */
        screen_apply();

        /* How long the loop BLOCKS here, which is the one cost the frame
         * counters could not see: t_start is taken after this call, so the
         * wait for a free framebuffer sits outside frame_total_us entirely.
         *
         * This is the throttle. The tail is pipelined -- rdpq_sync_full plus
         * detach_show return while the RDP is still rasterising -- so
         * display_get is where the CPU finally has to wait for the RDP to
         * release a buffer. The note at that tail says "three framebuffers
         * deep", and that is true at 240p; at 320x480 the heap only affords
         * TWO (a 480 KB buffer against ~1730 KB free after the arenas), and
         * double buffering is exactly the arrangement that stops the CPU and
         * RDP overlapping. Whether that is where the frame's unexplained
         * time goes is the question this number exists to answer. */
#if D_HWSTAT
        const uint32_t t_acq = TICKS_READ();
#endif
        surface_t *fb = display_get();
#if D_HWSTAT
        ph_acq_us += TICKS_TO_US(TICKS_SINCE(t_acq));
#endif

        /* A gamestate change means the frame just shown is the last of the
         * old screen: melt it away over what follows. Doom does this in
         * D_Display through wipegamestate; the state is compared here
         * because this loop, not D_Display, owns the frame. */
        {
            int D_FlowState(int *ga);
            static int last_state = -1;
            static bool have_prev;
            int ga;
            const int gs = D_FlowState(&ga);
            if (last_state >= 0 && gs != last_state && have_prev &&
                !r_wipe_active() && scr_settle == 0)
                r_wipe_start(prev_fb);
            if (scr_settle > 0) scr_settle--;
            last_state = gs;
            have_prev = prev_fb != NULL;
        }

        /* Wall-clock delta between frames, for the game tics. Feeding the
         * measured render time (as this used to) undercounts real time by
         * every vsync wait, and once the frame tail is pipelined the CPU no
         * longer observes the RDP finish at all. The wall clock includes
         * both, so the world runs at Doom's 35 Hz whatever the renderer and
         * the VI are doing. */
        static uint32_t prev_ticks;
        const uint32_t now_ticks = TICKS_READ();
        const uint32_t frame_delta_us =
            prev_ticks ? TICKS_TO_US(TICKS_DISTANCE(prev_ticks, now_ticks))
                       : 16667;
        prev_ticks = now_ticks;

        const uint32_t t_start = TICKS_READ();
#if D_RSPQPROF
        /* Frame boundary for the profiler's per-frame averages. Before the
         * attach, so a frame's samples cover the whole of its RDP work. */
        rspq_profile_next_frame();
#endif
        /* Attach WITHOUT clearing: what needs clearing depends on what the
         * BSP walk finds (see below), and the walk is pure CPU work that
         * queues geometry without touching the RDP. */
        /* Status-bar composite maintenance, at the no-attach point: the
         * capture costs ~40 recorded entries; the replay into the offscreen
         * strip happens only on frames the bar (or palette bank) actually
         * changed. */
        { void D_UI_UpdateBar(void); if (level_loaded) D_UI_UpdateBar(); }

        rdpq_attach(fb, display_get_zbuf());

        /* Full clear, unconditionally -- colour and depth.
         *
         * A cleverer version cleared colour only outside the sky span, on
         * the argument that world plus sky cover everything inside it. That
         * argument holds for a perfect frame, but it turns any single-frame
         * coverage slip into a patch of the PREVIOUS frame -- which, with
         * the camera moving, smears as ghost geometry "opening up" at the
         * screen edges, and which a static-camera emulator capture can
         * never reproduce (the previous frame looks identical). The clear
         * costs ~0.3 ms of RDP time that runs concurrently with the CPU's
         * BSP walk in the pipelined frame; buying back an entire class of
         * motion artifacts with it is the easy trade. The sky pass still
         * draws only its clamped span. */
#if D_SEAMPROBE && !defined(D_CLEARCOL)
        /* The seam probe needs a clear no game art wears, and needs to
         * know it exactly to scan for it. CLEARCOL still overrides. */
        #define D_CLEARCOL 255, 0, 255
#endif
#ifdef D_CLEARCOL
        /* Gap probe: any pixel no primitive covered keeps this colour, so
         * two builds with different values differ exactly on the holes. */
        /* Through a variadic indirection: argument splitting happens
         * before expansion, so RGBA32(D_CLEARCOL, 255) would see two
         * arguments, not four. */
        #define D_CLEAR_(...) RGBA32(__VA_ARGS__)
        rdpq_clear(D_CLEAR_(D_CLEARCOL, 255));
#else
        rdpq_clear(RGBA32(0, 0, 0, 255));
#endif
        rdpq_clear_z(0xFFFC);

#if R_BARSCISSOR
        /* Do not rasterise the world under the status bar.
         *
         * The bar is opaque and reaches the bottom edge exactly, so every
         * world pixel at or below V_BarBandTop() is overpainted before the
         * frame is shown -- and those are the NEAREST rows on screen, where
         * the floor fans are largest and the z buffer is busiest. At 480 rows
         * the band is 320x64 = 20,480 pixels of colour write, z read and z
         * write, per level frame, thrown away after the fact.
         *
         * The CLEARS stay full-screen deliberately. They are cheap in FILL
         * mode, they cost nothing extra for the band, and they guarantee no
         * uninitialised texel survives anywhere -- including the frames where
         * the predicate below is false and the world reaches the bottom.
         *
         * Restored before D_AutomapDraw: the automap and the UI bracket both
         * draw into the band and must not be clipped. */
        int D_BarBandDrawn(void);
        int V_BarBandTop(void);
        const bool world_scissored = D_BarBandDrawn() != 0;
        if (world_scissored)
            rdpq_set_scissor(0, 0, SCREEN_W, V_BarBandTop());
#endif

        r_flat_begin();
        r_sprite_begin();
        r_vapor_begin();
        r_env_begin();
        r_halo_begin();
#if D_SEAMPROBE
        /* The pose at the frame the scan will report, so a sighting can be
         * pinned with VIEWLOCK exactly. The periodic hwstat pose is sampled
         * every 120 frames and lands nowhere near a hole. */
        { extern int r_seam_pose[4];
          r_seam_pose[0] = (int)cam.x; r_seam_pose[1] = (int)cam.y;
          r_seam_pose[2] = (int)cam.z; r_seam_pose[3] = (int)(cam.angle * 1000.0f); }
#endif
        r_setup_walls();
        r_sky_span_reset();
#if D_DYNLIGHT
        /* Before the walk: the walk emits the geometry that queries the list,
         * so a light collected after it would not be seen until the next
         * frame -- which on a two-tic muzzle flash is most of its life. */
        { void D_LightsUpdate(void); D_LightsUpdate(); }
#endif
        {
            int D_InLevel(void);
            int D_AutomapActive(void);
            if (level_loaded && D_InLevel() && !D_AutomapActive()) {
#if D_HWSTAT
                { const uint32_t t_ = TICKS_READ();
                  r_render_level(&cam);
                  ph_walk_us += TICKS_TO_US(TICKS_SINCE(t_)); }
#else
                r_render_level(&cam);
#endif
#if D_TESTROOM
            } else if (!level_loaded) {
                for (int i = 0; i < stress_count; i++)
                    r_draw_wall(&cam, &stress_walls[i]);
#endif
            }
            /* Full-screen states (title page, intermission, finale) draw
             * nothing here: the frame is the cleared background plus the
             * COPY-mode UI bracket below. */
        }

#if DEBUG_RDP
        /* Control marker: if this square is missing too, the fault is in the
         * display/attach path rather than in wall rendering. */
        rdpq_set_mode_fill(RGBA32(255, 0, 0, 255));
        rdpq_fill_rectangle(8, 8, 40, 40);
#endif

        dt64_bind_tlut();

#if D_HWSTAT
        { const uint32_t t_ = TICKS_READ();
          r_flush_walls();
          ph_wallflush_us += TICKS_TO_US(TICKS_SINCE(t_)); }
        { const uint32_t t_ = TICKS_READ();
          r_flat_flush(&cam);
          ph_flatflush_us += TICKS_TO_US(TICKS_SINCE(t_)); }
#else
        r_flush_walls();
        r_flat_flush(&cam);
#endif

        /* Sprites last: alpha- and depth-tested, so they need the world
         * already in the depth buffer to be occluded correctly. They are
         * queued by the BSP walk, not scanned here. */
        { int D_InLevel(void); int D_AutomapActive(void);
          if (level_loaded && D_InLevel() && !D_AutomapActive()) {
#if D_HWSTAT
              const uint32_t t_ = TICKS_READ();
              r_sprite_flush();
              ph_sprite_us += TICKS_TO_US(TICKS_SINCE(t_));
#else
              r_sprite_flush();
#endif
#if R_REFLECT
              /* Mirror images of things over glowing pools: after the
               * sprites (nearer actors occlude them via z), before sky
               * and vapor (spill repaint, haze over the mirror). */
#if D_HWSTAT
              { const uint32_t t_ = TICKS_READ();
                r_reflect_flush(&cam);
                ph_refl_us += TICKS_TO_US(TICKS_SINCE(t_)); }
#else
              r_reflect_flush(&cam);
#endif
#endif
          } }

        /* Polished metal, after the walls are in the depth buffer and
         * before the backdrop: the sheen sits on the leaf, not on the sky. */
        { int D_InLevel(void); int D_AutomapActive(void);
          if (level_loaded && D_InLevel() && !D_AutomapActive())
              r_env_flush(&cam); }

        /* Backdrop last, depth-tested: it fills only the pixels the world
         * left untouched, and only in the columns the walk saw sky. */
        { int D_InLevel(void); int D_AutomapActive(void);
#if D_HWSTAT
          const uint32_t t_fx = TICKS_READ();
#endif
          /* Beams before the backdrop: the sky covers the part of a
           * shaft that is behind it, which is what keeps the beam out
           * of the sky instead of smeared across it. */
          if (level_loaded && D_InLevel() && !D_AutomapActive())
              r_shaft_flush(&cam);
          if (level_loaded && D_InLevel() && !D_AutomapActive()) r_sky_draw(&cam);
          /* Vapor last among the world passes: translucency needs every
           * opaque pixel behind it -- walls, flats, sprites, sky --
           * already resolved, and its z-probe does the hiding. */
          /* Light in the air: shafts and halos before the vapor, which
           * is thicker and belongs in front of them. */
          if (level_loaded && D_InLevel() && !D_AutomapActive())
              r_halo_flush(&cam);
          if (level_loaded && D_InLevel() && !D_AutomapActive())
              r_vapor_flush(&cam);
#if D_HWSTAT
          ph_fx_us += TICKS_TO_US(TICKS_SINCE(t_fx));
#endif
        }

#if R_BARSCISSOR
        /* Full screen again for everything that draws INTO the band: the
         * automap, the status bar itself, messages, the menu and the melt. */
        if (world_scissored) rdpq_set_scissor(0, 0, SCREEN_W, SCREEN_H);
#endif

        /* The automap replaces the view: filled-triangle line quads in their
         * own mode block, before the COPY-mode UI bracket below. */
        { void D_AutomapDraw(void); D_AutomapDraw(); }

        /* Screen-space overlays share one COPY-mode bracket: the weapon,
         * the status bar and messages, the menu, and the instrumentation
         * line -- drawn through the RDP like everything else, which is what
         * frees this loop from waiting for the RDP before writing text. */
        if (level_loaded) {
            /* Integer formatting throughout: %f drags in newlib's float
             * printf, ~31 KB of ROM for one HUD line. f is the previous
             * frame's start-to-RDP-idle time (same span the old blocking
             * measurement covered); c is this frame's CPU submit time. */
#if D_HWSTAT
            /* The pose tail makes the console self-reporting: standing at
             * a rendering fault and reading x,y,angle off the screen is what
             * VIEWLOCK needs to reproduce it in the emulator. */
            char hud[80];
            /* Build tag first, so it survives a long pose tail and is the
             * first thing legible in a photograph of the screen. */
            const int hud_n =
            snprintf(hud, sizeof hud, D_TAG "%sf%2u.%01u c%2u.%01u %s %d,%d,%d",
                     D_TAG[0] ? " " : "",
                     (unsigned)(frame_total_us / 1000),
                     (unsigned)((frame_total_us % 1000) / 100),
                     (unsigned)(submit_us / 1000),
                     (unsigned)((submit_us % 1000) / 100),
                     mus_status(),
                     (int)cam.x, (int)cam.y,
                     (int)(cam.angle * 1000.0f));

#if D_RSPIDLE
            /* r<rsp idle>/<rdp busy>, appended rather than given its own row:
             * one line of COPY-mode text is already drawn, and these only
             * change once every 120 frames. Same self-reporting idea as the
             * pose -- the console can be read without a host attached. */
            if (hud_n > 0 && (size_t)hud_n < sizeof hud) {
                unsigned rsp_idle, rdp_busy;
                rspidle_last(&rsp_idle, &rdp_busy);
                snprintf(hud + hud_n, sizeof hud - (size_t)hud_n,
                         " r%u/%u", rsp_idle, rdp_busy);
            }
#elif R_REFLECT
            /* Reflection + rumble self-report, same idea: R<regions
             * queued>/<ghost triangles drawn> and P<pak detected>. Standing
             * at a pool that shows R3/0 means ghosts were culled; R0/0
             * means the floor never registered; P0 means the controller
             * never presented a Rumble Pak, and no software can fix that. */
            if (hud_n > 0 && (size_t)hud_n < sizeof hud) {
                extern int r_refl_dbg_regions, r_refl_dbg_tris;
                snprintf(hud + hud_n, sizeof hud - (size_t)hud_n,
                         " R%d/%d P%d/%d", r_refl_dbg_regions, r_refl_dbg_tris,
                         joypad_get_rumble_supported(JOYPAD_PORT_1) ? 1 : 0,
                         (int)joypad_get_accessory_type(JOYPAD_PORT_1));
            }
#else
            (void)hud_n;
#endif
#endif

            V_BeginUI();
            {
                int D_InLevel(void);
                int D_AutomapActive(void);
                if (D_InLevel() && !D_AutomapActive()) r_psprite_draw();
            }
            D_UI_Draw();
#if D_HWSTAT
            /* Stamp the submit span BEFORE drawing the instrumentation
             * line: those ~40 glyph blits are measurement apparatus, and
             * letting them inside the span inflated the c-number every
             * finding gets priced against by ~1.5%. The debug text is the
             * only thing this excludes; game UI above is still counted. */
            submit_us = TICKS_TO_US(TICKS_SINCE(t_start));
            D_UI_DrawText(8, 8, hud);
#endif
            /* Whatever the card's root holds, when music could not open. */
            for (int i = 0; i < mus_listing_count; i++)
                D_UI_DrawText(8, 20 + i * 10, mus_listing[i]);
            V_EndUI();
        }

#if D_MENUTEST == 6
        /* Open the automap once the level is up, to capture it. */
        {
            void D_TestMenuKey(int key);
            static int countdown = 900;   /* let the walker cover ground */
            if (countdown > 0 && --countdown == 0) {
                debugf("menutest: opening the automap\n");
                D_TestMenuKey(9);       /* KEY_TAB */
            }
        }
#endif
#if D_MENUTEST == 5
        /* Drive a save the way a player does: menu, Save Game, first slot,
         * confirm the default name. It has to wait for the level --
         * M_SaveGame refuses unless usergame is set and a level is up, which
         * is why doing this at boot silently did nothing. */
        {
            void M_StartControlPanel(void);
            void D_TestMenuKey(int key);
            static int countdown = 150;
            if (countdown > 0 && --countdown == 0) {
                debugf("menutest: driving a save\n");
                M_StartControlPanel();
                D_TestMenuKey(0xaf); D_TestMenuKey(0xaf); D_TestMenuKey(0xaf);
                D_TestMenuKey(13);      /* Save Game */
                D_TestMenuKey(13);      /* slot 0 */
                D_TestMenuKey(13);      /* confirm the default name */
            }
        }
#endif

        if (r_wipe_active()) r_wipe_draw();

        /* submit_us is stamped inside the HWSTAT bracket above, before the
         * instrumentation text -- not here, where it would time the ruler. */

#if DEBUG_RDP || D_POSEHASH || D_SEAMPROBE
        /* Blocking in the validator build: the debug workflow reads the
         * CPU/RDP split and wants deterministic frame boundaries.
         *
         * POSEHASH needs it for a sharper reason. The pipelined tail below
         * returns while the RDP is still rasterising, so a fingerprint taken
         * there covers a half-drawn frame -- and how much of it is drawn
         * depends on how fast that particular ROM runs. Two builds then
         * disagree on almost every frame for reasons that have nothing to do
         * with what is being tested, which is exactly the false regression
         * this harness exists to rule out.
         *
         * SEAMPROBE scans the buffer for clear-coloured pixels, so the
         * frame must be finished, and its dump frames must not interleave
         * with the next frame's commands. */
        rdpq_detach_wait();
        frame_total_us = TICKS_TO_US(TICKS_SINCE(t_start));
        display_show(fb);
#if D_SEAMPROBE
        d_seam_frame(fb, color_to_packed16(D_CLEAR_(D_CLEARCOL, 255)));
#endif
#else
        /* The pipelined tail. sync_full's callback stamps when the RDP went
         * idle -- the same span the old rdpq_detach_wait measured -- and
         * detach_show flips the buffer from that interrupt. The CPU carries
         * straight on into game logic and audio below while the RDP is still
         * rasterising this frame; display_get at the top of the next
         * iteration is the only throttle, three framebuffers deep. */
        rdpq_sync_full(frame_done_cb, (void *)(uintptr_t)t_start);
        rdpq_detach_show();
#endif
#if D_POSEHASH
        /* After the submit, while fb is still the frame just drawn. Drains the
         * RDP itself -- see posehash.c. */
        posehash_frame(fb);
#endif
        prev_fb = fb;

        /* Advance the game world while the RDP draws. The interpolation
         * swap must be undone first: the simulation owns the true heights.
         * Level transitions live inside G_Ticker's gameaction machine now;
         * G_DoLoadLevel brackets P_SetupLevel with the port's teardown and
         * rebuild (D_LevelPreSetup drains the RDP before freeing assets). */
        { void D_InterpEnd(void); D_InterpEnd(); }
        if (level_loaded) {
#if D_HWSTAT
            { const uint32_t t_ = TICKS_READ();
              run_game_tics(frame_delta_us);
              ph_sim_us += TICKS_TO_US(TICKS_SINCE(t_)); }
#else
            run_game_tics(frame_delta_us);
#endif
#if D_LEVELTEST
            {   /* Exercise the whole flow without input: exit each level a
                 * second in, and mash "attack" through intermission and
                 * finale so their acceleration paths advance too. */
                void G_ExitLevel(void);
                extern int leveltime;
                int D_InLevel(void);
                void D_TestPressFire(int on);
                if (D_InLevel()) {
                    void D_TestToggleMap(void);
                    extern int gametic;
                    static int am_toggled_at;
                    if (leveltime == 20 && am_toggled_at != gametic) {
                        am_toggled_at = gametic;
                        D_TestToggleMap();
                    }
                    if (leveltime > 90 && leveltime < 95) G_ExitLevel();
                } else {
                    extern int gametic;
                    D_TestPressFire((gametic & 8) != 0);
                }
            }
#endif
        }

        /* Peak, not average: the frame that misses vsync is the one that
         * shows up as a stutter, and it is invisible in a mean. Ignore the
         * first second, which is dominated by level load and cold caches. */
        static uint32_t worst_us;
        static int      warm;
        if (warm < 60) warm++;
        else if (frame_total_us > worst_us) worst_us = frame_total_us;

#if D_HWSTAT
        /* WHERE the worst frame happened, not just how bad it was.
         *
         * A peak with no coordinates cannot be revisited: the whole point of
         * a profiling run is to come back to the pose with VIEWLOCK or FREEZE
         * and work on it, and "56.9 ms somewhere in the demo" does not permit
         * that. So the window's worst frame carries its map, its pose and its
         * own phase split.
         *
         * The phase counters accumulate across the window and are zeroed with
         * it, so the per-frame cost is their delta since the previous frame --
         * hence the four shadows, reset in the same place the counters are. */
        static uint32_t win_us, win_submit, win_walk, win_wall, win_flat, win_spr;
        static uint32_t win_refl, win_fx;
        static int win_ep, win_map, win_x, win_y, win_z, win_a;
        static uint32_t pw_prev, pl_prev, pf_prev, ps_prev, pr_prev, px_prev;
        {
            const uint32_t d_walk = ph_walk_us      - pw_prev;
            const uint32_t d_wall = ph_wallflush_us - pl_prev;
            const uint32_t d_flat = ph_flatflush_us - pf_prev;
            const uint32_t d_spr  = ph_sprite_us    - ps_prev;
            const uint32_t d_refl = ph_refl_us      - pr_prev;
            const uint32_t d_fx   = ph_fx_us        - px_prev;
            pw_prev = ph_walk_us;      pl_prev = ph_wallflush_us;
            pf_prev = ph_flatflush_us; ps_prev = ph_sprite_us;
            pr_prev = ph_refl_us;      px_prev = ph_fx_us;

            if (warm >= 60 && frame_total_us > win_us) {
                extern int gameepisode, gamemap;
                win_us   = frame_total_us;
                /* Splits the frame in two at the right place: submit_us is
                 * CPU only, frame_total_us includes the wait for the RDP. So
                 * (total - submit) is RDP-bound time and
                 * (submit - walk - wall - flat - spr) is CPU work in the
                 * passes nothing else times -- sky, vapor, halos, the
                 * automap and the UI. Without this the shortfall is just
                 * "unaccounted" and could be either. */
                win_submit = submit_us;
                win_walk = d_walk; win_wall = d_wall;
                win_flat = d_flat; win_spr  = d_spr;
                win_refl = d_refl; win_fx   = d_fx;
                win_ep   = gameepisode; win_map = gamemap;
                win_x = (int)cam.x; win_y = (int)cam.y; win_z = (int)cam.z;
                win_a = (int)(cam.angle * 1000.0f);
            }
        }
#endif

        /* Periodic stats over USB: on hardware this is how frame numbers
         * reach the host without reading them off the television. Outside
         * the measured span, so it perturbs nothing there -- but debugf over
         * an undrained USB link can stall for milliseconds, so the whole
         * block is measurement-build only: HWSTAT owns the stats, and the
         * profiler flags keep just their own reports on the same cadence.
         * The drop counters are the "why is a surface missing" telemetry:
         * every path that can decline to draw something is counted, so a
         * hole on screen can be attributed from the log instead of guessed
         * at. dq = wall quads dropped (batch full), df = flat jobs dropped,
         * ofl = flat clip buffer overflow, dsp = sprite jobs dropped. */
#if D_HWSTAT || D_RSPQPROF || D_RSPIDLE
        {
            static int statctr;
#if D_HWSTAT
            static int acc_dq, acc_df, acc_dsp, acc_nd;
            /* The flat flush split. r_flat_begin zeroes these every frame,
             * so they have to be harvested here and summed over the window
             * like the phase counters they sit beside. emit = the per-vertex
             * expansion in emit_fan, bind = texture binds and group
             * re-registration; flat-emit-bind is the polygon clip and the
             * depth-band split in draw_one. That division is the whole
             * question behind an RSP vertex overlay: only emit could move.
             * ares puts emit at 54% of the flush over two routes, but models
             * neither uncached-store stalls nor D-cache misses -- which is
             * exactly what emit_fan's r_tri_slot writes are -- so 54% is a
             * floor and this line exists to find the hardware number. */
            static uint32_t acc_emit, acc_bind;
            static uint32_t acc_bands, acc_polys, acc_quads, acc_spr;
            static uint32_t acc_period, acc_f, acc_c;

            /* Accumulated since boot: the per-frame counters reset every
             * frame, and a transient drop between two-second samples would
             * vanish from a spot reading. */
            acc_dq  += r_quads_dropped();
            acc_df  += r_flat_dropped();
            acc_dsp += r_sprite_dropped();
            acc_nd  += r_drop_npts + r_drop_depth + r_drop_side;
            acc_emit += (uint32_t)r_flat_emit_us();
            acc_bind += (uint32_t)r_flat_bind_us();
            /* What the RDP is being ASKED to draw, as opposed to how long it
             * took. Fill, per-primitive setup and mode syncs are three
             * different bottlenecks with three different remedies, and
             * rdp_busy alone cannot tell them apart: a band count that moves
             * with rdp_busy says setup, one that does not says fill. These
             * are the accessors that had no caller until now. */
            acc_bands += (uint32_t)r_flat_bands();
            acc_polys += (uint32_t)r_flat_calls();
            acc_quads += (uint32_t)r_quads_emitted();
            acc_spr   += (uint32_t)r_sprite_count();
            /* The whole frame decomposed: period is wall clock between
             * frames, f runs from after the acquire to the RDP going idle,
             * c is CPU submit. period - f - acq is the loop's own tail
             * (game logic, audio, the melt). */
            acc_period += frame_delta_us;
            acc_f      += frame_total_us;
            acc_c      += submit_us;
#endif

            if (++statctr % 120 == 0)
                {
#if D_HWSTAT
                    int D_FlowState(int *ga);
                    int ga, gs = D_FlowState(&ga);
                    { extern uint32_t r_ph_cull_us, r_ph_seg_us, r_ph_flat_us;
                      debugf("walk: cull=%lu segs=%lu flatq=%lu\n",
                             (unsigned long)r_ph_cull_us,
                             (unsigned long)r_ph_seg_us,
                             (unsigned long)r_ph_flat_us);
                      r_ph_cull_us = r_ph_seg_us = r_ph_flat_us = 0; }
                    debugf("phase: walk=%lu wall=%lu flat=%lu spr=%lu "
                           "(us per 120 frames)\n",
                           (unsigned long)ph_walk_us,
                           (unsigned long)ph_wallflush_us,
                           (unsigned long)ph_flatflush_us,
                           (unsigned long)ph_sprite_us);
                    /* Printed before ph_flatflush_us is zeroed below: the
                     * clip figure is the residual of the very window the
                     * phase line just reported. */
                    debugf("flat: emit=%lu bind=%lu clip=%lu "
                           "(us per 120 frames)\n",
                           (unsigned long)acc_emit,
                           (unsigned long)acc_bind,
                           (unsigned long)(ph_flatflush_us > acc_emit + acc_bind
                                           ? ph_flatflush_us - acc_emit - acc_bind
                                           : 0));
                    /* Per FRAME, not per window: a primitive count is
                     * only meaningful against one frame's worth of fill. */
                    debugf("frame: period=%lu acq=%lu f=%lu c=%lu "
                           "(us per frame)\n",
                           (unsigned long)(acc_period / 120),
                           (unsigned long)(ph_acq_us / 120),
                           (unsigned long)(acc_f / 120),
                           (unsigned long)(acc_c / 120));
                    { const uint32_t tail = acc_period > ph_acq_us + acc_c
                            ? acc_period - ph_acq_us - acc_c : 0;
                      const uint32_t known = ph_sim_us + ph_audio_us;
                      debugf("tail: sim=%lu audio=%lu other=%lu of %lu "
                             "(us per frame)\n",
                             (unsigned long)(ph_sim_us / 120),
                             (unsigned long)(ph_audio_us / 120),
                             (unsigned long)((tail > known ? tail - known : 0) / 120),
                             (unsigned long)(tail / 120)); }
                    { extern uint32_t r_mus_refill_us, r_snd_mix_us;
                      extern uint32_t r_snd_qwait_us;
                      debugf("audio: snd=%lu (qwait=%lu mix=%lu)"
                             " mus=%lu (refill=%lu) (us per frame)\n",
                             (unsigned long)(ph_snd_us / 120),
                             (unsigned long)(r_snd_qwait_us / 120),
                             (unsigned long)(r_snd_mix_us / 120),
                             (unsigned long)(ph_mus_us / 120),
                             (unsigned long)(r_mus_refill_us / 120));
                      r_mus_refill_us = r_snd_mix_us = r_snd_qwait_us = 0; }
                    ph_snd_us = ph_mus_us = 0;
                    ph_sim_us = ph_audio_us = 0;
                    acc_period = acc_f = acc_c = 0; ph_acq_us = 0;
                    debugf("prim: bands=%lu polys=%lu quads=%lu spr=%lu "
                           "(per frame)\n",
                           (unsigned long)(acc_bands / 120),
                           (unsigned long)(acc_polys / 120),
                           (unsigned long)(acc_quads / 120),
                           (unsigned long)(acc_spr   / 120));
                    acc_bands = acc_polys = acc_quads = acc_spr = 0;
                    acc_emit = acc_bind = 0;
#if R_REFLECT
                    /* How many mirror images the size gate kept against how
                     * many it turned away. Both numbers matter: kept=0 with
                     * small>0 would mean R_REFLMINPX has switched the pass
                     * off rather than trimmed it. */
                    { extern int r_refl_small, r_refl_kept;
                      debugf("refl: kept=%d small=%d minpx=%d\n",
                             r_refl_kept, r_refl_small, R_REFLMINPX);
                      r_refl_kept = r_refl_small = 0; }
#endif
#if D_DYNLIGHT
                    /* Is R_LIGHTMAX a ceiling anyone reaches? peak is the
                     * most slots any frame in the window used; full is how
                     * many adds arrived at a full registry. full=0 means the
                     * cap is dead configuration on this route, whatever it
                     * is set to. */
#if R_ENVMAP
                    { extern int r_env_dropped, r_env_peak;
                      debugf("env: peak=%d dropped=%d cap=%d\n",
                             r_env_peak, r_env_dropped, 16);
                      r_env_peak = r_env_dropped = 0; }
#endif
                    { extern int r_light_peak, r_light_full;
                      debugf("light: peak=%d full=%d cap=%d\n",
                             r_light_peak, r_light_full, R_LIGHTMAX);
                      r_light_peak = r_light_full = 0; }
#endif
                    ph_walk_us = ph_wallflush_us = 0;
                    ph_flatflush_us = ph_sprite_us = 0;
                    /* The per-frame shadows track those counters, so they
                     * have to be zeroed with them or the next frame's delta
                     * underflows into a vast bogus peak. */
                    pw_prev = pl_prev = pf_prev = ps_prev = 0;
                    pr_prev = px_prev = 0;
                    ph_refl_us = ph_fx_us = 0;
                    /* The window's worst, with the pose that produced it --
                     * this is the line a profiling run exists to collect. */
                    debugf("worst: %luus sub=%lu E%dM%d pose=%d,%d,%d,%d "
                           "walk=%lu wall=%lu flat=%lu spr=%lu "
                           "refl=%lu fx=%lu\n",
                           (unsigned long)win_us, (unsigned long)win_submit,
                           win_ep, win_map,
                           win_x, win_y, win_z, win_a,
                           (unsigned long)win_walk, (unsigned long)win_wall,
                           (unsigned long)win_flat, (unsigned long)win_spr,
                           (unsigned long)win_refl, (unsigned long)win_fx);
                    win_us = 0;
                    /* Arena high-water marks during play, not just at load.
                     * The load-time report fires before the first level's
                     * textures and every sprite frame is demand-loaded, so it
                     * reads 0 and says nothing about the real peak -- which is
                     * the number the reservations in mem.h have to cover. */
                    mem_report("in play");
                    debugf("hwstat: f=%lu c=%lu worst=%lu dq=%d df=%d "
                           "ofl=%d dsp=%d nd=%d gs=%d ga=%d pose=%d,%d,%d,%d\n",
                           (unsigned long)frame_total_us,
                           (unsigned long)submit_us,
                           (unsigned long)worst_us,
                           acc_dq, acc_df, r_drop_clipofl, acc_dsp, acc_nd,
                           gs, ga,
                           (int)cam.x, (int)cam.y, (int)cam.z,
                           (int)(cam.angle * 1000.0f));
#endif /* D_HWSTAT */
#if D_RSPQPROF
                    /* Where the RSP's time goes. wait_cpu is what this build
                     * exists to read: RSP time spent stalled waiting for the
                     * CPU to hand it commands. Large means there is slack to
                     * move submit work onto the RSP; near zero means it is
                     * already saturated, and an overlay would move the queue
                     * rather than the frame time.
                     *
                     * Shares are of the profiled window, which is the same
                     * thing as the per-frame share. Integer percentages: %f
                     * would drag newlib's float printf into the ROM for a
                     * debug line, the same reason the HUD above avoids it. */
                    {
                        rspq_profile_data_t pd;
                        rspq_profile_get_data(&pd);
                        if (pd.frame_count && pd.total_ticks) {
                            const uint64_t tot  = pd.total_ticks;
                            const uint64_t wcpu =
                                pd.slots[RSPQ_PROFILE_CSLOT_WAIT_CPU].total_ticks;
                            const uint64_t wrdp =
                                pd.slots[RSPQ_PROFILE_CSLOT_WAIT_RDP].total_ticks
                              + pd.slots[RSPQ_PROFILE_CSLOT_WAIT_RDP_SYNCFULL].total_ticks
                              + pd.slots[RSPQ_PROFILE_CSLOT_WAIT_RDP_SYNCFULL_MULTI].total_ticks;
                            const uint64_t swi  =
                                pd.slots[RSPQ_PROFILE_CSLOT_OVL_SWITCH].total_ticks;
                            uint64_t work = 0;
                            for (int i = 0; i < RSPQ_MAX_OVERLAYS; i++)
                                work += pd.slots[i].total_ticks;

                            debugf("rspqprof: frames=%lu frame=%luus work=%lu%% "
                                   "wait_cpu=%lu%% wait_rdp=%lu%% ovlsw=%lu%% "
                                   "rdp_busy=%lu%%\n",
                                   (unsigned long)pd.frame_count,
                                   RSPQPROF_US(tot / pd.frame_count),
                                   (unsigned long)((work * 100) / tot),
                                   (unsigned long)((wcpu * 100) / tot),
                                   (unsigned long)((wrdp * 100) / tot),
                                   (unsigned long)((swi  * 100) / tot),
                                   (unsigned long)((pd.rdp_busy_ticks * 100) / tot));

                            /* Which ucode owns the work, so an offload can be
                             * costed against what the RSP already runs:
                             * rdpq's triangle setup against the mixer's. */
                            for (int i = 0; i < RSPQ_MAX_OVERLAYS; i++)
                                if (pd.slots[i].name && pd.slots[i].total_ticks)
                                    debugf("  rspqovl %-16s %lu%% %luus/frame\n",
                                           pd.slots[i].name,
                                           (unsigned long)((pd.slots[i].total_ticks * 100) / tot),
                                           RSPQPROF_US(pd.slots[i].total_ticks / pd.frame_count));
                        }
                        /* Per-window like the phase accumulators above. A
                         * running average since boot would bury whatever the
                         * route is doing now under the title screen. */
                        rspq_profile_reset();
                    }
#endif
#if D_RSPIDLE
                    rspidle_report();
#endif
                }
        }
#endif /* D_HWSTAT || D_RSPQPROF || D_RSPIDLE */

        /* Keep the mixer fed. It fills whatever room the audio buffers have,
         * so this self-regulates against the frame rate. */
#if D_SOUND
        {
#if D_HWSTAT
          const uint32_t t0 = TICKS_READ();
          I_Sound_Update();
          const uint32_t t1 = TICKS_READ();
          mus_update();
          const uint32_t t2 = TICKS_READ();
          ph_snd_us   += TICKS_TO_US(TICKS_DISTANCE(t0, t1));
          ph_mus_us   += TICKS_TO_US(TICKS_DISTANCE(t1, t2));
          ph_audio_us += TICKS_TO_US(TICKS_DISTANCE(t0, t2));
#else
          I_Sound_Update();
          mus_update();
#endif
        }
#endif

        { static int once = 0; if (!once && level_loaded) { once = 1; r_bsp_dump(); } }

#if DEBUG_RDP
        /* Frame counter doubles as a throughput reading: if this crawls, a
         * partially-drawn capture is the explanation for a truncated image. */
        if ((++frame % 30) == 0) debugf("frame %lu\n", (unsigned long)frame);
#endif
    }
}

#if DEBUG_RDP
int r_debug_dump = 0;
#endif
