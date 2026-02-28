// Level-synchronous BFS baseline (NO work stealing), per-core queues, imbalanced redistribution
// - Each level: frontier nodes reside in per-core queues (imbalanced: odd cores get 2x nodes vs even cores)
// - Each hart pops ONLY from its core queue (no stealing)
// - Neighbors claimed via visited[v] = amoswap.d(&visited[v], 1)
// - Next level collected into next_level_queues[core] via atomic tail reservation
//
// Goals:
//  - Remove all work-stealing congestion/atomics
//  - Avoid deadlocks: publish runtime config before any barrier
//  - Use dense tid indexing (no "core<<4" assumption)

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
static constexpr int MAX_HARTS  = 2048;   // increase if you run >2048
static constexpr int MAX_CORES  = 128;    // increase if you run >128

static constexpr int ROWS = 1000;
static constexpr int COLS = 200;
static constexpr int64_t N = int64_t(ROWS) * int64_t(COLS);

__l2sp__ volatile int g_total_harts = 0;
__l2sp__ volatile int g_harts_per_core = 0;
__l2sp__ volatile int g_total_cores = 0;
__l2sp__ std::atomic<int> g_cfg_ready = 0;

struct WorkQueue {
    volatile int64_t head;              // consumer reservation (CAS on head)
    volatile int64_t tail;              // producer reservation (CAS on tail for concurrent pushes)
    volatile int64_t items[QUEUE_SIZE];
};

__l2sp__ WorkQueue core_queues[MAX_CORES];
__l2sp__ WorkQueue next_level_queues[MAX_CORES];

__l2sp__ volatile int64_t visited[N];
__l2sp__ int32_t dist_arr[N];

__l2sp__ volatile int64_t nodes_processed[MAX_HARTS];
__l2sp__ volatile int64_t discovered = 0;

// -------------------- Barrier --------------------
__l2sp__ int64_t g_local_sense[MAX_HARTS];
__l2sp__ std::atomic<int64_t> g_count = 0;
__l2sp__ std::atomic<int64_t> g_sense = 0;

// Broadcast slot for per-level total_work (written by tid0, read by all)
__l2sp__ volatile int64_t g_total_work_bcast = 0;

static inline int core_id() { return myCoreId(); }
static inline int lane_id() { return myThreadId(); }

// Dense tid in [0, g_total_harts)
static inline int get_thread_id() {
    return core_id() * (int)g_harts_per_core + lane_id();
}

