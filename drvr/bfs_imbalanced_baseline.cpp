// Level-synchronous BFS with power-law imbalanced vertex-ownership distribution.
// Vertex v is always owned by core floor(v * total_cores / N).
// RMAT graphs concentrate hub vertices in low vertex IDs → core 0 overloaded.
// NO work stealing. This is the baseline for measuring work-stealing speedup.
//
// Memory layout matches bfs_ws_utilization:
//   L2SP: work queues, frontier buffers, visited[], dist_arr[], barriers
//   DRAM: CSR graph (row_ptr, col_idx), overflow for visited/dist/frontiers

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
static constexpr int BFS_CHUNK_SIZE = 8;  // match ws_utilization

__l2sp__ volatile int g_total_harts = 0;
__l2sp__ volatile int g_harts_per_core = 0;
__l2sp__ volatile int g_total_cores = 0;
__l2sp__ std::atomic<int> g_initialized = 0;

// =====================================================================
// Per-core work queues (index into frontier storage)
// =====================================================================
struct WorkQueue {
    volatile int64_t head;
    volatile int64_t tail;
    int64_t start_idx;
};

__l2sp__ WorkQueue core_queues[MAX_CORES];

// =====================================================================
// Frontier double-buffer (L2SP + DRAM overflow)
// =====================================================================
struct FrontierBuffer {
    volatile int64_t tail;
    int64_t capacity;
    int64_t l2sp_capacity;
    int64_t* l2sp_items;
    int64_t* dram_items;
};

__l2sp__ FrontierBuffer g_next_frontier;
__l2sp__ int64_t *g_current_frontier_storage = nullptr;
__l2sp__ int64_t *g_current_frontier_dram_storage = nullptr;

// =====================================================================
// Barrier
// =====================================================================
__l2sp__ int64_t g_local_sense[MAX_HARTS];
__l2sp__ std::atomic<int64_t> g_count = 0;
__l2sp__ std::atomic<int64_t> g_sense = 0;

__l2sp__ volatile int32_t core_has_work[MAX_CORES];

// =====================================================================
// Graph + BFS state
// =====================================================================
__l2sp__ int32_t *g_row_ptr    = nullptr;
__l2sp__ int32_t *g_col_idx    = nullptr;
__l2sp__ volatile int64_t *visited = nullptr;
__l2sp__ int32_t *dist_arr     = nullptr;
__l2sp__ char    *g_file_buffer = nullptr;
__l2sp__ int32_t  g_num_vertices = 0;
__l2sp__ int32_t  g_num_edges    = 0;
__l2sp__ int32_t  g_bfs_source   = 0;
__l2sp__ int32_t  g_visited_in_l2sp = 0;
__l2sp__ int32_t  g_dist_in_l2sp = 0;
__l2sp__ int32_t  g_frontiers_in_l2sp = 0;

// =====================================================================
// Statistics
// =====================================================================
__l2sp__ volatile int64_t stat_nodes_per_core[MAX_CORES];
__l2sp__ volatile int64_t stat_edges_per_core[MAX_CORES];
__l2sp__ volatile int64_t stat_nodes_processed[MAX_HARTS];
__l2sp__ volatile int64_t discovered = 0;

// =====================================================================
// Utilities
// =====================================================================
static inline int get_thread_id() {
    return myCoreId() * myCoreThreads() + myThreadId();
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
        while (g_sense.load(std::memory_order_acquire) != local) {
            if (w < wmax) w <<= 1;
            hartsleep(w);
        }
    }
}

static inline void queue_init(WorkQueue* q) {
    q->head = 0;
    q->tail = 0;
    q->start_idx = 0;
}

static inline void queue_assign_slice(WorkQueue* q, int64_t start_idx, int64_t count) {
    q->start_idx = start_idx;
    q->head = 0;
    q->tail = count;
}

// =====================================================================
// Frontier accessors (L2SP → DRAM)
// =====================================================================
static inline int64_t* frontier_current_ptr(int64_t idx) {
    return (idx < g_next_frontier.l2sp_capacity)
        ? (g_current_frontier_storage + idx)
        : (g_current_frontier_dram_storage + (idx - g_next_frontier.l2sp_capacity));
}

static inline int64_t frontier_current_get(int64_t idx) {
    return *frontier_current_ptr(idx);
}

