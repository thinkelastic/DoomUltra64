/*
 * i_n64 -- the platform layer Doom's game code expects.
 *
 * Doom's sources call out to a small set of system services and read a smaller
 * set of globals. On a PC those come from i_system.c, d_main.c and doomstat.c,
 * none of which port: they carry the console, argument parsing, networking and
 * savegames with them. This provides only what the game layer actually
 * references, so the ported files compile against it unchanged.
 *
 * Values here reflect the target: single player, no demos, no dehacked. Where
 * Doom would branch on a runtime option, the constant it collapses to is the
 * one that matters for a cartridge.
 */
#include "doom/doomtype.h"
#include "doom/d_mode.h"
#include "doom/doomstat.h"
#include "doom/d_player.h"
#include "doom/i_system.h"
#include "doom/z_zone.h"

#include <libdragon.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

/* --- game state ------------------------------------------------------- */

/* The shareware and commercial IWADs differ in which things and levels exist,
 * and the game code checks this in a dozen places. Set from the IWAD's lumps
 * at startup rather than hardcoded, so a Freedoom or full-Doom cartridge both
 * behave correctly. */
GameMode_t   gamemode    = indetermined;
GameMission_t gamemission = doom;
GameVersion_t gameversion = exe_final2;


boolean      nomonsters   = false;
boolean      respawnparm  = false;
boolean      fastparm     = false;

/* Doom runs its logic at a fixed 35 Hz regardless of how fast it draws, and
 * every speed constant in the game is expressed per tic. Keeping that fixed is
 * what makes ported behaviour match: scaling it to the frame rate would change
 * how far a monster steps and how fast a door opens.
 *
 * leveltime belongs to p_tick.c now that the thinker list is in. */
int          gametic;

/* Level tallies, filled by the loader and read by the intermission screen. */

/* Doom precaches every texture a level uses into RAM at load. Here they are
 * baked on the host and streamed from the cartridge by p_level.c, so the flag
 * stays off rather than driving a second copy. */

/* The body queue recycles corpses once a level has accumulated too many, so
 * that a long fight cannot exhaust the thinker list. Its head and tail live in
 * p_mobj.c; only the slot index is g_game's. */


/* --- system services --------------------------------------------------- */

void I_Error(const char *error, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, error);
    vsnprintf(buf, sizeof buf, error, ap);
    va_end(ap);

    /* assertf halts with the message on screen and on the debug channel, which
     * is the closest thing to Doom's exit-to-console that a cartridge has. */
    assertf(false, "I_Error: %s", buf);
}

void I_Quit(void) { I_Error("quit"); }

void *I_Realloc(void *ptr, size_t size)
{
    void *n = realloc(ptr, size);
    if (!n && size) I_Error("I_Realloc: failed on %zu bytes", size);
    return n;
}

/* Doom's zone allocator wants one contiguous block to manage.
 *
 * On a PC this is sized from free system memory. Here the budget is fixed and
 * known: 8 MB with the Expansion Pak, of which baked textures peak around
 * 1.3 MB and the framebuffers and rspq take their own share. Three megabytes
 * leaves comfortable headroom for level data, thinkers and the lump cache,
 * and being a fixed budget it fails at startup rather than mid-level if it is
 * ever too small. */
/* Two megabytes, not three.
 *
 * E1M2 -- the largest map tried -- uses about 550 KB of zone, so three was
 * generous for its own sake and left the system heap at 200 KB once baked
 * textures were resident. That is not enough for rdpq's command buffers, and
 * the failure is silent: allocation fails, rendering stops, and the screen
 * goes black with the level loaded and every count correct. */
#define ZONE_SIZE (2 * 1024 * 1024)


