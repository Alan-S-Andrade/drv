// bfs_csr_shared_queue.cpp
// Level-synchronous CSR BFS with shared-queue load balancing (ROI-only).
//
// Per-vertex frontier with shared queue (contrast with bitmap + work-stealing):
//   - Frontier stored as explicit vertex list, tiered L2SP -> L1SP -> DRAM
//   - ALL harts CAS-pop vertex indices from ONE shared queue
//   - Whoever finishes first grabs more work — natural load balancing
//   - No per-core queues, no steal logic, no victim selection
//   - Discovery via CAS on dist[] (no separate visited array)
//   - Next frontier built via atomic tail + batched local buffer flush
//
// Memory layout:
//   L2SP: shared queue head/tail, frontier storage (hot entries), barrier, stats
//   L1SP: frontier overflow (distributed across cores, absolute addressing)
//   DRAM: CSR graph (row_ptr, col_idx), dist[], frontier overflow
//
// Graph bulk-loaded from file via MMIO (same as bfs_csr_work_stealing.cpp).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>

#include <pandohammer/cpuinfo.h>
#include <pandohammer/atomic.h>
#include <pandohammer/hartsleep.h>
#include <pandohammer/mmio.h>
#include <pandohammer/address.h>
#include <pandohammer/staticdecl.h>

#include "work_stealing.h"   // reuse barrier from ws::

// ---------- Configuration ----------
#ifndef DEFAULT_VTX_PER_THREAD
#define DEFAULT_VTX_PER_THREAD 1024
#endif
#ifndef DEFAULT_DEGREE
#define DEFAULT_DEGREE 16
#endif

static constexpr int MAX_HARTS = 1024;
static constexpr int MAX_CORES = 64;
static constexpr int BFS_CHUNK_SIZE = 8;      // Vertices per CAS pop batch
static constexpr int LOCAL_BUF_SIZE = 32;     // Per-hart discovery buffer (flushed in batch)

// L1SP data region layout
static constexpr uintptr_t L1SP_DATA_START = 16;     // After alignment padding
static constexpr uintptr_t L1SP_STACK_GUARD = 5120;  // 5KB guard for stack growth

// ---------- Shared Queue ----------
// Simple head/tail into double-buffered frontier storage.
// All harts CAS-pop from head. No separate item array needed.
struct SharedQueue {
    volatile int64_t head;
    volatile int64_t tail;
};

// ---------- Helpers ----------
static int parse_i(const char *s, int d)
{
    if (!s) return d;
    char *e = nullptr;
    long v = strtol(s, &e, 10);
    return (e && *e == 0) ? (int)v : d;
}

static void bulk_load(const char *name, void *dest, size_t size)
{
    char fname_buf[64];
    int fi = 0;
    while (name[fi] && fi < 63) { fname_buf[fi] = name[fi]; fi++; }
    fname_buf[fi] = '\0';

    struct ph_bulk_load_desc desc;
    desc.filename_addr = (long)fname_buf;
    desc.dest_addr     = (long)dest;
    desc.size          = (long)size;
    desc.result        = 0;
    ph_bulk_load_file(&desc);

    if (desc.result <= 0) {
        std::printf("ERROR: bulk load '%s' failed (result=%ld)\n", name, desc.result);
    }
}

static inline int32_t floor_pow2(int32_t n) {
    if (n <= 0) return 0;
    int32_t p = 1;
    while (p * 2 <= n) p *= 2;
    return p;
}

static inline int32_t ilog2_pow2(int32_t n) {
    int32_t k = 0;
    while ((1 << (k + 1)) <= n) k++;
    return k;
}

// ---------- L2SP Globals ----------

// Runtime config
__l2sp__ volatile int g_total_harts = 0;
__l2sp__ volatile int g_harts_per_core = 0;
__l2sp__ volatile int g_total_cores = 0;
__l2sp__ std::atomic<int> g_initialized = 0;

// Barrier
__l2sp__ ws::BarrierState<MAX_HARTS> g_barrier;

// Shared queue (indexes into current frontier buffer)
__l2sp__ SharedQueue shared_queue;

// Graph parameters
__l2sp__ int32_t g_N;
__l2sp__ int32_t g_degree;
__l2sp__ int32_t g_source;

