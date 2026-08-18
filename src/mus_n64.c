/*
 * mus_n64 -- music streamed from a flashcart's SD card.
 *
 * The music set is raw 48 kHz stereo PCM, one lump per level, and comes to
 * about a gigabyte. That is not a cartridge asset under any compression this
 * project could apply -- the ROM is 64 MB at absolute best -- so it is not
 * baked in like every other asset here. It stays on the SD card and is read as
 * it plays.
 *
 * That makes music the one feature that depends on the hardware it runs on.
 * libdragon speaks 64drive, EverDrive and SC64, and debug_init_sdfs reports
 * whether any of them is present with a card in it. On a bare cartridge that
 * returns false and everything below simply never runs: no music, no error,
 * the game unaffected. Failing closed matters more than usual here because the
 * common case -- an emulator, or a cart with no card -- is the failing one.
 *
 * Reading is decoupled from mixing by a ring buffer. The mixer's read callback
 * runs inside mixer_try_play and must never touch the card: an SD read there
 * would serialise the audio update against the PI bus and stall the frame.
 * Instead the callback only ever copies from the ring, and the ring is topped
 * up once a frame from the main loop, where a slow read costs a frame rather
 * than an audio dropout.
 */
#include <libdragon.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "mus_n64.h"

#if MUS_CART
#include <fat.h>
#include <ioctl.h>
/* SD -> cartridge SDRAM, with neither the N64 nor fatfs in the path. */
extern int cart_card_rd_cart(uint32_t cart, uint32_t lba, uint32_t count);
#endif

#if D_HWSTAT
/* The SD-card refill, timed apart from the rest of mus_update: the music
 * stream is read off the flashcart's card, and a blocking card read is the
 * obvious candidate for the 8 ms the audio update was measured to cost. */
uint32_t r_mus_refill_us;
#endif

/* The music WAD's own header: sample rate and channel count, ahead of the
 * track lumps. Everything else about the format is implied -- signed 16-bit,
 * interleaved, no per-lump header. */
typedef struct { uint32_t rate; uint32_t channels; } pcminfo_t;

/* A third of a second at 48 kHz stereo. Large enough that the card is read in
 * useful chunks rather than in dribs, small enough to be affordable in RDRAM
 * beside a 2 MB zone and half a megabyte of textures. */
#define RING_BYTES  (64 * 1024)

/* Refill when the ring drops below this. Leaves most of a frame's slack before
 * the mixer could run dry. */
#define RING_LOW    (RING_BYTES / 2)

static bool      available;

/* Why music is or is not playing, for display on screen.
 *
 * The debug channel goes over USB, which needs a cable attached; on a console
 * sitting under a television the only way to report this is to draw it. */
static const char *const play_status = "play";
static const char *status = "init";

/* The card's root directory as the console reads it, for on-screen display.
 * A cable would carry this over USB; drawing it needs no extra hardware. */
char mus_listing[MUS_LIST_MAX][40];
int  mus_listing_count;
static FILE     *fp;
static pcminfo_t info;

/* The track currently playing, as a byte range in the file. */
static long      track_start, track_len, track_pos;
static char      wad_path[96];
static bool      playing;

/* Statically reserved, not allocated.
 *
 * This was malloc_uncached(64K), which draws on a pool separate from the
 * system heap -- and by the time music starts, a 2 MB zone and half a megabyte
 * of textures are already resident. An allocation failure there is not
 * graceful: it took the console down with a red flash and killed the USB
 * channel before the assertion could be read, which is the same shape as the
 * black screen the oversized zone caused earlier.
 *
 * Reserving it in .bss removes the failure entirely. It costs the same 64 KB
 * whether music plays or not, which on 8 MB is a fair trade for a boot that
 * cannot fail this way. UncachedAddr keeps the RSP's view coherent without the
 * allocator's involvement. */
static uint8_t   ring_store[RING_BYTES] __attribute__((aligned(16)));
static uint8_t  *ring;
static int       ring_head, ring_tail;    /* byte offsets; head==tail is empty */
static waveform_t wave;

static inline int ring_used(void)
{
    const int d = ring_head - ring_tail;
    return d >= 0 ? d : d + RING_BYTES;
}

