#!/bin/bash
#SBATCH -J bfs_pgas_scatter
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
# STEP 2: GENERATE RMAT GRAPH (once, shared across all runs)
# ------------------------------------------------------------------
SCALE=14
EF=16

apptainer exec --cleanenv \
  --env XALT_EXECUTABLE_TRACKING=no \
  --bind "$SCRIPT_DIR:/work" \
  "$SIF" \
  bash -lc '
    set -e
    SCALE='"${SCALE}"'
    EF='"${EF}"'
    N=$((1 << SCALE))
    GRAPH_DIR="/work/build_stampede/drvr/bfs_pgas_scatter_results"
    mkdir -p "$GRAPH_DIR"
    echo "Generating RMAT graph: scale=${SCALE} ef=${EF} N=${N}..."
    python3 /work/gen_rmat_csr.py \
      --scale ${SCALE} \
      --ef ${EF} \
      --seed 42 \
      -o "$GRAPH_DIR/graph.bin"
  '

# ------------------------------------------------------------------
# STEP 3: SWEEP VGID COUNTS
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
    BINARY=/work/build_stampede/rv64/drvr/drvr_bfs_pgas_scatter
    CP=/work/build_stampede/pandocommand/libpandocommand_loader.so
    SWEEP_DIR=/work/build_stampede/drvr/bfs_pgas_scatter_results

    SCALE='"${SCALE}"'
    EF='"${EF}"'
    N=$((1 << SCALE))

    # Hardware config
    CX=8
    CY=4
    THR=16
    # Single SST DRAM bank — ramulator internally models 64 HBM channels.
    # RoBaRaCoCh mapping: channel bits are lowest, so consecutive 32B blocks
    # rotate across channels. VGID scatter spreads hub dist[] entries across
    # different channel regions.
    DRAM_BANKS=1

    # VGID sweep: 1 (baseline) through increasing scatter
    VGID_COUNTS=(1 4 16)

    for NVGIDS in "${VGID_COUNTS[@]}"; do
      RUN_NAME="scatter_vgid${NVGIDS}_s${SCALE}_c$((CX*CY))_t${THR}"
      RUN_DIR="${SWEEP_DIR}/${RUN_NAME}"

      echo "=============================================="
      echo "RUN: ${RUN_NAME} (${NVGIDS} VGIDs, scale=${SCALE}, ${CX}x${CY} cores, ${THR} thr/core, 1 SST bank / 64 HBM channels)"
      echo "=============================================="

      mkdir -p "$RUN_DIR"
      cp /work/model/summarize.py "$RUN_DIR/"
      cp /work/model/summarize_ramulator.py "$RUN_DIR/" 2>/dev/null || true

      # Link the shared graph file
      ln -sf "$SWEEP_DIR/graph.bin" "$RUN_DIR/graph.bin"

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
        --pxn-dram-banks=${DRAM_BANKS} \
        "$BINARY" \
        --scale ${SCALE} \
        --ef ${EF} \
        --vgids ${NVGIDS} \
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
    echo "PGAS SCATTER SWEEP COMPLETE"
    echo "Results in: ${SWEEP_DIR}"
    echo "=============================================="
  '
