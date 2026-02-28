# CMakeLists.txt excerpt for building L2SP latency measurement benchmarks
#
# Add this to your main CMakeLists.txt in the drvr/ directory
#

# ============================================================================
# L2SP Latency Measurements
# ============================================================================

# Minimal example - good for getting started
add_executable(drv_l2sp_latency_minimal 
    l2sp_latency_minimal.cpp
)
target_link_libraries(drv_l2sp_latency_minimal pandohammer)

# Full-featured measurement with multiple approaches
add_executable(drv_l2sp_latency
    l2sp_latency_measurement.cpp
)
target_link_libraries(drv_l2sp_latency pandohammer)

# ============================================================================
# Build Instructions
# ============================================================================

# If using CMake from your build directory:
#
#   cd build
#   cmake ..
#   make drv_l2sp_latency_minimal
#   make drv_l2sp_latency
#
# These create ELF executables that can be run in the simulator.

# ============================================================================
# Running in the Simulator
# ============================================================================

# For RISC-V simulation (assuming your simulator setup):
#
#   python3 model/hammerblade-r.py \
#     --num-pxn=1 \
#     --pxn-pods=1 \
#     --pod-cores-x=4 \
#     --pod-cores-y=4 \
#     --core-threads=16 \
#     build/drvr/drv_l2sp_latency_minimal
#
# This runs on 1 PXN, 1 pod, 4x4 cores (16 cores total), 16 threads per core
# (256 total harts)

# For different system configurations:
#   --pod-cores-x=2 --pod-cores-y=2 --core-threads=16  (64 harts)
#   --pod-cores-x=1 --pod-cores-y=1 --core-threads=4   (4 harts)

# ============================================================================
# Optional: SST Direct Execution
# ============================================================================

# If your simulator supports direct execution:
#
#   sst model/hammerblade-r.py --num-pxn=1 --pxn-pods=1 \
#     --pod-cores-x=4 --pod-cores-y=4 --core-threads=16 \
#     build/drvr/drv_l2sp_latency_minimal
#
# Check your simulator documentation for the exact command.

# ============================================================================
# Output Analysis
# ============================================================================

# The program produces:
#   1. Console output with measurements
#   2. SST statistics (if simulator captures them)
#
# Expected output format:
#
#   === L2SP LATENCY MEASUREMENT ===
#   Configuration:
#     Harts per core: 16
#     Cores per pod: 16
#     Total harts: 256
#
#   --- Single Access Latency ---
#   Hart,Core,Thread,AvgLatency(cycles)
#   0,0,0,25
#   1,0,1,26
#   ...
#
# Use these results to:
#   - Compare latency across different hart/core combinations
#   - Analyze bank conflict effects
#   - Profile memory subsystem performance
