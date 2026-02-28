#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <pandohammer/mmio.h>
#include <pandohammer/cpuinfo.h>
#include <pandohammer/atomic.h>
#include <pandohammer/hartsleep.h>

#define __l2sp__ __attribute__((section(".l2sp")))

struct Args {
    int test_id = 3;
    int rounds = 0;         // 0 = auto (total_harts * 2), or specify manually
    int num_harts = 0;      // 0 = use system value, or override
};

static int parse_int(const char* s, int defv) {
    if (!s) return defv;
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    return (end && *end == 0) ? (int)v : defv;
}

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--test") && i + 1 < argc) {
            a.test_id = parse_int(argv[++i], a.test_id);
        } else if (!std::strcmp(argv[i], "--rounds") && i + 1 < argc) {
            a.rounds = parse_int(argv[++i], a.rounds);
        } else if (!std::strcmp(argv[i], "--harts") && i + 1 < argc) {
            a.num_harts = parse_int(argv[++i], a.num_harts);
        } else if (!std::strcmp(argv[i], "--help")) {
            std::printf("Usage: %s [--test <0|1|2|3>] [--rounds <N>] [--harts <N>]\n", argv[0]);
            std::printf("  --test 0    : Run all measurements (default)\n");
            std::printf("  --test 1    : Single access latency only\n");
            std::printf("  --test 2    : Streaming access only\n");
            std::printf("  --test 3    : Cross-core alternating access only\n");
            std::printf("  --rounds N  : Number of rounds for test 3 (default: total_harts * 2)\n");
            std::printf("  --harts N   : Override number of harts (default: auto-detect)\n");
            std::exit(0);
        }
    }
    return a;
}

// ============================================================================
// Barrier synchronization (simple sense-reversing barrier)
// ============================================================================
__l2sp__ volatile int64_t barrier_count = 0;
__l2sp__ volatile int64_t barrier_sense = 0;
__l2sp__ volatile int init_done = 0;  // Flag: hart 0 finished initialization
__l2sp__ volatile int total_harts_global = 0;

static inline int get_hart_id() {
    return (myCoreId() << 4) + myThreadId();
}

static inline void barrier(int num_harts) {
    // Each hart increments the counter atomically
    int64_t old_count = atomic_fetch_add_i64(&barrier_count, 1);
    
    if (old_count == num_harts - 1) {
        // Last hart to arrive: reset counter and flip sense using atomic swap
        atomic_swap_i64(&barrier_count, 0);
        // Flip sense atomically by swapping: 0->1 or 1->0
        int64_t old_sense = atomic_swap_i64(&barrier_sense, 1 - barrier_sense);
    } else {
        // All other harts: spin on sense until it flips
        int64_t my_sense = barrier_sense;
        while (barrier_sense == my_sense) {
            hartsleep(1);
        }
    }
}


// ============================================================================
// Shared data in L2SP
// ============================================================================
__l2sp__ volatile uint64_t shared_data = 0x0123456789ABCDEFULL;

// Results storage
__l2sp__ uint64_t latency_samples[1024];  // Latency for each hart (64 cores * 16 threads)

