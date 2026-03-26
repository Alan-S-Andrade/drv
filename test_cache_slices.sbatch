#!/bin/bash
#SBATCH -J test_cache_slices
#SBATCH -o logs/%x_%j.out
#SBATCH -e logs/%x_%j.err

#SBATCH -A CCR22006
#SBATCH -p spr
#SBATCH -N 1
#SBATCH -n 1
#SBATCH -c 16
#SBATCH -t 00:30:00

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
# STEP 2: SMOKE TESTS — slices=1 (baseline) vs slices=2
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
    BINARY_SINGLE=/work/build_stampede/rv64/drvr/drvr_bfs_csr_weak_roi
    BINARY_MULTI=/work/build_stampede/rv64/drvr/drvr_bfs_csr_weak_multi_roi
    CP=/work/build_stampede/pandocommand/libpandocommand_loader.so
    BASE_DIR=/work/build_stampede/drvr/test_cache_slices

    mkdir -p "$BASE_DIR"

    # --- Generate tiny graphs ---
    # 1-pod graph: 1 core * 16 threads * 64 vtx/thread = 1024 vertices
    python3 /work/gen_uniform_csr.py -N 1024 -D 16 --seed 42 -o "$BASE_DIR/graph_1pod.bin"
    # 2-pod graph: 2 pods * 1 core * 16 threads * 64 vtx/thread = 2048 vertices
    python3 /work/gen_uniform_csr.py -N 2048 -D 16 --seed 42 -o "$BASE_DIR/graph_2pod.bin"

    PASS=true

    # ==================================================================
    # TEST 1: 1 pod, slices=1 (baseline — must match existing behavior)
    # ==================================================================
    echo ""
    echo "=============================================="
    echo "TEST 1: 1 pod, 1 core, slices=1 (baseline)"
    echo "=============================================="
    RUN_DIR="$BASE_DIR/p1_c1_slices1"
    mkdir -p "$RUN_DIR"
    cp /work/model/summarize.py "$RUN_DIR/"
    cp /work/model/summarize_ramulator.py "$RUN_DIR/"
    cp "$BASE_DIR/graph_1pod.bin" "$RUN_DIR/uniform_graph.bin"
    python3 /work/gen_bfs_init_state.py --graph "$RUN_DIR/uniform_graph.bin" -o "$RUN_DIR"
    cd "$RUN_DIR"

    PYTHONPATH=/work/py::/work/model \
      /install/bin/sst -n 1 \
      "$MODEL" \
      -- \
      --with-command-processor="$CP" \
      --num-pxn=1 \
      --pxn-pods=1 \
      --pod-cores-x=1 \
      --pod-cores-y=1 \
      --core-threads=16 \
      --pod-l2sp-banks=1 \
      --pod-l2sp-interleave=64 \
      --pxn-dram-banks=1 \
      --pxn-dram-cache-slices=1 \
      "$BINARY_SINGLE" \
      --V 64 \
      --D 16 \
      | tee "$RUN_DIR/output.txt"

    if [ -f "summarize.py" ]; then python3 summarize.py; fi
    echo ""

    # ==================================================================
    # TEST 2: 1 pod, slices=2 (must produce same BFS results)
    # ==================================================================
    echo ""
    echo "=============================================="
    echo "TEST 2: 1 pod, 1 core, slices=2"
    echo "=============================================="
    RUN_DIR="$BASE_DIR/p1_c1_slices2"
    mkdir -p "$RUN_DIR"
    cp /work/model/summarize.py "$RUN_DIR/"
    cp /work/model/summarize_ramulator.py "$RUN_DIR/"
    cp "$BASE_DIR/graph_1pod.bin" "$RUN_DIR/uniform_graph.bin"
    python3 /work/gen_bfs_init_state.py --graph "$RUN_DIR/uniform_graph.bin" -o "$RUN_DIR"
    cd "$RUN_DIR"

    PYTHONPATH=/work/py::/work/model \
      /install/bin/sst -n 1 \
      "$MODEL" \
      -- \
      --with-command-processor="$CP" \
      --num-pxn=1 \
      --pxn-pods=1 \
      --pod-cores-x=1 \
      --pod-cores-y=1 \
      --core-threads=16 \
      --pod-l2sp-banks=1 \
      --pod-l2sp-interleave=64 \
      --pxn-dram-banks=1 \
      --pxn-dram-cache-slices=2 \
      --dram-backend-config-sliced=/work/ramulator-configs/HBM-pando-32ch.cfg \
      "$BINARY_SINGLE" \
      --V 64 \
      --D 16 \
      | tee "$RUN_DIR/output.txt"

    if [ -f "summarize.py" ]; then python3 summarize.py; fi
    echo ""

    # ==================================================================
    # TEST 3: 2 pods, slices=2 (multi-pod + multi-slice)
    # ==================================================================
    echo ""
    echo "=============================================="
    echo "TEST 3: 2 pods, 1 core, slices=2"
    echo "=============================================="
    RUN_DIR="$BASE_DIR/p2_c1_slices2"
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
      --with-command-processor="$CP" \
      --num-pxn=1 \
      --pxn-pods=2 \
      --pod-cores-x=1 \
      --pod-cores-y=1 \
      --core-threads=16 \
      --pod-l2sp-banks=1 \
      --pod-l2sp-interleave=64 \
      --pxn-dram-banks=1 \
      --pxn-dram-cache-slices=2 \
      --dram-backend-config-sliced=/work/ramulator-configs/HBM-pando-32ch.cfg \
      "$BINARY_MULTI" \
      --V 64 \
      --D 16 \
      | tee "$RUN_DIR/output.txt"

    if [ -f "summarize.py" ]; then python3 summarize.py; fi
    echo ""

    # ==================================================================
    # RESULTS COMPARISON
    # ==================================================================
    echo ""
    echo "=============================================="
    echo "RESULTS COMPARISON"
    echo "=============================================="
    echo "--- Test 1 (baseline slices=1) ---"
    grep -E "(iterations|reached|sum_dist|RESULT)" "$BASE_DIR/p1_c1_slices1/output.txt" || true
    echo ""
    echo "--- Test 2 (slices=2, same config) ---"
    grep -E "(iterations|reached|sum_dist|RESULT)" "$BASE_DIR/p1_c1_slices2/output.txt" || true
    echo ""
    echo "--- Test 3 (2 pods, slices=2) ---"
    grep -E "(iterations|reached|sum_dist|RESULT)" "$BASE_DIR/p2_c1_slices2/output.txt" || true
    echo ""
    echo "=============================================="
    echo "ALL TESTS COMPLETE"
    echo "=============================================="
  '