/* --- savegames -----------------------------------------------------------
 *
 * Two backends behind one API: files on the SD card when there is one, and
 * the cartridge's SRAM when there is not.
 *
 * SRAM came first and is why the menu was lying. The cartridge carries
 * 128 KB, declared through the ROM header savetype (sram1m in the Makefile)
 * and persisted by the SC64 menu to a .sav beside the ROM. A Doom savegame
 * runs to about 30 KB on E1M1 and more on a large level, so 128 KB is two
 * slots, not the six the menu offers. Writes to the other four were rejected
 * here and the rejection went nowhere: the save silently did not happen, and
 * the load menu showed the slot as empty because it genuinely was. Both
 * halves of "only two saves, and the one I just made is missing" are that.
 *
 * The card fixes it properly -- six slots, each as large as the level needs.
 * It is mounted before this is ever called, since the IWAD is read through
 * it. Saves go in a saves folder beside the ROM, falling back to wherever
 * the IWAD was found, so the set stays together.
 *
 * Writes are read back before being called done. This driver discards each
 * sector's CRC (libcart's edx_card_rd_dram: "We ignore the CRC"), so a bad
 * transfer looks exactly like a good one -- and a savegame that reports
 * success and is not there is worse than one that admits it failed. If the
 * read-back does not match, the write falls back to SRAM when the slot is
 * one SRAM can hold, and reports failure when it is not.
 *
 * SRAM layout, unchanged: two 64 KB slots, each [16-byte wrapper][payload],
 * the wrapper carrying a magic and the payload length. The slot index comes
 * from the digit in the P_SaveGameFile name ("<prefix>_<n>.sav"). PI DMA
 * needs aligned RAM on this side; g_game's buffer is aligned, and the small
 * header reads bounce through an aligned scratch. */
#include "doom/i_save.h"
#include <stdio.h>
#include <errno.h>
#include <string.h>

#define SAVE_SRAM_BASE   0x08000000u
#define SAVE_SLOT_BYTES  0x10000u
#define SAVE_SRAM_SLOTS  2
#define SAVE_WRAP        16

static uint8_t save_scratch[64] __attribute__((aligned(16)));

static int save_slot_from_name(const char *name)
{
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (!dot || dot == name) return -1;
    const char c = dot[-1];
    return (c >= '0' && c <= '9') ? c - '0' : -1;
}

static boolean save_read_wrap(uint32_t base, uint32_t *psize)
{
    data_cache_hit_writeback_invalidate(save_scratch, SAVE_WRAP);
    dma_read(save_scratch, base, SAVE_WRAP);
    if (memcmp(save_scratch, "PDSV", 4) != 0) return false;
    uint32_t size;
    memcpy(&size, save_scratch + 4, 4);
    if (size == 0 || size > SAVE_SLOT_BYTES - SAVE_WRAP) return false;
    *psize = size;
    return true;
}

static boolean sram_read(int slot, byte *buffer,
                         size_t buffer_size, size_t *size_out)
{
    if (slot < 0 || slot >= SAVE_SRAM_SLOTS) return false;
    const uint32_t base = SAVE_SRAM_BASE + (uint32_t)slot * SAVE_SLOT_BYTES;

    uint32_t size;
    if (!save_read_wrap(base, &size) || size > buffer_size) return false;

    const uint32_t n = (size + 15u) & ~15u;
    data_cache_hit_writeback_invalidate(buffer, n);
    dma_read(buffer, base + SAVE_WRAP, n);
    *size_out = size;
    return true;
}

static boolean sram_read_header(int slot, byte *buffer, size_t len)
{
    if (slot < 0 || slot >= SAVE_SRAM_SLOTS ||
        len > sizeof(save_scratch)) return false;
    const uint32_t base = SAVE_SRAM_BASE + (uint32_t)slot * SAVE_SLOT_BYTES;

    uint32_t size;
    if (!save_read_wrap(base, &size) || size < len) return false;

    data_cache_hit_writeback_invalidate(save_scratch, sizeof(save_scratch));
    dma_read(save_scratch, base + SAVE_WRAP, (len + 15u) & ~15u);
    memcpy(buffer, save_scratch, len);
    return true;
}

static boolean sram_write(int slot, const byte *buffer, size_t size)
{
    if (slot < 0 || slot >= SAVE_SRAM_SLOTS) return false;
    if (size == 0 || size > SAVE_SLOT_BYTES - SAVE_WRAP) return false;
    const uint32_t base = SAVE_SRAM_BASE + (uint32_t)slot * SAVE_SLOT_BYTES;

    memcpy(save_scratch, "PDSV", 4);
    uint32_t sz = (uint32_t)size;
    memcpy(save_scratch + 4, &sz, 4);
    memset(save_scratch + 8, 0, SAVE_WRAP - 8);
    data_cache_hit_writeback(save_scratch, SAVE_WRAP);
    dma_write(save_scratch, base, SAVE_WRAP);

    data_cache_hit_writeback((void *)buffer, size);
    dma_write(buffer, base + SAVE_WRAP, (size + 1u) & ~1u);
    return true;
}

/* --- the card ---------------------------------------------------------- */

