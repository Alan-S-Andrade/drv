// PageRank with Work Stealing (TTAS)
//
// Graph: deterministic 2D grid (GRID_ROWS x GRID_COLS) in CSR
// Work unit: a chunk of CHUNK_SIZE consecutive vertices
// Distribution: odd-numbered cores get 2x chunks (imbalanced)
// Idle cores steal from overloaded cores to achieve load balance.
//
// Contention reduction:
//   - TTAS (test-then-test-and-set) on queue pop/steal — plain volatile read
//     filters out hopeless CAS attempts before touching the cache line.
//   - core_has_work[] hint lets stealers skip queues known to be empty.
//   - Exponential backoff on failed steal rounds.

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
static constexpr int GRID_ROWS       = 1000;
static constexpr int GRID_COLS       = 100;
static constexpr int NUM_VERTICES    = GRID_ROWS * GRID_COLS;   // 4096
static constexpr int MAX_EDGES       = (4 * GRID_ROWS * GRID_COLS)
                                     - (2 * GRID_ROWS) - (2 * GRID_COLS);
static constexpr int CHUNK_SIZE      = 64;                      // vertices per work unit
static constexpr int NUM_CHUNKS      = (NUM_VERTICES + CHUNK_SIZE - 1) / CHUNK_SIZE;
static constexpr int PR_ITERATIONS   = 10;
static constexpr int VERTEX_WORK_ITERS = 200; // synthetic per-vertex compute

static constexpr int PR_QUEUE_SIZE   = 512;
static constexpr int MAX_HARTS       = 1024;
static constexpr int MAX_CORES       = 64;
static constexpr int STEAL_K         = 4;    // Max chunks to steal per episode (tunable)
static constexpr int WQ_TRACE_STRIDE = 64;   // sample every N loop turns in work_stealing_process (hart 0)
static constexpr int MAX_WQ_TRACE_SAMPLES = 32768;

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

// ======================= Graph (CSR in DRAM) =======================
__dram__ int32_t g_row_ptr[NUM_VERTICES + 1];
__dram__ int32_t g_col_idx[MAX_EDGES];
__dram__ int32_t g_degree[NUM_VERTICES];
__dram__ int32_t g_num_edges = 0;

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
// Best-effort hint: 1 if queue likely non-empty, 0 otherwise
__l2sp__ volatile int32_t core_has_work[MAX_CORES];

// Per-core steal token: only one hart per core may steal at a time
__l2sp__ std::atomic<int> core_thief[MAX_CORES];

// ======================= Synchronization =======================
__l2sp__ int64_t           g_local_sense[MAX_HARTS];
__l2sp__ std::atomic<int64_t> g_count = 0;
__l2sp__ std::atomic<int64_t> g_sense = 0;

