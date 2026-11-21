#!/usr/bin/env python3
"""Take the game data out of SonAdvance-nx's ELF; write the table that puts
it back from the player's ROM at startup.

Every run of 16 or more bytes in the game's data (.sonadv_romdata, and the
decomp's own input sections of .data) that is byte for byte in the Europe ROM
is zeroed in the ELF. The table (romfill.bin, stored in the NRO's RomFS)
holds only positions:
  - Europe ROM: copy len bytes from ROM offset src to address dst.
  - USA ROM: the same data rebuilt from the USA ROM: copies from where the
    same bytes sit in the USA ROM, plus a small literal patch for the few
    bytes the USA ROM lacks (budgeted, listed in the report), and skipped
    ranges for Europe-only link-cable data the port cannot use.
Slots the loader writes at startup (RELA/RELR relocations) are never touched.

The tool proves its own output before writing it: it rebuilds the data from
the stripped ELF plus each ROM and compares with the original ELF, byte for
byte (USA: everything except the allowed skip ranges). Any mismatch, or a
patch over budget, stops the build.

usage: romfill.py <in.elf> <map> <eu.gba> <us.gba> <out.elf> <out_romfs_dir> <report>
"""
import struct, sys, os, hashlib, zlib, bisect, subprocess, collections

W = 16
LIT_BUDGET = int(os.environ.get('ROMFILL_LIT_BUDGET', '4096'))
# Europe ROM ranges with no USA counterpart that the port cannot use:
# Tiny Chao Garden program, multiboot programs, Chao message box tiles.
EU_SKIP = [(0x0009C170, 0x0009C170 + 0x1EDD4),
           (0x007C0258, 0x007D7E80 + 0x8000),
           (0x0044BF00, 0x0044F300)]
EU_SHA1 = 'eb00f101af23d728075ac2117e27ecd8a4b4c3e9'
US_SHA1 = 'd842afa7dd1e84de08addb94a51506f1bcafd551'

def die(msg):
    print('romfill: FAIL:', msg)
    sys.exit(1)

elf_path, map_path, eu_path, us_path, out_elf, out_dir, rep_path = sys.argv[1:8]
elf = bytearray(open(elf_path, 'rb').read())
orig = bytes(elf)
eu = open(eu_path, 'rb').read()
us = open(us_path, 'rb').read()
if hashlib.sha1(eu).hexdigest() != EU_SHA1: die('Europe ROM hash mismatch')
if hashlib.sha1(us).hexdigest() != US_SHA1: die('USA ROM hash mismatch')

# ---- ELF sections
shoff, = struct.unpack_from('<Q', elf, 0x28)
shentsize, shnum, shstrndx = struct.unpack_from('<HHH', elf, 0x3A)
secs = []
for i in range(shnum):
    name, typ, flags, addr, off, size = struct.unpack_from('<IIQQQQ', elf, shoff + i * shentsize)
    secs.append([name, typ, flags, addr, off, size])
stro = secs[shstrndx][4]
for s in secs:
    e = elf.index(b'\0', stro + s[0]); s[0] = elf[stro + s[0]:e].decode()
S = {s[0]: s for s in secs}
if '.sonadv_romdata' not in S: die('no .sonadv_romdata section (linker script not used?)')

def v2o(va):
    for n, t, f, a, o, sz in secs:
        if t != 8 and a <= va < a + sz and f & 2: return o + va - a
    return None

# ---- relocation slots (never strip): RELA and RELR
reloc = set()
if '.rela.dyn' in S:
    _, _, _, a, o, sz = S['.rela.dyn']
    for i in range(0, sz, 24):
        off, = struct.unpack_from('<Q', elf, o + i); reloc.add(off)
if '.relr.dyn' in S:
    _, _, _, a, o, sz = S['.relr.dyn']
    where = 0
    for i in range(0, sz, 8):
        ent, = struct.unpack_from('<Q', elf, o + i)
        if ent & 1 == 0:
            reloc.add(ent); where = ent + 8
        else:
            bits = ent >> 1
            for k in range(63):
                if bits & (1 << k): reloc.add(where + k * 8)
            where += 63 * 8
