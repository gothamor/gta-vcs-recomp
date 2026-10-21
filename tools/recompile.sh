#!/usr/bin/env bash
# Full pipeline from the user's ISO to generated C++ (generated/ee).
# Uses the committed function map; Ghidra is only needed to redo the analysis.
#   tools/recompile.sh "path/to/Grand Theft Auto - Vice City Stories (USA).iso"
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
ISO="${1:?usage: tools/recompile.sh <your VCS (USA) iso>}"

mkdir -p build/elf
python -I tools/vcs_disc.py elf "$ISO" build/elf/SLUS_215.90

RECOMP=build/tools/ps2xRecomp/ps2_recomp.exe
[ -x "$RECOMP" ] || RECOMP=build/release/third_party/PS2Recomp/ps2xRecomp/ps2_recomp.exe
if [ ! -x "$RECOMP" ]; then
  echo "Building the recompiler first..."
  cmd //c "tools\\devenv.cmd cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl"
  cmd //c "tools\\devenv.cmd cmake --build build/release --target ps2_recomp"
  RECOMP=build/release/third_party/PS2Recomp/ps2xRecomp/ps2_recomp.exe
fi

rm -rf generated/ee
mkdir -p generated/ee
"$RECOMP" game/vcs/vcs.toml > build/recomp.log 2>&1 || { tail -20 build/recomp.log; exit 1; }
grep -E "Warnings:" build/recomp.log || true
echo "Generated $(ls generated/ee | wc -l) files in generated/ee"