// ======================= Statistics =======================
__l2sp__ volatile int64_t stat_chunks_processed[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_attempts[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_success[MAX_HARTS];
__l2sp__ volatile int32_t g_current_iter = -1;
__l2sp__ volatile uint64_t g_core_l1sp_bytes = 0;

enum WQTracePhase : int32_t {
    WQ_PHASE_INIT = 0,
    WQ_PHASE_ITER_BEGIN = 1,
    WQ_PHASE_ITER_DURING = 2,
    WQ_PHASE_ITER_END = 3,
    WQ_PHASE_FINAL = 4
};

struct WQTraceSample {
    int32_t iter;
    int32_t phase;
};

__dram__ WQTraceSample g_wq_trace_samples[MAX_WQ_TRACE_SAMPLES];
__dram__ int32_t g_wq_trace_depths[MAX_WQ_TRACE_SAMPLES][MAX_CORES];
__l2sp__ volatile int32_t g_wq_trace_count = 0;
__l2sp__ volatile int32_t g_wq_trace_dropped = 0;

// ======================== Utility ========================

static inline int get_thread_id() {
    return (myCoreId() << 4) + myThreadId();
}

static inline const char* wq_phase_name(int32_t phase) {
    switch (phase) {
    case WQ_PHASE_INIT: return "init";
    case WQ_PHASE_ITER_BEGIN: return "iter_begin";
    case WQ_PHASE_ITER_DURING: return "iter_during";
    case WQ_PHASE_ITER_END: return "iter_end";
    case WQ_PHASE_FINAL: return "final";
    default: return "unknown";
    }
}

static void record_wq_trace(int32_t phase, int32_t iter) {
    int32_t idx = g_wq_trace_count;
    if (idx >= MAX_WQ_TRACE_SAMPLES) {
        g_wq_trace_dropped++;
        return;
    }
    g_wq_trace_count = idx + 1;

    g_wq_trace_samples[idx] = {iter, phase};
    for (int c = 0; c < g_total_cores; c++) {
        int64_t depth = core_queues[c].tail - core_queues[c].head;
        if (depth < 0) depth = 0;
        g_wq_trace_depths[idx][c] = (int32_t)depth;
    }
}

static void dump_wq_trace() {
    const uint64_t core_l1sp_bytes = g_core_l1sp_bytes;
    const uint64_t global_l1sp_bytes = core_l1sp_bytes * (uint64_t)g_total_cores;

    auto emit = [&](FILE* out) {
        std::fprintf(out, "WQTRACE_DUMP_BEGIN,bench=pagerank_work_stealing,cores=%d,samples=%d,dropped=%d\n",
                     g_total_cores, (int)g_wq_trace_count, (int)g_wq_trace_dropped);
        for (int32_t i = 0; i < g_wq_trace_count; i++) {
            const WQTraceSample& s = g_wq_trace_samples[i];
            std::fprintf(out, "WQTRACE,bench=pagerank_work_stealing,cores=%d,sample=%d,phase=%s,level=-1,iter=%d,queue=core,depths=",
                         g_total_cores, (int)i, wq_phase_name(s.phase), (int)s.iter);
            for (int c = 0; c < g_total_cores; c++) {
                if (c > 0) std::fprintf(out, "|");
                std::fprintf(out, "%d", (int)g_wq_trace_depths[i][c]);
            }
            std::fprintf(out, "\n");
        }
        std::fprintf(out, "WQTRACE_DUMP_END,bench=pagerank_work_stealing\n");

        std::fprintf(out,
                     "L1SPTRACE_DUMP_BEGIN,bench=pagerank_work_stealing,cores=%d,harts=%d,samples=%d\n",
                     g_total_cores, g_total_harts, (int)g_wq_trace_count);
        std::fprintf(out,
                     "L1SPTRACE_CONFIG,bench=pagerank_work_stealing,core_bytes=%lu,global_bytes=%lu\n",
                     (unsigned long)core_l1sp_bytes, (unsigned long)global_l1sp_bytes);
        for (int32_t i = 0; i < g_wq_trace_count; i++) {
            const WQTraceSample& s = g_wq_trace_samples[i];
            std::fprintf(out,
                         "L1SPTRACE_GLOBAL,bench=pagerank_work_stealing,sample=%d,phase=%s,level=-1,iter=%d,bytes=%lu\n",
                         (int)i, wq_phase_name(s.phase), (int)s.iter,
                         (unsigned long)global_l1sp_bytes);
        }
        for (int h = 0; h < g_total_harts; h++) {
            std::fprintf(out,
                         "L1SPTRACE_HART,bench=pagerank_work_stealing,hart=%d,core=%d,thread=%d,bytes=%lu\n",
                         h, h / g_harts_per_core, h % g_harts_per_core,
                         (unsigned long)core_l1sp_bytes);
        }
        std::fprintf(out, "L1SPTRACE_DUMP_END,bench=pagerank_work_stealing\n");
    };

    emit(stdout);

    char path[64];
    std::snprintf(path, sizeof(path), "run_%dcores.log", g_total_cores);
    FILE* fp = std::fopen(path, "w");
    if (fp != nullptr) {
        emit(fp);
        std::fclose(fp);
        std::printf("WQTRACE_FILE_WRITTEN,bench=pagerank_work_stealing,path=%s\n", path);
    } else {
        std::printf("WQTRACE_FILE_ERROR,bench=pagerank_work_stealing,path=%s\n", path);
    }
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

// ============= Queue Ops (TTAS-optimised) =============

static inline void queue_init(PRWorkQueue* q) {
    q->head = 0;
    q->tail = 0;
}

// Single-thread push (hart 0 only, during distribution)
static inline bool queue_push(PRWorkQueue* q, int core_id, int64_t item) {
    int64_t t = q->tail;
    if (t >= PR_QUEUE_SIZE) return false;
    q->items[t] = item;
    q->tail = t + 1;
    core_has_work[core_id] = 1;
    return true;
}

// Atomic push for refilling local queue (thief pushes stolen items for local harts)
static inline bool queue_push_atomic(PRWorkQueue* q, int core_id, int64_t item) {
    while (true) {
        int64_t t = atomic_load_i64(&q->tail);
        if (t >= PR_QUEUE_SIZE) return false;
        int64_t old_t = atomic_compare_and_swap_i64(&q->tail, t, t + 1);
        if (old_t == t) {
            q->items[t] = item;
            core_has_work[core_id] = 1;
            return true;
        }
        hartsleep(1);
    }
}

// TTAS pop – local harts pop from tail (LIFO for locality).
// Plain volatile reads filter hopeless CAS attempts.
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

    // Race with a stealer for the very last item
    int64_t old_head = atomic_compare_and_swap_i64(&q->head, h, h + 1);
    if (old_head == h && h == new_t) {
        // Won – take item, reset queue
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

// TTAS steal – other cores steal from head (FIFO).
// Two-phase: plain read → CAS.  One-shot: if CAS loses, move on.
static inline int64_t queue_steal_ttas(PRWorkQueue* q) {
    // ---- TEST phase ----
    int64_t h = q->head;   // volatile read, no bus traffic
    int64_t t = q->tail;
    if (h >= t) return -1;

    // ---- TEST-AND-SET phase ----
    h = atomic_load_i64(&q->head);  // coherent read
    t = q->tail;
    if (h >= t) return -1;

    int64_t work = q->items[h];
    int64_t old_head = atomic_compare_and_swap_i64(&q->head, h, h + 1);
    if (old_head == h) return work;  // success
    return -1;                       // contention → skip this victim
}

// TTAS steal-k: grab up to k items from head in one CAS (FIFO).
// Returns count stolen (0 if empty/contention). Sets *out_begin to first index.
static inline int64_t queue_steal_k_ttas(PRWorkQueue* q, int64_t* out_begin, int64_t max_k) {
    // ---- TEST phase ----
    int64_t h = q->head;   // volatile read, no bus traffic
    int64_t t = q->tail;
    if (h >= t) return 0;

    // ---- TEST-AND-SET phase ----
    h = atomic_load_i64(&q->head);  // coherent read
    t = q->tail;
    if (h >= t) return 0;

    int64_t avail = t - h;
    int64_t k = (avail < max_k) ? avail : max_k;

    int64_t old_head = atomic_compare_and_swap_i64(&q->head, h, h + k);
    if (old_head == h) {
        *out_begin = h;
        return k;
    }
    return 0;  // contention → skip this victim
}

// ============= Graph Construction =============

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
            g_degree[v] = deg;
        }
    }

    // Build row_ptr
    g_row_ptr[0] = 0;
    for (int v = 0; v < NUM_VERTICES; v++)
        g_row_ptr[v + 1] = g_row_ptr[v] + g_degree[v];

    // Pass 2 – fill col_idx (use degree as temp insertion-offset array)
    for (int v = 0; v < NUM_VERTICES; v++) {
        g_degree[v] = g_row_ptr[v];
    }

    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            int v   = r * GRID_COLS + c;
            int pos = g_degree[v];

            if (r > 0)              g_col_idx[pos++] = (r - 1) * GRID_COLS + c;
            if (r < GRID_ROWS - 1)  g_col_idx[pos++] = (r + 1) * GRID_COLS + c;
            if (c > 0)              g_col_idx[pos++] = r * GRID_COLS + (c - 1);
            if (c < GRID_COLS - 1)  g_col_idx[pos++] = r * GRID_COLS + (c + 1);
            g_degree[v] = pos;
        }
    }

    g_num_edges = g_row_ptr[NUM_VERTICES];
    // Restore degree[] to out-degree counts.
    for (int v = 0; v < NUM_VERTICES; v++) {
        g_degree[v] = g_row_ptr[v + 1] - g_row_ptr[v];
    }
    std::printf("Graph: %d vertices, %d directed edges (grid %dx%d CSR in DRAM)\n",
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
        core_has_work[c] = 0;
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
            queue_push(&core_queues[c], c, pack_range(v_begin, v_end));
            chunk_id++;
        }
    }
}