/* --- where the tracks live ---------------------------------------------
 *
 * Two layouts, and this reads either one.
 *
 * The first attempt was an ordinary music WAD, and it did not work. A WAD
 * keeps its directory at the end, so reading it meant seeking to byte
 * 628,133,332 of a 599 MB file. libdragon builds FatFs with FF_USE_FASTSEEK
 * off, so f_lseek walks the cluster chain a link at a time -- about 19,000
 * links at this card's 32 KB clusters -- and through the flashcart's SD path
 * that walk did not merely take a long time, it failed outright with EIO.
 * Splitting the WAD into one file per track removed the seek entirely: each
 * track opened at offset 0 and was read forwards, the one access pattern this
 * stack handles well.
 *
 * DOOMMUS.WAD brings them back into one file, and three things have changed
 * that make it work where it did not before.
 *
 * The directory is written immediately after the twelve-byte header rather
 * than at the end. Nothing in the format requires the tail layout -- the
 * header carries the directory's offset, so it may sit anywhere -- and at the
 * front it is reached by reading forwards from byte zero, seeking nowhere.
 *
 * The audio is 24 kHz mono now rather than 48 kHz stereo, a quarter of the
 * bytes. And the tracks Doom reuses between episodes are stored once and
 * addressed by several directory entries, which removes another 100 MB.
 *
 * Together those put the whole file at 150 MB and the furthest track about
 * 4,700 clusters in, against the 19,000 that failed. Since FatFs continues a
 * forward seek from the current cluster and only restarts from the beginning
 * when seeking backwards, that bounds every seek here to a quarter of the walk
 * this driver could not survive.
 *
 * "Bounds it shorter than the thing that failed" is not the same as "works",
 * and this is a driver that hands up corrupt sectors as though they were
 * valid. So the per-file layout stays: it is used when DOOMMUS.WAD is absent,
 * and it is fallen back to permanently if reading the container ever fails.
 * The worst case is exactly the behaviour that shipped before it existed.
 */
/* Each cartridge has its own music file, since the track names differ
 * entirely between the games and the two sets together are far more than a
 * card wants to carry for one of them. */
#if D_DOOM2
#define MUS_WAD_NAME  "DOOM2MUS.WAD"
#else
#define MUS_WAD_NAME  "DOOMMUS.WAD"
#endif
#define MUS_MAX_LUMPS 64

typedef struct {
    char name[9];
    long pos;
    long size;
} muslump_t;

static muslump_t wad_dir[MUS_MAX_LUMPS];
static int       wad_count;        /* 0 means the per-file layout */
static FILE     *wad_fp;           /* held open for the session */
static bool      wad_failed;       /* container misbehaved; use files */
static bool      retry_from_file;  /* a container read died mid-track */
static char      cur_track[16];

/* WAD fields are little-endian and this CPU is not, so they are assembled
 * rather than cast. */
static inline uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Open DOOMMUS.WAD and read its directory, or report that there is none.
 *
 * Nothing here seeks unless the file was built with the directory at the tail,
 * in which case it is read the slow way and logged -- it will most likely
 * fail, and knowing that is better than a silent absence of music. */
static bool mus_wad_open(void)
{
    /* FAT stores short entries upper-case and needs either a long-filename
     * record or the NT case bits to present anything else; which of those this
     * FatFs build honours decides which spelling resolves. */
    static const char *const forms[] = {
#if D_DOOM2
        "sd:/" MUS_WAD_NAME,        "sd:/doom2mus.wad",      "sd:/DOOM2MUS.wad",
        "sd:/Doom/" MUS_WAD_NAME,   "sd:/Doom/doom2mus.wad",
        "sd:/DOOM/" MUS_WAD_NAME,   "sd:/doom/doom2mus.wad",
#else
        "sd:/" MUS_WAD_NAME,        "sd:/doommus.wad",       "sd:/DOOMMUS.wad",
        "sd:/Doom/" MUS_WAD_NAME,   "sd:/Doom/doommus.wad",
        "sd:/DOOM/" MUS_WAD_NAME,   "sd:/doom/doommus.wad",
#endif
    };

    for (int attempt = 0; attempt < 6 && !wad_fp; attempt++) {
        for (unsigned k = 0; k < sizeof forms / sizeof forms[0] && !wad_fp; k++) {
            wad_fp = fopen(forms[k], "rb");
            /* Kept so the cart preloader can reopen it unbuffered. */
            if (wad_fp) { strncpy(wad_path, forms[k], sizeof wad_path - 1);
                          wad_path[sizeof wad_path - 1] = 0; }
        }
        if (!wad_fp) wait_ms(60);
    }
    if (!wad_fp) return false;

    uint8_t hdr[12];
    if (fread(hdr, 1, sizeof hdr, wad_fp) != sizeof hdr ||
        (memcmp(hdr, "PWAD", 4) != 0 && memcmp(hdr, "IWAD", 4) != 0)) {
        debugf("music: " MUS_WAD_NAME " is not a WAD, using per-track files\n");
        fclose(wad_fp); wad_fp = NULL;
        return false;
    }

    const long count  = (long)rd32le(hdr + 4);
    const long dirofs = (long)rd32le(hdr + 8);
    if (count <= 0 || count > 4096 || dirofs < 12) {
        debugf("music: " MUS_WAD_NAME " directory looks wrong (%ld lumps at "
               "%ld)\n", count, dirofs);
        fclose(wad_fp); wad_fp = NULL;
        return false;
    }

    if (dirofs != 12) {
        debugf("music: " MUS_WAD_NAME " has its directory at %ld, not 12 -- "
               "repack it with tools/mkmuswad.py\n", dirofs);
        if (fseek(wad_fp, dirofs, SEEK_SET) != 0) {
            fclose(wad_fp); wad_fp = NULL;
            return false;
        }
    }

    const int want = (int)(count < MUS_MAX_LUMPS ? count : MUS_MAX_LUMPS);
    for (int i = 0; i < want; i++) {
        uint8_t ent[16];
        if (fread(ent, 1, sizeof ent, wad_fp) != sizeof ent) {
            debugf("music: " MUS_WAD_NAME " directory ended after %d lumps\n", i);
            break;
        }
        wad_dir[wad_count].pos  = (long)rd32le(ent);
        wad_dir[wad_count].size = (long)rd32le(ent + 4);
        memcpy(wad_dir[wad_count].name, ent + 8, 8);
        wad_dir[wad_count].name[8] = '\0';
        if (wad_dir[wad_count].size > 0) wad_count++;
    }

    if (wad_count == 0) {
        fclose(wad_fp); wad_fp = NULL;
        return false;
    }

    debugf("music: " MUS_WAD_NAME " open, %d tracks\n", wad_count);
    return true;
}

