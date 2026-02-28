#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>

#include <pandohammer/mmio.h>
#include <pandohammer/cpuinfo.h>
#include <pandohammer/atomic.h>
#include <pandohammer/hartsleep.h>
#include <pandohammer/address.h>
#include <pandohammer/staticdecl.h>

static constexpr int QUEUE_SIZE = 4096;
static constexpr int64_t WORK_UNIT_ITERS = 10000;  // Match work_stealing_benchmark
static constexpr int MAX_HARTS = 1024;          // Maximum array size
static constexpr int MAX_CORES = 64;            // Maximum cores
static constexpr int MAX_HARTS_PER_CORE = 16;   // Maximum harts per core
static constexpr int HART_QUEUE_SIZE = QUEUE_SIZE / MAX_HARTS_PER_CORE; // Per-hart queue size
static constexpr int g_total_work = 1536;       // Total work units to distribute

// Runtime values (set by hart 0 during initialization)
__l2sp__ volatile int g_total_harts = 0;
__l2sp__ volatile int g_harts_per_core = 0;
__l2sp__ volatile int g_total_cores = 0;
__l2sp__ std::atomic<int> g_initialized = 0;    // Flag to signal initialization complete

// -------------------- Work Assignment --------------------
// Each hart gets its own work slice — no sharing between harts
struct HartWorkSlice {
    volatile int64_t work_count;
    volatile int64_t work_items[HART_QUEUE_SIZE];
};

struct CoreWorkAssignment {
    HartWorkSlice hart_slices[MAX_HARTS_PER_CORE];
};

// Static arrays in shared memory (L2SP — shared per core)
__l2sp__ CoreWorkAssignment core_assignments[MAX_CORES];  // Per-core, per-hart work
__l2sp__ int64_t g_local_sense[MAX_HARTS];                // Per-hart sense
__l2sp__ volatile int64_t stat_work_processed[MAX_HARTS];

__l2sp__ std::atomic<int64_t> g_count = 0;
__l2sp__ std::atomic<int64_t> g_sense = 0;

static inline int get_thread_id() {
    int tid = (myCoreId() << 4) + myThreadId();
    return tid;
}

static inline void barrier() {
    int tid = get_thread_id();
    
    // g_total_harts is now guaranteed to be initialized
    int total = g_total_harts;
    
    int64_t local = g_local_sense[tid];
    local ^= 1;
    g_local_sense[tid] = local;

    int64_t old = g_count.fetch_add(1, std::memory_order_acq_rel);

    if (old == total - 1) {
        // last hart: reset count for next round, then publish sense flip
        g_count.store(0, std::memory_order_relaxed);
        g_sense.store(local, std::memory_order_release);
    } else {
        long w = 1;
        long wmax = 64 * total;
        while (g_sense.load(std::memory_order_acquire) != local) {
            if (w < wmax) w <<= 1;
            hartsleep(w);
        }
    }
}

// -------------------- Synthetic Work Function --------------------
static inline void do_work(int64_t work_amount) {
    volatile int64_t sum = 0;
    for (int64_t i = 0; i < work_amount * WORK_UNIT_ITERS; i++) {
        sum += i;
    }
    sum = 5;
}

// -------------------- Static Work Processing --------------------
// Each hart processes its own private work slice — no contention
static void process_static_work(int tid) {
    std::atomic_thread_fence(std::memory_order_acquire);
    int harts_per_core = g_harts_per_core;
    
    int my_core = tid / harts_per_core;
    int my_hart_in_core = tid % harts_per_core;
    HartWorkSlice* my_work = &core_assignments[my_core].hart_slices[my_hart_in_core];
    int64_t local_processed = 0;
    
    // Each hart iterates over its own slice — no atomics needed
    for (int64_t i = 0; i < my_work->work_count; i++) {
        do_work(my_work->work_items[i]);
        local_processed++;
    }
    
    stat_work_processed[tid] = local_processed;
}

static void distribute_work_imbalanced(int tid, int total_work) {
    // Only hart 0 distributes work to cores and harts
    if (tid == 0) {
        std::printf("Hart 0 distributing work to %d cores (%d harts/core)...\n",
                   g_total_cores, g_harts_per_core);
        
        for (int c = 0; c < g_total_cores; c++) {
            int work_per_core = total_work / g_total_cores;
            int core_work = (c % 2 == 0) ? work_per_core : work_per_core * 2;
            
            // Divide core's work evenly among its harts
            int work_per_hart = core_work / g_harts_per_core;
            int remainder = core_work % g_harts_per_core;
            
            int assigned_total = 0;
            for (int h = 0; h < g_harts_per_core; h++) {
                int hart_work = work_per_hart + (h < remainder ? 1 : 0);
                if (hart_work > HART_QUEUE_SIZE) hart_work = HART_QUEUE_SIZE;
                
                core_assignments[c].hart_slices[h].work_count = hart_work;
                for (int i = 0; i < hart_work; i++) {
                    core_assignments[c].hart_slices[h].work_items[i] = 1;  // Unit work
                }
                assigned_total += hart_work;
            }
            
            std::printf("  Core %2d: %d items (%d per hart, %d remainder)\n",
                       c, assigned_total, work_per_hart, remainder);
        }
        std::printf("Distribution complete.\n");
    }
}