static inline void frontier_current_set(int64_t idx, int64_t value) {
    *frontier_current_ptr(idx) = value;
}

static inline int64_t* frontier_next_item_ptr(int64_t idx) {
    return (idx < g_next_frontier.l2sp_capacity)
        ? (g_next_frontier.l2sp_items + idx)
        : (g_next_frontier.dram_items + (idx - g_next_frontier.l2sp_capacity));
}

static inline int64_t frontier_next_item_get(int64_t idx) {
    return *frontier_next_item_ptr(idx);
}

static inline void frontier_next_item_set(int64_t idx, int64_t value) {
    *frontier_next_item_ptr(idx) = value;
}

// =====================================================================
// Queue / frontier operations
// =====================================================================
static inline int64_t queue_pop_chunk(WorkQueue* q, int core_id,
                                      int64_t* out_begin, int64_t max_chunk) {
    int64_t h = atomic_load_i64(&q->head);
    int64_t t = atomic_load_i64(&q->tail);
    if (h >= t) {
        core_has_work[core_id] = 0;
        return 0;
    }
    int64_t avail = t - h;
    int64_t chunk = (avail < max_chunk) ? avail : max_chunk;
    int64_t old_h = atomic_compare_and_swap_i64(&q->head, h, h + chunk);
    if (old_h == h) {
        *out_begin = h;
        return chunk;
    }
    return 0;
}

static inline bool next_frontier_push_atomic(int64_t work) {
    int64_t idx = atomic_fetch_add_i64(&g_next_frontier.tail, 1);
    if (idx >= g_next_frontier.capacity) return false;
    frontier_next_item_set(idx, work);
    return true;
}

static inline bool claim_node(int64_t v) {
    const int64_t old = atomic_swap_i64((volatile int64_t *)&visited[v], 1);
    return old == 0;
}

// =====================================================================
// L2SP allocator
// =====================================================================
extern "C" char l2sp_end[];

static inline uintptr_t align_up_uintptr(uintptr_t value, uintptr_t align) {
    return (value + align - 1) & ~(align - 1);
}

static void* try_alloc_l2sp(uintptr_t* heap, size_t bytes, size_t align,
                            uintptr_t l2sp_limit) {
    const uintptr_t start = align_up_uintptr(*heap, (uintptr_t)align);
    const uintptr_t end = start + (uintptr_t)bytes;
    if (end > l2sp_limit) return nullptr;
    *heap = end;
    return (void*)start;
}