static const muslump_t *wad_find(const char *track)
{
    /* Lump names are eight characters and need no terminator in the file;
     * every name read above was given one, so this compares as a string. */
    char key[9];
    snprintf(key, sizeof key, "%s", track);
    for (int i = 0; i < wad_count; i++)
        if (strcmp(wad_dir[i].name, key) == 0) return &wad_dir[i];
    return NULL;
}

/* Position the container at a track. Retried, like every other read here,
 * and verified against ftell so a seek that quietly did nothing is caught. */
static bool wad_seek(const muslump_t *l)
{
    for (int attempt = 0; attempt < 4; attempt++) {
        if (fseek(wad_fp, l->pos, SEEK_SET) == 0 && ftell(wad_fp) == l->pos)
            return true;
        debugf("music: seek to %s at %ld failed (errno %d)\n",
               l->name, l->pos, errno);
        wait_ms(60);
        /* Start the next walk from a known place rather than from wherever
         * the failure left the handle. */
        fseek(wad_fp, 0, SEEK_SET);
    }
    return false;
}


/* Tracks live at the card root, not in a subdirectory.
 *
 * Subdirectory paths do not resolve through this driver: "sd:/MUSIC" fails to
 * open as a directory, and "sd:/MUSIC/PE1M1.pcm" returns ENOENT on every one
 * of twelve attempts -- consistently, not intermittently. Root paths work:
 * the card root enumerates, and a file at the root opens first time.
 *
 * So the tracks sit at the root. It is untidy next to forty other entries,
 * and it is the layout that works. */
#define MUS_DIR "sd:/"

/* The directories a per-track file may sit in, matching where the container
 * is looked for. Tried in order; the root is where the split tracks were
 * originally written. */
static const char *const mus_dirs[] = {
    "sd:/", "sd:/Doom/", "sd:/DOOM/", "sd:/doom/",
};

/* --- streaming -------------------------------------------------------- */

/* Top the ring up from the card, reading at most `max_bytes` this call.
 *
 * The cap is what keeps music from costing frames: unbounded, this filled the
 * ring back to full the moment it dipped below RING_LOW -- a single ~32 KB
 * blocking SD read through FatFs landing in one frame, a guaranteed stutter
 * every ~0.7 s. Consumption is only ~0.8 KB per 60 Hz frame, so a small
 * per-frame quantum refills five times faster than the stream drains while
 * bounding the worst frame to a few milliseconds of I/O.
 *
 * Called once a frame from the main loop, never from the mixer. Loops the
 * track rather than stopping at the end, which is what Doom does with level
 * music. */
/* Back to the first byte of the track, which is where looping happens.
 *
 * For a per-track file that is offset zero and costs one cluster lookup. In
 * the container it is a backwards seek, so FatFs restarts the walk from the
 * beginning of the file -- the same bounded cost as the seek that selected
 * the track in the first place. */
#if MUS_CART
/* The whole track, resident in cartridge SDRAM above the ROM.
 *
 * Streaming it off the card cost 1476 us EVERY FRAME, of which 933 us was the
 * CPU spinning in libcart's __sc_sync while the SC64 pulled sectors, 221 us
 * was the PI bus, and the rest fatfs. None of that is bandwidth we need: the
 * stream only wants ~48 KB/s.
 *
 * The SC64 can fill its OWN SDRAM from the card without the N64, the PI bus
 * or fatfs in the path at all, and measured on hardware it does so at
 * 21.4 MB/s -- twenty times the rate the fread path manages. So the largest
 * track in the set (E1M5, 7.9 MB) lands in ~370 ms, once, at level load.
 * Playback afterwards is a PI DMA out of cart RAM: ~220 us a frame, and no
 * card access at all. Looping costs nothing, since a rewind is just an index.
 *
 * Everything here is best-effort. Any failure -- no room, a cart whose
 * rd_cart does not work, a fragmented file we cannot map -- leaves
 * cart_resident false and the ordinary fread path carries the track. */
static bool track_rewind(void);

