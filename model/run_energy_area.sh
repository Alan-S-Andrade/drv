#!/usr/bin/env bash
set -euo pipefail
#
# Run CACTI and McPAT for the DRV PandoHammer single-pod configuration:
#   64 cores, 16 harts/core, 128 KiB L1SP, 1 MiB L2SP (8 banks), 22 nm
#
# Prerequisites:
#   - CACTI 6.5 or 7 binary in PATH (or set CACTI_BIN)
#   - McPAT binary in PATH (or set MCPAT_BIN)
#
# Usage:
#   bash model/run_energy_area.sh
#   CACTI_BIN=/path/to/cacti MCPAT_BIN=/path/to/mcpat bash model/run_energy_area.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="${SCRIPT_DIR}/energy_area_results"
mkdir -p "$OUT_DIR"

CACTI_BIN="${CACTI_BIN:-/users/alanandr/cacti/cacti}"
CACTI_DIR="$(dirname "$CACTI_BIN")"
MCPAT_BIN="${MCPAT_BIN:-/users/alanandr/mcpat/mcpat}"

echo "=============================="
echo " DRV PandoHammer Energy/Area"
echo " 64 cores, 16 harts, 128K L1SP"
echo " 1 MiB L2SP (8 banks), 22 nm"
echo "=============================="
echo ""

# ---------- CACTI ----------
echo "--- CACTI: L1SP (128 KiB per core, 64 instances) ---"
( cd "$CACTI_DIR" && "$CACTI_BIN" -infile "${SCRIPT_DIR}/cacti_l1sp.cfg" ) | tee "$OUT_DIR/cacti_l1sp.out"
echo ""

echo "--- CACTI: L2SP bank (128 KiB per bank, 8 instances) ---"
( cd "$CACTI_DIR" && "$CACTI_BIN" -infile "${SCRIPT_DIR}/cacti_l2sp_bank.cfg" ) | tee "$OUT_DIR/cacti_l2sp_bank.out"
echo ""

echo "--- CACTI: DRAM Cache (64 KiB, 8-way) ---"
( cd "$CACTI_DIR" && "$CACTI_BIN" -infile "${SCRIPT_DIR}/cacti_dram_cache.cfg" ) | tee "$OUT_DIR/cacti_dram_cache.out"
echo ""

# ---------- McPAT ----------
echo "--- McPAT: Full pod (64 cores, 8 L2 banks, NoC, MC) ---"
"$MCPAT_BIN" -infile "${SCRIPT_DIR}/mcpat_64c_16h_128k.xml" -print_level 5 | tee "$OUT_DIR/mcpat_pod.out"
echo ""

# ---------- Summary ----------
echo "=============================="
echo " Summary"
echo "=============================="
echo ""
echo "Raw outputs saved to: $OUT_DIR/"
echo "  cacti_l1sp.out        — per-instance L1SP area/energy/latency"
echo "  cacti_l2sp_bank.out   — per-instance L2SP bank area/energy/latency"
echo "  cacti_dram_cache.out  — per-instance DRAM cache area/energy/latency"
echo "  mcpat_pod.out         — full pod area/power breakdown"
echo ""
echo "To get pod totals from CACTI, multiply:"
echo "  L1SP:  per-instance × 64 cores"
echo "  L2SP:  per-instance × 8 banks"
echo "  DRAM$: per-instance × 1 bank"
echo ""
echo "McPAT already accounts for 64 homogeneous cores and 8 L2 banks."
echo ""
echo "Note: Activity stats are zeroed (structural/leakage only)."
echo "Fill stats from simulator output for dynamic power estimates."
