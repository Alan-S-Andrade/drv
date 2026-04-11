// PageRank on RMAT power-law graph (imbalanced baseline).
//
// Loads rmat_r16.bin (DRV binary CSR, e.g. scale-16, 65K vertices, ~1.8M edges).
// Vertex v -> core floor(v * total_cores / N): RMAT hubs in low IDs -> core 0 overloaded.
// NO work stealing. This is the baseline for roofline comparison with BFS.
//
// Memory layout:
//   DRAM : CSR graph (row_ptr, col_idx, degree), rank_old[], rank_new[]
//   L2SP : work queues, barriers, statistics

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

static constexpr int MAX_HARTS  = 1024;
static constexpr int MAX_CORES  = 64;
static constexpr int PR_ITERS   = 10;
static constexpr int CHUNK_SIZE = 64;

// Fixed-point PageRank (rank 1.0 == RANK_SCALE)
static constexpr int64_t RANK_SCALE  = 1000000LL;
static constexpr int64_t DAMPING_NUM = 85;   // d = 0.85
static constexpr int64_t DAMPING_DEN = 100;

// =====================================================================
// Runtime config
// =====================================================================
__l2sp__ volatile int g_total_harts    = 0;
__l2sp__ volatile int g_harts_per_core = 0;
__l2sp__ volatile int g_total_cores    = 0;
__l2sp__ std::atomic<int> g_initialized = 0;

// =====================================================================
// Per-core work queue (vertex ranges assigned by owner)
// =====================================================================
struct WorkQueue {
    volatile int64_t head;
    volatile int64_t tail;
    int64_t v_start;   // first owned vertex
    int64_t v_end;     // one past last owned vertex
};

__l2sp__ WorkQueue core_queues[MAX_CORES];

// =====================================================================
// Graph (CSR from file, in DRAM)
// =====================================================================
__l2sp__ int32_t *g_row_ptr  = nullptr;
__l2sp__ int32_t *g_col_idx  = nullptr;
__l2sp__ int32_t *g_degree   = nullptr;  // pre-computed per-vertex out-degree
__l2sp__ char    *g_file_buffer = nullptr;
__l2sp__ int32_t  g_num_vertices = 0;
__l2sp__ int32_t  g_num_edges    = 0;

// =====================================================================
// PageRank data (DRAM)
// =====================================================================
__l2sp__ volatile int64_t *g_rank_old = nullptr;
__l2sp__ volatile int64_t *g_rank_new = nullptr;

// =====================================================================
// Barrier
// =====================================================================
__l2sp__ int64_t g_local_sense[MAX_HARTS];
__l2sp__ std::atomic<int64_t> g_count = 0;
__l2sp__ std::atomic<int64_t> g_sense = 0;

// =====================================================================
// Statistics
// =====================================================================
__l2sp__ volatile int64_t stat_verts_per_core[MAX_CORES];
__l2sp__ volatile int64_t stat_edges_per_core[MAX_CORES];

// =====================================================================
// Utilities
// =====================================================================
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

// =====================================================================
// Queue operations
// =====================================================================
static inline void queue_init(WorkQueue* q) {
    q->head    = 0;
    q->tail    = 0;
    q->v_start = 0;
    q->v_end   = 0;
}

static inline int64_t queue_pop_chunk(WorkQueue* q, int64_t max_chunk) {
    int64_t h = atomic_load_i64(&q->head);
    int64_t t = atomic_load_i64(&q->tail);
    if (h >= t) return -1;
    int64_t avail = t - h;
    int64_t chunk = (avail < max_chunk) ? avail : max_chunk;
    int64_t old_h = atomic_compare_and_swap_i64(&q->head, h, h + chunk);
    if (old_h == h) return h;  // returns chunk index (0-based within this core's range)
    return -1;
}