/* WELL clear of the ROM, and PROVEN clear before anything is written.
 *
 * This was 0x10C00000 -- 12 MB, chosen because Doom's ROM ends at 11.92 MB.
 * That is an 80 KB margin, which is not a margin, and it is catastrophically
 * wrong for Doom II: that ROM runs to 15.30 MB, so the preload wrote 4-8 MB
 * of music straight over the running game's own image. The symptom was a
 * black screen at the first level transition that never recovered, which is
 * exactly what overwriting your own code looks like.
 *
 * 32 MB instead. The SC64 carries 64 MB of SDRAM, so a 10 MB buffer here
 * ends at 42 MB with the whole ROM below it and room above. No ROM this
 * project can build comes near it.
 *
 * And the address is no longer TRUSTED: cart_probe writes a pattern and
 * reads it back before any track is loaded there. A cart that does not
 * answer -- a smaller SDRAM, a different flashcart, an extended-ROM layout
 * that reaches up here -- declines the preload and streams instead. The
 * music verification cannot catch this class of fault on its own: the track
 * would compare correct while something else entirely was being destroyed. */
#define MUS_CART_BASE   0x12000000u     /* 32 MB in */
#define MUS_CART_MAX    (10u * 1024u * 1024u)

static bool     cart_resident;
static uint32_t cart_skew;      /* track_start's offset within its sector */

/* Physical sector holding a given byte offset of the open track. */
/* Which physical sector backs a byte offset.
 *
 * MUST be asked through an UNBUFFERED handle. Through an ordinary stdio
 * stream a 1-byte fread fills stdio's block buffer, so fatfs reads several
 * sectors and its notion of "current sector" ends up on the LAST of them --
 * the mapping is then wrong by the buffer size, silently, and the track
 * plays from the wrong place. The first build of this preloader did exactly
 * that; its own verification caught it (MISMATCH at +0). */
static bool sector_of(FILE *h, long ofs, uint32_t *lba)
{
    unsigned char b;
    int64_t sect = 0;

    if (fseek(h, ofs, SEEK_SET) != 0) return false;
    if (fread(&b, 1, 1, h) != 1) return false;
    if (ioctl(fileno(h), IOFAT_GET_SECTOR, &sect) != 0) return false;
    *lba = (uint32_t)sect;
    return true;
}

/* Is MUS_CART_BASE real, writable memory that is not something else? */
static bool cart_probe(void)
{
    static int state;               /* 0 untested, 1 good, -1 unusable */
    static uint32_t pat[8] __attribute__((aligned(16)));
    static uint32_t chk[8] __attribute__((aligned(16)));

    if (state) return state > 0;
    for (int i = 0; i < 8; i++) pat[i] = 0xC0DE0000u + (uint32_t)i;
    data_cache_hit_writeback_invalidate(pat, sizeof pat);
    dma_write(pat, MUS_CART_BASE, sizeof pat);
    data_cache_hit_writeback_invalidate(chk, sizeof chk);
    dma_read(chk, MUS_CART_BASE, sizeof chk);
    for (int i = 0; i < 8; i++)
        if (chk[i] != pat[i]) {
            debugf("music: cart RAM at %08lx unusable (%08lx) -- streaming\n",
                   (unsigned long)MUS_CART_BASE, (unsigned long)chk[0]);
            state = -1;
            return false;
        }
    state = 1;
    return true;
}

