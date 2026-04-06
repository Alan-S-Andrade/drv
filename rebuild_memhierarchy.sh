#!/bin/bash
# Rebuild libmemHierarchy.so with DRV_CACHE_ALU support
# Direct g++ compilation bypassing broken libtool
set -e

SST_ELEMENTS_SRC="/work2/10238/vineeth_architect/stampede3/drv_stack/sst-elements/src"
SST_ELEMENTS_BUILD="/work2/10238/vineeth_architect/stampede3/drv_stack/sst-elements/build"
SST_CORE="/work2/10238/vineeth_architect/stampede3/drv-stack/sst-core"
SST_ELEMENTS_INSTALL="/work2/10238/vineeth_architect/stampede3/drv-stack/sst-elements"
DRV_API="/work2/10238/vineeth_architect/stampede3/drv_copy/drv/api"
DRV_ELEMENT="/work2/10238/vineeth_architect/stampede3/drv_copy/drv/element"
MEMH_SRC="$SST_ELEMENTS_SRC/sst/elements/memHierarchy"

OUTPUT_DIR="$SST_ELEMENTS_INSTALL/lib/sst-elements-library"
OUTPUT_SO="$OUTPUT_DIR/libmemHierarchy.so"
OBJ_DIR="$SST_ELEMENTS_BUILD/src/sst/elements/memHierarchy/.cache_alu_objs"

RAMULATOR_SRC="/work2/10238/vineeth_architect/stampede3/drv_copy/ramulator-build/src"

INCLUDES="-I$SST_ELEMENTS_BUILD/src \
  -I$SST_ELEMENTS_SRC \
  -I$SST_CORE/include \
  -I$SST_CORE/include/sst/core \
  -I$MEMH_SRC \
  -I$DRV_API \
  -I$DRV_ELEMENT \
  -I$RAMULATOR_SRC"

CXXFLAGS="-std=c++17 -g -O2 -fPIC -DHAVE_CONFIG_H -DDRV_CACHE_ALU"

# Exact source files from the autotools Makefile (am_libmemHierarchy_la_OBJECTS)
SOURCES="
cacheController.cc
cacheFactory.cc
bus.cc
memoryController.cc
memoryCacheController.cc
coherentMemoryController.cc
membackend/timingDRAMBackend.cc
membackend/memBackendConvertor.cc
membackend/simpleMemBackendConvertor.cc
membackend/flagMemBackendConvertor.cc
membackend/extMemBackendConvertor.cc
membackend/delayBuffer.cc
membackend/simpleMemBackend.cc
membackend/simpleDRAMBackend.cc
membackend/requestReorderSimple.cc
membackend/requestReorderByRow.cc
membackend/vaultSimBackend.cc
membackend/MessierBackend.cc
membackend/scratchBackendConvertor.cc
membackend/simpleMemScratchBackendConvertor.cc
membackend/cramSimBackend.cc
memLink.cc
memNIC.cc
memNICFour.cc
customcmd/defCustomCmdHandler.cc
directoryController.cc
scratchpad.cc
coherencemgr/coherenceController.cc
memHierarchyInterface.cc
memHierarchyScratchInterface.cc
standardInterface.cc
coherencemgr/MESI_L1.cc
coherencemgr/MESI_Inclusive.cc
coherencemgr/MESI_Private_Noninclusive.cc
coherencemgr/MESI_Shared_Noninclusive.cc
coherencemgr/Incoherent_L1.cc
coherencemgr/Incoherent.cc
multithreadL1Shim.cc
mshr.cc
testcpu/trivialCPU.cc
testcpu/streamCPU.cc
testcpu/scratchCPU.cc
testcpu/standardCPU.cc
dmaEngine.cc
networkMemInspector.cc
Sieve/sieveController.cc
Sieve/sieveFactory.cc
Sieve/broadcastShim.cc
Sieve/memmgr_sieve.cc
memNetBridge.cc
testcpu/standardMMIO.cc
membackend/ramulatorBackend.cc
"

echo "=== Rebuilding libmemHierarchy.so with DRV_CACHE_ALU ==="
mkdir -p "$OBJ_DIR/membackend" "$OBJ_DIR/coherencemgr" "$OBJ_DIR/customcmd" "$OBJ_DIR/testcpu" "$OBJ_DIR/Sieve"
cd "$MEMH_SRC"

OBJECTS=""
COMPILED=0
SKIPPED=0

for src in $SOURCES; do
  obj="$OBJ_DIR/${src%.cc}.o"
  OBJECTS="$OBJECTS $obj"

  if [ "$MEMH_SRC/$src" -nt "$obj" ] 2>/dev/null; then
    echo "  CXX $src"
    g++ $CXXFLAGS $INCLUDES -c "$src" -o "$obj" || exit 1
    COMPILED=$((COMPILED + 1))
  else
    SKIPPED=$((SKIPPED + 1))
  fi
done

echo "  Compiled: $COMPILED, Skipped (up to date): $SKIPPED"
echo ""
RAMULATOR_LIB="/work2/10238/vineeth_architect/stampede3/drv_copy/ramulator-build/libramulator.so"

echo "=== Linking libmemHierarchy.so ==="
mkdir -p "$OUTPUT_DIR"
RAMULATOR_LIB_DIR="/work2/10238/vineeth_architect/stampede3/drv_copy/ramulator-build"
g++ -shared -o "$OUTPUT_SO" $OBJECTS \
  -L"$RAMULATOR_LIB_DIR" -lramulator \
  -Wl,-rpath,/install/lib
echo "  Output: $OUTPUT_SO ($(du -h "$OUTPUT_SO" | cut -f1))"
echo "=== Done ==="
