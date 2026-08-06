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

# HWSTAT=1 draws the frame-time/pose line at the top of the screen and emits
# the USB telemetry. Off for normal play; the debugging workflow described in
# the project notes turns it on.
HWSTAT ?= 0
N64_CFLAGS += -DD_HWSTAT=$(HWSTAT)

# O3=1 compiles the three hottest renderer files -O3 instead of the global
# -O2: an A/B experiment for hardware demo-route medians only. The VR4300's
# 16 KB I-cache can turn the extra inlining into a net loss, so this is
# never the default; measure before adopting.
ifeq ($(O3),1)
$(BUILD_DIR)/src/r_wall.o $(BUILD_DIR)/src/r_flat.o $(BUILD_DIR)/src/r_bsp.o: N64_CFLAGS += -O3
endif

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

$(BUILD_DIR)/.assets.stamp: $(BUILD_DIR)/wad2n64 $(BUILD_DIR)/mkpvs $(WAD)
	@mkdir -p $(FS) $(BUILD_DIR)
	@echo "    [WAD  ] $(WAD)"
	@$(BUILD_DIR)/wad2n64 $(WAD) $(FS) --all
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
FLAGSIG := $(N64_CFLAGS)
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

# ------------------------------------------------------------------ tests
# Compiles the real renderer against a mock RDP and rasterises it on the host.
# Verifies projection, near-clipping, TMEM tile addressing and fog without an
# emulator or a flashcart, which is the only fast iteration loop available.
test: $(assets)
	@mkdir -p $(BUILD_DIR)/shots
	@echo "    [HOST ] $(BUILD_DIR)/host_render"
	@$(HOST_CC) -O2 -Wall -Wextra -I tests/shim \
	    -o $(BUILD_DIR)/host_render tests/host_render.c \
	    src/r_wall.c src/r_flat.c src/r_sprite.c -lm
	@$(BUILD_DIR)/host_render $(BUILD_DIR)/shots $(FS)
	@python3 tools/test_muswad.py

clean:
	rm -rf $(BUILD_DIR) $(FS) $(ROM).z64

-include $(wildcard $(BUILD_DIR)/*.d)
-include $(wildcard $(BUILD_DIR)/src/*.d)

.PHONY: all clean test
