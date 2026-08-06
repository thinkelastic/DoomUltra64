# DoomN64

Doom and Doom II on the Nintendo 64, rendered by the RDP. Built on
[libdragon](https://github.com/DragonMinded/libdragon) and validated on real
hardware through an SC64 flashcart.

Doom's game code is vendored under `src/doom` and compiled unmodified. The
software column and span rasteriser is gone; everything else that touched a PC
— video, sound, files, saves, input — was replaced.

## Why the RDP suits Doom

The RDP is fixed-function with a 2-stage colour combiner and **4 KB of texture
memory**. That rules out a shader-based port, but Doom's data model happens to
fit what the hardware does well:

- **Palettised art → CI8 + TLUT.** Doom is already 256-colour. One shared
  PLAYPAL TLUT serves every texture, so palette effects — damage red, pickups,
  invulnerability — are a 512-byte TLUT swap rather than a re-upload.
- **BSP traversal → free sort order.** Front-to-back ordering comes out of the
  BSP, so walls and flats need no depth buffer and the fill rate it would cost.
- **Light diminishing → vertex shade.** Sector light times distance falloff,
  modulated in the combiner. Deliberately *not* the RDP's fog unit: fog forces
  2-cycle mode, where the RDP samples TILE1 as well — and `rdpq_tex_upload_sub`
  can corrupt the next tile descriptor, so any renderer that uploads while
  drawing reads garbage. Shade keeps it in 1-cycle mode at roughly double the
  fill rate, and is exact for lighting that fades to black.
- **Texture composition → build time.** The console never assembles patches
  from `TEXTURE1`/`PNAMES`; `tools/wad2n64.c` bakes flat CI8 that DMAs straight
  off the cartridge.

## The constraint everything is built around

TMEM is 4 KB, and with a TLUT resident it is effectively 2 KB — **2048 CI8
texels, one 64×32 tile**. A 128×128 wall texture is eight of those.

So walls are drawn as a grid of tiles, and quads are batched and grouped by
tile rather than drawn as generated. Uploading per quad costs one texture load
each; grouping costs one per *distinct tile actually on screen* — a small fixed
number however much geometry samples it. `make test` checks the property by
subdividing one room into more and more segments and watching uploads stay
flat.

Reordering is only sound where geometry does not overlap in screen space, which
Doom's solid-segment clipping guarantees for walls. Sprites and masked
midtextures get their own ordered pass.

## Building

Requires only Docker; the image pins GCC 14.4 for `mips64-elf` and builds
libdragon from source. The first build takes a few minutes.

```sh
./build.sh                      # -> doom.z64
./build.sh GAME=doom2           # -> doom2.z64
./build.sh EXTWAD=1             # IWAD read from the card, not baked in
./build.sh test                 # host-side renderer and asset checks
./run.sh                        # run in ares, capture shots/doom.png
```

Point at your own IWAD with `WAD=path/to/DOOM.WAD`. Commercial IWADs are not
redistributable and `assets/` is gitignored;
[Freedoom](https://freedoom.github.io/) is a drop-in BSD-licensed substitute.

Use **ares** (paraLLEl-RDP), not mupen64plus. `run.sh` relays libdragon's debug
channel — asserts with backtraces, `debugf`, the RDP validator — to stdout,
which is the most useful debugging tool here.

Useful flags: `DEBUG=1` (RDP command validator), `HWSTAT=1` (on-screen frame
timing and pose), `DEMO=1` (walks a level unattended, for measurement),
`MENUTEST=n` (drives the menu at boot for capture), `SAVETEST=1` (reports what
is in each save slot). `Makefile` documents the rest.

## Installing on a flashcart

Developed against a SummerCart64. Any cart libdragon's SD stack drives should
work — the paths are not SC64-specific — but only the SC64 has been tested.

**1. Extract the release archive to the root of the SD card.** It creates a
single `Doom` folder:

```
sd:/Doom/
  Doom.z64  Doom2.z64        the ROMs
  DOOMMUS.WAD DOOM2MUS.WAD   music (optional, ~150 MB and ~216 MB)
  saves/                     savegames, six slots per game
```

**2. Add your own IWADs to `sd:/Doom/`.** They are not included and are not
redistributable:

```
sd:/Doom/DOOM.WAD            for Doom.z64
sd:/Doom/DOOM2.WAD           for Doom2.z64
```

Case does not matter — `DOOM.WAD`, `doom.wad` and `DOOM.wad` are all tried,
because which spelling a FAT volume presents depends on whether the long-name
record or the NT case bits win. Each ROM only accepts its own game's IWAD: the
textures and sprites are baked in at build time, so pointing Doom II at Doom's
WAD would resolve every name against the wrong art.

**3. Boot a ROM from the flashcart menu.** If the IWAD is missing the game says
so on screen and names the file it wanted, rather than failing obscurely.

Building the pieces yourself:

```sh
./build.sh EXTWAD=1                             # -> doom.z64
./build.sh GAME=doom2 EXTWAD=1                  # -> doom2.z64
./tools/mkmuswad.py DOOMMUS.wad  DOOMMUS.WAD    # from a source music WAD
./tools/mkmuswad.py DOOM2MUS.wad DOOM2MUS.WAD
```

`EXTWAD=1` leaves the IWAD out of the image, which is what makes the ROM read
it from the card; a default build bakes one in and needs no `.WAD` beside it.

Everything after the ROM is optional and degrades quietly: no card means no
music and saves fall back to the cartridge's SRAM, with the game otherwise
unaffected. `saves/` is a convenience — without it, savegames land beside the
ROM instead.

Music is 24 kHz mono PCM streamed through a ring buffer. It is packed into a
WAD whose **directory sits immediately after the header** rather than at the
end: a tail directory means seeking to the last byte of a 150 MB file, and
libdragon builds FatFs without fast seek, which this flashcart's SD path could
not survive.

## Controls

| | |
|---|---|
| Stick / D-pad | move and turn |
| C-left / C-right | strafe |
| C-up / C-down | cycle weapons (queued per press) |
| Z | fire |
| B | open doors, use switches |
| A | run |
| L or R | automap |
| Start | menu |

## Testing without hardware

`make test` compiles the *real* renderer against a mock RDP (`tests/shim/`) and
software-rasterises what it emits, enforcing invariants that are invisible at
runtime and fatal on console: every TMEM upload fits 2048 bytes, every sampled
texel lies inside the resident sub-rectangle, every vertex fits the RDP's s11.2
range, and the batch never silently drops a quad. It also checks the music WAD
layout the console-side parser depends on.

The harness is fast and catches a lot, but it only checks what it models. Of
the bugs that actually shipped in a ROM it caught none — each needed the
emulator or the console, and each is now recorded as an assertion or a comment
explaining the hardware's real semantics.

Hard-won ones worth knowing: the RDP does not participate in the VR4300's cache
coherency, so texture bytes still dirty in the data cache are invisible to it;
an unused vertex attribute must be `-1` in the triangle format, since zero
reads position floats as that attribute and trips the FPU on denormals; and the
emulator tolerates stale inherited depth state that the real RDP does not.

## Layout

```
src/                port layer: renderer, WAD streaming, arenas, platform
src/doom/           Doom's own game code, compiled unmodified
tools/wad2n64.c     WAD -> CI8 + shared TLUT, at build time
tools/mkmuswad.py   music -> one WAD with the directory at the front
tools/mkpvs.c       precomputed visibility baker (off by default)
tests/              host harness and the libdragon shim
```

## Status and limits

Both games are playable end to end on hardware: menus, attract demos, level
flow, intermissions, finales, saves, automap, and the screen melt between
states.

- **The Expansion Pak is required.** The heaviest maps reach a ~1.3 MB texture
  working set, and fitting that into 4 MB alongside the zone heap and three
  framebuffers would force mid-level eviction — PI DMA during play, on a bus
  that manages about 5 MB/s. The build checks at boot and halts with an
  explanation.
- **A savegame carries no record of which IWAD made it.** The two games use
  separate files (`doom_N.sav`, `doom2_N.sav`); loading one into the wrong game
  is undefined.
- **Precomputed visibility is off.** Sampled sets need widening to stay
  conservative, and widening them enough took E1M1 to 63% of the map visible on
  average — at which point it rejects little that the frustum and solid-segment
  clipping did not already reject. A set that is conservative by construction
  (2D anti-penumbra portal flow) is what would make it pay.
- CI4 assets with per-texture 16-colour palettes would double the effective
  TMEM budget and remain unexplored.

## Licence

Doom's source is GPL-2 and this links against it, so the project is GPL-2.
Game assets are not included and are not redistributable.
