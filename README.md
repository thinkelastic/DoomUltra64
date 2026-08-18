# DoomN64

Doom and Doom II on the Nintendo 64, rendered by the RDP. Built on
[libdragon](https://github.com/DragonMinded/libdragon) and validated on real
hardware through an SC64 flashcart.

Doom's game code is vendored under `src/doom` and compiled nearly untouched —
the only additions are the marked frame-interpolation snapshots (`old*`
state) and the flag-gated smoke-trail spawn, which provably leaves the
demo-sync random sequence untouched. The software column and span rasteriser is gone; everything else that
touched a PC — video, sound, files, saves, input — was replaced.

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
- **Standing lights.** The props Doom draws as light sources and lights
  nothing with: torches in all three colours, the techno and floor lamps,
  candelabra and candles. Colours come from the art, the radius scales
  with the prop, and they flicker on two sines well off each other's
  period so the sum never settles into a beat. Key cards, skulls and
  armour glow their own colour too (`KEYLIGHT=0`), which makes a pickup
  findable across a dark room. **Sixteen** slots, ranked by what each is
  worth from the eye, so a room full of torches still yields to a fireball.
  It was eight, and what held it there was the per-vertex query walking
  every light in the frame whether or not it could reach the surface being
  shaded — so a ninth light was charged to every lit vertex on screen. The
  walk already runs a sphere-vs-box test per surface to decide whether it is
  lit at all; keeping that answer as a bitmask instead of a boolean makes
  the per-vertex loop cost what a surface actually catches. On the standard
  attracts the registry peaks at ten, so eight was turning real lights away
  — a barrel cluster is eight on its own.
- **Halos and light shafts** (`HALO=0`). Fireballs, explosions and flames
  carry a glow in the air around them, not only on the surfaces they
  reach. Sky openings small enough to be a hole in a roof rather than open
  daylight throw a visible beam, and the beam is the shape of the opening:
  its outline extruded to the floor, so a long skylight reads as long and
  a corner view bends around the corner. The near silhouette tiles edge to
  edge, which is what keeps a lerp blender from compositing any pixel
  twice and banding the beam.
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
- **Moving liquids** (`LIQUIDFLOW=0`, `LIQUIDRIPPLE=0`, `REFLWOBBLE=0`).
  Vanilla animates a pool by swapping between a few flats on a timer, which
  reads as a flicker rather than as motion. The surface also drifts its
  texture continuously and swells on a travelling wave, and the
  reflections in it wobble on the same clock, so surface and image agree.
- **Light-scaled diminishing** (`FOGSCALE=0`), and sprites that fog with the
  world. Darkness closes in faster in dark sectors, as vanilla's light
  ramp intended, and things sit in that falloff instead of floating
  unfogged in front of it.
- **Glowing pickups.** The powerup spheres light the room and carry a glow
  in the air, each in its own colour — and the colour is the *measured*
  average of the sprite's own pixels through PLAYPAL rather than a guess, so
  the soulsphere's halo is the blue that is actually in it.
- **Rumble Pak** (`RUMBLE=0`). A thump when you take a hit — the same
  damage edge the red flash keys on — and concussion from explosions
  within 600 units, scaled by distance. The pak's motor is binary, so
  strength is an error-diffused duty cycle, the way N64 games always
  faked analog rumble.
- **Polished metal** (`ENVMAP=0`, `ENVAMT=`). Doors and metal wall plates
  catch and move the room's light instead of reading as flat painted panels
  — an environment-map sheen, not a mirror, so the texture underneath
  survives and it rides the wall's own light. It costs one quad and one
  32×32 tile per surface, because two properties collapse the usual
  per-vertex reflect to nothing: a door is a *vertical* plane, so its normal
  is constant, and the camera never pitches, so the reflected azimuth is
  affine in screen x — which a flat textured quad already interpolates.
  Twenty-five smooth textures take it; the rusted, grated and riveted
  variants are deliberately left matte.
- **Doom's real light curve** (`ZLIGHT=0`, `LIGHTCONTRAST=`, `LIGHTPIVOT=`,
  `LIGHTBOOST=`). Sixteen quantised light bands, a startmap row per band and
  a walk down the 32-row colormap with distance — so a dim sector runs *out*
  of colormap and floors at black instead of tapering, which is what makes a
  dark room read as dark. A contrast stretch about a low pivot separates the
  lit rooms from the unlit ones, and `LIGHTBOOST` sets the overall level
  afterwards. Things sit on the same curve as the world, so a monster in an
  unlit corner is genuinely hard to see.
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


**1. Extract the release archive to the root of the SD card.** It creates a
single `Doom` folder:

```
sd:/Doom/
  DoomUltra64.z64            the Doom ROM
  Doom2Ultra64.z64           the Doom II ROM
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

**4. Set Options → Graphic Detail to High.** This is not a cosmetic toggle: on
Low you lose the metal sheen, the pool reflections and the 320×480 mode
outright, because all three are gated on it. If a feature listed above appears
to be missing, check this first — it is the single most common reason.

Building the pieces yourself:

```sh
./build.sh release                              # -> DoomUltra64-<version>.zip
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

Saving needs no typing: the slot is offered the name of the level you are
standing in — "E1M5: PHOBOS LAB" — and A confirms it. That holds when you
overwrite an existing save too, so a slot is always labelled with where it was
actually made rather than where it was first used.

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
