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

static constexpr int QUEUE_SIZE = 8192;
static constexpr int MAX_HARTS  = 1024;
static constexpr int MAX_CORES  = 64;

static constexpr int ROWS = 10;
static constexpr int COLS = 10;
static constexpr int64_t N = int64_t(ROWS) * int64_t(COLS);

// If set, any hart not on core 0 will park forever (prevents multi-core barrier effects).
static constexpr int FORCE_ONLY_CORE0_PARTICIPATES = 0;

static inline uint64_t rdcycle_u64() {
    uint64_t c;
    asm volatile("rdcycle %0" : "=r"(c));
    return c;
}

// -------------------- Runtime topology --------------------
int g_total_harts = 0;
int g_harts_per_core = 16;
int g_total_cores = 0;
__l2sp__ std::atomic<int> g_initialized = 0;



// -------------------- Work Queue --------------------
struct WorkQueue {
    volatile int64_t head;              // pop reservation (CAS)
    volatile int64_t tail;              // push reservation (CAS for concurrent pushes)
    volatile int64_t items[QUEUE_SIZE];
};

__l2sp__ WorkQueue core_queues[MAX_CORES];
__l2sp__ WorkQueue next_level_queues[MAX_CORES];

__l2sp__ int64_t g_local_sense[MAX_HARTS];

// best-effort hint: 1 if queue likely non-empty, 0 if likely empty
__l2sp__ volatile int32_t core_has_work[MAX_CORES];

// Global remaining-work counter for the current level
__l2sp__ std::atomic<int64_t> g_level_remaining = 0;

// -------------------- Barrier --------------------
__l2sp__ std::atomic<int64_t> g_count = 0;
__l2sp__ std::atomic<int64_t> g_sense = 0;

static inline int get_thread_id() {
    return (myCoreId() << 4) + myThreadId();
}

static inline void barrier() {
    int tid = get_thread_id();
    int total = g_total_harts;

    int64_t local = g_local_sense[tid];
    local ^= 1;
    g_local_sense[tid] = local;

    int64_t old = g_count.fetch_add(1, std::memory_order_acq_rel);

    if (old == total - 1) {
        g_count.store(0, std::memory_order_relaxed);
        g_sense.store(local, std::memory_order_release);
    } else {
        long w = 1;
        long wmax = 64 * total;
        while (true) {
            if (g_sense.load(std::memory_order_acquire) == local) break;
            if (w < wmax) w <<= 1;
            hartsleep(w);
        }
    }
}

// -------------------- Queue Operations --------------------
static inline void queue_init(WorkQueue* q) {
    q->head = 0;
    q->tail = 0;
}

// Single-thread push (tid==0 redistribution)
static inline bool queue_push(WorkQueue* q, int core_id, int64_t work) {
    int64_t t = q->tail;
    if (t >= QUEUE_SIZE) return false;

    q->items[t] = work;
    q->tail = t + 1;
    core_has_work[core_id] = 1;
    return true;
}

// Multi-producer push (BFS expansion into next_level_queues[my_core])
static inline bool queue_push_atomic(WorkQueue* q, int core_id, int64_t work) {
    while (true) {
        int64_t t = atomic_load_i64(&q->tail);
        if (t >= QUEUE_SIZE) return false;

        int64_t old_t = atomic_compare_and_swap_i64(&q->tail, t, t + 1);
        if (old_t == t) {
            q->items[t] = work;
            core_has_work[core_id] = 1;
            return true;
        }
        hartsleep(1);
    }
}

// Linearizable pop: reserve index by CAS on head (FIFO)
static inline int64_t queue_pop_fifo(WorkQueue* q, int core_id) {
    int64_t h = atomic_load_i64(&q->head);
    int64_t t = atomic_load_i64(&q->tail);

    if (h >= t) {
        core_has_work[core_id] = 0;
        return -1;
    }

    int64_t old_h = atomic_compare_and_swap_i64(&q->head, h, h + 1);
    if (old_h == h) {
        return q->items[h];
    }
    return -1;
}

// -------------------- Graph Utilities --------------------
static inline int64_t id_of(int r, int c) { return int64_t(r) * COLS + c; }
static inline int row_of(int64_t id) { return int(id / COLS); }
static inline int col_of(int64_t id) { return int(id % COLS); }

// -------------------- BFS Shared State --------------------
__l2sp__ volatile int64_t visited[N];
__l2sp__ int32_t dist_arr[N];

