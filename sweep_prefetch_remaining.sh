#!/bin/bash
# Continue sweep for missing configs: l1sp_prefetch_4c, ws_8c, l1sp_cache_8c, l1sp_prefetch_8c
set -o pipefail

DRV=/users/alanandr/drv
SST=$HOME/local/sstcore/bin/sst
MODEL=$DRV/model/drvr.py
CP=$DRV/build/pandocommand/libpandocommand_loader.so
BIN_WS=$DRV/build/rv64/drvr/drvr_bfs_ws_utilization
BIN_L1SP_CACHE=$DRV/build/rv64/drvr/drvr_bfs_work_stealing_l1sp_cache
BIN_L1SP_PREFETCH=$DRV/build/rv64/drvr/drvr_bfs_l1sp_prefetch
GRAPH=$DRV/rmat_r16.bin

RUN_TIMEOUT=1800

OUTDIR=$DRV/prefetch_sweep
CSV="$OUTDIR/prefetch_sweep_results.csv"

run_sst() {
    local binary=$1 variant=$2 cx=$3 harts=$4
    local cores=$cx total_harts=$((cores * harts))
    local tag="${variant}_${cores}c_${harts}h"
    local rundir="$OUTDIR/$tag"

    # Skip if already completed
    if grep -q "^${variant},${cores},${harts}," "$CSV" 2>/dev/null; then
        echo "=== SKIP $tag (already in CSV) ==="
        return 0
    fi

    mkdir -p "$rundir"
    cp -f "$GRAPH" "$rundir/rmat_r16.bin"

    echo "=== Running $tag ($variant, ${cores} cores, ${harts} harts/core, ${total_harts} total, timeout=${RUN_TIMEOUT}s) ==="
    cd "$rundir"

    timeout "$RUN_TIMEOUT" bash -c "
      PYTHONPATH=$DRV/py:$DRV/model \
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
        echo "$variant,$cores,$harts,$total_harts,FAIL,FAIL,FAIL,0,0,0,0,0,0" >> "$CSV"
        cd "$DRV"
        return 0
    fi

    local cycles nodes edges
    cycles=$(grep 'Cycles elapsed:' "$rundir/output.txt" | awk '{print $NF}')
    nodes=$(grep 'Nodes discovered:' "$rundir/output.txt" | grep -oP '\d+' | head -1)
    edges=$(grep 'Total edges traversed:' "$rundir/output.txt" | awk '{print $NF}')

    local pf_hits=0 pf_misses=0 pf_hitrate=0 pf_nodes=0 pf_edges=0 pf_cycles=0
    if grep -q 'PREFETCH_STATS:' "$rundir/output.txt"; then
        pf_hits=$(grep 'PREFETCH_STATS:' "$rundir/output.txt" | grep -oP 'hits=\K\d+')
        pf_misses=$(grep 'PREFETCH_STATS:' "$rundir/output.txt" | grep -oP 'misses=\K\d+')
        pf_hitrate=$(grep 'PREFETCH_STATS:' "$rundir/output.txt" | grep -oP 'hitrate=\K\d+')
        pf_nodes=$(grep 'PREFETCH_STATS:' "$rundir/output.txt" | grep -oP 'nodes=\K\d+')
        pf_edges=$(grep 'PREFETCH_STATS:' "$rundir/output.txt" | grep -oP 'edges=\K\d+')
        pf_cycles=$(grep 'PREFETCH_STATS:' "$rundir/output.txt" | grep -oP 'cycles=\K\d+')
    fi

    echo "$variant,$cores,$harts,$total_harts,$cycles,$nodes,$edges,$pf_hits,$pf_misses,$pf_hitrate,$pf_nodes,$pf_edges,$pf_cycles" >> "$CSV"

    if [[ -f "$rundir/ramulator_system_pxn0_dram0.stats" ]]; then
        cp "$rundir/ramulator_system_pxn0_dram0.stats" "$OUTDIR/${tag}_ramulator.stats"
    fi

    echo "  -> $tag: ${cycles} cycles, ${nodes} nodes, ${edges:-0} edges"
    cd "$DRV"
}

echo "================================================================"
echo "  Continuing L1SP Prefetch Sweep (remaining configs)"
echo "================================================================"

HARTS=16

for cores in 4 8; do
    [[ -f "$BIN_WS" ]] && run_sst "$BIN_WS" "ws" "$cores" "$HARTS"
    [[ -f "$BIN_L1SP_CACHE" ]] && run_sst "$BIN_L1SP_CACHE" "l1sp_cache" "$cores" "$HARTS"
    run_sst "$BIN_L1SP_PREFETCH" "l1sp_prefetch" "$cores" "$HARTS"
done

echo ""
echo "=== Remaining runs complete ==="
echo "Results: $CSV"
cat "$CSV"
