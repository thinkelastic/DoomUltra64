#!/usr/bin/env bash
# Run the ROM in ares and capture a screenshot.
#
#   ./run.sh                 headless via Xvfb, writes shots/doom.png
#   ./run.sh :0              render on an existing display instead
#
# ares is used rather than mupen64plus for two reasons: its paraLLEl-RDP is
# accurate enough to trust for RDP work, and mupen64plus aborts on this machine
# even on a stock libdragon example ROM.
#
# Two ares settings matter and are easy to get wrong:
#   * Video/Driver must be "OpenGL 3.2". The XShm, SDL and XVideo drivers all
#     display nothing under Xvfb -- verified against a known-good ROM, so it is
#     the driver and not the ROM.
#   * libdragon's debug output (asserts, backtraces, debugf) arrives on ares's
#     stdout, which is what makes DEBUG=1 builds worth running here.
set -euo pipefail

cd "$(dirname "$0")"

TARGET_DISPLAY="${1:-}"
SHOTDIR="${SHOTDIR:-$PWD/shots}"
RUN_SECONDS="${RUN_SECONDS:-45}"
# ROM=doom2.z64 runs the Doom II cartridge instead.
ROM="${ROM:-doom.z64}"

mkdir -p "$SHOTDIR"
LOG="$SHOTDIR/ares.log"

cleanup() {
    [[ -n "${ARES_PID:-}" ]] && kill "$ARES_PID" 2>/dev/null || true
    [[ -n "${XVFB_PID:-}" ]] && kill "$XVFB_PID" 2>/dev/null || true
}
trap cleanup EXIT

if [[ -z "$TARGET_DISPLAY" ]]; then
    export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
    TARGET_DISPLAY=":98"
    Xvfb "$TARGET_DISPLAY" -screen 0 1024x768x24 >/dev/null 2>&1 &
    XVFB_PID=$!
    for _ in $(seq 1 100); do
        DISPLAY="$TARGET_DISPLAY" timeout 1 ares --version >/dev/null 2>&1 && break
    done
fi

DISPLAY="$TARGET_DISPLAY" ares \
    --system "Nintendo 64" \
    --setting Video/Driver="OpenGL 3.2" \
    --no-file-prompt \
    "$ROM" >"$LOG" 2>&1 &
ARES_PID=$!

# Let the ROM run for a fixed wall-clock span before the capture.
#
# This used to be a busy loop of five million shell iterations under a
# timeout, which is not a duration at all: on an idle machine it finished in
# about seven seconds whatever RUN_SECONDS said, and under load it ran for
# minutes. Soak tests silently became smoke tests, and A/B runs compared
# different amounts of gameplay. Sleep instead, so the number means what it
# says and two runs cover the same span.
sleep "$RUN_SECONDS"

DISPLAY="$TARGET_DISPLAY" import -window root "$SHOTDIR/doom.png"

echo "=== ROM output (asserts, debugf, RDP validator) ==="
grep -viE "stalled compile|^$" "$LOG" | head -30

echo
echo "=== captured $SHOTDIR/doom.png ==="
