#!/bin/bash
set -eo pipefail

# ---------- Environment ----------
export RISCV_HOME=/users/alanandr/local/riscv
export PATH=$RISCV_HOME/bin:/users/alanandr/local/sstcore/bin:$PATH
export LD_LIBRARY_PATH=/users/alanandr/local/sstcore/lib/sst-elements-library:/users/alanandr/local/sstcore/lib:/users/alanandr/local/sstelements/lib:${LD_LIBRARY_PATH:-}

BUILD_DIR="/users/alanandr/drv/build_tiered_faa"
GRAPH_FILE="/users/alanandr/drv/rmat_r12.bin"
MODEL="/users/alanandr/drv/model/drvr.py"
CP="${BUILD_DIR}/pandocommand/libpandocommand_loader.so"
RESULTS_DIR="/users/alanandr/drv/tiered_faa_results"

mkdir -p "${RESULTS_DIR}"

CORE_CONFIGS=(
  "1 1"
  "2 1"
  "4 1"
  "8 1"
  "16 1"
)

VARIANTS=(
  "bfs_work_stealing"
  "bfs_tiered_wq"
  "bfs_tiered_wq_faa"
)

run_bfs_variant() {
    local VARIANT="$1"
    local CX="$2"
    local CY="$3"
    local CORES=$((CX * CY))
    local RUN_NAME="${VARIANT}_c${CORES}"
    local RUN_DIR="${RESULTS_DIR}/${RUN_NAME}"

    echo ""
    echo "======================================================="
    echo "  ${RUN_NAME} (${CX}x${CY} cores, 16 harts/core)"
    echo "======================================================="

    mkdir -p "${RUN_DIR}"
    cp "${GRAPH_FILE}" "${RUN_DIR}/rmat_r16.bin"

    local BINARY="${BUILD_DIR}/rv64/drvr/drvr_${VARIANT}"

    cd "${RUN_DIR}"

    PYTHONPATH=/users/alanandr/drv/py::/users/alanandr/drv/model \
      sst -n 1 \
      "$MODEL" \
      -- \
      --with-command-processor="$CP" \
      --num-pxn=1 \
      --pxn-pods=1 \
      --pod-cores-x=${CX} \
      --pod-cores-y=${CY} \
      --core-threads=16 \
      --pod-l2sp-banks=8 \
      --dram-backend simple \
      --dram-access-time 70ns \
      "$BINARY" \
      2>&1 | tee output.txt

    echo "  Completed: ${RUN_NAME}"
}

# Run all 5 core counts × 3 variants = 15 runs
TOTAL=$((${#CORE_CONFIGS[@]} * ${#VARIANTS[@]}))
COUNT=0
for cfg in "${CORE_CONFIGS[@]}"; do
    read -r CX CY <<< "$cfg"
    for VARIANT in "${VARIANTS[@]}"; do
        COUNT=$((COUNT + 1))
        echo ""
        echo ">>> Run ${COUNT}/${TOTAL} <<<"
        run_bfs_variant "$VARIANT" "$CX" "$CY"
    done
done

# ---------- Extract results ----------
echo ""
echo "======================================================="
echo "  RESULTS SUMMARY"
echo "======================================================="

SUMMARY="${RESULTS_DIR}/summary.csv"
echo "variant,cores,cycles,nodes_discovered,edges_traversed" > "$SUMMARY"

for cfg in "${CORE_CONFIGS[@]}"; do
    read -r CX CY <<< "$cfg"
    CORES=$((CX * CY))
    for VARIANT in "${VARIANTS[@]}"; do
        RUN_DIR="${RESULTS_DIR}/${VARIANT}_c${CORES}"
        OUTFILE="${RUN_DIR}/output.txt"
        if [[ -f "$OUTFILE" ]]; then
            CYCLES=$(grep -oP 'Cycles elapsed:\s+\K[0-9]+' "$OUTFILE" || echo "0")
            NODES=$(grep -oP 'Nodes discovered:\s+\K[0-9]+' "$OUTFILE" || echo "0")
            EDGES=$(grep -oP 'Total edges traversed:\s+\K[0-9]+' "$OUTFILE" || echo "0")
            echo "${VARIANT},${CORES},${CYCLES},${NODES},${EDGES}" >> "$SUMMARY"
            echo "  ${VARIANT} @ ${CORES}c: ${CYCLES} cycles, ${NODES} nodes, ${EDGES} edges"
        else
            echo "  ${VARIANT} @ ${CORES}c: output not found"
        fi
    done
done

echo ""
echo "Summary CSV: ${SUMMARY}"
echo "Done."