// =====================================================================
// Graph loading
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

    int hdr_N      = header[0];
    int hdr_E      = header[1];
    int hdr_D      = header[2];
    int hdr_source = header[4];

    if (hdr_N <= 0 || hdr_E < 0) {
        std::printf("ERROR: invalid CSR header: N=%d E=%d\n", hdr_N, hdr_E);
        return false;
    }

    const size_t file_size = 20
        + (size_t)(hdr_N + 1) * sizeof(int32_t)
        + (size_t)hdr_E * sizeof(int32_t)
        + (size_t)hdr_N * sizeof(int32_t);

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
    g_bfs_source   = hdr_source;
    g_row_ptr = (int32_t *)(buf + 20);
    g_col_idx = (int32_t *)(buf + 20 + (size_t)(hdr_N + 1) * sizeof(int32_t));

    // Use highest-degree vertex as BFS source (maximizes imbalance)
    int32_t max_deg = -1;
    int32_t max_deg_vertex = 0;
    for (int32_t v = 0; v < hdr_N; v++) {
        const int32_t deg = g_row_ptr[v + 1] - g_row_ptr[v];
        if (deg > max_deg) {
            max_deg = deg;
            max_deg_vertex = v;
        }
    }
    g_bfs_source = max_deg_vertex;

    // Allocate visited, dist, frontiers: L2SP first, DRAM overflow
    uintptr_t l2sp_heap = align_up_uintptr((uintptr_t)l2sp_end, 8);
    const uintptr_t l2sp_base = 0x20000000;
    const uintptr_t l2sp_limit = l2sp_base + podL2SPSize();

    g_visited_in_l2sp = 0;
    g_dist_in_l2sp = 0;
    g_frontiers_in_l2sp = 0;

    const size_t visited_bytes = (size_t)hdr_N * sizeof(int64_t);
    const size_t dist_bytes    = (size_t)hdr_N * sizeof(int32_t);

    visited = (volatile int64_t *)try_alloc_l2sp(
        &l2sp_heap, visited_bytes, alignof(int64_t), l2sp_limit);
    if (visited) { g_visited_in_l2sp = 1; }
    else         { visited = (volatile int64_t *)std::malloc(visited_bytes); }

    dist_arr = (int32_t *)try_alloc_l2sp(
        &l2sp_heap, dist_bytes, alignof(int32_t), l2sp_limit);
    if (dist_arr) { g_dist_in_l2sp = 1; }
    else          { dist_arr = (int32_t *)std::malloc(dist_bytes); }

    if (!visited || !dist_arr) {
        std::printf("ERROR: allocation failed for visited/dist_arr\n");
        std::free(buf);
        if (!g_visited_in_l2sp) std::free((void *)visited);
        if (!g_dist_in_l2sp) std::free(dist_arr);
        return false;
    }

    // Frontier buffers
    const uintptr_t frontier_heap = align_up_uintptr(l2sp_heap, alignof(int64_t));
    const uintptr_t frontier_avail =
        (frontier_heap < l2sp_limit) ? (l2sp_limit - frontier_heap) : 0;
    // Each vertex needs 2 × int64_t (current + next)
    const int64_t l2sp_verts =
        (int64_t)((frontier_avail / (2 * sizeof(int64_t))) > (uintptr_t)hdr_N
            ? hdr_N : (frontier_avail / (2 * sizeof(int64_t))));
    const int64_t dram_verts = hdr_N - l2sp_verts;

    g_next_frontier.l2sp_capacity = l2sp_verts;
    g_current_frontier_storage = nullptr;
    g_current_frontier_dram_storage = nullptr;
    g_next_frontier.l2sp_items = nullptr;
    g_next_frontier.dram_items = nullptr;

    if (l2sp_verts > 0) {
        const size_t bytes = (size_t)l2sp_verts * sizeof(int64_t);
        g_current_frontier_storage =
            (int64_t *)try_alloc_l2sp(&l2sp_heap, bytes, alignof(int64_t), l2sp_limit);
        g_next_frontier.l2sp_items =
            (int64_t *)try_alloc_l2sp(&l2sp_heap, bytes, alignof(int64_t), l2sp_limit);
        g_frontiers_in_l2sp = 1;
    }
    if (dram_verts > 0) {
        const size_t bytes = (size_t)dram_verts * sizeof(int64_t);
        g_current_frontier_dram_storage = (int64_t *)std::malloc(bytes);
        g_next_frontier.dram_items = (int64_t *)std::malloc(bytes);
    }
    g_next_frontier.tail = 0;
    g_next_frontier.capacity = hdr_N;

    const bool missing_l2sp =
        l2sp_verts > 0 && (!g_current_frontier_storage || !g_next_frontier.l2sp_items);
    const bool missing_dram =
        dram_verts > 0 && (!g_current_frontier_dram_storage || !g_next_frontier.dram_items);
    if (missing_l2sp || missing_dram) {
        std::printf("ERROR: frontier buffer allocation failed\n");
        std::free(buf);
        if (!g_visited_in_l2sp) std::free((void *)visited);
        if (!g_dist_in_l2sp) std::free(dist_arr);
        std::free(g_current_frontier_dram_storage);
        std::free(g_next_frontier.dram_items);
        return false;
    }

    std::printf("Graph loaded: N=%d E=%d D=%d source=%d max_deg=%d\n",
                hdr_N, hdr_E, hdr_D, g_bfs_source, max_deg);
    return true;
}

