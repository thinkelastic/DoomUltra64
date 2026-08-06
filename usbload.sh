#!/usr/bin/env bash
# Upload the ROM over USB and stream the console's debug output.
#
# The EverDrive 64 X7 takes a ROM over its USB port, so the card never has to
# leave the console: build, run this, and the N64 is running the new build a
# few seconds later. It also carries debugf back the other way, which is the
# only way to see asserts, backtraces and diagnostics from real hardware.
#
#   ./usbload.sh              upload doom.z64 and follow the output
#   ./usbload.sh -l           also reupload automatically whenever it rebuilds
#
# -b disables ncurses so the output is plain text that can be piped and logged.
set -euo pipefail

cd "$(dirname "$0")"

ROM="${ROM:-doom.z64}"
LOG="${LOG:-/tmp/unfl-debug.log}"
[[ -f "$ROM" ]] || { echo "no $ROM -- build it first"; exit 1; }

# -d takes an OPTIONAL filename, so it must be given one explicitly: without
# it, UNFLoader swallows the ROM path as its log target and uploads nothing --
# and truncates the ROM in the process.
exec tools/bin/UNFLoader -b -d "$LOG" "$@" "$ROM"
