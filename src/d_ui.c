/*
 * d_ui -- the menu, status bar and message system, wired to the console.
 *
 * These are Doom's own files, taken from upstream Chocolate Doom rather than
 * the the source fork fork the game layer came from, because only the UI files
 * carried that fork's platform layer. They draw through V_DrawPatch, which
 * src/v_draw.c supplies on the RDP.
 *
 * What they need from this port is what a PC gives them for free: somewhere to
 * be initialised, a place in the frame to draw, a tick, and events. The one
 * subtlety is input ordering -- the menu has to see a button before the game
 * does, and while it is open the player must not also be walking around.
 */
#include "doom/doomtype.h"
#include "doom/doomdef.h"
#include "doom/doomstat.h"
#include "doom/doomkeys.h"
#include "doom/d_event.h"
#include "doom/m_menu.h"
#include "doom/st_stuff.h"
#include "doom/hu_stuff.h"
#include "doom/v_video.h"
#include "doom/r_defs.h"
#include "doom/i_system.h"

#include "doom/i_swap.h"

#include <libdragon.h>
#include <ctype.h>
#include <string.h>

void D_UI_Init(void)
{
    M_Init();
    ST_Init();
    HU_Init();

    /* The message font is the first thing to notice a lump cache that has
     * quietly emptied: HU_Start indexes font[0] without checking, so a null
     * there is a crash rather than a missing glyph. */
    extern patch_t *hu_font[];
    if (!hu_font[0])
        I_Error("HU_Init: message font did not load");
}

/* Called after a level loads: the status bar rebuilds its faces and the
 * message system clears whatever the last level left on screen. */
void D_UI_LevelStart(void)
{
    ST_Start();
    HU_Start();
}

void D_UI_Ticker(void)
{
    M_Ticker();
    ST_Ticker();
    HU_Ticker();
}

/* Drawn after the world and the weapon, in that order: the status bar sits
 * over the view, and the menu sits over everything. The caller holds the
 * V_BeginUI bracket open around this and the weapon, so no patch pays for
 * its own mode setup. */
/* Pixels of horizontal safe area for edge-anchored overlays. Four is
 * enough for the clipping seen on a CRT without visibly floating the text
 * away from the corner. */
#define UI_SAFE_X 4

/* Pre-attach: run the vendored bar drawer into the capture list, diff it
 * against the committed frame, and recomposite the offscreen strip when
 * anything -- including the palette bank -- changed. Runs before the
 * frame's main attach so the composite's own attach nests cleanly (the
 * same slot the wipe's blit uses). Bonus over the old flow: the
 * ST_doPaletteStuff inside ST_Drawer now selects the flash bank BEFORE the
 * frame binds its TLUT, so flashes land the same frame instead of one
 * late, and the staging buffer is never mutated behind an in-flight DMA. */
void D_UI_UpdateBar(void)
{
    void V_BarCaptureBegin(void);
    int V_BarCaptureEnd(void);
    void V_BarComposite(void);
    void V_SetYShift(int shift);

    if (gamestate != GS_LEVEL || !gametic) return;
    V_BarCaptureBegin();
    V_SetYShift(40);
    ST_Drawer(false, true);
    V_SetYShift(0);
    if (V_BarCaptureEnd())
        V_BarComposite();
}

void D_UI_Draw(void)
{
    void V_SetYShift(int shift);
    void V_SetXShift(int shift);
    void WI_Drawer(void);
    void F_Drawer(void);
    void D_PageDrawer(void);

    /* Doom lays its UI out for 320x200; this screen is 320x240. Each
     * subsystem gets the placement that matches its anchor: the status bar
     * rides 40 rows down to sit flush with the bottom edge (the weapon
     * already shifts the same amount), messages stay pinned to the top, and
     * full-screen states centre their 200-row layout 20 rows down. The
     * menu draws over every state, exactly as D_Display orders it. */
    switch (gamestate) {
    case GS_LEVEL:
        if (!gametic)
            break;              /* D_Display's guard: no bar before a tic */
        /* (bar handled below; composite maintained by D_UI_UpdateBar) */
        {
            /* The bar comes from the composited strip when one is valid
             * (D_UI_UpdateBar rebuilt it pre-attach if anything changed);
             * the direct path survives as the fallback for allocation
             * failure or capture overflow. */
            int V_BarValid(void);
            void V_BarBlit(void);
            if (V_BarValid()) {
                V_BarBlit();
            } else {
                V_SetYShift(40);
                ST_Drawer(false, true);
                V_SetYShift(0);
            }
        }
        /* Messages and the automap's level name hug the left edge, where a
         * television's overscan would clip them. */
        V_SetXShift(UI_SAFE_X);
        HU_Drawer();
        V_SetXShift(0);
        break;
    case GS_INTERMISSION:
        V_SetYShift(20);
        WI_Drawer();
        break;
    case GS_FINALE:
        V_SetYShift(20);
        F_Drawer();
        break;
    case GS_DEMOSCREEN:
        V_SetYShift(20);
        D_PageDrawer();
        break;
    }
    V_SetYShift(20);
    M_Drawer();
    V_SetYShift(0);
}

