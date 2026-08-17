# GAME=doom2 builds the Doom II cartridge instead of Doom's.
#
# The two share no assets. Different textures, flats, sprites, sounds and menu
# graphics, all baked into the image at build time, so one ROM cannot serve
# both. Each game keeps its own build and filesystem directory as well, so
# switching between them does not force a full rebuild of the other.
GAME ?= doom

ifeq ($(GAME),doom2)
BUILD_DIR = build-doom2
else
BUILD_DIR = build
endif

include $(N64_INST)/include/n64.mk

# n64.mk ships -ftrivial-auto-var-init=pattern, which memsets every
# uninitialised local array on every call: draw_one alone pattern-fills ~2.9 KB
# per invocation, ~90 times a frame, before doing any work. GCC honours the
# last occurrence, and project flags are appended after the include, so this
# wins. Measured in the disassembly: the 0xFE block fills disappear.
N64_CFLAGS += -ftrivial-auto-var-init=uninitialized

ifeq ($(GAME),doom2)
ROM           = doom2
FS            = filesystem-doom2
IWAD_DEFAULT  = assets/DOOM2.WAD
N64_ROM_TITLE = "Doom2Ultra64"
# Which IWAD and which music file this cartridge looks for on the card. The
# game is also identified from the IWAD's own lumps at boot, which is what
# drives Doom's logic; this only decides the filenames to open, and has to be
# fixed at build time because the assets baked in are already one game's.
N64_CFLAGS   += -DD_DOOM2=1
else
ROM           = doom
FS            = filesystem
IWAD_DEFAULT  = assets/DOOM.WAD
N64_ROM_TITLE = "DoomUltra64"
endif

# DEBUG=1 enables libdragon's RDP command validator and an on-screen control
# marker. Invalid RDP state renders as a blank screen with no other symptom,
# so this is the first thing to reach for when nothing appears.
DEBUG ?= 0
# Isolates stages of the shading pipeline; see R_PIPELINE in src/r_wall.c.
PIPELINE ?= 0
N64_CFLAGS += -DR_PIPELINE=$(PIPELINE)

# SHOW=1 draws walls only, SHOW=2 draws flats only. Use it to attribute
# missing geometry to a subsystem instead of guessing at it.
# NOTRI=1 keeps all CPU work but drops the triangle submissions, to
# attribute flush time between the math and the rdpq calls.
NOTRI ?= 0
N64_CFLAGS += -DR_NOTRI=$(NOTRI)

# TRIFAST=0 submits one rdpq_triangle per triangle instead of reusing the
# RSP's vertex slots across a quad or fan. Kept as the A/B and the fallback
# if a libdragon update ever moves the triangle protocol.
# FREEZE=n stops the simulation after n tics: an A/B build renders a
# motionless world, so a pixel diff isolates the renderer.
FREEZE ?= 0
N64_CFLAGS += -DD_FREEZE=$(FREEZE)

# TRIBENCH=1 runs the submission micro-benchmark at boot.
# PVS: 0 off, 1 prune with the baked visibility, 2 validate (traverse
# unpruned and report anything drawn from outside the set).
#
# Off by default, and the reason is worth recording. Sampled visibility is
# only safe once it is widened -- for subsector fringes the sampling never
# reaches, and for the edges BSP splits leave without segs -- and widening
# it enough to render pixel-identically took E1M1 from 43% to 63% of the
# map visible on average. At that density it rejects little the frustum and
# the solid-segment clipping were not already rejecting, and measured 3-5%
# off the traversal for ~1 MB of baked sets. A tight set that is
# conservative by construction rather than by dilation -- 2D anti-penumbra
# portal flow -- is what would make this pay; sampling plus slop does not.
PVS ?= 0
N64_CFLAGS += -DR_PVS=$(PVS)

# MENUTEST walks the menu at boot for capture: 1 opens the main menu,
# 2 goes on to the episode list, 3 starts a game. Starting a game is the
# only path no demo reaches, so it needs driving from here.
MENUTEST ?= 0
N64_CFLAGS += -DD_MENUTEST=$(MENUTEST)

TRIBENCH ?= 0
N64_CFLAGS += -DD_TRIBENCH=$(TRIBENCH)

TRIFAST ?= 1
N64_CFLAGS += -DR_TRIFAST=$(TRIFAST)

SHOW ?= 0
N64_CFLAGS += -DR_SHOW=$(SHOW)

# FLATDBG=1 paints each depth band of each flat a flat colour, so the
# polygons a floor or ceiling is really built from are visible on screen.
# For attributing a geometry artifact to a primitive instead of guessing.
FLATDBG ?= 0
N64_CFLAGS += -DR_FLATDBG=$(FLATDBG)

# TRIPROBE=1 checks the invariant banding exists to hold: that no emitted
# flat triangle spans more than FLAT_MAX_DEPTH_RATIO in depth, since rdpq
# normalises the texture coefficients against a triangle's nearest vertex
# and the far end of a deeper one warps. Reports the top-left quarter of
# the viewport separately. TRIPROBE=2 additionally paints that quad magenta.
# Off by default; costs a few flops per triangle.
TRIPROBE ?= 0
N64_CFLAGS += -DR_TRIPROBE=$(TRIPROBE)

# FLATNUDGE=<hundredths of a unit> is how far each subsector polygon's
# vertices are pushed outward from their own centroid, to close the
# T-junction hairlines that neighbouring BSP cells would otherwise leave.
# It buys that by making neighbours OVERLAP -- which is fine at different
# heights, where depth settles it, and is exactly the stray-coloured-line
# seam artifact where two COPLANAR floors of different art meet.
#
# DEFAULT 0 NOW, deliberately. With AA off the RDP writes a pixel only if
# its corner sample is inside the span (see 29ebf85), so the cure for
# seams is not overlap but IDENTICAL boundary chains on both sides --
# which the nudge destroys (it rotates each side's copy of the boundary
# by pushing shared vertices toward different centroids). The weld below,
# the shared band ladder and the canonical clip interpolation are what
# make the chains identical; the nudge would undo all three.
FLATNUDGE ?= 0
N64_CFLAGS += -DR_FLATNUDGE=$(FLATNUDGE)

# FLATWELD=1 welds T-junctions at load: where one cell's corner lands
# partway along a neighbour's edge, the neighbour gains that corner as a
# vertex -- copied verbatim, so both sides carry the same floats and
# interpolate the same boundary. Written to let FLATNUDGE go to 0; ON by
# default now that the corner-sample write rule is understood, because
# identical chains are the fix and a T-junction is one of the three ways
# the chains diverged (the others: the per-polygon band series and the
# traversal-direction interpolation, both fixed in r_flat.c).
FLATWELD ?= 1
N64_CFLAGS += -DR_FLATWELD=$(FLATWELD)

# SEGLINE=1 trims each BSP cell by its seg's LINEDEF, rather than by the
# line through the seg's own two vertices.
#
# They should be the same line. They are not, and the gap is not small: the
# node builder writes split vertices back to the VERTEXES lump as 16-bit
# INTEGERS, so a vertex it created lands up to ~0.7 units off the linedef it
# was cut from. On a long seg that is a slight tilt; on a short one it is an
# ANGLE, and the clip extends that line across the whole cell. Over
# DOOM.WAD, 289 segs on two-sided lines move their cell boundary more than
# half a unit at 128 units out and 181 move it more than a unit -- worst
# case 11 degrees, from a 3.6-unit seg (tools/segline.py measures this).
#
# Both subsectors either side of a two-sided line then derive their shared
# boundary from their OWN rounded seg, so the two land on different lines
# and leave a sliver -- the residual stair-area diagonals left after the
# chain-identity fix, and the reason FLATNUDGE used to be needed to paper
# over them. A linedef's endpoints are original map vertices and are shared
# by both sides, so clipping by it puts both boundaries on one identical
# line. The weld above then closes what is left, which is only along-line
# T-junctions. 0 restores the seg-endpoint clip.
SEGLINE ?= 1
N64_CFLAGS += -DR_SEGLINE=$(SEGLINE)

# VERTEXROUND=<tenths of a quarter-pixel> biases triangle vertices before
# they are floored to the RDP's s13.2 grid: +5 is half a step up, -5 half
# a step down, 0 is the shipped floor. Diagnostic. If two sides of a
# shared edge disagree by less than one step, which way the quantiser
# leans decides who claims the boundary pixel -- so a systematic bias
# shows as better one way and worse the other. +5 measured worse.
VERTEXROUND ?= 0
N64_CFLAGS += -DR_VERTEX_ROUND=$(VERTEXROUND)