static void cart_preload(void)
{
    cart_resident = false;
    if (track_len <= 0) return;   /* per-file path: length not measured */
    if (!cart_probe()) return;

    cart_skew = (uint32_t)(track_start & 511);
    /* An odd skew would put the PI address permanently out of parity with the
     * ring, which dma_read rejects. Nothing in the set does this today, and
     * streaming is the right answer if one ever does. */
    if (cart_skew & 1) {
        debugf("music: odd track skew %lu -- streaming\n",
               (unsigned long)cart_skew);
        return;
    }
    const uint32_t total = (uint32_t)track_len + cart_skew;
    const uint32_t nsect = (total + 511u) / 512u;
    if ((uint64_t)nsect * 512u > MUS_CART_MAX) {
        debugf("music: track too big for cart RAM (%lu KB)\n",
               (unsigned long)(nsect / 2));
        return;
    }

    int32_t csize = 0;                       /* cluster size, in sectors */
    if (ioctl(fileno(fp), IOFAT_GET_CLUSTER_SIZE, &csize) != 0 || csize <= 0) {
        debugf("music: no cluster size -- streaming\n");
        return;
    }

    /* A handle of our own, set unbuffered BEFORE any I/O on it -- which is
     * the only point at which setvbuf is defined -- so the sector queries
     * below reach fatfs one sector at a time. */
    if (!wad_path[0] || fp != wad_fp) {
        debugf("music: not the container -- streaming\n");
        return;
    }
    FILE *h = fopen(wad_path, "rb");
    if (!h) {
        debugf("music: probe handle open failed (errno %d) -- streaming\n", errno);
        return;
    }
    setvbuf(h, NULL, _IONBF, 0);

    const long base = track_start - (long)cart_skew;
    const uint32_t t0 = TICKS_READ();
    uint32_t done = 0, runs = 0;

    /* Walk the file a cluster at a time, merging physically contiguous
     * clusters into one rd_cart. A WAD written in one pass is usually a
     * handful of runs, so this is a few big transfers, not nsect small ones. */
    while (done < nsect) {
        uint32_t lba;
        if (!sector_of(h, base + (long)done * 512, &lba)) {
            debugf("music: sector map failed at +%lu -- streaming\n",
                   (unsigned long)done * 512u);
            fclose(h); return;
        }

        uint32_t run = 0;
        while (done + run < nsect) {
            const uint32_t want = nsect - (done + run) < (uint32_t)csize
                                ? nsect - (done + run) : (uint32_t)csize;
            if (run) {
                uint32_t nxt;
                if (!sector_of(h, base + (long)(done + run) * 512, &nxt))
                    { fclose(h); return; }
                if (nxt != lba + run) break;
            }
            run += want;
            if (want < (uint32_t)csize) break;
        }
        if (cart_card_rd_cart(MUS_CART_BASE + done * 512u, lba, run) != 0) {
            debugf("music: rd_cart failed at lba %lu\n", (unsigned long)lba);
            fclose(h);
            return;
        }
        done += run;
        runs++;
    }
    fclose(h);

    /* Verify before trusting it.
     *
     * sector_of() asks fatfs which sector backs a byte offset, but it asks
     * through STDIO -- and a 1-byte fread can be served from stdio's own
     * block buffer, or pull a multi-sector block, leaving fatfs's notion of
     * "current sector" pointing somewhere other than the offset we asked
     * about. That mismaps a run without failing, which is worse than an
     * error: the track plays, but out of the wrong part of the file.
     *
     * So compare what landed in cart RAM against what the file actually
     * says, at several points including both ends. A mismatch anywhere
     * drops us back to streaming, which is always correct. */
    {
        static uint8_t a_[512] __attribute__((aligned(16)));
        static uint8_t b_[512] __attribute__((aligned(16)));
        /* 512-aligned: libdragon's dma_read asserts that the RAM and PI
         * addresses share parity, and an odd offset against an aligned
         * buffer trips it. Sector-aligned probes keep both even. */
        const long probes[5] = { 0, (track_len / 4) & ~511L,
                                 (track_len / 2) & ~511L,
                                 (track_len - 1024) & ~511L,
                                 (track_len - 512) & ~511L };
        for (int i = 0; i < 5; i++) {
            const long ofs = probes[i] < 0 ? 0 : probes[i];
            if (ofs + 512 > track_len) continue;
            if (fseek(fp, track_start + ofs, SEEK_SET) != 0) return;
            if (fread(a_, 1, 512, fp) != 512) return;
            data_cache_hit_writeback_invalidate(b_, sizeof b_);
            dma_read(b_, MUS_CART_BASE + cart_skew + (uint32_t)ofs, 512);
            if (memcmp(a_, b_, 512) != 0) {
                debugf("music: cart preload MISMATCH at +%ld -- streaming\n", ofs);
                return;                 /* cart_resident stays false */
            }
        }
    }

    if (!track_rewind()) { debugf("music: rewind after preload failed\n"); return; }
    cart_resident = true;
    debugf("music: preloaded %lu KB into cart RAM, %lu runs, %lu ms, verified\n",
           (unsigned long)(nsect / 2), (unsigned long)runs,
           (unsigned long)(TICKS_TO_US(TICKS_SINCE(t0)) / 1000));
}
#endif /* MUS_CART */

static bool track_rewind(void)
{
    if (fseek(fp, track_start, SEEK_SET) != 0) return false;
    track_pos = 0;
    return true;
}

/* Give up on the container and let mus_update restart this track from its own
 * file. Only the first failure matters: after this the per-file layout is used
 * for the rest of the session. */
static void container_failed(const char *why)
{
    debugf("music: %s reading %s from " MUS_WAD_NAME " -- "
           "falling back to per-track files\n", why, cur_track);
    wad_failed = true;
    retry_from_file = true;
    playing = false;
}

