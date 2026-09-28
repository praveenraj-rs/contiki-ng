#!/bin/bash
# Sync sf-mobility.c/.h from the development copy in mobility-adaptive-msf
# into this benchmarks folder, so `python3 run_benchmarks.py` always
# exercises your latest scheduler changes.
set -e
SELF_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SRC_DIR="$SELF_DIR/../mobility-adaptive-msf"

for f in sf-mobility.c sf-mobility.h; do
  if ! diff -q "$SRC_DIR/$f" "$SELF_DIR/$f" >/dev/null 2>&1; then
    cp "$SRC_DIR/$f" "$SELF_DIR/$f"
    echo "synced $f"
  else
    echo "$f already up to date"
  fi
done
