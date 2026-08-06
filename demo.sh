#!/usr/bin/env bash
# Record an attract-mode run: boots a DEMO=1 ROM, samples the screen at
# intervals and assembles the frames into a contact sheet and an animation.
#
# Build the ROM with `./build.sh DEMO=1` first. The point is the peak frame
# time in the HUD, which only a route reveals -- a single vantage point misses
# every frame that actually stutters.
#
# Software rendering under Xvfb runs well below real time, so the interval
# between samples is emulator time, not wall-clock time.
set -euo pipefail

cd "$(dirname "$0")"

ROM="${1:-doom.z64}"
OUT="${OUT:-shots/demo}"
FRAMES="${FRAMES:-16}"
SPACING="${SPACING:-2200000}"   # busy-loop iterations between samples

mkdir -p "$OUT"
rm -f "$OUT"/frame_*.png

cleanup() {
    [[ -n "${ARES_PID:-}" ]] && kill "$ARES_PID" 2>/dev/null || true
    [[ -n "${XVFB_PID:-}" ]] && kill "$XVFB_PID" 2>/dev/null || true
}
trap cleanup EXIT

export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
DISP=":97"
Xvfb "$DISP" -screen 0 1024x768x24 >/dev/null 2>&1 &
XVFB_PID=$!
for _ in $(seq 1 100); do
    DISPLAY="$DISP" timeout 1 ares --version >/dev/null 2>&1 && break
done

DISPLAY="$DISP" ares \
    --system "Nintendo 64" \
    --setting Video/Driver="OpenGL 3.2" \
    --no-file-prompt \
    "$ROM" >"$OUT/ares.log" 2>&1 &
ARES_PID=$!

# Let it boot and load the level before the first sample.
timeout 120 bash -c 'n=0; while [ $n -lt 6000000 ]; do n=$((n+1)); done' || true

for i in $(seq 1 "$FRAMES"); do
    DISPLAY="$DISP" import -window root "$OUT/$(printf 'frame_%02d.png' "$i")" 2>/dev/null || true
    timeout 120 bash -c "n=0; while [ \$n -lt $SPACING ]; do n=\$((n+1)); done" || true
done

echo "=== ROM output ==="
grep -viE "stalled compile|^$" "$OUT/ares.log" | head -20
echo "=== $(ls "$OUT"/frame_*.png | wc -l) frames in $OUT ==="
