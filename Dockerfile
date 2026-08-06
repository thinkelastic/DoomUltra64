# DoomN64 build environment.
# Extends the libdragon toolchain image (mips64-elf GCC 14.4) by building and
# installing libdragon itself, so project builds need no toolchain setup.
FROM ghcr.io/dragonminded/libdragon:latest

ENV N64_INST=/n64_toolchain

COPY toolchain/libdragon /libdragon-src
WORKDIR /libdragon-src
RUN ./build.sh && rm -rf /libdragon-src

WORKDIR /project