# VIFILTER=0 disables the VI's coverage-based resample filter at 320. That
# filter is what smooths the 320-wide framebuffer up to the VI's 640-wide
# output, and it blends on polygon edges using the coverage the RDP wrote
# there -- which is a candidate for stray coloured pixels along a seam
# between two primitives. Off means harder, more aliased edges.
VIFILTER ?= 1
N64_CFLAGS += -DD_VIFILTER=$(VIFILTER)

# FLATZ=<tenths> sets the flats' z near constant, against the 4.0 walls
# and sprites use.
#
# It was 3.5, biasing flats BEHIND the walls so a wall won every shared
# edge -- chosen to stop floor texture wedging through a wall at a grazing
# angle. It overshot. Half a unit of depth per unit distance is a wide
# margin, and where two walls converge on a ceiling corner the wall's win
# stops being a hairline and becomes a notch several pixels across, with
# the wall showing where the ceiling should be. It moves with the view
# angle, because how far each surface's linearly-interpolated z sags from
# the true curve depends on how the primitive is foreshortened.
#
# At 4.3 the flats win instead, by a smaller margin than they were losing
# by. Confirmed on hardware: the corner notch is gone and nothing wedges
# through a wall.
FLATZ ?= 43
N64_CFLAGS += -DR_FLATZ=$(FLATZ)

# SPRZ=<tenths> sets the sprites' z near constant. It must exceed FLATZ or
# the floor clips the feet off anything standing on it, and every tenth
# above 40 is also a tenth ahead of the WALLS, where it buys a sprite
# showing through one it stands close behind. Default is FLATZ+2: enough to
# clear the floor's sag, as little as possible over the walls.
SPRZ ?= $(shell expr $(FLATZ) + 2)
N64_CFLAGS += -DR_SPRZ=$(SPRZ)

# SPRFADE=<world units> makes SPRZ's margin over the flats DECAY with depth,
# on the curve D0/(D0 + d). 0 keeps the flat constant SPRZ always was.
#
# The margin has to exist and only has to exist NEAR. What it protects is the
# contact with the floor a thing stands on; what it costs is that the same
# tenths sit ahead of the WALLS, and a near offset separates by dNEAR/d -- so
# the zone in which a sprite wrongly beats a wall is a fixed FRACTION of the
# depth, which is a GROWING number of world units. That is the whole reason
# distant enemies show through walls and near ones never do: at a flat SPRZ
# 45 the zone is 12.5% of depth, 12 units at depth 100 and 250 at 2000.
#
# Fading it collapses the far end without touching the near one. At the
# default 256, against FLATZ=43 / SPRZ=45:
#
#   depth      0    128    256    512   1000   2000
#   near     4.50   4.43   4.40   4.37   4.34   4.32
#   zone     12.5%  10.8%  10.0%   9.2%   8.5%   8.1%
#   units       0     14     26     47     85    162      (was 125 / 250)
#
# 7.5% IS THE FLOOR and this deliberately stops above it: sprites must stay
# ahead of the FLATS at every depth or the floor takes the feet, and for a
# big distant sprite the whole thing -- that is how pull-only lost barrel
# explosions. The flats lead the walls by (FLATZ/40 - 1), so no sprite bias
# can beat 7.5% while FLATZ is 43. Lowering FLATZ is the next lever, and a
# different contact: it answers to wall-versus-ceiling corners.
SPRFADE ?= 256
N64_CFLAGS += -DR_SPRFADE=$(SPRFADE)

# SPROCCL=1 occlusion-culls sprites against the BSP walk's own closed
# columns, instead of queueing them and letting the z buffer decide.
#
# This is the actual fix for a distant monster drawing through a wall, and
# the two levers above are not -- they only ever moved the range at which it
# starts. The reason is structural: a sprite must beat the FLATS or a floor
# clips the feet off what stands on it, the flats already lead the WALLS by
# (FLATZ/40 - 1), and a near offset separates surfaces by dNEAR/d -- so the
# margin that wins the near contact is a fixed FRACTION of depth ahead of
# the walls, and a fixed fraction is a GROWING number of world units. 12.5%
# is 12 units at depth 100 and 250 at 2000. No single constant is right at
# both ends, which is what the whole bias stack in r_flat.h is a record of.
#
# So the question is removed rather than tuned: a thing the walk has already
# walled off is never queued, and its depth never has to argue. The test is
# the same one the subtree cull uses (solid columns, then the loosest open
# window over them), it is only valid because the walk runs front to back,
# and it is conservative -- one open column under the sprite keeps it.
#
# 0 restores the z-only behaviour, which is what to set if a sprite ever
# disappears that should not: this culls on geometry the walk believes is
# closed, so a bug in the occlusion arrays shows up here as a missing
# monster rather than as a missing wall.
SPROCCL ?= 1
N64_CFLAGS += -DR_SPROCCL=$(SPROCCL)

# BARSCISSOR=1 stops the world passes rasterising under the status bar.
#
# The bar is opaque and reaches the bottom edge exactly -- 208+32 at 240 rows,
# 416+64 at 480 -- so every world pixel beneath it is overpainted before the
# frame is shown. Those are also the NEAREST rows on screen, where the floor
# fans are largest and the z buffer busiest: at 480 the band is 320x64 =
# 20,480 pixels of colour write plus a z read and a z write, discarded after
# the fact, every level frame. Roughly 0.33 ms of pure fill and ~123 KB of
# RDRAM traffic.
#
# This is the best value-per-risk item the 480i audit found, and it matters
# more at 480i than at 240p for the ordinary reason: the RDP's spare capacity
# was measured at 40% when the frame was half as tall.
#
# The clears stay FULL SCREEN on purpose -- they are cheap in FILL mode and
# they guarantee no uninitialised texel anywhere -- and the scissor is lifted
# before the automap, the bar and the menu, all of which draw into the band.
# It applies only on frames where the bar is actually drawn (D_BarBandDrawn:
# GS_LEVEL and gametic != 0), so the pre-first-tic frame is untouched.
BARSCISSOR ?= 1
N64_CFLAGS += -DR_BARSCISSOR=$(BARSCISSOR)

# BARCOPY=1 re-blits the composited status bar in COPY mode at any resolution.
#
# V_BeginUI refuses COPY unless the screen is exactly 320x240, because COPY
# cannot MAGNIFY: it moves four texels a clock and DSDX steps S once per
# four-pixel group. That reasoning is HORIZONTAL and it does not reach this
# blit, which magnifies on neither axis -- V_BarComposite has already resolved
# the 320x240 art into a real-pixel strip, and every rectangle is emitted at
# exactly the size of the sub-surface it samples. r_wipe.c has been doing the
# same 1:1 RGBA16 tile blit at 480 rows since the melt moved onto the RDP.
#
# 20,480 px at four texels a clock instead of one: ~0.33 ms to ~0.08, every
# in-level frame. 0 falls back to the shared bracket's mode.
#
# Only the 1:1 strip is claimed here. The SCALED half of the UI -- psprites
# and the composite itself -- stays on the standard path: whether the RDP's
# four-texel COPY group honours a fractional DtDy on silicon is unproven, and
# the failure would be a one-row phase shift in the glyphs. That one wants a
# photograph of a real screen, not an argument.
BARCOPY ?= 1
N64_CFLAGS += -DR_BARCOPY=$(BARCOPY)

# SKYCLAMP=1 stops the sky backdrop drawing below the horizon.
#
# Doom's sky wraps: 128 texture rows on a 200-row screen, repeated downward,
# because the repeats "sit below the horizon and are almost always behind
# world geometry". The sky pass runs LAST and depth-tested, so almost always
# is not good enough -- every one of those pixels costs a pipe cycle and a
# 16-bit z read to lose its compare. At 480 rows the repeat is a whole second
# band: ~55,000 px a frame plus its own TMEM uploads, drawn to be rejected.
#
# It cannot show, for a geometric reason rather than a statistical one. A sky
# ceiling at height ch projects to y = cy - (ch - camz)*focal_y/d, and this
# renderer has no pitch, so cy is SCREEN_H/2 always. With ch above the eye
# that second term is positive at every depth: y stays strictly above the
# horizon and only approaches it as d runs to infinity.
#
# The converse is real, which is why the walk accumulates a flag instead of
# this being unconditional: stand on a high ledge over an outdoor courtyard
# and its sky ceiling is BELOW your eye, so the sky legitimately appears under
# the horizon. Any such opening clears the flag and the clamp is abandoned for
# the whole frame. Two rows of slack are added over the exact bound, because
# the endpoints are floats and a black seam along the horizon would cost far
# more than 640 pixels.
#
# KNOW THE FAILURE MODE BEFORE TRUSTING THIS. It is the bad one: where the
# unclamped pass used to paint a wrapped (and arguably wrong-looking) sky over
# a pixel the world left uncovered, the clamped pass paints nothing and the
# black clear shows through. It does not CREATE coverage gaps -- a gap below
# the horizon means world geometry was already missing there, e.g. a flat
# region dropped at the polygon-pool ceiling -- but it does convert a subtle
# artifact into an obvious one. That is why the frame's colour clear is
# unconditional in the first place.
#
# So it is on, but it is not yet PROVEN: a POSEHASH abdiff over a sky-heavy
# route should be hash-identical, and a D_CLEARCOL probe build should leave no
# magenta pixel where sky belongs. SKYCLAMP=0 is the instant fallback if a
# black band appears along or below the horizon.
SKYCLAMP ?= 1
N64_CFLAGS += -DR_SKYCLAMP=$(SKYCLAMP)