// DRAM pointers (stored in L2SP for fast access)
__l2sp__ char    *g_file_buffer;
__l2sp__ int32_t *g_csr_offsets;
__l2sp__ int32_t *g_csr_edges;
__l2sp__ int32_t *g_dist;

// Control
__l2sp__ volatile int g_sim_exit;

// Reduction accumulators
__l2sp__ volatile int64_t g_sum_dist;
__l2sp__ volatile int32_t g_reached;
__l2sp__ volatile int32_t g_max_dist;

// Per-hart / per-core stats
__l2sp__ volatile int64_t stat_nodes_processed[MAX_HARTS];
__l2sp__ volatile int64_t stat_edges_per_core[MAX_CORES];
__l2sp__ volatile int64_t stat_nodes_per_core[MAX_CORES];

// ---------- Double-buffered frontier (L2SP -> L1SP -> DRAM) ----------
// Two buffers: cur_buf and cur_buf^1.  Swapped each level (no data copy).
__l2sp__ volatile int cur_buf;                  // 0 or 1
__l2sp__ volatile int64_t frontier_tail[2];    // next-push index per buffer

// L2SP tier
__l2sp__ int64_t *frontier_l2sp[2];
__l2sp__ int64_t frontier_l2sp_cap;            // entries per buffer in L2SP

// L1SP tier (distributed across cores via absolute addressing)
__l2sp__ uintptr_t g_l1sp_abs_base[MAX_CORES]; // absolute base per core
__l2sp__ int32_t g_l1sp_data_bytes_per_core;
__l2sp__ int64_t frontier_l1sp_cap;            // entries per buffer total
__l2sp__ int32_t frontier_l1sp_per_core;       // entries per core (power of 2)
__l2sp__ int32_t frontier_l1sp_shift;          // log2(per_core)
__l2sp__ uintptr_t frontier_l1sp_offset[2];    // byte offset within each core's L1SP

// DRAM tier
__l2sp__ int64_t *frontier_dram[2];
__l2sp__ int64_t frontier_total_cap;           // total entries per buffer (L2SP+L1SP+DRAM)

// L2SP heap boundary
extern "C" char l2sp_end[];

// ---------- Thread ID + Barrier ----------
static inline int get_thread_id() {
    return myCoreId() * (int)g_harts_per_core + myThreadId();
}

static inline void barrier() {
    ws::barrier(&g_barrier, get_thread_id(), g_total_harts);
}

// ---------- L1SP Data Region Setup ----------
static inline void init_l1sp_data_regions() {
    for (int c = 0; c < g_total_cores; c++) {
        uintptr_t addr = 0;
        addr = ph_address_set_absolute(addr, 1);
        addr = ph_address_absolute_set_core(addr, (uintptr_t)c);
        addr = ph_address_absolute_set_l1sp_offset(addr, 0);
        g_l1sp_abs_base[c] = addr;
    }

    uint64_t core_l1sp = coreL1SPSize();
    uint64_t hart_cap = core_l1sp / (uint64_t)g_harts_per_core;
    if (hart_cap > L1SP_DATA_START + L1SP_STACK_GUARD) {
        g_l1sp_data_bytes_per_core = (int32_t)(hart_cap - L1SP_STACK_GUARD - L1SP_DATA_START);
    } else {
        g_l1sp_data_bytes_per_core = 0;
    }
}

// ---------- Frontier Accessors (L2SP -> L1SP -> DRAM) ----------
static inline int64_t* frontier_ptr(int buf, int64_t idx) {
    if (idx < frontier_l2sp_cap)
        return frontier_l2sp[buf] + idx;
    int64_t r = idx - frontier_l2sp_cap;
    if (r < frontier_l1sp_cap) {
        int core = (int)(r >> frontier_l1sp_shift);
        int local = (int)(r & ((1 << frontier_l1sp_shift) - 1));
        return (int64_t*)(g_l1sp_abs_base[core] + frontier_l1sp_offset[buf]) + local;
    }
    return frontier_dram[buf] + (r - frontier_l1sp_cap);
}

static inline int64_t frontier_get(int buf, int64_t idx) {
    return *frontier_ptr(buf, idx);
}

static inline void frontier_set(int buf, int64_t idx, int64_t val) {
    *frontier_ptr(buf, idx) = val;
}

