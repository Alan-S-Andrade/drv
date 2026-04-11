#!/bin/bash
#SBATCH -J bfs_tiered_wq
#SBATCH -o logs/%x_%j.out
#SBATCH -e logs/%x_%j.err

#SBATCH -A CCR22006
#SBATCH -p icx
#SBATCH -N 1
#SBATCH -n 1
#SBATCH -c 32
#SBATCH -t 06:00:00

set -eo pipefail

cd "$SLURM_SUBMIT_DIR"
mkdir -p logs

module purge
set +u
module load tacc-apptainer/1.4.1

SIF="$WORK/drv.sif"

if [[ ! -f "$SIF" ]]; then
    echo "ERROR: Container image not found at $SIF"
    exit 1
fi

# ---------- Parameters ----------
SCALE=16
EF=16
SEED=42
CUSP=16
BUILD_DIR="build_tiered_wq"
GRAPH_FILE="rmat_r16.bin"

# ---------- Step 1: Generate graph ----------
echo "=== Generating RMAT graph (scale=${SCALE}, ef=${EF}, seed=${SEED}, cusp=${CUSP}) ==="
python3 gen_rmat.py --scale ${SCALE} --ef ${EF} --seed ${SEED} --cusp ${CUSP} \
    --format drv -o "${GRAPH_FILE}"

# ---------- Step 2: Build inside container ----------
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

    echo "Building baseline + tiered WQ binaries..."
    make -j'"${SLURM_CPUS_PER_TASK}"' rv64 Drv pandocommand_loader
    echo "Build complete"
  '

# ---------- Step 3: Run core-scaling sweep ----------
echo ""
echo "=== Running core-scaling sweep ==="

RESULTS_DIR="tiered_wq_results"
mkdir -p "${RESULTS_DIR}"

# Core configs: "CORES_X CORES_Y"  → total cores = CX*CY
CORE_CONFIGS=(
  "1 1"
  "2 1"
  "4 1"
  "8 1"
  "16 1"
)

run_bfs_variant() {
    local VARIANT="$1"
    local CX="$2"
    local CY="$3"
    local CORES=$((CX * CY))
    local RUN_NAME="${VARIANT}_c${CORES}"
    local RUN_DIR="${RESULTS_DIR}/${RUN_NAME}"

    echo ""
    echo "======================================================="
    echo "  ${RUN_NAME} (${CX}x${CY} cores, 16 harts/core)"
    echo "======================================================="

    mkdir -p "${RUN_DIR}"
    cp "${GRAPH_FILE}" "${RUN_DIR}/rmat_r16.bin"

    apptainer exec --cleanenv \
      --env XALT_EXECUTABLE_TRACKING=no \
      --bind "$SLURM_SUBMIT_DIR:/work" \
      "$SIF" \
      bash -lc '
        set -e
        export RISCV_HOME=/install
        export PATH=/install/bin:$PATH

        MODEL=/work/model/drvr.py
        BINARY=/work/'"${BUILD_DIR}"'/rv64/drvr/drvr_'"${VARIANT}"'
        CP=/work/'"${BUILD_DIR}"'/pandocommand/libpandocommand_loader.so
        RUN_DIR=/work/'"${RUN_DIR}"'

        cd "$RUN_DIR"

        PYTHONPATH=/work/py::/work/model \
          /install/bin/sst -n 1 \
          "$MODEL" \
          -- \
          --with-command-processor="$CP" \
          --num-pxn=1 \
          --pxn-pods=1 \
          --pod-cores-x='"${CX}"' \
          --pod-cores-y='"${CY}"' \
          --core-threads=16 \
          --pod-l2sp-banks=8 \
          "$BINARY" \
          | tee output.txt
      '

    echo "  Completed: ${RUN_NAME}"
}

# Run all 5 core counts × 2 variants = 10 runs
for cfg in "${CORE_CONFIGS[@]}"; do
    read -r CX CY <<< "$cfg"
    run_bfs_variant "bfs_work_stealing" "$CX" "$CY"
    run_bfs_variant "bfs_tiered_wq"    "$CX" "$CY"
done

# ---------- Step 4: Extract results ----------
echo ""
echo "======================================================="
echo "  RESULTS SUMMARY"
echo "======================================================="

SUMMARY="${RESULTS_DIR}/summary.csv"
echo "variant,cores,cycles,nodes_discovered,edges_traversed" > "$SUMMARY"

for cfg in "${CORE_CONFIGS[@]}"; do
    read -r CX CY <<< "$cfg"
    CORES=$((CX * CY))
    for VARIANT in bfs_work_stealing bfs_tiered_wq; do
        RUN_DIR="${RESULTS_DIR}/${VARIANT}_c${CORES}"
        OUTFILE="${RUN_DIR}/output.txt"
        if [[ -f "$OUTFILE" ]]; then
            CYCLES=$(grep -oP 'Cycles elapsed:\s+\K[0-9]+' "$OUTFILE" || echo "0")
            NODES=$(grep -oP 'Nodes discovered:\s+\K[0-9]+' "$OUTFILE" || echo "0")
            EDGES=$(grep -oP 'Total edges traversed:\s+\K[0-9]+' "$OUTFILE" || echo "0")
            echo "${VARIANT},${CORES},${CYCLES},${NODES},${EDGES}" >> "$SUMMARY"
            echo "  ${VARIANT} @ ${CORES}c: ${CYCLES} cycles, ${NODES} nodes, ${EDGES} edges"
        else
            echo "  ${VARIANT} @ ${CORES}c: output not found"
        fi
    done
done

echo ""
echo "Summary CSV: ${SUMMARY}"
echo "To generate roofline plot:"
echo "  python3 plot_roofline_tiered_comparison.py --csv ${SUMMARY}"
echo ""
echo "Job complete."
