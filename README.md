# DoomN64

Doom and Doom II on the Nintendo 64, rendered by the RDP. Built on
[libdragon](https://github.com/DragonMinded/libdragon) and validated on real
hardware through an SC64 flashcart.

Doom's game code is vendored under `src/doom` and compiled nearly untouched —
the only additions are the marked frame-interpolation snapshots (`old*`
state) and the flag-gated smoke-trail spawn, which provably leaves the
demo-sync random sequence untouched. The software column and span rasteriser is gone; everything else that
touched a PC — video, sound, files, saves, input — was replaced.

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

## What the port adds

Effects the RDP's per-vertex shade makes nearly free. Each is removable with
a Makefile flag, and with the stack disabled the renderer is validated
pixel-identical to its plain output:

- **Colored dynamic lights** (`DYNLIGHT=0`). Muzzle flashes, projectiles and
  explosions light the walls, floors and things around them with distance
  falloff, folded into the shade the renderer already computes. Barrels
  flash on death; rocket impacts flare wider and hotter than their flight
  glow; lost souls carry their fire with them, brightening for the charge
  and popping like a small barrel when they die.
- **Smoke trails** (`SMOKETRAIL=0`). Rockets and the monsters' fireballs
  leave the revenant tracer's own smoke puffs behind them — fogged, lit by
  the projectile's glow, interpolated like every other thing, and invisible
  to demo sync (the spawn consumes no random stream at all, so the title
  attracts play back unchanged).
- **Reflections** (`REFLECT=0`). Things and walls over a glowing pool
  mirror in it, dimmed and tinted the liquid's own hue — and the polished
  surfaces do it too: teleporter pads, the temples' green marble, and the
  blue and silver tech floors, each tinted by its own art. The image is masked to
  pool pixels by the depth buffer alone — each vertex carries the depth at
  which its view ray crosses the water plane, so anything nearer wins and
  the mirror clips itself with zero polygon math.
- **Full-resolution liquids and floors** (`CI4FLATS=0`). Every flat whose
  art fits one 16-entry bank of the palette ships as full 64×64 CI4 — 20
  of the IWAD's 107, losslessly, both NUKAGE frames among them — instead
  of the 32×32 downsample TMEM
  used to force. The RDP's CI4 palette field selects the matching bank of
  the resident PLAYPAL, so palette flashes recolour them for free.
- **Emissive liquids.** Nukage, lava and blood glow with colour sampled from
  their own art and spill it up nearby walls at the waterline.
- **Liquid vapor** (`VAPOR=0`). A translucent noise layer drifts and churns
  over every glowing pool — green haze hugging nukage and slime, a darker
  smoke pall over lava.
- **Light-scaled diminishing** (`FOGSCALE=0`), and sprites that fog with the
  world. Darkness closes in faster in dark sectors, as vanilla's light
  ramp intended, and things sit in that falloff instead of floating
  unfogged in front of it.
- **Frame interpolation** (`INTERP=0`). The simulation stays 35 Hz; the
  picture does not. View, things, sector movers, the weapon bob and the
  vapor all glide at frame rate, and a paused or menu-held world presents
  its exact current state rather than lerping a frozen pair.

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
is in each save slot), `WIDE=1` (640x240, see below). `Makefile` documents the
rest.

## WIDE=1 — 640x240, and the one question it asks

`WIDE=1` doubles the horizontal sampling rate: 640x240, the same 240
scanlines, no interlace. The N64's 480-line modes are interlaced and flicker
on a CRT, and vertical is the expensive direction here for a structural
reason — projected Y is never clipped, only clamped against `RDP_Y_GUARD`, and
that clamp collapses quad edges. Near walls already project to y = -3360 at
320 wide; scaling Y as well would double every such value and drive far more
geometry into a clamp that is a known source of artefacts.

The flag is built to move **one** variable. Geometry, CPU submit cost and TMEM
uploads are all unchanged — upload count is set by which tiles are on screen,
not by how many pixels they cover, and LOD selection is deliberately pinned to
the vertical scale so both modes pick the same mips. `make test WIDE=1`
confirms it: identical upload and triangle counts to the 320 build, `oob=0`.
What doubles is fill rate, RDRAM bandwidth, and the framebuffers.

So an A/B of the `f` and `c` figures in the `HWSTAT` line answers exactly one
question: **is there spare RDP capacity at 320x240?** If `f` barely moves, the
RDP was idle and the pixels are nearly free. If it tracks the doubled fill,
the RDP was already the constraint.