reloc_bytes = set()
for r in reloc:
    for k in range(8): reloc_bytes.add(r + k)

# ---- ranges to consider: all of .sonadv_romdata, plus game input sections of .data
ranges = [(S['.sonadv_romdata'][3], S['.sonadv_romdata'][3] + S['.sonadv_romdata'][5])]
for line in open(map_path, errors='ignore'):
    p = line.split()
    if len(p) == 4 and p[0].startswith('.data') and not p[0].startswith('.data.rel.ro') \
            and p[1].startswith('0x') and p[2].startswith('0x'):
        path = p[3]
        if path.startswith('/') or path.startswith('src/platform/'): continue
        a = int(p[1], 16); n = int(p[2], 16)
        if n >= W: ranges.append((a, a + n))
ranges.sort()

# ---- ROM indexes: 16-byte windows at 4-byte aligned ROM offsets
def index(rom):
    """window -> offset, or a list of offsets when the window repeats."""
    d = {}
    for i in range(0, len(rom) - W + 1, 4):
        w = rom[i:i + W]
        v = d.get(w)
        if v is None: d[w] = i
        elif isinstance(v, list):
            if len(v) < 256: v.append(i)
        else: d[w] = [v, i]
    return d

def low(w): return len(set(w)) <= 2

print('romfill: indexing ROMs')
EI = index(eu)

def find_seed(data, p, idx, lo, rom=None):
    """Window at p..p+3 that matches an aligned ROM window; return (pos, romoff).
    With several copies in the ROM, take the one that matches furthest on."""
    for k in range(p, p + 4):
        w = data[k:k + W]
        if len(w) < W or low(w): continue
        r = idx.get(bytes(w))
        if r is None: continue
        if not isinstance(r, list): return k, r
        best, bl = r[0], -1
        for c in r:
            e = k
            while e < len(data) and c + e - k < len(rom) and data[e] == rom[c + e - k]: e += 1
            if e - k > bl: best, bl = c, e - k
        return k, best
    return None

# ---- Europe runs
runs = []  # (va, len, eu_off)
for lo, hi in ranges:
    o = v2o(lo)
    if o is None: continue
    data = orig[o:o + (hi - lo)]
    p = 0; last = 0
    while p < len(data) - W + 1:
        if any((lo + p + k) in reloc_bytes for k in range(4)):
            p += 1; continue
        hit = find_seed(data, p, EI, last, eu)
        if not hit:
            p += 4; continue
        k, r = hit
        s, rs = k, r
        while s > last and rs > 0 and data[s - 1] == eu[rs - 1] and (lo + s - 1) not in reloc_bytes:
            s -= 1; rs -= 1
        e, re_ = k, r
        while e < len(data) and re_ < len(eu) and data[e] == eu[re_] and (lo + e) not in reloc_bytes:
            e += 1; re_ += 1
        if e - s >= W:
            runs.append((lo + s, e - s, rs))
            last = e; p = e
        else:
            p = k + 1
# Pass 2: short runs (16 to 18 bytes) the aligned index cannot seed, found
# with a set of every 16-byte window of the ROM at every offset.
print('romfill: pass 2 (every ROM offset)')
covered = []
for va, n, _ in runs: covered.append((va, va + n))
covered.sort()
starts = [c[0] for c in covered]
def is_covered(va):
    k = bisect.bisect_right(starts, va) - 1
    return k >= 0 and va < covered[k][1]
