#!/usr/bin/env bash
# Prove that a renderer change alters no pixel, over a whole route.
#
# VALIDATED Aug 11 2026: A/A identical AND the historically false-failing
# no-op pair (R_FASTFLOOR=0 vs =1) identical, 144/144 fingerprints each.
#
# The false fails had FOUR stacked causes, all route/timing, none rendering.
# Two were fixed when this header was first written (half-drawn frames --
# rdpq_detach_wait under POSEHASH; gametic-keyed routes -- now leveltime).
# The remaining two lived in the DEMO walker this harness drives the route
# with: its warmup/door/turn state advanced per rendered FRAME, and its
# inputs were sampled once per frame however many tics that frame covered --
# both are pure functions of build speed, which is exactly what an A/B
# varies, so the two ROMs walked DIFFERENT ROUTES and every fingerprint
# after the first knife-edge decision differed. demo_drive is now fully
# leveltime-keyed and re-evaluated per tic inside run_game_tics (main.c),
# making the route a function of simulation state alone.
#
# A FAIL from this script is therefore evidence about the change under test
# again. Two caveats stand: routes before/after the walker change are not
# comparable, and this still answers only "did the picture change", never
# "is it faster" (ares mis-models VR4300 timing).
#
#   ./abdiff.sh R_FASTFLOOR=0 -- R_FASTFLOOR=1
#   ./abdiff.sh -- TRIFAST=0                     # A is the default build
#
# Builds both sides with POSEHASH on, runs each in ares, and diffs the frame
# fingerprints the ROM prints. Nothing is captured or cropped: see
# src/posehash.h for why the ROM hashes its own framebuffer instead.
#
# Exit status is the answer -- 0 when every common tic matches, 1 when any
# differ, so this is usable as a check and not only as a report.
#
# What it cannot tell you is whether a change is FASTER. ares under-models
# VR4300 memory timing badly, which is exactly the axis most of these changes
# move. This answers "did the picture change", nothing else.
set -euo pipefail

cd "$(dirname "$0")"

INTERVAL="${INTERVAL:-15}"        # tics between fingerprints
RUN_SECONDS="${RUN_SECONDS:-70}"
# INTERP=0 is required by the Makefile, not chosen here.
#
# DEMO=1 rather than a direct MAP boot, because a direct boot leaves the player
# standing still with no input: every frame is the same picture, so the whole
# route collapses to one pose and the comparison passes without having tested
# anything. Demo playback is recorded input replayed per tic, which moves the
# camera through the level and is deterministic for exactly the same reason
# the fingerprints are comparable at all.
COMMON="${COMMON:-POSEHASH=$INTERVAL INTERP=0 DEMO=1}"

A_FLAGS=(); B_FLAGS=(); seen_sep=0
for arg in "$@"; do
    if [ "$arg" = "--" ]; then seen_sep=1; continue; fi
    if [ "$seen_sep" = 0 ]; then A_FLAGS+=("$arg"); else B_FLAGS+=("$arg"); fi
done
if [ "$seen_sep" = 0 ]; then
    echo "usage: $0 [A flags...] -- [B flags...]" >&2
    exit 2
fi

WORK="${WORK:-$(mktemp -d)}"
trap 'rm -rf "$WORK"' EXIT

# Build failures must never reach the comparison. A stale ROM from the previous
# side would be diffed against itself and report a perfect match -- the most
# convincing wrong answer this script could give.
run_side() {
    local name="$1"; shift
    echo "==> building $name: $COMMON $*"
    if ! ./build.sh $COMMON "$@" > "$WORK/$name.build" 2>&1; then
        echo "!! $name failed to build" >&2
        tail -20 "$WORK/$name.build" >&2
        exit 1
    fi
    cp doom.z64 "$WORK/$name.z64"

    echo "==> running $name"
    ROM="$WORK/$name.z64" RUN_SECONDS="$RUN_SECONDS" ./run.sh > "$WORK/$name.run" 2>&1 || true
    grep -a '^posehash:' shots/ares.log | sort -u > "$WORK/$name.hashes" || true

    local n; n=$(wc -l < "$WORK/$name.hashes")
    echo "    $n fingerprints"
    if [ "$n" -eq 0 ]; then
        echo "!! $name produced no fingerprints -- did the ROM boot?" >&2
        grep -aiE 'assert|no iwad|exception' "$WORK/$name.run" | head -5 >&2 || true
        exit 1
    fi
}

run_side A "${A_FLAGS[@]+"${A_FLAGS[@]}"}"
run_side B "${B_FLAGS[@]+"${B_FLAGS[@]}"}"

# Compare only tics both runs reached. A run that got further is not a
# mismatch, and padding the shorter one with blanks would report one.
#
# join matches keys as STRINGS, so both sides are sorted lexically on the tic
# and not numerically -- a numeric sort here silently drops most of the
# matches and the script reports a handful of compared frames as a clean pass.
extract() {
    sed -n 's/^posehash: tic=\([0-9]*\) h=\(.*\)$/\1 \2/p' "$1" | sort -k1,1 -u
}
join -j1 -o 0,1.2,2.2 <(extract "$WORK/A.hashes") <(extract "$WORK/B.hashes") \
    > "$WORK/pairs" || true

common=$(wc -l < "$WORK/pairs")
differ=$(awk '$2 != $3' "$WORK/pairs" | wc -l)

echo
echo "=== compared $common tics present in both runs ==="
if [ "$common" -eq 0 ]; then
    echo "NO OVERLAP -- the two runs share no tic, nothing was compared" >&2
    exit 1
fi
if [ "$differ" -eq 0 ]; then
    echo "IDENTICAL: every one of $common frames hashes the same"
    exit 0
fi
echo "DIFFER: $differ of $common frames"
awk '$2 != $3 {print "  tic " $1 ":  A=" $2 "  B=" $3}' "$WORK/pairs" | head -20
exit 1
