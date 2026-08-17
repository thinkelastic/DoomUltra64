"""Propose a VIEWLOCK pose looking at a given Doom thing type.

  python3 tools/thingpose.py assets/DOOM.WAD 2035        # explosive barrels

Walks the map's NODES to resolve which sector a point falls in, so the camera
is placed somewhere it can actually stand: same floor as the target, room
overhead, and a clear line to it.
"""
import struct, sys, math

EYE = 41

def lumps(path):
    d = open(path, 'rb').read()
    magic, n, off = struct.unpack_from('<4sii', d, 0)
    return d, [struct.unpack_from('<ii8s', d, off + i * 16) for i in range(n)]

def name(e): return e[2].rstrip(b'\x00').decode()

def maps(dirs):
    return [(name(e), i) for i, e in enumerate(dirs)
            if (len(name(e)) == 4 and name(e)[0] == 'E' and name(e)[2] == 'M')
            or (len(name(e)) == 5 and name(e).startswith('MAP'))]

def rd(d, dirs, i, nm):
    for j in range(i + 1, min(i + 12, len(dirs))):
        if name(dirs[j]) == nm:
            return d[dirs[j][0]:dirs[j][0] + dirs[j][1]]
    return b''

def unpack_all(buf, fmt):
    sz = struct.calcsize(fmt)
    return [struct.unpack_from(fmt, buf, k * sz) for k in range(len(buf) // sz)]

def analyse(path, thingtype, want_map=None):
    d, dirs = lumps(path)
    for nm, i in maps(dirs):
        if want_map and nm != want_map:
            continue
        things = unpack_all(rd(d, dirs, i, 'THINGS'),   '<hhhhh')
        lines  = unpack_all(rd(d, dirs, i, 'LINEDEFS'), '<HHHHHHH')
        sides  = unpack_all(rd(d, dirs, i, 'SIDEDEFS'), '<hh8s8s8sH')
        segs   = unpack_all(rd(d, dirs, i, 'SEGS'),     '<HHhHhh')
        ssecs  = unpack_all(rd(d, dirs, i, 'SSECTORS'), '<HH')
        nodes  = unpack_all(rd(d, dirs, i, 'NODES'),    '<hhhh8h2H')
        secs   = unpack_all(rd(d, dirs, i, 'SECTORS'),  '<hh8s8shhh')
        if not nodes or not secs:
            continue

        def sector_at(x, y):
            n = len(nodes) - 1
            while not (n & 0x8000):
                nx, ny, ndx, ndy = nodes[n][0:4]
                side = 0 if (ndx * (y - ny) - ndy * (x - nx)) > 0 else 1
                n = nodes[n][12 + side]
            ss = ssecs[n & 0x7FFF]
            sg = segs[ss[1]]
            ld, sd = lines[sg[3]], sg[4]
            si = ld[5] if sd == 0 else ld[6]
            return sides[si][5] if si != 0xFFFF else None

        for (tx, ty, ang, tt, flags) in things:
            if tt != thingtype:
                continue
            ts = sector_at(tx, ty)
            if ts is None:
                continue
            tfh, tch = secs[ts][0], secs[ts][1]
            # stand where the floor matches and there is room overhead
            for dist in (128, 176, 224, 288):
                for k in range(16):
                    a = k * math.pi / 8.0
                    px, py = tx + math.cos(a) * dist, ty + math.sin(a) * dist
                    ps = sector_at(px, py)
                    if ps is None:
                        continue
                    pfh, pch = secs[ps][0], secs[ps][1]
                    if abs(pfh - tfh) > 24 or pch - pfh < 64:
                        continue
                    face = (a + math.pi) % (2 * math.pi)
                    print('%-6s thing at %5d,%5d sec=%-4d floor=%-5d light=%-3d | '
                          'dist=%-4d VIEWLOCK=%d,%d,%d,%d MAP=%s'
                          % (nm, tx, ty, ts, tfh, secs[ts][4], dist,
                             round(px), round(py), pfh + EYE, round(face * 1000),
                             nm[1] + ',' + nm[3] if nm[0] == 'E' else '1,' + nm[3:]))
                    break
                else:
                    continue
                break

analyse(sys.argv[1], int(sys.argv[2]),
        sys.argv[3] if len(sys.argv) > 3 else None)
