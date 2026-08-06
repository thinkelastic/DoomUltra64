#!/usr/bin/env python3
"""Split the music WAD into one raw PCM file per track, downsampled.

Two reasons this does not just copy the audio through.

One file per track, rather than one WAD: a WAD keeps its directory at the end,
so reading it on the N64 means seeking to the last byte of a 599 MB file, and
libdragon's FatFs walks the cluster chain one link at a time. Through an
EverDrive's SD path that walk fails outright. Every track being its own file
means opening at offset 0 and reading forwards, which is the access pattern the
stack handles.

And downsampled, because the card is marginal. 48 kHz stereo is 192 KB/s
sustained; 24 kHz mono is 48 KB/s. Four times less traffic through a path that
has been corrupting reads, and the set shrinks from about a gigabyte to a
quarter of that.

The conversion is a halving in both dimensions, which keeps it exact and cheap:
each pair of stereo frames averages into one mono sample. Averaging rather than
dropping samples acts as a crude low-pass and avoids the aliasing that plain
decimation would fold into the audible range.

    ./tools/split_music.py DOOMMUS.wad /run/media/you/N64
"""
import os
import struct
import sys

import numpy as np

# The source is 48 kHz stereo; halving both gives 24 kHz mono.
RATE_DIV = 2


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    src, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)

    with open(src, "rb") as f:
        if f.read(4) not in (b"IWAD", b"PWAD"):
            print(f"{src}: not a WAD")
            return 1

        count, diroff = struct.unpack("<ii", f.read(8))
        f.seek(diroff)
        entries = []
        for _ in range(count):
            pos, size = struct.unpack("<ii", f.read(8))
            name = f.read(8).rstrip(b"\0").decode("ascii", "replace")
            entries.append((name, pos, size))

        rate, channels = 48000, 2
        for name, pos, size in entries:
            if name == "PCMINFO":
                f.seek(pos)
                rate, channels = struct.unpack("<II", f.read(8))
                break

        out_rate = rate // RATE_DIV
        print(f"source {rate} Hz x{channels}  ->  output {out_rate} Hz mono")

        written = 0
        total_in = total_out = 0
        for name, pos, size in entries:
            if name == "PCMINFO" or size <= 0:
                continue

            f.seek(pos)
            dst = os.path.join(outdir, f"{name}.pcm")

            with open(dst, "wb") as out:
                # A whole track is tens of megabytes; work in blocks, keeping
                # each block a whole number of output samples so no frame is
                # split across a boundary.
                frame = channels * 2
                block_frames = (1 << 20) // frame // RATE_DIV * RATE_DIV
                remaining = size

                while remaining > 0:
                    want = min(block_frames * frame, remaining)
                    raw = f.read(want)
                    if not raw:
                        break
                    remaining -= len(raw)

                    usable = len(raw) // (frame * RATE_DIV) * (frame * RATE_DIV)
                    if usable == 0:
                        break

                    a = np.frombuffer(raw[:usable], dtype="<i2").astype(np.int32)
                    # (frames, channels) -> average channels -> average pairs
                    a = a.reshape(-1, channels).mean(axis=1)
                    a = a.reshape(-1, RATE_DIV).mean(axis=1)
                    out.write(a.astype("<i2").tobytes())

            osize = os.path.getsize(dst)
            secs = osize / (out_rate * 2)
            print(f"  {name}.pcm  {osize / 1048576:6.1f} MB  {secs:5.1f}s")
            written += 1
            total_in += size
            total_out += osize

    print(f"{written} tracks: {total_in / 1048576:.0f} MB "
          f"-> {total_out / 1048576:.0f} MB  ({out_rate} Hz mono)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
