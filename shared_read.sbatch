#!/bin/bash
#SBATCH -J shared_read_l2sp
#SBATCH -o logs/%x_%j.out
#SBATCH -e logs/%x_%j.err

#SBATCH -A CCR22006
#SBATCH -p pvc
#SBATCH -N 1
#SBATCH -n 1
#SBATCH -c 96
#SBATCH -t 10:00:00

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

# ------------------------------------------------------------------
# STEP 1: BUILD
# ------------------------------------------------------------------
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

    # Build only (rv64 builds all RISC-V binaries, Drv + pandocommand_loader for SST)
    echo "Building binary..."
    make -j'"${SLURM_CPUS_PER_TASK}"' rv64 Drv pandocommand_loader
    echo "Build complete"
  '

# ------------------------------------------------------------------
# STEP 2: RUN SHARED READ SWEEP
# ------------------------------------------------------------------
apptainer exec --cleanenv \
  --env XALT_EXECUTABLE_TRACKING=no \
  --bind "$SLURM_SUBMIT_DIR:/work" \
  "$SIF" \
  bash -lc '
    set -e
    export RISCV_HOME=/install
    export PATH=/install/bin:$PATH

    MODEL=/work/model/drvr.py

    BINARY=/work/build_stampede/rv64/drvr/drvr_shared_read_l2sp

    CP=/work/build_stampede/pandocommand/libpandocommand_loader.so
    SWEEP_DIR=/work/build_stampede/drvr/shared_read_results

    mkdir -p "$SWEEP_DIR"

    # ---- SWEEP CONFIGURATIONS ----
    # Format: CX CY THR
    CONFIGS=(
      "1  1 16"
      "2  1 16"
      "4  1 16"
      "8  1 16"
      "8  2 16"
      "8  4 16"
      "8  8 16"
    )

    for cfg in "${CONFIGS[@]}"; do
      read -r CX CY THR <<< "$cfg"
      CORES=$((CX * CY))
      RUN_NAME="shared_c${CORES}_t${THR}"
      RUN_DIR="${SWEEP_DIR}/${RUN_NAME}"

      echo "=============================================="
      echo "RUN: ${RUN_NAME} (${CX}x${CY} cores, ${THR} threads/core)"
      echo "=============================================="

      mkdir -p "$RUN_DIR"
      cp /work/build_stampede/drvr/drvr-run-shared_read_l2sp/summarize.py "$RUN_DIR/" || echo "Warning: summarize.py not found in source dir"

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
        --pod-l2sp-banks=4 \
	--pod-l2sp-interleave=64 \
        "$BINARY" \
        | tee "$RUN_DIR/output.txt"

      echo ""
      if [ -f "summarize.py" ]; then
          echo "--- Summary for ${RUN_NAME} ---"
          python3 summarize.py
      fi
      echo ""

      cd /work
    done

    echo "=============================================="
    echo "SHARED READ SWEEP COMPLETE"
    echo "Results in: build_stampede/drvr/shared_read_results/"
    echo "=============================================="
  '