// ---------- Shared Queue Fetch-Add Pop ----------
// All harts pop batches from the shared queue (indexes into current frontier).
// Uses fetch-and-add instead of CAS to eliminate retry-induced contention.
static inline int64_t sq_pop_chunk(int64_t *out_buf, int64_t max_chunk) {
    // TTAS: volatile pre-check to avoid unnecessary atomics
    int64_t h = shared_queue.head;
    int64_t t = shared_queue.tail;
    if (h >= t) return 0;

    // Fetch-and-add: always succeeds, no retries
    int64_t old_h = atomic_fetch_add_i64(&shared_queue.head, max_chunk);
    t = atomic_load_i64(&shared_queue.tail);

    if (old_h >= t) return 0;  // overshot, no work left

    // Clamp chunk to available work
    int64_t k = max_chunk;
    if (old_h + k > t) k = t - old_h;

    // Read vertex IDs from current frontier buffer
    for (int64_t i = 0; i < k; i++) {
        out_buf[i] = frontier_get(cur_buf, old_h + i);
    }
    return k;
}

// ---------- Next Frontier Batch Push ----------
// Flush local discovery buffer to next frontier in one atomic tail bump.
static inline void flush_discoveries(int64_t *buf, int count, int next_buf) {
    if (count <= 0) return;
    int64_t base = atomic_fetch_add_i64(&frontier_tail[next_buf], (int64_t)count);
    if (base + count > frontier_total_cap) {
        std::printf("ERROR: frontier overflow at index %ld (cap=%ld)\n",
                    (long)(base + count), (long)frontier_total_cap);
        return;
    }
    for (int i = 0; i < count; i++) {
        frontier_set(next_buf, base + i, buf[i]);
    }
}

// ---------- BFS Level Processing ----------
static void process_bfs_level(int tid, int32_t level)
{
    const int my_core = tid / g_harts_per_core;
    const int next_buf = cur_buf ^ 1;

    int32_t *offsets = g_csr_offsets;
    int32_t *edges   = g_csr_edges;
    int32_t *dist    = g_dist;

    int64_t local_processed = 0;
    int64_t local_edges = 0;
    int64_t chunk_buf[BFS_CHUNK_SIZE];

    // Per-hart local buffer for batching next-frontier pushes
    int64_t disc_buf[LOCAL_BUF_SIZE];
    int disc_count = 0;

    while (true) {
        int64_t count = sq_pop_chunk(chunk_buf, BFS_CHUNK_SIZE);

        if (count > 0) {
            for (int64_t i = 0; i < count; i++) {
                int32_t u = (int32_t)chunk_buf[i];
                local_processed++;

                int32_t edge_begin = offsets[u];
                int32_t edge_end   = offsets[u + 1];
                local_edges += (edge_end - edge_begin);

                for (int32_t ei = edge_begin; ei < edge_end; ei++) {
                    int32_t v = edges[ei];
                    if (atomic_compare_and_swap_i32(&dist[v], -1, level + 1) == -1) {
                        disc_buf[disc_count++] = (int64_t)v;
                        if (disc_count == LOCAL_BUF_SIZE) {
                            flush_discoveries(disc_buf, disc_count, next_buf);
                            disc_count = 0;
                        }
                    }
                }
            }
        } else {
            // Flush pending before checking termination
            if (disc_count > 0) {
                flush_discoveries(disc_buf, disc_count, next_buf);
                disc_count = 0;
            }
            // Confirm queue is truly empty
            int64_t h = atomic_load_i64(&shared_queue.head);
            int64_t t = atomic_load_i64(&shared_queue.tail);
            if (h >= t) break;
            hartsleep(1);  // race window, retry
        }
    }

    // Final flush
    if (disc_count > 0) {
        flush_discoveries(disc_buf, disc_count, next_buf);
    }

    stat_nodes_processed[tid] += local_processed;
    atomic_fetch_add_i64(&stat_nodes_per_core[my_core], local_processed);
    atomic_fetch_add_i64(&stat_edges_per_core[my_core], local_edges);
}