# REFLWOB=<tenths of a world unit> is how far the water carries a reflection
# sideways at full throw. 0 stops the ripple without removing the machinery.
#
# WORLD units, deliberately, because the water is ONE SURFACE. The amplitude
# used to be a fraction of each image's own projected width -- defensible in
# that a distant reflection then wobbles by as much of itself as a near one
# does, but it made the displacement a property of the reflected OBJECT
# rather than of the liquid. A cacodemon and a shotgun shell floating side by
# side at the same depth below the same ripple were bent by different
# amounts, and the wall ghosts were worse: they passed half the projected
# span between their own endpoints, so the same water swung a 512-unit wall
# an order of magnitude further than a barrel beside it.
#
# The perspective behaviour the fraction was protecting survives for free:
# the throw is scaled by pixels-per-world-unit, which already falls off as
# 1/depth. 26 reproduces the old amplitude for a typical 40-texel monster,
# which is the case the 0.13 fraction was tuned against, so the look should
# read as it did -- except that everything in a pool now rides the same wave.
REFLWOB ?= 26
N64_CFLAGS += -DR_REFLWOB=$(REFLWOB)

# REFLDECAL=1 draws liquid reflections in the RDP's DECAL z-mode, coplanar
# with the pool, instead of biasing them in front of it.
#
# A ghost's z is the depth at which the eye ray meets the water, so it is
# coplanar with the pool by construction -- precisely what decal mode is for.
# The old scheme instead pushed it a constant NEARER than the pool so it would
# win the tie: R_REFL_Z_NEAR = FLAT_Z_NEAR + 1.0. That wins against the pool,
# and against everything else inside the same margin. Because a near offset
# separates surfaces by dNEAR/d, the margin covers a fixed FRACTION of depth,
# so a thing standing in the outer part of the distance to a pool was drawn
# over by its own reflection -- 11% of the pool's depth when FLATZ was 3.5,
# and 25% once FLATZ moved to 4.3, since the ghost was pinned to FLATZ + 1 and
# rode along.
#
# No bias can fix that, which is why this is a mode change rather than another
# constant: a one-sided nearer-than test cannot distinguish "the pool" from
# "just in front of the pool". Coplanarity can. With decal the ghost takes the
# flats' own near constant, lands on the pool, and is occluded by anything
# genuinely nearer.
#
# libdragon notes decal mode is not bulletproof against z-fighting. This is
# its best case -- ghost and pool are the same plane through the same near
# constant -- but REFLDECAL=0 restores the biased form, and that is the lever
# to reach for if a reflection flickers or drops out rather than merely
# sitting in the wrong order.
REFLDECAL ?= 1
N64_CFLAGS += -DR_REFLDECAL=$(REFLDECAL)

# CLEARCOL=r,g,b paints the framebuffer clear that colour instead of black.
# A gap probe: any pixel no primitive covered keeps it, so two builds with
# different values differ on exactly the holes and nowhere else.
CLEARCOL ?=
ifneq ($(CLEARCOL),)
N64_CFLAGS += -DD_CLEARCOL=$(CLEARCOL)
endif

# SEAMPROBE=1 is the seam microscope (src/d_seam.c): magenta clear, a scan
# of every finished frame for pixels nothing drew, and a one-shot dump of
# the real RDP triangle stream -- the words the RSP assembled -- the first
# time a stable sighting appears. Slow (validator on, blocking detach);
# diagnosis only. Stand at the seam and hold still.
SEAMPROBE ?= 0
N64_CFLAGS += -DD_SEAMPROBE=$(SEAMPROBE)

# TRICPU=1 computes triangle setup on the VR4300 instead of the RSP; see
# R_TRI_CPU in src/r_wall.h. Both paths feed the same RSP overlay, so this is
# a clean A/B for which unit the frame is actually waiting on.
TRICPU ?= 0
N64_CFLAGS += -DR_TRI_CPU=$(TRICPU)

# DEMO=1 walks the level unattended, so a run covers the frames that actually
# hurt instead of one vantage point. Works on hardware too.
# Sound effects. On by default; SOUND=0 builds silent.
# Bilinear texture filtering on walls and floors. BILINEAR=0 for the
# original point-sampled look.
BILINEAR ?= 1
N64_CFLAGS += -DR_BILINEAR=$(BILINEAR)

SOUND ?= 1
N64_CFLAGS += -DD_SOUND=$(SOUND)

LEVELTEST ?= 0
N64_CFLAGS += -DD_LEVELTEST=$(LEVELTEST)

# EXITAT=n finishes the level at tic n, so the intermission can be reached
# unattended -- the screen after a level, with that level's textures still
# resident. Pair with MAP=e,m to pick which level's working set is in the
# arena when the intermission asks for its own art.
EXITAT ?= 0
N64_CFLAGS += -DD_EXITAT=$(EXITAT)

# TEXARENA=<KB> shrinks the texture arena below this console's 2 MB, to
# reproduce a tighter budget than the machine in hand has. The
# intermission loads its own art while the finished level's is still
# resident, so what is left over decides whether it draws at all.
TEXARENA ?= 0
ifneq ($(TEXARENA),0)
N64_CFLAGS += -DMEM_TEXTURE_ARENA=$(shell expr $(TEXARENA) \* 1024)
endif

ROTTEST ?= 0
N64_CFLAGS += -DR_ROT_TEST=$(ROTTEST)

# TESTROOM=1 draws the pre-Doom bring-up room when no IWAD can be opened,
# instead of reporting that none was found. Only useful for renderer work.
TESTROOM ?= 0
N64_CFLAGS += -DD_TESTROOM=$(TESTROOM)

# SAVETEST=1 round-trips every savegame slot at boot and reports which
# backend took it. Overwrites real saves, so it is never the default.
SAVETEST ?= 0
N64_CFLAGS += -DD_SAVETEST=$(SAVETEST)

DEMO ?= 0
N64_CFLAGS += -DR_DEMO=$(DEMO)

# Frame interpolation: render between 35 Hz tics so motion tracks the frame
# rate. INTERP=0 snaps to tic state, for A/B comparison and bit-exactness.
INTERP ?= 1
N64_CFLAGS += -DD_INTERP=$(INTERP)

# WIDE=1 renders at 640x240 instead of 320x240: twice the horizontal sampling
# rate, the same 240 scanlines, no interlace (the N64's 480-line modes are
# interlaced and flicker on a CRT).
#
# Geometry and TMEM uploads are unchanged -- upload count is driven by which
# tiles are on screen, not by how many pixels they cover, and LOD selection is
# pinned to the vertical scale so it picks the same mips as the 320 build.
# Vertical projection is bit-identical between the two by design.
#
# CPU SUBMIT IS NOT UNCHANGED, and this is not a clean fill-rate probe. That
# was the original claim and hardware disproved it: over the same demo route,
# submit went 8.0 -> 14.4 ms and the BSP walk 3.4 -> 8.8 ms, because the
# occlusion arrays and their loops are indexed per SCREEN COLUMN. Frame time
# went 16.9 -> 21.3 ms median, vsync misses 52% -> 92%. Fill and bandwidth
# double too (~600 KB -> ~1.2 MB of buffers), but on a CPU-bound frame they
# are not what the number is showing you.
WIDE ?= 0
N64_CFLAGS += -DR_WIDE=$(WIDE)

