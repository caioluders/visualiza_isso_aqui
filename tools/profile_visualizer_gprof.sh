#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$ROOT_DIR/build/app_default}"
BIN="$BUILD_DIR/visualiza_isso_aqui"
OUT_DIR="$ROOT_DIR/build/profiling"
REPORT="$OUT_DIR/gprof_visualizer.txt"

mkdir -p "$OUT_DIR"
rm -f "$ROOT_DIR/gmon.out" "$BUILD_DIR/gmon.out" "$OUT_DIR/gmon.out" "$REPORT"

if [[ ! -x "$BIN" ]]; then
  echo "Binary not found: $BIN" >&2
  exit 1
fi

echo "Running $BIN"
echo "Reproduce the slowdown, then close the app normally."
"$BIN"

GMON_FILE=""
if [[ -f "$ROOT_DIR/gmon.out" ]]; then
  GMON_FILE="$ROOT_DIR/gmon.out"
elif [[ -f "$BUILD_DIR/gmon.out" ]]; then
  GMON_FILE="$BUILD_DIR/gmon.out"
fi

if [[ -z "$GMON_FILE" ]]; then
  echo "No gmon.out file was produced." >&2
  exit 1
fi

cp "$GMON_FILE" "$OUT_DIR/gmon.out"
gprof "$BIN" "$GMON_FILE" > "$REPORT"

echo
echo "Profile written to:"
echo "  $REPORT"
echo
echo "Top flat-profile entries:"
sed -n '1,40p' "$REPORT"