/* Where saves go, most specific first. The saves folder beside the ROM is
 * where they belong; the directories after it are the ones the IWAD search
 * uses, so a card without that folder still works. */
static const char *const save_dirs[] = {
    "sd:/Doom/saves/", "sd:/DOOM/saves/", "sd:/doom/saves/",
    "sd:/Doom/", "sd:/DOOM/", "sd:/doom/", "sd:/saves/", "sd:/",
};

/* Remembered once a slot has been read or written successfully, so the
 * directory search happens once rather than on every menu redraw. */
static int save_dir_known = -1;

/* Which backend last served a write, and the reason the card did not, for
 * the self-test to report. A console under a television has no USB cable,
 * so this has to come back on screen. */
static char save_last_backend = '-';
static int  save_last_errno;
static char save_last_path[80];

/* The caller already built a filename ("doom_0.sav"); only the directory
 * in front of it is ours to choose. */
static void save_path(char *out, size_t cap, int dir, const char *name)
{
    const char *slash = strrchr(name, '/');
    snprintf(out, cap, "%s%s", save_dirs[dir], slash ? slash + 1 : name);
}

/* Open a slot for reading, trying each directory until one has it. */
static FILE *sd_open_read(const char *name)
{
    char path[80];
    if (save_dir_known >= 0) {
        save_path(path, sizeof path, save_dir_known, name);
        FILE *f = fopen(path, "rb");
        if (f) return f;
    }
    for (unsigned d = 0; d < sizeof save_dirs / sizeof save_dirs[0]; d++) {
        save_path(path, sizeof path, (int)d, name);
        FILE *f = fopen(path, "rb");
        if (f) { save_dir_known = (int)d; return f; }
    }
    return NULL;
}

static boolean sd_read_header(const char *name, byte *buffer, size_t len)
{
    FILE *f = sd_open_read(name);
    if (!f) return false;
    const size_t got = fread(buffer, 1, len, f);
    fclose(f);
    return got == len;
}

static boolean sd_read(const char *name, byte *buffer, size_t cap,
                       size_t *size_out)
{
    FILE *f = sd_open_read(name);
    if (!f) return false;
    const size_t got = fread(buffer, 1, cap, f);
    fclose(f);
    if (got == 0) return false;
    *size_out = got;
    return true;
}

/* Write, then read back and compare before reporting success.
 *
 * The card driver discards each sector's CRC, so a corrupt write is
 * indistinguishable from a good one at this level. A savegame is the one
 * thing here that must not quietly not happen. */
static boolean sd_write(const char *name, const byte *buffer, size_t size)
{
    char path[80];
    const int ndirs = (int)(sizeof save_dirs / sizeof save_dirs[0]);

    /* -1 means "the directory that already worked", then each in order.
     * fopen for writing does not create a missing directory, so the ones
     * that are not there simply fail and cost nothing. */
    for (int k = (save_dir_known >= 0 ? -1 : 0); k < ndirs; k++) {
        const int d = (k < 0) ? save_dir_known : k;

        save_path(path, sizeof path, d, name);
        FILE *f = fopen(path, "wb");
        if (!f) continue;
        const size_t put = fwrite(buffer, 1, size, f);
        fclose(f);
        if (put != size) continue;

        /* Read the header back. A short or wrong read means the bytes did
         * not land, whatever fwrite reported. */
        byte check[64];
        const size_t n = size < sizeof check ? size : sizeof check;
        FILE *v = fopen(path, "rb");
        if (!v) continue;
        const size_t got = fread(check, 1, n, v);
        fclose(v);
        if (got != n || memcmp(check, buffer, n) != 0) continue;

        save_dir_known = d;
        snprintf(save_last_path, sizeof save_last_path, "%s", path);
        return true;
    }
    save_last_errno = errno;
    return false;
}

/* --- the API the game calls -------------------------------------------- */

boolean I_SaveSlotRead(const char *name, byte *buffer,
                       size_t buffer_size, size_t *size_out)
{
    const int slot = save_slot_from_name(name);
    if (slot < 0) return false;
    if (sd_read(name, buffer, buffer_size, size_out)) return true;
    return sram_read(slot, buffer, buffer_size, size_out);
}

boolean I_SaveSlotReadHeader(const char *name, byte *buffer, size_t len)
{
    const int slot = save_slot_from_name(name);
    if (slot < 0) return false;
    if (sd_read_header(name, buffer, len)) return true;
    return sram_read_header(slot, buffer, len);
}

