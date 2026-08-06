#!/usr/bin/env python3
"""Decode a .dt64 CI8 texture through playpal.tlut back to a PNG.

Verification only: this reverses the exact bytes the ROM will DMA, so a
correct-looking image here means the patch composition, the palette, and the
5551 quantisation are all right.
"""
import struct
import sys

from PIL import Image

# Padded so the runtime can point the RDP at the texels in place.
HEADER_SIZE = 16


def load_tlut(path):
    """Read the shared TLUT and undo RGBA5551 back to 8-bit RGB."""
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) != 512:
        sys.exit(f"{path}: expected 512 bytes, got {len(raw)}")

    palette = []
    for (packed,) in struct.iter_unpack(">H", raw):
        r5, g5, b5 = (packed >> 11) & 31, (packed >> 6) & 31, (packed >> 1) & 31
        # Replicate the high bits into the low ones so white stays white.
        palette.append(((r5 * 255) // 31, (g5 * 255) // 31, (b5 * 255) // 31))
    return palette


def load_dt64(path):
    with open(path, "rb") as f:
        raw = f.read()
    magic, version, width, height, flags, _lofs, _tofs = struct.unpack_from(
        ">4sHHHHhh", raw, 0)
    if magic != b"DT64":
        sys.exit(f"{path}: bad magic {magic!r}")

    texels = raw[HEADER_SIZE:]
    if len(texels) != width * height:
        sys.exit(f"{path}: expected {width * height} texels, got {len(texels)}")
    return version, width, height, flags, texels


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: dt64view.py <tex.dt64> <playpal.tlut> <out.png>")
    tex_path, tlut_path, out_path = sys.argv[1:4]

    palette = load_tlut(tlut_path)
    version, width, height, flags, texels = load_dt64(tex_path)

    img = Image.new("RGB", (width, height))
    img.putdata([palette[b] for b in texels])
    img.save(out_path)

    print(f"{tex_path}: v{version} {width}x{height} "
          f"{'masked' if flags & 1 else 'opaque'} -> {out_path}")
    print(f"  distinct palette indices used: {len(set(texels))}")


if __name__ == "__main__":
    main()
