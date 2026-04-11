// Level-synchronous BFS baseline on CSR graph with NO work stealing.
// Matches bfs_work_stealing for:
// - graph input format
// - source selection (highest-degree vertex)
// - first-frontier skew toward core 0
// and differs only by prohibiting inter-core stealing.

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

static constexpr int QUEUE_SIZE = 512;
static constexpr int MAX_HARTS  = 2048;
static constexpr int MAX_CORES  = 128;
static constexpr int BFS_CHUNK_SIZE = 64;
static constexpr int INITIAL_SKEW_WEIGHT = 32;

__l2sp__ volatile int g_total_harts = 0;
__l2sp__ volatile int g_harts_per_core = 0;
__l2sp__ volatile int g_total_cores = 0;
__l2sp__ std::atomic<int> g_cfg_ready = 0;

struct WorkQueue {
    volatile int64_t head;
    volatile int64_t tail;
    volatile int64_t items[QUEUE_SIZE];
};

__l2sp__ WorkQueue core_queues[MAX_CORES];
__l2sp__ WorkQueue next_level_queues[MAX_CORES];

__l2sp__ int64_t g_local_sense[MAX_HARTS];
__l2sp__ std::atomic<int64_t> g_count = 0;
__l2sp__ std::atomic<int64_t> g_sense = 0;
__l2sp__ volatile int64_t g_total_work_bcast = 0;

__l2sp__ int32_t *g_row_ptr    = nullptr;
__l2sp__ int32_t *g_col_idx    = nullptr;
__l2sp__ volatile int64_t *visited = nullptr;
__l2sp__ int32_t *dist_arr     = nullptr;
__l2sp__ char    *g_file_buffer = nullptr;
__l2sp__ int32_t  g_num_vertices = 0;
__l2sp__ int32_t  g_num_edges    = 0;
__l2sp__ int32_t  g_bfs_source   = 0;
__l2sp__ int32_t  g_dist_in_l2sp = 0;
__l2sp__ int32_t  g_visited_in_l2sp = 0;

__l2sp__ volatile int64_t nodes_processed[MAX_HARTS];
__l2sp__ volatile int64_t discovered = 0;

