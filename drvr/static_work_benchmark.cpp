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

// Match no-steal BFS/Pagerank pattern: explicit CSR graph in DRAM.
static constexpr int ROWS = 100;
static constexpr int COLS = 1000;
static constexpr int32_t NUM_VERTICES = ROWS * COLS;
static constexpr int32_t MAX_EDGES = (4 * ROWS * COLS) - (2 * ROWS) - (2 * COLS);
static constexpr int32_t CHUNK_SIZE = 32; // vertices per queue item
static constexpr int32_t NUM_CHUNKS = (NUM_VERTICES + CHUNK_SIZE - 1) / CHUNK_SIZE;

// Pack a (begin, end) range into a single int64_t queue item
static inline int64_t pack_range(int32_t begin, int32_t end) {
    return ((int64_t)(uint32_t)begin << 32) | (int64_t)(uint32_t)end;
}
static inline int32_t range_begin(int64_t packed) { return (int32_t)(packed >> 32); }
static inline int32_t range_end(int64_t packed) { return (int32_t)(packed & 0xFFFFFFFF); }

// Runtime values (set by hart 0 during initialization)
__l2sp__ volatile int g_total_harts = 0;
__l2sp__ volatile int g_harts_per_core = 0;
__l2sp__ volatile int g_total_cores = 0;
__l2sp__ std::atomic<int> g_initialized = 0;    // Flag to signal initialization complete

// -------------------- Graph (CSR in DRAM) --------------------
__dram__ int32_t g_row_ptr[NUM_VERTICES + 1];
__dram__ int32_t g_col_idx[MAX_EDGES];
__dram__ int32_t g_degree[NUM_VERTICES];
__dram__ int32_t g_num_edges = 0;

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

// Per-core accumulator — cache-line padded so each core reads/writes its own L2SP line
struct alignas(64) CoreLocalSum {
    volatile int64_t value;
};
__l2sp__ CoreLocalSum g_core_sum[MAX_CORES];

static inline int get_thread_id() {
    int tid = myCoreId() * myCoreThreads() + myThreadId();
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

static void build_grid_csr_graph() {
    for (int32_t v = 0; v < NUM_VERTICES; v++) {
        g_degree[v] = 0;
    }

    for (int32_t r = 0; r < ROWS; r++) {
        for (int32_t c = 0; c < COLS; c++) {
            int32_t v = r * COLS + c;
            int32_t deg = 0;
            if (r > 0) deg++;
            if (r + 1 < ROWS) deg++;
            if (c > 0) deg++;
            if (c + 1 < COLS) deg++;
            g_degree[v] = deg;
        }
    }

    g_row_ptr[0] = 0;
    for (int32_t v = 0; v < NUM_VERTICES; v++) {
        g_row_ptr[v + 1] = g_row_ptr[v] + g_degree[v];
    }

    for (int32_t v = 0; v < NUM_VERTICES; v++) {
        g_degree[v] = g_row_ptr[v];
    }

    for (int32_t r = 0; r < ROWS; r++) {
        for (int32_t c = 0; c < COLS; c++) {
            int32_t v = r * COLS + c;
            int32_t pos = g_degree[v];
            if (r > 0) g_col_idx[pos++] = (r - 1) * COLS + c;
            if (r + 1 < ROWS) g_col_idx[pos++] = (r + 1) * COLS + c;
            if (c > 0) g_col_idx[pos++] = r * COLS + (c - 1);
            if (c + 1 < COLS) g_col_idx[pos++] = r * COLS + (c + 1);
            g_degree[v] = pos;
        }
    }

    g_num_edges = g_row_ptr[NUM_VERTICES];
    for (int32_t v = 0; v < NUM_VERTICES; v++) {
        g_degree[v] = g_row_ptr[v + 1] - g_row_ptr[v];
    }
}

// -------------------- Synthetic Work Function --------------------
// Process a range [v_begin, v_end) of graph vertices from CSR.
static inline void do_work_range(int32_t v_begin, int32_t v_end, int core_id) {
    volatile int64_t *sum = &g_core_sum[core_id].value;
    for (int32_t v = v_begin; v < v_end; v++) {
        int32_t start = g_row_ptr[v];
        int32_t end = g_row_ptr[v + 1];
        int64_t local = 0;
        for (int32_t e = start; e < end; e++) {
            local += (int64_t)g_col_idx[e];
        }
        for (int64_t i = 0; i < WORK_UNIT_ITERS; i++) {
            local += i;
        }
        *sum += (local & 0xFF);
    }
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
        int32_t rb = range_begin(my_work->work_items[i]);
        int32_t re = range_end(my_work->work_items[i]);
        do_work_range(rb, re, my_core);
        local_processed += (re - rb);
    }
    
    stat_work_processed[tid] = local_processed;
}

