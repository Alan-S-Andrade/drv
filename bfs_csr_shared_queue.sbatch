#!/bin/bash
#SBATCH -J bfs_csr_sq
#SBATCH -o logs/%x_%j.out
#SBATCH -e logs/%x_%j.err

#SBATCH -A CCR22006
#SBATCH -p pvc
#SBATCH -N 1
#SBATCH -n 1
#SBATCH -c 96
#SBATCH -t 24:00:00

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
# STEP 1b: Use pre-built libmemHierarchy.so with coherence ALU tagging
# ------------------------------------------------------------------
MEMH_LIB="/work2/10238/vineeth_architect/stampede3/drv-stack/sst-elements/lib/sst-elements-library/libmemHierarchy.so"

if [[ ! -f "$MEMH_LIB" ]]; then
    echo "ERROR: libmemHierarchy.so not found at $MEMH_LIB"
    echo "Run rebuild_memhierarchy.sh first if needed."
    exit 1
fi

# ------------------------------------------------------------------
# STEP 2: RUN BFS CSR SHARED-QUEUE SWEEP
# ------------------------------------------------------------------

# Graph parameters (same as work-stealing baseline)
VTX_PER_THREAD=1024
DEGREE=16

# Graph type: "uniform" or "rmat"
GRAPH_TYPE="rmat"
# Edge factor for RMAT (only used when GRAPH_TYPE=rmat)
EF=16

apptainer exec --cleanenv \
  --env XALT_EXECUTABLE_TRACKING=no \
  --env OMPI_MCA_mtl=^psm2 \
  --bind "$SCRIPT_DIR:/work" \
  --bind "$RAMULATOR_LIB:/install/lib/libramulator.so" \
  --bind "$RAMULATOR_CONFIGS:/work/ramulator-configs" \
  --bind "$MEMH_LIB:/install/lib/sst-elements-library/libmemHierarchy.so" \
  "$SIF" \
  bash -lc '
    set -e
    export RISCV_HOME=/install
    export PATH=/install/bin:$PATH

    MODEL=/work/model/drvr.py
    BINARY=/work/build_stampede/rv64/drvr/drvr_bfs_csr_shared_queue
    CP=/work/build_stampede/pandocommand/libpandocommand_loader.so
    SWEEP_DIR=/work/build_stampede/drvr/bfs_csr_sq_results

    VTX_PER_THREAD='"${VTX_PER_THREAD}"'
    DEGREE='"${DEGREE}"'
    GRAPH_TYPE='"${GRAPH_TYPE}"'
    EF='"${EF}"'

    mkdir -p "$SWEEP_DIR"

    # ---- SWEEP CONFIGURATIONS ----
    # Format: PODS CX CY THR DRAM_CACHE_KIB
    # Same configurations as work-stealing baseline
    CONFIGS=(
      #"1 1  1 16 4096"
      #"1 2  1 16 4096"
      #"1 4  1 16 4096"
      "1 8  1 16 64"
      "2 8  1 16 128"
      "4 8  4 16 256"
      #"8 8  8 16 4096"
    )

    for cfg in "${CONFIGS[@]}"; do
      read -r PODS CX CY THR DRAM_CACHE_KIB <<< "$cfg"
      DRAM_CACHE_SIZE=$((DRAM_CACHE_KIB * 1024))
      CORES=$((CX * CY))
      THREADS_PER_POD=$((CORES * THR))
      N=$((VTX_PER_THREAD * THREADS_PER_POD * PODS))

      CACHE_MB=$((DRAM_CACHE_KIB / 1024))
      RUN_NAME="bfs_csr_sq_p${PODS}_c${CORES}_t${THR}_dc${CACHE_MB}M"
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
        RUN_DEGREE=$((2 * EF + 4))
        echo "Generating RMAT+CUSP graph: N=${N} (scale=${SCALE}), ef=${EF}, cusp=${THREADS_PER_POD}..."
        python3 /work/gen_rmat_csr.py \
          --scale ${SCALE} \
          --ef ${EF} \
          --cusp ${THREADS_PER_POD} \
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

      # Generate pre-initialized BFS state
      echo "Generating BFS init state..."
      python3 /work/gen_bfs_init_state.py \
        --graph "$RUN_DIR/uniform_graph.bin" \
        -o "$RUN_DIR"

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
        --pxn-dram-cache-size=${DRAM_CACHE_SIZE} \
        --pxn-dram-cache-slices=4 \
        --dram-backend-config-sliced=/work/ramulator-configs/HBM-pando-16ch.cfg \
        --pxn-dram-cache-alu=0 \
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
    echo "BFS CSR SHARED-QUEUE SWEEP COMPLETE"
    echo "Results in: build_stampede/drvr/bfs_csr_sq_results/"
    echo "=============================================="
  '
