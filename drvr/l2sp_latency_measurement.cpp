// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington
//
// L2SP Latency Measurement: Measure per-hart access latency to same L2SP location
// Shows latency differences when multiple harts/cores access the same location
//
// Compile: Include in your CMakeLists.txt or build system
// Run: Varies by simulator configuration (RISC-V hart count, core count, pod count)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <pandohammer/mmio.h>
#include <pandohammer/cpuinfo.h>
#include <pandohammer/atomic.h>
#include <pandohammer/hartsleep.h>

#define __l2sp__ __attribute__((section(".l2sp")))

// ============================================================================
// Barrier for Multi-Hart Synchronization (from bfs_multihart.cpp pattern)
// ============================================================================
__l2sp__ static volatile int64_t g_barrier_count = 0;
__l2sp__ static volatile int64_t g_barrier_phase = 0;
__l2sp__ static int64_t g_hart_phase[2048];  // Support up to 2048 threads

static inline void barrier_sync(int total_harts) {
    int hart_id = (myCoreId() << 4) + myThreadId();
    int64_t my_phase = g_hart_phase[hart_id];
    
    int64_t old = atomic_fetch_add_i64(&g_barrier_count, 1);
    
    if (old == total_harts - 1) {
        // Last hart resets and advances phase
        atomic_swap_i64(&g_barrier_count, 0);
        atomic_fetch_add_i64(&g_barrier_phase, 1);
    } else {
        // Wait for phase to advance
        long backoff = 1;
        long max_backoff = 8 * 1024;
        while (g_barrier_phase == my_phase) {
            if (backoff < max_backoff) backoff <<= 1;
            hartsleep(backoff);
        }
    }
    
    g_hart_phase[hart_id] = my_phase + 1;
}

// ============================================================================
// Shared L2SP Data: One location accessed by all harts
// ============================================================================
__l2sp__ volatile uint64_t g_shared_location = 0xDEADBEEFCAFEBABEULL;

// Per-hart latency measurements
__l2sp__ uint64_t g_latency[2048];  // latency in cycles for each hart
__l2sp__ uint64_t g_access_count[2048];  // number of accesses per hart

// ============================================================================
// APPROACH 1: Single-Access Latency (High Precision)
// Each hart performs ONE read and measures latency
// ============================================================================
static void measure_single_access_latency(int total_harts, int repeats) {
    int hart_id = (myCoreId() << 4) + myThreadId();
    const int hpc = myCoreThreads();
    const int cpp = numPodCores();
    
    // Only run on pod 0, pxn 0 (L2SP is pod-local)
    if (myPXNId() != 0 || myPodId() != 0) return;
    
    if (hart_id == 0) {
        std::printf("=== SINGLE-ACCESS LATENCY MEASUREMENT ===\n");
        std::printf("Total harts: %d, Repeats: %d\n", total_harts, repeats);
    }
    
    barrier_sync(total_harts);
    
    // Hart 0 initializes the shared location
    if (hart_id == 0) {
        g_shared_location = 0x1234567890ABCDEFULL;
    }
    
    barrier_sync(total_harts);
    
    // Each hart performs 'repeats' single accesses with timing
    uint64_t total_latency = 0;
    
    for (int r = 0; r < repeats; r++) {
        // Warm up (optional)
        volatile uint64_t tmp = g_shared_location;
        (void)tmp;
        
        barrier_sync(total_harts);  // All harts synchronized before access
        
        // TIME THE ACCESS
        uint64_t t0 = cycle();
        volatile uint64_t value = g_shared_location;
        uint64_t t1 = cycle();
        
        uint64_t access_latency = t1 - t0;
        total_latency += access_latency;
        (void)value;  // Prevent compiler from optimizing away the read
        
        barrier_sync(total_harts);  // Sync after measurement
    }
    
    // Store average latency for this hart
    g_latency[hart_id] = total_latency / repeats;
    g_access_count[hart_id] = repeats;
    
    barrier_sync(total_harts);
    
    // Hart 0 prints results
    if (hart_id == 0) {
        std::printf("\nPer-hart access latency (cycles):\n");
        std::printf("HartID | Core | Thread | AvgLatency\n");
        std::printf("-------|------|--------|----------\n");
        
        for (int i = 0; i < total_harts; i++) {
            int core_id = i >> 4;
            int thread_id = i & 0xF;
            std::printf("%6d | %4d | %6d | %10llu\n", 
                i, core_id, thread_id, (unsigned long long)g_latency[i]);
        }
    }
}

