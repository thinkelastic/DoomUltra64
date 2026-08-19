# DoomUltra64

**Doom and Doom II on the Nintendo 64**, drawn by the console's graphics chip
instead of in software. Both games play start to finish on real hardware.

Built on [libdragon](https://github.com/DragonMinded/libdragon) and tested on
an SC64 flashcart.

---

## Play it

**1. Unzip the latest release** to the root of your SD card. You get one
folder:

```
sd:/Doom/
  DoomUltra64.z64     the Doom ROM
  Doom2Ultra64.z64    the Doom II ROM
  saves/              your savegames
```

**2. Add your own game files.** Doom's data isn't included and can't be — copy
the `.WAD` from your own copy (Steam, GOG, the discs) into the same folder:

```
sd:/Doom/DOOM.WAD     for DoomUltra64.z64
sd:/Doom/DOOM2.WAD    for Doom2Ultra64.z64
```

Upper or lower case both work. Each ROM takes only its own game's file. If it's
missing, the game says so on screen and names what it wanted. No copy?
[Freedoom](https://freedoom.github.io/) is a free replacement.

**3. Boot a ROM** from your flashcart's menu.

**4. Set Options → Graphic Detail → High.** Do this first. On Low you lose the
reflections, the metal sheen and the higher resolution — if anything below
looks missing, this is almost always why.

**An Expansion Pak is required.** The busiest levels need more memory than a
stock N64 has.

---

## Controls

| Button | Action |
|---|---|
| Stick / D-pad | Move and turn |
| C-left / C-right | Strafe |
| C-up / C-down | Change weapon |
| Z | Fire |
| B | Open doors, use switches |
| A | Run |
| L or R | Automap |
| Start | Menu |

Turning too fast or too slow? **Options → Controller** sets the turn speed.

Saving needs no typing — the slot is offered the name of the level you're in,
and A accepts it.

---

## Music (optional)

The soundtrack ships separately because it's large and not ours to hand out.
Drop these beside the ROMs:

```
sd:/Doom/DOOMMUS.WAD      sd:/Doom/DOOM2MUS.WAD
```

Without them the game plays fine, just silent.

**Making your own.** You can build the music from the game files you already
own. You'll need `wildmidi` and a set of instrument sounds — the 2024
rereleases keep theirs in `Common.kpf`, which gives you that release's sound:

```sh
tools/mkmusfromiwad.py doom.wad  DOOMMUS.WAD  --gus /path/to/Common.kpf
tools/mkmusfromiwad.py doom2.wad DOOM2MUS.WAD --gus /path/to/Common.kpf
```

Any GUS-style instrument set works. Expect a few minutes and ~200 MB per game.

---

## Mods

Drop add-on `.wad` files into a `mods` folder beside the game:

```
sd:/Doom/mods/
  sigil.wad
  hellrevealed.wad
```

Then pick one in **Options → MODS**. The list shows what's on the card; choose
one and the game reloads with it, choose **NO MOD** to go back. Your choice is
remembered next time you switch on. You'll need to be at the title screen —
the game won't swap a mod out from under a level you're playing.

**What works:** new levels. That's most of what mods are, and megawads play
start to finish.

**What doesn't:** a mod's own textures, monsters and sounds. Those live in the
cartridge, baked in when the ROM was built, so a mod that brings custom art
shows the stock art instead — or a blank wall where a texture should be. It's
a limit of putting Doom on a cartridge, not something the mod got wrong.

If you can build (below), `./build.sh PWAD=sigil.wad` makes a ROM with that
mod's art baked in properly, and then everything works.

---

## What it adds

The N64's graphics chip has room to spare where Doom's original renderer
didn't, and this spends it on light.

- **Coloured dynamic lights** — muzzle flashes, fireballs and explosions light
  the walls around them; torches and lamps burn in their own colours; keys,
  armour and powerups glow, so a pickup is findable across a dark room.
- **Light in the air** — fireballs carry a glow, and holes in the sky throw
  visible shafts.
- **Reflections** — things and walls mirror in nukage, blood and lava, and on
  polished floors like teleporter pads. Metal doors catch the room's light.
- **Liquids that move** — pools drift and ripple, glow, and spill that light up
  the walls beside them.
- **Doom's real light curve** — dim rooms fall to black instead of a flat grey.
- **Smoother motion** — the game still ticks at Doom's 35 Hz, but the view,
  monsters, lifts and doors glide at full frame rate.
- **Rumble Pak** support, with a thump when you take a hit.

Any of these can be switched off at build time for the plain look.

---

## Build it yourself

Only Docker is needed — the image pins the compiler and builds libdragon.
The first build takes a few minutes.

```sh
./build.sh                        # -> doom.z64
./build.sh GAME=doom2             # -> doom2.z64
./build.sh EXTWAD=1               # reads the .WAD from the card
./build.sh PWAD=sigil.wad         # bakes a mod's art in, so all of it works
./build.sh release                # -> DoomUltra64-<version>.zip
./build.sh test                   # host-side checks
./run.sh                          # run in ares, screenshot to shots/
```

Point at your own data with `WAD=path/to/DOOM.WAD`. Use **ares**, not
mupen64plus — `run.sh` relays the console's debug output to your terminal.
`Makefile` documents every option.

## Licence

Doom's source is GPL-2 and this links against it, so this project is GPL-2 —
see `LICENSE`. Game data and music are not included and not redistributable.