static void refill(int max_bytes)
{
    if (!playing) return;

    int budget = max_bytes;
    while (ring_used() < RING_LOW && budget > 0) {
        /* Write into the contiguous space ahead of head, so a wrap costs two
         * passes rather than a split read. */
        int space = RING_BYTES - ring_head;
        const int used = ring_used();
        if (space > RING_BYTES - 1 - used) space = RING_BYTES - 1 - used;
        if (space > budget) space = budget;
        if (space <= 0) break;

        /* Inside the container the track has a known end and the next one
         * begins immediately after it, so the loop point is a byte count
         * rather than end-of-file. */
        if (track_len > 0) {
            const long left = track_len - track_pos;
            if (left <= 0) {
                if (!track_rewind()) { container_failed("rewind failed"); return; }
                continue;
            }
            if ((long)space > left) space = (int)left;
        }

        size_t got;
#if MUS_CART
        if (cart_resident) {
            /* No card, no fatfs: one PI DMA straight out of cart SDRAM. */
            dma_read(ring + ring_head,
                     MUS_CART_BASE + cart_skew + (uint32_t)track_pos,
                     (unsigned long)space);
            /* Invalidate the LINES the transfer touched, not the byte range.
             *
             * dma_read writes through the uncached alias, so nothing else
             * makes the CPU's view of the ring current -- but the cache op
             * asserts on a 16-byte aligned address AND a 16-byte multiple
             * length, and neither ring_head nor space is either: space is
             * capped by the ring wrap, the free space, the budget, and by
             * (track_len - track_pos), which is an arbitrary byte count at
             * the loop point. Handing it the raw range crashed on the
             * assertion mid-play, once a track first wrapped.
             *
             * Rounding outward is safe here: in this path the ring is only
             * ever written by DMA through the uncached alias, so the lines
             * on either side hold no dirty CPU writes to lose -- only clean
             * lines left by wave_read's memcpy out of it. */
            {
                const uint32_t lo = (uint32_t)(ring + ring_head) & ~15u;
                const uint32_t hi = ((uint32_t)(ring + ring_head)
                                     + (uint32_t)space + 15u) & ~15u;
                data_cache_hit_invalidate((void *)lo, hi - lo);
            }
            got = (size_t)space;
        } else
#endif
        got = fread(ring + ring_head, 1, (size_t)space, fp);

        /* Short read means end of track. Rewind and carry on -- a seek to
         * offset zero needs no chain walk, unlike a seek to the end. */
        if (got == 0) {
            /* Either end of track or a failed read -- rewinding distinguishes
             * them, since a rewind that also yields nothing is a dead stream
             * rather than a loop point. */
            if (!track_rewind()) {
                if (fp == wad_fp) { container_failed("rewind failed"); return; }
                playing = false;
                status = "seek-died";
                debugf("music: rewind failed (errno %d)\n", errno);
                return;
            }
            got = fread(ring + ring_head, 1, (size_t)space, fp);
            if (got == 0) {
                if (fp == wad_fp) { container_failed("read returned nothing"); return; }
                playing = false;
                status = "read-died";
                debugf("music: read returned nothing after rewind (errno %d)\n",
                       errno);
                return;
            }
        }

        track_pos += (long)got;
        ring_head = (ring_head + (int)got) % RING_BYTES;
        budget   -= (int)got;
    }
}

/* Per-frame read quantum. 4 KB/frame sustains 240 KB/s against the stream's
 * 48 KB/s drain, so the ring stays near the low-water mark while the worst
 * case a frame absorbs is one bounded read instead of half the ring. */
#define MUS_REFILL_QUANTUM 4096


/* Mixer callback. Copies from the ring and nothing else -- no I/O, no locking.
 * If the ring has run dry the gap is filled with silence rather than stalling:
 * a brief dropout is recoverable, a blocked audio update is not. */
static void wave_read(void *ctx, samplebuffer_t *sbuf, int wpos, int wlen,
                      bool seeking)
{
    (void)ctx; (void)wpos;

    if (seeking) { ring_tail = ring_head; }   /* discard, we cannot seek a stream */

    const int framesz = 2 * (int)info.channels;      /* 16-bit samples */
    int16_t *out = (int16_t *)samplebuffer_append(sbuf, wlen);
    int want = wlen * framesz;
    const int total = want;

    uint8_t *dst = (uint8_t *)out;
    while (want > 0) {
        int have = ring_used();
        if (have <= 0) { memset(dst, 0, (size_t)want); want = 0; break; }

        int run = RING_BYTES - ring_tail;
        if (run > have) run = have;
        if (run > want) run = want;

        memcpy(dst, ring + ring_tail, (size_t)run);
        ring_tail = (ring_tail + run) % RING_BYTES;
        dst  += run;
        want -= run;
    }

    /* The .pcm files store little-endian samples (numpy's default in
     * tools/split_music.py); the CPU and mixer are big-endian, and playing
     * the bytes as-is is loud white noise -- which is what the music was
     * from the first day it was audible. Swap here, once per sample on its
     * way out of the ring: ~a thousand swaps per 40 ms buffer, nothing
     * against the SD read that delivered them. The silence fill above is
     * zeros, which swap to themselves. */
    for (int i = 0; i < total / 2; i++)
        out[i] = (int16_t)__builtin_bswap16((uint16_t)out[i]);
}

/* --- interface -------------------------------------------------------- */

