#!/usr/bin/env bash
# Builds the headless Linux harness from the same patched tree the Switch
# build uses (run switch/build.sh first so build/sa2 is patched).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HOST="$ROOT/build/host"
mkdir -p "$HOST"
rsync -a --checksum "$ROOT/build/sa2/" "$HOST/" --exclude build --exclude "sa1_nx.*" --exclude src/platform/switch
rm -rf "$HOST/src/platform/switch"
cp "$ROOT/tools/host/harness.c" "$HOST/src/platform/pret_sdl/harness.c"
python3 -I - "$HOST" <<'EOF'
import sys
root = sys.argv[1]
p = root + '/src/platform/pret_sdl/sdl2.c'
s = open(p).read()
if 'Harness_Frame' not in s:
    s = s.replace("""    if (headless) {
        REG_VCOUNT = DISPLAY_HEIGHT + 1;
        HANDLE_VBLANK_INTRS();""", """    if (headless) {
        extern void Harness_Frame(void);
        Harness_Frame();
        REG_VCOUNT = DISPLAY_HEIGHT + 1;
        HANDLE_VBLANK_INTRS();""")
open(p, 'w').write(s)
# Mirror the Switch build's SA1 sprite table choice on the host.
p = root + '/include/gba/defines.h'
s = open(p).read()
s = s.replace('#if defined(__SWITCH__) && defined(SWITCH_LEGACY_OAM)', '#if defined(SWITCH_LEGACY_OAM)')
open(p, 'w').write(s)
p = root + '/Makefile'
s = open(p).read()
needle = "CPPFLAGS += -D TITLE_BAR=$(BUILD_NAME).$(PLATFORM) -D PLATFORM_GBA=0 -D PLATFORM_SDL=1 -D PLATFORM_WIN32=0 $(shell sdl2-config --cflags)"
if needle + ' -D SWITCH_LEGACY_OAM=1' not in s:
    s = s.replace(needle, needle + ' -D SWITCH_LEGACY_OAM=1', 1)
open(p, 'w').write(s)
EOF
make -C "$HOST" sdl GAME_NAME=sa1 -j"$(nproc)" > "$ROOT/build/host.log" 2>&1 || { grep -n -E "error" "$ROOT/build/host.log" | head; exit 1; }
echo "host harness: $HOST/sa1.sdl"

# Reference build at the original 240x160 (same harness), for side-by-side checks.
REF="$ROOT/build/hostref"
mkdir -p "$REF"
rsync -a --checksum "$HOST/" "$REF/" --exclude build --exclude 'sa1.sdl'
python3 -I - "$REF" <<'PY'
import sys
p = sys.argv[1] + '/include/gba/defines.h'
s = open(p).read()
s = s.replace('#if defined(__PSP__)\n// PSP: Use GBA-native resolution', '#if defined(__PSP__) || defined(HOST_GBA_RES)\n// PSP: Use GBA-native resolution', 1)
open(p, 'w').write(s)
p = sys.argv[1] + '/Makefile'
s = open(p).read()
s = s.replace(' -D SWITCH_LEGACY_OAM=1', ' -D SWITCH_LEGACY_OAM=1 -D HOST_GBA_RES=1', 1)
open(p, 'w').write(s)
PY
make -C "$REF" sdl GAME_NAME=sa1 -j"$(nproc)" > "$ROOT/build/hostref.log" 2>&1 || { grep -n -E "error" "$ROOT/build/hostref.log" | head; exit 1; }
echo "reference (240x160): $REF/sa1.sdl"
