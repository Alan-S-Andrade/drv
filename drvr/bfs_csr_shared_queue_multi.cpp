// bfs_csr_shared_queue_multi.cpp
// Multi-pod level-synchronous CSR BFS with per-pod shared-queue load balancing
// and inter-pod work stealing via DRAM overflow pools.
//
// Architecture:
//   - Each pod owns a vertex partition [pod*N_local .. (pod+1)*N_local)
//   - Intra-pod: shared queue in L2SP, per-vertex frontier (same as single-pod)
//   - Inter-pod: DRAM overflow work pools enable cross-pod work stealing
//   - Remote discoveries: written to DRAM exchange buffers, merged after barrier
//
// Work stealing flow:
//   1. Each pod expands its own frontier via L2SP shared queue
//   2. When a pod's next-frontier exceeds a spill threshold, excess goes to
//      that pod's DRAM work pool (stealable by other pods)
//   3. When a pod's shared queue empties mid-level, it steals from other pods'
//      DRAM work pools via CAS on the pool head
//   4. Stolen vertices are expanded directly (graph/dist in shared DRAM)
//
// Memory layout:
//   L2SP (per-pod): shared queue head/tail, frontier storage, barrier, stats
//   L1SP: frontier overflow (distributed across cores within pod)
//   DRAM (shared): CSR graph, dist[], remote discovery exchange, work pools,
//                  inter-pod barriers, global coordination

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
static constexpr int MAX_PODS  = 8;
static constexpr int BFS_CHUNK_SIZE = 8;      // Vertices per CAS pop batch
static constexpr int LOCAL_BUF_SIZE = 32;     // Per-hart discovery buffer
static constexpr int STEAL_CHUNK_SIZE = 16;   // Vertices per DRAM steal batch

// L1SP data region layout
static constexpr uintptr_t L1SP_DATA_START = 16;
static constexpr uintptr_t L1SP_STACK_GUARD = 5120;  // 5KB guard

// ---------- Shared Queue (L2SP, per-pod) ----------
struct SharedQueue {
    volatile int64_t head;
    volatile int64_t tail;
};

