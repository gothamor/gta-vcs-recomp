#!/usr/bin/env bash
# Run vcs.exe for N seconds (default 60), log to build/run/<name>.log
#   tools/run.sh [seconds] [logname]
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SECS="${1:-60}"; NAME="${2:-run}"
mkdir -p "$ROOT/build/run"
cd "$ROOT/build/run"
ISO="${VCS_ISO:-$ROOT/Grand Theft Auto - Vice City Stories (USA).iso}"
timeout "$SECS" "$ROOT/build/release/third_party/PS2Recomp/ps2xRuntime/vcs.exe" "$ISO" > "$NAME.log" 2>&1
echo "exit=$? lines=$(wc -l < "$NAME.log")"
