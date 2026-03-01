// PageRank Baseline — no work stealing
//
// Graph: deterministic 2D grid (GRID_ROWS x GRID_COLS) + random long-range edges
// Work unit: a chunk of CHUNK_SIZE consecutive vertices
// Distribution: odd-numbered cores get 2x chunks (imbalanced)
// Each core's harts only process their own queue — no stealing.

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

// ======================= Configuration =======================
static constexpr int GRID_ROWS       = 64;
static constexpr int GRID_COLS       = 64;
static constexpr int NUM_VERTICES    = GRID_ROWS * GRID_COLS;   // 4096
static constexpr int MAX_EDGES       = NUM_VERTICES * 5;        // grid + random
static constexpr int CHUNK_SIZE      = 64;                      // vertices per work unit
static constexpr int NUM_CHUNKS      = (NUM_VERTICES + CHUNK_SIZE - 1) / CHUNK_SIZE;
static constexpr int PR_ITERATIONS   = 10;
static constexpr int VERTEX_WORK_ITERS = 200; // synthetic per-vertex compute

static constexpr int PR_QUEUE_SIZE   = 512;
static constexpr int MAX_HARTS       = 1024;
static constexpr int MAX_CORES       = 64;

// Fixed-point PageRank (rank 1.0 == RANK_SCALE)
static constexpr int64_t RANK_SCALE  = 1000000LL;
static constexpr int64_t DAMPING_NUM = 85;   // d = 0.85
static constexpr int64_t DAMPING_DEN = 100;

// Pack a (begin, end) vertex range into a single int64_t queue item
static inline int64_t pack_range(int32_t begin, int32_t end) {
    return ((int64_t)(uint32_t)begin << 32) | (int64_t)(uint32_t)end;
}
static inline int32_t range_begin(int64_t packed) { return (int32_t)(packed >> 32); }
static inline int32_t range_end(int64_t packed) { return (int32_t)(packed & 0xFFFFFFFF); }

// ======================= Runtime Config =======================
__l2sp__ volatile int g_total_harts    = 0;
__l2sp__ volatile int g_harts_per_core = 0;
__l2sp__ volatile int g_total_cores    = 0;
__l2sp__ std::atomic<int> g_initialized = 0;

// ======================= Graph (CSR) =======================
__l2sp__ int32_t g_row_ptr[NUM_VERTICES + 1];
__l2sp__ int32_t g_col_idx[MAX_EDGES];
__l2sp__ int32_t g_degree[NUM_VERTICES];
__l2sp__ int32_t g_num_edges = 0;

// ======================= PageRank Data =======================
__l2sp__ volatile int64_t g_rank_old[NUM_VERTICES];
__l2sp__ volatile int64_t g_rank_new[NUM_VERTICES];

// ======================= Work Queues =======================
struct PRWorkQueue {
    volatile int64_t head;
    volatile int64_t tail;
    volatile int64_t items[PR_QUEUE_SIZE];
};

__l2sp__ PRWorkQueue core_queues[MAX_CORES];

// ======================= Synchronization =======================
__l2sp__ int64_t           g_local_sense[MAX_HARTS];
__l2sp__ std::atomic<int64_t> g_count = 0;
__l2sp__ std::atomic<int64_t> g_sense = 0;

// ======================= Statistics =======================
__l2sp__ volatile int64_t stat_chunks_processed[MAX_HARTS];

// ======================== Utility ========================

static inline int get_thread_id() {
    return (myCoreId() << 4) + myThreadId();
}

static inline void barrier() {
    int tid   = get_thread_id();
    int total = g_total_harts;

    int64_t local = g_local_sense[tid];
    local ^= 1;
    g_local_sense[tid] = local;

    int64_t old = g_count.fetch_add(1, std::memory_order_acq_rel);
    if (old == total - 1) {
        g_count.store(0, std::memory_order_relaxed);
        g_sense.store(local, std::memory_order_release);
    } else {
        long w    = 1;
        long wmax = 64 * total;
        while (g_sense.load(std::memory_order_acquire) != local) {
            if (w < wmax) w <<= 1;
            hartsleep(w);
        }
    }
}

