#!/usr/bin/env bash
# Builds the combined Video Game Module firmware (screen mirroring + test patterns).
# Clones the official firmware at a pinned commit, applies vgm-combined.patch, adds the
# pattern generator and builds with Flipper's own toolchain (downloaded on first run).
# Usage: ./build.sh [work-dir]      Output: <work-dir>/build/app/firmware.uf2
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${1:-$HERE/work}"
UPSTREAM="https://github.com/flipperdevices/video-game-module.git"
COMMIT="b57986deb02a933e83986be9fa9c680c32287b77"

if [ ! -d "$WORK/.git" ]; then
    git clone "$UPSTREAM" "$WORK"
fi
cd "$WORK"
git checkout -q "$COMMIT"
git checkout -q -- .
git submodule update --init --recursive --depth 1

patch -p1 < "$HERE/vgm-combined.patch"
cp "$HERE/patterns/tpg.c" "$HERE/patterns/tpg.h" app/

export FBT_TOOLCHAIN_PATH="${FBT_TOOLCHAIN_PATH:-$WORK}"
set +u
# shellcheck disable=SC1091
source scripts/toolchain/fbtenv.sh
set -u

mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(getconf _NPROCESSORS_ONLN)"
echo "Built: $PWD/app/firmware.uf2"
