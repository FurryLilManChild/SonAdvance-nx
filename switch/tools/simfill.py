#!/usr/bin/env python3
"""Simulate SonAdvance-nx's startup fill (romfill.c) on a finished NRO.

Loads the NRO's segments as the Switch does (address = NRO offset), reads
romfill.bin from the NRO with the same header offsets as romfill.c, applies
the ops for each ROM, then checks:
  - Europe: the filled memory equals the original (unstripped) ELF's.
  - Both: the CRC of .sonadv_romdata equals the one stored in the table,
    which is what romfill.c checks on the console.

usage: simfill.py <nro> <romfill.bin> <unstripped.elf> <eu.gba> <us.gba>
"""
import struct, sys, zlib

nro_p, tab_p, elf_p, eu_p, us_p = sys.argv[1:6]
nro = open(nro_p, 'rb').read()
tab_file = open(tab_p, 'rb').read()
elf = open(elf_p, 'rb').read()
roms = {1: open(eu_p, 'rb').read(), 2: open(us_p, 'rb').read()}

segs = struct.unpack_from('<6I', nro, 0x20)
bss_size, = struct.unpack_from('<I', nro, 0x38)
mem_size = segs[4] + segs[5] + bss_size
base_img = bytearray(mem_size)
for i in range(3):
    off, size = segs[2 * i], segs[2 * i + 1]
    base_img[off:off + size] = nro[off:off + size]

p = nro.find(tab_file)
if p < 0: sys.exit('simfill: FAIL: romfill.bin not in the NRO')
tab = nro[p:p + len(tab_file)]

# Header exactly as romfill.c reads it.
assert tab[0:8] == b'SAROMFL1'
version, = struct.unpack_from('<I', tab, 8)
anchor, = struct.unpack_from('<Q', tab, 12)
data_size, = struct.unpack_from('<Q', tab, 20)
n_eu, n_us, lit_size, crc_eu, crc_us = struct.unpack_from('<5I', tab, 28)
HDR = 92
eu_ops = HDR
us_ops = eu_ops + n_eu * 16
lit = us_ops + n_us * 20
assert version == 1 and lit + lit_size == len(tab), 'size/version check'

# ELF image of the same range, for the Europe comparison.
shoff, = struct.unpack_from('<Q', elf, 0x28)
shentsize, shnum, _ = struct.unpack_from('<HHH', elf, 0x3A)
orig = bytearray(mem_size)
for i in range(shnum):
    name, typ, flags, addr, off, size = struct.unpack_from('<IIQQQQ', elf, shoff + i * shentsize)
    if flags & 2 and typ != 8 and addr + size <= mem_size:
        orig[addr:addr + size] = elf[off:off + size]

ok = True
for which, name in ((1, 'Europe'), (2, 'USA')):
    img = bytearray(base_img)
    rom = roms[which]
    base = 0  # in the NRO image, address == offset, so &__sonadv_romdata_start - anchor == 0
    crc = 0   # over exactly the bytes written, in table order, as romfill.c does
    if which == 1:
        for i in range(n_eu):
            dst, src, ln = struct.unpack_from('<QII', tab, eu_ops + i * 16)
            assert src + ln <= len(rom)
            img[base + dst:base + dst + ln] = rom[src:src + ln]
            crc = zlib.crc32(bytes(img[base + dst:base + dst + ln]), crc)
    else:
        for i in range(n_us):
            typ, dst, src, ln = struct.unpack_from('<IQII', tab, us_ops + i * 20)
            if typ == 0: img[base + dst:base + dst + ln] = rom[src:src + ln]
            elif typ == 1: img[base + dst:base + dst + ln] = tab[lit + src:lit + src + ln]
            if typ != 2: crc = zlib.crc32(bytes(img[base + dst:base + dst + ln]), crc)
    want = crc_eu if which == 1 else crc_us
    line = f'simfill: {name}: crc {crc:08x} (table {want:08x}) {"OK" if crc == want else "MISMATCH"}'
    if crc != want: ok = False
    if which == 1:
        lo = segs[4]; hi = segs[4] + segs[5]
        same = img[lo:hi] == orig[lo:hi]
        line += f'; data segment equals the original ELF: {same}'
        if not same: ok = False
    print(line)
print('simfill: PASS' if ok else 'simfill: FAIL')
sys.exit(0 if ok else 1)
