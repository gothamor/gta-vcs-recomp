#!/usr/bin/env bash
# Configure (first time) and build vcs.exe. Output: build/release/bin/vcs.exe
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
if [ ! -f build/release/build.ninja ]; then
  cmd //c "tools\devenv.cmd cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF"
fi
cmd //c "tools\devenv.cmd cmake --build build/release --target ps2EntryRunner" 2>&1 | grep -E "error|FAILED|Linking|warning: unused" | grep -v "Wdeprecated" || true
test -f build/release/third_party/PS2Recomp/ps2xRuntime/vcs.exe
