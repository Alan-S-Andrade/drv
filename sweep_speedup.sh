#!/bin/bash
# Sweep BFS variants at 1, 2, 4, 8 cores on the same rmat_32k.bin graph
set -o pipefail

DRV=/users/alanandr/drv
SST=$HOME/local/sstcore/bin/sst
MODEL=$DRV/model/drvr.py
CP=$DRV/build/pandocommand/libpandocommand_loader.so
BIN_WS=$DRV/build/rv64/drvr/drvr_bfs_ws_utilization
BIN_BL=$DRV/build/rv64/drvr/drvr_bfs_nosteal_baseline
GRAPH=$DRV/rmat_32k.bin

OUTDIR=$DRV/speedup_sweep
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

CSV="$OUTDIR/speedup_results.csv"
echo "variant,cores,cycles_elapsed,nodes_discovered" > "$CSV"

CONFIGS=("1 1" "2 1" "4 1" "8 1")

run_sst() {
    local binary=$1
    local variant=$2
    local cx=$3
    local cy=$4
    local cores=$((cx * cy))
    local rundir="$OUTDIR/${variant}_${cores}c"
    mkdir -p "$rundir"
    cp -f "$GRAPH" "$rundir/rmat_32k.bin"
    cp -f "$GRAPH" "$rundir/rmat_r16.bin"

    echo "=== Running $variant at $cores cores (${cx}x${cy}) ==="
    cd "$rundir"

    PYTHONPATH=$DRV/py::$DRV/model \
      "$SST" -n 1 "$MODEL" \
      -- \
      --with-command-processor="$CP" \
      --num-pxn=1 --pxn-pods=1 \
      --pod-cores-x="$cx" --pod-cores-y="$cy" \
      --core-threads=16 \
      "$binary" \
      2>&1 | tee "$rundir/output.txt"

    local cycles nodes
    cycles=$(grep 'Cycles elapsed:' "$rundir/output.txt" | tail -1 | awk '{print $NF}')
    nodes=$(grep 'Nodes discovered:' "$rundir/output.txt" | tail -1 | grep -oP '\d+' | head -1)
    echo "$variant,$cores,$cycles,$nodes" >> "$CSV"
    echo "  -> $variant ${cores}c: ${cycles} cycles, ${nodes} nodes"
    echo ""
}

echo "Graph: $GRAPH (N=32768 RMAT power-law)"
echo "Output: $OUTDIR"
echo ""

for cfg in "${CONFIGS[@]}"; do
    read -r CX CY <<< "$cfg"
    run_sst "$BIN_WS" "perfect_ws" "$CX" "$CY"
    run_sst "$BIN_BL" "nosteal"    "$CX" "$CY"
done

echo "=== All runs complete ==="
cat "$CSV"