boolean I_SaveSlotWrite(const char *name, const byte *buffer, size_t size)
{
    const int slot = save_slot_from_name(name);
    debugf("save: write '%s' slot=%d size=%u\n", name ? name : "(null)",
           slot, (unsigned)size);
    if (slot < 0 || size == 0) return false;
    if (sd_write(name, buffer, size)) { save_last_backend = 'S'; return true; }
    /* No card, or it would not take it. SRAM holds the first two slots. */
    if (sram_write(slot, buffer, size)) { save_last_backend = 'R'; return true; }
    save_last_backend = '-';
    return false;
}

#if D_SAVETEST
/* Report what is actually in each save slot, and which backend answered.
 *
 * SAVETEST=1 only reads: it is meant to be booted straight after saving a
 * game normally, and it says whether that save landed, where, and whether
 * the menu would accept it. SAVETEST=2 additionally round-trips a payload
 * through every slot first, which overwrites them.
 *
 * The result comes back through assertf, the only thing on this console that
 * draws text without the IWAD's font and without a cable. */
void I_SaveSelfTest(void)
{
    char report[6][64];
    char all[512];

#if D_SAVETEST >= 2
    {
        static byte payload[40 * 1024] __attribute__((aligned(16)));
        for (unsigned i = 0; i < sizeof payload; i++)
            payload[i] = (byte)(i * 7 + 3);
        for (int slot = 0; slot < 6; slot++) {
            char name[32];
            snprintf(name, sizeof name, "doom_%d.sav", slot);
            save_last_backend = '-';
            const boolean w = I_SaveSlotWrite(name, payload, sizeof payload);
            debugf("savetest: slot %d write(%u bytes)=%s via %c\n",
                   slot, (unsigned)sizeof payload, w ? "ok" : "FAIL",
                   save_last_backend);
        }
    }
#endif

    for (int slot = 0; slot < 6; slot++) {
        char name[32];
        byte hdr[40];                 /* SAVESTRINGSIZE + VERSIONSIZE */
        snprintf(name, sizeof name, "doom_%d.sav", slot);

        /* Which backend has it: ask each directly rather than through the
         * public call, so the answer names the one that actually replied. */
        const boolean on_sd   = sd_read_header(name, hdr, sizeof hdr);
        const boolean on_sram = !on_sd && sram_read_header(slot, hdr, sizeof hdr);

        if (!on_sd && !on_sram) {
            snprintf(report[slot], sizeof report[slot], "%d -  empty", slot);
            continue;
        }

        /* The description, as the menu reads it: printable and non-empty. */
        char desc[25];
        int printable = 1, any = 0;
        for (int i = 0; i < 24; i++) {
            const byte c = hdr[i];
            if (c == 0) break;
            if (c < 32 || c > 126) { printable = 0; break; }
            desc[i] = (char)c; any = 1;
        }
        desc[24] = 0;
        if (!printable || !any) snprintf(desc, sizeof desc, "<not text>");
        else desc[any ? (int)strnlen((char *)hdr, 24) : 0] = 0;

        char ver[17];
        memcpy(ver, hdr + 24, 16); ver[16] = 0;
        for (int i = 0; i < 16; i++)
            if (ver[i] && (ver[i] < 32 || ver[i] > 126)) { ver[i] = 0; break; }

        snprintf(report[slot], sizeof report[slot], "%d %c  \"%s\" [%s]",
                 slot, on_sd ? 'S' : 'R', desc, ver);
        debugf("savetest: %s\n", report[slot]);
    }

    snprintf(all, sizeof all,
             "savetest -- what is in each slot\n\n"
             "%s\n%s\n%s\n%s\n%s\n%s\n\n"
             "S = card, R = cartridge SRAM\n"
             "the menu wants printable text and [version N]",
             report[0], report[1], report[2],
             report[3], report[4], report[5]);
    assertf(0, "%s", all);
}
#endif

byte *I_ZoneBase(int *size)
{
    /* 16-byte aligned: zone blocks end up holding level data the RDP reads. */
    byte *base = memalign(16, ZONE_SIZE);
    if (!base) I_Error("I_ZoneBase: could not reserve %d bytes", ZONE_SIZE);
    *size = ZONE_SIZE;
    return base;
}

void I_Tactile(int on, int off, int total)
{
    (void)on; (void)off; (void)total;   /* rumble pak, not wired up yet */
}
