#!/bin/bash
#SBATCH -J bfs_cache_sweep
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
# STEP 2: DRAM CACHE SIZE SWEEP (fixed 8 cores, 1 pod)
# ------------------------------------------------------------------

# Graph parameters
VTX_PER_THREAD=1024
DEGREE=16

# Graph type: "uniform" or "rmat"
GRAPH_TYPE="rmat"
EF=16

# Fixed compute configuration: 1 pod, 8x2=16 cores, 16 threads/core
PODS=1
CX=8
CY=2
THR=16

# DRAM cache sizes to sweep (bytes): 128KB, 192KB, 256KB
CACHE_SIZES=(131072 196608 262144)

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
    SWEEP_DIR=/work/build_stampede/drvr/bfs_csr_sq_cache_sweep

    VTX_PER_THREAD='"${VTX_PER_THREAD}"'
    DEGREE='"${DEGREE}"'
    GRAPH_TYPE='"${GRAPH_TYPE}"'
    EF='"${EF}"'

    PODS='"${PODS}"'
    CX='"${CX}"'
    CY='"${CY}"'
    THR='"${THR}"'

    CORES=$((CX * CY))
    THREADS_PER_POD=$((CORES * THR))
    N=$((VTX_PER_THREAD * THREADS_PER_POD * PODS))

    mkdir -p "$SWEEP_DIR"

    # ---- Generate graph once (shared across all cache sizes) ----
    GRAPH_DIR="${SWEEP_DIR}/graph_data"
    mkdir -p "$GRAPH_DIR"

    if [ "$GRAPH_TYPE" = "rmat" ]; then
      SCALE=$(python3 -c "import math; print(int(math.log2($N)))")
      RUN_DEGREE=$((2 * EF + 4))
      echo "Generating RMAT+CUSP graph: N=${N} (scale=${SCALE}), ef=${EF}, cusp=${THREADS_PER_POD}..."
      python3 /work/gen_rmat_csr.py \
        --scale ${SCALE} \
        --ef ${EF} \
        --cusp ${THREADS_PER_POD} \
        --seed 42 \
        -o "$GRAPH_DIR/uniform_graph.bin"
    else
      RUN_DEGREE=${DEGREE}
      echo "Generating uniform graph: N=${N} degree=${DEGREE}..."
      python3 /work/gen_uniform_csr.py \
        -N ${N} \
        -D ${DEGREE} \
        --seed 42 \
        -o "$GRAPH_DIR/uniform_graph.bin"
    fi

    echo "Generating BFS init state..."
    python3 /work/gen_bfs_init_state.py \
      --graph "$GRAPH_DIR/uniform_graph.bin" \
      -o "$GRAPH_DIR"

    # ---- Sweep DRAM cache sizes ----
    CACHE_SIZES=('"$(IFS=' '; echo "${CACHE_SIZES[*]}")"')

    for CACHE_SZ in ${CACHE_SIZES[@]}; do
      CACHE_KB=$((CACHE_SZ / 1024))
      RUN_NAME="bfs_csr_sq_c${CORES}_cache${CACHE_KB}KB"
      RUN_DIR="${SWEEP_DIR}/${RUN_NAME}"

      echo "=============================================="
      echo "RUN: ${RUN_NAME} (${PODS} pod, ${CX}x${CY}=${CORES} cores, ${THR} thr/core, DRAM cache=${CACHE_KB}KB, N=${N})"
      echo "=============================================="

      mkdir -p "$RUN_DIR"
      cp /work/model/summarize.py "$RUN_DIR/"
      cp /work/model/summarize_ramulator.py "$RUN_DIR/"

      # Symlink graph data into run dir (avoid duplicating ~72MB per run)
      ln -sf "$GRAPH_DIR/uniform_graph.bin" "$RUN_DIR/uniform_graph.bin"
      for f in "$GRAPH_DIR"/bfs_init_*.bin; do
        ln -sf "$f" "$RUN_DIR/$(basename $f)"
      done

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
        --pxn-dram-cache-size=${CACHE_SZ} \
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
    echo "DRAM CACHE SIZE SWEEP COMPLETE"
    echo "Results in: build_stampede/drvr/bfs_csr_sq_cache_sweep/"
    echo "=============================================="
  '
