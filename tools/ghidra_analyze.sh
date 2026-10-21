#!/usr/bin/env bash
# Headless Ghidra analysis of the user's SLUS_215.90, exporting the PS2Recomp
# function map. Needs Ghidra 12.1.4 + the ghidra-emotionengine-reloaded
# extension and JDK 21 (see README).
#   tools/ghidra_analyze.sh build/elf/SLUS_215.90
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ELF="${1:-$ROOT/build/elf/SLUS_215.90}"
GHIDRA="${GHIDRA_HOME:-$HOME/scoop/apps/ghidra/current}"
export JAVA_HOME="${JAVA_HOME:-$HOME/scoop/apps/temurin21-jdk/current}"
PROJ="$ROOT/ghidra/project"
OUT="$ROOT/ghidra/export"
mkdir -p "$PROJ" "$OUT"

"$GHIDRA/support/analyzeHeadless.bat" "$(cygpath -w "$PROJ")" vcs \
  -import "$(cygpath -w "$ELF")" -overwrite \
  -scriptPath "$(cygpath -w "$ROOT/third_party/PS2Recomp/ps2xRecomp/tools/ghidra")" \
  -postScript ExportPS2Functions.java "$(cygpath -w "$OUT/ghidra_export.toml")" "$(cygpath -w "$OUT/functions.csv")" \
  -max-cpu 6 < /dev/null
