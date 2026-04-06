// Level-synchronous BFS with per-core work stealing (using shared work_stealing.h library)
//
// - Each level: frontier nodes distributed to per-core queues (imbalanced: odd cores get 2x)
// - Harts pop from local queue; if empty, one thief per core steals from others
// - Neighbors claimed via visited[v] = amoswap.d(&visited[v], 1)
// - Level termination uses a global remaining-work counter, not per-hart empty rounds
// - Circular queue indexing eliminates head/tail reset races
// - TTAS single-shot pop/steal reduces cache-line contention

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <atomic>

#include <pandohammer/mmio.h>
#include <pandohammer/cpuinfo.h>
#include <pandohammer/atomic.h>
#include <pandohammer/hartsleep.h>
#include <pandohammer/address.h>
#include <pandohammer/staticdecl.h>

#include "work_stealing.h"

static constexpr int QCAP = 8192;           // Queue capacity (power of 2)
static constexpr int MAX_HARTS  = 1024;
static constexpr int MAX_CORES  = 64;
static constexpr int BFS_CHUNK_SIZE = 64;   // Nodes per batch local pop
static constexpr int STEAL_K = 64;          // Nodes per batch steal
static constexpr int RECENT_SIZE = 4;       // Recently-failed ring buffer

static constexpr int ROWS = 1000;
static constexpr int COLS = 1000;
static constexpr int64_t N = int64_t(ROWS) * int64_t(COLS);

__l2sp__ volatile int g_total_harts = 0;
__l2sp__ volatile int g_harts_per_core = 0;
__l2sp__ volatile int g_total_cores = 0;
__l2sp__ std::atomic<int> g_initialized = 0;

// Per-core queues: current level and next level
__l2sp__ ws::WorkQueue<QCAP> core_queues[MAX_CORES];
__l2sp__ ws::WorkQueue<QCAP> next_level_queues[MAX_CORES];

// Barrier state
__l2sp__ ws::BarrierState<MAX_HARTS> g_barrier;

// Best-effort hint: 1 if queue likely non-empty, 0 if likely empty
__l2sp__ volatile int32_t core_has_work[MAX_CORES];

// Per-core steal token: only one hart per core may steal at a time
__l2sp__ std::atomic<int> core_thief[MAX_CORES];

// Global remaining-work counter for the current level
__l2sp__ std::atomic<int64_t> g_level_remaining = 0;

// -------------------- Graph Utilities --------------------
static inline int64_t id_of(int r, int c) { return int64_t(r) * COLS + c; }
static inline int row_of(int64_t id) { return int(id / COLS); }
static inline int col_of(int64_t id) { return int(id % COLS); }

// -------------------- BFS Shared State --------------------
__l2sp__ volatile int64_t visited[N];
__l2sp__ int32_t dist_arr[N];

