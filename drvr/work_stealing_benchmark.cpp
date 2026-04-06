// Per-Core Work Stealing Benchmark (using shared work_stealing.h library)
//
// - Imbalanced distribution: odd cores get 2x work
// - Harts pop from local core queue; if empty, one thief per core steals
// - Circular queue indexing eliminates head/tail reset races
// - TTAS single-shot pop/steal reduces cache-line contention

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

#include "work_stealing.h"

static constexpr int QCAP = 4096;                    // Queue capacity (power of 2)
static constexpr int64_t WORK_UNIT_ITERS = 10000;    // Iterations per work unit
static constexpr int MAX_HARTS = 1024;
static constexpr int MAX_CORES = 64;
static constexpr int g_total_work = 1536;             // Total work units to distribute
static constexpr int WORK_CHUNK_SIZE = 32;            // Work units per queue item (range)
static constexpr int STEAL_K = 4;                     // Max items to steal per episode
static constexpr int RECENT_SIZE = 4;                 // Recently-failed ring buffer size

// Runtime values (set by hart 0 during initialization)
__l2sp__ volatile int g_total_harts = 0;
__l2sp__ volatile int g_harts_per_core = 0;
__l2sp__ volatile int g_total_cores = 0;
__l2sp__ std::atomic<int> g_initialized = 0;

// Per-core queues and steal token
__l2sp__ ws::WorkQueue<QCAP> core_queues[MAX_CORES];
__l2sp__ std::atomic<int> core_thief[MAX_CORES];

// Barrier state
__l2sp__ ws::BarrierState<MAX_HARTS> g_barrier;