// ============= Queue Ops =============

static inline void queue_init(PRWorkQueue* q) {
    q->head = 0;
    q->tail = 0;
}

// Single-thread push (hart 0 only, during distribution)
static inline bool queue_push(PRWorkQueue* q, int64_t item) {
    int64_t t = q->tail;
    if (t >= PR_QUEUE_SIZE) return false;
    q->items[t] = item;
    q->tail = t + 1;
    return true;
}

// TTAS pop – local harts pop from tail (LIFO for locality).
static inline int64_t queue_pop_ttas(PRWorkQueue* q) {
    // ---- TEST phase (cheap volatile reads) ----
    int64_t t = q->tail;
    if (t == 0) return -1;
    int64_t h = q->head;
    if (h >= t) return -1;

    // ---- TEST-AND-SET phase (CAS on tail) ----
    int64_t new_t = t - 1;
    int64_t old_t = atomic_compare_and_swap_i64(&q->tail, t, new_t);
    if (old_t != t) return -1;   // contention → bail immediately

    // Re-read head after winning the tail CAS
    h = atomic_load_i64(&q->head);
    if (h <= new_t) {
        return q->items[new_t];  // got the item cleanly
    }

    // Race for the very last item
    int64_t old_head = atomic_compare_and_swap_i64(&q->head, h, h + 1);
    if (old_head == h && h == new_t) {
        atomic_compare_and_swap_i64(&q->tail, new_t, 0);
        atomic_compare_and_swap_i64(&q->head, h + 1, 0);
        return q->items[new_t];
    }

    // Lost race – clean up
    atomic_compare_and_swap_i64(&q->tail, new_t, 0);
    atomic_compare_and_swap_i64(&q->head, h + 1, 0);
    atomic_compare_and_swap_i64(&q->head, h, 0);
    return -1;
}

// ============= Graph Construction =============

static inline uint32_t simple_hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x45d9f3bU;
    x ^= x >> 16;
    x *= 0x45d9f3bU;
    x ^= x >> 16;
    return x;
}

static void build_grid_graph() {
    // Pass 1 – count degree per vertex
    for (int v = 0; v < NUM_VERTICES; v++) g_degree[v] = 0;

    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            int v   = r * GRID_COLS + c;
            int deg = 0;
            if (r > 0)              deg++;
            if (r < GRID_ROWS - 1)  deg++;
            if (c > 0)              deg++;
            if (c < GRID_COLS - 1)  deg++;
            deg++;  // one random long-range edge
            g_degree[v] = deg;
        }
    }

    // Build row_ptr
    g_row_ptr[0] = 0;
    for (int v = 0; v < NUM_VERTICES; v++)
        g_row_ptr[v + 1] = g_row_ptr[v] + g_degree[v];

    // Pass 2 – fill col_idx (use g_rank_new as temp insertion-offset array)
    for (int v = 0; v < NUM_VERTICES; v++)
        g_rank_new[v] = g_row_ptr[v];

    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            int v   = r * GRID_COLS + c;
            int pos = (int)g_rank_new[v];

            if (r > 0)              g_col_idx[pos++] = (r - 1) * GRID_COLS + c;
            if (r < GRID_ROWS - 1)  g_col_idx[pos++] = (r + 1) * GRID_COLS + c;
            if (c > 0)              g_col_idx[pos++] = r * GRID_COLS + (c - 1);
            if (c < GRID_COLS - 1)  g_col_idx[pos++] = r * GRID_COLS + (c + 1);

            // Random long-range edge
            uint32_t tgt = simple_hash((uint32_t)v) % NUM_VERTICES;
            if ((int)tgt == v) tgt = (tgt + 1) % NUM_VERTICES;
            g_col_idx[pos++] = (int32_t)tgt;

            g_rank_new[v] = pos;
        }
    }

    g_num_edges = g_row_ptr[NUM_VERTICES];
    std::printf("Graph: %d vertices, %d edges (grid %dx%d + random)\n",
               NUM_VERTICES, (int)g_num_edges, GRID_ROWS, GRID_COLS);
}