// ---------- DRAM Work Pool (one per pod, stealable) ----------
// Simple head/tail into a flat DRAM array of vertex IDs.
struct DRAMWorkPool {
    volatile int64_t head;
    volatile int64_t tail;
    int64_t capacity;
    int64_t *items;   // DRAM array
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

// Blocking atomic OR (standard RISC-V amoor.w)
static inline int32_t atomic_or_i32(volatile int32_t *ptr, int32_t val)
{
    int32_t ret;
    asm volatile("amoor.w %0, %2, 0(%1)"
                 : "=r"(ret) : "r"(ptr), "r"(val) : "memory");
    return ret;
}

// ---------- Inter-pod Barrier (DRAM) ----------
static volatile int64_t g_inter_pod_count = 0;
static volatile int64_t g_inter_pod_phase = 0;
static int64_t g_pod_local_phase[MAX_PODS];

static inline void inter_pod_barrier(int pod_id, int num_pods)
{
    int64_t my_phase = g_pod_local_phase[pod_id];

    int64_t old = atomic_fetch_add_i64(&g_inter_pod_count, 1);
    if (old == num_pods - 1) {
        atomic_fetch_add_i64(&g_inter_pod_count, -num_pods);
        atomic_fetch_add_i64(&g_inter_pod_phase, 1);
    } else {
        long w = 64, wmax = 2048;
        while (atomic_load_i64(&g_inter_pod_phase) == my_phase) {
            hartsleep(w);
            if (w < wmax) w <<= 1;
        }
    }
    g_pod_local_phase[pod_id] = my_phase + 1;
}

// ---------- DRAM globals for multi-pod coordination ----------
static volatile int32_t g_pods_ready = 0;
static volatile int32_t g_global_done = 0;
static volatile int32_t g_global_any_work = 0;
static volatile int32_t g_global_bfs_iters = 0;
static volatile int32_t g_global_sim_exit = 0;

// DRAM pointers set by pod 0 tid 0, read by all pods
static char    *g_dram_file_buffer = nullptr;
static int32_t *g_dram_csr_offsets = nullptr;
static int32_t *g_dram_csr_edges = nullptr;
static int32_t *g_dram_dist = nullptr;
static int32_t  g_dram_N = 0;
static int32_t  g_dram_N_local = 0;
static int32_t  g_dram_degree = 0;
static int32_t  g_dram_source = 0;
static int32_t  g_dram_total_threads_per_pod = 0;
static int32_t  g_dram_num_pods = 0;
static int32_t  g_dram_total_cores_per_pod = 0;
static int32_t  g_dram_harts_per_core = 0;

// Remote discovery exchange: per-pod vertex ID buffers in DRAM
// g_remote_disc[target_pod] is an array of discovered vertex IDs for that pod
// g_remote_disc_tail[target_pod] is the atomic append counter
static int64_t *g_remote_disc[MAX_PODS];
static volatile int64_t g_remote_disc_tail[MAX_PODS];
static int64_t g_remote_disc_cap = 0;  // capacity per pod

// DRAM work pools (one per pod, stealable by other pods)
static DRAMWorkPool g_work_pool[MAX_PODS];

// Global reduction accumulators in DRAM
static volatile int64_t g_global_sum_dist = 0;
static volatile int32_t g_global_reached = 0;
static volatile int32_t g_global_max_dist = 0;

// Per-pod steal stats in DRAM
static volatile int64_t g_steals_attempted[MAX_PODS];
static volatile int64_t g_steals_succeeded[MAX_PODS];
static volatile int64_t g_vertices_stolen[MAX_PODS];
static volatile int64_t g_vertices_spilled[MAX_PODS];

// ---------- L2SP globals (per-pod) ----------

// Runtime config
__l2sp__ volatile int g_total_harts = 0;
__l2sp__ volatile int g_harts_per_core = 0;
__l2sp__ volatile int g_total_cores = 0;
__l2sp__ std::atomic<int> g_initialized = 0;

// Barrier
__l2sp__ ws::BarrierState<MAX_HARTS> g_barrier;

// Shared queue (indexes into current frontier buffer)
__l2sp__ SharedQueue shared_queue;

// Graph parameters (local)
__l2sp__ int32_t g_N;           // total vertices (global)
__l2sp__ int32_t g_N_local;     // vertices owned by this pod
__l2sp__ int32_t g_vtx_offset;  // first vertex owned by this pod
__l2sp__ int32_t g_degree;
__l2sp__ int32_t g_source;
__l2sp__ int32_t g_pod_id;
__l2sp__ int32_t g_num_pods;

// DRAM pointers (stored in L2SP for fast access)
__l2sp__ int32_t *g_csr_offsets;
__l2sp__ int32_t *g_csr_edges;
__l2sp__ int32_t *g_dist;

// Control
__l2sp__ volatile int g_bfs_done;
__l2sp__ volatile int g_sim_exit;

// Reduction accumulators (per-pod, in L2SP)
__l2sp__ volatile int64_t g_sum_dist;
__l2sp__ volatile int32_t g_reached;
__l2sp__ volatile int32_t g_max_dist;

// Per-hart / per-core stats
__l2sp__ volatile int64_t stat_nodes_processed[MAX_HARTS];
__l2sp__ volatile int64_t stat_edges_per_core[MAX_CORES];
__l2sp__ volatile int64_t stat_nodes_per_core[MAX_CORES];

// ---------- Double-buffered frontier (L2SP -> L1SP -> DRAM) ----------
__l2sp__ volatile int cur_buf;
__l2sp__ volatile int64_t frontier_tail[2];

// L2SP tier
__l2sp__ int64_t *frontier_l2sp[2];
__l2sp__ int64_t frontier_l2sp_cap;

// L1SP tier
__l2sp__ uintptr_t g_l1sp_abs_base[MAX_CORES];
__l2sp__ int32_t g_l1sp_data_bytes_per_core;
__l2sp__ int64_t frontier_l1sp_cap;
__l2sp__ int32_t frontier_l1sp_per_core;
__l2sp__ int32_t frontier_l1sp_shift;
__l2sp__ uintptr_t frontier_l1sp_offset[2];

// DRAM tier
__l2sp__ int64_t *frontier_dram[2];
__l2sp__ int64_t frontier_total_cap;

// Spill threshold: above this, next-frontier excess goes to DRAM work pool
__l2sp__ int64_t frontier_spill_threshold;

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
        addr = ph_address_absolute_set_pxn(addr, 0);
        addr = ph_address_absolute_set_pod(addr, (uintptr_t)g_pod_id);
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

// ---------- Shared Queue CAS Pop (L2SP, intra-pod) ----------
static inline int64_t sq_pop_chunk(int64_t *out_buf, int64_t max_chunk) {
    // TTAS: volatile pre-check
    int64_t h = shared_queue.head;
    int64_t t = shared_queue.tail;
    if (h >= t) return 0;

    // Atomic check + CAS
    h = atomic_load_i64(&shared_queue.head);
    t = atomic_load_i64(&shared_queue.tail);
    if (h >= t) return 0;

    int64_t avail = t - h;
    int64_t k = (avail < max_chunk) ? avail : max_chunk;
    int64_t old_h = atomic_compare_and_swap_i64(&shared_queue.head, h, h + k);
    if (old_h != h) return 0;

    for (int64_t i = 0; i < k; i++) {
        out_buf[i] = frontier_get(cur_buf, h + i);
    }
    return k;
}

// ---------- DRAM Work Pool Steal (inter-pod) ----------
// CAS-pop a chunk of vertex IDs from another pod's DRAM work pool.
static inline int64_t dram_pool_steal(DRAMWorkPool *pool, int64_t *out_buf, int64_t max_chunk) {
    // TTAS
    int64_t h = pool->head;
    int64_t t = pool->tail;
    if (h >= t) return 0;

    h = atomic_load_i64(&pool->head);
    t = atomic_load_i64(&pool->tail);
    if (h >= t) return 0;

    int64_t avail = t - h;
    int64_t k = (avail < max_chunk) ? avail : max_chunk;
    int64_t old_h = atomic_compare_and_swap_i64(&pool->head, h, h + k);
    if (old_h != h) return 0;

    for (int64_t i = 0; i < k; i++) {
        out_buf[i] = pool->items[h + i];
    }
    return k;
}

// ---------- Next Frontier Batch Push ----------
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

// ---------- Expand a single vertex (used by both local and stolen paths) ----------
static inline void expand_vertex(int32_t u, int32_t level, int next_buf,
                                 int my_pod, int N_local,
                                 int64_t *disc_buf, int *disc_count,
                                 int64_t *local_edges_out)
{
    int32_t *offsets = g_csr_offsets;
    int32_t *edges   = g_csr_edges;
    int32_t *dist    = g_dist;

    int32_t edge_begin = offsets[u];
    int32_t edge_end   = offsets[u + 1];
    *local_edges_out += (edge_end - edge_begin);

    for (int32_t ei = edge_begin; ei < edge_end; ei++) {
        int32_t v = edges[ei];
        if (atomic_compare_and_swap_i32(&dist[v], -1, level + 1) == -1) {
            // Determine which pod owns v
            int target_pod = v / N_local;
            if (target_pod == my_pod) {
                // Local discovery: buffer for local next-frontier
                disc_buf[*disc_count] = (int64_t)v;
                (*disc_count)++;
                if (*disc_count == LOCAL_BUF_SIZE) {
                    flush_discoveries(disc_buf, *disc_count, next_buf);
                    *disc_count = 0;
                }
            } else {
                // Remote discovery: write to DRAM exchange buffer for target pod
                int64_t pos = atomic_fetch_add_i64(&g_remote_disc_tail[target_pod], 1);
                if (pos < g_remote_disc_cap) {
                    g_remote_disc[target_pod][pos] = (int64_t)v;
                }
            }
        }
    }
}

// ---------- BFS Level Processing (intra-pod + inter-pod stealing) ----------
static void process_bfs_level(int tid, int32_t level)
{
    const int my_core = tid / g_harts_per_core;
    const int next_buf = cur_buf ^ 1;
    const int my_pod = g_pod_id;
    const int N_local = g_N_local;
    const int npods = g_num_pods;

    int64_t local_processed = 0;
    int64_t local_edges = 0;
    int64_t chunk_buf[BFS_CHUNK_SIZE > STEAL_CHUNK_SIZE ? BFS_CHUNK_SIZE : STEAL_CHUNK_SIZE];

    // Per-hart local buffer for batching next-frontier pushes
    int64_t disc_buf[LOCAL_BUF_SIZE];
    int disc_count = 0;

    // Phase 1: Drain local shared queue
    while (true) {
        int64_t count = sq_pop_chunk(chunk_buf, BFS_CHUNK_SIZE);

        if (count > 0) {
            for (int64_t i = 0; i < count; i++) {
                int32_t u = (int32_t)chunk_buf[i];
                local_processed++;
                expand_vertex(u, level, next_buf, my_pod, N_local,
                              disc_buf, &disc_count, &local_edges);
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
            hartsleep(1);
        }
    }

    // Phase 2: Drain own pod's DRAM work pool (spilled excess)
    {
        DRAMWorkPool *own_pool = &g_work_pool[my_pod];
        while (true) {
            int64_t count = dram_pool_steal(own_pool, chunk_buf, STEAL_CHUNK_SIZE);
            if (count <= 0) break;
            for (int64_t i = 0; i < count; i++) {
                int32_t u = (int32_t)chunk_buf[i];
                local_processed++;
                expand_vertex(u, level, next_buf, my_pod, N_local,
                              disc_buf, &disc_count, &local_edges);
            }
        }
    }

    // Phase 3: Try stealing from other pods' DRAM work pools
    if (npods <= 1) goto done_stealing;
    {
    uint32_t rng = ws::xorshift_seed(tid + my_pod * g_total_harts);
    int consecutive_failures = 0;

    while (consecutive_failures < npods * 2) {
        rng = ws::xorshift_next(rng);
        int victim = (int)(rng % (uint32_t)npods);
        if (victim == my_pod) continue;

        DRAMWorkPool *pool = &g_work_pool[victim];

        int64_t count = dram_pool_steal(pool, chunk_buf, STEAL_CHUNK_SIZE);
        if (count > 0) {
            atomic_fetch_add_i64(&g_steals_succeeded[my_pod], 1);
            atomic_fetch_add_i64(&g_vertices_stolen[my_pod], count);
            consecutive_failures = 0;  // reset failure counter

            for (int64_t i = 0; i < count; i++) {
                int32_t u = (int32_t)chunk_buf[i];
                local_processed++;
                expand_vertex(u, level, next_buf, my_pod, N_local,
                              disc_buf, &disc_count, &local_edges);
            }
            // After successful steal, drain local queue (discoveries may have arrived)
            while (true) {
                int64_t lcount = sq_pop_chunk(chunk_buf, BFS_CHUNK_SIZE);
                if (lcount <= 0) break;
                for (int64_t i = 0; i < lcount; i++) {
                    int32_t u = (int32_t)chunk_buf[i];
                    local_processed++;
                    expand_vertex(u, level, next_buf, my_pod, N_local,
                                  disc_buf, &disc_count, &local_edges);
                }
            }
        } else {
            atomic_fetch_add_i64(&g_steals_attempted[my_pod], 1);
            consecutive_failures++;
        }
    }
    }
done_stealing:

    // Final flush
    if (disc_count > 0) {
        flush_discoveries(disc_buf, disc_count, next_buf);
    }

    stat_nodes_processed[tid] += local_processed;
    atomic_fetch_add_i64(&stat_nodes_per_core[my_core], local_processed);
    atomic_fetch_add_i64(&stat_edges_per_core[my_core], local_edges);
}

// ---------- Frontier Memory Allocation (L2SP -> L1SP -> DRAM) ----------
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

    // L2SP tier
    {
        uintptr_t heap = (*l2sp_heap + 7) & ~(uintptr_t)7;
        uintptr_t avail = (heap < l2sp_limit) ? (l2sp_limit - heap) : 0;
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

    // L1SP tier
    if (remaining > 0 && *l1sp_heap_off < l1sp_data_end) {
        int32_t avail_bytes = (int32_t)(l1sp_data_end - *l1sp_heap_off);
        int32_t entries_raw = avail_bytes / (int32_t)(2 * sizeof(int64_t));
        int32_t epc = floor_pow2(entries_raw);
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

    // DRAM tier
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

    // Spill threshold: 75% of local frontier capacity
    frontier_spill_threshold = (frontier_total_cap * 3) / 4;

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
    const int num_pods       = pods_per_pxn;
    const int threads_per_pod = cores_per_pod * harts_per_core;

    const int total_harts_hw =
        numPXN() * pods_per_pxn * cores_per_pod * harts_per_core;

    // Local thread ID within this pod
    const int tid = core_in_pod * harts_per_core + hart_in_core;

    // Park non-pxn-0 threads
    if (pxn_id != 0) {
        while (g_global_sim_exit == 0) hartsleep(1000);
        return 0;
    }
    if (tid >= threads_per_pod) {
        while (g_global_sim_exit == 0) hartsleep(1000);
        return 0;
    }

    // Vertex partitioning
    const int N_local = vtx_per_thread * threads_per_pod;
    const int N_total = N_local * num_pods;

    // ================================================================
    // Phase 0a: Pod 0, tid 0 — bulk-load graph + allocate shared DRAM structures
    // ================================================================
    if (pod_in_pxn == 0 && tid == 0) {
        const int64_t total_edges = (int64_t)N_total * degree;

        size_t file_size = 20
            + (size_t)(N_total + 1) * sizeof(int32_t)
            + (size_t)total_edges * sizeof(int32_t)
            + (size_t)N_total * sizeof(int32_t);

        g_dram_file_buffer = (char *)std::malloc(file_size);
        if (!g_dram_file_buffer) {
            std::printf("ERROR: malloc failed for file buffer (%lu bytes)\n",
                        (unsigned long)file_size);
            g_dram_N = 0;
            g_pods_ready = -1;
            goto pod0_init_done;
        }

        bulk_load("uniform_graph.bin", g_dram_file_buffer, file_size);

        {
            int32_t *header = (int32_t *)g_dram_file_buffer;
            int hdr_N      = header[0];
            int hdr_E      = header[1];
            int hdr_source = header[4];

            if (hdr_N != N_total || hdr_E > (int)total_edges) {
                std::printf("ERROR: graph header mismatch: file(N=%d E=%d) != expected(N=%d E<=%lld)\n",
                            hdr_N, hdr_E, N_total, (long long)total_edges);
                std::free(g_dram_file_buffer);
                g_dram_file_buffer = nullptr;
                g_dram_N = 0;
                g_pods_ready = -1;
                goto pod0_init_done;
            }

            g_dram_source = hdr_source;
            g_dram_csr_offsets = (int32_t *)(g_dram_file_buffer + 20);
            g_dram_csr_edges   = (int32_t *)(g_dram_file_buffer + 20 + (size_t)(N_total + 1) * sizeof(int32_t));
        }

        // Allocate dist[] and bulk-load
        {
            size_t dist_bytes = (size_t)N_total * sizeof(int32_t);
            g_dram_dist = (int32_t *)std::malloc(dist_bytes);
            if (!g_dram_dist) {
                std::printf("ERROR: malloc failed for dist[]\n");
                std::free(g_dram_file_buffer);
                g_dram_N = 0;
                g_pods_ready = -1;
                goto pod0_init_done;
            }
            bulk_load("bfs_dist_init.bin", g_dram_dist, dist_bytes);
        }

        // Allocate remote discovery exchange buffers (per-pod vertex ID lists)
        {
            // Each pod can receive up to N_local remote discoveries per level
            g_remote_disc_cap = N_local;
            for (int p = 0; p < num_pods; p++) {
                g_remote_disc[p] = (int64_t *)std::malloc((size_t)N_local * sizeof(int64_t));
                if (!g_remote_disc[p]) {
                    std::printf("ERROR: malloc failed for remote disc buf pod %d\n", p);
                    g_dram_N = 0;
                    g_pods_ready = -1;
                    goto pod0_init_done;
                }
                g_remote_disc_tail[p] = 0;
            }
        }

        // Allocate DRAM work pools (one per pod)
        {
            // Each pool can hold up to N_local / 4 entries (25% of partition)
            int64_t pool_cap = N_local / 4;
            if (pool_cap < 1024) pool_cap = 1024;

            for (int p = 0; p < num_pods; p++) {
                g_work_pool[p].head = 0;
                g_work_pool[p].tail = 0;
                g_work_pool[p].capacity = pool_cap;
                g_work_pool[p].items = (int64_t *)std::malloc((size_t)pool_cap * sizeof(int64_t));
                if (!g_work_pool[p].items) {
                    std::printf("ERROR: malloc failed for work pool pod %d\n", p);
                    g_dram_N = 0;
                    g_pods_ready = -1;
                    goto pod0_init_done;
                }
                g_steals_attempted[p] = 0;
                g_steals_succeeded[p] = 0;
                g_vertices_stolen[p] = 0;
                g_vertices_spilled[p] = 0;
            }
        }

        g_dram_N = N_total;
        g_dram_N_local = N_local;
        g_dram_degree = degree;
        g_dram_total_threads_per_pod = threads_per_pod;
        g_dram_num_pods = num_pods;
        g_dram_total_cores_per_pod = cores_per_pod;
        g_dram_harts_per_core = harts_per_core;
        g_global_done = 0;
        g_global_any_work = 0;
        g_global_bfs_iters = 0;
        g_global_sim_exit = 0;
        g_global_sum_dist = 0;
        g_global_reached = 0;
        g_global_max_dist = 0;

        g_inter_pod_count = 0;
        g_inter_pod_phase = 0;
        for (int p = 0; p < MAX_PODS; p++)
            g_pod_local_phase[p] = 0;

        std::printf("=== Multi-pod CSR BFS with Shared Queue + DRAM Work Stealing ===\n");
        std::printf("N=%d (%d per pod), %d pods, E=%d, degree=%d, source=%d\n",
                    N_total, N_local, num_pods, ((int32_t *)g_dram_file_buffer)[1],
                    degree, g_dram_source);
        std::printf("HW: total_harts=%d, pxn=%d pods/pxn=%d cores/pod=%d harts/core=%d\n",
                    total_harts_hw, numPXN(), pods_per_pxn, cores_per_pod, harts_per_core);
        std::printf("Threads/pod=%d, DRAM pool cap=%ld\n",
                    threads_per_pod, (long)g_work_pool[0].capacity);

        g_pods_ready = 1;
    }

pod0_init_done:

    // All pod 0 threads wait for init
    if (pod_in_pxn == 0) {
        while (g_pods_ready == 0) {
            for (int i = 0; i < 100; i++) asm volatile("nop");
        }
    }

    // Other pods wait
    if (pod_in_pxn != 0) {
        while (g_pods_ready == 0) hartsleep(100);
    }

    if (g_pods_ready < 0 || g_dram_N == 0) {
        if (pod_in_pxn == 0 && tid == 0) g_global_sim_exit = 1;
        while (g_global_sim_exit == 0) hartsleep(100);
        return 1;
    }

    // ================================================================
    // Phase 0b: Each pod tid 0 sets up L2SP from DRAM parameters
    // ================================================================
    if (tid == 0) {
        g_total_cores    = g_dram_total_cores_per_pod;
        g_harts_per_core = g_dram_harts_per_core;
        g_total_harts    = g_dram_total_threads_per_pod;

        g_N          = g_dram_N;
        g_N_local    = g_dram_N_local;
        g_vtx_offset = pod_in_pxn * g_dram_N_local;
        g_degree     = g_dram_degree;
        g_source     = g_dram_source;
        g_pod_id     = pod_in_pxn;
        g_num_pods   = g_dram_num_pods;
        g_bfs_done   = 0;
        g_sim_exit   = 0;
        g_sum_dist   = 0;
        g_reached    = 0;
        g_max_dist   = 0;
        cur_buf      = 0;
        frontier_tail[0] = 0;
        frontier_tail[1] = 0;
        shared_queue.head = 0;
        shared_queue.tail = 0;

        ws::barrier_init(&g_barrier, g_dram_total_threads_per_pod);

        g_csr_offsets = g_dram_csr_offsets;
        g_csr_edges   = g_dram_csr_edges;
        g_dist        = g_dram_dist;

        for (int i = 0; i < g_dram_total_threads_per_pod; i++)
            stat_nodes_processed[i] = 0;
        for (int c = 0; c < g_dram_total_cores_per_pod; c++) {
            stat_nodes_per_core[c] = 0;
            stat_edges_per_core[c] = 0;
        }

        init_l1sp_data_regions();

        // Allocate frontier storage
        uintptr_t l2sp_heap = ((uintptr_t)l2sp_end + 7) & ~(uintptr_t)7;
        uintptr_t l2sp_base = 0x20000000;
        uintptr_t l2sp_limit = l2sp_base + podL2SPSize();
        uintptr_t l1sp_heap_off = L1SP_DATA_START;
        uintptr_t l1sp_data_end = L1SP_DATA_START + (uintptr_t)g_l1sp_data_bytes_per_core;

        if (!alloc_frontier_storage(&l2sp_heap, l2sp_limit,
                                    &l1sp_heap_off, l1sp_data_end, g_dram_N_local)) {
            std::printf("ERROR: Pod %d frontier alloc failed\n", pod_in_pxn);
            g_N_local = 0;
        } else {
            size_t l2sp_used = l2sp_heap - l2sp_base;
            int64_t dram_entries = g_dram_N_local - frontier_l2sp_cap - frontier_l1sp_cap;

            if (pod_in_pxn == 0) {
                std::printf("\nMemory tiers (per frontier buffer, per pod):\n");
                std::printf("  L2SP: %ld entries (%lu bytes used / %lu total)\n",
                            (long)frontier_l2sp_cap, (unsigned long)l2sp_used,
                            (unsigned long)podL2SPSize());
                if (frontier_l1sp_cap > 0) {
                    std::printf("  L1SP: %ld entries (%d/core x %d cores)\n",
                                (long)frontier_l1sp_cap, frontier_l1sp_per_core,
                                g_total_cores);
                }
                std::printf("  DRAM: %ld entries\n", (long)dram_entries);
                std::printf("  Spill threshold: %ld entries\n\n",
                            (long)frontier_spill_threshold);
            }

            // Set up initial frontier: source vertex (only owning pod)
            int source_pod = g_dram_source / g_dram_N_local;
            if (pod_in_pxn == source_pod) {
                frontier_set(0, 0, (int64_t)(g_dram_source));
                frontier_tail[0] = 1;
            }
        }

        std::atomic_thread_fence(std::memory_order_release);
        g_initialized.store(1, std::memory_order_release);
    } else {
        while (g_initialized.load(std::memory_order_acquire) == 0)
            hartsleep(10);
    }

    if (g_N_local == 0) {
        if (tid == 0) g_global_sim_exit = 1;
        while (g_global_sim_exit == 0) hartsleep(100);
        return 1;
    }

    const int my_pod = g_pod_id;
    const int npods = g_num_pods;
    const int N_local_val = g_N_local;

    // Vertex range for parallel stats reduction
    const int vtx_per_thr = (N_local_val + g_total_harts - 1) / g_total_harts;
    const int v_lo = tid * vtx_per_thr;
    const int v_hi = std::min(v_lo + vtx_per_thr, N_local_val);

    barrier();

    // Inter-pod barrier: all pods ready
    if (tid == 0) {
        inter_pod_barrier(my_pod, npods);
    }
    barrier();

    if (my_pod == 0 && tid == 0) {
        std::printf("BFS from source %d (pod %d)\n", g_source, g_source / N_local_val);
    }

    // ================================================================
    // BFS Loop
    // ================================================================
    uint64_t t_bfs_start = cycle();

    while (true) {
        int cur_level = g_global_bfs_iters;

        // Hart 0: set up shared queue from current frontier
        if (tid == 0) {
            int64_t fsize = frontier_tail[cur_buf];

            // Spill excess to DRAM work pool if above threshold
            int64_t local_count = fsize;
            int64_t spill_count = 0;
            if (fsize > frontier_spill_threshold) {
                spill_count = fsize - frontier_spill_threshold;

                DRAMWorkPool *pool = &g_work_pool[my_pod];
                pool->head = 0;
                pool->tail = 0;

                int64_t to_spill = spill_count;
                if (to_spill > pool->capacity) to_spill = pool->capacity;

                // Keep the rest in the local shared queue (don't drop vertices)
                local_count = fsize - to_spill;

                for (int64_t i = 0; i < to_spill; i++) {
                    pool->items[i] = frontier_get(cur_buf, local_count + i);
                }
                pool->tail = to_spill;
                atomic_fetch_add_i64(&g_vertices_spilled[my_pod], to_spill);
            } else {
                // Reset work pool for this level
                g_work_pool[my_pod].head = 0;
                g_work_pool[my_pod].tail = 0;
            }

            shared_queue.head = 0;
            shared_queue.tail = local_count;
            frontier_tail[cur_buf ^ 1] = 0;
        }
        barrier();

        if (tid == 0 && my_pod == 0) {
            std::printf("Level %d\n", cur_level);
        }

        // Process level (intra-pod shared queue + inter-pod stealing)
        ph_stat_phase(1);
        process_bfs_level(tid, cur_level);
        ph_stat_phase(0);

        // Intra-pod barrier after processing
        barrier();

        // ---- Inter-pod barrier ----
        if (tid == 0) {
            inter_pod_barrier(my_pod, npods);
        }
        barrier();

        // ---- Merge remote discoveries into local next-frontier ----
        // Hart 0 merges incoming remote discoveries
        if (tid == 0) {
            int64_t incoming = g_remote_disc_tail[my_pod];
            if (incoming > 0) {
                // Append to next frontier
                int next_buf = cur_buf ^ 1;
                int64_t base = frontier_tail[next_buf];
                int64_t to_add = incoming;
                if (base + to_add > frontier_total_cap) {
                    to_add = frontier_total_cap - base;
                    if (to_add < 0) to_add = 0;
                }
                for (int64_t i = 0; i < to_add; i++) {
                    frontier_set(next_buf, base + i, g_remote_disc[my_pod][i]);
                }
                frontier_tail[next_buf] = base + to_add;

                // Reset for next level
                g_remote_disc_tail[my_pod] = 0;
            }
        }

        barrier();

        // ---- Swap buffers ----
        if (tid == 0) {
            cur_buf ^= 1;
        }
        barrier();

        // ---- Check global termination ----
        if (tid == 0) {
            int64_t my_frontier = frontier_tail[cur_buf];
            if (my_frontier > 0) {
                atomic_or_i32(&g_global_any_work, 1);
            }

            inter_pod_barrier(my_pod, npods);

            if (my_pod == 0) {
                if (g_global_any_work) {
                    g_global_bfs_iters++;
                    atomic_swap_i32_posted(&g_global_any_work, 0);
                } else {
                    g_global_done = 1;
                }
            }

            inter_pod_barrier(my_pod, npods);

            g_bfs_done = g_global_done;
        }

        barrier();

        if (g_bfs_done) break;
    }

    uint64_t t_bfs_end = cycle();

    // ================================================================
    // Parallel stats reduction
    // ================================================================
    ph_stat_phase(1);

    int64_t local_sum = 0;
    int32_t local_cnt = 0;
    int32_t local_max = 0;

    int vtx_off = g_vtx_offset;
    for (int vl = v_lo; vl < v_hi; vl++) {
        int d = g_dist[vtx_off + vl];
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

    // Pod leader: reduce across pods via DRAM
    if (tid == 0) {
        atomic_fetch_add_i64(&g_global_sum_dist, g_sum_dist);
        atomic_fetch_add_i32(&g_global_reached, (int32_t)g_reached);

        int32_t pod_max = g_max_dist;
        int32_t old_gmax = g_global_max_dist;
        while (pod_max > old_gmax) {
            int32_t prev = atomic_compare_and_swap_i32(&g_global_max_dist, old_gmax, pod_max);
            if (prev == old_gmax) break;
            old_gmax = prev;
        }

        inter_pod_barrier(my_pod, npods);
    }
    barrier();

    // ================================================================
    // Results (pod 0, tid 0)
    // ================================================================
    if (my_pod == 0 && tid == 0) {
        uint64_t bfs_cycles = t_bfs_end - t_bfs_start;

        std::printf("\n=== BFS Complete ===\n");
        std::printf("Levels: %d (%llu cycles)\n",
                    g_global_bfs_iters, (unsigned long long)bfs_cycles);

        int pct = g_global_reached > 0 ? (int)(100LL * g_global_reached / g_dram_N) : 0;
        std::printf("Reached: %d/%d (%d%%)\n", (int)g_global_reached, g_dram_N, pct);
        long long avg_x10 = g_global_reached > 0
            ? (10LL * g_global_sum_dist / g_global_reached) : 0;
        std::printf("max_dist=%d  sum_dist=%lld  avg_dist=%lld.%lld\n",
                    (int)g_global_max_dist, (long long)g_global_sum_dist,
                    avg_x10 / 10, avg_x10 % 10);

        bool ok = true;
        if (g_dram_dist[g_dram_source] != 0) {
            std::printf("FAIL: dist[%d]=%d (expected 0)\n", g_dram_source,
                        g_dram_dist[g_dram_source]);
            ok = false;
        }
        if (g_global_reached < 1) {
            std::printf("FAIL: reached=%d (expected >= 1)\n", (int)g_global_reached);
            ok = false;
        }
        std::printf(ok ? "RESULT: PASS\n" : "RESULT: FAIL\n");

        // Per-pod work balance & steal stats
        std::printf("\nPer-pod statistics:\n");
        std::printf("Pod | Steals Attempted | Steals Succeeded | Vertices Stolen | Vertices Spilled\n");
        std::printf("----|------------------|------------------|-----------------|------------------\n");
        for (int p = 0; p < npods; p++) {
            std::printf("%3d | %16ld | %16ld | %15ld | %16ld\n",
                        p,
                        (long)g_steals_attempted[p],
                        (long)g_steals_succeeded[p],
                        (long)g_vertices_stolen[p],
                        (long)g_vertices_spilled[p]);
        }

        if (bfs_cycles > 0 && g_global_reached > 0) {
            std::printf("\n  Cycles per node: %lu\n",
                        (unsigned long)(bfs_cycles / (uint64_t)g_global_reached));
        }

        // Memory tier utilization
        std::printf("\nFrontier tier utilization (per pod):\n");
        std::printf("  L2SP: %ld entries\n", (long)frontier_l2sp_cap);
        if (frontier_l1sp_cap > 0) {
            std::printf("  L1SP: %ld entries (%d/core)\n",
                        (long)frontier_l1sp_cap, frontier_l1sp_per_core);
        }
        int64_t dram_entries = frontier_total_cap - frontier_l2sp_cap - frontier_l1sp_cap;
        if (dram_entries > 0) {
            std::printf("  DRAM: %ld entries\n", (long)dram_entries);
        }

        // Cleanup
        for (int p = 0; p < npods; p++) {
            std::free(g_remote_disc[p]);
            std::free(g_work_pool[p].items);
        }
        std::free(g_dram_dist);
        std::free(g_dram_file_buffer);
        std::free(frontier_dram[0]);
        std::free(frontier_dram[1]);

        std::printf("\nBFS complete, signaling exit.\n");
        std::fflush(stdout);
        g_global_sim_exit = 1;
    }

    // All threads: propagate exit
    if (tid == 0 && my_pod != 0) {
        while (g_global_sim_exit == 0) hartsleep(100);
        g_sim_exit = 1;
    }
    if (my_pod == 0 && tid == 0) {
        g_sim_exit = 1;
    }

    while (g_sim_exit == 0) hartsleep(100);
    return 0;
}