# HIRES=1 selects 512x480 INTERLACED (480i) instead of the 240p modes,
# stretched to 4:3 with no borders.
#
# 512 rather than 640 on purpose. The costs above are indexed per screen
# COLUMN -- that is what took the demo route from 16.9 to 21.3 ms median at
# 640x240 -- so 512 pays four fifths of that CPU price, and 3.2x the fill
# rather than 4x. It also leaves the melt's saved frame enough heap to
# exist, which 640x480 does not. The rows are what the RDP feels: twice
# the fill, twice the z traffic. Expect the constraint to move from the
# CPU to the RDP.
#
# The buffer set still drops from three to two, because three 480 KB
# buffers plus the z buffer do not fit in what the arenas leave (main.c).
#
# 480i on a CRT interlace-flickers high-contrast horizontal detail -- the
# status bar is the worst case. That is the format, not the port.
HIRES ?= 0
N64_CFLAGS += -DR_HIRES=$(HIRES)

# HWSTAT=1 draws the frame-time/pose line at the top of the screen and emits
# the USB telemetry. Off for normal play; the debugging workflow described in
# the project notes turns it on.
HWSTAT ?= 0
N64_CFLAGS += -DD_HWSTAT=$(HWSTAT)

# TAG=<short string> stamps the HUD line so a photograph of the television says
# which build produced it.
#
# Learned the hard way: an A/B pair was measured on console and the screenshots
# came back indistinguishable, because two builds that differ only in a compiler
# flag draw byte-identical HUDs. The numbers were real and unusable.
TAG ?=
N64_CFLAGS += -DD_TAG=\"$(TAG)\"

# RSPQPROF=1 reports, with the periodic telemetry, where the RSP's time goes:
# running ucode, waiting for the CPU to hand it commands, or waiting for the
# RDP. That middle number is the one worth building for. The frame is CPU-bound
# in the submit path on hardware, and moving submit work onto the RSP only pays
# if the RSP is idle waiting for us -- if it is already saturated, an overlay
# moves the queue rather than the frame time.
#
# Requires ./build.sh RSPQPROF=1: the counters are sampled by libdragon's RSP
# microcode, so this needs a libdragon built with RSPQ_PROFILE=1 and therefore
# its own toolchain image (see the Dockerfile). Against the stock one the
# profiler is a set of empty stubs that would report a plausible-looking zero,
# so src/main.c refuses to compile instead.
#
# Not free: the sampling perturbs what it measures. Read the ratios, and take
# absolute frame times from an ordinary HWSTAT=1 build.
#
# DOES NOT BUILD against the pinned libdragon, and the reason is structural
# rather than a missing flag. Profiling adds resident code to the RSP queue,
# and every overlay has to fit in what is left of the RSP's 4 KB of IMEM
# alongside it -- rdpq's own microcode then overflows rom_imem by 112 bytes and
# libdragon will not link. Nothing tunable buys that back: RSPQ_DEBUG, the
# obvious candidate, gates no microcode at all (it is C-side only), and rdpq is
# the one overlay this renderer cannot do without. Dropping libdragon's H.264
# overlays clears the same overflow for THEM, which the Dockerfile does, but
# rdpq is still over on its own.
#
# So the flag is wired end to end and left switched off, against a libdragon
# upgrade making it usable. The compile-time check in src/main.c is what keeps
# that honest: with a non-profiling libdragon every rspq_profile_* call is an
# empty stub, so the build fails rather than reporting a confident zero.
# Measuring RSP occupancy on this libdragon needs a different mechanism.
RSPQPROF ?= 0
N64_CFLAGS += -DD_RSPQPROF=$(RSPQPROF)

# RSPIDLE=1 answers the same question RSPQPROF was meant to, from the CPU side,
# and does build. When rspq's queue runs dry the ucode executes `break` and the
# RSP HALTS (RSPQCmd_WaitNewInput in rsp_queue.inc), so SP_STATUS's halted bit
# is a true "nothing to do" signal rather than an inference. A timer interrupt
# samples it, and DP_STATUS alongside it, and the periodic telemetry reports
# both as a duty cycle. See src/rspidle.h.
#
# Separate from HWSTAT on purpose: this adds a 4 kHz interrupt, and HWSTAT
# builds are where the demo-route frame-time medians come from. Never read
# absolute frame times from a build with this on -- read the ratios.
RSPIDLE ?= 0
N64_CFLAGS += -DD_RSPIDLE=$(RSPIDLE)

# R_FASTFLOOR=0 goes back to calling floorf in the vertex conversion.
#
# floorf is a libm CALL here, not an instruction: GCC's MIPS backend has no
# pattern that lowers it to the VR4300's floor.w.s, and n64.mk's -ftrapping-math
# means -fno-trapping-math does not unlock one. Every vertex needs four (x, y,
# and both halves of the s16.16 pair), so a wall quad was paying sixteen calls
# in the innermost submit loop -- the path the frame is CPU-bound in.
#
# The replacement is trunc plus a one-step correction, which is floorf's exact
# result for every finite input in int32 range. Output is bit-identical, so
# this is purely a speed switch; the flag exists to A/B it on hardware, which
# is the only place the difference can be read.
R_FASTFLOOR ?= 1
N64_CFLAGS += -DR_FASTFLOOR=$(R_FASTFLOOR)

# DYNLIGHT=1 adds moving point lights -- muzzle flash, fireballs, rockets.
#
# Doom's lighting model has no way to express these: a sector has one light
# level and that is the whole of it. This adds a per-vertex term on top, which
# costs no TMEM and no combiner stage -- the RDP is already interpolating a
# shade across every surface, so the only change is what value each vertex is
# handed. That is what makes it affordable when higher-resolution art is not.
#
# Brightness only, not colour: the batch vertex carries one shade float which
# the flush fans out to r=g=b, so hue would mean widening the batch or carrying
# a per-quad tint. See src/r_light.h.
#
# ON by default as of the sludge work: emissive flats are the reason. A muzzle
# flash is two tics and easy to miss, but a nukage pool is always there, and
# lighting it (and the walls it laps against) changes how a room reads rather
# than adding a flicker.
#
# It is not free -- the frame is CPU-bound in the submit path, and this adds a
# per-corner query on walls and a per-vertex one on flats. DYNLIGHT=0 still
# compiles every query to a constant zero and returns the shade expressions to
# exactly what they were, byte for byte, if that cost ever needs reclaiming.
DYNLIGHT ?= 1
N64_CFLAGS += -DD_DYNLIGHT=$(DYNLIGHT)

# How many dynamic lights can be live at once. Was fixed at 8, and what held
# it there was the per-vertex query looping over every light in the FRAME --
# so a ninth light was charged to every lit vertex on screen and the registry
# evicted by priority instead. LIGHTSEL below removes that coupling, which is
# what makes raising this affordable: a barrel cluster is eight lights on its
# own and used to evict the fireball that lit it.
#
# The reach mask is a uint16_t, so 16 is the ceiling without widening it.
LIGHTMAX ?= 16
N64_CFLAGS += -DR_LIGHTMAX=$(LIGHTMAX)

# LIGHTSEL=0 restores the per-vertex query over the whole light list, which is
# what flats did before the reach mask was kept. Retained as the A/B and the
# fallback, in the TRIFAST spirit: the two are supposed to be BIT-IDENTICAL at
# equal LIGHTMAX, because a light the mask omits is one whose falloff term was
# going to be <= 0 at every vertex of that polygon. That identity is a claim
# about the sphere-vs-box test, and this flag is how it stays checkable:
#
#   ./abdiff.sh LIGHTSEL=0 LIGHTMAX=8 -- LIGHTSEL=1 LIGHTMAX=8   # must PASS
#   ./abdiff.sh LIGHTMAX=8 -- LIGHTMAX=16                        # must DIFFER
#
# The first proves the selection changes no pixel; the second proves the
# raised cap actually reaches the screen rather than being dead configuration.
LIGHTSEL ?= 1
N64_CFLAGS += -DR_LIGHTSEL=$(LIGHTSEL)

# FOGSCALE=1 scales the light-diminishing range by sector brightness, the
# way vanilla's diminishing tables do: bright sectors see far, dim sectors
# fall off close. Light-255 sectors are bit-identical to FOGSCALE=0 (the
# scale factor is exactly 1.0 there), so bright areas cannot regress; flats
# gain the same falloff walls have (they previously had none), which is the
# one deliberate look change. FOGSCALE=0 restores the fixed 512..3500 ramp.
FOGSCALE ?= 1
N64_CFLAGS += -DR_FOGSCALE=$(FOGSCALE)