// ---------- Frontier Memory Allocation (L2SP -> L1SP -> DRAM) ----------
// Allocates double-buffered frontier storage across memory tiers.
// Returns false on allocation failure.
static bool alloc_frontier_storage(uintptr_t *l2sp_heap, uintptr_t l2sp_limit,
                                   uintptr_t *l1sp_heap_off, uintptr_t l1sp_data_end,
                                   int32_t N)
{
    frontier_l2sp_cap = 0;
    frontier_l1sp_cap = 0;
    frontier_l1sp_per_core = 0;
    frontier_l1sp_shift = 0;
    frontier_total_cap = N;
    frontier_l2sp[0] = nullptr;
    frontier_l2sp[1] = nullptr;
    frontier_dram[0] = nullptr;
    frontier_dram[1] = nullptr;

    // L2SP tier: as many entries as fit (split between 2 buffers)
    {
        uintptr_t heap = (*l2sp_heap + 7) & ~(uintptr_t)7;
        uintptr_t avail = (heap < l2sp_limit) ? (l2sp_limit - heap) : 0;
        // Each entry = int64_t, need 2 buffers
        int64_t max_per_buf = (int64_t)(avail / (2 * sizeof(int64_t)));
        if (max_per_buf > N) max_per_buf = N;

        if (max_per_buf > 0) {
            size_t bytes = (size_t)max_per_buf * sizeof(int64_t);
            frontier_l2sp[0] = (int64_t *)heap;
            heap += bytes;
            heap = (heap + 7) & ~(uintptr_t)7;
            frontier_l2sp[1] = (int64_t *)heap;
            heap += bytes;
            heap = (heap + 7) & ~(uintptr_t)7;
            frontier_l2sp_cap = max_per_buf;
            *l2sp_heap = heap;
        }
    }

    int64_t remaining = N - frontier_l2sp_cap;

    // L1SP tier: distributed across cores (power-of-2 per core for fast indexing)
    if (remaining > 0 && *l1sp_heap_off < l1sp_data_end) {
        int32_t avail_bytes = (int32_t)(l1sp_data_end - *l1sp_heap_off);
        // Need space for 2 buffers in each core's L1SP
        int32_t entries_raw = avail_bytes / (int32_t)(2 * sizeof(int64_t));
        int32_t epc = floor_pow2(entries_raw);  // entries per core per buffer
        if (epc > 0) {
            int64_t total = (int64_t)epc * g_total_cores;
            if (total > remaining) {
                epc = floor_pow2((int32_t)(remaining / g_total_cores));
                total = (int64_t)epc * g_total_cores;
            }
            if (epc > 0) {
                frontier_l1sp_cap = total;
                frontier_l1sp_per_core = epc;
                frontier_l1sp_shift = ilog2_pow2(epc);
                frontier_l1sp_offset[0] = *l1sp_heap_off;
                frontier_l1sp_offset[1] = *l1sp_heap_off + (uintptr_t)epc * sizeof(int64_t);
                *l1sp_heap_off += (uintptr_t)epc * 2 * sizeof(int64_t);
            }
        }
        remaining = N - frontier_l2sp_cap - frontier_l1sp_cap;
    }

    // DRAM tier: everything else
    if (remaining > 0) {
        size_t bytes = (size_t)remaining * sizeof(int64_t);
        frontier_dram[0] = (int64_t *)std::malloc(bytes);
        frontier_dram[1] = (int64_t *)std::malloc(bytes);
        if (!frontier_dram[0] || !frontier_dram[1]) {
            std::printf("ERROR: malloc failed for frontier DRAM (%lu bytes x 2)\n",
                        (unsigned long)bytes);
            std::free(frontier_dram[0]);
            std::free(frontier_dram[1]);
            return false;
        }
    }

    return true;
}

