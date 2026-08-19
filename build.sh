#!/usr/bin/env bash
# Build the ROM inside the pinned toolchain container.
#
# Runs as the invoking user so build artifacts are not root-owned.
set -euo pipefail

cd "$(dirname "$0")"

# RSPQPROF=1 needs a libdragon with the RSP profiler compiled in, which is a
# property of the toolchain image rather than of the project's flags -- see the
# Dockerfile. Give it its own image so the two never overwrite each other, and
# a plain ./build.sh keeps producing an unperturbed ROM.
image=doomultra64-build
build_args=()
for arg in "$@"; do
    if [ "$arg" = "RSPQPROF=1" ]; then
        image=doomultra64-build-rspqprof
        build_args=(--build-arg RSPQ_PROFILE=1)
    fi
done

if ! docker image inspect "$image" >/dev/null 2>&1; then
    echo "==> building toolchain image $image (one time, a few minutes)"
    docker build "${build_args[@]+"${build_args[@]}"}" -t "$image" .
fi

exec docker run --rm \
    -u "$(id -u):$(id -g)" \
    -v "$PWD:/project" \
    -w /project \
    "$image" \
    make "$@"