static inline int get_thread_id() {
    return myCoreId() * (int)g_harts_per_core + myThreadId();
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

static inline void queue_init(WorkQueue* q) {
    q->head = 0;
    q->tail = 0;
}

static inline bool queue_push_st(WorkQueue* q, int64_t work) {
    int64_t t = q->tail;
    if (t >= QUEUE_SIZE) return false;
    q->items[t] = work;
    q->tail = t + 1;
    return true;
}

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

static inline int64_t queue_pop_chunk_mc(WorkQueue* q, int64_t* out_begin, int64_t max_chunk) {
    int64_t backoff = 1;
    const int64_t max_backoff = 32;

    while (true) {
        int64_t h = atomic_load_i64(&q->head);
        int64_t t = atomic_load_i64(&q->tail);
        if (h >= t) return 0;

        int64_t avail = t - h;
        int64_t chunk = (avail < max_chunk) ? avail : max_chunk;
        int64_t old_h = atomic_compare_and_swap_i64(&q->head, h, h + chunk);
        if (old_h == h) {
            *out_begin = h;
            return chunk;
        }

        hartsleep(backoff);
        if (backoff < max_backoff) backoff <<= 1;
    }
}

static inline bool claim_node(int64_t v) {
    const int64_t old = atomic_swap_i64((volatile int64_t *)&visited[v], 1);
    return old == 0;
}

extern "C" char l2sp_end[];

static inline uintptr_t align_up_uintptr(uintptr_t value, size_t align) {
    const uintptr_t mask = (uintptr_t)align - 1;
    return (value + mask) & ~mask;
}

static void* try_alloc_l2sp(uintptr_t* heap, size_t bytes, size_t align, uintptr_t l2sp_limit) {
    uintptr_t ptr = align_up_uintptr(*heap, align);
    if (ptr + bytes > l2sp_limit) return nullptr;
    *heap = ptr + bytes;
    return (void*)ptr;
}

static bool load_graph_from_file() {
    char fname[32];
    const char *src = "rmat_s16.bin";
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

    if (hdr_N <= 0 || hdr_E < 0) {
        std::printf("ERROR: invalid CSR header in %s: N=%d E=%d D=%d\n",
                    src, hdr_N, hdr_E, hdr_D);
        return false;
    }

    const size_t file_size = 20
        + (size_t)(hdr_N + 1) * sizeof(int32_t)
        + (size_t)hdr_E * sizeof(int32_t)
        + (size_t)hdr_N * sizeof(int32_t);

    char *buf = (char *)std::malloc(file_size);
    if (!buf) {
        std::printf("ERROR: malloc failed for file buffer (%lu bytes)\n",
                    (unsigned long)file_size);
        return false;
    }

    struct ph_bulk_load_desc desc;
    desc.filename_addr = (long)fname;
    desc.dest_addr     = (long)buf;
    desc.size          = (long)file_size;
    desc.result        = 0;
    ph_bulk_load_file(&desc);

    if (desc.result <= 0) {
        std::printf("ERROR: bulk load of %s failed (result=%ld)\n", src, desc.result);
        std::free(buf);
        return false;
    }

    g_file_buffer  = buf;
    g_num_vertices = hdr_N;
    g_num_edges    = hdr_E;
    g_row_ptr = (int32_t *)(buf + 20);
    g_col_idx = (int32_t *)(buf + 20 + (size_t)(hdr_N + 1) * sizeof(int32_t));

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

    const size_t visited_bytes = (size_t)hdr_N * sizeof(int64_t);
    const size_t dist_bytes = (size_t)hdr_N * sizeof(int32_t);
    uintptr_t l2sp_heap = align_up_uintptr((uintptr_t)l2sp_end, 8);
    const uintptr_t l2sp_base = 0x20000000;
    const uintptr_t l2sp_limit = l2sp_base + podL2SPSize();

    g_visited_in_l2sp = 0;
    g_dist_in_l2sp = 0;

    visited = (volatile int64_t *)try_alloc_l2sp(&l2sp_heap, visited_bytes, alignof(int64_t), l2sp_limit);
    if (visited != nullptr) {
        g_visited_in_l2sp = 1;
    } else {
        visited = (volatile int64_t *)std::malloc(visited_bytes);
    }

    dist_arr = (int32_t *)try_alloc_l2sp(&l2sp_heap, dist_bytes, alignof(int32_t), l2sp_limit);
    if (dist_arr != nullptr) {
        g_dist_in_l2sp = 1;
    } else {
        dist_arr = (int32_t *)std::malloc(dist_bytes);
    }
    if (!visited || !dist_arr) {
        std::printf("ERROR: malloc failed for visited/dist_arr\n");
        std::free(buf);
        if (!g_visited_in_l2sp) std::free((void *)visited);
        if (!g_dist_in_l2sp) std::free(dist_arr);
        g_file_buffer = nullptr;
        visited  = nullptr;
        dist_arr = nullptr;
        g_visited_in_l2sp = 0;
        g_dist_in_l2sp = 0;
        return false;
    }

    std::printf("Graph loaded: N=%d E=%d D=%d source=%d max_deg=%d (%lu bytes)\n",
                hdr_N, hdr_E, hdr_D, g_bfs_source, max_deg, (unsigned long)file_size);
    return true;
}

static inline void process_single_node(int64_t u, int32_t level, WorkQueue* my_next) {
    const int32_t row_start = g_row_ptr[u];
    const int32_t row_end = g_row_ptr[u + 1];
    for (int32_t ei = row_start; ei < row_end; ei++) {
        const int64_t v = g_col_idx[ei];
        if (claim_node(v)) {
            dist_arr[v] = level + 1;
            queue_push_mp(my_next, v);
            atomic_fetch_add_i64(&discovered, 1);
        }
    }
}

static void process_bfs_level(int tid, int32_t level) {
    const int my_core = tid / g_harts_per_core;
    WorkQueue* my_q = &core_queues[my_core];
    WorkQueue* my_next = &next_level_queues[my_core];

    int64_t local_processed = 0;
    while (true) {
        int64_t begin_idx;
        int64_t count = queue_pop_chunk_mc(my_q, &begin_idx, BFS_CHUNK_SIZE);
        if (count == 0) break;

        for (int64_t i = 0; i < count; i++) {
            int64_t u = my_q->items[begin_idx + i];
            local_processed++;
            process_single_node(u, level, my_next);
        }
    }

    nodes_processed[tid] += local_processed;
}

static void distribute_frontier_imbalanced(int tid, bool skew_initial_frontier) {
    if (tid != 0) return;

    const int total_cores = g_total_cores;
    int64_t total_nodes = 0;
    for (int c = 0; c < total_cores; c++) {
        total_nodes += (next_level_queues[c].tail - next_level_queues[c].head);
    }

    for (int c = 0; c < total_cores; c++) {
        queue_init(&core_queues[c]);
    }

    if (total_nodes == 0) {
        for (int c = 0; c < total_cores; c++) queue_init(&next_level_queues[c]);
        return;
    }

    int weights[MAX_CORES];
    int64_t quotas[MAX_CORES];
    int sum_w = 0;
    for (int c = 0; c < total_cores; c++) {
        weights[c] = (c & 1) ? 2 : 1;
        if (skew_initial_frontier && c == 0) {
            weights[c] = INITIAL_SKEW_WEIGHT;
        }
        sum_w += weights[c];
    }

    int64_t assigned = 0;
    for (int c = 0; c < total_cores; c++) {
        quotas[c] = (total_nodes * (int64_t)weights[c]) / (int64_t)sum_w;
        assigned += quotas[c];
    }

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

        std::printf("=== BFS Baseline (NO stealing; CSR graph; skewed initial frontier) ===\n");
        std::printf("Graph: N=%d E=%d (bulk-loaded from uniform_graph.bin)\n",
                    g_num_vertices, g_num_edges);
        std::printf("Hardware: %d cores x %d harts = %d total harts\n",
                    g_total_cores, g_harts_per_core, g_total_harts);
        std::printf("Source: node %ld (highest-degree vertex)\n", (long)source_id);
        std::printf("Hot state: visited in %s, dist_arr in %s\n",
                    g_visited_in_l2sp ? "L2SP" : "DRAM",
                    g_dist_in_l2sp ? "L2SP" : "DRAM");
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
        distribute_frontier_imbalanced(tid, level == 0);
        barrier();
        level++;
    }

    uint64_t end_cycles = 0;
    if (tid == 0) asm volatile("rdcycle %0" : "=r"(end_cycles));

    barrier();

    if (tid == 0) {
        std::printf("\n=== BFS Complete ===\n");
        std::printf("Levels traversed: %d\n", level);
        std::printf("Nodes discovered: %ld / %d\n", (long)discovered, g_num_vertices);
        std::printf("dist[source=%d] = %d (expected 0)\n",
                    g_bfs_source, dist_arr[g_bfs_source]);

        int64_t total_processed = 0;
        for (int h = 0; h < g_total_harts; h++) total_processed += nodes_processed[h];
        std::printf("Total nodes processed: %ld\n", (long)total_processed);

        const uint64_t elapsed = end_cycles - start_cycles;
        std::printf("Cycles elapsed:        %lu\n", (unsigned long)elapsed);
        if (total_processed > 0) {
            std::printf("Cycles per node:       %lu\n",
                        (unsigned long)(elapsed / (uint64_t)total_processed));
        }

        std::free(g_file_buffer);
        if (!g_visited_in_l2sp) std::free((void *)visited);
        if (!g_dist_in_l2sp) std::free(dist_arr);
        g_file_buffer = nullptr;
        g_row_ptr = nullptr;
        g_col_idx = nullptr;
        visited = nullptr;
        dist_arr = nullptr;
        g_visited_in_l2sp = 0;
        g_dist_in_l2sp = 0;
    }
}

int main(int argc, char** argv) {
    if (myCoreId() == 0 && myThreadId() == 0) {
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

        g_cfg_ready.store(1, std::memory_order_release);
    } else {
        while (g_cfg_ready.load(std::memory_order_acquire) == 0) {
            hartsleep(10);
        }
    }

    bfs();
    return 0;
}