static void distribute_work_imbalanced(int tid) {
    // Only hart 0 distributes work to cores and harts
    if (tid == 0) {
        std::printf("Hart 0 distributing work to %d cores (%d harts/core)...\n",
                   g_total_cores, g_harts_per_core);
        
        // Match work_stealing_benchmark distribution: odd cores get 2x weight
        int odd_cores = g_total_cores / 2;
        int even_cores = g_total_cores - odd_cores;
        int base_chunks = NUM_CHUNKS / (even_cores + 2 * odd_cores);
        int leftover = NUM_CHUNKS - base_chunks * (even_cores + 2 * odd_cores);

        int32_t next_chunk = 0;

        for (int c = 0; c < g_total_cores; c++) {
            int core_chunks = (c % 2 == 0) ? base_chunks : base_chunks * 2;
            if (leftover > 0) { core_chunks++; leftover--; }
            
            // Divide this core's chunks evenly among harts
            int chunks_per_hart = core_chunks / g_harts_per_core;
            int remainder = core_chunks % g_harts_per_core;
            
            int assigned_chunks = 0;
            for (int h = 0; h < g_harts_per_core; h++) {
                int hart_chunks = chunks_per_hart + (h < remainder ? 1 : 0);
                int num_ranges = 0;
                for (int k = 0; k < hart_chunks && next_chunk < NUM_CHUNKS; k++, next_chunk++) {
                    int32_t begin = next_chunk * CHUNK_SIZE;
                    int32_t end = begin + CHUNK_SIZE;
                    if (end > NUM_VERTICES) end = NUM_VERTICES;
                    if (num_ranges >= HART_QUEUE_SIZE) break;
                    core_assignments[c].hart_slices[h].work_items[num_ranges] = pack_range(begin, end);
                    num_ranges++;
                }
                core_assignments[c].hart_slices[h].work_count = num_ranges;
                assigned_chunks += num_ranges;
            }
            
            std::printf("  Core %2d: %d chunk ranges (base=%d)\n",
                        c, assigned_chunks, base_chunks);
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
        std::printf("Graph: %d x %d = %d vertices, %d directed edges (CSR in DRAM)\n",
                    ROWS, COLS, NUM_VERTICES, MAX_EDGES);
        std::printf("Total chunk ranges: %d (chunk size: %d vertices)\n", NUM_CHUNKS, CHUNK_SIZE);
        std::printf("\n");
        
        // Initialize arrays
        for (int i = 0; i < g_total_harts; i++) {
            g_local_sense[i] = 0;
            stat_work_processed[i] = 0;
        }
        
        // Initialize per-core accumulators
        for (int i = 0; i < g_total_cores; i++) {
            g_core_sum[i].value = 0;
        }

        // Initialize core assignments (per-hart slices)
        for (int i = 0; i < g_total_cores; i++) {
            for (int h = 0; h < MAX_HARTS_PER_CORE; h++) {
                core_assignments[i].hart_slices[h].work_count = 0;
            }
        }

        build_grid_csr_graph();
        
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
    distribute_work_imbalanced(tid);
    
    barrier();
    
    // Print initial work distribution
    if (tid == 0) {
        std::printf("\nInitial work distribution (per hart, in chunks):\n");
        for (int c = 0; c < g_total_cores; c++) {
            int64_t core_total = 0;
            for (int h = 0; h < g_harts_per_core; h++) {
                core_total += core_assignments[c].hart_slices[h].work_count;
            }
            std::printf("  Core %2d: %ld total chunks (", c, (long)core_total);
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
        std::printf("  Graph edges:          %d\n", (int)g_num_edges);
        uint64_t elapsed = end_cycles - start_cycles;
        std::printf("  Cycles elapsed:       %lu\n", (unsigned long)elapsed);
        if (total_processed > 0) {
            std::printf("  Cycles per work unit: %lu\n", (unsigned long)(elapsed / total_processed));
        }
    }

    barrier();
    
    return 0;
}
