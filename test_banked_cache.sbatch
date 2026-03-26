#!/bin/bash
#SBATCH -J test_banked_cache
#SBATCH -o logs/%x_%j.out
#SBATCH -e logs/%x_%j.err

#SBATCH -A CCR22006
#SBATCH -p pvc
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
# STEP 2: QUICK TEST - 1 core, 16 threads, tiny graph
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
    RUN_DIR=/work/build_stampede/drvr/test_banked_cache

    mkdir -p "$RUN_DIR"
    cp /work/model/summarize.py "$RUN_DIR/"
    cp /work/model/summarize_ramulator.py "$RUN_DIR/"

    # Tiny uniform graph: 1 core * 16 threads * 64 vtx/thread = 1024 vertices
    echo "Generating tiny uniform graph..."
    python3 /work/gen_uniform_csr.py \
      -N 1024 \
      -D 16 \
      --seed 42 \
      -o "$RUN_DIR/uniform_graph.bin"

    cd "$RUN_DIR"

    echo "Running with --pxn-dram-cache-banks=4 ..."
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
      --pxn-dram-cache-banks=4 \
      "$BINARY" \
      --V 64 \
      --D 16 \
      | tee "$RUN_DIR/output.txt"

    echo ""
    echo "=== TEST COMPLETE ==="
    if grep -q "RESULT: PASS" "$RUN_DIR/output.txt"; then
      echo "RESULT: PASS"
    else
      echo "RESULT: FAIL or DID NOT FINISH"
    fi
  '
