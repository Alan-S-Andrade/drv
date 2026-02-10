#!/bin/bash
#SBATCH -J bfs_sweep
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

# First: build inside the container
apptainer exec --cleanenv \
  --env XALT_EXECUTABLE_TRACKING=no \
  --bind "$SLURM_SUBMIT_DIR:/work" \
  "$SIF" \
  bash -lc '
    set -e
    export RISCV_HOME=/install
    export PATH=/install/bin:$PATH
    cd build_stampede
    cmake .. \
      -DSST_CORE_PREFIX=/install \
      -DSST_ELEMENTS_PREFIX=/install \
      -DCMAKE_INSTALL_PREFIX=/work/.local
    make -j'"${SLURM_CPUS_PER_TASK}"' drvr-run_bfs_multi_sw_l2sp Drv
    echo "Build complete"
  '

# Second: run the sweep inside the container
apptainer exec --cleanenv \
  --env XALT_EXECUTABLE_TRACKING=no \
  --bind "$SLURM_SUBMIT_DIR:/work" \
  "$SIF" \
  bash -lc '
    set -e
    export RISCV_HOME=/install
    export PATH=/install/bin:$PATH

    MODEL=/work/model/drvr.py
    BINARY=/work/build_stampede/rv64/drvr/drvr_bfs_multi_sw_l2sp
    CP=/work/build_stampede/pandocommand/libpandocommand_loader.so
    SWEEP_DIR=/work/build_stampede/drvr/sweep_results

    mkdir -p "$SWEEP_DIR"

    # ---- SWEEP CONFIGURATION ----
    # Format: "CORES_X CORES_Y THREADS R C"
    # Cores = CORES_X * CORES_Y, Threads per core = 16 (fixed)
    # Grid = R x C nodes
    CONFIGS=(
      # Core scaling sweep (fixed 64x64 grid)
      "1 1 16 64 64"
      "2 1 16 64 64"
      "4 1 16 64 64"
      "8 1 16 64 64"

      # Problem size sweep (fixed 4 cores)
      "4 1 16 32 32"
      "4 1 16 48 48"
      "4 1 16 100 100"
    )

    for cfg in "${CONFIGS[@]}"; do
      read -r CX CY THR R C <<< "$cfg"
      CORES=$((CX * CY))
      RUN_NAME="c${CORES}_r${R}_c${C}"
      RUN_DIR="${SWEEP_DIR}/${RUN_NAME}"

      echo "=============================================="
      echo "RUN: ${RUN_NAME} (${CX}x${CY} cores, ${THR} threads/core, grid ${R}x${C})"
      echo "=============================================="

      mkdir -p "$RUN_DIR"

      # Copy summarize.py to run dir
      cp /work/build_stampede/drvr/drvr-run-bfs_multi_sw_l2sp/summarize.py "$RUN_DIR/"

      cd "$RUN_DIR"

      PYTHONPATH=/work/py::/work/model \
        /install/bin/sst -n 1 \
        "$MODEL" \
        -- \
        --with-command-processor="$CP" \
        --num-pxn=1 \
        --pxn-pods=1 \
        --pod-cores-x=${CX} \
        --pod-cores-y=${CY} \
        --core-threads=${THR} \
        "$BINARY" \
        --R ${R} --C ${C} \
        | tee "$RUN_DIR/output.txt"

      echo ""
      echo "--- Summary for ${RUN_NAME} ---"
      python3 "$RUN_DIR/summarize.py"
      echo ""

      cd /work
    done

    echo "=============================================="
    echo "ALL RUNS COMPLETE"
    echo "Results in: build_stampede/drvr/sweep_results/"
    echo "=============================================="
  '