**This measurement only works on hardware.** In ares the frame time is
entirely CPU submit — `f` and `c` come out within ~0.2 ms of each other in
both modes, so the emulator reports no RDP cost to compare. Use `./demo.sh`
on an SC64 and compare medians across the same route; ares can confirm the
mode renders, nothing more.

The UI, weapon and automap are authored in Doom's 320-wide frame and drawn at
`UI_XSCALE` — a pure horizontal double, since `SCREEN_H` never changes. A
640x240 pixel is half as wide as it is tall, so the doubled HUD comes out the
same apparent size and shape as the 320 one.

That costs the UI its COPY mode, for a reason worth recording: **COPY cannot
magnify.** It moves four texels per clock and steps S once per four-pixel
*group* — which is exactly why libdragon's RSP fixup multiplies DSDX by 4. At
1:1 that is four consecutive texels per four pixels and the blit is perfect;
ask for a 2x destination and the group boundaries quantise the sampling rather
than doubling each texel, and every one-pixel feature ghosts and gaps. On the
status bar it turns `BULL 40/200` into a row of split glyphs. So wide mode
draws the UI in standard mode at one pixel per clock. A few tens of thousands
of pixels a frame; 320 keeps COPY untouched.

What wide mode does *not* buy is sharper texture. LOD is pinned, so the extra
columns give cleaner geometry edges and nothing else. Unpinning it (`focal_y`
-> `focal_x` in `r_wall.c`) is physically correct at 640 and does sharpen near
walls, but it holds every surface one mip higher for twice the distance and
costs TMEM uploads — the resource the whole renderer is built to conserve.
Measure the cheap version first.

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
./build.sh release                              # -> DoomN64-<version>.zip
./build.sh EXTWAD=1                             # -> doom.z64
./build.sh GAME=doom2 EXTWAD=1                  # -> doom2.z64
./tools/mkmuswad.py DOOMMUS.wad  DOOMMUS.WAD    # from a source music WAD
./tools/mkmuswad.py DOOM2MUS.wad DOOM2MUS.WAD
```

`release` builds both card ROMs and packs the archive above — the `Doom/`
folder with an empty `saves/`, plus the music WADs when they sit in the
repository root. It refuses to pack a ROM with an IWAD baked in, and the zip
is named by `git describe`.

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
src/doom/           Doom's own game code, plus marked interpolation snapshots
n64.ld              repo-local link script; packs the per-seg render core
                    into one contiguous I-cache window (see r_wall.h)
tools/wad2n64.c     WAD -> CI8 + shared TLUT, at build time
tools/mkmuswad.py   music -> one WAD with the directory at the front
tools/mkpvs.c       precomputed visibility baker (off by default)
tests/              host harness and the libdragon shim
```

## Status and limits

Both games are playable end to end on hardware: menus, attract demos, level
flow, intermissions, finales, saves, automap, and the screen melt between
states.

- **The Expansion Pak is required.** The heaviest maps reach a ~1.7 MB texture
  working set (E2M2, measured in play — the load-time figure of 1.3 MB this
  used to quote was taken before the level's textures and its demand-loaded
  sprite frames had arrived, and understated the peak by a quarter). Fitting
  that into 4 MB alongside the zone heap and three framebuffers would force
  mid-level eviction — PI DMA during play, on a bus that manages about
  5 MB/s. The build checks at boot and halts with an explanation.
- **A savegame carries no record of which IWAD made it.** The two games use
  separate files (`doom_N.sav`, `doom2_N.sav`); loading one into the wrong game
  is undefined.
- **Precomputed visibility is off.** Sampled sets need widening to stay
  conservative, and widening them enough took E1M1 to 63% of the map visible on
  average — at which point it rejects little that the frustum and solid-segment
  clipping did not already reject. Re-running the baker's portal mode showed it
  both looser than sampling and unsound, so the honest path is a re-tuned
  sampled baker — parked unless hardware shows the BSP walk itself is the
  constraint.
- **CI4 is measured — and taken as quality, not speed.** As a performance
  lever it moves 1–3% on the wrong side of the pipeline, so it was parked
  until the Aug 2026 hardware session showed the RDP topping out at 60%
  busy. With fill affordable, the 64×64 aligned-run flats shipped
  (`CI4FLATS`, above); the remaining 48 flats span more than one palette
  bank and keep the downsample.

## Licence

Doom's source is GPL-2 and this links against it, so the project is GPL-2 —
the full text is in `LICENSE`. Game assets are not included and are not
redistributable.
