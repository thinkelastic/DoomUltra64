#!/usr/bin/env python3
"""Build a music WAD from the music inside an IWAD.

Doom stores its music as MUS -- a compacted MIDI variant -- and the console
plays PCM, so the tracks have to be rendered somewhere. This does it on the
host: MUS to MIDI, MIDI to 48 kHz stereo through a synthesiser, then straight
into mkmuswad.py for the downsample and packing.

Any GUS patch set works. The 2024 rereleases ship their own inside
Common.kpf, which is a zip -- point --gus at either that file or a directory
and the tracks come out with that release's instruments rather than a generic
synth's. (The rereleases do not ship recorded audio: their music is still MUS,
and the instrument set is the whole difference.)

Needs wildmidi on PATH.

    tools/mkmusfromiwad.py doom.wad DOOMMUS.WAD --gus /path/to/Common.kpf
"""
import argparse, hashlib, os, shutil, struct, subprocess, sys, tempfile, wave, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
CTRL = {0: 0x00, 1: 0x00, 2: 0x01, 3: 0x07, 4: 0x0A, 5: 0x0B, 6: 0x5B,
        7: 0x5D, 8: 0x40, 9: 0x43, 10: 0x78, 11: 0x7B, 12: 0x7E, 13: 0x7F,
        14: 0x79}


def vlq(n):
    out = bytearray([n & 0x7F]); n >>= 7
    while n:
        out.insert(0, (n & 0x7F) | 0x80); n >>= 7
    return bytes(out)


def mus2mid(data):
    """MUS -> MIDI. Every synthesiser speaks MIDI and none speak MUS."""
    if data[:4] != b'MUS\x1a':
        raise ValueError('not a MUS lump')
    sstart = struct.unpack('<H', data[6:8])[0]
    pos, delay, trk = sstart, 0, bytearray()
    vol = [100] * 16
    ch_midi = lambda c: 9 if c == 15 else (c if c < 9 else c + 1)   # 15 is drums
    while pos < len(data):
        ev = data[pos]; pos += 1
        last, typ, ch = ev >> 7, (ev >> 4) & 7, ev & 15
        mc = ch_midi(ch)
        if typ == 0:
            n = data[pos]; pos += 1
            trk += vlq(delay) + bytes([0x80 | mc, n & 0x7F, 64])
        elif typ == 1:
            n = data[pos]; pos += 1
            if n & 0x80:
                vol[ch] = data[pos] & 0x7F; pos += 1
            trk += vlq(delay) + bytes([0x90 | mc, n & 0x7F, vol[ch]])
        elif typ == 2:
            bend = data[pos] * 64; pos += 1
            trk += vlq(delay) + bytes([0xE0 | mc, bend & 0x7F, (bend >> 7) & 0x7F])
        elif typ == 3:
            c = data[pos] & 0x7F; pos += 1
            trk += vlq(delay) + bytes([0xB0 | mc, CTRL.get(c, 0), 0])
        elif typ == 4:
            c = data[pos] & 0x7F; pos += 1
            v = data[pos] & 0x7F; pos += 1
            trk += vlq(delay) + (bytes([0xC0 | mc, v]) if c == 0
                                 else bytes([0xB0 | mc, CTRL.get(c, 0), v]))
        elif typ == 6:
            break
        delay = 0
        if last:
            d = 0
            while True:
                b = data[pos]; pos += 1
                d = (d << 7) | (b & 0x7F)
                if not b & 0x80:
                    break
            delay = d
    trk += vlq(0) + b'\xFF\x2F\x00'
    return (b'MThd' + struct.pack('>IHHH', 6, 0, 1, 70)
            + b'MTrk' + struct.pack('>I', len(trk)) + bytes(trk))


def music_lumps(path):
    d = open(path, 'rb').read()
    n, off = struct.unpack('<II', d[4:12])
    out = []
    for i in range(n):
        e = off + 16 * i
        p, sz = struct.unpack('<II', d[e:e + 8])
        nm = d[e + 8:e + 16].split(b'\0')[0].decode('latin1')
        if nm.startswith('D_') and sz:
            out.append((nm, d[p:p + sz]))
    return out