bool mus_init(const char *unused)
{
    (void)unused;

    /* Retry the card init itself.
     *
     * Everything about this card has needed retrying -- sector reads, the
     * deferred mount, directory entries -- and initialisation is no exception:
     * it succeeded on every boot until it did not, with no change to the code.
     * A few attempts with a pause between costs nothing on the boot where it
     * works first time. */
    bool sd = false;
    for (int attempt = 0; attempt < 6 && !sd; attempt++) {
        sd = debug_init_sdfs("sd:/", -1);
        if (!sd) wait_ms(100);
    }
    if (!sd) {
        status = "no-sd";
        debugf("music: no flashcart SD after 6 tries, running without music\n");
        return false;
    }

    /* Force the deferred mount before anything else touches the volume. The
     * first access after boot fails while later ones succeed, so this absorbs
     * that rather than letting it land on a real read. */
    /* Do not enumerate anything -- just open the file, and retry.
     *
     * The driver reads sectors and discards the CRC that comes with them
     * (libcart/cart.c, edx_card_rd_dram: "We ignore the CRC"), so a corrupt
     * read is handed up as though it were valid. That is what produced a root
     * listing of ten correctly-named entries on one boot and three entries
     * named '?' on the next, from the same card and the same code.
     *
     * A directory listing has no way to notice it is wrong. Opening a named
     * file does: it either succeeds or it does not, and retrying costs
     * nothing. So nothing here enumerates -- the filename is the lookup, and
     * a bad read simply means trying again.
     *
     * The same read also spins on a fixed 65536-iteration countdown before
     * aborting, which is why the first access after boot tends to fail while
     * later ones succeed. The delay between attempts covers that too. */
    bool opened = mus_wad_open();

    for (int attempt = 0; attempt < 12 && !opened; attempt++) {
        /* Try each spelling. FAT stores short entries in upper case and
         * relies on either a long-filename record or the NT case-flag bits to
         * present anything else; whichever of those this FatFs build honours
         * decides which of these resolves. DOOMMUS.wad opened from this same
         * directory, so the path is not the problem -- the name is. */
        static const char *forms[] = {
            "sd:/PE1M1.pcm", "sd:/PE1M1.PCM", "sd:/pe1m1.pcm", "sd:/PE1M1",
        };
        FILE *probe = NULL;
        int which = -1;
        for (unsigned k = 0; k < sizeof forms / sizeof forms[0] && !probe; k++) {
            probe = fopen(forms[k], "rb");
            if (probe) which = (int)k;
        }
        if (probe) debugf("music: opened as '%s'\n", forms[which]);
        if (probe) {
            fseek(probe, 0, SEEK_END);
            const long len = ftell(probe);
            fclose(probe);
            if (len > 0) {
                opened = true;
                debugf("music: opened on attempt %d, PE1M1 is %ld bytes\n",
                       attempt + 1, len);
                break;
            }
        }
        wait_ms(100);
    }

    if (!opened) {
        status = "no-music";
        debugf("music: neither sd:/" MUS_WAD_NAME " nor %sPE1M1.pcm would "
               "open (errno %d)\n", MUS_DIR, errno);
        return false;
    }

    /* Format is fixed by the converter that wrote these files.
     *
     * 24 kHz mono, halved from the source's 48 kHz stereo. That is 48 KB/s
     * sustained rather than 192 -- four times less traffic through a card that
     * has been corrupting reads -- and it shrinks the set from about a
     * gigabyte to a quarter of that. */
    info.rate     = 24000;
    info.channels = 1;

    ring = UncachedAddr(ring_store);

    available = true;
    status = "ready";
    debugf("music: streaming from %s\n", MUS_DIR);
    return true;
}

void mus_play_track(const char *track);

/* Options-menu music level, 0..120 as the menu scales it, against the
 * 0.6 base gain the stream was mixed for. */
static float mus_master = 1.0f;

void mus_apply_volume(void)
{
    mixer_ch_set_vol(MUS_CHANNEL, 0.6f * mus_master, 0.6f * mus_master);
}

void mus_set_volume(int volume)
{
    mus_master = volume <= 0 ? 0.0f : (volume >= 120 ? 1.0f : volume / 120.0f);
    mus_apply_volume();
}

void mus_play(int episode, int map)
{
    char name[16];
    snprintf(name, sizeof name, "PE%dM%d", episode, map);
    mus_play_track(name);
}

/* Play a track by bare name -- the level tracks are PE<e>M<m>, and the game
 * shell's states have their own: PINTRO (title), PINTER (intermission),
 * PVICTOR (finale text), PBUNNY (the episode 3 scroller). */
