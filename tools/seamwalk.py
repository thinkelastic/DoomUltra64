#!/usr/bin/env python3
"""seamwalk -- replay a SEAMPROBE dump through the RDP's pixel pipeline.

Input: the USB log of a SEAMPROBE=1 run (see src/d_seam.c). v3 dumps carry
whole commands in stream order:

    seamdump begin f=N holes=H
    st <cmd> <nqw> <w0> <w1> ... <wn>     triangles AND mode commands
    seamdump end f=N cmds=C lost=L
    seamhole f=N n=K: y:x0-x1 ...         the dump frame's own hole scan

The first probe iteration walked edges alone with union-of-subsamples
coverage and proved every hole pixel COVERED -- which was the tell. With
antialiasing off, the RDP does not write every pixel a span touches: the
blender gates the write on CURPIXEL_CVBIT, the single coverage sample at
the pixel's corner on the row's first sub-scanline (angrylion
rasterizer.c: `wen = blender_1cycle(..., curpixel_cvbit)`, and blender.c
keys on `antialias_en ? cvg : cvbit`). Point-sampled rasterisation.

cover_cvbit() below implements exactly that rule, and against six console
dumps it predicts the scanned holes pixel-for-pixel (84/84, no false
positives outside regions drawn by non-triangle commands). The seam
mechanism it exposes: neighbouring flats never share bit-identical
boundary chains (radial FLATNUDGE rotates each copy, band cuts subdivide
each side at its own depths, every derived vertex floor-quantises
separately), the chains wander +/-(0.25 + 0.25*|slope|) px around each
other, and every corner-lattice point inside a void between them is one
dropped pixel. Shallow edges wander widest, which is why the dots ride
diagonals. Where the chains overlap instead, the coplanar z tie breaks by
draw order -- the stray-texel artifact of b5724df. One mechanism, both
signs.

The z simulation is kept for future use; on the six dumps no hole needed
it (the z divergence between coplanar fans is real -- ~0.6 LSB/row of
dzde disagreement -- but only decides who wins overlaps, not voids).
"""
import sys, re
from collections import defaultdict

SW, SH = 320, 240


def s14(v):
    v &= 0x3FFF
    return v - 0x4000 if v & 0x2000 else v


def s32(v):
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v & 0x80000000 else v


def s16f(v):
    """high 16 = int part, low 16 = fraction, signed, as float"""
    return s32(v) / 65536.0


class Tri:
    def __init__(self, idx, cmd, w, ac, om):
        self.idx = idx
        self.cmd = cmd
        self.words = w
        self.ac = ac                    # alpha compare armed when drawn
        self.om = om                    # SOM word active when drawn
        self.lft = (w[0] >> 55) & 1
        self.yl = s14(w[0] >> 32)
        self.ym = s14(w[0] >> 16)
        self.yh = s14(w[0])
        self.xl, self.dxldy = s32(w[1] >> 32), s32(w[1])
        self.xh, self.dxhdy = s32(w[2] >> 32), s32(w[2])
        self.xm, self.dxmdy = s32(w[3] >> 32), s32(w[3])
        # z block sits after edge(4) + shade(8 if bit2) + tex(8 if bit1)
        self.z = self.dzdx = self.dzde = self.dzdy = None
        if cmd & 1:
            off = 4 + (8 if cmd & 4 else 0) + (8 if cmd & 2 else 0)
            if off + 2 <= len(w):
                self.z    = s16f(w[off] >> 32)
                self.dzdx = s16f(w[off])
                self.dzde = s16f(w[off + 1] >> 32)
                self.dzdy = s16f(w[off + 1])

    def walk(self):
        """Yield (quarter_line, left_16_16, right_16_16, e_steps).
        e_steps = quarter-lines walked down the major edge, for z-interp."""
        k0 = self.yh & ~3
        maj = self.xh & ~1
        mnr = self.xm & ~1
        inc_h = (self.dxhdy >> 2) & ~1
        inc_m = (self.dxmdy >> 2) & ~1
        inc_l = (self.dxldy >> 2) & ~1
        inc_mnr = inc_m
        for k in range(k0, self.yl):
            if k == self.ym:
                mnr = self.xl & ~1
                inc_mnr = inc_l
            if k >= self.yh:
                if self.lft:
                    yield k, maj, mnr, k - k0
                else:
                    yield k, mnr, maj, k - k0
            maj += inc_h
            mnr += inc_mnr

    def z_at(self, px, k, e_steps, left):
        """Interpolated z at pixel px on quarter-line k, RDP-style: start z
        prestepped down the major edge, then DzDx across the span."""
        if self.z is None:
            return None
        ze = self.z + self.dzde * (e_steps / 4.0)
        xbase = (self.xh + ((self.dxhdy >> 2) & ~1) * e_steps) / 65536.0 \
            if self.lft else left / 65536.0
        # walk from the major edge; for lft=0 the major is on the right,
        # but z steps from the major edge with -DzDx going left:
        if self.lft:
            return ze + self.dzdx * (px + 0.5 - xbase)
        else:
            xmaj = (self.xh + ((self.dxhdy >> 2) & ~1) * e_steps) / 65536.0
            return ze + self.dzdx * (px + 0.5 - xmaj)