def prepare_gus(src, work):
    """Lay out a patch set wildmidi can read, from a directory or a .kpf zip."""
    inst = os.path.join(work, 'gus')
    if zipfile.is_zipfile(src):
        with zipfile.ZipFile(src) as z:
            names = [n for n in z.namelist() if n.upper().startswith('GUS/')]
            if not names:
                sys.exit(f'{src}: no GUS/ patches inside')
            z.extractall(work, members=names)
        root = os.path.join(work, 'GUS')
    else:
        root = src
    cfg = None
    for dirpath, _, files in os.walk(root):
        for f in files:
            if f.lower().endswith('.cfg'):
                cfg = os.path.join(dirpath, f)
            if f.lower().endswith('.pat'):
                os.makedirs(inst, exist_ok=True)
                dst = os.path.join(inst, f.lower())
                # The configs name patches in lower case and the archives
                # store them upper; on a case-sensitive filesystem that is
                # the difference between music and silence.
                if not os.path.exists(dst):
                    shutil.copy(os.path.join(dirpath, f), dst)
    if not cfg:
        sys.exit(f'{src}: found no instrument .cfg')
    out = os.path.join(work, 'wildmidi.cfg')
    with open(out, 'w') as f:
        f.write(f'dir {inst}\nsource {cfg}\n')
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('iwad'), ap.add_argument('out')
    ap.add_argument('--gus', required=True,
                    help='GUS patch directory, or a .kpf/zip containing GUS/')
    a = ap.parse_args()
    if not shutil.which('wildmidi'):
        sys.exit('wildmidi not found on PATH')

    with tempfile.TemporaryDirectory(prefix='mkmus') as work:
        cfg = prepare_gus(a.gus, work)
        lumps = music_lumps(a.iwad)
        print(f'{len(lumps)} music lumps in {os.path.basename(a.iwad)}')

        # Doom reuses tracks across levels; render each distinct one once.
        seen, tracks = {}, []
        for nm, mus in lumps:
            h = hashlib.sha1(mus).hexdigest()[:12]
            if h not in seen:
                mid = os.path.join(work, h + '.mid')
                wav = os.path.join(work, h + '.wav')
                raw = os.path.join(work, h + '.raw')
                open(mid, 'wb').write(mus2mid(mus))
                subprocess.run(['wildmidi', '-c', cfg, '-o', wav, '-r', '48000',
                                mid], check=True, capture_output=True)
                w = wave.open(wav)
                if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (48000, 2, 2):
                    sys.exit('unexpected render format')
                open(raw, 'wb').write(w.readframes(w.getnframes()))
                w.close(); os.remove(wav); os.remove(mid)
                seen[h] = raw
                print(f'  {nm:9s} {os.path.getsize(raw) // 1024:7d} KB')
            else:
                print(f'  {nm:9s} shares audio with an earlier track')
            # The port's rule (see d_bridge.c): the lump name with D_ for P.
            tracks.append((('P' + nm[2:].upper())[:8], seen[h]))

        # A source WAD for mkmuswad.py: PCMINFO plus one lump per track,
        # duplicates pointing at one run of bytes.
        src = os.path.join(work, 'source.wad')
        body, offs = bytearray(), {}
        for _, raw in tracks:
            if raw not in offs:
                offs[raw] = 12 + len(body)
                body += open(raw, 'rb').read()
        info = 12 + len(body)
        body += struct.pack('<II', 48000, 2)
        ents = [('PCMINFO', info, 8)] + [(n, offs[r], os.path.getsize(r))
                                         for n, r in tracks]
        with open(src, 'wb') as f:
            f.write(b'PWAD' + struct.pack('<II', len(ents), 12 + len(body)))
            f.write(body)
            for n, p, s in ents:
                f.write(struct.pack('<II', p, s) + n.encode().ljust(8, b'\0'))

        subprocess.run([sys.executable, os.path.join(HERE, 'mkmuswad.py'),
                        src, a.out], check=True)


if __name__ == '__main__':
    main()
