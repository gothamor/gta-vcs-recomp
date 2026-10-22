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

# Always (re)build the recompiler from the main tree: it compiles in the runtime's
# stub list (ps2_call_list.h), so a stale binary silently ignores new stubs.
if [ ! -f build/release/build.ninja ]; then
  cmd //c "tools\devenv.cmd cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF" > build/configure.log
fi
cmd //c "tools\devenv.cmd cmake --build build/release --target ps2_recomp" > build/build_recomp.log 2>&1 || { tail -20 build/build_recomp.log; exit 1; }
RECOMP=build/release/third_party/PS2Recomp/ps2xRecomp/ps2_recomp.exe

python -I tools/make_config.py ghidra/export/ghidra_export.toml build/analyzer/analyzer.toml 2>/dev/null   || echo "(analysis exports not present: using committed game/vcs/vcs.toml)"

# Generate into a scratch dir, then copy only changed files so rebuilds stay incremental.
rm -rf build/generated_fresh
sed 's#^output = .*#output = "build/generated_fresh"#' game/vcs/vcs.toml > build/vcs_fresh.toml
"$RECOMP" build/vcs_fresh.toml > build/recomp.log 2>&1 || { tail -20 build/recomp.log; exit 1; }
grep -E "Warnings:" build/recomp.log || true
python -I tools/sync_generated.py build/generated_fresh generated/ee

# VU1 microcode -> C++ (runs instead of the VU1 interpreter while VU1 memory holds exactly that code)
mkdir -p build/vu1
python -I tools/vu1_recomp.py build/elf/SLUS_215.90 build/vu1/vu1_compiled.cpp game/vcs/vu1_uploads.txt | tail -1
mkdir -p generated/vu1
cmp -s build/vu1/vu1_compiled.cpp generated/vu1/vu1_compiled.cpp || cp build/vu1/vu1_compiled.cpp generated/vu1/vu1_compiled.cpp
