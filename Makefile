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
N64_ROM_TITLE = "Doom II"
# Which IWAD and which music file this cartridge looks for on the card. The
# game is also identified from the IWAD's own lumps at boot, which is what
# drives Doom's logic; this only decides the filenames to open, and has to be
# fixed at build time because the assets baked in are already one game's.
N64_CFLAGS   += -DD_DOOM2=1
else
ROM           = doom
FS            = filesystem
IWAD_DEFAULT  = assets/DOOM.WAD
N64_ROM_TITLE = "Doom"
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
# The point of this flag is that it moves ONE variable. Geometry, CPU submit
# cost and TMEM uploads are all unchanged -- upload count is driven by which
# tiles are on screen, not by how many pixels they cover, and LOD selection is
# deliberately pinned to the vertical scale so it picks the same mips as the
# 320 build. What doubles is fill rate, RDRAM bandwidth, and the framebuffers
# (~600 KB -> ~1.2 MB for three buffers plus z).
#
# So an A/B of `f` in the HWSTAT line answers exactly one question: is there
# spare RDP capacity at 320x240? Build both, run the same demo route, compare
# medians. Vertical projection is bit-identical between the two by design.
WIDE ?= 0
N64_CFLAGS += -DR_WIDE=$(WIDE)

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

# FOGSCALE=1 scales the light-diminishing range by sector brightness, the
# way vanilla's diminishing tables do: bright sectors see far, dim sectors
# fall off close. Light-255 sectors are bit-identical to FOGSCALE=0 (the
# scale factor is exactly 1.0 there), so bright areas cannot regress; flats
# gain the same falloff walls have (they previously had none), which is the
# one deliberate look change. FOGSCALE=0 restores the fixed 512..3500 ramp.
FOGSCALE ?= 1
N64_CFLAGS += -DR_FOGSCALE=$(FOGSCALE)

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

# VAPOR=0 removes the liquid-vapor pass: the translucent noise layer that
# puts a green haze over nukage and slime and a smoke pall over lava. One
# z-tested fan per visible pool, drawn after all opaque passes.
VAPOR ?= 1
N64_CFLAGS += -DR_VAPOR=$(VAPOR)

# SMOKETRAIL=1 gives rockets and the monsters' fireballs a smoke trail: the
# same MT_SMOKE puffs the revenant's tracer already leaves, spawned one tic
# behind the shell -- every other tic for rockets, every fourth for the
# slower fireballs (the same ~40-unit spacing). Game-visible decoration,
# sync-invisible by construction: the spawn consumes no random stream at
# all (jitter is hashed from simulation state), so recorded demos play back
# unchanged -- proven route-identical with abdiff over the walker route.
SMOKETRAIL ?= 1
N64_CFLAGS += -DD_SMOKETRAIL=$(SMOKETRAIL)

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
N64_CFLAGS += -DR_REFLECT=$(REFLECT)

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

src = src/main.c src/mem.c src/dt64.c src/r_wall.c src/wad.c src/p_level.c src/r_bsp.c src/r_flat.c src/r_sky.c src/r_sprite.c src/r_am.c src/r_tri.c src/r_pvs.c src/r_wipe.c

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
RELZIP := DoomN64-$(RELVER).zip
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
	@cp doom.z64  $(BUILD_DIR)/release/Doom/Doom.z64
	@cp doom2.z64 $(BUILD_DIR)/release/Doom/Doom2.z64
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
