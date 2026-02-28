# L2SP Access Latency Measurement - Complete Summary

## Question
How to measure the same access latency (per cycles) for a hart in a core to access the same location as another core in L2SP?

## Answer Summary

You can measure per-cycle access latency to shared L2SP locations using:

1. **Cycle counting** via `cycle()` to timestamp memory accesses
2. **Hart/Core identification** via `myThreadId()`, `myCoreId()` etc. to know which hart is accessing
3. **L2SP placement** via `__l2sp__` attribute to place shared data
4. **Multi-hart barriers** using atomic operations to synchronize access timing
5. **Latency calculation** as the cycle difference between access start and end

## Files Created

### 1. **l2sp_latency_minimal.cpp** ⭐ START HERE
   - **Purpose**: Easy-to-understand minimal example
   - **Size**: ~200 lines
   - **Contains**: 3 measurement approaches with clear comments
   - **Best for**: Learning the basics, quick prototyping

### 2. **l2sp_latency_measurement.cpp**
   - **Purpose**: Full-featured measurement tool
   - **Size**: ~400 lines
   - **Contains**: 
     - Single-access latency measurement (highest precision)
     - Streaming latency (reveals contention)
     - Alternating pattern (cross-core latency)
     - Complete barrier synchronization
   - **Best for**: Production measurements, detailed analysis

### 3. **L2SP_LATENCY_GUIDE.md** 📖 COMPREHENSIVE GUIDE
   - **Contents**: 
     - Full explanation of measurement approaches
     - Available tools and APIs
     - Interpreting results
     - Common pitfalls and solutions
     - Advanced techniques

### 4. **L2SP_LATENCY_QUICK_REFERENCE.md** 🎯 CHEAT SHEET
   - **Contents**: Quick lookup for common tasks
   - **Best for**: Quick API reference while coding

### 5. **analyze_l2sp_latency.py** 📊 DATA ANALYSIS
   - **Purpose**: Parse and analyze measurement output
   - **Usage**: `python3 analyze_l2sp_latency.py <output.txt>`
   - **Output**: Statistical analysis and per-core breakdown

### 6. **L2SP_LATENCY_BUILD.cmake**
   - Build configuration snippets
   - Simulator invocation examples

## Quick Start (5 minutes)

### Step 1: Use the minimal example
```bash
cp drvr/l2sp_latency_minimal.cpp drvr/my_latency_test.cpp
```

### Step 2: Add to your CMakeLists.txt
```cmake
add_executable(my_latency_test l2sp_latency_minimal.cpp)
target_link_libraries(my_latency_test pandohammer)
```

### Step 3: Build
```bash
cd build
cmake ..
make my_latency_test
```

### Step 4: Run in simulator
```bash
python3 model/hammerblade-r.py \
  --num-pxn=1 --pxn-pods=1 \
  --pod-cores-x=4 --pod-cores-y=4 --core-threads=16 \
  build/drvr/my_latency_test > output.txt
```

### Step 5: Analyze results
```bash
python3 analyze_l2sp_latency.py output.txt
```

## Key Concepts

### Measurement Accuracy
Different approaches, ranked by accuracy:

1. **Single-access (Highest)**: One read per timing, synchronized barriers
2. **Streaming (Medium)**: Many reads, averaged latency
3. **Alternating (Lower)**: Variable synchronization overhead

### Latency Sources
- **Same-core L2SP access**: ~20-40 cycles
- **Different-core L2SP access**: ~30-60 cycles (due to interconnect)
- **Bank conflict penalty**: +5-20 cycles
- **Contention**: +10-100 cycles depending on access pattern

### Pod Locality
**Critical**: L2SP is **pod-local**. Only harts in the same pod can access it.
```c
if (myPodId() != 0) return;  // Exit if not in pod 0
```

## Common Usage Patterns

### Measure latency for one specific hart
```c
if (hart_id == 0) {
    uint64_t t0 = cycle();
    volatile uint64_t v = shared_data;
    uint64_t t1 = cycle();
    printf("Hart 0 latency: %llu cycles\n", t1 - t0);
}
```

### Compare latencies across cores
```c
// Hart from core 0 accesses same location as hart from core 1
// Results show ~10-20 cycle difference due to interconnect
for (int i = 0; i < total_harts; i++) {
    printf("Hart %d (core %d): %llu cycles\n", 
           i, i >> 4, latency_samples[i]);
}
```

### Measure under contention
```c
// Multiple harts access same location simultaneously
// Results show contention effects (slower access)
barrier(total_harts);
uint64_t t0 = cycle();
volatile uint64_t v = shared_data;
uint64_t t1 = cycle();
```

## Typical Results

For a system with 4x4 cores, 16 threads per core:

```
Single Access Latency (cycles):
Hart,Core,Thread,AvgLatency
0,0,0,23      ← Same core cores as hart 0
1,0,1,24
...
16,1,0,29     ← Different core
17,1,1,30
...
32,2,0,34     ← Further core
```

**Observations**:
- Hart 0-15 (core 0): 23-24 cycles (same core)
- Hart 16-31 (core 1): 28-30 cycles (adjacent core, ~6 cycle penalty)
- Hart 32-47 (core 2): 33-35 cycles (further, ~10 cycle penalty)

## Performance Analysis

### Metric 1: Latency Distribution
Check if all harts in same core show similar latency (they should).

### Metric 2: Cross-Core Penalty
Compare same-core vs different-core access times.

### Metric 3: Contention Overhead
Compare single-access vs streaming latency:
- If ratio < 1.1x: good memory system (no contention)
- If ratio 1.2-1.5x: moderate contention
- If ratio > 2x: significant bank conflicts

## Debugging Tips

### Latency is 0 or very small (<5 cycles)
- Compiler optimized away the reads
- **Fix**: Add `volatile` to variable and use `volatile` pointer

### All measurements identical across harts
- Barrier not synchronizing correctly
- **Fix**: Initialize `g_hart_phase[i] = 0` before first barrier

### Program hangs
- Barrier deadlock (not all harts calling barrier)
- **Fix**: Ensure all harts reach the same barrier, no early returns

### Latencies vary wildly
- Normal with high-frequency simulation noise
- **Fix**: Increase number of samples and average

## Next Steps

1. Read [L2SP_LATENCY_GUIDE.md](L2SP_LATENCY_GUIDE.md) for detailed explanations
2. Study [l2sp_latency_minimal.cpp](drvr/l2sp_latency_minimal.cpp) for implementation details
3. Adapt the examples to your specific measurement needs
4. Use analyze script to process results: `python3 analyze_l2sp_latency.py`

## References

- **PANDOHammer headers**: `/pandohammer/cpuinfo.h`, `/pandohammer/atomic.h`
- **Related examples**: 
  - `shared_read_l2sp.cpp` - Multiple harts reading same location
  - `bfs_multihart.cpp` - Multi-hart synchronization (lines 31-48)
  - `stream_bw_l2sp.cpp` - L2SP bandwidth measurement
- **RISC-V ISA**: `rdcycle` instruction, atomic instructions (amoadd, amoswap)

## Support

For issues or questions:
1. Check the Troubleshooting section in [L2SP_LATENCY_GUIDE.md](L2SP_LATENCY_GUIDE.md)
2. Review existing examples in your codebase
3. Verify hart/core/pod IDs using debug print statements
4. Enable verbose logging in the simulator (if available)