# NEARLIGHT=1 adds the NEAR half of that same diminishing, which the
# renderer never had: vanilla drags what is close to the eye toward full
# art brightness whatever the sector light says, and only the far half was
# modelled here. Without it every surface inside R_FOG_NEAR sits at a flat
# lightlevel/255 with no distance term, so a dim room's ceiling a few paces
# overhead is painted one dead value and reads as a dark shape rather than
# a lit plane. See r_nearlight in src/r_wall.h for the term and the numbers.
# Light-255 surfaces are bit-identical to NEARLIGHT=0 (the sum clamps at
# 1.0 there); NEARLIGHT=0 restores the old flat near field for A/B.
# ZLIGHT=1 replaces the additive near-light term with Doom's ACTUAL zlight
# curve: 16 quantised light bands, a startmap row per band, and a walk down
# the 32-row colormap with distance. NEARLIGHT restores the missing gradient
# but keeps this port's lightlevel/255 base, which is a different shape from
# vanilla's startmap -- ZLIGHT reproduces the banding, the way a bright
# sector is full-bright at every distance, and the way a dim one floors at
# black instead of tapering. See r_zlight in r_wall.h. 0 keeps the additive
# form, and is bit-identical to it.
ZLIGHT ?= 0
N64_CFLAGS += -DR_ZLIGHT=$(ZLIGHT)

NEARLIGHT ?= 1
N64_CFLAGS += -DR_NEARLIGHT=$(NEARLIGHT)

# NODEZCHECK=1 proves the incremental node-height refresh bit-identical to
# the full tree walk EVERY frame (snapshot, rerun reference, compare) -- a
# debug build for door/lift routes; costs more than the work it checks.
NODEZCHECK ?= 0
N64_CFLAGS += -DD_NODEZ_CHECK=$(NODEZCHECK)

# QUANT=0 restores the float wall batch and flush-time conversion. The
# default stores RSP-format integers at push (where depth and its
# reciprocal are live) and the flush is a pure copy of command words --
# same stream by construction, which `./abdiff.sh QUANT=0 -- QUANT=1`
# proves over a whole route. Ignored by TRIFAST=0 and the host build.
QUANT ?= 1
N64_CFLAGS += -DR_TRI_QUANT=$(QUANT)

# FUSESPLIT=0 restores the two-pass depth-band peel (near clip + far clip
# over the same polygon). The default classifies each edge once and emits
# both sides in one pass, with each side's boundary predicates and
# interpolants kept verbatim -- `./abdiff.sh FUSESPLIT=0 -- FUSESPLIT=1`
# proves route identity.
FUSESPLIT ?= 1
N64_CFLAGS += -DR_FUSESPLIT=$(FUSESPLIT)

# FASTNARROW=1 steps clip_narrow's per-column window edges by constant
# deltas instead of re-deriving the lerp each column (~14 cycles/column).
# The iterated adds round differently from the recomputed lerp -- by
# ~5e-3 px over a full span, inside the window's one-pixel outward
# rounding -- so this shipped opt-in until soaked. Soaked Aug 11 2026:
# `abdiff.sh -- FASTNARROW=1` over both demo routes (e1m1 144/144,
# e2m2 143/143 fingerprints, per-side clean builds) -- every frame
# hashes identical, so the drift never crosses a pixel edge in
# practice. Every skip/reseed boundary re-derives exactly.
FASTNARROW ?= 1
N64_CFLAGS += -DR_FASTNARROW=$(FASTNARROW)

# VAPOR=1 restores the liquid-vapor pass: the translucent noise layer that
# puts a green haze over nukage and slime and a smoke pall over lava. One
# z-tested fan per visible pool, drawn after all opaque passes.
#
# OFF by default. The haze read as fog sitting on the surface rather than
# rising off it, and it washed out the pools' own colour and the
# reflections they carry -- which are the reason the liquid surfaces are
# interesting to look at in the first place. Nothing else depends on it:
# the D_GLOW_* liquid families it consumed are also what drive the glow,
# the reflections and the ripple, so they stay. At 0 the whole translation
# unit leaves the build and r_vapor.h's no-op stubs take over, so this
# costs nothing to keep around.
VAPOR ?= 0
N64_CFLAGS += -DR_VAPOR=$(VAPOR)

# LIQUIDFLOW=0 stops the liquids sliding. Vanilla nukage does not move --
# it swaps between three stills every eight tics, which reads as a
# slideshow. Drifting the texture coordinates turns the same frames into a
# current, and it advances at DISPLAY rate (the sub-tic fraction rides
# along) where the frame cycle steps at 4.4 Hz. One subtraction per
# emissive flat per frame; nothing else moves.
LIQUIDFLOW ?= 1
N64_CFLAGS += -DR_LIQUIDFLOW=$(LIQUIDFLOW)

# LIQUIDRIPPLE=0 stops the swell that rides on top of the drift: each
# vertex of an emissive flat is displaced by a sine of its WORLD position,
# so neighbouring subsectors agree exactly along shared edges. Long
# wavelength, few texels of amplitude -- enough to undulate, small enough
# that a T-junction's unavoidable disagreement stays under a pixel.
LIQUIDRIPPLE ?= 1
N64_CFLAGS += -DR_LIQUIDRIPPLE=$(LIQUIDRIPPLE)

# SMOKETRAIL=1 gives rockets and the monsters' fireballs a smoke trail: the
# same MT_SMOKE puffs the revenant's tracer already leaves, spawned one tic
# behind the shell -- every other tic for rockets, every fourth for the
# slower fireballs (the same ~40-unit spacing). Game-visible decoration,
# sync-invisible by construction: the spawn consumes no random stream at
# all (jitter is hashed from simulation state), so recorded demos play back
# unchanged -- proven route-identical with abdiff over the walker route.
SMOKETRAIL ?= 1
N64_CFLAGS += -DD_SMOKETRAIL=$(SMOKETRAIL)

# HALO=0 removes light in the air: the glow around every dynamic light,
# and the shafts that fall through small sky openings. Both are one
# primitive -- a view-facing translucent billboard, z-tested against the
# world, added rather than mixed -- and both cost fill rather than CPU,
# which the Aug 12 hardware session priced as affordable (rdp_busy 41%
# median). Shafts are rejected over open sky; see r_halo.h.
HALO ?= 1
N64_CFLAGS += -DR_HALO=$(HALO)

# SHAFT=0 keeps the halos around lights but drops the beams that fall
# through sky openings. They are separable because they are different
# claims: a halo says "this light is bright", which needs no geometry to
# be right, while a beam says "the hole above you is THERE", which does --
# and a beam whose placement reads wrong is worse than no beam. Requires
# HALO=1; with HALO=0 both are gone anyway.
SHAFT ?= 1
N64_CFLAGS += -DR_SHAFT=$(SHAFT)

# SHAFTAMT=<hundredths> is how strongly a beam reads. The two failure modes
# are close together: too low and it is not light at all, too high and it
# stops being light in the AIR and becomes a solid pale slab hanging in the
# room. 0.42 was the slab, 0.21 was the correction and undershot.
SHAFTAMT ?= 32
N64_CFLAGS += -DR_SHAFTAMT=$(SHAFTAMT)

# SHAFTHUE=<hundredths> is how much of the SKY's colour a beam takes. The
# light falling through a hole in the roof comes from this level's backdrop,
# so r_sky averages the rows of it that show above the horizon and the beam
# is tinted with that hue: neutral under E1's grey sky, gold in Doom II's
# city, red under Inferno. 0 restores plain white light, 100 takes the hue
# saturated -- which is too far, because the blender mixes TOWARD the beam's
# colour and a pure red one drives green and blue out of everything it
# crosses. 55 is where the halos settled the same trade-off.
SHAFTHUE ?= 55
N64_CFLAGS += -DR_SHAFTHUE=$(SHAFTHUE)

# REFLECT=1 draws mirror images of things standing over glowing liquid --
# a monster wading through nukage reflects in it, dimmed and tinted the
# pool's own hue. The image is masked to pool pixels by the z-buffer alone
# (each vertex carries the depth at which its view ray crosses the water
# plane; anything nearer already in the buffer wins), so the cost is one
# blended quad per nearby thing and zero clipping math. The RDP-side fill
# is affordable per the Aug 12 hardware session: rdp_busy tops out at 60%.
# Capture gates predate this flag -- pin REFLECT=0 when comparing against
# ref3 captures.
REFLECT ?= 1

# Smallest mirror image worth queueing, in screen pixels of ghost HEIGHT.
# 0 restores the old behaviour (every thing over the pool, however distant).
#
# The pass gated on reach and on being off-screen, never on size, so a thing
# across the room queued a full ghost to draw a smudge. What makes this worth
# a flag rather than a constant: the hardware run that motivated it says the
# reflect submit is 2-3.3 ms on precisely the frames that overrun a 16.67 ms
# field, while the median frame has CPU to spare -- so this trims the tail,
# and how hard to trim it is a look decision, not a perf one. Raise it until
# the reflections you lose are ones you could not see anyway.
REFLMINPX ?= 10
N64_CFLAGS += -DR_REFLMINPX=$(REFLMINPX)
N64_CFLAGS += -DR_REFLECT=$(REFLECT)

