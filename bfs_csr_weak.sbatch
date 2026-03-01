#!/bin/bash
#SBATCH -J bfs_csr_weak
#SBATCH -o logs/%x_%j.out
#SBATCH -e logs/%x_%j.err

#SBATCH -A CCR22006
#SBATCH -p pvc
#SBATCH -N 4
#SBATCH -n 1
#SBATCH -c 96
#SBATCH -t 15:00:00

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SLURM_SUBMIT_DIR:-$SCRIPT_DIR}"
mkdir -p logs

module purge
set +u
module load tacc-apptainer/1.4.1

SIF="$SCRIPT_DIR/drv_latest.sif"
RAMULATOR_SRC="$SCRIPT_DIR/../ramulator-build"
RAMULATOR_LIB="$RAMULATOR_SRC/libramulator.so"
RAMULATOR_CONFIGS="$RAMULATOR_SRC/configs"

if [[ ! -f "$SIF" ]]; then
    echo "ERROR: Container image not found at $SIF"
    exit 1
fi

# ------------------------------------------------------------------
# STEP 1: BUILD
# ------------------------------------------------------------------
apptainer exec --cleanenv \
  --env XALT_EXECUTABLE_TRACKING=no \
  --bind "$SCRIPT_DIR:/work" \
  --bind "$RAMULATOR_LIB:/install/lib/libramulator.so" \
  --bind "$RAMULATOR_SRC:/tmp/ramulator:ro" \
  "$SIF" \
  bash -lc '
    set -e
    export RISCV_HOME=/install
    export PATH=/install/bin:$PATH
    cd /work/build_stampede
    cmake .. \
      -DSST_CORE_PREFIX=/install \
      -DSST_ELEMENTS_PREFIX=/install \
      -DGNU_RISCV_TOOLCHAIN_PREFIX=/install \
      -DCMAKE_INSTALL_PREFIX=/work/.local \
      -DSST_ENABLE_RAMULATOR=1 \
      -DRAMULATOR_DIR=/tmp/ramulator

    echo "Building binary..."
    make -j'"${SLURM_CPUS_PER_TASK}"' rv64 Drv pandocommand_loader
    echo "Build complete"
  '

# ------------------------------------------------------------------
# STEP 2: RUN BFS CSR WEAK SCALING SWEEP
# ------------------------------------------------------------------
apptainer exec --cleanenv \
  --env XALT_EXECUTABLE_TRACKING=no \
  --bind "$SCRIPT_DIR:/work" \
  --bind "$RAMULATOR_LIB:/install/lib/libramulator.so" \
  --bind "$RAMULATOR_CONFIGS:/work/ramulator-configs" \
  "$SIF" \
  bash -lc '
    set -e
    export RISCV_HOME=/install
    export PATH=/install/bin:$PATH

    MODEL=/work/model/drvr.py

    BINARY=/work/build_stampede/rv64/drvr/drvr_bfs_csr_weak

    CP=/work/build_stampede/pandocommand/libpandocommand_loader.so
    SWEEP_DIR=/work/build_stampede/drvr/bfs_csr_weak_results_ramulator

    mkdir -p "$SWEEP_DIR"

    # ---- SWEEP CONFIGURATIONS ----
    # Format: CX CY THR
    CONFIGS=(
      "1  1 16"
      #"2  1 16"
      #"4  1 16"
      #"8  1 16"
      #"8  2 16"
      #"8  4 16"
      #"8  8 16"
    )

    for cfg in "${CONFIGS[@]}"; do
      read -r CX CY THR <<< "$cfg"
      CORES=$((CX * CY))
      TOTAL_THREADS=$((CORES * THR))
      RUN_NAME="bfs_csr_c${CORES}_t${THR}"
      RUN_DIR="${SWEEP_DIR}/${RUN_NAME}"

      echo "=============================================="
      echo "RUN: ${RUN_NAME} (${CX}x${CY} cores, ${THR} threads/core, N=$((TOTAL_THREADS * 1024)) vertices)"
      echo "=============================================="

      mkdir -p "$RUN_DIR"
      cp /work/model/summarize.py "$RUN_DIR/"

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
        --pod-l2sp-banks=1 \
        --pod-l2sp-interleave=64 \
        --pxn-dram-banks=4 \
        --pxn-dram-interleave=64 \
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
    echo "BFS CSR WEAK SCALING SWEEP COMPLETE"
    echo "Results in: build_stampede/drvr/bfs_csr_weak_results_ramulator/"
    echo "=============================================="
  '