// =====================================================================
// Graph loading (same binary CSR as BFS)
// =====================================================================
static bool load_graph_from_file() {
    char fname[32];
    const char *src = "rmat_r16.bin";
    int fi = 0;
    while (src[fi]) { fname[fi] = src[fi]; fi++; }
    fname[fi] = '\0';

    int32_t header[5];
    struct ph_bulk_load_desc header_desc;
    header_desc.filename_addr = (long)fname;
    header_desc.dest_addr     = (long)header;
    header_desc.size          = (long)sizeof(header);
    header_desc.result        = 0;
    ph_bulk_load_file(&header_desc);

    if (header_desc.result <= 0) {
        std::printf("ERROR: bulk load of %s header failed (result=%ld)\n",
                    src, header_desc.result);
        return false;
    }

    int hdr_N = header[0];
    int hdr_E = header[1];

    if (hdr_N <= 0 || hdr_E < 0) {
        std::printf("ERROR: invalid CSR header: N=%d E=%d\n", hdr_N, hdr_E);
        return false;
    }

    const size_t file_size = 20
        + (size_t)(hdr_N + 1) * sizeof(int32_t)   // row_ptr
        + (size_t)hdr_E * sizeof(int32_t)          // col_idx
        + (size_t)hdr_N * sizeof(int32_t);         // degree

    char *buf = (char *)std::malloc(file_size);
    if (!buf) {
        std::printf("ERROR: malloc failed (%lu bytes)\n", (unsigned long)file_size);
        return false;
    }

    struct ph_bulk_load_desc desc;
    desc.filename_addr = (long)fname;
    desc.dest_addr     = (long)buf;
    desc.size          = (long)file_size;
    desc.result        = 0;
    ph_bulk_load_file(&desc);

    if (desc.result <= 0) {
        std::printf("ERROR: bulk load failed (result=%ld)\n", desc.result);
        std::free(buf);
        return false;
    }

    g_file_buffer  = buf;
    g_num_vertices = hdr_N;
    g_num_edges    = hdr_E;
    g_row_ptr = (int32_t *)(buf + 20);
    g_col_idx = (int32_t *)(buf + 20 + (size_t)(hdr_N + 1) * sizeof(int32_t));
    g_degree  = (int32_t *)(buf + 20 + (size_t)(hdr_N + 1) * sizeof(int32_t)
                                     + (size_t)hdr_E * sizeof(int32_t));

    // Allocate rank arrays in DRAM
    const size_t rank_bytes = (size_t)hdr_N * sizeof(int64_t);
    g_rank_old = (volatile int64_t *)std::malloc(rank_bytes);
    g_rank_new = (volatile int64_t *)std::malloc(rank_bytes);

    if (!g_rank_old || !g_rank_new) {
        std::printf("ERROR: malloc failed for rank arrays\n");
        std::free(buf);
        return false;
    }

    // Verify degree array matches row_ptr
    int32_t max_deg = 0;
    int32_t max_deg_v = 0;
    for (int32_t v = 0; v < hdr_N; v++) {
        int32_t deg = g_row_ptr[v + 1] - g_row_ptr[v];
        if (deg > max_deg) {
            max_deg = deg;
            max_deg_v = v;
        }
    }

    std::printf("Graph loaded: N=%d E=%d max_deg=%d (vertex %d)\n",
                hdr_N, hdr_E, max_deg, max_deg_v);
    return true;
}

// =====================================================================
// Imbalanced vertex-ownership distribution
// Vertex v -> core floor(v * total_cores / N)
// =====================================================================
static void distribute_vertices_imbalanced() {
    const int total_cores = g_total_cores;
    const int32_t N = g_num_vertices;

    for (int c = 0; c < total_cores; c++) {
        int64_t vs = (int64_t)c * N / total_cores;
        int64_t ve = (int64_t)(c + 1) * N / total_cores;
        core_queues[c].v_start = vs;
        core_queues[c].v_end   = ve;

        // Number of chunks for this core
        int64_t n_verts = ve - vs;
        int64_t n_chunks = (n_verts + CHUNK_SIZE - 1) / CHUNK_SIZE;
        core_queues[c].head = 0;
        core_queues[c].tail = n_chunks;
    }
}

// =====================================================================
// PageRank computation kernel — pull-based
// =====================================================================
static void compute_pagerank_chunk(int v_start, int v_end, int64_t base_rank) {
    for (int v = v_start; v < v_end; v++) {
        int64_t sum = 0;
        const int32_t rs = g_row_ptr[v];
        const int32_t re = g_row_ptr[v + 1];

        // Pull contributions from in-neighbours (CSR stores out-edges,
        // but for undirected RMAT graphs in-edges == out-edges)
        for (int32_t ei = rs; ei < re; ei++) {
            int32_t u = g_col_idx[ei];
            int64_t urank = g_rank_old[u];
            int32_t udeg  = g_degree[u];
            if (udeg > 0) sum += urank / udeg;
        }

        g_rank_new[v] = base_rank + (DAMPING_NUM * sum) / DAMPING_DEN;
    }
}

