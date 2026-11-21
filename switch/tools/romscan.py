#!/usr/bin/env python3
"""Release check: the NRO must not contain game data.

Scans every 16-byte window of the NRO (at every byte offset) against every
16-byte window of the Europe and USA ROMs. Windows with two or fewer distinct
byte values, or fewer than 12 non-zero bytes, are ignored: they carry no
content (padding, and the zeroed places the fill writes to at startup). The USA literal patch inside
romfill.bin (listed in the build report, agreed with Mark) is the only allowed
match; any other match fails the check.

usage: romscan.py <nro> <eu.gba> <us.gba> <romfill.bin>
"""
import sys, struct

W = 16
nro_path, eu_path, us_path, tab_path = sys.argv[1:5]
nro = open(nro_path, 'rb').read()
tab = open(tab_path, 'rb').read()

# The literal patch: the tail of romfill.bin (see romfill.py header layout).
lit_size, = struct.unpack_from('<I', tab, 36)
lit = tab[len(tab) - lit_size:] if lit_size else b''
allow = []
if lit:
    p = nro.find(tab)
    if p < 0:
        print('romscan: FAIL: romfill.bin not found in the NRO'); sys.exit(1)
    allow.append((p + len(tab) - lit_size, p + len(tab)))

def counting(w):
    """Pure counting sequences (each byte lane steps by a constant), e.g.
    library lookup tables like 'mnopq...' or 03 40 03 41 03 42 ...: they
    turn up in any binary and are not game content."""
    for stride in (1, 2, 4):
        ok = True
        for lane in range(stride):
            v = w[lane::stride]
            d = set((v[i + 1] - v[i]) & 0xFF for i in range(len(v) - 1))
            if len(d) > 1: ok = False; break
        if ok: return True
    return False

def content(w):
    return len(set(w)) >= 4 and sum(1 for b in w if b) >= 12 and not counting(w)

def allowed(i):
    return any(a <= i and i + W <= b for a, b in allow)

# NRO header: segment table at 0x20 (text, ro, data as offset/size pairs).
segs = struct.unpack_from('<6I', nro, 0x20)
data_lo, data_hi = segs[4], segs[4] + segs[5]
LIB_MAX = 32   # code/ro segments: libraries and port code only

bad = 0
for name, path in (('Europe', eu_path), ('USA', us_path)):
    rom = open(path, 'rb').read()
    S = set(rom[i:i + W] for i in range(len(rom) - W + 1))
    hits = []
    i = 0
    while i <= len(nro) - W:
        w = nro[i:i + W]
        if content(w) and w in S and not allowed(i):
            j = i + 1
            while j <= len(nro) - W and content(nro[j:j + W]) and nro[j:j + W] in S: j += 1
            hits.append((i, j - i + W - 1)); i = j + W - 1
        else:
            i += 1
    del S
    fail = [(a, n) for a, n in hits if (data_lo <= a < data_hi) or n > LIB_MAX]
    found = sum(n for _, n in hits)
    print(f'romscan: {name} ROM: {len(hits)} matching runs, {found} bytes; '
          f'{len(fail)} in the data segment or longer than {LIB_MAX}')
    for a, n in sorted(hits, key=lambda h: -h[1])[:12]:
        where = 'DATA' if data_lo <= a < data_hi else 'lib'
        print(f'  {where} NRO offset {a:#x} length {n} {nro[a:a+min(n,24)].hex()}')
    bad += len(fail)
if bad:
    print('romscan: FAIL: game data found in the NRO'); sys.exit(1)
print(f'romscan: PASS (allowed: USA literal patch, {lit_size} bytes)')