// ---------- Main ----------
extern "C" int main(int argc, char **argv)
{
    int vtx_per_thread = DEFAULT_VTX_PER_THREAD;
    int degree         = DEFAULT_DEGREE;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--V") && i + 1 < argc)
            vtx_per_thread = parse_i(argv[++i], vtx_per_thread);
        else if (!strcmp(argv[i], "--D") && i + 1 < argc)
            degree = parse_i(argv[++i], degree);
        else if (!strcmp(argv[i], "--help")) {
            std::printf("Usage: %s --V <vtx/thread> --D <degree>\n", argv[0]);
            return 0;
        }
    }

    // ---- Hardware topology ----
    const int hart_in_core   = myThreadId();
    const int core_in_pod    = myCoreId();
    const int pod_in_pxn     = myPodId();
    const int pxn_id         = myPXNId();
    const int harts_per_core = myCoreThreads();
    const int cores_per_pod  = numPodCores();
    const int pods_per_pxn   = numPXNPods();
    const int threads_per_pod = cores_per_pod * harts_per_core;

    const int total_harts_hw =
        numPXN() * pods_per_pxn * cores_per_pod * harts_per_core;

    // Only run on pod 0, pxn 0
    if (pxn_id != 0 || pod_in_pxn != 0) {
        while (g_sim_exit == 0) hartsleep(1000);
        return 0;
    }

    const int tid = core_in_pod * harts_per_core + hart_in_core;

    if (tid >= threads_per_pod) {
        while (g_sim_exit == 0) hartsleep(1000);
        return 0;
    }

    // ================================================================
    // Hart 0: load graph, allocate structures
    // ================================================================
    if (tid == 0) {
        g_total_cores    = cores_per_pod;
        g_harts_per_core = harts_per_core;
        g_total_harts    = threads_per_pod;

        const int N = vtx_per_thread * threads_per_pod;
        const int64_t total_edges = (int64_t)N * degree;

        g_N        = N;
        g_degree   = degree;
        g_sim_exit = 0;
        g_sum_dist = 0;
        g_reached  = 0;
        g_max_dist = 0;
        cur_buf    = 0;
        frontier_tail[0] = 0;
        frontier_tail[1] = 0;
        shared_queue.head = 0;
        shared_queue.tail = 0;

        ws::barrier_init(&g_barrier, threads_per_pod);

        for (int i = 0; i < threads_per_pod; i++)
            stat_nodes_processed[i] = 0;
        for (int c = 0; c < cores_per_pod; c++) {
            stat_nodes_per_core[c] = 0;
            stat_edges_per_core[c] = 0;
        }

        // Set up L1SP data regions
        init_l1sp_data_regions();

        // Allocate and bulk-load graph
        size_t file_size = 20
            + (size_t)(N + 1) * sizeof(int32_t)
            + (size_t)total_edges * sizeof(int32_t)
            + (size_t)N * sizeof(int32_t);

        g_file_buffer = (char *)std::malloc(file_size);
        if (!g_file_buffer) {
            std::printf("ERROR: malloc failed for file buffer (%lu bytes)\n",
                        (unsigned long)file_size);
            g_N = 0;
            g_initialized.store(1, std::memory_order_release);
            return 1;
        }

        bulk_load("uniform_graph.bin", g_file_buffer, file_size);

        int32_t *header = (int32_t *)g_file_buffer;
        int hdr_N      = header[0];
        int hdr_E      = header[1];
        int hdr_source = header[4];

        if (hdr_N != N || hdr_E > (int)total_edges) {
            std::printf("ERROR: graph header mismatch: file(N=%d E=%d) != expected(N=%d E<=%lld)\n",
                        hdr_N, hdr_E, N, (long long)total_edges);
            std::free(g_file_buffer);
            g_N = 0;
            g_initialized.store(1, std::memory_order_release);
            return 1;
        }

        g_source      = hdr_source;
        g_csr_offsets  = (int32_t *)(g_file_buffer + 20);
        g_csr_edges    = (int32_t *)(g_file_buffer + 20 + (size_t)(N + 1) * sizeof(int32_t));

        // Allocate dist[] in DRAM + bulk load
        size_t dist_bytes = (size_t)N * sizeof(int32_t);
        g_dist = (int32_t *)std::malloc(dist_bytes);
        if (!g_dist) {
            std::printf("ERROR: malloc failed for dist[]\n");
            std::free(g_file_buffer);
            g_N = 0;
            g_initialized.store(1, std::memory_order_release);
            return 1;
        }
        bulk_load("bfs_dist_init.bin", g_dist, dist_bytes);

        // Allocate frontier storage (L2SP -> L1SP -> DRAM)
        uintptr_t l2sp_heap = ((uintptr_t)l2sp_end + 7) & ~(uintptr_t)7;
        uintptr_t l2sp_base = 0x20000000;
        uintptr_t l2sp_limit = l2sp_base + podL2SPSize();
        uintptr_t l1sp_heap_off = L1SP_DATA_START;
        uintptr_t l1sp_data_end = L1SP_DATA_START + (uintptr_t)g_l1sp_data_bytes_per_core;

        if (!alloc_frontier_storage(&l2sp_heap, l2sp_limit,
                                    &l1sp_heap_off, l1sp_data_end, N)) {
            std::free(g_dist);
            std::free(g_file_buffer);
            g_N = 0;
            g_initialized.store(1, std::memory_order_release);
            return 1;
        }

        size_t l2sp_used = l2sp_heap - l2sp_base;
        int64_t dram_frontier_entries = N - frontier_l2sp_cap - frontier_l1sp_cap;

        std::printf("=== CSR BFS with Shared Queue (per-vertex frontier) ===\n");
        std::printf("CSR BFS (bulk load): N=%d E=%d degree=%d source=%d\n",
                    N, hdr_E, degree, hdr_source);
        std::printf("HW: total_harts=%d, pxn=%d pods/pxn=%d cores/pod=%d harts/core=%d\n",
                    total_harts_hw, numPXN(), pods_per_pxn, cores_per_pod, harts_per_core);
        std::printf("Using: %d cores x %d harts = %d total\n",
                    cores_per_pod, harts_per_core, threads_per_pod);

        std::printf("\nMemory tiers (per frontier buffer):\n");
        std::printf("  L2SP: %ld entries (%lu bytes used / %lu total)\n",
                    (long)frontier_l2sp_cap, (unsigned long)l2sp_used,
                    (unsigned long)podL2SPSize());
        if (frontier_l1sp_cap > 0) {
            std::printf("  L1SP: %ld entries (%d/core x %d cores, %d bytes/core usable)\n",
                        (long)frontier_l1sp_cap, frontier_l1sp_per_core,
                        g_total_cores, g_l1sp_data_bytes_per_core);
        } else {
            std::printf("  L1SP: 0 entries (no room or not needed)\n");
        }
        std::printf("  DRAM: %ld entries\n", (long)dram_frontier_entries);
        std::printf("  Total capacity: %ld entries (= N)\n", (long)frontier_total_cap);
        std::printf("  Chunk size: %d, local buffer: %d\n\n", BFS_CHUNK_SIZE, LOCAL_BUF_SIZE);

        // Set up initial frontier: just the source vertex
        frontier_set(0, 0, (int64_t)g_source);
        frontier_tail[0] = 1;

        std::atomic_thread_fence(std::memory_order_release);
        g_initialized.store(1, std::memory_order_release);
    } else {
        while (g_initialized.load(std::memory_order_acquire) == 0)
            hartsleep(10);
    }

    if (g_N == 0) {
        if (tid == 0) g_sim_exit = 1;
        while (g_sim_exit == 0) hartsleep(100);
        return 1;
    }

    const int N = g_N;

    // Vertex range for parallel stats reduction
    const int vtx_per_thr = (N + g_total_harts - 1) / g_total_harts;
    const int v_lo = tid * vtx_per_thr;
    const int v_hi = std::min(v_lo + vtx_per_thr, N);

    barrier();

    // ================================================================
    // BFS Loop
    // ================================================================
    uint64_t t_bfs_start = cycle();
    int32_t level = 0;

    while (true) {
        // Hart 0: set up shared queue from current frontier, reset next
        if (tid == 0) {
            int64_t fsize = frontier_tail[cur_buf];
            shared_queue.head = 0;
            shared_queue.tail = fsize;
            // Reset the next buffer's tail for this level's discoveries
            frontier_tail[cur_buf ^ 1] = 0;
        }
        barrier();

        int64_t total_work = atomic_load_i64(&shared_queue.tail);
        if (total_work == 0) break;

        if (tid == 0) {
            std::printf("Level %d: frontier=%ld\n", level, (long)total_work);
        }

        // Process level
        ph_stat_phase(1);
        process_bfs_level(tid, level);
        ph_stat_phase(0);

        barrier();

        // Swap buffers (no data copy needed)
        if (tid == 0) {
            cur_buf ^= 1;
        }

        barrier();
        level++;
    }

    uint64_t t_bfs_end = cycle();

    // ================================================================
    // Parallel stats reduction
    // ================================================================
    ph_stat_phase(1);

    int64_t local_sum = 0;
    int32_t local_cnt = 0;
    int32_t local_max = 0;

    for (int v = v_lo; v < v_hi; v++) {
        int d = g_dist[v];
        if (d >= 0) {
            local_cnt++;
            local_sum += d;
            if (d > local_max) local_max = d;
        }
    }

    atomic_fetch_add_i64(&g_sum_dist, local_sum);
    atomic_fetch_add_i32(&g_reached, local_cnt);

    int32_t old_max = g_max_dist;
    while (local_max > old_max) {
        int32_t prev = atomic_compare_and_swap_i32(&g_max_dist, old_max, local_max);
        if (prev == old_max) break;
        old_max = prev;
    }

    ph_stat_phase(0);
    barrier();

    // ================================================================
    // Results (hart 0)
    // ================================================================
    if (tid == 0) {
        uint64_t bfs_cycles = t_bfs_end - t_bfs_start;

        std::printf("\n=== BFS Complete ===\n");
        std::printf("Levels: %d (%llu cycles)\n", level, (unsigned long long)bfs_cycles);

        int pct = g_reached > 0 ? (int)(100LL * g_reached / N) : 0;
        std::printf("Reached: %d/%d (%d%%)\n", (int)g_reached, N, pct);
        long long avg_x10 = g_reached > 0 ? (10LL * g_sum_dist / g_reached) : 0;
        std::printf("max_dist=%d  sum_dist=%lld  avg_dist=%lld.%lld\n",
                    (int)g_max_dist, (long long)g_sum_dist,
                    avg_x10 / 10, avg_x10 % 10);

        bool ok = true;
        if (g_dist[g_source] != 0) {
            std::printf("FAIL: dist[%d]=%d (expected 0)\n", g_source, g_dist[g_source]);
            ok = false;
        }
        if (g_reached < 1) {
            std::printf("FAIL: reached=%d (expected >= 1)\n", (int)g_reached);
            ok = false;
        }
        std::printf(ok ? "RESULT: PASS\n" : "RESULT: FAIL\n");

        // Per-core work balance
        int64_t total_processed = 0;
        int64_t total_edges_done = 0;
        int64_t min_nodes = 0x7FFFFFFFFFFFFFFFLL;
        int64_t max_nodes = 0;

        std::printf("\nPer-core statistics:\n");
        std::printf("Core | Processed | Edges\n");
        std::printf("-----|-----------|------\n");

        for (int c = 0; c < g_total_cores; c++) {
            int64_t cp = stat_nodes_per_core[c];
            int64_t ce = stat_edges_per_core[c];
            total_processed += cp;
            total_edges_done += ce;
            if (cp < min_nodes) min_nodes = cp;
            if (cp > max_nodes) max_nodes = cp;
            std::printf("%4d | %9ld | %9ld\n", c, (long)cp, (long)ce);
        }

        int64_t avg = (g_total_cores > 0) ? (total_processed / g_total_cores) : 0;
        int64_t imbalance_pct = (max_nodes > 0)
            ? ((max_nodes - min_nodes) * 100 / max_nodes) : 0;

        std::printf("\nSummary:\n");
        std::printf("  Total nodes processed: %ld\n", (long)total_processed);
        std::printf("  Total edges traversed: %ld\n", (long)total_edges_done);
        std::printf("  Average per core:      %ld nodes\n", (long)avg);
        std::printf("  Min/Max per core:      %ld / %ld\n", (long)min_nodes, (long)max_nodes);
        std::printf("  Imbalance (max-min)/max: %ld%%\n", (long)imbalance_pct);
        if (total_processed > 0) {
            std::printf("  Cycles per node:       %lu\n",
                        (unsigned long)(bfs_cycles / total_processed));
        }

        // Memory tier utilization
        std::printf("\nFrontier tier utilization:\n");
        std::printf("  L2SP: %ld / %ld entries\n",
                    (long)std::min((int64_t)g_reached, frontier_l2sp_cap),
                    (long)frontier_l2sp_cap);
        if (frontier_l1sp_cap > 0) {
            std::printf("  L1SP: up to %ld entries (%d/core)\n",
                        (long)frontier_l1sp_cap, frontier_l1sp_per_core);
        }
        int64_t dram_entries = frontier_total_cap - frontier_l2sp_cap - frontier_l1sp_cap;
        if (dram_entries > 0) {
            std::printf("  DRAM: up to %ld entries\n", (long)dram_entries);
        }

        std::free(g_dist);
        std::free(g_file_buffer);
        std::free(frontier_dram[0]);
        std::free(frontier_dram[1]);

        std::printf("\nBFS complete, signaling exit.\n");
        std::fflush(stdout);
        g_sim_exit = 1;
    }

    while (g_sim_exit == 0) hartsleep(100);
    return 0;
}