int main(int argc, char** argv) {
    const int hart_in_core = myThreadId();
    const int core_id = myCoreId();
    const int harts_per_core = myCoreThreads();
    const int total_cores = numPodCores();
    const int max_hw_harts = total_cores * harts_per_core;
    const int tid = get_thread_id();
    
    // Thread 0 initializes shared memory structures
    if (tid == 0) {
        // Set runtime configuration to match hardware
        g_total_cores = total_cores;
        g_harts_per_core = harts_per_core;
        g_total_harts = max_hw_harts;
        
        std::printf("=== Static (no stealing) Work Benchmark ==\n");
        std::printf("Hardware: %d cores x %d harts = %d total harts\n", 
                   total_cores, harts_per_core, max_hw_harts);
        std::printf("Using: %d cores x %d harts = %d total harts\n",
                   g_total_cores, g_harts_per_core, g_total_harts);
        std::printf("Total work units: %d\n", g_total_work);
        std::printf("\n");
        
        // Initialize arrays
        for (int i = 0; i < g_total_harts; i++) {
            g_local_sense[i] = 0;
            stat_work_processed[i] = 0;
        }
        
        // Initialize core assignments (per-hart slices)
        for (int i = 0; i < g_total_cores; i++) {
            for (int h = 0; h < MAX_HARTS_PER_CORE; h++) {
                core_assignments[i].hart_slices[h].work_count = 0;
            }
        }
        
        // Signal initialization complete with memory fence
        std::atomic_thread_fence(std::memory_order_release);
        g_initialized.store(1, std::memory_order_release);
    } else {
        // Other harts: wait for initialization to complete
        while (g_initialized.load(std::memory_order_acquire) == 0) {
            hartsleep(10);
        }
    }
    
    barrier();
    
    // Phase 1: Distribute work (imbalanced) to cores
    distribute_work_imbalanced(tid, g_total_work);
    
    barrier();
    
    // Print initial work distribution
    if (tid == 0) {
        std::printf("\nInitial work distribution (per hart):\n");
        for (int c = 0; c < g_total_cores; c++) {
            int64_t core_total = 0;
            for (int h = 0; h < g_harts_per_core; h++) {
                core_total += core_assignments[c].hart_slices[h].work_count;
            }
            std::printf("  Core %2d: %ld total items (", c, (long)core_total);
            for (int h = 0; h < g_harts_per_core; h++) {
                if (h > 0) std::printf(", ");
                std::printf("%ld", (long)core_assignments[c].hart_slices[h].work_count);
            }
            std::printf(")\n");
        }
        std::printf("\nStarting work processing...\n");
    }
    
    barrier();
    
    // Record start time
    uint64_t start_cycles = 0;
    uint64_t end_cycles = 0;
    if (tid == 0) {
        asm volatile("rdcycle %0" : "=r"(start_cycles));
    }
    
    // Phase 2: Static work execution (no stealing)
    process_static_work(tid);
    
    barrier();
    
    // Record end time
    if (tid == 0) {
        asm volatile("rdcycle %0" : "=r"(end_cycles));
    }

    barrier();

    // Phase 3: Report statistics
    if (tid == 0) {
        std::printf("\n=== Results ===\n");

        int64_t total_processed = 0;
        int64_t max_processed = 0;
        int64_t min_processed = INT64_MAX;

        std::printf("\nPer-hart statistics:\n");
        std::printf("Hart | Core | Processed\n");
        std::printf("-----|------|----------\n");
        
        for (int h = 0; h < g_total_harts; h++) {
            int64_t processed = stat_work_processed[h];
            int core = h / g_harts_per_core;
            
            std::printf("%4d | %4d | %9ld\n", h, core, (long)processed);
            
            total_processed += processed;
            
            if (processed > max_processed) max_processed = processed;
            if (processed < min_processed && processed > 0) min_processed = processed;
        }
        std::fflush(stdout);
        
        // Per-core summary
        std::printf("\nPer-core summary:\n");
        std::printf("Core | Total Processed\n");
        std::printf("-----|----------------\n");
        for (int c = 0; c < g_total_cores; c++) {
            int64_t core_total = 0;
            for (int h = c * g_harts_per_core; h < (c + 1) * g_harts_per_core; h++) {
                core_total += stat_work_processed[h];
            }
            std::printf("%4d | %14ld\n", c, (long)core_total);
        }
        
        std::printf("\nSummary:\n");
        std::printf("  Total work processed: %ld\n", (long)total_processed);
        std::printf("  Hart-level load balance:\n");
        std::printf("    Min processed:      %ld\n", (long)min_processed);
        std::printf("    Max processed:      %ld\n", (long)max_processed);
        if (min_processed > 0 && min_processed != INT64_MAX) {
            int64_t ratio_pct = (max_processed * 100) / min_processed;
            std::printf("    Imbalance ratio:    %ld%% (max/min)\n", (long)ratio_pct);
        }
        uint64_t elapsed = end_cycles - start_cycles;
        std::printf("  Cycles elapsed:       %lu\n", (unsigned long)elapsed);
        if (total_processed > 0) {
            std::printf("  Cycles per work unit: %lu\n", (unsigned long)(elapsed / total_processed));
        }
    }

    barrier();
    
    return 0;
}
