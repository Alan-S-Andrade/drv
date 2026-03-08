#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <build-dir> <cmake-run-target> [log-file]"
  echo "Example:"
  echo "  $0 build drvr-run-bfs_work_stealing logs/run_2cores.log"
  exit 1
fi

BUILD_DIR="$1"
RUN_TARGET="$2"
LOG_FILE="${3:-logs/run_2cores.log}"

mkdir -p "$(dirname "$LOG_FILE")"

# Capture full run output into a concrete file.
cmake --build "$BUILD_DIR" --target "$RUN_TARGET" 2>&1 | tee "$LOG_FILE"

echo "Wrote log: $LOG_FILE"
