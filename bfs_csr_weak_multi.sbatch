#!/bin/bash
#SBATCH -J bfs_csr_weak_multi
#SBATCH -o logs/%x_%j.out
#SBATCH -e logs/%x_%j.err

#SBATCH -A CCR22006
#SBATCH -p pvc
#SBATCH -N 4
#SBATCH -n 1
#SBATCH -c 96
#SBATCH -t 48:00:00

set -eo pipefail

SCRIPT_DIR="/work2/10238/vineeth_architect/stampede3/drv_copy/drv"
cd "$SCRIPT_DIR"
mkdir -p logs

module purge
set +u
module load tacc-apptainer/1.4.1

SIF="/work2/10238/vineeth_architect/stampede3/drv_latest.sif"
RAMULATOR_SRC="/work2/10238/vineeth_architect/stampede3/drv_copy/ramulator-build"
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
# STEP 2: RUN BFS CSR WEAK SCALING MULTI-POD SWEEP
# ------------------------------------------------------------------

# Graph parameters
VTX_PER_THREAD=1024
DEGREE=16

# Graph type: "uniform" or "rmat"
GRAPH_TYPE="uniform"
# Edge factor for RMAT (only used when GRAPH_TYPE=rmat)
EF=16

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

    BINARY=/work/build_stampede/rv64/drvr/drvr_bfs_csr_weak_multi

    CP=/work/build_stampede/pandocommand/libpandocommand_loader.so
    SWEEP_DIR=/work/build_stampede/drvr/bfs_csr_weak_multi_results

    VTX_PER_THREAD='"${VTX_PER_THREAD}"'
    DEGREE='"${DEGREE}"'
    GRAPH_TYPE='"${GRAPH_TYPE}"'
    EF='"${EF}"'

    mkdir -p "$SWEEP_DIR"

    # ---- SWEEP CONFIGURATIONS ----
    # Format: PODS CX CY THR
    CONFIGS=(
      # Multi-pod weak scaling
      #"2 8 4 16"
      "4 8 8 16"
    )

    for cfg in "${CONFIGS[@]}"; do
      read -r PODS CX CY THR <<< "$cfg"
      CORES=$((CX * CY))
      THREADS_PER_POD=$((CORES * THR))
      N=$((VTX_PER_THREAD * THREADS_PER_POD * PODS))

      RUN_NAME="bfs_csr_p${PODS}_c${CORES}_t${THR}"
      RUN_DIR="${SWEEP_DIR}/${RUN_NAME}"

      echo "=============================================="
      echo "RUN: ${RUN_NAME} (${PODS} pods, ${CX}x${CY} cores, ${THR} threads/core, N=${N}, degree=${DEGREE})"
      echo "=============================================="

      mkdir -p "$RUN_DIR"
      cp /work/model/summarize.py "$RUN_DIR/"
      cp /work/model/summarize_ramulator.py "$RUN_DIR/"

      # Generate graph into run dir
      if [ "$GRAPH_TYPE" = "rmat" ]; then
        SCALE=$(python3 -c "import math; print(int(math.log2($N)))")
        # Upper-bound degree for buffer allocation: 2*EF+4
        RUN_DEGREE=$((2 * EF + 4))
        echo "Generating RMAT+CUSP graph: N=${N} (scale=${SCALE}), ef=${EF}, cusp=${PODS}..."
        python3 /work/gen_rmat_csr.py \
          --scale ${SCALE} \
          --ef ${EF} \
          --cusp ${PODS} \
          --seed 42 \
          -o "$RUN_DIR/uniform_graph.bin"
      else
        RUN_DEGREE=${DEGREE}
        echo "Generating uniform graph: N=${N} degree=${DEGREE}..."
        python3 /work/gen_uniform_csr.py \
          -N ${N} \
          -D ${DEGREE} \
          --seed 42 \
          -o "$RUN_DIR/uniform_graph.bin"
      fi

      cd "$RUN_DIR"

      PYTHONPATH=/work/py::/work/model \
        /install/bin/sst -n 1 \
        "$MODEL" \
        -- \
        --with-command-processor="$CP" \
        --num-pxn=1 \
        --pxn-pods=${PODS} \
        --pod-cores-x=${CX} \
        --pod-cores-y=${CY} \
        --core-threads=${THR} \
        --pod-l2sp-banks=4 \
        --pod-l2sp-interleave=64 \
        --pxn-dram-banks=1 \
        "$BINARY" \
        --V ${VTX_PER_THREAD} \
        --D ${RUN_DEGREE} \
        | tee "$RUN_DIR/output.txt"

      echo ""
      if [ -f "summarize.py" ]; then
          echo "--- Summary for ${RUN_NAME} ---"
          python3 summarize.py
      fi
      if [ -f "summarize_ramulator.py" ] && [ -f "ramulator_system_pxn0_dram0.stats" ]; then
          echo "--- Ramulator Summary for ${RUN_NAME} ---"
          python3 summarize_ramulator.py
      fi
      echo ""

      cd /work
    done

    echo "=============================================="
    echo "BFS CSR WEAK SCALING MULTI-POD SWEEP COMPLETE"
    echo "Results in: ${SWEEP_DIR}"
    echo "=============================================="
  '