// =====================================================================
// BFS level processing (NO work stealing — local queue only)
// =====================================================================
static void process_bfs_level(int tid, int32_t level) {
    const int my_core = tid / g_harts_per_core;
    WorkQueue* my_queue = &core_queues[my_core];

    int64_t local_nodes = 0;
    int64_t local_edges = 0;

    for (;;) {
        int64_t begin_idx;
        int64_t count = queue_pop_chunk(my_queue, my_core, &begin_idx, BFS_CHUNK_SIZE);

        if (count > 0) {
            for (int64_t i = 0; i < count; i++) {
                int64_t u = frontier_current_get(my_queue->start_idx + begin_idx + i);
                const int32_t rs = g_row_ptr[u];
                const int32_t re = g_row_ptr[u + 1];
                local_edges += (re - rs);
                for (int32_t ei = rs; ei < re; ei++) {
                    const int64_t v = g_col_idx[ei];
                    if (claim_node(v)) {
                        dist_arr[v] = level + 1;
                        next_frontier_push_atomic(v);
                        atomic_fetch_add_i64(&discovered, 1);
                    }
                }
                local_nodes++;
            }
            continue;
        }

        // Verify truly empty (not CAS contention)
        int64_t h = atomic_load_i64(&my_queue->head);
        int64_t t = atomic_load_i64(&my_queue->tail);
        if (h < t) continue;
        break;
    }

    atomic_fetch_add_i64(&stat_nodes_per_core[my_core], local_nodes);
    atomic_fetch_add_i64(&stat_edges_per_core[my_core], local_edges);
    stat_nodes_processed[tid] += local_nodes;
}

// =====================================================================
// ALL-TO-CORE-0 frontier distribution (extreme power-law imbalance)
//
// ALL frontier vertices are assigned to core 0.
// This models the worst-case power-law partitioning where a naive
// vertex-ownership scheme assigns essentially all hub-connected work
// to a single core. Other cores sit idle.
// =====================================================================
static void advance_to_next_level_imbalanced(int tid) {
    if (tid != 0) return;

    const int total_cores = g_total_cores;
    const int64_t total_nodes = g_next_frontier.tail;

    if (total_nodes == 0) {
        for (int c = 0; c < total_cores; c++) {
            queue_init(&core_queues[c]);
            core_has_work[c] = 0;
        }
        g_next_frontier.tail = 0;
        return;
    }

    // Copy ALL nodes into current frontier starting at offset 0
    for (int64_t i = 0; i < total_nodes; i++) {
        frontier_current_set(i, frontier_next_item_get(i));
    }

    // ALL work goes to core 0
    queue_assign_slice(&core_queues[0], 0, total_nodes);
    core_has_work[0] = 1;
    for (int c = 1; c < total_cores; c++) {
        queue_assign_slice(&core_queues[c], total_nodes, 0);
        core_has_work[c] = 0;
    }

    g_next_frontier.tail = 0;
}

// =====================================================================
// Count total frontier work
// =====================================================================
static int64_t count_total_work() {
    int64_t total = 0;
    for (int c = 0; c < g_total_cores; c++) {
        int64_t d = core_queues[c].tail - core_queues[c].head;
        if (d > 0) total += d;
    }
    return total;
}