__l2sp__ volatile int64_t stat_nodes_processed[MAX_HARTS];
__l2sp__ volatile int64_t discovered = 0;
__l2sp__ volatile int64_t stat_empty_polls[MAX_HARTS];

static inline bool claim_node(int64_t v) {
    const int64_t old = atomic_swap_i64(&visited[v], 1);
    return old == 0;
}

// -------------------- BFS Level Processing (NO stealing) --------------------
static void process_bfs_level_no_steal(int tid, int32_t level) {
    const int harts_per_core = g_harts_per_core;
    const int my_core = tid / harts_per_core;

    WorkQueue* my_queue = &core_queues[my_core];
    WorkQueue* my_next_queue = &next_level_queues[my_core];

    int64_t local_processed = 0;
    int64_t local_empty = 0;

    int64_t local_backoff = 4;
    const int64_t local_backoff_max = 256;

    while (true) {
        if (g_level_remaining.load(std::memory_order_acquire) <= 0) break;

        int64_t u = queue_pop_fifo(my_queue, my_core);

        if (u < 0) {
            local_empty++;
            hartsleep(local_backoff);
            if (local_backoff < local_backoff_max) local_backoff <<= 1;
            continue;
        }

        g_level_remaining.fetch_sub(1, std::memory_order_acq_rel);

        local_backoff = 4;

        local_processed++;
        const int ur = row_of(u);
        const int uc = col_of(u);

        if (ur > 0) {
            const int64_t v = u - COLS;
            if (claim_node(v)) {
                dist_arr[v] = level + 1;
                queue_push_atomic(my_next_queue, my_core, v);
                atomic_fetch_add_i64(&discovered, 1);
            }
        }
        if (ur + 1 < ROWS) {
            const int64_t v = u + COLS;
            if (claim_node(v)) {
                dist_arr[v] = level + 1;
                queue_push_atomic(my_next_queue, my_core, v);
                atomic_fetch_add_i64(&discovered, 1);
            }
        }
        if (uc > 0) {
            const int64_t v = u - 1;
            if (claim_node(v)) {
                dist_arr[v] = level + 1;
                queue_push_atomic(my_next_queue, my_core, v);
                atomic_fetch_add_i64(&discovered, 1);
            }
        }
        if (uc + 1 < COLS) {
            const int64_t v = u + 1;
            if (claim_node(v)) {
                dist_arr[v] = level + 1;
                queue_push_atomic(my_next_queue, my_core, v);
                atomic_fetch_add_i64(&discovered, 1);
            }
        }
    }

    stat_nodes_processed[tid] += local_processed;
    stat_empty_polls[tid] += local_empty;
}

