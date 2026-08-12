# DoomN64 build environment.
# Extends the libdragon toolchain image (mips64-elf GCC 14.4) by building and
# installing libdragon itself, so project builds need no toolchain setup.
FROM ghcr.io/dragonminded/libdragon:latest

ENV N64_INST=/n64_toolchain

# RSPQ_PROFILE=1 compiles libdragon's RSP queue profiler in. It cannot be
# switched on from the project's own flags: the counters are sampled by the RSP
# microcode itself (src/rspq/rsp_profile.S) and by the core queue, both of which
# live in this image. With it off, libdragon still exports rspq_profile_*, but
# as empty stubs -- a build that calls them links cleanly and reports zeros.
#
# So the profiling toolchain is a separate image, which is what ./build.sh
# RSPQPROF=1 builds and selects. Off here because the sampling perturbs the
# very thing it measures; the shipping ROM must not carry it.
ARG RSPQ_PROFILE=0

COPY toolchain/libdragon /libdragon-src
WORKDIR /libdragon-src
# Both edits are asserted rather than assumed: a sed that silently matched
# nothing would produce a stub-only libdragon under a name promising the
# opposite, which is the failure this whole arrangement exists to prevent.
#
# Dropping H.264 is not cosmetic. Profiling adds resident code to the rspq
# microcode, and every overlay has to fit in what is left of the RSP's 4 KB of
# IMEM alongside it -- libdragon's H.264 inter-prediction overlay then misses
# by 112 bytes and the library will not build. Nothing here decodes video, and
# these overlays are never registered, so they cannot affect what the profiler
# reports; mpeg1, yuv and fmv are left alone so the profiling image differs
# from the shipping one by as little as possible.
RUN set -eu; \
    sed -i "s/^\(#define RSPQ_PROFILE  *\)0/\1${RSPQ_PROFILE}/" \
        include/rspq_constants.h; \
    grep -qE "^#define RSPQ_PROFILE +${RSPQ_PROFILE}\b" include/rspq_constants.h; \
    if [ "${RSPQ_PROFILE}" != "0" ]; then \
        sed -i '/h264/d' src/video/libdragon.mk; \
        ! grep -q 'h264' src/video/libdragon.mk; \
    fi; \
    ./build.sh; \
    rm -rf /libdragon-src

WORKDIR /project