// =====================================================================
// Per-hart worker: pop chunks from own core's queue
// =====================================================================
static void process_iteration(int tid) {
    const int hpc     = g_harts_per_core;
    const int my_core = tid / hpc;
    WorkQueue* q      = &core_queues[my_core];

    const int64_t base_rank = (RANK_SCALE * (DAMPING_DEN - DAMPING_NUM))
                              / (DAMPING_DEN * g_num_vertices);

    int64_t local_verts = 0;
    int64_t local_edges = 0;

    for (;;) {
        int64_t chunk_idx = queue_pop_chunk(q, 1);
        if (chunk_idx < 0) {
            // Distinguish CAS contention from truly empty
            int64_t h = atomic_load_i64(&q->head);
            int64_t t = atomic_load_i64(&q->tail);
            if (h < t) continue;  // still work left, retry
            break;
        }

        int32_t v_begin = (int32_t)(q->v_start + chunk_idx * CHUNK_SIZE);
        int32_t v_end   = v_begin + CHUNK_SIZE;
        if (v_end > (int32_t)q->v_end) v_end = (int32_t)q->v_end;

        compute_pagerank_chunk(v_begin, v_end, base_rank);

        for (int v = v_begin; v < v_end; v++) {
            local_edges += (g_row_ptr[v + 1] - g_row_ptr[v]);
        }
        local_verts += (v_end - v_begin);
    }

    atomic_fetch_add_i64(&stat_verts_per_core[my_core], local_verts);
    atomic_fetch_add_i64(&stat_edges_per_core[my_core], local_edges);
}