__l2sp__ volatile int64_t stat_nodes_processed[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_attempts[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_success[MAX_HARTS];
__l2sp__ volatile int64_t discovered = 0;

static inline int get_thread_id() {
    return (myCoreId() << 4) + myThreadId();
}

static inline void barrier() {
    ws::barrier(&g_barrier, get_thread_id(), g_total_harts);
}

static inline bool claim_node(int64_t v) {
    const int64_t old = atomic_swap_i64(&visited[v], 1);
    return old == 0;
}

// Process a single BFS node: check 4 neighbors, claim unvisited, push to next level
static inline void process_single_node(int64_t u, int32_t level,
                                       ws::WorkQueue<QCAP>* my_next_queue, int my_core) {
    const int ur = row_of(u);
    const int uc = col_of(u);

    if (ur > 0) {
        const int64_t v = u - COLS;
        if (claim_node(v)) {
            dist_arr[v] = level + 1;
            ws::queue_push_atomic(my_next_queue, v);
            atomic_fetch_add_i64(&discovered, 1);
        }
    }
    if (ur + 1 < ROWS) {
        const int64_t v = u + COLS;
        if (claim_node(v)) {
            dist_arr[v] = level + 1;
            ws::queue_push_atomic(my_next_queue, v);
            atomic_fetch_add_i64(&discovered, 1);
        }
    }
    if (uc > 0) {
        const int64_t v = u - 1;
        if (claim_node(v)) {
            dist_arr[v] = level + 1;
            ws::queue_push_atomic(my_next_queue, v);
            atomic_fetch_add_i64(&discovered, 1);
        }
    }
    if (uc + 1 < COLS) {
        const int64_t v = u + 1;
        if (claim_node(v)) {
            dist_arr[v] = level + 1;
            ws::queue_push_atomic(my_next_queue, v);
            atomic_fetch_add_i64(&discovered, 1);
        }
    }
}

// -------------------- BFS Level Processing --------------------
static void process_bfs_level(int tid, int32_t level) {
    const int harts_per_core = g_harts_per_core;
    const int total_cores = g_total_cores;
    const int my_core = tid / harts_per_core;

    ws::WorkQueue<QCAP>* my_queue = &core_queues[my_core];
    ws::WorkQueue<QCAP>* my_next_queue = &next_level_queues[my_core];

    // Steal policy knobs
    const int STEAL_START = 4;      // Wait for N local misses before starting steal episodes
    const int STEAL_VICTIMS = 2;    // Probe this many victims per episode
    int64_t steal_backoff = 4;
    const int64_t steal_backoff_max = 512;

    uint32_t rng = ws::xorshift_seed(tid);
    int recently_tried[RECENT_SIZE];
    ws::clear_recent(recently_tried, RECENT_SIZE);
    int rt_idx = 0;

    int64_t local_processed = 0;
    int64_t local_steal_attempts = 0;
    int64_t local_steal_success = 0;

    int empty_streak = 0;

    int64_t chunk_buf[BFS_CHUNK_SIZE];
    int64_t stolen_buf[STEAL_K];

    int64_t local_backoff = 4;
    const int64_t local_backoff_max = 128;

    while (g_level_remaining.load(std::memory_order_acquire) > 0) {
        // Batch pop: grab up to BFS_CHUNK_SIZE nodes at once
        int64_t count = ws::queue_pop_chunk(my_queue, chunk_buf, BFS_CHUNK_SIZE);

        if (count > 0) {
            g_level_remaining.fetch_sub(count, std::memory_order_acq_rel);

            for (int64_t i = 0; i < count; i++) {
                local_processed++;
                process_single_node(chunk_buf[i], level, my_next_queue, my_core);
            }

            empty_streak = 0;
            steal_backoff = 4;
            local_backoff = 4;
        } else {
            // Eagerly mark own queue empty so stealers skip us
            core_has_work[my_core] = 0;

            // Try to become this core's thief (only one hart per core steals at a time)
            if (core_thief[my_core].exchange(1, std::memory_order_acquire) != 0) {
                // Another hart on this core is already stealing — back off harder
                hartsleep(local_backoff * 4);
                if (local_backoff < local_backoff_max) local_backoff <<= 1;
                continue;
            }

            // Won the steal token
            empty_streak++;

            // Short local backoff first — don't steal immediately
            if (empty_streak < STEAL_START) {
                core_thief[my_core].store(0, std::memory_order_release);
                hartsleep(local_backoff);
                if (local_backoff < local_backoff_max) local_backoff <<= 1;
                continue;
            }

            bool found = false;
            for (int k = 0; k < STEAL_VICTIMS; k++) {
                int victim = ws::pick_victim<RECENT_SIZE>(
                    &rng, my_core, total_cores, core_has_work, recently_tried);
                if (victim < 0) break;

                local_steal_attempts++;
                count = ws::queue_pop_chunk(&core_queues[victim], stolen_buf, STEAL_K);
                if (count > 0) {
                    local_steal_success++;
                    found = true;

                    // Thief keeps first item; pushes rest into local queue for sibling harts
                    for (int64_t i = 1; i < count; i++) {
                        ws::queue_push_atomic(my_queue, stolen_buf[i]);
                        core_has_work[my_core] = 1;
                    }

                    // Process thief's own item — subtract 1 from remaining
                    g_level_remaining.fetch_sub(1, std::memory_order_acq_rel);
                    local_processed++;
                    process_single_node(stolen_buf[0], level, my_next_queue, my_core);

                    empty_streak = 0;
                    steal_backoff = 4;
                    local_backoff = 4;
                    ws::clear_recent(recently_tried, RECENT_SIZE);
                    break;
                } else {
                    ws::record_recent(recently_tried, &rt_idx, RECENT_SIZE, victim);
                }
            }

            // Release steal token
            core_thief[my_core].store(0, std::memory_order_release);

            if (!found) {
                hartsleep(steal_backoff);
                if (steal_backoff < steal_backoff_max) steal_backoff <<= 1;
                continue;
            }
        }
    }

    stat_nodes_processed[tid] += local_processed;
    stat_steal_attempts[tid] += local_steal_attempts;
    stat_steal_success[tid] += local_steal_success;
}

// -------------------- Redistribution (imbalanced) --------------------
static void distribute_frontier_imbalanced(int tid) {
    if (tid != 0) return;

    int total_cores = g_total_cores;

    int64_t total_nodes = 0;
    for (int c = 0; c < total_cores; c++) {
        total_nodes += ws::queue_size(&next_level_queues[c]);
    }

    if (total_nodes == 0) {
        for (int c = 0; c < total_cores; c++) {
            ws::queue_init(&core_queues[c]);
            core_has_work[c] = 0;
        }
        return;
    }

    int weights[MAX_CORES];
    int64_t quotas[MAX_CORES];
    int sum_weights = 0;
    for (int c = 0; c < total_cores; c++) {
        weights[c] = (c % 2 == 0) ? 1 : 2;
        sum_weights += weights[c];
    }

    int64_t assigned = 0;
    for (int c = 0; c < total_cores; c++) {
        quotas[c] = (total_nodes * weights[c]) / sum_weights;
        assigned += quotas[c];
    }

    int64_t rem = total_nodes - assigned;
    int idx = 0;
    while (rem > 0) {
        quotas[idx % total_cores]++;
        rem--;
        idx++;
    }

    for (int c = 0; c < total_cores; c++) {
        ws::queue_init(&core_queues[c]);
        core_has_work[c] = 0;
    }

    int target_core = 0;
    while (target_core < total_cores && quotas[target_core] == 0) target_core++;

    for (int src_core = 0; src_core < total_cores; src_core++) {
        ws::WorkQueue<QCAP>* src = &next_level_queues[src_core];
        int64_t h = src->head;
        int64_t t = src->tail;

        for (int64_t i = h; i < t; i++) {
            int64_t node = ws::queue_item_at(src, i);

            if (target_core >= total_cores) {
                ws::queue_push(&core_queues[total_cores - 1], node);
                core_has_work[total_cores - 1] = 1;
                continue;
            }

            ws::queue_push(&core_queues[target_core], node);
            core_has_work[target_core] = 1;
            quotas[target_core]--;

            while (target_core < total_cores && quotas[target_core] == 0) target_core++;
        }

        ws::queue_init(src);
    }
}

// -------------------- Work Count --------------------
static int64_t count_total_work() {
    int64_t total = 0;
    for (int c = 0; c < g_total_cores; c++) {
        total += ws::queue_size(&core_queues[c]);
    }
    return total;
}

// -------------------- BFS Driver --------------------
static void bfs(int64_t source_id) {
    const int tid = get_thread_id();

    if (tid == 0) {
        for (int64_t i = 0; i < N; i++) {
            visited[i] = 0;
            dist_arr[i] = -1;
        }

        for (int i = 0; i < g_total_harts; i++) {
            stat_nodes_processed[i] = 0;
            stat_steal_attempts[i] = 0;
            stat_steal_success[i] = 0;
        }

        for (int c = 0; c < g_total_cores; c++) {
            ws::queue_init(&core_queues[c]);
            ws::queue_init(&next_level_queues[c]);
            core_has_work[c] = 0;
            core_thief[c].store(0, std::memory_order_relaxed);
        }

        visited[source_id] = 1;
        dist_arr[source_id] = 0;
        discovered = 1;

        ws::queue_push(&core_queues[0], source_id);
        core_has_work[0] = 1;

        std::printf("=== BFS with Work Stealing (FIFO + global remaining) ===\n");
        std::printf("Graph: %d x %d = %ld nodes\n", ROWS, COLS, (long)N);
        std::printf("Hardware: %d cores x %d harts = %d total harts\n",
                    g_total_cores, g_harts_per_core, g_total_harts);
        std::printf("Source: node %ld (r=%d, c=%d)\n",
                    (long)source_id, row_of(source_id), col_of(source_id));
        std::printf("\n");

        g_initialized.store(1, std::memory_order_release);
    } else {
        while (g_initialized.load(std::memory_order_acquire) == 0) {
            hartsleep(10);
        }
    }

    barrier();

    uint64_t start_cycles = 0;
    if (tid == 0) {
        asm volatile("rdcycle %0" : "=r"(start_cycles));
    }

    int32_t level = 0;

    while (true) {
        int64_t total_work = count_total_work();

        if (tid == 0) {
            g_level_remaining.store(total_work, std::memory_order_release);
        }

        barrier();

        if (total_work == 0) break;

        if (tid == 0) {
            std::printf("Level %d: total_work=%ld, discovered=%ld\n",
                        level, (long)total_work, (long)discovered);
            std::printf("  Distribution: ");
            for (int c = 0; c < g_total_cores; c++) {
                std::printf("C%d:%ld ", c, (long)ws::queue_size(&core_queues[c]));
            }
            std::printf("\n");
        }

        barrier();
        process_bfs_level(tid, level);
        barrier();
        distribute_frontier_imbalanced(tid);
        barrier();
        level++;
    }

    uint64_t end_cycles = 0;
    if (tid == 0) {
        asm volatile("rdcycle %0" : "=r"(end_cycles));
    }

    barrier();

    if (tid == 0) {
        std::printf("\n=== BFS Complete ===\n");
        std::printf("Levels traversed: %d\n", level);
        std::printf("Nodes discovered: %ld / %ld\n", (long)discovered, (long)N);

        const int64_t far = id_of(ROWS - 1, COLS - 1);
        std::printf("dist[(%d,%d)] = %d (expected %d)\n",
                    ROWS - 1, COLS - 1, dist_arr[far], (ROWS - 1) + (COLS - 1));

        int64_t total_processed = 0;
        int64_t total_attempts = 0;
        int64_t total_success = 0;

        std::printf("\nPer-hart statistics:\n");
        std::printf("Hart | Processed | Steal Attempts | Steals OK\n");
        std::printf("-----|-----------|----------------|----------\n");

        for (int h = 0; h < g_total_harts; h++) {
            int64_t processed = stat_nodes_processed[h];
            int64_t attempts  = stat_steal_attempts[h];
            int64_t success   = stat_steal_success[h];

            std::printf("%4d | %9ld | %14ld | %9ld\n",
                        h, (long)processed, (long)attempts, (long)success);

            total_processed += processed;
            total_attempts  += attempts;
            total_success   += success;
        }

        std::printf("\nSummary:\n");
        std::printf("  Total nodes processed: %ld\n", (long)total_processed);
        std::printf("  Total steal attempts:  %ld\n", (long)total_attempts);
        if (total_attempts > 0) {
            std::printf("Successful steals:     %ld (%ld%%)\n",
                        (long)total_success,
                        (long)(100 * total_success / total_attempts));
        }

        uint64_t elapsed = end_cycles - start_cycles;
        std::printf("Cycles elapsed:        %lu\n", (unsigned long)elapsed);
        if (total_processed > 0) {
            std::printf("Cycles per node:       %lu\n",
                        (unsigned long)(elapsed / total_processed));
        }
    }
}

int main(int argc, char** argv) {
    const int tid = get_thread_id();

    if (tid == 0) {
        g_total_cores = numPodCores();
        g_harts_per_core = myCoreThreads();
        g_total_harts = g_total_cores * g_harts_per_core;
        ws::barrier_init(&g_barrier, g_total_harts);
    }

    barrier();
    bfs(id_of(0, 0));
    barrier();

    return 0;
}
