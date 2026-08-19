# DoomUltra64

**Doom and Doom II on the Nintendo 64**, drawn by the console's own graphics
chip rather than by software. Both games run start to finish on real hardware
— menus, demos, levels, intermissions, finales, saves, the automap and the
screen melt between them.

Built on [libdragon](https://github.com/DragonMinded/libdragon) and tested on
an SC64 flashcart.

---

## Play it

**1. Download the latest release** and unzip it to the root of your SD card.
You'll get one folder:

```
sd:/Doom/
  DoomUltra64.z64     the Doom ROM
  Doom2Ultra64.z64    the Doom II ROM
  saves/              your savegames, six slots per game
```

**2. Add your own game files.** Doom's data isn't included and can't be —
copy the `.WAD` from your own copy (Steam, GOG, the original discs) into the
same folder:

```
sd:/Doom/DOOM.WAD     for DoomUltra64.z64
sd:/Doom/DOOM2.WAD    for Doom2Ultra64.z64
```

Upper or lower case both work. Each ROM only takes its own game's file — the
art is baked into the ROM at build time, so Doom II can't run on Doom's data.
If the file is missing, the game says so on screen and names what it wanted.

Don't have a copy? [Freedoom](https://freedoom.github.io/) is a free,
drop-in replacement.

**3. Boot a ROM from your flashcart's menu.**

**4. Turn on Options → Graphic Detail → High.** Do this first. On Low you
lose the reflections, the metal sheen and the higher resolution — if
something below looks missing, this is almost always why.

**You'll need an Expansion Pak.** The busiest levels need more memory than a
stock N64 has, and the game checks at boot and tells you rather than
misbehaving later.

---

## Controls

| | |
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

Saving needs no typing. The slot is offered the name of the level you're
standing in — "E1M5: PHOBOS LAB" — and A accepts it.

---

## Music (optional)

The soundtrack ships separately because it's large and, like the game data,
not ours to hand out. Drop the music files beside the ROMs:

```
sd:/Doom/DOOMMUS.WAD      sd:/Doom/DOOM2MUS.WAD
```

Without them the game plays fine, just silent. Everything else on the card is
optional too — with no card at all, saves fall back to the cartridge.

### Making your own

You can build the music from the game files you already own. It needs
`wildmidi` installed, and a set of instrument sounds — the 2024 rereleases
include theirs inside `Common.kpf`, which gives you that release's sound:

```sh
tools/mkmusfromiwad.py doom.wad  DOOMMUS.WAD  --gus /path/to/Common.kpf
tools/mkmusfromiwad.py doom2.wad DOOM2MUS.WAD --gus /path/to/Common.kpf
```

Any GUS-style instrument set works, so you can pick the sound you like.
Expect a few minutes and around 200 MB per game.

---

## What it adds

The N64's graphics chip has spare capacity where Doom's original renderer
didn't, and this spends it on light:

**Coloured dynamic lights** — muzzle flashes, fireballs and explosions light
the walls and floors around them. Torches, lamps and candles burn in their own
colours. Keys, armour and the powerup spheres glow, which makes a pickup
findable across a dark room.

**Light in the air** — fireballs carry a glow, and openings to the sky throw
visible shafts shaped like the hole that casts them.

**Reflections** — things and walls mirror in nukage, blood and lava, tinted by
the liquid, and on polished floors like the teleporter pads and tech plating.
Metal doors and wall panels catch and move the room's light.

**Liquids that move** — pools drift, swell and ripple, glow with their own
colour, and spill that light up the walls they lap against.

**Doom's real light curve** — dim rooms fall to black instead of staying a
flat grey, so darkness reads as darkness.

**Smoother motion** — the simulation stays at Doom's 35 Hz, but the view,
monsters, lifts and doors all glide at full frame rate.

**Rumble Pak** support, and a thump when you take a hit.

Every one of these can be switched off at build time if you'd rather have the
plain look.

---

## Build it yourself

Only Docker is needed; the image pins the compiler and builds libdragon from
source. The first build takes a few minutes.

```sh
./build.sh                        # -> doom.z64
./build.sh GAME=doom2             # -> doom2.z64
./build.sh EXTWAD=1               # reads the .WAD from the card
./build.sh release                # -> DoomUltra64-<version>.zip
./build.sh test                   # host-side checks
./run.sh                          # run in ares, screenshot to shots/
```

Point at your own data with `WAD=path/to/DOOM.WAD`. Use **ares**, not
mupen64plus — `run.sh` relays the console's debug output to your terminal,
which is the most useful tool here.

`Makefile` documents every option, including `HWSTAT=1` for on-screen frame
timing and `DEBUG=1` for the graphics-command validator.

## Licence

Doom's source is GPL-2 and this links against it, so this project is GPL-2 —
see `LICENSE`. Game data and music are not included and are not
redistributable.
