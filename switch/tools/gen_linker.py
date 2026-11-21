#!/usr/bin/env python3
"""Make SonAdvance-nx's linker script and specs from libnx's switch.ld.

The game's constant data (.rodata and .data.rel.ro of the decomp's own
objects) is moved into a writable output section, .sonadv_romdata, in the
DATA part of the image (after __data_start, outside RELRO, which libnx makes
read-only after relocating). At startup the port fills that data from the
player's ROM (see romfill.py and romfill.c). Libraries, crt files and the
port's own platform code keep the stock layout.

usage: gen_linker.py <switch.ld> <switch.specs> <out_dir>
"""
import sys, os
src, specs, out = sys.argv[1:4]
s = open(src).read()
LIBS = ('*.a:*', '/opt/*', 'src/platform/*')

def only_libs(sections):
    return '\n'.join(f'\t\t{p}({sections})' for p in LIBS)

old_ro = '\t\t*(.rodata .rodata.* .gnu.linkonce.r.*)\n'
assert s.count(old_ro) == 1, 'rodata rule not found'
s = s.replace(old_ro, only_libs('.rodata .rodata.* .gnu.linkonce.r.*') + '\n')

old_rel = ('\t\t*(.data.rel.ro.local* .gnu.linkonce.d.rel.ro.local.*)\n'
           '\t\t*(.data.rel.ro .data.rel.ro.* .gnu.linkonce.d.rel.ro.*)\n')
assert s.count(old_rel) == 1, 'data.rel.ro rule not found'
s = s.replace(old_rel, only_libs('.data.rel.ro.local* .gnu.linkonce.d.rel.ro.local.*') + '\n'
              + only_libs('.data.rel.ro .data.rel.ro.* .gnu.linkonce.d.rel.ro.*') + '\n')

anchor = '\tPROVIDE_HIDDEN( __data_start = . );\n'
assert s.count(anchor) == 1, '__data_start not found'
s = s.replace(anchor, anchor + '''
\t/* SonAdvance-nx: the game's constant data, filled from the player's ROM. */
\t.sonadv_romdata : ALIGN(16)
\t{
\t\tPROVIDE_HIDDEN( __sonadv_romdata_start = . );
\t\t*(.rodata .rodata.* .gnu.linkonce.r.*)
\t\t*(.data.rel.ro.local* .gnu.linkonce.d.rel.ro.local.*)
\t\t*(.data.rel.ro .data.rel.ro.* .gnu.linkonce.d.rel.ro.*)
\t\t. = ALIGN(16);
\t\tPROVIDE_HIDDEN( __sonadv_romdata_end = . );
\t} :data
''')
os.makedirs(out, exist_ok=True)
ld = os.path.join(out, 'sonadv.ld')
open(ld, 'w').write(s)
sp = open(specs).read()
old = '-T %:getenv(DEVKITPRO /libnx/switch.ld)'
assert old in sp, 'specs link line not found'
open(os.path.join(out, 'sonadv.specs'), 'w').write(sp.replace(old, '-T ' + os.path.abspath(ld)))
print('linker script:', ld)
