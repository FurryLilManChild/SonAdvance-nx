#!/usr/bin/env python3
"""Turn a SonAdvance-nx instant replay (logs/replay_NNN_fFRAME.bin) into PNGs.

usage: replay.py replay.bin outdir [--every N]

Writes frame_NNN_fFRAME.png for each recorded frame (the 240x160 area in
classic view) and frames.txt with level, camera and the visible OAM entries.
Needs Pillow.
"""
import struct, sys, os

def main():
    src, out = sys.argv[1], sys.argv[2]
    every = int(sys.argv[sys.argv.index('--every') + 1]) if '--every' in sys.argv else 1
    from PIL import Image
    data = open(src, 'rb').read()
    magic = data[:8]
    if magic != b'SANXRPL1':
        sys.exit('not a replay file')
    version, frames, fbytes, w, h, oam, io, pltt = struct.unpack_from('<8I', data, 8)
    os.makedirs(out, exist_ok=True)
    off = 8 + 32
    head = struct.calcsize('<QBBBBii')
    log = open(os.path.join(out, 'frames.txt'), 'w')
    for i in range(frames):
        rec = data[off + i * fbytes: off + (i + 1) * fbytes]
        frame, classic, level, char, stage, camx, camy = struct.unpack_from('<QBBBBii', rec, 0)
        img_off = head
        pix = struct.unpack_from('<%dH' % (w * h), rec, img_off)
        o = img_off + w * h * 2
        oamb = rec[o:o + oam]
        cw, ch = (240, 160) if classic else (w, h)
        if i % every == 0:
            im = Image.new('RGB', (cw, ch))
            px = im.load()
            for y in range(ch):
                for x in range(cw):
                    c = pix[y * w + x]
                    px[x, y] = ((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3)
            im.save(os.path.join(out, 'frame_%03d_f%d.png' % (i, frame)))
        objs = []
        for k in range(oam // 8):
            a0, a1, a2 = struct.unpack_from('<HHH', oamb, k * 8)
            if (a0 & 0x300) == 0x200:
                continue  # hidden
            objs.append('%d:(%d,%d)t%d' % (k, a1 & 0x1FF, a0 & 0xFF, a2 & 0x3FF))
        log.write('%3d f%d level %d char %d stage %d classic %d cam %d,%d objs %d %s\n' % (
            i, frame, level, char, stage, classic, camx, camy, len(objs), ' '.join(objs[:24])))
    print('%d frames -> %s' % (frames, out))

if __name__ == '__main__':
    main()
