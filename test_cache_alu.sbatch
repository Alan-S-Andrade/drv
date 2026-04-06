#!/bin/bash
#SBATCH -J test_cache_alu
#SBATCH -o logs/%x_%j.out
#SBATCH -e logs/%x_%j.err

#SBATCH -A CCR22006
#SBATCH -p spr
#SBATCH -N 1
#SBATCH -n 1
#SBATCH -c 16
#SBATCH -t 01:00:00

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
MEMH_LIB="/work2/10238/vineeth_architect/stampede3/drv-stack/sst-elements/lib/sst-elements-library/libmemHierarchy.so"

if [[ ! -f "$SIF" ]]; then
    echo "ERROR: Container image not found at $SIF"
    exit 1
fi

# ------------------------------------------------------------------
# STEP 1: BUILD (Drv element + RISC-V binaries)
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
if [[ ! -f "$MEMH_LIB" ]]; then
    echo "ERROR: libmemHierarchy.so not found at $MEMH_LIB"
    echo "Run rebuild_memhierarchy.sh first if needed."
    exit 1
fi

# ------------------------------------------------------------------
# STEP 2: TEST — 2 pods, 1 core: ALU=0 (baseline) vs ALU=2
# ------------------------------------------------------------------
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
    BINARY_MULTI=/work/build_stampede/rv64/drvr/drvr_bfs_csr_weak_multi_roi
    CP=/work/build_stampede/pandocommand/libpandocommand_loader.so
    BASE_DIR=/work/build_stampede/drvr/test_cache_alu

    mkdir -p "$BASE_DIR"

    # Generate 2-pod graph: 2 pods * 1 core * 16 threads * 64 vtx/thread = 2048 vertices
    python3 /work/gen_uniform_csr.py -N 2048 -D 16 --seed 42 -o "$BASE_DIR/graph_2pod.bin"

    COMMON_ARGS="--with-command-processor=$CP \
      --num-pxn=1 \
      --pxn-pods=2 \
      --pod-cores-x=1 \
      --pod-cores-y=1 \
      --core-threads=16 \
      --pod-l2sp-banks=1 \
      --pod-l2sp-interleave=64 \
      --pxn-dram-banks=1 \
      --pxn-dram-cache-size=4194304 \
      --pxn-dram-cache-slices=4 \
      --dram-backend-config-sliced=/work/ramulator-configs/HBM-pando-16ch.cfg"

    # ==================================================================
    # TEST 1: ALU=0 (baseline — all atomics go through Ramulator)
    # ==================================================================
    echo ""
    echo "=============================================="
    echo "TEST 1: 2 pods, 1 core, 4MB cache, ALU=0 (baseline)"
    echo "=============================================="
    RUN_DIR="$BASE_DIR/p2_c1_alu0"
    mkdir -p "$RUN_DIR"
    cp /work/model/summarize.py "$RUN_DIR/"
    cp /work/model/summarize_ramulator.py "$RUN_DIR/"
    cp "$BASE_DIR/graph_2pod.bin" "$RUN_DIR/uniform_graph.bin"
    python3 /work/gen_bfs_init_state.py --graph "$RUN_DIR/uniform_graph.bin" -o "$RUN_DIR"
    cd "$RUN_DIR"

    PYTHONPATH=/work/py::/work/model \
      /install/bin/sst -n 1 \
      "$MODEL" \
      -- \
      $COMMON_ARGS \
      --pxn-dram-cache-alu=0 \
      "$BINARY_MULTI" \
      --V 64 \
      --D 16 \
      | tee "$RUN_DIR/output.txt"

    if [ -f "summarize.py" ]; then python3 summarize.py; fi
    echo ""

    # ==================================================================
    # TEST 2: ALU=2 (coherence-only ALU)
    # ==================================================================
    echo ""
    echo "=============================================="
    echo "TEST 2: 2 pods, 1 core, 4MB cache, ALU=2 (coherence only)"
    echo "=============================================="
    RUN_DIR="$BASE_DIR/p2_c1_alu2"
    mkdir -p "$RUN_DIR"
    cp /work/model/summarize.py "$RUN_DIR/"
    cp /work/model/summarize_ramulator.py "$RUN_DIR/"
    cp "$BASE_DIR/graph_2pod.bin" "$RUN_DIR/uniform_graph.bin"
    python3 /work/gen_bfs_init_state.py --graph "$RUN_DIR/uniform_graph.bin" -o "$RUN_DIR"
    cd "$RUN_DIR"

    PYTHONPATH=/work/py::/work/model \
      /install/bin/sst -n 1 \
      "$MODEL" \
      -- \
      $COMMON_ARGS \
      --pxn-dram-cache-alu=2 \
      "$BINARY_MULTI" \
      --V 64 \
      --D 16 \
      | tee "$RUN_DIR/output.txt"

    if [ -f "summarize.py" ]; then python3 summarize.py; fi
    echo ""

    # ==================================================================
    # COMPARISON
    # ==================================================================
    echo ""
    echo "=============================================="
    echo "COMPARISON"
    echo "=============================================="
    echo ""
    echo "--- TEST 1: ALU=0 (baseline) ---"
    grep -E "(BFS done|Simulation is complete|RESULT)" "$BASE_DIR/p2_c1_alu0/output.txt" || true
    echo ""
    echo "--- TEST 2: ALU=2 (coherence-only) ---"
    grep -E "(BFS done|Simulation is complete|RESULT)" "$BASE_DIR/p2_c1_alu2/output.txt" || true
    echo ""

    # BFS correctness check
    R1=$(grep "sum_dist" "$BASE_DIR/p2_c1_alu0/output.txt" | head -1)
    R2=$(grep "sum_dist" "$BASE_DIR/p2_c1_alu2/output.txt" | head -1)
    echo "Correctness:"
    if [ "$R1" = "$R2" ]; then
      echo "  PASS: BFS results match"
    else
      echo "  WARNING: Results differ"
      echo "    ALU=0: $R1"
      echo "    ALU=2: $R2"
    fi

    echo ""
    echo "--- Per-slice DRAM cache stats ---"
    echo "ALU=0:"
    grep "pxn0_dram0_s" "$BASE_DIR/p2_c1_alu0/output.txt" | grep -E "^\w" | head -4
    echo ""
    echo "ALU=2:"
    grep "pxn0_dram0_s" "$BASE_DIR/p2_c1_alu2/output.txt" | grep -E "^\w" | head -4

    echo ""
    echo "=============================================="
    echo "TESTS COMPLETE"
    echo "=============================================="
  '
