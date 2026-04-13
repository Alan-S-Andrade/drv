#!/bin/bash
#SBATCH -J bfs_lat_sweep
#SBATCH -o logs/%x_%j.out
#SBATCH -e logs/%x_%j.err

#SBATCH -A CCR22006
#SBATCH -p spr
#SBATCH -N 1
#SBATCH -n 1
#SBATCH -c 112
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
# STEP 1b: Use pre-built libmemHierarchy.so with coherence ALU tagging
# ------------------------------------------------------------------
MEMH_LIB="/work2/10238/vineeth_architect/stampede3/drv-stack/sst-elements/lib/sst-elements-library/libmemHierarchy.so"

if [[ ! -f "$MEMH_LIB" ]]; then
    echo "ERROR: libmemHierarchy.so not found at $MEMH_LIB"
    echo "Run rebuild_memhierarchy.sh first if needed."
    exit 1
fi

# ------------------------------------------------------------------
# STEP 2: RUN BFS CSR SHARED-QUEUE DRAM LINK LATENCY SWEEP
# ------------------------------------------------------------------

# Fixed 64-core config: 8x8 cores, 16 thread/core
PODS=1
CX=8
CY=8
THR=1
DRAM_CACHE_KIB=512

# Graph parameters
VTX_PER_THREAD=1024
GRAPH_TYPE="rmat"
EF=16

# Latencies to sweep (in ns)
LATENCIES=(1)     #(50 400 200 12)                       #(800 100 25 1)           #(50 400 200 12)           #(800 100 25 1)   

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
    BINARY=/work/build_stampede/rv64/drvr/drvr_bfs_csr_shared_queue_baseline
    CP=/work/build_stampede/pandocommand/libpandocommand_loader.so
    SWEEP_DIR=/work/build_stampede/drvr/bfs_csr_sq_latency_single_sweep_final2

    PODS='"${PODS}"'
    CX='"${CX}"'
    CY='"${CY}"'
    THR='"${THR}"'
    DRAM_CACHE_KIB='"${DRAM_CACHE_KIB}"'
    VTX_PER_THREAD='"${VTX_PER_THREAD}"'
    GRAPH_TYPE='"${GRAPH_TYPE}"'
    EF='"${EF}"'

    DRAM_CACHE_SIZE=$((DRAM_CACHE_KIB * 1024))
    CORES=$((CX * CY))
    THREADS_PER_POD=$((CORES * THR))
    N=$((VTX_PER_THREAD * THREADS_PER_POD * PODS))

    mkdir -p "$SWEEP_DIR"

    # Generate graph once (same for all latency points)
    SCALE=$(python3 -c "import math; print(int(math.log2($N)))")
    RUN_DEGREE=$((2 * EF + 4))

    GRAPH_DIR="${SWEEP_DIR}/shared_graph"
    mkdir -p "$GRAPH_DIR"

    echo "Generating RMAT+CUSP graph: N=${N} (scale=${SCALE}), ef=${EF}, cusp=${THREADS_PER_POD}..."
    python3 /work/gen_rmat_csr.py \
      --scale ${SCALE} \
      --ef ${EF} \
      --cusp ${THREADS_PER_POD} \
      --seed 42 \
      -o "$GRAPH_DIR/uniform_graph.bin"

    echo "Generating BFS init state..."
    python3 /work/gen_bfs_init_state.py \
      --graph "$GRAPH_DIR/uniform_graph.bin" \
      -o "$GRAPH_DIR"

    LATENCIES=('"$(echo "${LATENCIES[@]}")"')

    for LAT in "${LATENCIES[@]}"; do
      RUN_NAME="bfs_csr_sq_64c_lat${LAT}ns"
      RUN_DIR="${SWEEP_DIR}/${RUN_NAME}"

      echo "=============================================="
      echo "RUN: ${RUN_NAME} (64 cores, DRAM link latency = ${LAT}ns)"
      echo "=============================================="

      mkdir -p "$RUN_DIR"
      cp /work/model/summarize.py "$RUN_DIR/"
      cp /work/model/summarize_ramulator.py "$RUN_DIR/"

      # Symlink shared graph data into run dir
      ln -sf "$GRAPH_DIR/uniform_graph.bin" "$RUN_DIR/uniform_graph.bin"
      ln -sf "$GRAPH_DIR/bfs_dist_init.bin" "$RUN_DIR/bfs_dist_init.bin"

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
        --pxn-dram-link-latency="${LAT}ns" \
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
    echo "DRAM LINK LATENCY SWEEP COMPLETE"
    echo "Results in: build_stampede/drvr/bfs_csr_sq_latency_single_sweep_final2/"
    echo "=============================================="
  '
