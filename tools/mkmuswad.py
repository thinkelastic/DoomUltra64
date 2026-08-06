#!/usr/bin/env python3
"""Build DOOMMUS.WAD: every music track in one file the N64 can actually read.

This replaces tools/split_music.py's forty loose files with a single WAD, and
the reason that split existed has to be addressed or this just reintroduces
the bug.

A WAD keeps its directory at the end. Reading the original on the N64 meant
seeking to the last byte of a 599 MB file, and libdragon builds FatFs with
FF_USE_FASTSEEK off, so f_lseek walks the cluster chain one link at a time --
about 19,000 links at the card's 32 KB clusters. Through a flashcart's SD path
that walk failed outright with EIO, which is why the tracks were split into
files that open at offset 0 and are read forwards.

Three things make one file workable now.

The directory goes immediately after the twelve-byte header instead of at the
end. Nothing in the format requires the tail layout -- the header carries the
directory's offset, so it may sit anywhere -- and at the front it is reached by
reading forwards from byte zero, seeking nowhere. Every tool that reads WADs by
the header still opens it.

The audio is downsampled here rather than copied, exactly as split_music.py did
it: 48 kHz stereo to 24 kHz mono, each pair of stereo frames averaged into one
sample. Averaging rather than dropping samples is a crude low-pass that avoids
folding aliases into the audible range, and it is four times less traffic
through a card that has been corrupting reads.

And tracks that share audio share it here too. Doom reuses music across
episodes, and the source WAD records that by pointing several directory entries
at one run of bytes; copying each out separately would add about 100 MB of
duplicates and push every later track that much deeper into the file.

Together those put the file at 150 MB with the furthest track about 4,700
clusters in, a quarter of the walk that failed.

The directory is written twice: once as a placeholder, then again with the real
offsets once the audio has been streamed and the sizes are known. Predicting
them from the input sizes would work out to the same numbers, but a prediction
that silently drifts from what was written would corrupt every track after the
first, and seeking backwards costs nothing on the host.

    ./tools/mkmuswad.py DOOMMUS.wad /run/media/you/N64/DOOMMUS.WAD
    ./tools/mkmuswad.py pcm-dir/    /run/media/you/N64/DOOMMUS.WAD
"""
import os
import struct
import sys

# The source is 48 kHz stereo; halving both gives 24 kHz mono.
RATE_DIV = 2

HDR_SIZE = 12
ENT_SIZE = 16


def read_dir(f):
    """Directory of a source music WAD, wherever it keeps it."""
    f.seek(0)
    if f.read(4) not in (b"IWAD", b"PWAD"):
        return None, 48000, 2

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

    return entries, rate, channels


def copy_track(src, out, size, channels):
    """Downsample one track from src into out. Returns bytes written."""
    import numpy as np

    # A whole track is tens of megabytes; work in blocks, keeping each block a
    # whole number of output samples so no frame is split across a boundary.
    frame = channels * 2
    block_frames = (1 << 20) // frame // RATE_DIV * RATE_DIV
    remaining = size
    written = 0

    while remaining > 0:
        raw = src.read(min(block_frames * frame, remaining))
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
        block = a.astype("<i2").tobytes()
        out.write(block)
        written += len(block)

    return written


def copy_file(path, out):
    """Copy an already-converted .pcm through. Returns bytes written."""
    written = 0
    with open(path, "rb") as f:
        while True:
            block = f.read(1 << 20)
            if not block:
                break
            out.write(block)
            written += len(block)
    return written


def file_key(path):
    """Identity of an already-converted track, for spotting duplicates."""
    import hashlib

    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            block = f.read(1 << 20)
            if not block:
                break
            h.update(block)
    return h.hexdigest()


def shares(written, pos):
    """Name of the first track already occupying this offset."""
    for name, p, _ in written:
        if p == pos:
            return name
    return "?"


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    src, outpath = sys.argv[1], sys.argv[2]

    srcf = None
    if os.path.isdir(src):
        # Pre-split tracks: pack them as they are.
        names = sorted(f for f in os.listdir(src) if f.lower().endswith(".pcm"))
        tracks = [(os.path.splitext(n)[0].upper(), os.path.join(src, n), 0)
                  for n in names]
        rate, channels = 24000, 1
        convert = False
    else:
        srcf = open(src, "rb")
        entries, rate, channels = read_dir(srcf)
        if entries is None:
            print(f"{src}: not a WAD and not a directory")
            return 1
        tracks = [(n, p, s) for n, p, s in entries
                  if n != "PCMINFO" and s > 0]
        convert = True
        print(f"source {rate} Hz x{channels}  ->  output "
              f"{rate // RATE_DIV} Hz mono")

    # The directory is sized before anything is written, so a track that turns
    # out to produce nothing has to be recognised now rather than left as a
    # zero-length entry. Whether a track survives the halving needs no size
    # prediction -- only whether it holds one complete pair of frames.
    def keeps(name, ref, size):
        if len(name) > 8:
            print(f"  skipping {name}: lump names are eight characters at most")
            return False
        have = size if convert else os.path.getsize(ref)
        if convert and have < channels * 2 * RATE_DIV or have == 0:
            print(f"  skipping {name}: too short to yield a sample")
            return False
        return True

    tracks = [t for t in tracks if keeps(*t)]
    if not tracks:
        print(f"{src}: no tracks")
        return 1

    dirofs = HDR_SIZE
    datofs = dirofs + ENT_SIZE * len(tracks)

    with open(outpath, "wb") as out:
        out.write(b"PWAD")
        out.write(struct.pack("<ii", len(tracks), dirofs))
        out.write(b"\0" * (ENT_SIZE * len(tracks)))       # placeholder

        # Doom reuses music across episodes, and the source WAD says so by
        # pointing several directory entries at one run of bytes. Copying each
        # of them out separately would put about 100 MB of duplicate audio on
        # the card -- and since the cost of reaching a track is how deep into
        # the file it starts, that padding lands directly on the one measure
        # this layout has to keep small. Aliased entries share their data here
        # too: nothing requires WAD lumps to occupy disjoint ranges, and the
        # runtime addresses a track by offset and length either way.
        written = []
        seen = {}
        end = datofs
        for name, ref, size in tracks:
            key = (ref, size) if convert else file_key(ref)
            if key in seen:
                pos, n = seen[key]
                written.append((name, pos, n))
                print(f"  {name:8s} {'= ' + shares(written, pos):>14s}")
                continue

            pos = out.tell()
            assert pos == end, f"{name}: at {pos}, expected {end}"
            if convert:
                srcf.seek(ref)
                n = copy_track(srcf, out, size, channels)
            else:
                n = copy_file(ref, out)
            end = pos + n
            seen[key] = (pos, n)
            written.append((name, pos, n))
            secs = n / (rate // (RATE_DIV if convert else 1) * 2)
            print(f"  {name:8s} {n / 1048576:6.1f} MB  {secs:5.1f}s")

        # Now the real directory, over the placeholder.
        out.seek(dirofs)
        for name, pos, size in written:
            out.write(struct.pack("<ii", pos, size))
            out.write(name.encode("ascii").ljust(8, b"\0"))

    if srcf:
        srcf.close()

    total = os.path.getsize(outpath)
    unique = len({p for _, p, _ in written})
    print(f"{outpath}: {len(written)} tracks ({unique} distinct), "
          f"{total / 1048576:.0f} MB, directory at byte {dirofs}, "
          f"deepest track starts {max(p for _, p, _ in written) / 1048576:.0f} MB in")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
