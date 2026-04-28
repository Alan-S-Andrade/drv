#!/bin/bash
#SBATCH -J ligra_bfs_weak  # Job name
#SBATCH -o ligra_bfs_weak_%j.out
#SBATCH -e ligra_bfs_weak_%j.err
#SBATCH -p spr              # Sapphire Rapids queue
#SBATCH -N 1
#SBATCH -n 112
#SBATCH -t 01:00:00          # 1 hour
#SBATCH -A CCR22006          # Update allocation as needed

# =============================================================================
# Ligra BFS_PushOnly — Weak Scaling with DRAM Bandwidth Measurement
#
# Matches DRV's weak-scaling model with SAME WORK PER CORE:
#   DRV:   N = 1024 vtx/hart × 16 harts/core × cores = 16384 × cores
#   Ligra: N = 16384 vtx/core × cores  (SKX: 1 thread per core)
#
# DRAM bandwidth is measured via perf_event_open() inside BFS_PushOnly,
# bracketing ONLY the BFS while-loop (not graph loading, init, or stats).
# Each Compute() round prints an "IMC:" line with:
#   IMC: <bfs_kernel_sec> <dram_read_bytes> <dram_write_bytes>
# =============================================================================

set -euo pipefail

# Paths
BASE_DIR="/work2/10238/vineeth_architect/stampede3/drv_copy"
BFS_BIN="$BASE_DIR/ligra/apps/BFS_PushOnly"
RMAT_GEN="$BASE_DIR/ligra/utils/rMatGraph"
GRAPH_DIR="$BASE_DIR/ligra/graphs"
RESULTS_DIR="$BASE_DIR/ligra_results_weak_rmat_${NUMA_POLICY:-interleave}"

mkdir -p "$GRAPH_DIR" "$RESULTS_DIR"

# Configuration matching DRV weak scaling
# DRV: N = cores × 16_harts × 1024_vtx = cores × 16384
# Ligra: 100× DRV base so graphs are large enough for real CPU measurement
# N = cores × 16384 × 100 = cores × 1,638,400
DEGREE=16
ROUNDS=10
VTX_PER_CORE=1310720  # 16384 × 80

# Core counts to sweep (socket 0 only — max 56 cores)
CORES=(1 2 4 8 16 32 56)
# NUMA memory policy: "interleave", "membind0", or "socket0"
#   interleave — numactl --interleave=all        (data on both sockets, only socket 0 IMC measured)
#   membind0   — numactl --membind=0             (all data on socket 0, cores on both sockets)
#   socket0    — numactl --membind=0 --cpunodebind=0  (memory AND cores on socket 0 only, max 56 cores)
NUMA_POLICY="${NUMA_POLICY:-interleave}"

# SPR-HBM Xeon Max 9480 peak memory bandwidth (GB/s)
# HBM2e: 16 stacks/socket, ~1 TB/s theoretical per socket
# STREAM Triad achievable: ~750 GB/s per socket, ~690 GB/s Copy
# NOTE: counters via cpu=0 measure socket 0 only (same isolation as SKX)
#
# Memory latency for Little's Law required concurrency calculation.
# SPR-HBM idle latency ~120ns (HBM2e), loaded ~150-200ns.
# Using unloaded latency gives a lower bound on required concurrency.
MEM_LATENCY_NS=${MEM_LATENCY_NS:-120}

if [ "$NUMA_POLICY" = "socket0" ]; then
    PEAK_BW_GBS=750.0
    NUMACTL_ARGS="--membind=0 --cpunodebind=0"
elif [ "$NUMA_POLICY" = "membind0" ]; then
    PEAK_BW_GBS=750.0
    NUMACTL_ARGS="--membind=0"
else
    PEAK_BW_GBS=750.0
    NUMACTL_ARGS="--interleave=all"
fi

echo "=============================================="
echo "Ligra BFS_PushOnly — Weak Scaling + Bandwidth"
echo "Weak scaling matching DRV: c56 ↔ DRV c64, degree=${DEGREE}"
echo "NUMA policy: ${NUMA_POLICY} (numactl ${NUMACTL_ARGS})"
echo "DRAM measurement: inline IMC counters (BFS kernel only)"
echo "Date: $(date)"
echo "Hostname: $(hostname)"
echo "Cores available: $(nproc)"
echo "Peak DRAM BW: ${PEAK_BW_GBS} GB/s (theoretical)"
echo "Memory latency: ${MEM_LATENCY_NS} ns (for Little's Law)"
echo "=============================================="

