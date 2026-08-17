"""Enumerate light-shaft sky wells in a Doom WAD, and propose a camera pose
for each: a spot in a ROOFED neighbouring sector looking at the well.

Mirrors D_BuildSkyWells (d_bridge.c) and r_shaft_add's rejects (r_halo.c).
"""
import struct, sys, math

SKY = b'F_SKY1\x00\x00'
SHAFT_MAX_SPAN = 512.0
SHAFT_MIN_LIGHT = 96
EYE = 41          # Doom's VIEWHEIGHT

def lumps(path):
    d = open(path, 'rb').read()
    magic, n, off = struct.unpack_from('<4sii', d, 0)
    dirs = []
    for i in range(n):
        fp, sz, nm = struct.unpack_from('<ii8s', d, off + i * 16)
        dirs.append((nm.rstrip(b'\x00').decode(), fp, sz))
    return d, dirs

def maps(dirs):
    out = []
    for i, (nm, fp, sz) in enumerate(dirs):
        if (len(nm) == 4 and nm[0] == 'E' and nm[2] == 'M') or \
           (len(nm) == 5 and nm.startswith('MAP')):
            out.append((nm, i))
    return out

def rd(d, dirs, i, name):
    for j in range(i + 1, min(i + 12, len(dirs))):
        if dirs[j][0] == name:
            return d[dirs[j][1]:dirs[j][1] + dirs[j][2]]
    return b''

def analyse(path):
    d, dirs = lumps(path)
    for nm, i in maps(dirs):
        verts = [struct.unpack_from('<hh', v, k * 4)
                 for v in [rd(d, dirs, i, 'VERTEXES')]
                 for k in range(len(v) // 4)]
        sides = [struct.unpack_from('<hh8s8s8sH', s, k * 30)
                 for s in [rd(d, dirs, i, 'SIDEDEFS')]
                 for k in range(len(s) // 30)]
        lines = [struct.unpack_from('<HHHHHHH', l, k * 14)
                 for l in [rd(d, dirs, i, 'LINEDEFS')]
                 for k in range(len(l) // 14)]
        secs  = [struct.unpack_from('<hh8s8shhh', s, k * 26)
                 for s in [rd(d, dirs, i, 'SECTORS')]
                 for k in range(len(s) // 26)]
        if not secs:
            continue

        sky  = [s[3] == SKY for s in secs]
        well = list(sky)
        # D_BuildSkyWells: sky on both sides of a two-sided line disqualifies both
        for (v1, v2, fl, sp, tg, rs, ls) in lines:
            if rs == 0xFFFF or ls == 0xFFFF:
                continue
            fs, bs = sides[rs][5], sides[ls][5]
            if sky[fs] and sky[bs]:
                well[fs] = well[bs] = False

        # bbox of each sector, from the vertices of its lines
        bb = {}
        touch = {}   # sector -> [(neighbour, line)]
        for li, (v1, v2, fl, sp, tg, rs, ls) in enumerate(lines):
            ss = []
            if rs != 0xFFFF: ss.append(sides[rs][5])
            if ls != 0xFFFF: ss.append(sides[ls][5])
            for s in ss:
                x0, y0 = verts[v1]; x1, y1 = verts[v2]
                b = bb.get(s)
                if b is None:
                    bb[s] = [min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1)]
                else:
                    b[0] = min(b[0], x0, x1); b[1] = min(b[1], y0, y1)
                    b[2] = max(b[2], x0, x1); b[3] = max(b[3], y0, y1)
            if len(ss) == 2 and ss[0] != ss[1]:
                touch.setdefault(ss[0], []).append((ss[1], li))
                touch.setdefault(ss[1], []).append((ss[0], li))

        for s in range(len(secs)):
            if not well[s] or s not in bb:
                continue
            fh, ch, fp, cp, light, spec, tag = secs[s]
            x0, y0, x1, y1 = bb[s]
            span = max(x1 - x0, y1 - y0)
            if span > SHAFT_MAX_SPAN:  continue      # open sky, not a well
            if light < SHAFT_MIN_LIGHT: continue     # too dark to throw a beam
            if ch - fh < 32:            continue     # no room to fall
            cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0

            # A roofed neighbour to stand in, and a pose looking at the well.
            for (nb, li) in touch.get(s, []):
                if sky[nb]:
                    continue                          # still outdoors
                nfh, nch = secs[nb][0], secs[nb][1]
                if nch - nfh < 64:
                    continue                          # no room to stand
                v1, v2 = lines[li][0], lines[li][1]
                ax, ay = verts[v1]; bx, by = verts[v2]
                mx, my = (ax + bx) / 2.0, (ay + by) / 2.0
                # step from the shared line into the NEIGHBOUR, away from the well
                dx, dy = mx - cx, my - cy
                L = math.hypot(dx, dy) or 1.0
                nbb = bb.get(nb)
                depth = 160.0
                px, py = mx + dx / L * depth, my + dy / L * depth
                if nbb and not (nbb[0] + 8 < px < nbb[2] - 8 and
                                nbb[1] + 8 < py < nbb[3] - 8):
                    depth = 72.0                      # small room: stay inside
                    px, py = mx + dx / L * depth, my + dy / L * depth
                ang = math.atan2(cy - py, cx - px) % (2 * math.pi)
                print('%-6s well sec=%-4d %4dx%-4d span=%-4d floor=%-5d ceil=%-5d '
                      'light=%-3d | stand sec=%d floor=%d  VIEWLOCK=%d,%d,%d,%d  MAP=%s'
                      % (nm, s, x1 - x0, y1 - y0, span, fh, ch, light,
                         nb, nfh, round(px), round(py), nfh + EYE,
                         round(ang * 1000),
                         nm[1] + ',' + nm[3] if nm[0] == 'E' else '1,' + nm[3:]))
                break

for p in sys.argv[1:]:
    print('=== %s' % p)
    analyse(p)
