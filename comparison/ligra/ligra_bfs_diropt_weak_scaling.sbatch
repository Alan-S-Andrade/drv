#!/bin/bash
#SBATCH -J ligra_bfs_do_weak  # Job name
#SBATCH -o ligra_bfs_do_weak_%j.out
#SBATCH -e ligra_bfs_do_weak_%j.err
#SBATCH -p spr              # Sapphire Rapids queue
#SBATCH -N 1
#SBATCH -n 112
#SBATCH -t 01:00:00
#SBATCH -A CCR22006

# =============================================================================
# Ligra BFS (direction-optimizing) — Weak Scaling, MTEPS only
# Uses standard Ligra BFS binary (sparse↔dense switching enabled).
# Same graphs/cores/NUMA policy as ligra_bfs_weak_scaling.sbatch, but no IMC
# instrumentation — MTEPS computed from Ligra's "Running time" lines.
# =============================================================================

set -euo pipefail

BASE_DIR="/work2/10238/vineeth_architect/stampede3/drv_copy"
BFS_BIN="$BASE_DIR/ligra/apps/BFS"
RMAT_GEN="$BASE_DIR/ligra/utils/rMatGraph"
GRAPH_DIR="$BASE_DIR/ligra/graphs"
RESULTS_DIR="$BASE_DIR/ligra_results_weak_rmat_diropt_${NUMA_POLICY:-interleave}"

mkdir -p "$GRAPH_DIR" "$RESULTS_DIR"

DEGREE=16
ROUNDS=10
VTX_PER_CORE=1310720  # 16384 × 80, matches push-only sbatch

CORES=(1 2 4 8 16 32 56)
NUMA_POLICY="${NUMA_POLICY:-interleave}"

if [ "$NUMA_POLICY" = "socket0" ]; then
    NUMACTL_ARGS="--membind=0 --cpunodebind=0"
elif [ "$NUMA_POLICY" = "membind0" ]; then
    NUMACTL_ARGS="--membind=0"
else
    NUMACTL_ARGS="--interleave=all"
fi

echo "=============================================="
echo "Ligra BFS (direction-optimizing) — Weak Scaling"
echo "NUMA policy: ${NUMA_POLICY} (numactl ${NUMACTL_ARGS})"
echo "Date: $(date)"
echo "Hostname: $(hostname)"
echo "=============================================="

CSV="$RESULTS_DIR/weak_scaling_mteps.csv"
echo "cores,vertices,edges,degree,vtx_per_core,bfs_time_sec,mteps" > "$CSV"

for C in "${CORES[@]}"; do
    N=$((VTX_PER_CORE * C))
    M=$((N * DEGREE))

    echo ""
    echo "=== Cores=$C  N=$N  M=$M ==="

    GFILE="$GRAPH_DIR/rmat_${N}.adj"
    if [ ! -f "$GFILE" ]; then
        echo "Generating RMAT graph: N=$N M=$M -> $GFILE"
        "$RMAT_GEN" -a 0.57 -b 0.19 -c 0.19 -m "$M" "$N" "$GFILE"
    fi

    OUTFILE="$RESULTS_DIR/weak_c${C}_N${N}.txt"

    export OMP_NUM_THREADS=$C
    numactl $NUMACTL_ARGS \
        "$BFS_BIN" -rounds "$ROUNDS" "$GFILE" \
        > "$OUTFILE" 2> "$RESULTS_DIR/weak_c${C}_N${N}_stderr.txt"

    # Parse "Running time" lines, take median across timed rounds
    read BFS_TIME MTEPS <<< $(
        python3 -c "
times = []
for line in open('${OUTFILE}'):
    if 'Running time' in line:
        times.append(float(line.split()[-1]))
times.sort()
if not times:
    print('0 0')
else:
    mid = len(times) // 2
    t = times[mid]
    mteps = ${M} / t / 1e6 if t > 0 else 0
    print(f'{t:.6f} {mteps:.1f}')
"
    )

    echo "  BFS time (median): ${BFS_TIME}s"
    echo "  MTEPS: ${MTEPS}"

    echo "${C},${N},${M},${DEGREE},${VTX_PER_CORE},${BFS_TIME},${MTEPS}" >> "$CSV"
done

echo ""
echo "=== Done ==="
echo "Results CSV: $CSV"
echo "Date: $(date)"