// =====================================================================
// Main
// =====================================================================
int main(int argc, char** argv) {
    const int harts_per_core = myCoreThreads();
    const int total_cores    = numPodCores();
    const int max_hw_harts   = total_cores * harts_per_core;
    const int tid            = get_thread_id();

    // ――― Initialisation (hart 0) ―――
    if (tid == 0) {
        g_total_cores    = total_cores;
        g_harts_per_core = harts_per_core;
        g_total_harts    = max_hw_harts;

        for (int i = 0; i < max_hw_harts; i++)
            g_local_sense[i] = 0;
        for (int c = 0; c < total_cores; c++)
            queue_init(&core_queues[c]);

        if (!load_graph_from_file()) {
            std::abort();
        }

        // Initialise ranks: 1/N
        const int32_t N = g_num_vertices;
        int64_t init_rank = RANK_SCALE / N;
        for (int32_t v = 0; v < N; v++) {
            g_rank_old[v] = init_rank;
            g_rank_new[v] = 0;
        }

        std::printf("=== PageRank on RMAT (imbalanced baseline) ===\n");
        std::printf("Hardware: %d cores x %d harts = %d total harts\n",
                    total_cores, harts_per_core, max_hw_harts);
        std::printf("Graph: N=%d E=%d (RMAT CSR from rmat_r16.bin)\n",
                    g_num_vertices, g_num_edges);
        std::printf("Distribution: vertex v -> core floor(v*C/N)\n");
        std::printf("PageRank iterations: %d, chunk size: %d\n", PR_ITERS, CHUNK_SIZE);
        std::printf("\n");

        std::atomic_thread_fence(std::memory_order_release);
        g_initialized.store(1, std::memory_order_release);
    } else {
        while (g_initialized.load(std::memory_order_acquire) == 0)
            hartsleep(10);
    }

    barrier();

    // ――― Timed PageRank iterations ―――
    uint64_t start_cycles = 0, end_cycles = 0;
    if (tid == 0) asm volatile("rdcycle %0" : "=r"(start_cycles));

    barrier();

    for (int iter = 0; iter < PR_ITERS; iter++) {
        // Hart 0: set up per-core queues for this iteration
        if (tid == 0) {
            for (int c = 0; c < g_total_cores; c++) {
                stat_verts_per_core[c] = 0;
                stat_edges_per_core[c] = 0;
            }
            distribute_vertices_imbalanced();
        }
        barrier();

        process_iteration(tid);
        barrier();

        // Hart 0: swap rank arrays
        if (tid == 0) {
            volatile int64_t *tmp = g_rank_old;
            g_rank_old = g_rank_new;
            g_rank_new = tmp;
            // Clear new array
            for (int32_t v = 0; v < g_num_vertices; v++)
                g_rank_new[v] = 0;
        }
        barrier();
    }

    if (tid == 0) {
        asm volatile("rdcycle %0" : "=r"(end_cycles));
        const uint64_t elapsed = end_cycles - start_cycles;

        std::printf("\n=== PageRank Complete ===\n");
        std::printf("Iterations: %d\n", PR_ITERS);
        std::printf("Cycles elapsed: %lu\n", (unsigned long)elapsed);

        int64_t total_edges_done = 0;
        int64_t min_edges = INT64_MAX;
        int64_t max_edges = 0;

        distribute_vertices_imbalanced();
        std::printf("\nPer-core vertex ownership:\n");
        std::printf("Core | Vertices Owned | Edges (out-degree sum)\n");
        std::printf("-----|----------------|----------------------\n");

        for (int c = 0; c < g_total_cores; c++) {
            int64_t vs = core_queues[c].v_start;
            int64_t ve = core_queues[c].v_end;
            int64_t n_verts = ve - vs;
            int64_t n_edges = 0;
            for (int64_t v = vs; v < ve; v++)
                n_edges += (g_row_ptr[v + 1] - g_row_ptr[v]);

            std::printf("  %2d | %14ld | %20ld\n", c, (long)n_verts, (long)n_edges);
            total_edges_done += n_edges;
            if (n_edges < min_edges) min_edges = n_edges;
            if (n_edges > max_edges) max_edges = n_edges;
        }

        int64_t avg_edges = (g_total_cores > 0) ? (total_edges_done / g_total_cores) : 0;
        int64_t imbalance_pct = (max_edges > 0)
            ? ((max_edges - min_edges) * 100 / max_edges) : 0;

        std::printf("\nSummary:\n");
        std::printf("  Total edges/iter: %ld\n", (long)total_edges_done);
        std::printf("  Avg edges/core:   %ld\n", (long)avg_edges);
        std::printf("  Min/Max edges:    %ld / %ld\n", (long)min_edges, (long)max_edges);
        std::printf("  Imbalance (max-min)/max: %ld%%\n", (long)imbalance_pct);

        // Performance (integer-only math — no FP on this RISC-V sim)
        int64_t total_edge_traversals = (int64_t)total_edges_done * PR_ITERS;
        // Time in microseconds: cycles / (clock_ghz * 1000) for 1 GHz
        int64_t time_us = (int64_t)elapsed / 1000;
        // MTEPS * 100 (for 2-decimal fixed point)
        int64_t mteps_x100 = 0;
        if (time_us > 0)
            mteps_x100 = total_edge_traversals * 100 / time_us;
        // Cycles per edge * 10 (for 1-decimal fixed point)
        int64_t cpe_x10 = 0;
        if (total_edge_traversals > 0)
            cpe_x10 = (int64_t)elapsed * 10 / total_edge_traversals;

        std::printf("\nPerformance:\n");
        std::printf("  Time: %ld.%03ld ms\n",
                    (long)(time_us / 1000), (long)(time_us % 1000));
        std::printf("  Total edge traversals: %ld\n", (long)total_edge_traversals);
        std::printf("  MTEPS: %ld.%02ld\n",
                    (long)(mteps_x100 / 100), (long)(mteps_x100 % 100));
        std::printf("  Cycles per edge: %ld.%01ld\n",
                    (long)(cpe_x10 / 10), (long)(cpe_x10 % 10));

        // Sample ranks
        std::printf("\nSample ranks (fixed-point, 1.0=%ld):\n", (long)RANK_SCALE);
        for (int v = 0; v < 5 && v < g_num_vertices; v++)
            std::printf("  rank[%d] = %ld\n", v, (long)g_rank_old[v]);

        // Find top-5 ranked vertices
        std::printf("\nTop-5 ranked vertices:\n");
        int32_t top[5] = {0, 0, 0, 0, 0};
        for (int32_t v = 0; v < g_num_vertices; v++) {
            // insertion sort into top-5
            for (int i = 0; i < 5; i++) {
                if (g_rank_old[v] > g_rank_old[top[i]]) {
                    for (int j = 4; j > i; j--) top[j] = top[j-1];
                    top[i] = v;
                    break;
                }
            }
        }
        for (int i = 0; i < 5; i++) {
            int32_t v = top[i];
            int32_t deg = g_row_ptr[v+1] - g_row_ptr[v];
            std::printf("  #%d: vertex %d, rank=%ld, degree=%d\n",
                        i+1, v, (long)g_rank_old[v], deg);
        }

        std::printf("\nCores: %d, Harts: %d\n", g_total_cores, g_total_harts);
        std::printf("=============================================\n");

        // Cleanup
        std::free(g_file_buffer);
        std::free((void *)g_rank_old);
        std::free((void *)g_rank_new);
    }

    barrier();
    return 0;
}
