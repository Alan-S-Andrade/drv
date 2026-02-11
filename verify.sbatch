#!/bin/bash
#SBATCH -J bfs_verify
#SBATCH -o logs/%x_%j.out
#SBATCH -e logs/%x_%j.err

#SBATCH -A CCR22006
#SBATCH -p pvc   # Using dev queue for verification
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
# STEP 2: RUN VERIFICATION (4 Cores, 48x16 Grid)
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
    
    # Binary name is drvr_<name> (underscores), NOT drvr-run-<name> (the CMake run dir)
    BINARY=/work/build_stampede/rv64/drvr/drvr_bfs_multi_sw_l2sp
    
    CP=/work/build_stampede/pandocommand/libpandocommand_loader.so
    SWEEP_DIR=/work/build_stampede/drvr/sweep_results_2

    mkdir -p "$SWEEP_DIR"

    # ---- VERIFICATION CONFIGURATION ----
    # 4 Cores, 16 Threads, 48x16 Grid
    CONFIGS=(	    
      "8 1 16 100 100"
      #"4 1 16 70 70"
      #"2 1 16 50 50"

    )

    for cfg in "${CONFIGS[@]}"; do
      read -r CX CY THR R C <<< "$cfg"
      CORES=$((CX * CY))
      RUN_NAME="verify_c${CORES}_r${R}_c${C}"
      RUN_DIR="${SWEEP_DIR}/${RUN_NAME}"

      echo "=============================================="
      echo "RUN: ${RUN_NAME} (${CX}x${CY} cores, ${THR} threads/core, grid ${R}x${C})"
      echo "=============================================="

      mkdir -p "$RUN_DIR"
      cp /work/build_stampede/drvr/drvr-run-bfs_multi_sw_l2sp/summarize.py "$RUN_DIR/" || echo "Warning: summarize.py not found in source dir"

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
      if [ -f "summarize.py" ]; then
          echo "--- Summary for ${RUN_NAME} ---"
          python3 summarize.py
      fi
      echo ""

      cd /work
    done

    echo "=============================================="
    echo "VERIFICATION RUN COMPLETE"
    echo "Results in: build_stampede/drvr/sweep_results_2/"
    echo "=============================================="
  '