// Per-hart statistics
__l2sp__ volatile int64_t stat_work_processed[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_attempts[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_success[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_failures[MAX_HARTS];

// Per-core sum variables — cache-line padded so each core hits its own L2SP line
struct alignas(64) CoreLocalSum {
    volatile int64_t value;
};
__l2sp__ CoreLocalSum g_core_sum[MAX_CORES];

static inline int get_thread_id() {
    return (myCoreId() << 4) + myThreadId();
}

static inline void barrier() {
    ws::barrier(&g_barrier, get_thread_id(), g_total_harts);
}

// Process a range [begin, end) of work units; each unit does WORK_UNIT_ITERS iterations
static inline void do_work_range(int32_t begin, int32_t end, int core_id) {
    volatile int64_t *sum = &g_core_sum[core_id].value;
    for (int32_t w = begin; w < end; w++) {
        for (int64_t i = 0; i < WORK_UNIT_ITERS; i++) {
            *sum += i;
        }
    }
}

static void work_steal_loop(int tid, int total_harts) {
    std::atomic_thread_fence(std::memory_order_acquire);
    int harts_per_core = g_harts_per_core;
    int total_cores = g_total_cores;

    if (tid == 0) {
        std::printf("Hart 0 entering work_steal_loop, total_cores=%d, harts_per_core=%d\n",
                   total_cores, harts_per_core);
        std::fflush(stdout);
    }

    int my_core = tid / harts_per_core;
    ws::WorkQueue<QCAP>* my_queue = &core_queues[my_core];

    uint32_t rng = ws::xorshift_seed(tid);
    int recently_tried[RECENT_SIZE];
    ws::clear_recent(recently_tried, RECENT_SIZE);
    int rt_idx = 0;

    int64_t local_processed = 0;
    int64_t local_steal_attempts = 0;
    int64_t local_steal_success = 0;
    int64_t local_steal_failures = 0;

    int64_t stolen_buf[STEAL_K];

    int consecutive_empty = 0;
    const int MAX_EMPTY_ROUNDS = total_cores * 8 + 16;

    if (tid == 0) {
        std::printf("Hart 0: my_core=%d, MAX_EMPTY_ROUNDS=%d\n", my_core, MAX_EMPTY_ROUNDS);
        std::fflush(stdout);
    }

    int64_t backoff = 1;
    const int64_t max_backoff = 64;

    while (consecutive_empty < MAX_EMPTY_ROUNDS) {
        // Try to get work from own core's queue first
        int64_t packed = ws::queue_pop(my_queue);

        if (packed >= 0) {
            int32_t rb = ws::range_begin(packed);
            int32_t re = ws::range_end(packed);
            do_work_range(rb, re, my_core);
            local_processed += (re - rb);
            consecutive_empty = 0;
            backoff = 1;
            continue;
        }

        // Try to become this core's thief (only one hart per core steals at a time)
        if (core_thief[my_core].exchange(1, std::memory_order_acquire) == 0) {
            bool found_work = false;
            for (int rounds = 0; rounds < total_cores - 1; rounds++) {
                int victim = ws::pick_victim<RECENT_SIZE>(
                    &rng, my_core, total_cores, nullptr, recently_tried);
                if (victim < 0) break;

                local_steal_attempts++;
                int64_t count = ws::queue_pop_chunk(&core_queues[victim],
                                                    stolen_buf, STEAL_K);

                if (count > 0) {
                    local_steal_success++;
                    // Thief keeps first item; pushes rest into local queue for sibling harts
                    for (int64_t s = 1; s < count; s++) {
                        ws::queue_push_atomic(my_queue, stolen_buf[s]);
                    }
                    // Process only the thief's own item
                    {
                        int32_t rb = ws::range_begin(stolen_buf[0]);
                        int32_t re = ws::range_end(stolen_buf[0]);
                        do_work_range(rb, re, my_core);
                        local_processed += (re - rb);
                    }
                    found_work = true;
                    consecutive_empty = 0;
                    backoff = 1;
                    ws::clear_recent(recently_tried, RECENT_SIZE);
                    break;
                } else {
                    local_steal_failures++;
                    ws::record_recent(recently_tried, &rt_idx, RECENT_SIZE, victim);
                    hartsleep(backoff);
                }
            }

            // Release steal token
            core_thief[my_core].store(0, std::memory_order_release);

            if (!found_work) {
                consecutive_empty++;
                hartsleep(backoff);
                if (backoff < max_backoff) backoff <<= 1;
            }
        } else {
            // Another hart on this core is already stealing — back off harder
            consecutive_empty++;
            hartsleep(backoff * 4);
            if (backoff < max_backoff) backoff <<= 1;
        }
    }

    // Record statistics
    stat_work_processed[tid] = local_processed;
    stat_steal_attempts[tid] = local_steal_attempts;
    stat_steal_success[tid] = local_steal_success;
    stat_steal_failures[tid] = local_steal_failures;
}

static void distribute_work_imbalanced(int tid, int total_work) {
    if (tid != 0) return;

    std::printf("Hart 0 distributing work...\n");
    std::printf("  total_work=%d, g_total_cores=%d\n", total_work, g_total_cores);

    int odd_cores = g_total_cores / 2;
    int even_cores = g_total_cores - odd_cores;
    int base_work = total_work / (even_cores + 2 * odd_cores);

    for (int c = 0; c < g_total_cores; c++) {
        int my_work = (c % 2 == 0) ? base_work : base_work * 2;
        if (my_work > QCAP) my_work = QCAP;

        std::printf("  Core %2d: distributing %d items... ", c, my_work);
        std::fflush(stdout);

        int pushed_units = 0;
        int pushed_chunks = 0;
        for (int w = 0; w < my_work; w += WORK_CHUNK_SIZE) {
            int32_t end = (w + WORK_CHUNK_SIZE > my_work) ? my_work : w + WORK_CHUNK_SIZE;
            if (ws::queue_push(&core_queues[c], ws::pack_range((int32_t)w, (int32_t)end))) {
                pushed_units += (end - w);
                pushed_chunks++;
            } else {
                std::printf("PUSH FAILED at chunk %d! ", pushed_chunks);
                break;
            }
        }
        std::printf("pushed %d units in %d chunks, size=%ld\n",
                   pushed_units, pushed_chunks, (long)ws::queue_size(&core_queues[c]));
    }
    std::printf("Distribution complete.\n");
}

int main(int argc, char** argv) {
    const int tid = get_thread_id();

    if (tid == 0) {
        g_total_cores = numPodCores();
        g_harts_per_core = myCoreThreads();
        g_total_harts = g_total_cores * g_harts_per_core;

        std::printf("=== Per-Core Work Stealing Benchmark ==\n");
        std::printf("Hardware: %d cores x %d harts = %d total harts\n",
                   g_total_cores, g_harts_per_core, g_total_harts);
        std::printf("Using: %d cores x %d harts = %d total harts\n",
                   g_total_cores, g_harts_per_core, g_total_harts);
        std::printf("Total work units: %d (chunk size: %d)\n\n", g_total_work, WORK_CHUNK_SIZE);

        for (int i = 0; i < g_total_harts; i++) {
            stat_work_processed[i] = 0;
            stat_steal_attempts[i] = 0;
            stat_steal_success[i] = 0;
            stat_steal_failures[i] = 0;
        }

        for (int i = 0; i < g_total_cores; i++) {
            ws::queue_init(&core_queues[i]);
            g_core_sum[i].value = 0;
            core_thief[i].store(0, std::memory_order_relaxed);
        }

        ws::barrier_init(&g_barrier, g_total_harts);

        std::atomic_thread_fence(std::memory_order_release);
        g_initialized.store(1, std::memory_order_release);
    } else {
        while (g_initialized.load(std::memory_order_acquire) == 0) {
            hartsleep(10);
        }
    }

    barrier();

    distribute_work_imbalanced(tid, g_total_work);

    barrier();

    if (tid == 0) {
        std::printf("\nInitial work distribution:\n");
        for (int c = 0; c < g_total_cores; c++) {
            std::printf("  Core %2d: %ld chunks\n", c, (long)ws::queue_size(&core_queues[c]));
        }
        std::printf("\nStarting work stealing...\n");
    }

    barrier();

    if (tid == 0) {
        std::printf("Past barrier 3, about to start work...\n");
        std::fflush(stdout);
    }

    uint64_t start_cycles = 0;
    uint64_t end_cycles = 0;
    if (tid == 0) {
        asm volatile("rdcycle %0" : "=r"(start_cycles));
    }

    work_steal_loop(tid, g_total_harts);

    if (tid == 0) {
        std::printf("Work stealing complete!\n");
        std::fflush(stdout);
    }

    barrier();

    if (tid == 0) {
        asm volatile("rdcycle %0" : "=r"(end_cycles));
    }

    barrier();

    // Report statistics
    if (tid == 0) {
        std::printf("\n=== Results ===\n");

        int64_t total_processed = 0;
        int64_t total_steal_attempts = 0;
        int64_t total_steal_success = 0;
        int64_t total_steal_failures = 0;
        int64_t max_processed = 0;
        int64_t min_processed = 0x7FFFFFFFFFFFFFFFLL;

        std::printf("\nPer-hart statistics:\n");
        std::printf("Hart | Processed | Steal Attempts | Steals OK | Steals Fail\n");
        std::printf("-----|-----------|----------------|-----------|------------\n");

        for (int h = 0; h < g_total_harts; h++) {
            int64_t processed = stat_work_processed[h];
            int64_t attempts = stat_steal_attempts[h];
            int64_t success = stat_steal_success[h];
            int64_t failures = stat_steal_failures[h];

            std::printf("%4d | %9ld | %14ld | %9ld | %10ld\n",
                       h, (long)processed, (long)attempts, (long)success, (long)failures);

            total_processed += processed;
            total_steal_attempts += attempts;
            total_steal_success += success;
            total_steal_failures += failures;

            if (processed > max_processed) max_processed = processed;
            if (processed < min_processed) min_processed = processed;
        }
        std::fflush(stdout);

        std::printf("\nSummary:\n");
        std::fflush(stdout);
        std::printf("  Total work processed: %ld\n", (long)total_processed);
        std::printf("  Total steal attempts: %ld\n", (long)total_steal_attempts);
        if (total_steal_attempts > 0) {
            std::printf("  Successful steals:    %ld (%ld%%)\n",
                       (long)total_steal_success,
                       (long)(100 * total_steal_success / total_steal_attempts));
        } else {
            std::printf("  Successful steals:    %ld\n", (long)total_steal_success);
        }
        std::printf("  Failed steals:        %ld\n", (long)total_steal_failures);
        std::printf("  Load balance:\n");
        std::printf("    Min processed:      %ld\n", (long)min_processed);
        std::printf("    Max processed:      %ld\n", (long)max_processed);
        uint64_t elapsed = end_cycles - start_cycles;
        std::printf("  Cycles elapsed:       %lu\n", (unsigned long)elapsed);
        if (total_processed > 0) {
            std::printf("  Cycles per work unit: %lu\n", (unsigned long)(elapsed / total_processed));
        }
    }

    barrier();
    return 0;
}
