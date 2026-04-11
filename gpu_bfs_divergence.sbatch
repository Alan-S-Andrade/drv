#!/bin/bash
#SBATCH -J gpu_bfs_divergence
#SBATCH -o logs/%x_%j.out
#SBATCH -e logs/%x_%j.err

#SBATCH -A CCR22006
#SBATCH -p gpu-a100          # ← adjust to your GPU partition (gpu-a100, rtx, etc.)
#SBATCH -N 1
#SBATCH -n 1
#SBATCH -c 8
#SBATCH --gres=gpu:1
#SBATCH -t 01:00:00

# ===========================================================================
#  GPU BFS Warp-Divergence Experiment
#
#  Purpose: show that SIMD (warp-based) execution on GPUs is penalised
#  when threads in a warp follow different branch paths.
#
#  Experiment 1 — UNIFORM graph (constant degree):
#    All 32 lanes in a warp process the same number of neighbors.
#    Minimal divergence → near-peak SIMD utilisation.
#
#  Experiment 2 — RMAT/power-law graph:
#    Degree varies from 1 to thousands.  Within a warp some lanes process
#    hub vertices (long loops) while others process leaves (short/no loop).
#    → massive warp divergence, wasted SIMD lanes.
#
#  Together: the ratio of MTEPS(uniform)/MTEPS(rmat) isolates the
#  divergence cost, since both graphs have the same vertex/edge count.
# ===========================================================================

set -eo pipefail

cd "$SLURM_SUBMIT_DIR"
mkdir -p logs gpu_bfs_results

module purge
module load cuda/12.0           # ← adjust to available CUDA module

# ---------- build ----------
echo "=== Building gpu_bfs_divergence ==="
nvcc -O3 -arch=sm_80 -o gpu_bfs_divergence gpu_bfs_divergence.cu
echo "Build OK"

# ---------- parameters (match across GPU and DRV experiments) ----------
SCALE=16            # 2^16 = 65 536 vertices
EF=16               # edge factor → ~1M edges per graph
UNIFORM_N=65536     # same vertex count as RMAT
UNIFORM_D=32        # constant degree (close to RMAT average)
NUM_RUNS=5

UNIFORM_GRAPH="gpu_bfs_results/uniform_s${SCALE}_d${UNIFORM_D}.bin"
RMAT_GRAPH="gpu_bfs_results/rmat_s${SCALE}_ef${EF}.bin"

# ---------- generate graphs ----------
echo ""
echo "=== Generating graphs ==="

python3 gen_uniform_csr.py \
    -N $UNIFORM_N -D $UNIFORM_D \
    --source 0 \
    -o "$UNIFORM_GRAPH"

python3 gen_rmat.py \
    --scale $SCALE --ef $EF \
    --format drv \
    -o "$RMAT_GRAPH"

# ---------- run experiments ----------
echo ""
echo "======================================================="
echo "  Experiment 1: UNIFORM graph (constant degree = ${UNIFORM_D})"
echo "  → minimal warp divergence expected"
echo "======================================================="
./gpu_bfs_divergence "$UNIFORM_GRAPH" $NUM_RUNS | tee gpu_bfs_results/uniform_result.txt

echo ""
echo "======================================================="
echo "  Experiment 2: RMAT power-law graph (scale=${SCALE}, ef=${EF})"
echo "  → heavy warp divergence expected"
echo "======================================================="
./gpu_bfs_divergence "$RMAT_GRAPH" $NUM_RUNS | tee gpu_bfs_results/rmat_result.txt

# ---------- summary ----------
echo ""
echo "======================================================="
echo "  SUMMARY: GPU SIMD warp-divergence cost"
echo "======================================================="
U_MTEPS=$(grep "^Throughput:" gpu_bfs_results/uniform_result.txt | awk '{print $2}')
R_MTEPS=$(grep "^Throughput:" gpu_bfs_results/rmat_result.txt   | awk '{print $2}')
echo "  Uniform MTEPS:  $U_MTEPS"
echo "  RMAT MTEPS:     $R_MTEPS"
echo ""
echo "  When RMAT MTEPS << Uniform MTEPS, the gap is warp divergence."
echo "  GPUs execute all 32 lanes in lock-step; diverging branches"
echo "  serialise paths → lanes with short loops idle while the"
echo "  slowest lane finishes its hub-vertex loop."
echo ""
echo "  Results saved to gpu_bfs_results/"