// -------------------- Redistribution (imbalanced) --------------------
static void distribute_frontier_imbalanced(int tid) {
    if (tid != 0) return;

    int total_cores = g_total_cores;

    int64_t total_nodes = 0;
    for (int c = 0; c < total_cores; c++) {
        int64_t count = next_level_queues[c].tail - next_level_queues[c].head;
        total_nodes += count;
    }

    if (total_nodes == 0) {
        for (int c = 0; c < total_cores; c++) {
            queue_init(&core_queues[c]);
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
        queue_init(&core_queues[c]);
        core_has_work[c] = 0;
    }

    int target_core = 0;
    while (target_core < total_cores && quotas[target_core] == 0) target_core++;

    for (int src_core = 0; src_core < total_cores; src_core++) {
        WorkQueue* src = &next_level_queues[src_core];

        int64_t h = src->head;
        int64_t t = src->tail;

        for (int64_t i = h; i < t; i++) {
            int64_t node = src->items[i];

            if (target_core >= total_cores) {
                queue_push(&core_queues[total_cores - 1], total_cores - 1, node);
                continue;
            }

            queue_push(&core_queues[target_core], target_core, node);
            quotas[target_core]--;

            while (target_core < total_cores && quotas[target_core] == 0) target_core++;
        }

        queue_init(src);
    }
}

// -------------------- Work Count --------------------
static int64_t count_total_work() {
    int64_t total = 0;
    for (int c = 0; c < g_total_cores; c++) {
        int64_t count = core_queues[c].tail - core_queues[c].head;
        total += count;
    }
    return total;
}

// -------------------- BFS Driver --------------------
static void bfs(int64_t source_id) {
    const int tid = get_thread_id();

    if (tid == 0) {
        // initialize graph state
        for (int64_t i = 0; i < N; i++) {
            visited[i] = 0;
            dist_arr[i] = -1;
        }

        for (int i = 0; i < g_total_harts; i++) {
            g_local_sense[i] = 0;
            stat_nodes_processed[i] = 0;
            stat_empty_polls[i] = 0;
        }

        for (int c = 0; c < g_total_cores; c++) {
            queue_init(&core_queues[c]);
            queue_init(&next_level_queues[c]);
            core_has_work[c] = 0;
        }

        visited[source_id] = 1;
        dist_arr[source_id] = 0;
        discovered = 1;

        queue_push(&core_queues[0], 0, source_id);

        std::printf("=== BFS Baseline (NO stealing; FIFO + global remaining) ===\n");
        std::printf("Graph: %d x %d = %ld nodes\n", ROWS, COLS, (long)N);
        std::printf("Hardware: %d cores x %d harts = %d total harts\n",
                    g_total_cores, g_harts_per_core, g_total_harts);
        std::printf("Source: node %ld (r=%d, c=%d)\n",
                    (long)source_id, row_of(source_id), col_of(source_id));
        std::printf("[L2SP] L2SP accesses tracked by SSTRISCVCore hw counters\n\n");

        g_initialized.store(1, std::memory_order_release);
    } else {
        while (g_initialized.load(std::memory_order_acquire) == 0) {
            hartsleep(10);
        }
    }

    barrier();

    uint64_t start_cycles = 0;
    if (tid == 0) start_cycles = rdcycle_u64();

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
        }

        barrier();
        // Track only BFS work; exclude barrier spin/wait traffic from stats.
        ph_stat_phase(1);
        process_bfs_level_no_steal(tid, level);
        ph_stat_phase(0);
        barrier();
        distribute_frontier_imbalanced(tid);
        barrier();
        level++;
    }

    uint64_t end_cycles = 0;
    if (tid == 0) end_cycles = rdcycle_u64();

    barrier();

    if (tid == 0) {
        std::printf("\n=== BFS Complete ===\n");
        std::printf("Levels traversed: %d\n", level);
        std::printf("Nodes discovered: %ld / %ld\n", (long)discovered, (long)N);

        const int64_t far = id_of(ROWS - 1, COLS - 1);
        std::printf("dist[(%d,%d)] = %d (expected %d)\n",
                    ROWS - 1, COLS - 1, dist_arr[far], (ROWS - 1) + (COLS - 1));

        int64_t total_processed = 0;

        std::printf("\nPer-hart statistics:\n");
        std::printf("Hart | Processed | Empty polls\n");
        std::printf("-----|-----------|------------\n");
        for (int h = 0; h < g_total_harts; h++) {
            int64_t processed = stat_nodes_processed[h];
            int64_t empties   = stat_empty_polls[h];
            std::printf("%4d | %9ld | %11ld\n",
                        h, (long)processed, (long)empties);
            total_processed += processed;
        }

        uint64_t elapsed = end_cycles - start_cycles;

        std::printf("\nSummary:\n");
        std::printf("  Total nodes processed: %ld\n", (long)total_processed);
        std::printf("  Cycles elapsed:        %lu\n", (unsigned long)elapsed);
        if (total_processed > 0) {
            std::printf("  Cycles per node:       %lu\n",
                        (unsigned long)(elapsed / (uint64_t)total_processed));
        }
        std::printf("  (L2SP load/store/atomic stats from SSTRISCVCore hw counters)\n");
    }
}

int main(int argc, char** argv) {
    const int tid = get_thread_id();

    if (FORCE_ONLY_CORE0_PARTICIPATES) {
        if (myCoreId() != 0) {
            while (1) { hartsleep(1 << 20); }
        }
    }

    if (tid == 0) {
        g_total_cores = numPodCores();
        g_harts_per_core = myCoreThreads();
        g_total_harts = g_total_cores * g_harts_per_core;

        if (g_total_cores > MAX_CORES) {
            std::printf("ERROR: g_total_cores=%d > MAX_CORES=%d\n", g_total_cores, MAX_CORES);
            std::abort();
        }
        if (g_total_harts > MAX_HARTS) {
            std::printf("ERROR: g_total_harts=%d > MAX_HARTS=%d\n", g_total_harts, MAX_HARTS);
            std::abort();
        }

        // If you truly want forced 1-core behavior (only affects program-level loops):
        // g_total_cores = 1;
        // g_total_harts = g_harts_per_core;
    }

    barrier();
    bfs(id_of(0, 0));
    barrier();

    return 0;
}
