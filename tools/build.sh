#!/usr/bin/env bash
#
# build.sh - build COD4VITA from a fresh checkout, all the way to the VPK.
#
#   git clone https://github.com/NDRWhun/COD4VITA && cd COD4VITA
#   bash tools/build.sh                                   # -> build/COD4VITA.vpk
#   bash tools/build.sh -DSHADER_ARCHIVE=/path/shaders.kgxp   # bundle your shader archive
#
# Needs VitaSDK ($VITASDK set, $VITASDK/bin on PATH), cmake, and make or ninja.
# On Windows, run this from Git Bash (the toolchain wants a unix shell).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [ -z "${VITASDK:-}" ]; then
  echo "!! VITASDK is not set. Install VitaSDK (https://vitasdk.org), then re-run."
  exit 1
fi

JOBS="$(nproc 2>/dev/null || echo 4)"
GEN=()
command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)

cmake -S . -B build "${GEN[@]}" -DKISAK_PLATFORM=vita \
      -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" "$@"
cmake --build build -j "$JOBS"

echo "==> build/COD4VITA.vpk"