# CSV output
CSV="$RESULTS_DIR/weak_scaling_bw.csv"
echo "cores,vertices,edges,degree,vtx_per_core,bfs_time_sec,mteps,dram_read_bytes,dram_write_bytes,dram_total_bytes,dram_bytes_per_edge,algo_bytes,algo_bytes_per_edge,dram_to_algo_ratio,dram_bw_gbs,useful_bw_gbs,bw_utilization_pct,llc_misses,llc_miss_bytes,llc_miss_to_dram_ratio,avg_mem_concurrency,l1d_pending,cpu_cycles,stalls_mem_any,stalls_l3_miss,stalls_mem_frac,stalls_l3_frac,required_concurrency,concurrency_gap,ipc,retiring_pct,exe0_dark_pct,instr_per_llc_miss,inst_retired,uops_retired_slots,exe_bound_0_ports,iterations,reached,max_dist,sum_dist" > "$CSV"

for C in "${CORES[@]}"; do
    N=$((VTX_PER_CORE * C))
    M=$((N * DEGREE))

    echo ""
    echo "=== Cores=$C  N=$N  M=$M ==="

    # Generate RMAT graph if needed
    GFILE="$GRAPH_DIR/rmat_${N}.adj"
    if [ ! -f "$GFILE" ]; then
        echo "Generating RMAT graph: N=$N M=$M -> $GFILE"
        "$RMAT_GEN" -a 0.57 -b 0.19 -c 0.19 -m "$M" "$N" "$GFILE"
    fi

    OUTFILE="$RESULTS_DIR/weak_c${C}_N${N}.txt"

    # Run BFS — IMC counters are bracketed inside the binary
    export OMP_NUM_THREADS=$C
    numactl $NUMACTL_ARGS \
        "$BFS_BIN" -rounds "$ROUNDS" "$GFILE" \
        > "$OUTFILE" 2> "$RESULTS_DIR/weak_c${C}_N${N}_stderr.txt"

    # Parse BFS output (take last timed run's stats)
    BFS_STATS=$(grep -E "^(BFS done|Reached|max_dist)" "$OUTFILE" | tail -3)
    ITERS=$(echo "$BFS_STATS" | grep "BFS done" | sed 's/.*in \([0-9]*\) iterations.*/\1/')
    REACHED=$(echo "$BFS_STATS" | grep "Reached" | sed 's/.*Reached: \([0-9]*\).*/\1/')
    MAX_DIST=$(echo "$BFS_STATS" | grep "max_dist" | sed 's/.*max_dist=\([0-9]*\).*/\1/')
    SUM_DIST=$(echo "$BFS_STATS" | grep "max_dist" | sed 's/.*sum_dist=\([0-9]*\).*/\1/')

    # Parse IMC lines and compute median-round bandwidth.
    # IMC lines: "IMC: <bfs_sec> <rb> <wb> <llc_misses> <l1d_pending> <cycles> <stalls_mem> <stalls_l3>"
    # First IMC line is warmup (untimed Compute call) — skip it.
    # From the remaining timed rounds, sort by bfs_sec and pick the median.
    read BFS_TIME DRAM_READ_BYTES DRAM_WRITE_BYTES DRAM_TOTAL_BYTES DRAM_BW_GBS BW_UTIL LLC_MISSES LLC_MISS_BYTES L1D_PENDING CPU_CYCLES AVG_CONCURRENCY STALLS_MEM STALLS_L3 STALLS_MEM_FRAC STALLS_L3_FRAC INST_RETIRED UOPS_SLOTS EXE0 <<< $(
        python3 -c "
import sys

lines = []
for line in open('${OUTFILE}'):
    if line.startswith('IMC:'):
        parts = line.split()
        bfs_s  = float(parts[1])
        rb     = int(parts[2])
        wb     = int(parts[3])
        llcm   = int(parts[4]) if len(parts) > 4 else 0
        l1dp   = int(parts[5]) if len(parts) > 5 else 0
        cyc    = int(parts[6]) if len(parts) > 6 else 0
        smem   = int(parts[7]) if len(parts) > 7 else 0
        sl3    = int(parts[8]) if len(parts) > 8 else 0
        iret   = int(parts[9]) if len(parts) > 9 else 0
        uslot  = int(parts[10]) if len(parts) > 10 else 0
        e0     = int(parts[11]) if len(parts) > 11 else 0
        if rb < 0 or wb < 0:
            continue
        lines.append((bfs_s, rb, wb, llcm, l1dp, cyc, smem, sl3, iret, uslot, e0))

if len(lines) <= 1:
    times = []
    for line in open('${OUTFILE}'):
        if 'Running time' in line:
            times.append(float(line.split()[-1]))
    times.sort()
    mid = len(times) // 2 if times else 0
    bfs_t = times[mid] if times else 0
    print(f'{bfs_t} 0 0 0 0.000 0.0 0 0 0 0 0.000 0 0 0.000 0.000 0 0 0')
    sys.exit()

# Skip warmup (first line), use timed rounds
rounds = lines[1:]
rounds.sort(key=lambda x: x[0])
mid = len(rounds) // 2
bfs_sec, rb, wb, llcm, l1dp, cyc, smem, sl3, iret, uslot, e0 = rounds[mid]
total = rb + wb
bw_gbs = total / bfs_sec / 1e9 if bfs_sec > 0 else 0
util = bw_gbs / ${PEAK_BW_GBS} * 100
llc_bytes = llcm * 64
avg_conc = l1dp / cyc if cyc > 0 else 0.0
smem_frac = smem / cyc if cyc > 0 else 0.0
sl3_frac  = sl3 / cyc if cyc > 0 else 0.0
print(f'{bfs_sec:.6f} {rb} {wb} {total} {bw_gbs:.3f} {util:.1f} {llcm} {llc_bytes} {l1dp} {cyc} {avg_conc:.3f} {smem} {sl3} {smem_frac:.4f} {sl3_frac:.4f} {iret} {uslot} {e0}')
"
    )

    # Algorithmic bytes: push-only BFS on CSR (all arrays are 4-byte uintE/uintT)
    #   Offset reads:      N × 4  (one per frontier vertex)
    #   Edge reads:         M × 4  (read each neighbor ID)
    #   Dist/parent reads:  M × 4  (check each neighbor's visited status)
    #   Dist/parent writes: N × 4  (mark each discovered vertex)
    #   Total = 8*(N + M), same for DRV and Ligra on the same graph
    ALGO_BYTES=$(( 8 * (N + M) ))

    read DRAM_BYTES_PER_EDGE ALGO_BYTES_PER_EDGE DRAM_ALGO_RATIO LLC_MISS_DRAM_RATIO USEFUL_BW_GBS MTEPS REQ_CONCURRENCY CONC_GAP IPC RETIRING_PCT EXE0_DARK_PCT INSTR_PER_LLC_MISS <<< $(
        python3 -c "
M=${M}; C=${C}; dt=${DRAM_TOTAL_BYTES}; ab=${ALGO_BYTES}; llcb=${LLC_MISS_BYTES}
bw=${DRAM_BW_GBS}; bfs_t=float('${BFS_TIME}'); avg_conc=float('${AVG_CONCURRENCY}')
peak_bw=${PEAK_BW_GBS}; lat_ns=${MEM_LATENCY_NS}
cyc=${CPU_CYCLES}; iret=${INST_RETIRED}; uslot=${UOPS_SLOTS}; e0=${EXE0}; llcm=${LLC_MISSES}
dpe = dt/M if M > 0 else 0
ape = ab/M if M > 0 else 0
ratio = dt/ab if ab > 0 else 0
llc_dram = llcb/dt if dt > 0 else 0
useful_bw = float(bw) * llc_dram
mteps = M / bfs_t / 1e6 if bfs_t > 0 else 0
# Little's Law: required_concurrency_per_core = (peak_bw / cores) * latency / 64B
req_conc = (peak_bw * 1e9 / C) * (lat_ns * 1e-9) / 64.0 if C > 0 else 0
conc_gap = req_conc - avg_conc if req_conc > avg_conc else 0
# Core utilization metrics (aggregated across all active cores)
ipc = iret / cyc if cyc > 0 else 0
# SPR pipeline width = 6 uops/cycle
retiring_pct = uslot / (6.0 * cyc) * 100 if cyc > 0 else 0
exe0_dark_pct = e0 / cyc * 100 if cyc > 0 else 0
instr_per_miss = iret / llcm if llcm > 0 else 0
print(f'{dpe:.2f} {ape:.2f} {ratio:.2f} {llc_dram:.2f} {useful_bw:.3f} {mteps:.1f} {req_conc:.1f} {conc_gap:.1f} {ipc:.3f} {retiring_pct:.1f} {exe0_dark_pct:.1f} {instr_per_miss:.1f}')
"
    )

    echo "  BFS kernel time (median): ${BFS_TIME}s"
    echo "  DRAM read: ${DRAM_READ_BYTES} B, write: ${DRAM_WRITE_BYTES} B"
    echo "  LLC misses: ${LLC_MISSES}  (× 64B = ${LLC_MISS_BYTES} B)"
    echo "  DRAM BW: ${DRAM_BW_GBS} GB/s  (useful: ${USEFUL_BW_GBS} GB/s)  (utilization: ${BW_UTIL}%)"
    echo "  DRAM bytes/edge: ${DRAM_BYTES_PER_EDGE}  (algo minimum: ${ALGO_BYTES_PER_EDGE})"
    echo "  DRAM/algo ratio: ${DRAM_ALGO_RATIO}x  (algo_bytes: ${ALGO_BYTES})"
    echo "  LLC_MISS_BYTES/DRAM ratio: ${LLC_MISS_DRAM_RATIO}  (1.0 = all DRAM from LLC misses)"
    echo "  Avg mem concurrency: ${AVG_CONCURRENCY}  (L1D_PEND_MISS.PENDING / cycles)"
    echo "  Required concurrency: ${REQ_CONCURRENCY}  (Little's Law: peak_bw/${C}cores × ${MEM_LATENCY_NS}ns / 64B)"
    echo "  Concurrency gap: ${CONC_GAP}  (required - actual)"
    echo "  Stalls MEM_ANY: ${STALLS_MEM_FRAC} of cycles  (${STALLS_MEM} / ${CPU_CYCLES})"
    echo "  Stalls L3_MISS: ${STALLS_L3_FRAC} of cycles  (${STALLS_L3} / ${CPU_CYCLES})"
    echo "  --- Core utilization ---"
    echo "  IPC: ${IPC}  (peak=6.0)"
    echo "  Retiring: ${RETIRING_PCT}%  (pipeline slots doing useful work)"
    echo "  Dark (0-ports active): ${EXE0_DARK_PCT}%  (cycles with zero execution)"
    echo "  Instr/LLC miss: ${INSTR_PER_LLC_MISS}  (instructions between DRAM stalls)"
    echo "  MTEPS: ${MTEPS}"
    echo "  Iters=$ITERS  Reached=$REACHED  max_dist=$MAX_DIST"

    echo "${C},${N},${M},${DEGREE},${VTX_PER_CORE},${BFS_TIME},${MTEPS},${DRAM_READ_BYTES},${DRAM_WRITE_BYTES},${DRAM_TOTAL_BYTES},${DRAM_BYTES_PER_EDGE},${ALGO_BYTES},${ALGO_BYTES_PER_EDGE},${DRAM_ALGO_RATIO},${DRAM_BW_GBS},${USEFUL_BW_GBS},${BW_UTIL},${LLC_MISSES},${LLC_MISS_BYTES},${LLC_MISS_DRAM_RATIO},${AVG_CONCURRENCY},${L1D_PENDING},${CPU_CYCLES},${STALLS_MEM},${STALLS_L3},${STALLS_MEM_FRAC},${STALLS_L3_FRAC},${REQ_CONCURRENCY},${CONC_GAP},${IPC},${RETIRING_PCT},${EXE0_DARK_PCT},${INSTR_PER_LLC_MISS},${INST_RETIRED},${UOPS_SLOTS},${EXE0},${ITERS},${REACHED},${MAX_DIST},${SUM_DIST}" >> "$CSV"
done

echo ""
echo "=== Done ==="
echo "Results CSV: $CSV"
echo "Date: $(date)"
