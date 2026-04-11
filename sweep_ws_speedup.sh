#!/bin/bash
# sweep_ws_speedup.sh — Run imbalanced baseline and work-stealing BFS
# at multiple core counts, collect cycles and imbalance data.
#
# Usage:  bash sweep_ws_speedup.sh [CORE_COUNTS...]
#         Default: 2 4 8 16
#
# Output: ws_speedup_results.csv

set -euo pipefail

D="/users/alanandr/drv"
SST="$HOME/local/sstcore/bin/sst"
MODEL="$D/model/drvr.py"
CP="$D/build/pandocommand/libpandocommand_loader.so"
BASELINE_BIN="$D/build/rv64/drvr/drvr_bfs_imbalanced_baseline"
WS_BIN="$D/build/rv64/drvr/drvr_bfs_ws_utilization"
RESULT_FILE="$D/ws_speedup_results.csv"

CORE_COUNTS=("${@:-2 4 8 16}")
if [[ "${#CORE_COUNTS[@]}" -eq 0 || "${CORE_COUNTS[0]}" == "2 4 8 16" ]]; then
    CORE_COUNTS=(2 4 8 16)
fi

export PYTHONPATH="$D/py::$D/model"

echo "variant,cores,cycles,discovered,imbalance_pct,total_edges" > "$RESULT_FILE"

run_sst() {
    local bin="$1"
    local cores="$2"
    "$SST" -n 1 "$MODEL" -- \
        --with-command-processor="$CP" \
        --num-pxn=1 --pxn-pods=1 \
        --pod-cores-x="$cores" --pod-cores-y=1 \
        --core-threads=16 \
        "$bin" 2>&1
}

parse_output() {
    local output="$1"
    local variant="$2"
    local cores="$3"

    local cycles
    cycles=$(echo "$output" | grep "Cycles elapsed:" | awk '{print $NF}')

    local disc
    disc=$(echo "$output" | grep "Nodes discovered:" | head -1 | awk '{print $3}')

    local imbal
    imbal=$(echo "$output" | grep "Imbalance" | grep -oP '\d+(?=%)' | head -1)
    imbal=${imbal:-0}

    local edges
    edges=$(echo "$output" | grep "Total edges:" | grep -oP '\d+' | tail -1)
    edges=${edges:-0}

    echo "$variant,$cores,$cycles,$disc,$imbal,$edges"
}

for CORES in "${CORE_COUNTS[@]}"; do
    echo "=== $CORES cores: Imbalanced Baseline ==="
    OUT=$(run_sst "$BASELINE_BIN" "$CORES")
    ROW=$(parse_output "$OUT" "baseline" "$CORES")
    echo "$ROW" | tee -a "$RESULT_FILE"
    echo "$OUT" | grep -E "(Cycles elapsed|Imbalance|Per-core queues)" | head -5
    echo ""

    echo "=== $CORES cores: Work Stealing ==="
    OUT=$(run_sst "$WS_BIN" "$CORES")
    ROW=$(parse_output "$OUT" "ws" "$CORES")
    echo "$ROW" | tee -a "$RESULT_FILE"
    echo "$OUT" | grep -E "(Cycles elapsed|Imbalance|Per-core queues)" | head -5
    echo ""
done

echo "Results saved to $RESULT_FILE"
cat "$RESULT_FILE"