FULL = set(int.from_bytes(eu[i:i + W], 'little') for i in range(len(eu) - W + 1))
extra = 0
for lo, hi in ranges:
    o = v2o(lo)
    if o is None: continue
    data = orig[o:o + (hi - lo)]
    p = 0
    while p <= len(data) - W:
        va = lo + p
        w = data[p:p + W]
        if low(w) or is_covered(va) or is_covered(va + W - 1) or \
                any((va + k) in reloc_bytes for k in range(W)) or \
                int.from_bytes(w, 'little') not in FULL:
            p += 1; continue
        r = eu.find(bytes(w))
        e, re_ = p + W, r + W
        while e < len(data) and re_ < len(eu) and data[e] == eu[re_] and \
                (lo + e) not in reloc_bytes and not is_covered(lo + e):
            e += 1; re_ += 1
        runs.append((va, e - p, r)); extra += e - p
        p = e
del FULL
print(f'romfill: pass 2 added {extra} bytes')
runs.sort()
total = sum(n for _, n, _ in runs)
print(f'romfill: {len(runs)} runs, {total} bytes of game data')
if total < 4_000_000: die(f'only {total} bytes found; expected about 5 MB')

# ---- USA ops for each run
print('romfill: indexing USA ROM')
UI = index(us)
del EI
COPY, LIT, SKIP = 0, 1, 2
us_ops = []      # (type, va, src, len)  src: US offset or literal blob offset
lit = bytearray()
lit_where = collections.Counter()

def in_skip(eo):
    return any(a <= eo < b for a, b in EU_SKIP)

for va, n, eo in runs:
    run = orig[v2o(va):v2o(va) + n]
    i = 0; delta = None
    ops = []
    def add(t, d, s, l):
        if ops and ops[-1][0] == t and ops[-1][1] + ops[-1][3] == d and \
                (t == SKIP or ops[-1][2] + ops[-1][3] == s):
            ops[-1] = (t, ops[-1][1], ops[-1][2], ops[-1][3] + l)
        else:
            ops.append((t, d, s, l))
    # Short runs (pass 2) can be too short for the aligned index: look for the
    # whole run in the USA ROM first.
    whole = us.find(run) if n < 64 else -1
    if whole >= 0:
        us_ops.append((COPY, va, whole, n))
        continue
    pending = []   # positions not placed yet: resolved at the next match or the run end
    def resolve(cands):
        for j in pending:
            for d in cands:
                if d is not None and 0 <= j + d < len(us) and us[j + d] == run[j]:
                    add(COPY, va + j, j + d, 1); break
            else:
                if in_skip(eo + j): add(SKIP, va + j, 0, 1)
                else:
                    add(LIT, va + j, len(lit), 1); lit.append(run[j]); lit_where[va + j] += 1
        pending.clear()
    while i < n:
        # Keep following the current alignment while it matches: repeated
        # patterns (level maps) would otherwise send a fresh seed to another
        # copy of the same 16 bytes elsewhere in the ROM.
        if delta is not None and 0 <= i + delta < len(us) and us[i + delta] == run[i]:
            resolve([delta])
            e = i
            while e < n and 0 <= e + delta < len(us) and us[e + delta] == run[e]: e += 1
            add(COPY, va + i, i + delta, e - i)
            i = e
            continue
        hit = find_seed(run, i, UI, 0, us) if i <= n - W else None
        if hit:
            k, u = hit
            s, us_s = k, u
            while s > i and us_s > 0 and run[s - 1] == us[us_s - 1]: s -= 1; us_s -= 1
            pending.extend(range(i, s))
            resolve([us_s - s, delta])   # this match's alignment first, then the previous one
            e, ue = k, u
            while e < n and ue < len(us) and run[e] == us[ue]: e += 1; ue += 1
            add(COPY, va + s, us_s, e - s)
            delta = us_s - s
            i = e
        else:
            pending.append(i)
            i += 1
    resolve([delta])
    us_ops.extend(ops)

skip_bytes = sum(l for t, _, _, l in us_ops if t == SKIP)
print(f'romfill: USA: {len(us_ops)} ops, literal patch {len(lit)} bytes, skipped {skip_bytes} bytes')
if len(lit) > LIT_BUDGET: die(f'USA literal patch {len(lit)} bytes > budget {LIT_BUDGET}')