static inline void barrier() {
    const int total = g_total_harts;
    const int tid = get_thread_id();

    int64_t local = g_local_sense[tid] ^ 1;
    g_local_sense[tid] = local;

    int64_t old = g_count.fetch_add(1, std::memory_order_acq_rel);
    if (old == total - 1) {
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

// -------------------- Queue ops --------------------
static inline void queue_init(WorkQueue* q) {
    q->head = 0;
    q->tail = 0;
}

// Single-thread push (used only by tid0 during redistribution)
static inline bool queue_push_st(WorkQueue* q, int64_t work) {
    int64_t t = q->tail;
    if (t >= QUEUE_SIZE) return false;
    q->items[t] = work;
    q->tail = t + 1;
    return true;
}

// Multi-producer push (all harts in same core push into next_level_queues[my_core])
static inline bool queue_push_mp(WorkQueue* q, int64_t work) {
    while (true) {
        int64_t t = atomic_load_i64(&q->tail);
        if (t >= QUEUE_SIZE) return false;

        int64_t old_t = atomic_compare_and_swap_i64(&q->tail, t, t + 1);
        if (old_t == t) {
            q->items[t] = work;
            return true;
        }
        hartsleep(1);
    }
}

// Multi-consumer pop (harts in same core) via CAS on head; FIFO order.
static inline int64_t queue_pop_mc(WorkQueue* q) {
    int64_t backoff = 1;
    const int64_t max_backoff = 32;

    while (true) {
        int64_t h = atomic_load_i64(&q->head);
        int64_t t = atomic_load_i64(&q->tail);
        if (h >= t) return -1;

        int64_t old_h = atomic_compare_and_swap_i64(&q->head, h, h + 1);
        if (old_h == h) {
            return q->items[h];
        }

        hartsleep(backoff);
        if (backoff < max_backoff) backoff <<= 1;
    }
}

// -------------------- Graph utils --------------------
static inline int64_t id_of(int r, int c) { return int64_t(r) * COLS + c; }
static inline int row_of(int64_t id) { return int(id / COLS); }
static inline int col_of(int64_t id) { return int(id % COLS); }

static inline bool claim_node(int64_t v) {
    const int64_t old = atomic_swap_i64(&visited[v], 1);
    return old == 0;
}

// -------------------- Level processing (NO stealing) --------------------
static void process_bfs_level(int tid, int32_t level) {
    const int my_core = tid / g_harts_per_core;
    WorkQueue* my_q = &core_queues[my_core];
    WorkQueue* my_next = &next_level_queues[my_core];

    int64_t local_processed = 0;

    while (true) {
        int64_t u = queue_pop_mc(my_q);
        if (u < 0) break;

        local_processed++;

        const int ur = row_of(u);
        const int uc = col_of(u);

        if (ur > 0) {
            const int64_t v = u - COLS;
            if (claim_node(v)) {
                dist_arr[v] = level + 1;
                queue_push_mp(my_next, v);
                atomic_fetch_add_i64(&discovered, 1);
            }
        }
        if (ur + 1 < ROWS) {
            const int64_t v = u + COLS;
            if (claim_node(v)) {
                dist_arr[v] = level + 1;
                queue_push_mp(my_next, v);
                atomic_fetch_add_i64(&discovered, 1);
            }
        }
        if (uc > 0) {
            const int64_t v = u - 1;
            if (claim_node(v)) {
                dist_arr[v] = level + 1;
                queue_push_mp(my_next, v);
                atomic_fetch_add_i64(&discovered, 1);
            }
        }
        if (uc + 1 < COLS) {
            const int64_t v = u + 1;
            if (claim_node(v)) {
                dist_arr[v] = level + 1;
                queue_push_mp(my_next, v);
                atomic_fetch_add_i64(&discovered, 1);
            }
        }
    }

    nodes_processed[tid] += local_processed;
}

// -------------------- Redistribution (imbalanced) --------------------
// Moves all nodes from next_level_queues[*] into core_queues[*] using weights: even=1, odd=2
static void distribute_frontier_imbalanced(int tid) {
    if (tid != 0) return;

    const int total_cores = g_total_cores;

    int64_t total_nodes = 0;
    for (int c = 0; c < total_cores; c++) {
        total_nodes += (next_level_queues[c].tail - next_level_queues[c].head);
    }

    // Reset destination
    for (int c = 0; c < total_cores; c++) queue_init(&core_queues[c]);

    if (total_nodes == 0) {
        // nothing to distribute
        for (int c = 0; c < total_cores; c++) queue_init(&next_level_queues[c]);
        return;
    }

    int weights[MAX_CORES];
    int64_t quotas[MAX_CORES];
    int sum_w = 0;
    for (int c = 0; c < total_cores; c++) {
        weights[c] = (c & 1) ? 2 : 1;
        sum_w += weights[c];
    }

    int64_t assigned = 0;
    for (int c = 0; c < total_cores; c++) {
        quotas[c] = (total_nodes * (int64_t)weights[c]) / (int64_t)sum_w;
        assigned += quotas[c];
    }

    // remainder
    int64_t rem = total_nodes - assigned;
    int idx = 0;
    while (rem > 0) {
        quotas[idx % total_cores]++;
        rem--;
        idx++;
    }

    int target = 0;
    while (target < total_cores && quotas[target] == 0) target++;

    for (int src_core = 0; src_core < total_cores; src_core++) {
        WorkQueue* src = &next_level_queues[src_core];
        const int64_t h = src->head;
        const int64_t t = src->tail;

        for (int64_t i = h; i < t; i++) {
            const int64_t node = src->items[i];

            if (target >= total_cores) {
                queue_push_st(&core_queues[total_cores - 1], node);
                continue;
            }

            queue_push_st(&core_queues[target], node);
            quotas[target]--;
            while (target < total_cores && quotas[target] == 0) target++;
        }

        queue_init(src);
    }
}

static int64_t count_total_work() {
    int64_t total = 0;
    for (int c = 0; c < g_total_cores; c++) {
        total += (core_queues[c].tail - core_queues[c].head);
    }
    return total;
}

// -------------------- BFS driver --------------------
static void bfs(int64_t source_id) {
    const int tid = get_thread_id();

    if (tid == 0) {
        for (int64_t i = 0; i < N; i++) {
            visited[i] = 0;
            dist_arr[i] = -1;
        }

        for (int i = 0; i < g_total_harts; i++) {
            g_local_sense[i] = 0;
            nodes_processed[i] = 0;
        }

        for (int c = 0; c < g_total_cores; c++) {
            queue_init(&core_queues[c]);
            queue_init(&next_level_queues[c]);
        }

        visited[source_id] = 1;
        dist_arr[source_id] = 0;
        discovered = 1;

        queue_push_st(&core_queues[0], source_id);

        std::printf("=== BFS Baseline (NO stealing; imbalanced redistribution) ===\n");
        std::printf("Graph: %d x %d = %ld nodes\n", ROWS, COLS, (long)N);
        std::printf("Hardware: %d cores x %d harts = %d total harts\n",
                    g_total_cores, g_harts_per_core, g_total_harts);
        std::printf("Source: node %ld (r=%d, c=%d)\n",
                    (long)source_id, row_of(source_id), col_of(source_id));
        std::printf("\n");
    }

    barrier();

    uint64_t start_cycles = 0;
    if (tid == 0) asm volatile("rdcycle %0" : "=r"(start_cycles));

    int32_t level = 0;

    while (true) {
        if (tid == 0) {
            g_total_work_bcast = count_total_work();
            std::printf("Level %d: frontier=%ld discovered=%ld\n",
                        level, (long)g_total_work_bcast, (long)discovered);
        }
        barrier();

        const int64_t total_work = g_total_work_bcast;
        if (total_work == 0) break;

        barrier();
        process_bfs_level(tid, level);
        barrier();
        distribute_frontier_imbalanced(tid);
        barrier();

        level++;
    }

    uint64_t end_cycles = 0;
    if (tid == 0) asm volatile("rdcycle %0" : "=r"(end_cycles));

    barrier();

    if (tid == 0) {
        std::printf("\n=== BFS Complete ===\n");
        std::printf("Levels traversed: %d\n", level);
        std::printf("Nodes discovered: %ld / %ld\n", (long)discovered, (long)N);

        const int64_t far = id_of(ROWS - 1, COLS - 1);
        std::printf("dist[(%d,%d)] = %d (expected %d)\n",
                    ROWS - 1, COLS - 1, dist_arr[far], (ROWS - 1) + (COLS - 1));

        int64_t total_processed = 0;
        for (int h = 0; h < g_total_harts; h++) total_processed += nodes_processed[h];

        std::printf("Total nodes processed: %ld\n", (long)total_processed);

        const uint64_t elapsed = end_cycles - start_cycles;
        std::printf("Cycles elapsed:        %lu\n", (unsigned long)elapsed);
        if (total_processed > 0) {
            std::printf("Cycles per node:       %lu\n",
                        (unsigned long)(elapsed / (uint64_t)total_processed));
        }
    }
}

int main(int argc, char** argv) {
    // Publish runtime config BEFORE any barrier, and wait on a flag (prevents deadlock)
    const int core = core_id();
    const int lane = lane_id();

    if (core == 0 && lane == 0) {
        g_total_cores = numPodCores();
        g_harts_per_core = myCoreThreads();
        g_total_harts = g_total_cores * g_harts_per_core;

        // Optional safety checks (can remove if you want)
        if (g_total_cores > MAX_CORES) {
            std::printf("ERROR: g_total_cores=%d > MAX_CORES=%d\n", g_total_cores, MAX_CORES);
            std::abort();
        }
        if (g_total_harts > MAX_HARTS) {
            std::printf("ERROR: g_total_harts=%d > MAX_HARTS=%d\n", g_total_harts, MAX_HARTS);
            std::abort();
        }

        g_cfg_ready.store(1, std::memory_order_release);
    } else {
        while (g_cfg_ready.load(std::memory_order_acquire) == 0) {
            hartsleep(10);
        }
    }

    bfs(id_of(0, 0));
    return 0;
}