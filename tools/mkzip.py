#!/usr/bin/env python3
"""Zip a staged directory tree, keeping empty directories.

    mkzip.py OUT.zip ROOTDIR TOPDIR

Zips ROOTDIR/TOPDIR into OUT.zip with entries rooted at TOPDIR/.  The
standard `zipfile -c` CLI would almost do, but the release layout ships an
empty Doom/saves/ and the CLI does not reliably store directory entries --
and the toolchain image carries no `zip` binary at all, which is why this
exists.
"""
import os
import sys
import zipfile


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    out, rootdir, top = sys.argv[1], sys.argv[2], sys.argv[3]

    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for root, dirs, files in os.walk(os.path.join(rootdir, top)):
            rel = os.path.relpath(root, rootdir)
            if not dirs and not files:
                # A trailing slash is what marks a directory entry.
                z.writestr(zipfile.ZipInfo(rel + "/"), b"")
            for f in sorted(files):
                z.write(os.path.join(root, f), os.path.join(rel, f))
    return 0


if __name__ == "__main__":
    sys.exit(main())
