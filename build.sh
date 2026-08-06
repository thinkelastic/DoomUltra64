#!/usr/bin/env bash
# Build the ROM inside the pinned toolchain container.
#
# Runs as the invoking user so build artifacts are not root-owned.
set -euo pipefail

cd "$(dirname "$0")"

if ! docker image inspect doomn64-build >/dev/null 2>&1; then
    echo "==> building toolchain image (one time, a few minutes)"
    docker build -t doomn64-build .
fi

exec docker run --rm \
    -u "$(id -u):$(id -g)" \
    -v "$PWD:/project" \
    -w /project \
    doomn64-build \
    make "$@"
