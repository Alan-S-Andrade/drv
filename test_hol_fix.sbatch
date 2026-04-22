#!/bin/bash
#SBATCH -J test_hol_fix
#SBATCH -o logs/%x_%j.out
#SBATCH -e logs/%x_%j.err

#SBATCH -A CCR22006
#SBATCH -p pvc
#SBATCH -N 1
#SBATCH -n 1
#SBATCH -c 96
#SBATCH -t 02:00:00

# Quick test: verify that the head-of-line blocking fix in
# memBackendConvertor.cc improves scaling with hart count.
#
# Runs BFS baseline on 8 cores with t1 vs t16 on a small graph.
# Key metric: "Measured Avg DRAM Latency (MemCtrl)" in summarize output.
# Before fix: t16 showed ~10x higher MemCtrl latency than t1.
# After fix:  latency gap should shrink significantly.

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

if [[ ! -f "$SIF" ]]; then echo "ERROR: Container image not found at $SIF"; exit 1; fi
if [[ ! -f "$MEMH_LIB" ]]; then echo "ERROR: libmemHierarchy.so not found at $MEMH_LIB"; exit 1; fi

# ------------------------------------------------------------------
# STEP 1: BUILD (reuse existing binaries, just ensure up to date)
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

    make -j'"${SLURM_CPUS_PER_TASK}"' rv64 Drv pandocommand_loader
    echo "Build complete"
  '

# ------------------------------------------------------------------
# STEP 2: RUN t1 vs t16 on 8 cores, small graph
# ------------------------------------------------------------------
# Small config: 8 cores, VTX_PER_THREAD=1024, uniform graph
# This keeps runtime short while exercising the DRAM path.

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
    TEST_DIR=/work/build_stampede/drvr/test_hol_fix

    VTX_PER_THREAD=1024
    DEGREE=16
    CX=8
    CY=1

    mkdir -p "$TEST_DIR"

    # Generate ONE shared graph for the larger config (t16)
    # t1:  N = 1024 * 8 * 1  =  8192
    # t16: N = 1024 * 8 * 16 = 131072
    # Use the t16 graph size for both (t1 will just use fewer harts on a bigger graph)
    N_GRAPH=$((VTX_PER_THREAD * CX * CY * 16))

    GRAPH_DIR="${TEST_DIR}/shared_graph"
    mkdir -p "$GRAPH_DIR"

    echo "Generating uniform graph: N=${N_GRAPH} degree=${DEGREE}..."
    python3 /work/gen_uniform_csr.py \
      -N ${N_GRAPH} \
      -D ${DEGREE} \
      --seed 42 \
      -o "$GRAPH_DIR/uniform_graph.bin"

    echo "Generating BFS init state..."
    python3 /work/gen_bfs_init_state.py \
      --graph "$GRAPH_DIR/uniform_graph.bin" \
      -o "$GRAPH_DIR"

    # ---- Run t1 and t16 ----
    for THR in 1 16; do
      CORES=$((CX * CY))
      RUN_NAME="hol_test_c${CORES}_t${THR}"
      RUN_DIR="${TEST_DIR}/${RUN_NAME}"

      echo ""
      echo "=============================================="
      echo "RUN: ${RUN_NAME} (8 cores, ${THR} harts/core, $(( CORES * THR )) total harts)"
      echo "=============================================="

      mkdir -p "$RUN_DIR"
      cp /work/model/summarize.py "$RUN_DIR/"
      cp /work/model/summarize_ramulator.py "$RUN_DIR/"
      ln -sf "$GRAPH_DIR/uniform_graph.bin" "$RUN_DIR/uniform_graph.bin"
      ln -sf "$GRAPH_DIR/bfs_dist_init.bin" "$RUN_DIR/bfs_dist_init.bin"

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
        --pxn-dram-banks=1 \
        --pxn-dram-cache-size=0 \
        --pxn-dram-cache-slices=4 \
        --dram-backend-config-sliced=/work/ramulator-configs/HBM-pando-16ch.cfg \
        --pxn-dram-cache-alu=0 \
        "$BINARY" \
        --V ${VTX_PER_THREAD} \
        --D ${DEGREE} \
        | tee "$RUN_DIR/output.txt"

      echo ""
      echo "--- Summary for ${RUN_NAME} ---"
      python3 summarize.py 2>&1 | grep -E "simulated time|Useful Phase|DRAM Latency|MemCtrl|Outstanding|Avg Load Latency|rejected|cycles_with"
      echo ""

      cd /work
    done

    echo ""
    echo "=============================================="
    echo "HOL FIX TEST COMPLETE"
    echo "Compare Measured Avg DRAM Latency (MemCtrl) between t1 and t16."
    echo "Before fix: ~10x gap. After fix: should be much closer."
    echo "Results in: build_stampede/drvr/test_hol_fix/"
    echo "=============================================="
  '