# ---- strip
for va, n, _ in runs:
    o = v2o(va); elf[o:o + n] = bytes(n)

# ---- prove: rebuild from each ROM and compare with the original
rd_lo, rd_hi = ranges[0][0], max(h for _, h in ranges)
def image_after(ops_apply):
    img = bytearray(elf)
    ops_apply(img)
    return img
def apply_eu(img):
    for va, n, eo in runs:
        o = v2o(va); img[o:o + n] = eu[eo:eo + n]
def apply_us(img):
    for t, va, s, l in us_ops:
        o = v2o(va)
        if t == COPY: img[o:o + l] = us[s:s + l]
        elif t == LIT: img[o:o + l] = lit[s:s + l]
img_eu = image_after(apply_eu)
if bytes(img_eu) != orig: die('Europe rebuild differs from the original ELF')
img_us = image_after(apply_us)
skipset = set()
for t, va, s, l in us_ops:
    if t == SKIP:
        o = v2o(va); skipset.update(range(o, o + l))
bad = [i for i in range(len(orig)) if img_us[i] != orig[i] and i not in skipset] if bytes(img_us) != orig else []
if bad: die(f'USA rebuild differs outside skip ranges at {len(bad)} bytes, first file offset {bad[0]:#x}')
print('romfill: proof OK: Europe rebuild identical; USA rebuild identical outside skip ranges')

# Expected CRCs for the runtime self-check: over exactly the bytes the fill
# writes, in table order. (Not the whole section: it also holds pointer slots
# the loader relocates to a different address on every launch.)
rs = S['.sonadv_romdata']; rn = rs[5]
crc_eu = 0
for va, n, eo in runs:
    o = v2o(va); crc_eu = zlib.crc32(bytes(img_eu[o:o + n]), crc_eu)
crc_us = 0
for t, va, s_, l in us_ops:
    if t != SKIP:
        o = v2o(va); crc_us = zlib.crc32(bytes(img_us[o:o + l]), crc_us)

# ---- table
os.makedirs(out_dir, exist_ok=True)
hdr = b'SAROMFL1' + struct.pack('<IQQIIIIII', 1, rs[3], rn, len(runs), len(us_ops), len(lit), crc_eu, crc_us, 0)
hdr += bytes.fromhex(EU_SHA1) + bytes.fromhex(US_SHA1)
body = b''.join(struct.pack('<QII', va, eo, n) for va, n, eo in runs)
body += b''.join(struct.pack('<IQII', t, va, s, l) for t, va, s, l in us_ops)
open(os.path.join(out_dir, 'romfill.bin'), 'wb').write(hdr + body + bytes(lit))
open(out_elf, 'wb').write(elf)

# ---- report
syms = []
for l in subprocess.run([os.environ.get('NM', 'nm'), '-S', '-n', elf_path], capture_output=True, text=True).stdout.split('\n'):
    p = l.split()
    if len(p) >= 3:
        try: syms.append((int(p[0], 16), p[-1]))
        except ValueError: pass
addrs = [a for a, _ in syms]
def sym(va):
    k = bisect.bisect_right(addrs, va) - 1
    return syms[k][1] if k >= 0 else '?'
with open(rep_path, 'w') as f:
    f.write(f'runs {len(runs)} bytes {total}\n')
    f.write(f'USA ops {len(us_ops)} literal {len(lit)} skipped {skip_bytes}\n')
    f.write(f'crc_eu {crc_eu:08x} crc_us {crc_us:08x}\n')
    f.write('literal patch by symbol:\n')
    by = collections.Counter()
    for va, c in lit_where.items(): by[sym(va)] += c
    for s_, c in by.most_common(): f.write(f'  {c:6d} {s_}\n')
print(f'romfill: wrote {out_elf}, {out_dir}/romfill.bin ({len(hdr) + len(body) + len(lit)} bytes)')