// ============= Victim Selection =============

// Fast xorshift PRNG for victim selection — mixes tid + counter to decorrelate thieves
static inline uint32_t xorshift_victim(uint32_t seed) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static constexpr int RECENTLY_TRIED_SIZE = 4; // small "recently failed" ring buffer

// ============= Work Stealing =============

static void work_stealing_process(int tid) {
    std::atomic_thread_fence(std::memory_order_acquire);
    int hpc         = g_harts_per_core;
    int total_cores = g_total_cores;
    int my_core     = tid / hpc;
    int my_local_id = tid % hpc;   // hart index within core
    PRWorkQueue* my_q = &core_queues[my_core];

    // Xorshift RNG state — seeded per-hart so thieves diverge
    uint32_t rng_state = (uint32_t)(tid + 1) * 2654435761u;

    // Small ring buffer of recently-failed victims to avoid immediate retries
    int recently_tried[RECENTLY_TRIED_SIZE];
    for (int i = 0; i < RECENTLY_TRIED_SIZE; i++) recently_tried[i] = -1;
    int rt_idx = 0;

    int64_t processed            = 0;
    int64_t local_steal_attempts = 0;
    int64_t local_steal_success  = 0;

    // Local buffer for stolen items — avoid re-touching victim's cache lines
    int64_t stolen_buf[STEAL_K];

    int consecutive_empty = 0;
    const int MAX_EMPTY   = total_cores * 2;      // was total_cores*4+16
    int64_t backoff       = 4;                     // start higher
    const int64_t max_backoff = 512;               // was 64
    int64_t loop_turn = 0;

    while (consecutive_empty < MAX_EMPTY) {
        loop_turn++;
        if (tid == 0 && (loop_turn % WQ_TRACE_STRIDE) == 0) {
            record_wq_trace(WQ_PHASE_ITER_DURING, g_current_iter);
        }

        // 1. Try own queue first
        int64_t packed = queue_pop_ttas(my_q);
        if (packed >= 0) {
            compute_pagerank_range(range_begin(packed), range_end(packed));
            processed++;
            consecutive_empty = 0;
            backoff = 4;
            continue;
        }

        // Eagerly mark own queue empty so stealers skip us
        core_has_work[my_core] = 0;

        // Try to become this core's thief (only one hart per core steals at a time)
        if (core_thief[my_core].exchange(1, std::memory_order_acquire) != 0) {
            // Another hart on this core is already stealing — back off harder, retry local
            hartsleep(backoff * 4);
            if (backoff < max_backoff) backoff <<= 1;
            continue;
        }

        // Won the steal token — steal from other cores (random victim)
        bool found = false;
        for (int r = 0; r < total_cores - 1; r++) {
            // Pick a random victim, skip self, empty hints, and recently-failed
            int victim;
            int pick_tries = 0;
            do {
                rng_state = xorshift_victim(rng_state);
                victim = (int)(rng_state % (uint32_t)total_cores);
                bool in_recent = false;
                if (victim != my_core && core_has_work[victim] != 0) {
                    for (int ri = 0; ri < RECENTLY_TRIED_SIZE; ri++) {
                        if (recently_tried[ri] == victim) { in_recent = true; break; }
                    }
                    if (!in_recent) break;
                }
                pick_tries++;
            } while (pick_tries < total_cores);
            if (victim == my_core || core_has_work[victim] == 0) continue;

            local_steal_attempts++;
            int64_t steal_begin;
            int64_t stolen = queue_steal_k_ttas(&core_queues[victim], &steal_begin, STEAL_K);

            if (stolen > 0) {
                local_steal_success++;
                // Copy stolen items to local buffer, then release victim's cache lines
                for (int64_t s = 0; s < stolen; s++) {
                    stolen_buf[s] = core_queues[victim].items[steal_begin + s];
                }
                // Thief keeps first item; pushes rest into local queue for sibling harts
                for (int64_t s = 1; s < stolen; s++) {
                    queue_push_atomic(my_q, my_core, stolen_buf[s]);
                }
                // Process only the thief's own item
                compute_pagerank_range(range_begin(stolen_buf[0]), range_end(stolen_buf[0]));
                processed++;
                found = true;
                consecutive_empty = 0;
                backoff = 4;
                // Clear recently-tried on success
                for (int ri = 0; ri < RECENTLY_TRIED_SIZE; ri++) recently_tried[ri] = -1;
                break;
            } else {
                // Record this victim as recently failed
                recently_tried[rt_idx] = victim;
                rt_idx = (rt_idx + 1) % RECENTLY_TRIED_SIZE;
            }
        }

        // Release steal token
        core_thief[my_core].store(0, std::memory_order_release);

        if (!found) {
            consecutive_empty++;
            hartsleep(backoff);
            if (backoff < max_backoff) backoff <<= 1;
        }
    }

    stat_chunks_processed[tid] += processed;
    stat_steal_attempts[tid]   += local_steal_attempts;
    stat_steal_success[tid]    += local_steal_success;
}