# REFLWOBBLE=0 makes the reflections perfect mirrors again. On moving
# water they should not be: the image is displaced sideways by an amount
# that grows with depth below the surface and varies with time, so it
# shears in bands like a real reflection on a swell. Amplitude is a
# fraction of the image's own width, so distance does not change how
# much of itself wobbles, and it runs off the same clock as the liquid's
# ripple so surface and reflection agree.
REFLWOBBLE ?= 1
N64_CFLAGS += -DR_REFLWOBBLE=$(REFLWOBBLE)

# ENVMAP=1 gives polished metal walls -- DOOR1, DOOR3, BIGDOOR2/4, SHAWN2,
# METAL1 -- an environment-map sheen, so a door catches and moves the room's
# light instead of reading as a flat painted panel.
#
# This REPLACES the door mirror, which reflected the things standing in front
# of a door across its own plane. That never shipped: its ghost-quad clip
# returned the empty set at poses where it plainly should not, and a true
# mirror cannot show the room BEHIND the viewer without walking the BSP again
# from a mirrored camera -- a second world render on a frame already CPU-bound.
#
# The sheen costs one quad and one 32x32 tile per door, because two properties
# of this renderer collapse the usual per-vertex reflect to nothing: a door is
# a VERTICAL plane, so its normal is constant and reflection leaves the view
# ray's vertical component alone; and the camera never PITCHES, so screen x is
# view azimuth and screen y is view elevation, both by a fixed scale. The
# reflected azimuth is 2*theta_normal - theta_view, which over a door's width
# is affine in screen x -- and affine in screen space is what a flat textured
# quad already interpolates. Hence no perspective correction and no per-pixel
# work; see the note at the top of src/r_env.c.
#
# The map itself is synthesised at first use rather than baked: what a door
# reflects in a Doom room is not a scene, it is a bright band where the walls
# meet the ceiling lights. No cartridge space, no wad2n64 change.
#
# ENVAMT=<hundredths> sets how strongly it reads; the door's own texture has
# to survive underneath, so this is a sheen and not a mirror. It also rides
# the wall's light, so a door in a dark room stops being flat without glowing.
ENVMAP ?= 1
N64_CFLAGS += -DR_ENVMAP=$(ENVMAP)
ENVAMT ?= 18
N64_CFLAGS += -DR_ENVAMT=$(ENVAMT)
# ENVKNEE=<0..255> is the sector light level below which a door gets NO sheen.
# A knee rather than a slope: polished metal catches a room that has something
# to catch, and a dim room does not give a weak highlight so much as none --
# a faint sheen in the dark reads as a rendering fault, not as a material.
# 48 of 255 is roughly "anything but a near-black room".
ENVKNEE ?= 48
N64_CFLAGS += -DR_ENVKNEE=$(ENVKNEE)
# ENVFULL=<0..255> is where it reaches full strength. Ramping all the way to
# fullbright left every ordinary room at about a third and the sheen never
# really arrived; stopping at 176 means "off in the dark, on everywhere else"
# with a ramp wide enough not to be an edge you can walk across.
ENVFULL ?= 176
N64_CFLAGS += -DR_ENVFULL=$(ENVFULL)

# SPRSHINE=1 gives roughly cylindrical props -- barrels, tech pillars -- a
# sliding specular highlight, so they stop reading as flat cutouts.
#
# NOT an environment map, and it cannot be one: a billboard has no surface
# normal to reflect. Every sprite faces the camera, so a real env lookup would
# return the same answer for every barrel everywhere and read as a wash
# sliding with the view. What a cylinder does have is a KNOWN shape --
# horizontal position across the sprite IS angle around the barrel -- so the
# highlight of a fixed room light lands at u = sin((theta_light - theta_view)/2)
# and slides across the sprite as the player walks around it.
#
# Honest for a cylinder and wrong for anything else, which is why the walk
# arms it per thing TYPE rather than the pass applying it to every sprite.
#
# Drawn as a second pass over the same tiles: RGB from SHADE, alpha from
# TEX0 * SHADE, so the sprite's own cutout masks it and a transparent texel
# contributes nothing. It rides the thing's own lighting, so a barrel in a
# dark room stops being flat without glowing. SPRSHINEAMT is hundredths at
# the band's centre.
SPRSHINE ?= 1
N64_CFLAGS += -DR_SPRSHINE=$(SPRSHINE)
SPRSHINEAMT ?= 26
N64_CFLAGS += -DR_SPRSHINEAMT=$(SPRSHINEAMT)

# CI4FLATS=1 ships every flat whose 4096 indices share one high nibble as
# full-resolution 64x64 CI4 (59 of the IWAD's 107, losslessly) instead of
# the 32x32 CI8 downsample: 2048 bytes is exactly the TMEM budget beside
# the TLUT, and the RDP's CI4 palette field selects the matching 16-entry
# bank of the resident PLAYPAL -- no new palette machinery, flash-correct
# by construction. Costs RDP fill only (uploads double in bytes), which
# the Aug 12 session priced as affordable. The flag reaches wad2n64
# through the assets rule; fold refs predate it, so pin CI4FLATS=0 when
# comparing against ref3 captures.
CI4FLATS ?= 1
N64_CFLAGS += -DD_CI4FLATS=$(CI4FLATS)

# RUMBLE=1 drives a Rumble Pak in controller 1: a thump when the player
# takes a hit (the same damagecount edge the red flash keys on) and
# concussion from explosions within 600 units, scaled by distance (the
# dynamic-light walk already knows every blast). The pak's motor is
# binary, so strength is duty cycle -- an error-diffused on/off per frame.
# Output-only: reads the sim, writes a peripheral; it cannot move a pixel
# or desync a demo. No pak, no effect.
RUMBLE ?= 1
N64_CFLAGS += -DD_RUMBLE=$(RUMBLE)

# KEYLIGHT=0 removes the glow from key cards and skulls. They are the
# dimmest entries in the light registry on purpose: eight slots with
# weakest-first eviction means a fireball can always take a key's slot in
# a firefight, and the small radius pools light around the key instead of
# lighting the room.
KEYLIGHT ?= 1
N64_CFLAGS += -DD_KEYLIGHT=$(KEYLIGHT)

# FORCERUMBLE=1 is a DIAGNOSTIC: drive the motor without the accessory
# handshake, self-pulsing every four seconds -- for third-party pads with
# built-in rumble that never identify as a Rumble Pak. Never a default:
# the motor command writes into pak address space, and a Controller Pak
# sitting in a real controller would be corrupted by it.
FORCERUMBLE ?= 0
N64_CFLAGS += -DD_FORCERUMBLE=$(FORCERUMBLE)

# ZCHECK=1 proves the z-only node refresh reproduces the full pass exactly:
# it runs both on every refresh and asserts every field of every node matches.
# Costs far more than the work it is checking -- for validation only.
ZCHECK ?= 0
N64_CFLAGS += -DD_ZREFRESH_CHECK=$(ZCHECK)

# POSEHASH=n fingerprints one frame every n tics and prints the hash, so two
# builds can be compared over a whole route instead of at one vantage point.
# ./abdiff.sh drives it. See src/posehash.h for why the ROM hashes its own
# framebuffer rather than the host capturing the screen.
#
# INTERP=0 is not a suggestion here: with interpolation on, where the camera
# sits inside a tic depends on when the frame happened to render, so the same
# tic in two builds is not the same picture and every hash differs for reasons
# unrelated to whatever is being tested. Enforced rather than documented,
# because the failure looks exactly like a real regression.
POSEHASH ?= 0
N64_CFLAGS += -DD_POSEHASH=$(POSEHASH)
ifneq ($(POSEHASH),0)
ifneq ($(INTERP),0)
$(error POSEHASH needs INTERP=0: with interpolation on, the pose within a tic \
depends on render timing, so two builds hash different pictures at the same \
tic and every frame reads as a regression)
endif
endif
# The source is added to `src` after the list is assigned, further down: a
# `src +=` up here would be silently discarded by the `src =` that follows.

