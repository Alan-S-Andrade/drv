#!/bin/bash
#SBATCH -J ligra_bfs       # Job name
#SBATCH -o ligra_bfs_%j.out # Output file
#SBATCH -e ligra_bfs_%j.err # Error file
#SBATCH -p spr              # Partition (Sapphire Rapids)
#SBATCH -N 1                # Single node
#SBATCH -n 1                # Single task
#SBATCH -t 02:00:00         # 2 hour time limit
#SBATCH -A ASC24004         # Allocation (update as needed)

# =============================================================================
# Ligra BFS_PushOnly Benchmark Script
# Compares against DRV/PANDO BFS (bfs_csr_weak) using same graph sizes
# =============================================================================

set -euo pipefail

# Paths
LIGRA_DIR="/work2/10238/vineeth_architect/stampede3/drv_copy/ligra"
BFS_BIN="$LIGRA_DIR/apps/BFS_PushOnly"
GEN_UNIFORM="$LIGRA_DIR/utils/gen_uniform_graph"
RMAT_GEN="$LIGRA_DIR/utils/rMatGraph"
GRAPH_DIR="$LIGRA_DIR/graphs"
RESULTS_DIR="/work2/10238/vineeth_architect/stampede3/drv_copy/ligra_results"

# Ensure output directories exist
mkdir -p "$GRAPH_DIR" "$RESULTS_DIR"

# Configuration matching DRV sweep
DEGREE=16
ROUNDS=10
# Graph sizes matching DRV weak-scaling configs (N = cores * 16_harts * 1024_vtx)
SIZES=(16384 32768 65536 131072 262144 524288 1048576)
# Thread counts to sweep on SPR node (socket 0 only — 56 cores)
# Capped at 56: IMC/HBM perf counters only measure socket 0
THREADS=(1 2 4 8 16 32 56)

# Load modules
module load gcc/13.2.0 2>/dev/null || true

echo "=============================================="
echo "Ligra BFS_PushOnly Benchmark"
echo "Date: $(date)"
echo "Hostname: $(hostname)"
echo "Cores: $(nproc)"
echo "=============================================="

# =============================================================================
# Phase 1: Generate graphs
# =============================================================================
echo ""
echo "=== Phase 1: Graph Generation ==="

for N in "${SIZES[@]}"; do
    M=$((N * DEGREE))

    # Uniform random graph (directed, matching DRV)
    UFILE="$GRAPH_DIR/uniform_${N}.adj"
    if [ ! -f "$UFILE" ]; then
        echo "Generating uniform graph: N=$N M=$M -> $UFILE"
        "$GEN_UNIFORM" -d "$DEGREE" "$N" "$UFILE"
    else
        echo "Uniform graph exists: $UFILE"
    fi

    # R-MAT graph (symmetric, for comparison)
    RFILE="$GRAPH_DIR/rmat_${N}.adj"
    if [ ! -f "$RFILE" ]; then
        # rMatGraph requires N to be a power of 2 (which our sizes are)
        echo "Generating R-MAT graph: N=$N M=$M -> $RFILE"
        "$RMAT_GEN" -m "$M" "$N" "$RFILE"
    else
        echo "R-MAT graph exists: $RFILE"
    fi
done

echo "Graph generation complete."
echo ""

# =============================================================================
# Phase 2: Run BFS benchmarks
# =============================================================================
echo "=== Phase 2: BFS Benchmarks ==="

# CSV header for structured output
RESULTS_CSV="$RESULTS_DIR/ligra_bfs_results.csv"
echo "graph_type,vertices,edges,degree,threads,round,time_sec" > "$RESULTS_CSV"

for GTYPE in uniform rmat; do
    for N in "${SIZES[@]}"; do
        if [ "$GTYPE" = "uniform" ]; then
            GFILE="$GRAPH_DIR/uniform_${N}.adj"
            GFLAG=""  # directed graph
        else
            GFILE="$GRAPH_DIR/rmat_${N}.adj"
            GFLAG=""   # rmat generated without -s, so directed
        fi

        if [ ! -f "$GFILE" ]; then
            echo "WARNING: Graph file not found: $GFILE, skipping"
            continue
        fi

        for T in "${THREADS[@]}"; do
            echo "--- $GTYPE N=$N threads=$T ---"
            OUTFILE="$RESULTS_DIR/${GTYPE}_${N}_t${T}.txt"

            # Run BFS_PushOnly with specified thread count
            OMP_NUM_THREADS=$T \
            numactl --interleave=all \
            "$BFS_BIN" $GFLAG -rounds "$ROUNDS" "$GFILE" 2>&1 | tee "$OUTFILE"

            # Parse timing from Ligra output: "Running time : <seconds>"
            # Ligra prints one warmup + ROUNDS timed runs
            grep "Running time" "$OUTFILE" | while IFS= read -r line; do
                TIME=$(echo "$line" | awk '{print $NF}')
                echo "$GTYPE,$N,$((N * DEGREE)),$DEGREE,$T,0,$TIME" >> "$RESULTS_CSV"
            done

            # Also extract BFS stats from the output
            grep -E "^(BFS_PushOnly|Vertices|BFS done|Reached|max_dist)" "$OUTFILE" \
                > "$RESULTS_DIR/${GTYPE}_${N}_t${T}_stats.txt" 2>/dev/null || true
        done
    done
done

echo ""
echo "=== Benchmark Complete ==="
echo "Results CSV: $RESULTS_CSV"
echo "Individual outputs in: $RESULTS_DIR/"
echo "Date: $(date)"