// ============================================================================
// Main measurement function
// ============================================================================
int main(int argc, char** argv) {
    // Parse command line arguments
    Args args = parse_args(argc, argv);
    
    // Get hart information
    int cid = myCoreId();      // Core ID within pod
    int hart_id = get_hart_id();
    int hpc = myCoreThreads(); // Harts per core
    int cpp = numPodCores();   // Cores per pod
    int system_total_harts = cpp * hpc;
    
    // Use override if provided, otherwise use system value
    int local_total_harts = (args.num_harts > 0) ? args.num_harts : system_total_harts;
    
    // Only run on pod 0, pxn 0 (L2SP is pod-local)
    if (myPXNId() != 0 || myPodId() != 0) return 0;
    
    // Immediate debug: print from hart 0 right away
    if (hart_id == 0) {
        std::printf("Hart 0 starting, local_total_harts=%d\n", local_total_harts);
        std::fflush(stdout);
        
        // Initialize barrier
        atomic_swap_i64(&barrier_count, 0);
        atomic_swap_i64(&barrier_sense, 0);
        init_done = 1;
        
        std::printf("\n=== L2SP LATENCY MEASUREMENT ===\n");
        std::printf("Configuration:\n");
        std::printf("  Harts per core: %d\n", hpc);
        std::printf("  Cores per pod: %d\n", cpp);
        std::printf("  Total harts (system): %d\n", system_total_harts);
        std::printf("  Total harts (using): %d\n", local_total_harts);
        std::fflush(stdout);
    }
    
    // Wait for hart 0 to initialize
    while (!init_done) {
        hartsleep(1);
    }
    
    // Initialize all latency samples to 0
    for (int i = hart_id; i < 1024; i += local_total_harts) {
        latency_samples[i] = 0;
    }
    
    // First barrier: all harts synchronize before measurements
    barrier(local_total_harts);
    
    // ========================================================================
    // MEASUREMENT 1: Single accesses (cleanest measurement)
    // ========================================================================
    if (args.test_id == 0 || args.test_id == 1) {
        if (hart_id == 0) {
            std::printf("\n--- Single Access Latency ---\n");
        }
        
        barrier(local_total_harts);
        
        // Perform 10 independent measurements and average
        uint64_t sum_latency = 0;
        const int num_samples = 10;
        
        for (int sample = 0; sample < num_samples; sample++) {
            barrier(local_total_harts);  // Synchronize all harts
            
            // START TIMING
            uint64_t t0 = cycle();
            volatile uint64_t value = shared_data;  // Read from L2SP
            uint64_t t1 = cycle();
            // END TIMING
            
            uint64_t latency = t1 - t0;
            sum_latency += latency;
            
            // Prevent compiler from optimizing away the read
            if (value == 0) std::printf("X");
        }
        
        uint64_t avg_latency = sum_latency / num_samples;
        latency_samples[hart_id] = avg_latency;
        
        barrier(local_total_harts);
        
        // Print results
        if (hart_id == 0) {
            std::printf("Core,Hart,Thread,AvgLatency(cycles)\n");
            for (int i = 0; i < local_total_harts; i++) {
                int core = i >> 4;
                int thread = i & 0xF;
                std::printf("%d,%d,%d,%llu\n", 
                    core, i, thread, (unsigned long long)latency_samples[i]);
            }
        }
    }
    
    // ========================================================================
    // MEASUREMENT 2: Streaming access (reveals contention)
    // ========================================================================
    if (args.test_id == 0 || args.test_id == 2) {
        if (hart_id == 0) {
            std::printf("\n--- Streaming Access (1000 reads) ---\n");
        }
        
        barrier(local_total_harts);
        
        uint64_t t0 = cycle();
        uint64_t accumulator = 0;
        for (int i = 0; i < 1000; i++) {
            accumulator += shared_data;  // Read same location repeatedly
        }
        uint64_t t1 = cycle();
        
        latency_samples[hart_id] = (t1 - t0) / 1000;  // Average per read
        
        // Prevent optimization
        if (accumulator == 0) std::printf("Y");
        
        barrier(local_total_harts);
        
        if (hart_id == 0) {
            std::printf("Core,Hart,AvgLatency(cycles),Total(cycles)\n");
            for (int i = 0; i < local_total_harts; i++) {
                int core = i >> 4;
                uint64_t avg = latency_samples[i];
                std::printf("%d,%d,%llu,%llu\n", 
                    core, i, (unsigned long long)avg, (unsigned long long)(avg * 1000));
            }
        }
    }
    
    // ========================================================================
    // MEASUREMENT 3: Cross-core access pattern
    // ========================================================================
    if (args.test_id == 0 || args.test_id == 3) {
        if (hart_id == 0) {
            std::printf("\n--- Cross-Core Alternating Access ---\n");
            std::printf("(Each hart accesses in turn)\n");
        }
        
        barrier(local_total_harts);
        
        // Calculate number of rounds: default is total_harts * 2 for 2 measurements per hart
        int num_rounds = (args.rounds > 0) ? args.rounds : (local_total_harts * 2);
        
        if (hart_id == 0) {
            std::printf("Running %d rounds to measure all %d harts\n", num_rounds, local_total_harts);
        }
        
        uint64_t sum_latency = 0;
        
        // Perform num_rounds; in round R, hart R%total_harts accesses
        for (int round = 0; round < num_rounds; round++) {
            barrier(local_total_harts);
            
            if (round % local_total_harts == hart_id) {
                uint64_t t0 = cycle();
                volatile uint64_t v = shared_data;
                uint64_t t1 = cycle();
                sum_latency += (t1 - t0);
                (void)v;
            }
        }
        
        // Store average (for harts that had accesses)
        int num_accesses = num_rounds / local_total_harts + (hart_id < (num_rounds % local_total_harts) ? 1 : 0);
        latency_samples[hart_id] = (num_accesses > 0) ? (sum_latency / num_accesses) : 0;
        
        barrier(local_total_harts);
        
        if (hart_id == 0) {
            std::printf("Core,Hart,AvgLatency(cycles)\n");
            for (int i = 0; i < local_total_harts; i++) {
                int core = i >> 4;
                uint64_t lat = latency_samples[i];
                std::printf("%d,%d,%llu\n", core, i, (unsigned long long)lat);
            }
            std::fflush(stdout);
        }
        
        // Barrier: all harts wait for printing to complete
        barrier(local_total_harts);
    }
    
    // ========================================================================
    // Summary
    // ========================================================================
    if (hart_id == 0) {
        std::printf("\n=== MEASUREMENT COMPLETE ===\n");
        if (args.test_id == 0) {
            std::printf("Ran all 3 measurements\n");
        } else {
            std::printf("Ran test %d\n", args.test_id);
        }
        std::printf("Total runtime: %llu cycles\n", (unsigned long long)cycle());
        std::fflush(stdout);
    }
    
    // Final barrier: keep all harts alive until output is complete
    barrier(local_total_harts);
    
    return 0;
}