// =====================================================================
// BFS driver
// =====================================================================
static void bfs() {
    const int tid = get_thread_id();

    if (tid == 0) {
        if (!load_graph_from_file()) {
            std::abort();
        }

        const int32_t num_verts = g_num_vertices;
        const int64_t source_id = g_bfs_source;

        for (int64_t i = 0; i < num_verts; i++) {
            visited[i] = 0;
            dist_arr[i] = -1;
        }
        for (int i = 0; i < g_total_harts; i++) {
            g_local_sense[i] = 0;
            stat_nodes_processed[i] = 0;
        }
        for (int c = 0; c < g_total_cores; c++) {
            queue_init(&core_queues[c]);
            core_has_work[c] = 0;
            stat_nodes_per_core[c] = 0;
            stat_edges_per_core[c] = 0;
        }
        g_next_frontier.tail = 0;

        visited[source_id] = 1;
        dist_arr[source_id] = 0;
        discovered = 1;

        // Source goes to core 0 (vertex-ownership assigns it there)
        queue_assign_slice(&core_queues[0], 0, 1);
        frontier_current_set(0, source_id);
        core_has_work[0] = 1;
        for (int c = 1; c < g_total_cores; c++) {
            queue_assign_slice(&core_queues[c], 1, 0);
        }

        std::printf("=== BFS Imbalanced Baseline (NO stealing; all-to-core-0) ===\n");
        std::printf("Graph: N=%d E=%d (RMAT CSR)\n",
                    g_num_vertices, g_num_edges);
        std::printf("Distribution: ALL frontier to core 0 [extreme imbalance]\n");
        std::printf("Hardware: %d cores x %d harts = %d total harts\n",
                    g_total_cores, g_harts_per_core, g_total_harts);
        std::printf("Source: node %ld (highest-degree vertex)\n", (long)source_id);
        std::printf("Hot state: visited in %s, dist_arr in %s, frontiers in %s\n",
                    g_visited_in_l2sp ? "L2SP" : "DRAM",
                    g_dist_in_l2sp ? "L2SP" : "DRAM",
                    g_frontiers_in_l2sp ? "L2SP+DRAM" : "DRAM");
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

        barrier();

        if (total_work == 0) break;

        if (tid == 0) {
            std::printf("Level %d: frontier=%ld, discovered=%ld\n",
                        level, (long)total_work, (long)discovered);
            std::printf("  Per-core queues: ");
            for (int c = 0; c < g_total_cores; c++) {
                int64_t d = core_queues[c].tail - core_queues[c].head;
                if (d < 0) d = 0;
                std::printf("C%d:%ld ", c, (long)d);
            }
            std::printf("\n");
        }

        barrier();

        process_bfs_level(tid, level);

        barrier();

        advance_to_next_level_imbalanced(tid);

        barrier();
        level++;
    }

    uint64_t end_cycles = 0;
    if (tid == 0) {
        asm volatile("rdcycle %0" : "=r"(end_cycles));
    }

    barrier();

    if (tid == 0) {
        const uint64_t elapsed = end_cycles - start_cycles;

        std::printf("\n=== BFS Complete ===\n");
        std::printf("Levels traversed: %d\n", level);
        std::printf("Nodes discovered: %ld / %d\n", (long)discovered, g_num_vertices);
        std::printf("Cycles elapsed:   %lu\n", (unsigned long)elapsed);
        if (discovered > 0) {
            std::printf("Cycles per node:  %lu\n",
                        (unsigned long)(elapsed / (uint64_t)discovered));
        }

        // Per-core work balance report
        std::printf("\n========== WORK BALANCE (per-core) ==========\n");
        std::printf("Core | Nodes Processed | Edges Traversed\n");
        std::printf("-----|-----------------|----------------\n");

        int64_t total_nodes_done = 0;
        int64_t total_edges_done = 0;
        int64_t min_edges = INT64_MAX;
        int64_t max_edges = 0;

        for (int c = 0; c < g_total_cores; c++) {
            int64_t nodes = stat_nodes_per_core[c];
            int64_t edges = stat_edges_per_core[c];
            std::printf("  %2d | %15ld | %15ld\n", c, (long)nodes, (long)edges);
            total_nodes_done += nodes;
            total_edges_done += edges;
            if (edges < min_edges) min_edges = edges;
            if (edges > max_edges) max_edges = edges;
        }

        int64_t avg_edges = (g_total_cores > 0) ? (total_edges_done / g_total_cores) : 0;
        int64_t imbalance_pct = (max_edges > 0)
            ? ((max_edges - min_edges) * 100 / max_edges) : 0;

        std::printf("\nSummary:\n");
        std::printf("  Total nodes: %ld  Total edges: %ld\n",
                    (long)total_nodes_done, (long)total_edges_done);
        std::printf("  Avg edges/core: %ld\n", (long)avg_edges);
        std::printf("  Min/Max edges:  %ld / %ld\n", (long)min_edges, (long)max_edges);
        std::printf("  Imbalance (max-min)/max: %ld%%\n", (long)imbalance_pct);
        std::printf("=============================================\n");

        // Cleanup
        std::free(g_file_buffer);
        if (!g_visited_in_l2sp) std::free((void *)visited);
        if (!g_dist_in_l2sp) std::free(dist_arr);
        std::free(g_current_frontier_dram_storage);
        std::free(g_next_frontier.dram_items);
        g_file_buffer = nullptr;
        g_row_ptr     = nullptr;
        g_col_idx     = nullptr;
    }
}

int main(int argc, char** argv) {
    const int tid = get_thread_id();

    if (tid == 0) {
        g_total_cores = numPodCores();
        g_harts_per_core = myCoreThreads();
        g_total_harts = g_total_cores * g_harts_per_core;
    }

    barrier();
    bfs();
    barrier();

    return 0;
}
