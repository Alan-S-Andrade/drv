#!/bin/bash
#SBATCH -J drv_mimd_bfs
#SBATCH -o logs/%x_%j.out
#SBATCH -e logs/%x_%j.err

#SBATCH -A CCR22006
#SBATCH -p icx
#SBATCH -N 1
#SBATCH -n 1
#SBATCH -c 32
#SBATCH -t 06:00:00

# ===========================================================================
#  DRV (PANDOHammer) Single-Core 16-Hart MIMD BFS Experiment
#
#  Purpose: demonstrate MIMD advantage over GPU SIMD for irregular graph
#  workloads.  The DRV architecture runs 16 hardware threads (harts) per
#  core, each with independent control flow — no warp-style lock-step.
#
#  When traversing a power-law (RMAT) graph:
#    - GPU: warp lanes processing different-degree vertices diverge →
#      the warp serialises branches, wasting SIMD lanes
#    - DRV: each hart independently processes its own vertex, no lane
#      waste; harts that finish early can steal work from others
#
#  We run two graphs (same sizes as the GPU experiment):
#    1. Uniform (constant degree) — isolates divergence cost
#    2. RMAT (power-law)          — triggers divergence on GPU
#
#  Then compare DRV MTEPS vs GPU MTEPS from gpu_bfs_divergence.sbatch.
# ===========================================================================

set -eo pipefail

cd "$SLURM_SUBMIT_DIR"
mkdir -p logs drv_mimd_results

module purge
set +u
module load tacc-apptainer/1.4.1

SIF="$WORK/drv.sif"

if [[ ! -f "$SIF" ]]; then
    echo "ERROR: Container image not found at $SIF"
    exit 1
fi

# ---------- parameters (must match gpu_bfs_divergence.sbatch) ----------
SCALE=16             # 2^16 = 65 536 vertices
EF=16                # edge factor
UNIFORM_N=65536
UNIFORM_D=32
CX=1                 # 1 core (X)
CY=1                 # 1 core (Y) → total 1 core
THR=16               # 16 harts per core — matches GPU warp width comparison
BUILD_DIR="build_mimd_1c16h"

# ------------------------------------------------------------------
# STEP 1: GENERATE GRAPHS
# ------------------------------------------------------------------
echo "=== Generating graphs ==="

python3 gen_uniform_csr.py \
    -N $UNIFORM_N -D $UNIFORM_D \
    --seed 42 \
    -o "drv_mimd_results/uniform_s${SCALE}_d${UNIFORM_D}.bin"

python3 gen_rmat.py \
    --scale $SCALE --ef $EF \
    --format drv \
    -o "drv_mimd_results/rmat_s${SCALE}_ef${EF}.bin"

# ------------------------------------------------------------------
# STEP 2: BUILD inside container
# ------------------------------------------------------------------
echo ""
echo "=== Building DRV binaries ==="

apptainer exec --cleanenv \
  --env XALT_EXECUTABLE_TRACKING=no \
  --bind "$SLURM_SUBMIT_DIR:/work" \
  "$SIF" \
  bash -lc '
    set -e
    export RISCV_HOME=/install
    export PATH=/install/bin:$PATH

    mkdir -p /work/'"${BUILD_DIR}"'
    cd /work/'"${BUILD_DIR}"'

    cmake /work \
      -DSST_CORE_PREFIX=/install \
      -DSST_ELEMENTS_PREFIX=/install \
      -DCMAKE_INSTALL_PREFIX=/work/.local

    echo "Building BFS work-stealing binary..."
    make -j'"${SLURM_CPUS_PER_TASK}"' rv64 Drv pandocommand_loader
    echo "Build complete"
  '

# ------------------------------------------------------------------
# STEP 3: RUN BFS — both graphs, single core, 16 harts
# ------------------------------------------------------------------

run_drv_bfs() {
    local GRAPH_NAME="$1"
    local GRAPH_FILE="$2"
    local RUN_DIR="drv_mimd_results/${GRAPH_NAME}"

    echo ""
    echo "======================================================="
    echo "  DRV MIMD BFS: ${GRAPH_NAME} (${CX}x${CY} core, ${THR} harts)"
    echo "======================================================="

    mkdir -p "$RUN_DIR"

    # The BFS binary looks for "rmat_r16.bin" in its working directory.
    # We symlink our graph file to that name.
    cp "$GRAPH_FILE" "$RUN_DIR/rmat_r16.bin"

    apptainer exec --cleanenv \
      --env XALT_EXECUTABLE_TRACKING=no \
      --bind "$SLURM_SUBMIT_DIR:/work" \
      "$SIF" \
      bash -lc '
        set -e
        export RISCV_HOME=/install
        export PATH=/install/bin:$PATH

        MODEL=/work/model/drvr.py
        BINARY=/work/'"${BUILD_DIR}"'/rv64/drvr/drvr_bfs_work_stealing
        CP=/work/'"${BUILD_DIR}"'/pandocommand/libpandocommand_loader.so
        RUN_DIR=/work/'"${RUN_DIR}"'

        cd "$RUN_DIR"

        echo "Running SST simulation..."
        PYTHONPATH=/work/py::/work/model \
          /install/bin/sst -n 1 \
          "$MODEL" \
          -- \
          --with-command-processor="$CP" \
          --num-pxn=1 \
          --pxn-pods=1 \
          --pod-cores-x='"${CX}"' \
          --pod-cores-y='"${CY}"' \
          --core-threads='"${THR}"' \
          "$BINARY" \
          | tee output.txt

        echo ""
        echo "--- Simulation output for '"${GRAPH_NAME}"' ---"
        grep -E "BFS|level|TEPS|time|nodes|hart|tick" output.txt || true
      '
}

run_drv_bfs "uniform_1c16h" "drv_mimd_results/uniform_s${SCALE}_d${UNIFORM_D}.bin"
run_drv_bfs "rmat_1c16h"    "drv_mimd_results/rmat_s${SCALE}_ef${EF}.bin"

# ------------------------------------------------------------------
# STEP 4: SUMMARY
# ------------------------------------------------------------------
echo ""
echo "======================================================="
echo "  SUMMARY: DRV MIMD (1 core, 16 harts) BFS Results"
echo "======================================================="
echo ""
echo "--- Uniform graph ---"
grep -E "BFS|TEPS|time|nodes|tick" drv_mimd_results/uniform_1c16h/output.txt 2>/dev/null || echo "(check logs)"
echo ""
echo "--- RMAT graph ---"
grep -E "BFS|TEPS|time|nodes|tick" drv_mimd_results/rmat_1c16h/output.txt 2>/dev/null || echo "(check logs)"
echo ""
echo "KEY INSIGHT: DRV MIMD architecture should show relatively"
echo "consistent performance between uniform and RMAT graphs because"
echo "each hart runs independently — no warp divergence penalty."
echo "Compare the RMAT/Uniform ratio here vs the GPU experiment."
echo ""
echo "  GPU:  MTEPS(rmat) << MTEPS(uniform)  → SIMD divergence penalty"
echo "  DRV:  MTEPS(rmat) ≈ MTEPS(uniform)   → MIMD handles irregularity"
echo ""
echo "Results saved to drv_mimd_results/"