# O3=1 compiles the three hottest renderer files -O3 instead of the global
# -O2: an A/B experiment for hardware demo-route medians only. The VR4300's
# 16 KB I-cache can turn the extra inlining into a net loss, so this is
# never the default; measure before adopting.
ifeq ($(O3),1)
$(BUILD_DIR)/src/r_wall.o $(BUILD_DIR)/src/r_flat.o $(BUILD_DIR)/src/r_bsp.o $(BUILD_DIR)/src/r_tri.o $(BUILD_DIR)/src/r_ssdata.o: N64_CFLAGS += -O3
endif

# R_BATCH_CDE=0 goes back to plain stores for wall-batch appends. The default
# issues the VR4300's Create Dirty Exclusive cache op on each of a quad's
# seven D-cache lines before filling it: a store miss normally FETCHES the
# line from RDRAM first (~40+ cycles) only to overwrite all of it; CDE
# allocates the line dirty with no fetch. Safe because every pushed quad
# fully overwrites its record. See batch_push in r_wall.c.
R_BATCH_CDE ?= 1
N64_CFLAGS += -DR_BATCH_CDE=$(R_BATCH_CDE)

# Cartridge save RAM: 128 KB of SRAM, which the SC64 menu persists to a .sav
# beside the ROM. Two 64 KB savegame slots live in it; see i_n64.c.
N64_ROM_SAVETYPE = sram1m

# VIEWLOCK=x,y,z,milliangle pins the camera to a pose from the hwstat
# telemetry, for reproducing an on-console rendering fault in the emulator.
ifdef VIEWLOCK
N64_CFLAGS += -DR_VIEWLOCK=$(VIEWLOCK)
endif
# MAP=e,m boots straight into that level (with VIEWLOCK or for testing).
ifdef MAP
N64_CFLAGS += -DR_BOOT_MAP=$(MAP)
endif
ifdef FLATDUMP
N64_CFLAGS += -DR_FLATDUMP=1
endif
ifdef NOCLOSURE
N64_CFLAGS += -DR_NOCLOSURE=1
endif
ifeq ($(DEBUG),1)
N64_CFLAGS += -DDEBUG_RDP=1
endif

# Point WAD= at your own IWAD. Commercial IWADs are not redistributable and
# assets/ is gitignored; Freedoom works as a shareable substitute.
# The registered IWAD: all three episodes, and the later status bar with
# real key slots and the BULL/SHEL/RCKT/CELL ammo labels that st_stuff's
# widget positions were written against. The shareware doom1.wad here is a
# 1994 revision whose bar reads "KEYS" and a second "AMMO", which the v1.9
# game code lays its widgets over wrongly -- same era mismatch as its
# mixed-case texture names.
WAD ?= $(IWAD_DEFAULT)

# The directory n64.mk packs into the DFS image. It defaults to "filesystem",
# which is not a prerequisite of the rule and so is not derived from the assets
# -- setting FS alone left the Doom II ROM packed with Doom's assets and Doom's
# IWAD, a 21 MB image that built without complaint and was simply the wrong
# game.
N64_MKDFS_ROOT = $(FS)

# Every wall texture is composed on the host and baked into the ROM: the N64
# never assembles patches at runtime. The full set is ~4.6 MB, nothing against
# a cartridge, and the runtime loads only the ones a level actually names.

src = src/main.c src/mem.c src/dt64.c src/r_wall.c src/wad.c src/p_level.c src/r_bsp.c src/r_flat.c src/r_sky.c src/r_sprite.c src/r_am.c src/r_tri.c src/r_pvs.c src/r_wipe.c src/r_env.c

# Only compiled when asked for, so an ordinary ROM carries none of it. See the
# RSPIDLE comment above.
ifeq ($(RSPIDLE),1)
src += src/rspidle.c
endif
ifneq ($(POSEHASH),0)
src += src/posehash.c
endif
ifeq ($(DYNLIGHT),1)
src += src/r_light.c
endif
ifeq ($(VAPOR),1)
src += src/r_vapor.c
endif
ifeq ($(RUMBLE),1)
src += src/d_rumble.c
endif
ifeq ($(HALO),1)
src += src/r_halo.c
endif
ifeq ($(SEAMPROBE),1)
src += src/d_seam.c
endif

# floorf/ceilf as instructions instead of libm calls, for the WHOLE link.
#
# Always compiled in, and deliberately not behind a flag: it replaces the
# library's definitions rather than adding a call site, so there is nothing to
# switch off at the point of use. Project objects are resolved before -lm is
# searched, so the archive members are never pulled in and every caller
# rebinds -- including prebuilt libdragon, whose rdpq triangle setup pays two
# floorf per vertex and which no edit of this project could otherwise reach.
# Same linker behaviour the patched flashcart driver relies on.
src += src/r_fastmath.c
# -fno-builtin or GCC recognises the body as floorf and emits a call to floorf,
# which is that function. It compiles cleanly and recurses until the stack is
# gone, so this line is load-bearing.
$(BUILD_DIR)/src/r_fastmath.o: N64_CFLAGS += -fno-builtin

# Doom's own game code, vendored under src/doom and compiled unmodified. The
# platform services it expects (lumps, zone, I_Error) are supplied by the
# w_n64/i_n64 shims rather than by porting d_main.c and its dependencies.
#
# This makes the project GPL-2: Doom's source is GPL and this links against it.
src += src/w_n64.c src/i_n64.c src/v_draw.c src/d_ui.c src/i_sound_n64.c src/mus_n64.c src/doom/sounds.c src/d_bridge.c src/d_verify.c src/r_ssdata.c
src += src/doom/m_fixed.c src/doom/m_bbox.c src/doom/tables.c src/doom/z_zone.c
src += src/doom/p_setup.c

# The thinker list and the actors that run on it. info.c is the state and
# mobjinfo tables -- 4700 lines of data that replace the hand-written
# doomednum-to-sprite table the renderer used to carry.
src += src/doom/info.c src/doom/p_tick.c src/doom/p_mobj.c
src += src/doom/p_maputl.c src/doom/m_random.c

# Collision, line of sight and the AI. p_map.c replaces the stub P_TryMove
# that has been refusing every move since Stage 2, and p_enemy.c supplies the
# A_* actions the state table names.
src += src/doom/p_map.c src/doom/p_sight.c src/doom/p_enemy.c

# Weapons, damage and pickups. p_pspr.c supplies the last of the A_* actions,
# so src/d_actions.c disappears with it.
src += src/doom/p_pspr.c src/doom/p_inter.c src/doom/d_items.c src/doom/p_user.c

# Sector effects: doors, lifts, floors, ceilings, lights, switches, teleports.
# These replace our p_spec.c, and with it the last reason to keep p_level.c's
# duplicate geometry alive.
src += src/doom/p_spec.c src/doom/p_doors.c src/doom/p_plats.c
src += src/doom/p_floor.c src/doom/p_ceilng.c src/doom/p_lights.c
src += src/doom/p_switch.c src/doom/p_telept.c

# Menus, status bar and messages, taken from upstream Chocolate Doom rather
# than the openfpgaOS fork the game layer came from.
#
# Only these UI files carried that fork's platform layer: m_menu.c reached into
# of_syscall.h and friends, which are inline assembly against RISC-V registers.
# Comparing the two showed hu_lib.c identical to upstream and st_lib.c six
# lines apart, while m_menu.c's 364 extra lines were all Analogizer refresh
# modes and CRT options -- FPGA concerns with no meaning here. So upstream
# loses nothing this target wants.
src += src/doom/m_misc.c
# The game shell: gamestate machine, demo playback, intermission and finale.
src += src/doom/g_game.c src/doom/wi_stuff.c src/doom/f_finale.c src/doom/m_cheat.c src/doom/p_saveg.c src/doom/am_map.c
src += src/doom/m_menu.c src/doom/st_stuff.c src/doom/st_lib.c
src += src/doom/hu_stuff.c src/doom/hu_lib.c

N64_CFLAGS += -I src -I src/doom

# Link against the libdragon built from toolchain/libdragon, not the prebuilt
# copy in the toolchain image.
#
# src/libcart/cart.c is patched there: the EverDrive read path verifies what it
# reads rather than discarding each sector's CRC. A -L searched before the
# default library directory is enough to prefer it, and needs no write access
# to the image.
# Link the patched flashcart driver as an explicit object.
#
# src/libcart/cart.c in toolchain/libdragon is modified: the EverDrive read
# path verifies what it reads instead of discarding each sector's CRC. Getting
# the ROM to use it turned out not to be a matter of library search order --
# -L never reached the link at all, and the stock 560-byte function kept
# appearing in the ELF while ours is 248.
#
# An explicitly listed object wins outright: the linker resolves it first and
# then has no unresolved symbol left that would pull cart.o out of libdragon.a.
# The check that matters is the symbol size in the built ELF, not that the
# flags look right.
# Assets produced by one wad2n64 run, funnelled through a stamp file.
assets_conv = $(FS)/playpal.tlut