// ============= PageRank per-chunk kernel =============

static inline void compute_pagerank_range(int v_start, int v_end) {

    // base_rank = (1 − d) / N   (in fixed-point)
    int64_t base_rank = (RANK_SCALE * (DAMPING_DEN - DAMPING_NUM))
                        / (DAMPING_DEN * NUM_VERTICES);

    for (int v = v_start; v < v_end; v++) {
        int64_t sum = 0;
        int start = g_row_ptr[v];
        int end   = g_row_ptr[v + 1];

        // Pull contributions from in-neighbours
        for (int e = start; e < end; e++) {
            int u        = g_col_idx[e];
            int64_t urank = g_rank_old[u];
            int32_t udeg  = g_degree[u];
            if (udeg > 0) sum += urank / udeg;
        }

        int64_t new_rank = base_rank + (DAMPING_NUM * sum) / DAMPING_DEN;

        // Synthetic extra work so that compute dominates synchronisation
        volatile int64_t dummy = 0;
        for (int i = 0; i < VERTEX_WORK_ITERS; i++) {
            dummy += i;
        }

        g_rank_new[v] = new_rank;
    }
}

// ============= Work Distribution (hart 0 only) =============

static void distribute_chunks(int total_cores) {
    for (int c = 0; c < total_cores; c++) {
        queue_init(&core_queues[c]);
    }

    // Odd cores get 2× chunks, even cores get 1×
    int odd_cores    = total_cores / 2;
    int even_cores   = total_cores - odd_cores;
    int total_weight = even_cores + 2 * odd_cores;
    int base_chunks  = NUM_CHUNKS / total_weight;
    int leftover     = NUM_CHUNKS - base_chunks * total_weight;

    int chunk_id = 0;
    for (int c = 0; c < total_cores; c++) {
        int my_chunks = (c % 2 == 0) ? base_chunks : base_chunks * 2;
        if (leftover > 0) { my_chunks++; leftover--; }

        for (int i = 0; i < my_chunks && chunk_id < NUM_CHUNKS; i++) {
            int32_t v_begin = chunk_id * CHUNK_SIZE;
            int32_t v_end = v_begin + CHUNK_SIZE;
            if (v_end > NUM_VERTICES) v_end = NUM_VERTICES;
            queue_push(&core_queues[c], pack_range(v_begin, v_end));
            chunk_id++;
        }
    }
}

// ============= Baseline processing (no stealing) =============

static void baseline_process(int tid) {
    std::atomic_thread_fence(std::memory_order_acquire);
    int hpc       = g_harts_per_core;
    int my_core   = tid / hpc;
    PRWorkQueue* q = &core_queues[my_core];
    int64_t processed = 0;

    while (true) {
        int64_t packed = queue_pop_ttas(q);
        if (packed < 0) break;
        compute_pagerank_range(range_begin(packed), range_end(packed));
        processed++;
    }

    stat_chunks_processed[tid] += processed;
}

// ======================== main ========================