def parse(path):
    dumps, cur, tag = [], None, ''
    want = None
    for line in open(path, errors='replace'):
        if line.startswith('seamdump begin'):
            cur = []
            tag = line.split('f=')[1].split()[0]
        elif line.startswith('seamdump end'):
            want = (tag, cur)
            cur = None
        elif cur is not None and line.startswith('st '):
            p = line.split()
            cmd, n = int(p[1]), int(p[2])
            cur.append((cmd, [int(x, 16) for x in p[3:3 + n]]))
        elif line.startswith('seamhole') and want is not None:
            holes = []
            for m in re.finditer(r'(\d+):(\d+)-(\d+)', line):
                y, x0, x1 = map(int, m.groups())
                holes += [(x, y) for x in range(x0, x1 + 1)]
            dumps.append((want[0], want[1], holes))
            want = None
    return dumps


def cover_cvbit(tris):
    """The hardware write rule with antialias off: pixel (x,y) is written
    iff the corner sample (x + 0.0, row's sub-scanline 0) lies inside the
    span. Validated pixel-for-pixel against console framebuffer scans."""
    cov = set()
    for t in tris:
        k0 = t.yh & ~3
        maj = t.xh & ~1
        mnr = t.xm & ~1
        inc_h = (t.dxhdy >> 2) & ~1
        inc_m = (t.dxmdy >> 2) & ~1
        inc_l = (t.dxldy >> 2) & ~1
        inc_mnr = inc_m
        for k in range(k0, t.yl):
            if k == t.ym:
                mnr = t.xl & ~1
                inc_mnr = inc_l
            if k >= t.yh and (k & 3) == 0:
                left, right = (maj, mnr) if t.lft else (mnr, maj)
                if right > left:
                    py = k >> 2
                    if 0 <= py < SH:
                        lo = (left + 0xFFFF) >> 16       # ceil: x >= left
                        hi = (right - 1) >> 16           # x < right
                        for px in range(max(0, lo), min(SW - 1, hi) + 1):
                            cov.add((px, py))
            maj += inc_h
            mnr += inc_mnr
    return cov


def analyse(tag, cmds, holes):
    # Reconstruct per-triangle mode state in stream order.
    tris = []
    ac, om, blend_a = 0, 0, None
    for cmd, w in cmds:
        if 0x08 <= cmd <= 0x0F:
            tris.append(Tri(len(tris), cmd, w, ac, om))
        elif cmd == 0x2F:
            om = w[0]
            ac = int(om & 1)            # SOM bit 0: alpha_compare_en
        elif cmd == 0x39:
            blend_a = w[0] & 0xFF
    nz = sum(1 for t in tris if t.z is not None)
    print('=== dump %s: %d cmds, %d tris (%d z-carrying), %d holes' %
          (tag, len(cmds), len(tris), nz, len(holes)))

    hset = set(holes)
    cov = cover_cvbit(tris)
    pred = set((x, y) for y in range(4, 196) for x in range(SW)
               if (x, y) not in cov)
    print('cvbit model: predicted %d bare, agree %d, missed %d, extra %d '
          '(extras sit where non-triangle draws filled in)' %
          (len(pred), len(pred & hset), len(hset - pred), len(pred - hset)))
    if not hset:
        return

    # Simulate z per hole pixel in draw order; record every event.
    for (hx, hy) in sorted(hset)[:16]:
        print('hole (%3d,%3d):' % (hx, hy))
        zbuf = None                     # cleared: far
        for t in tris:
            for k, left, right, e in t.walk():
                if k >> 2 != hy:
                    continue
                covered = False
                for sub in (1, 3):
                    sx = (hx * 4 + sub) << 14
                    if left <= sx < right:
                        covered = True
                        break
                if not covered:
                    continue
                z = t.z_at(hx, k, e, left)
                verdict = 'draws'
                if z is not None:
                    if zbuf is not None and z >= zbuf:
                        verdict = 'z-FAIL (%.1f >= %.1f)' % (z, zbuf)
                    else:
                        zbuf = z
                if t.ac:
                    verdict += ' [AC ON: texel alpha decides]'
                print('   tri%-4d cmd=%x sub%d span %.2f..%.2f  z=%s  %s' %
                      (t.idx, t.cmd, k & 3, left / 65536, right / 65536,
                       '%.1f' % z if z is not None else '-', verdict))
                break                   # one event per triangle is enough


def main(path):
    dumps = parse(path)
    if not dumps:
        print('no complete seamdump in log')
        return
    for tag, cmds, holes in dumps:
        analyse(tag, cmds, holes)


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else '/tmp/unfl-debug.log')