/* Draw a line of text with the HUD font, through the same RDP path as every
 * other patch. This is what lets the frame loop's instrumentation stop
 * writing into the framebuffer with the CPU -- the write that forced a full
 * rdpq_detach_wait before the text could go in. Call inside a V_BeginUI
 * bracket. */
void D_UI_DrawText(int x, int y, const char *s)
{
    extern patch_t *hu_font[];

    for (; *s; s++) {
        const int c = toupper((unsigned char)*s);
        if (c < '!' || c > '_' || !hu_font[c - '!']) { x += 4; continue; }

        patch_t *p = hu_font[c - '!'];
        V_DrawPatch(x, y, p);
        x += SHORT(p->width);
    }
}


/* --- input ------------------------------------------------------------- */

/* Turn a button into a Doom key event, once per press.
 *
 * Doom's menu is edge-driven -- it moves one item per keypress, not one per
 * frame the button is held -- so the transition is what matters, and holding a
 * direction must not scroll the menu at 50 items a second. */
static void post_key(int key, boolean down)
{
    boolean G_Responder(event_t *ev);
    event_t ev;
    ev.type = down ? ev_keydown : ev_keyup;
    ev.data1 = key;
    ev.data2 = ev.data3 = 0;
    /* Chocolate's D_ProcessEvents order: the menu gets first refusal, the
     * game the rest -- that is what makes any key at the title screen open
     * the menu, and pause/weapon keys work without menu involvement. */
    if (!M_Responder(&ev))
        G_Responder(&ev);
}

#if D_MENUTEST
/* Press and release a key exactly as a controller button would, menu first
 * and the game second -- reaching only M_Responder meant keys the game owns,
 * such as the automap toggle, could not be driven from a test at all. */
void D_TestMenuKey(int key)
{
    post_key(key, true);
    post_key(key, false);
}
#endif

/* Map the controller and let the menu consume what it wants.
 *
 * Returns true when the menu is active, so the caller knows to leave the
 * player standing still: without that, walking continues underneath an open
 * menu and the first press of Start sends you into a wall. */
/* Taken by value: joypad_buttons_t is packed, so the address of one inside
 * joypad_inputs_t cannot be handed around safely. */
boolean D_UI_HandleInput(joypad_buttons_t held)
{
    static joypad_buttons_t prev;

    struct { unsigned b; int key; } const map[] = {
        { 0, KEY_ESCAPE    },   /* start  -- open and close the menu */
        { 1, KEY_UPARROW   },
        { 2, KEY_DOWNARROW },
        { 3, KEY_LEFTARROW },
        { 4, KEY_RIGHTARROW},
        { 5, KEY_ENTER     },   /* a      -- select in menus; use in play */
        { 6, '='           },   /* c-up   -- automap zoom in (map only) */
        { 7, '-'           },   /* c-down -- automap zoom out (map only) */
    };

    /* The zoom keys only mean anything on the map; keep the C buttons out
     * of the responders otherwise so nothing else grows a surprise bind. */
    int D_AutomapActive(void);
    const boolean am = D_AutomapActive();

    const boolean now[] = {
        held.start, held.d_up, held.d_down, held.d_left, held.d_right,
        held.a,
        am && held.c_up, am && held.c_down,
    };
    const boolean was[] = {
        prev.start, prev.d_up, prev.d_down, prev.d_left, prev.d_right,
        prev.a,
        am && prev.c_up, am && prev.c_down,
    };

    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (now[i] != was[i]) post_key(map[i].key, now[i]);

    /* B backs out of a menu. In play it opens doors and works switches, which
     * is a ticcmd flag rather than a key, so nothing is posted for it here.
     *
     * The latch pairs each press with its own release: the menu can close
     * while the button is still down, and posting the release of a key that
     * was never pressed -- or failing to post one that was -- leaves the
     * responder holding it forever. */
    static int b_key;
    if (held.b && !prev.b) {
        if (menuactive) { b_key = KEY_BACKSPACE; post_key(b_key, true); }
    } else if (!held.b && prev.b && b_key) {
        post_key(b_key, false);
        b_key = 0;
    }

    /* Either trigger toggles the automap, read as one button so holding one
     * and pressing the other does not toggle twice. */
    const boolean trig_now = held.l || held.r;
    const boolean trig_was = prev.l || prev.r;
    static int trig_key;
    if (trig_now && !trig_was) {
        if (!menuactive) { trig_key = KEY_TAB; post_key(trig_key, true); }
    } else if (!trig_now && trig_was && trig_key) {
        post_key(trig_key, false);
        trig_key = 0;
    }

    /* Off the map, the C buttons cycle weapons. Queued on the press edge:
     * holding one must not run through the whole arsenal, and taps faster
     * than a tic must not be lost. */
    if (!am && !menuactive) {
        void G_QueueWeaponStep(int direction);
        if (held.c_up   && !prev.c_up)   G_QueueWeaponStep(+1);
        if (held.c_down && !prev.c_down) G_QueueWeaponStep(-1);
    }

    prev = held;
    return menuactive;
}
