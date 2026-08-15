#!/usr/bin/env python3
"""Measure how far each seg's own clip line sits from its linedef's line.

build_polys (r_ssdata.c) trims a BSP cell by the infinite line through the
SEG's two vertices. Doom's node builder writes split vertices back to the
VERTEXES lump as 16-bit INTEGERS, so a split endpoint is rounded off the
true linedef by up to ~0.7 units -- and a short seg with a rounded endpoint
defines a line whose ANGLE is wrong, which slices a wedge off a cell that
may extend hundreds of units past the seg.

The linedef's own endpoints are original map vertices and are shared by the
subsectors on BOTH sides, so clipping by the linedef line instead makes the
two sides' boundaries land on one identical line.

Reports the divergence the seg line introduces, at the seg endpoints and at
a working distance out along the line.
"""
import struct, sys, math
from collections import Counter, defaultdict

def lumps(path):
    d = open(path, 'rb').read()
    magic, n, off = struct.unpack_from('<4sii', d, 0)
    out = []
    for i in range(n):
        fo, sz, nm = struct.unpack_from('<ii8s', d, off + 16 * i)
        out.append((nm.rstrip(b'\0').decode('ascii', 'replace'), fo, sz))
    return d, out

def maps_in(dirents):
    """Map marker -> {lumpname: (off,size)} for the lumps that follow it."""
    want = ('VERTEXES', 'LINEDEFS', 'SEGS', 'SSECTORS', 'NODES', 'SIDEDEFS')
    res = {}
    for i, (nm, fo, sz) in enumerate(dirents):
        if not (len(nm) == 4 and nm[0] == 'E' and nm[2] == 'M') and \
           not (len(nm) == 5 and nm.startswith('MAP')):
            continue
        sub = {}
        for nm2, fo2, sz2 in dirents[i + 1:i + 12]:
            if nm2 in want:
                sub[nm2] = (fo2, sz2)
        if 'SEGS' in sub:
            res[nm] = sub
    return res

def load(d, sub):
    vo, vs = sub['VERTEXES']
    verts = [struct.unpack_from('<hh', d, vo + 4 * i) for i in range(vs // 4)]
    lo, ls = sub['LINEDEFS']
    lines = [struct.unpack_from('<HHHHHHH', d, lo + 14 * i) for i in range(ls // 14)]
    so, ss = sub['SEGS']
    segs = [struct.unpack_from('<HHhHhh', d, so + 12 * i) for i in range(ss // 12)]
    return verts, lines, segs

def main():
    wad = sys.argv[1] if len(sys.argv) > 1 else 'assets/DOOM.WAD'
    d, dirents = lumps(wad)
    ms = maps_in(dirents)

    REACH = 128.0        # how far past the seg a cell edge plausibly runs
    tot = Counter()
    permap = defaultdict(Counter)
    worst = []

    for mname in sorted(ms):
        verts, lines, segs = load(d, ms[mname])
        for si, (v1, v2, ang, ldnum, side, off) in enumerate(segs):
            if ldnum >= len(lines):
                continue
            ax, ay = verts[v1]
            bx, by = verts[v2]
            lv1, lv2 = lines[ldnum][0], lines[ldnum][1]
            if lv1 >= len(verts) or lv2 >= len(verts):
                continue
            px, py = verts[lv1]
            qx, qy = verts[lv2]

            ldx, ldy = qx - px, qy - py
            llen = math.hypot(ldx, ldy)
            if llen < 1e-6:
                continue
            # perpendicular offset of each seg endpoint from the TRUE line
            d1 = ((ax - px) * ldy - (ay - py) * ldx) / llen
            d2 = ((bx - px) * ldy - (by - py) * ldx) / llen

            sdx, sdy = bx - ax, by - ay
            slen = math.hypot(sdx, sdy)
            if slen < 1e-6:
                tot['degenerate'] += 1
                continue

            # Angle between the seg's line and the linedef's line, and the
            # divergence that angle produces REACH units along the line.
            cross = abs(sdx * ldy - sdy * ldx) / (slen * llen)
            cross = min(1.0, cross)
            ang_err = math.asin(cross)
            reach_err = math.tan(ang_err) * REACH + max(abs(d1), abs(d2))

            # Only a TWO-SIDED line has a neighbouring floor to disagree
            # with; a sliver behind a one-sided wall is never seen.
            two = lines[ldnum][5] != 0xFFFF and lines[ldnum][6] != 0xFFFF

            tot['segs'] += 1
            if two:
                tot['segs_2s'] += 1
            if abs(d1) > 1e-9 or abs(d2) > 1e-9:
                tot['off_line'] += 1
            for thr in (0.05, 0.25, 0.5, 1.0):
                if max(abs(d1), abs(d2)) > thr:
                    tot['endpt>%g' % thr] += 1
                if reach_err > thr:
                    tot['reach>%g' % thr] += 1
                    if two:
                        tot['2s_reach>%g' % thr] += 1
                        permap[mname][thr] += 1
            worst.append((reach_err, mname, si, slen, d1, d2,
                          math.degrees(ang_err), two))

    worst.sort(reverse=True)
    print("WAD: %s   maps: %d" % (wad, len(ms)))
    print("segs total          %6d" % tot['segs'])
    print("endpoint off line   %6d  (%.1f%%)" %
          (tot['off_line'], 100.0 * tot['off_line'] / max(1, tot['segs'])))
    for thr in (0.05, 0.25, 0.5, 1.0):
        print("  endpoint dev >%4g  %6d      line dev >%4g @%gu  %6d" %
              (thr, tot['endpt>%g' % thr], thr, REACH, tot['reach>%g' % thr]))
    print("\nTWO-SIDED only (a neighbouring floor exists to disagree with):")
    print("  segs on two-sided lines %6d" % tot['segs_2s'])
    for thr in (0.05, 0.25, 0.5, 1.0):
        print("    line dev >%4g @%gu   %6d" %
              (thr, REACH, tot['2s_reach>%g' % thr]))

    print("\nper map, two-sided segs with line dev >0.5u @%gu:" % REACH)
    rows = sorted(permap.items(), key=lambda kv: -kv[1][0.5])
    for mn, c in rows[:10]:
        print("    %-6s %4d" % (mn, c[0.5]))

    print("\nworst 12 by divergence %gu along the line:" % REACH)
    print("  %-6s %-6s %8s %8s %8s %9s %8s %5s" %
          ("map", "seg", "seglen", "d1", "d2", "angerr", "reachdev", "2side"))
    for r, mn, si, sl, d1, d2, ae, two in worst[:12]:
        print("  %-6s %-6d %8.1f %8.3f %8.3f %8.4f° %8.2f %5s" %
              (mn, si, sl, d1, d2, ae, r, "yes" if two else "no"))

main()
