#!/usr/bin/env bash
# Cloud build of the Switch NRO.
# Host: prepares the tree (upstream copy + patches + our platform layer) and
# builds SAT-R's host tools. Docker (devkitPro image): shaders and the NRO.
# Usage: switch/build.sh <label>   e.g. switch/build.sh switch-test1
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LABEL="${1:-dev}"
VERSION="0.1.0"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
BUILD="$ROOT/build/sa2"
GEN="$ROOT/build/gen"
ROM="${SONADV_ROM_EU:-$ROOT/assets/eu.gba}"

[[ -f "$ROM" ]] || { echo "missing Europe ROM at $ROM"; exit 1; }
echo "eb00f101af23d728075ac2117e27ecd8a4b4c3e9  $ROM" | sha1sum -c --quiet

COMMIT="$(git -C "$ROOT" rev-parse --short HEAD)"
DIRTY="$(git -C "$ROOT" status --porcelain --untracked-files=no | grep -q . && echo "+dirty" || true)"
STAMP="$(date -u +%Y-%m-%dT%H:%MZ)"
BUILD_ID="$LABEL $COMMIT$DIRTY $STAMP"
echo "[build] $BUILD_ID"

# 1. Fresh tree: pinned upstream plus our patches and platform layer.
mkdir -p "$BUILD" "$GEN"
# No --delete: generated artifacts stay between builds; tracked sources are
# refreshed to pristine upstream, then patched again below.
rsync -a --checksum "$ROOT/upstream/sa2/" "$BUILD/" --exclude .git --exclude build
for p in "$ROOT"/switch/patches/*.patch; do
    (cd "$BUILD" && patch -p1 --forward --silent < "$p") || { echo "patch failed: $p"; exit 1; }
done
mkdir -p "$BUILD/src/platform/switch"
cp "$ROOT"/switch/platform/*.c "$BUILD/src/platform/switch/"
cp "$ROM" "$BUILD/baserom_sa1.gba"

# 2. Host tools (native x86, need libpng which the devkitPro image lacks).
make -C "$BUILD" tools -j"$(nproc)" > "$ROOT/build/tools.log" 2>&1 || { tail -20 "$ROOT/build/tools.log"; exit 1; }

# 3. Shaders: uam in the image, then embedded as C headers.
for s in present_vsh present_fsh; do
    stage=vert; [[ $s == *fsh ]] && stage=frag
    "$DEVKITPRO/tools/bin/uam" -s $stage -o "$GEN/$s.dksh" "$ROOT/switch/shaders/$s.glsl"
    python3 -I - "$GEN/$s.dksh" "$GEN/${s}_dksh.h" "${s}_dksh" <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read()
name = sys.argv[3]
with open(sys.argv[2], 'w') as f:
    f.write('#pragma once\n#include <stdint.h>\n')
    f.write(f'static const uint8_t {name}[] __attribute__((aligned(256))) = {{\n')
    for i in range(0, len(data), 16):
        f.write(','.join(str(b) for b in data[i:i+16]) + ',\n')
    f.write(f'}};\nstatic const uint32_t {name}_len = {len(data)};\n')
EOF
done

# 4. NRO with devkitA64.
make -C "$BUILD/libagbsyscall" PLATFORM=switch CPU_ARCH=arm64 > "$ROOT/build/libagbsyscall.log" 2>&1 \
    || { tail -20 "$ROOT/build/libagbsyscall.log"; exit 1; }
make -C "$BUILD" switch GAME_NAME=sa1 -j"$(nproc)" \
    SONADV_GEN_DIR="$GEN" SONADV_BUILD_ID="$BUILD_ID" SONADV_VERSION="$VERSION" \
    > "$ROOT/build/nro.log" 2>&1 || { grep -n -E "error|Error" "$ROOT/build/nro.log" | head -30; exit 1; }

# 5. Checks before anything goes to Mark.
NRO="$BUILD/sa1_nx.nro"
ELF="$BUILD/sa1_nx.elf"
[[ -f "$NRO" ]] || { echo "no NRO produced"; exit 1; }
for sym in dkDeviceCreate dkQueuePresentImage gpsp_draw_frame m4aSoundVSync AgbMain SDL_OpenAudioDevice; do
    "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-nm" "$ELF" > "$ROOT/build/nm.txt"
    grep -E " [Tt] $sym$" "$ROOT/build/nm.txt" > /dev/null || { echo "missing symbol $sym"; exit 1; }
done
grep -a -F "$BUILD_ID" "$NRO" > /dev/null || { echo "build id not embedded"; exit 1; }

OUT="$ROOT/out/$LABEL"
mkdir -p "$OUT"
cp "$NRO" "$OUT/SonAdvance-nx.nro"
cp "$ELF" "$OUT/SonAdvance-nx.elf"
echo "[build] OK: $OUT/SonAdvance-nx.nro ($(stat -c %s "$NRO") bytes)"
sha256sum "$OUT/SonAdvance-nx.nro"