# EXTWAD=1 leaves the IWAD out of the cartridge image: the runtime reads it
# from the flashcart's SD card instead (sd:/DOOM.WAD). The ROM then carries
# no id Software lump data, only code and the art baked from whatever WAD
# built it -- and the player supplies, or swaps, their own.
EXTWAD ?= 0

# The IWAD ships in the ROM itself. It is far too large for RDRAM (14 MB
# against 8), so lumps are streamed off the cartridge on demand. Kept out of
# the stamp rule because it is a plain copy with its own dependency.
assets = $(assets_conv)
ifeq ($(EXTWAD),0)
assets += $(FS)/doom.wad
endif

# Named explicitly because make takes the first target it sees as the default,
# and every rule below this line is one edit away from silently becoming it --
# which builds that one target, exits 0, and looks like a successful build.
.DEFAULT_GOAL := all

all: $(ROM).z64

# ------------------------------------------------------------- host tooling
# n64.mk points CC at the mips64-elf cross compiler, so name the host one
# explicitly: the converter runs at build time, not on the console.
HOST_CC ?= gcc

$(BUILD_DIR)/wad2n64: tools/wad2n64.c
	@mkdir -p $(dir $@)
	@echo "    [HOST ] $@"
	@$(HOST_CC) -O2 -Wall -Wextra -o $@ $<

# One converter run produces the palette and every texture, so funnel the whole
# asset set through a single stamp file rather than re-running it per output.
$(BUILD_DIR)/mkpvs: tools/mkpvs.c
	@mkdir -p $(dir $@)
	@echo "    [HOST ] $@"
	@$(HOST_CC) -O2 -Wall -Wextra -o $@ $< -lm

# .flags is a prerequisite so that flipping CI4FLATS (part of FLAGSIG via
# N64_CFLAGS) re-runs the converter -- without it a CI4FLATS=0 build would
# silently keep the previous build's CI4 flats and the lever would lie.
$(BUILD_DIR)/.assets.stamp: $(BUILD_DIR)/wad2n64 $(BUILD_DIR)/mkpvs $(WAD) $(BUILD_DIR)/.flags
	@mkdir -p $(FS) $(BUILD_DIR)
	@echo "    [WAD  ] $(WAD)"
	@$(BUILD_DIR)/wad2n64 $(WAD) $(FS) --all $(if $(filter 0,$(CI4FLATS)),--no-ci4)
	@if [ "$(PVS)" != "0" ]; then $(BUILD_DIR)/mkpvs $(WAD) $(FS); fi
	@echo "    [SFX  ] converting effects to wav64"
	@$(N64_INST)/bin/audioconv64 --wav-mono -o $(FS) $(FS)/*.wav
	@rm -f $(FS)/*.wav
	@touch $@

$(assets_conv): $(BUILD_DIR)/.assets.stamp
	@:

$(FS)/doom.wad: $(WAD)
	@mkdir -p $(FS)
	@echo "    [IWAD ] $@"
	@cp "$(WAD)" $@

# Every flag above changes what the code means, and an object file carries no
# record of the command line that produced it. So flipping MENUTEST, EXTWAD or
# GAME and rebuilding relinked whatever objects happened to be up to date,
# compiled under the previous flags -- which does not fail, it produces a ROM
# that is quietly a different build than the one asked for. That has cost real
# debugging time more than once: an A/B measured against a stale ROM, and an
# EXTWAD image that still carried the IWAD.
#
# The flag set is written to a file whenever it changes, and every object
# depends on that file.
# Per-file overrides have to be in the signature too, not just the global set.
# O3 adds -O3 to three renderer objects through a target-specific variable,
# which never appears in N64_CFLAGS -- so a plain `make` after `make O3=1` saw
# an unchanged signature, rebuilt nothing, and relinked the -O3 objects into a
# ROM claiming to be the baseline. That is the exact failure this mechanism
# exists to prevent, reintroduced through the one door it was not watching.
FLAGSIG := $(N64_CFLAGS) O3=$(O3)
$(shell mkdir -p $(BUILD_DIR); \
        [ "$$(cat $(BUILD_DIR)/.flags 2>/dev/null)" = '$(FLAGSIG)' ] || \
        printf '%s' '$(FLAGSIG)' > $(BUILD_DIR)/.flags)
$(src:%.c=$(BUILD_DIR)/%.o): $(BUILD_DIR)/.flags

# ------------------------------------------------------------------- ROM
$(BUILD_DIR)/$(ROM).dfs: $(assets)

ifneq ($(EXTWAD),0)
# Dropping the IWAD from the asset list is not enough. The DFS image is packed
# from whatever the filesystem directory holds, so a copy left behind by an
# earlier EXTWAD=0 build gets baked in regardless -- producing a ROM twice the
# intended size that still runs perfectly, which is the hardest kind of wrong
# to notice.
$(BUILD_DIR)/$(ROM).dfs: extwad-prune
.PHONY: extwad-prune
extwad-prune:
	@rm -f $(FS)/doom.wad
endif
$(BUILD_DIR)/$(ROM).elf: $(src:%.c=$(BUILD_DIR)/%.o)

$(ROM).z64: $(BUILD_DIR)/$(ROM).dfs

# ---------------------------------------------------------------- release
# `./build.sh release` builds both games' card ROMs and packs the archive the
# README's install section describes: one Doom/ folder to drop at the SD root,
# with an empty saves/ inside. EXTWAD only -- a default build bakes the
# commercial IWAD into the image, and no such ROM may ever be distributed.
#
# The size check is belt and braces over extwad-prune: an EXTWAD ROM is
# 11-16 MB and one with an IWAD baked in is 11-15 MB bigger, so 20 MB cleanly
# splits them, and a ROM that fails it aborts the pack rather than shipping.
#
# Music WADs ride along when they sit beside the Makefile. They are rendered
# from the games' own scores, which makes them as non-redistributable as the
# IWADs -- a public release normally ships without them, and the pack says
# what it skipped.
RELVER := $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
RELZIP := DoomUltra64-$(RELVER).zip
.PHONY: release
release:
	@$(MAKE) --no-print-directory EXTWAD=1
	@$(MAKE) --no-print-directory EXTWAD=1 GAME=doom2
	@for r in doom.z64 doom2.z64; do \
	    sz=$$(wc -c < $$r); \
	    if [ "$$sz" -ge 20000000 ]; then \
	        echo "!! $$r is $$sz bytes -- an IWAD is baked in, refusing to pack"; \
	        exit 1; \
	    fi; \
	done
	@rm -rf $(BUILD_DIR)/release
	@mkdir -p $(BUILD_DIR)/release/Doom/saves
	@cp doom.z64  $(BUILD_DIR)/release/Doom/DoomUltra64.z64
	@cp doom2.z64 $(BUILD_DIR)/release/Doom/Doom2Ultra64.z64
	@for m in DOOMMUS.WAD DOOM2MUS.WAD; do \
	    if [ -f "$$m" ]; then cp "$$m" $(BUILD_DIR)/release/Doom/; \
	    else echo "    [NOTE ] $$m not present -- archive ships without it"; fi; \
	done
	@echo "    [ZIP  ] $(RELZIP)"
	@python3 tools/mkzip.py $(RELZIP) $(BUILD_DIR)/release Doom
	@python3 -m zipfile -l $(RELZIP)

# ------------------------------------------------------------------ tests
# Compiles the real renderer against a mock RDP and rasterises it on the host.
# Verifies projection, near-clipping, TMEM tile addressing and fog without an
# emulator or a flashcart, which is the only fast iteration loop available.
test: $(assets)
	@mkdir -p $(BUILD_DIR)/shots
	@echo "    [HOST ] $(BUILD_DIR)/host_render"
	@$(HOST_CC) -O2 -Wall -Wextra -I tests/shim -DR_WIDE=$(WIDE) \
	    -o $(BUILD_DIR)/host_render tests/host_render.c \
	    src/r_wall.c src/r_flat.c src/r_sprite.c -lm
	@$(BUILD_DIR)/host_render $(BUILD_DIR)/shots $(FS)
	@python3 tools/test_muswad.py

clean:
	rm -rf $(BUILD_DIR) $(FS) $(ROM).z64

-include $(wildcard $(BUILD_DIR)/*.d)
-include $(wildcard $(BUILD_DIR)/src/*.d)

.PHONY: all clean test
