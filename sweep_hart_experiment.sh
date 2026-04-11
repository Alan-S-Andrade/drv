#!/bin/bash
# Sweep BFS (work-stealing + imbalanced baseline) at 2, 4, 8, 16 cores
# for BOTH 16 harts/core and 32 harts/core on the same RMAT power-law graph.
set -o pipefail

DRV=/users/alanandr/drv
SST=$HOME/local/sstcore/bin/sst
MODEL=$DRV/model/drvr.py
CP=$DRV/build/pandocommand/libpandocommand_loader.so
BIN_WS=$DRV/build/rv64/drvr/drvr_bfs_ws_utilization
BIN_BL=$DRV/build/rv64/drvr/drvr_bfs_imbalanced_baseline
GRAPH=$DRV/rmat_r16.bin

# Per-run wall-clock timeout in seconds (16-core runs can be very slow)
RUN_TIMEOUT=600

OUTDIR=$DRV/hart_sweep
mkdir -p "$OUTDIR"

CSV="$OUTDIR/hart_sweep_results.csv"
echo "variant,cores,harts_per_core,total_harts,cycles_elapsed,nodes_discovered" > "$CSV"

# Utilization data file (parsed from run output)
UTIL_CSV="$OUTDIR/utilization_results.csv"
echo "variant,cores,harts_per_core,l2sp_total,l2sp_used,l2sp_pct,l1sp_per_core,l1sp_data_used,nodes_processed,edges_traversed,imbalance_pct" > "$UTIL_CSV"

CORE_COUNTS=(2 4 8 16)
HART_COUNTS=(16 32)

run_sst() {
    local binary=$1
    local variant=$2
    local cx=$3
    local harts=$4
    local cores=$cx
    local total_harts=$((cores * harts))
    local tag="${variant}_${cores}c_${harts}h"
    local rundir="$OUTDIR/$tag"
    mkdir -p "$rundir"

    cp -f "$GRAPH" "$rundir/rmat_r16.bin"

    echo "=== Running $tag ($variant, ${cores} cores, ${harts} harts/core, ${total_harts} total) ==="
    cd "$rundir"

    timeout "$RUN_TIMEOUT" bash -c "
      PYTHONPATH=$DRV/py::$DRV/model \
        \"$SST\" -n 1 \"$MODEL\" \
        -- \
        --with-command-processor=\"$CP\" \
        --num-pxn=1 \
        --pxn-pods=1 \
        --pod-cores-x=\"$cx\" \
        --pod-cores-y=1 \
        --core-threads=\"$harts\" \
        \"$binary\"
    " 2>&1 | tee "$rundir/output.txt"
    local rc=${PIPESTATUS[0]}

    if [[ $rc -ne 0 ]]; then
        echo "  -> $tag: FAILED (exit=$rc, timeout=${RUN_TIMEOUT}s)"
        echo "$variant,$cores,$harts,$total_harts,FAIL,FAIL" >> "$CSV"
        echo "$variant,$cores,$harts,0,0,0,0,0,0,0,0" >> "$UTIL_CSV"
        cd "$DRV"
        return 0
    fi

    # Parse cycles
    local cycles
    cycles=$(grep 'Cycles elapsed:' "$rundir/output.txt" | awk '{print $NF}')

    # Parse nodes discovered
    local nodes
    nodes=$(grep 'Nodes discovered:' "$rundir/output.txt" | grep -oP '\d+' | head -1)

    echo "$variant,$cores,$harts,$total_harts,$cycles,$nodes" >> "$CSV"

    # Parse utilization data
    local l2sp_total l2sp_used l2sp_pct l1sp_per_core l1sp_data nodes_proc edges_trav imbal
    l2sp_total=$(grep 'L2SP total:' "$rundir/output.txt" | awk '{print $3}' || echo "0")
    l2sp_used=$(grep 'L2SP used (total):' "$rundir/output.txt" | awk '{print $4}' || echo "0")
    l2sp_pct=$(grep 'L2SP used (total):' "$rundir/output.txt" | grep -oP '\d+\.\d+' | head -1 || echo "0")
    l1sp_per_core=$(grep 'L1SP per-core:' "$rundir/output.txt" | awk '{print $3}' || echo "0")
    l1sp_data=$(grep 'Data region:' "$rundir/output.txt" | grep -oP '\d+ used' | grep -oP '\d+' || echo "0")
    nodes_proc=$(grep 'Total nodes processed:' "$rundir/output.txt" | awk '{print $NF}' || echo "0")
    edges_trav=$(grep 'Total edges traversed:' "$rundir/output.txt" | awk '{print $NF}' || echo "0")
    imbal=$(grep 'Imbalance' "$rundir/output.txt" | grep -oP '\d+' | head -1 || echo "0")

    echo "$variant,$cores,$harts,$l2sp_total,$l2sp_used,$l2sp_pct,$l1sp_per_core,$l1sp_data,$nodes_proc,$edges_trav,$imbal" >> "$UTIL_CSV"

    echo "  -> $tag: ${cycles} cycles, ${nodes} nodes"
    echo ""
}

echo "================================================================"
echo "  Hart Sweep Experiment: 16 vs 32 harts/core"
echo "  Cores: ${CORE_COUNTS[*]}"
echo "  Harts: ${HART_COUNTS[*]}"
echo "  Graph: $GRAPH"
echo "  Output: $OUTDIR"
echo "================================================================"
echo ""

for harts in "${HART_COUNTS[@]}"; do
    for cores in "${CORE_COUNTS[@]}"; do
        run_sst "$BIN_WS" "ws" "$cores" "$harts"
        run_sst "$BIN_BL" "baseline" "$cores" "$harts"
    done
done

echo ""
echo "=== All runs complete ==="
echo "Performance results: $CSV"
echo "Utilization results: $UTIL_CSV"
cat "$CSV"
echo ""
cat "$UTIL_CSV"