int main(int argc, char** argv) {
    const int harts_per_core = myCoreThreads();
    const int total_cores    = numPodCores();
    const int max_hw_harts   = total_cores * harts_per_core;
    const int tid            = get_thread_id();

    // ---------- Initialisation (hart 0) ----------
    if (tid == 0) {
        g_total_cores    = total_cores;
        g_harts_per_core = harts_per_core;
        g_total_harts    = max_hw_harts;

        std::printf("=== PageRank Baseline (no stealing) ===\n");
        std::printf("Hardware: %d cores x %d harts = %d total harts\n",
                   total_cores, harts_per_core, max_hw_harts);
        std::printf("Graph: %d vertices (%dx%d grid), %d chunks of %d\n",
                   NUM_VERTICES, GRID_ROWS, GRID_COLS, NUM_CHUNKS, CHUNK_SIZE);
        std::printf("PageRank iterations: %d, vertex work iters: %d\n",
                   PR_ITERATIONS, VERTEX_WORK_ITERS);
        std::printf("\n");

        for (int i = 0; i < max_hw_harts; i++) {
            g_local_sense[i]         = 0;
            stat_chunks_processed[i] = 0;
        }
        for (int c = 0; c < total_cores; c++) {
            queue_init(&core_queues[c]);
        }

        build_grid_graph();

        int64_t init_rank = RANK_SCALE / NUM_VERTICES;
        for (int v = 0; v < NUM_VERTICES; v++) {
            g_rank_old[v] = init_rank;
            g_rank_new[v] = 0;
        }

        std::atomic_thread_fence(std::memory_order_release);
        g_initialized.store(1, std::memory_order_release);
    } else {
        while (g_initialized.load(std::memory_order_acquire) == 0)
            hartsleep(10);
    }

    barrier();

    // ================================================================
    //  Baseline PageRank (no work stealing)
    // ================================================================
    if (tid == 0) {
        std::printf("Starting baseline PageRank...\n");
        // clear stats
        for (int i = 0; i < max_hw_harts; i++)
            stat_chunks_processed[i] = 0;
    }
    barrier();

    uint64_t start_cycles = 0, end_cycles = 0;
    if (tid == 0) asm volatile("rdcycle %0" : "=r"(start_cycles));

    barrier();

    for (int iter = 0; iter < PR_ITERATIONS; iter++) {
        // Hart 0: refill per-core queues for this iteration
        if (tid == 0) distribute_chunks(g_total_cores);
        barrier();

        baseline_process(tid);
        barrier();

        // Hart 0: advance iteration (new→old)
        if (tid == 0) {
            for (int v = 0; v < NUM_VERTICES; v++) {
                g_rank_old[v] = g_rank_new[v];
                g_rank_new[v] = 0;
            }
        }
        barrier();
    }

    if (tid == 0) {
        asm volatile("rdcycle %0" : "=r"(end_cycles));
        uint64_t elapsed = end_cycles - start_cycles;

        std::printf("\n=== Results (Baseline) ===\n");
        std::printf("Cycles elapsed: %lu\n", (unsigned long)elapsed);

        int64_t total_chunks = 0;
        int64_t max_core = 0, min_core = INT64_MAX;

        std::printf("\nPer-core chunk totals:\n");
        for (int c = 0; c < g_total_cores; c++) {
            int64_t ct = 0;
            for (int h = c * harts_per_core; h < (c + 1) * harts_per_core; h++)
                ct += stat_chunks_processed[h];
            total_chunks += ct;
            if (ct > max_core) max_core = ct;
            if (ct < min_core) min_core = ct;
            std::printf("  Core %2d: %ld chunks\n", c, (long)ct);
        }
        std::printf("  Total chunks: %ld (expected %d)\n",
                   (long)total_chunks, NUM_CHUNKS * PR_ITERATIONS);
        std::printf("  Max core: %ld, Min core: %ld\n", (long)max_core, (long)min_core);
        if (min_core > 0) {
            std::printf("  Imbalance ratio: %ld.%02ldx\n",
                       (long)(max_core / min_core),
                       (long)((max_core * 100 / min_core) % 100));
        }
        if (total_chunks > 0) {
            std::printf("  Cycles per chunk: %lu\n",
                       (unsigned long)(elapsed / total_chunks));
        }

        std::printf("\nSample ranks:\n");
        for (int v = 0; v < 5; v++)
            std::printf("  rank[%d] = %ld\n", v, (long)g_rank_old[v]);

        std::printf("\nCores: %d, Harts: %d\n", g_total_cores, g_total_harts);
    }

    barrier();
    return 0;
}
