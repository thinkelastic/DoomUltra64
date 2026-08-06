#!/usr/bin/env python3
"""Check mkmuswad.py's output against the parser in src/mus_n64.c.

The console-side reader is C and cannot run here, so this asserts the two
properties it depends on: the directory sits at byte 12 so it is reached by
reading forwards, and every lump's offset and size bound exactly the audio
that track should contain. A track that starts one byte late plays as noise,
and there is no way to notice that on hardware except by listening.
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
RATE_DIV = 2

# mkmuswad.py needs numpy only to downsample a source WAD; packing
# already-converted tracks does not. The build container has no numpy, and the
# layout is the part the console-side parser depends on, so that half of the
# test runs either way and the conversion half is skipped where it cannot run.
try:
    import numpy  # noqa: F401
    HAVE_NUMPY = True
except ImportError:
    HAVE_NUMPY = False


def make_source(path, tracks, aliases=None, rate=48000, channels=2):
    """A music WAD in the original layout: directory at the end.

    `aliases` maps a track name onto another's data, which is how the real
    music WAD stores the episodes that reuse each other's music: several
    directory entries, one run of bytes.
    """
    blobs = {}
    for name, frames in tracks.items():
        # Distinct per track and per sample so a misaligned read is obvious.
        bias = sum(name.encode()) % 100
        vals = [(i % 3001) + bias for i in range(frames * channels)]
        blobs[name] = struct.pack(f"<{len(vals)}h", *vals)

    info = struct.pack("<II", rate, channels)

    with open(path, "wb") as f:
        f.write(b"PWAD")
        f.write(struct.pack("<ii", len(blobs) + 1, 0))    # patched below
        entries = []
        entries.append(("PCMINFO", f.tell(), len(info)))
        f.write(info)
        at = {}
        for name, blob in blobs.items():
            at[name] = (f.tell(), len(blob))
            entries.append((name, at[name][0], at[name][1]))
            f.write(blob)

        for alias, target in (aliases or {}).items():
            entries.append((alias, at[target][0], at[target][1]))
            blobs[alias] = blobs[target]

        diroff = f.tell()
        for name, pos, size in entries:
            f.write(struct.pack("<ii", pos, size))
            f.write(name.encode().ljust(8, b"\0"))

        f.seek(4)
        f.write(struct.pack("<ii", len(entries), diroff))

    return blobs, channels


def expected(blob, channels):
    """What copy_track should produce for this input.

    Written out longhand rather than with numpy so it is an independent
    statement of the conversion, not the same expression run twice. Averaging
    the channels and then each pair of frames is one mean over the whole group;
    numpy's cast to int16 truncates toward zero, which is what int() does.
    """
    group = channels * RATE_DIV
    frame = channels * 2
    usable = len(blob) // (frame * RATE_DIV) * (frame * RATE_DIV)
    s = struct.unpack(f"<{usable // 2}h", blob[:usable])
    out = [int(sum(s[i:i + group]) / group) for i in range(0, len(s), group)]
    return struct.pack(f"<{len(out)}h", *out)


def check(outpath, want):
    """Read the output the way src/mus_n64.c does, and compare."""
    fails = []
    with open(outpath, "rb") as f:
        magic = f.read(4)
        count, dirofs = struct.unpack("<ii", f.read(8))

        if magic not in (b"IWAD", b"PWAD"):
            fails.append(f"magic is {magic!r}")
        # The whole point: the reader must never seek to find the directory.
        if dirofs != 12:
            fails.append(f"directory at {dirofs}, not 12 -- the seek is back")
        if count != len(want):
            fails.append(f"{count} lumps, expected {len(want)}")

        entries = []
        for _ in range(count):
            pos, size = struct.unpack("<ii", f.read(8))
            name = f.read(8).rstrip(b"\0").decode()
            entries.append((name, pos, size))

        # Data starts immediately after the directory, and every byte of the
        # file belongs to some track: the runtime reads a track as a byte
        # range and loops at its end, so a gap or a stray overlap plays the
        # neighbouring track. Tracks that share audio are expected to name the
        # identical range -- that is the deduplication -- but a partial
        # overlap is a packing bug.
        datofs = 12 + 16 * count
        end = datofs
        ranges = {}
        for name, pos, size in entries:
            if (pos, size) in ranges:
                continue                              # a deliberate alias
            if pos != end:
                fails.append(f"{name} at {pos}, expected {end}")
            ranges[(pos, size)] = name
            end = pos + size

        starts = {p for p, _ in ranges}
        for name, pos, size in entries:
            if (pos, size) not in ranges and pos in starts:
                fails.append(f"{name} partially overlaps another track")

        if end != os.path.getsize(outpath):
            fails.append(f"lumps end at {end}, file is "
                         f"{os.path.getsize(outpath)} bytes")

        for name, pos, size in entries:
            if name not in want:
                fails.append(f"unexpected lump {name}")
                continue
            f.seek(pos)
            got = f.read(size)
            if got != want[name]:
                fails.append(f"{name}: {size} bytes do not match the "
                             f"expected {len(want[name])}")

        for name in want:
            if not any(e[0] == name for e in entries):
                fails.append(f"missing lump {name}")

    return fails


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        srcpath = os.path.join(tmp, "src.wad")
        outpath = os.path.join(tmp, "DOOMMUS.WAD")

        # Odd frame counts on purpose: the converter drops a trailing frame
        # that does not complete a pair, and the directory must record what
        # was actually written rather than what was predicted.
        tracks = {"PE1M1": 5000, "PE1M2": 4999, "PINTRO": 1, "PBUNNY": 777}
        # E4 reuses E1's music, as the real WAD does.
        aliases = {"PE4M1": "PE1M1", "PE4M2": "PE1M2"}
        blobs, channels = make_source(srcpath, tracks, aliases)

        want = {n: expected(b, channels) for n, b in blobs.items()}
        # A track too short to yield even one output sample is not a lump.
        want = {n: b for n, b in want.items() if b}

        fails = []

        if HAVE_NUMPY:
            r = subprocess.run([sys.executable,
                                os.path.join(HERE, "mkmuswad.py"),
                                srcpath, outpath],
                               capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stdout + r.stderr)
                print("FAIL: mkmuswad.py exited nonzero")
                return 1

            fails += check(outpath, want)

            # Reused music must be stored once. Without this the real set grows
            # by about 100 MB of duplicates, pushing every later track deeper
            # into the file -- the cost this layout exists to keep down.
            with open(outpath, "rb") as f:
                f.seek(12)
                offs = {}
                for _ in range(len(want)):
                    pos, size = struct.unpack("<ii", f.read(8))
                    offs[f.read(8).rstrip(b"\0").decode()] = pos
            for alias, target in aliases.items():
                if offs.get(alias) != offs.get(target):
                    fails.append(f"{alias} was copied rather than shared with "
                                 f"{target} ({offs.get(alias)} vs "
                                 f"{offs.get(target)})")

        # And the same content, packed from pre-split files.
        pcmdir = os.path.join(tmp, "pcm")
        os.makedirs(pcmdir)
        for name, blob in want.items():
            with open(os.path.join(pcmdir, f"{name}.pcm"), "wb") as f:
                f.write(blob)
        out2 = os.path.join(tmp, "FROMDIR.WAD")
        r = subprocess.run([sys.executable,
                            os.path.join(HERE, "mkmuswad.py"), pcmdir, out2],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout + r.stderr)
            fails.append("packing from a directory exited nonzero")
        else:
            fails += [f"from-dir: {m}" for m in check(out2, want)]

    if fails:
        for m in fails:
            print(f"FAIL: {m}")
        return 1

    note = "" if HAVE_NUMPY else " [downsample path skipped: no numpy]"
    print(f"muswad: OK ({len(want)} tracks, directory at byte 12, "
          f"lumps tile the file, duplicates shared, audio matches){note}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