void mus_play_track(const char *track)
{
    if (!available) return;

    mus_stop();

    if (track != cur_track)
        snprintf(cur_track, sizeof cur_track, "%s", track);
    retry_from_file = false;

    if (fp && fp != wad_fp) fclose(fp);
    fp = NULL;

    /* The container first, when there is one and it has not let us down. */
    if (wad_count > 0 && !wad_failed) {
        const muslump_t *l = wad_find(track);
        if (l && wad_seek(l)) {
            fp          = wad_fp;
            track_start = l->pos;
            track_len   = l->size;
            track_pos   = 0;
            debugf("music: %s from " MUS_WAD_NAME " at %ld, %ld bytes\n",
                   l->name, l->pos, l->size);
        } else if (l) {
            wad_failed = true;      /* seeking is the thing that breaks */
        }
    }

    char path[64];
    snprintf(path, sizeof path, MUS_DIR "%s.pcm", track);

    /* Retry, as the probe does.
     *
     * The open itself is reliable; what is not is the data behind it. Opening
     * this file twelve times in a row succeeded every time, yet the length
     * came back as zero on six of the first seven attempts -- and on one of
     * them a different spelling of the name resolved. The driver discards each
     * sector's CRC, so a corrupt read arrives looking exactly like a good one.
     *
     * A plausible length is the check available here: the file is tens of
     * megabytes, so a zero or absurd value means the read was wrong, not that
     * the track is empty. */

    /* Open it, and never ask how long it is.
     *
     * fseek(END) walks the cluster chain to the last cluster -- about 1100 of
     * them for an 18 MB track at this volume's 16 KB clusters -- and long
     * chain walks are precisely what this driver cannot survive. It is the
     * same operation that made the WAD unreadable, and it is the only step
     * here that was still failing: the open itself succeeded on every attempt.
     *
     * The length was only ever used to know when to loop, and fread already
     * reports that by returning zero. So the file is opened and read forwards
     * and nothing seeks anywhere except back to the start, which costs one
     * cluster lookup rather than a thousand. */
    if (!fp) {
        for (int attempt = 0; attempt < 8 && !fp; attempt++) {
            for (unsigned d = 0;
                 d < sizeof mus_dirs / sizeof mus_dirs[0] && !fp; d++) {
                snprintf(path, sizeof path, "%s%s.pcm", mus_dirs[d], track);
                fp = fopen(path, "rb");
            }
            if (!fp) wait_ms(60);
        }
        if (!fp) {
            status = "no-track";
            debugf("music: %s would not open (errno %d)\n", path, errno);
            return;
        }
        debugf("music: %s open, streaming without seeking\n", path);

        track_start = 0;
        track_pos   = 0;
        track_len   = 0;          /* unknown, and deliberately not measured */
    }

    ring_head = ring_tail = 0;
    playing = true;
#if MUS_CART
    /* Before the priming refill below, so that read comes out of cart RAM
     * too. track_len is 0 on the per-file path (the length is deliberately
     * not measured there), and cart_preload declines that case, so those
     * tracks keep streaming exactly as before. */
    cart_preload();
#endif
    /* Prime a full low-water mark before the first mix. This is the one
     * unbounded read left, and it happens at level load where a long stall is
     * already expected -- never on the frame path. */
    refill(RING_LOW);

    wave = (waveform_t){
        .name      = "music",
        .bits      = 16,
        .channels  = (uint8_t)info.channels,
        .frequency = (float)info.rate,
        /* Unknown length, and therefore no loop.
         *
         * A stream has no length the mixer can know, so len is the maximum and
         * the mixer simply keeps asking for more. loop_len must stay zero:
         * libdragon asserts on a waveform that claims both an unknown length
         * and a loop, because there is no end to loop back from.
         *
         * The track still repeats -- refill() seeks back to the start when it
         * runs out, so the looping happens in the file rather than the mixer,
         * which is where it belongs when the data is streamed. */
        .len       = WAVEFORM_MAX_LEN,
        .loop_len  = 0,
        .read      = wave_read,
        .ctx       = NULL,
    };

    /* Raise this channel's ceiling before playing.
     *
     * Each channel carries its own frequency limit, fixed when the mixer is
     * initialised, and it governs how much sample buffer the channel reserves.
     * Raising the output rate alone does not lift it -- the assertion names
     * both numbers and the call that changes them. 16-bit stereo at 48 kHz is
     * what the tracks are; the buffer size is left to the mixer. */
    mixer_ch_set_limits(MUS_CHANNEL, 16, (float)info.rate, 0);
    mixer_ch_play(MUS_CHANNEL, &wave);
    {
        void mus_apply_volume(void);
        mus_apply_volume();
    }
    status = play_status;
}

void mus_stop(void)
{
    if (!available) return;
    if (playing) mixer_ch_stop(MUS_CHANNEL);
    playing = false;
    ring_head = ring_tail = 0;
}

void mus_update(void)
{
    if (!available) return;

    /* A container read died. Restart the same track from its own file, now
     * that wad_failed routes past the container -- done here rather than
     * inside refill so the restart is not re-entered from within itself. */
    if (retry_from_file) {
        retry_from_file = false;
        if (cur_track[0]) mus_play_track(cur_track);
        return;
    }

    /* The status set at mus_play only records that playback started. Keep it
     * honest about what is happening now: a stream that stopped feeding is not
     * still playing, and reporting otherwise sent me looking in the wrong
     * place. */
    if (playing) {
        {
#if D_HWSTAT
            { const uint32_t t_ = TICKS_READ();
              refill(MUS_REFILL_QUANTUM);
              r_mus_refill_us += TICKS_TO_US(TICKS_SINCE(t_)); }
#else
            refill(MUS_REFILL_QUANTUM);
#endif
        }
#if D_HWSTAT
        /* Telemetry builds only: a debugf over an undrained USB link can
         * stall for milliseconds, which a release build should never risk
         * for a status line. */
        static int report;
        if (++report % 120 == 0)
            debugf("music: buffered %d bytes\n", ring_used());
#endif
    } else if (status == play_status) {
        status = "stopped";
    }
}

const char *mus_status(void) { return status; }

int mus_ring_used(void) { return available ? ring_used() : -1; }