// ============================================================================
// APPROACH 2: Streaming Access Latency (Measures contention effect)
// Each hart continuously reads and measures throughput/latency under contention
// ============================================================================
static void measure_streaming_latency(int total_harts, int iterations) {
    int hart_id = (myCoreId() << 4) + myThreadId();
    
    // Only run on pod 0, pxn 0
    if (myPXNId() != 0 || myPodId() != 0) return;
    
    if (hart_id == 0) {
        std::printf("\n=== STREAMING ACCESS LATENCY MEASUREMENT ===\n");
        std::printf("Total harts: %d, Iterations: %d\n", total_harts, iterations);
    }
    
    barrier_sync(total_harts);
    
    // Hart 0 initializes
    if (hart_id == 0) {
        g_shared_location = 0x0123456789ABCDEFULL;
    }
    
    barrier_sync(total_harts);
    
    // Start measurement
    uint64_t t0 = cycle();
    
    uint64_t sink = 0;
    for (int i = 0; i < iterations; i++) {
        sink += g_shared_location;
    }
    
    uint64_t t1 = cycle();
    
    // Per-hart metrics
    g_latency[hart_id] = (t1 - t0) / iterations;  // Average latency per read
    g_access_count[hart_id] = iterations;
    
    // Prevent sink from being optimized away
    if (sink == 0xFFFFFFFFULL) std::printf("sink zero\n");
    
    barrier_sync(total_harts);
    
    if (hart_id == 0) {
        std::printf("\nStreaming latency per read (cycles):\n");
        std::printf("HartID | AvgLatency (cycles) | Total Access Time\n");
        std::printf("-------|---------------------|------------------\n");
        
        for (int i = 0; i < total_harts; i++) {
            uint64_t total_time = g_latency[i] * g_access_count[i];
            std::printf("%6d | %19llu | %18llu\n", 
                i, (unsigned long long)g_latency[i], (unsigned long long)total_time);
        }
    }
}

// ============================================================================
// APPROACH 3: Cross-Core Latency with Alternating Access Pattern
// Measures latency when cores take turns accessing the same location
// ============================================================================
static void measure_alternating_access_latency(int total_harts, int accesses_per_hart) {
    int hart_id = (myCoreId() << 4) + myThreadId();
    
    if (myPXNId() != 0 || myPodId() != 0) return;
    
    if (hart_id == 0) {
        std::printf("\n=== ALTERNATING ACCESS PATTERN ===\n");
        std::printf("Total harts: %d, Accesses per hart: %d\n", total_harts, accesses_per_hart);
    }
    
    barrier_sync(total_harts);
    
    if (hart_id == 0) {
        g_shared_location = 0xCCCCCCCCCCCCCCCCULL;
    }
    
    barrier_sync(total_harts);
    
    uint64_t total_latency = 0;
    uint64_t min_latency = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t max_latency = 0;
    
    // Each hart performs accesses, but ordered by hart_id to see cross-core effects
    for (int acc = 0; acc < accesses_per_hart; acc++) {
        barrier_sync(total_harts);
        
        // Hart hart_id performs an access when acc == hart_id % harts
        if (acc % total_harts == hart_id) {
            uint64_t t0 = cycle();
            volatile uint64_t v = g_shared_location;
            uint64_t t1 = cycle();
            
            uint64_t lat = t1 - t0;
            total_latency += lat;
            if (lat < min_latency) min_latency = lat;
            if (lat > max_latency) max_latency = lat;
            
            (void)v;
        }
    }
    
    g_latency[hart_id] = total_latency;
    
    barrier_sync(total_harts);
    
    if (hart_id == 0) {
        std::printf("\nAlternating access latency:\n");
        std::printf("HartID | Total (cyc) | Count\n");
        std::printf("-------|-------------|-------\n");
        
        for (int i = 0; i < total_harts; i++) {
            int num_accesses = (accesses_per_hart + total_harts - 1 - i) / total_harts;
            std::printf("%6d | %11llu | %5d\n", 
                i, (unsigned long long)g_latency[i], num_accesses);
        }
    }
}

// ============================================================================
// Main Entry Point
// ============================================================================
extern "C" int main(int argc, char** argv) {
    // Parse arguments
    int test_id = 1;  // Which test to run
    int repeats = 10;  // Number of repeats
    
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--test") && i + 1 < argc) {
            test_id = std::atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--repeats") && i + 1 < argc) {
            repeats = std::atoi(argv[++i]);
        }
    }
    
    // Initialize barrier phase
    int hart_id = (myCoreId() << 4) + myThreadId();
    const int hpc = myCoreThreads();
    const int cpp = numPodCores();
    const int total_harts = cpp * hpc;
    
    if (hart_id == 0) {
        std::printf("\n=== L2SP LATENCY MEASUREMENT ===\n");
        std::printf("Harts per core: %d, Cores per pod: %d, Total harts: %d\n", 
                    hpc, cpp, total_harts);
        std::printf("Clock Hz: %llu\n", (unsigned long long)myCoreThreads());
    }
    
    // Initialize hart phases (only once per hart)
    if (hart_id < 2048) {
        g_hart_phase[hart_id] = 0;
    }
    
    // Tag statistics for the simulator
    ph_stat_phase(1);
    
    // Run selected test
    switch (test_id) {
        case 1:
            measure_single_access_latency(total_harts, repeats);
            break;
        case 2:
            measure_streaming_latency(total_harts, repeats * 100);
            break;
        case 3:
            measure_alternating_access_latency(total_harts, repeats);
            break;
        default:
            if (hart_id == 0) {
                std::printf("Unknown test %d\n", test_id);
            }
            break;
    }
    
    ph_stat_phase(0);
    
    if (hart_id == 0) {
        std::printf("\n=== MEASUREMENT COMPLETE ===\n");
        std::printf("Global cycle count: %llu\n", (unsigned long long)cycle());
    }
    
    return 0;
}
