#!/bin/bash
# Sweep CAS vs FAA BFS variants at 1, 2, 4, 8, 16 cores
set -o pipefail

DRV=/users/alanandr/drv
SST=$HOME/local/sstcore/bin/sst
MODEL=$DRV/model/drvr.py
CP=$DRV/build/pandocommand/libpandocommand_loader.so
BIN_CAS=$DRV/build/rv64/drvr/drvr_bfs_work_stealing
BIN_FAA=$DRV/build/rv64/drvr/drvr_bfs_work_stealing_faa
GRAPH=$DRV/rmat_32k.bin

# Fallback: try rmat_r16.bin if rmat_32k.bin doesn't exist
if [ ! -f "$GRAPH" ]; then
    GRAPH=$DRV/rmat_s16.adj
    echo "Warning: rmat_32k.bin not found, using $GRAPH"
fi

OUTDIR=$DRV/cas_vs_faa_sweep
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

CSV="$OUTDIR/cas_vs_faa_results.csv"
echo "variant,cores,cycles_elapsed,nodes_discovered,edges_traversed" > "$CSV"

CONFIGS=("1 1" "2 1" "4 1" "8 1" "16 1")

run_sst() {
    local binary=$1
    local variant=$2
    local cx=$3
    local cy=$4
    local cores=$((cx * cy))
    local rundir="$OUTDIR/${variant}_${cores}c"
    mkdir -p "$rundir"

    # Copy graph files with expected names
    for name in rmat_32k.bin rmat_r16.bin; do
        cp -f "$GRAPH" "$rundir/$name" 2>/dev/null || true
    done

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

    local cycles nodes edges
    cycles=$(grep 'Cycles elapsed:' "$rundir/output.txt" | tail -1 | awk '{print $NF}')
    nodes=$(grep 'Nodes discovered:' "$rundir/output.txt" | tail -1 | grep -oP '\d+' | head -1)
    edges=$(grep 'Total edges\|num_edges\|E=' "$rundir/output.txt" | head -1 | grep -oP '\d+' | tail -1)
    echo "$variant,$cores,$cycles,$nodes,$edges" >> "$CSV"
    echo "  -> $variant ${cores}c: ${cycles} cycles, ${nodes} nodes"
    echo ""
}

echo "CAS vs FAA BFS Sweep"
echo "Graph: $GRAPH"
echo "Output: $OUTDIR"
echo ""

for cfg in "${CONFIGS[@]}"; do
    read -r CX CY <<< "$cfg"
    run_sst "$BIN_CAS" "cas" "$CX" "$CY"
    run_sst "$BIN_FAA" "faa" "$CX" "$CY"
done

echo "=== All runs complete ==="
echo ""
cat "$CSV"
echo ""
echo "Results saved to $CSV"
echo "Run: python3 plot_cas_vs_faa_roofline.py to generate roofline plot"