// ======================== main ========================

int main(int argc, char** argv) {
    const int harts_per_core = myCoreThreads();
    const int total_cores    = numPodCores();
    const int max_hw_harts   = total_cores * harts_per_core;
    const int tid            = get_thread_id();

    // ---------- Initialisation (hart 0) ----------
    if (tid == 0) {
        g_wq_trace_count = 0;
        g_wq_trace_dropped = 0;
        g_current_iter = -1;
        g_total_cores    = total_cores;
        g_harts_per_core = harts_per_core;
        g_total_harts    = max_hw_harts;

        std::printf("=== PageRank Work Stealing (TTAS) ===\n");
        std::printf("Hardware: %d cores x %d harts = %d total harts\n",
                   total_cores, harts_per_core, max_hw_harts);
        std::printf("Graph: %d vertices (%dx%d grid CSR in DRAM), %d chunks of %d\n",
                   NUM_VERTICES, GRID_ROWS, GRID_COLS, NUM_CHUNKS, CHUNK_SIZE);
        std::printf("PageRank iterations: %d, vertex work iters: %d\n",
                   PR_ITERATIONS, VERTEX_WORK_ITERS);
        g_core_l1sp_bytes = coreL1SPSize();
        std::printf("L1SP: per-core=%lu bytes, global=%lu bytes\n",
                   (unsigned long)g_core_l1sp_bytes,
                   (unsigned long)(g_core_l1sp_bytes * (uint64_t)g_total_cores));
        std::printf("\n");

        for (int i = 0; i < max_hw_harts; i++) {
            g_local_sense[i]         = 0;
            stat_chunks_processed[i] = 0;
            stat_steal_attempts[i]   = 0;
            stat_steal_success[i]    = 0;
        }
        for (int c = 0; c < total_cores; c++) {
            queue_init(&core_queues[c]);
            core_has_work[c] = 0;
            core_thief[c].store(0, std::memory_order_relaxed);
        }

        build_grid_graph();

        int64_t init_rank = RANK_SCALE / NUM_VERTICES;
        for (int v = 0; v < NUM_VERTICES; v++) {
            g_rank_old[v] = init_rank;
            g_rank_new[v] = 0;
        }

        std::atomic_thread_fence(std::memory_order_release);
        g_initialized.store(1, std::memory_order_release);
        record_wq_trace(WQ_PHASE_INIT, -1);
    } else {
        while (g_initialized.load(std::memory_order_acquire) == 0)
            hartsleep(10);
    }

    barrier();

    // ================================================================
    //  Work Stealing PageRank
    // ================================================================
    if (tid == 0) {
        std::printf("Starting work-stealing PageRank...\n");
        for (int i = 0; i < max_hw_harts; i++) {
            stat_chunks_processed[i] = 0;
            stat_steal_attempts[i]   = 0;
            stat_steal_success[i]    = 0;
        }
    }
    barrier();

    uint64_t start_cycles = 0, end_cycles = 0;
    if (tid == 0) asm volatile("rdcycle %0" : "=r"(start_cycles));

    barrier();

    for (int iter = 0; iter < PR_ITERATIONS; iter++) {
        if (tid == 0) {
            g_current_iter = iter;
            distribute_chunks(g_total_cores);
            record_wq_trace(WQ_PHASE_ITER_BEGIN, iter);
        }
        barrier();

        work_stealing_process(tid);
        barrier();
        if (tid == 0) {
            record_wq_trace(WQ_PHASE_ITER_END, iter);
        }

        if (tid == 0) {
            for (int v = 0; v < NUM_VERTICES; v++) {
                g_rank_old[v] = g_rank_new[v];
                g_rank_new[v] = 0;
            }
        }
        barrier();
    }

    if (tid == 0) {
        record_wq_trace(WQ_PHASE_FINAL, PR_ITERATIONS);
        asm volatile("rdcycle %0" : "=r"(end_cycles));
        uint64_t elapsed = end_cycles - start_cycles;

        std::printf("\n=== Results (Work Stealing) ===\n");
        std::printf("Cycles elapsed: %lu\n", (unsigned long)elapsed);

        int64_t total_chunks   = 0;
        int64_t total_attempts = 0;
        int64_t total_success  = 0;
        int64_t max_core = 0, min_core = INT64_MAX;

        std::printf("\nPer-core stats:\n");
        std::printf("Core | Chunks | Steal Att. | Steals OK\n");
        std::printf("-----|--------|------------|----------\n");
        for (int c = 0; c < g_total_cores; c++) {
            int64_t cc = 0, ca = 0, cs = 0;
            for (int h = c * harts_per_core; h < (c + 1) * harts_per_core; h++) {
                cc += stat_chunks_processed[h];
                ca += stat_steal_attempts[h];
                cs += stat_steal_success[h];
            }
            total_chunks  += cc;
            total_attempts += ca;
            total_success  += cs;
            if (cc > max_core) max_core = cc;
            if (cc < min_core) min_core = cc;
            std::printf("%4d | %6ld | %10ld | %9ld\n",
                       c, (long)cc, (long)ca, (long)cs);
        }
        std::printf("  Total chunks: %ld (expected %d)\n",
                   (long)total_chunks, NUM_CHUNKS * PR_ITERATIONS);
        std::printf("  Max core: %ld, Min core: %ld\n", (long)max_core, (long)min_core);
        if (min_core > 0) {
            std::printf("  Imbalance ratio: %ld.%02ldx\n",
                       (long)(max_core / min_core),
                       (long)((max_core * 100 / min_core) % 100));
        }
        std::printf("  Total steal attempts: %ld\n", (long)total_attempts);
        std::printf("  Successful steals:    %ld", (long)total_success);
        if (total_attempts > 0)
            std::printf(" (%ld%%)", (long)(100 * total_success / total_attempts));
        std::printf("\n");
        if (total_chunks > 0) {
            std::printf("  Cycles per chunk: %lu\n",
                       (unsigned long)(elapsed / total_chunks));
        }

        std::printf("\nSample ranks:\n");
        for (int v = 0; v < 5; v++)
            std::printf("  rank[%d] = %ld\n", v, (long)g_rank_old[v]);

        std::printf("\nCores: %d, Harts: %d\n", g_total_cores, g_total_harts);
        dump_wq_trace();
    }

    barrier();
    return 0;
}
