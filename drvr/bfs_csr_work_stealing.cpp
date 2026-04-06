// bfs_csr_work_stealing.cpp
// Level-synchronous CSR BFS with per-core work stealing (ROI-only).
//
// Work-stealing over frontier bitmap words:
//   - Frontier stored as bitmap in L2SP (same as baseline)
//   - Each "work item" in a queue is a bitmap word index (up to 32 vertices)
//   - Hart 0 distributes word indices to per-core queues (imbalanced)
//   - Harts pop word indices, expand all set vertices in that word
//   - If local queue empty, steal word indices from other cores
//   - Next level: separate bitmap, atomic OR (same as baseline)
//
// This keeps queue memory tiny (~256 KB) vs vertex-level queues (~16 MB).
//
// Graph bulk-loaded from file via MMIO (same as bfs_csr_weak_roi.cpp).
// dist[] in DRAM, CSR in DRAM, bitmaps + queues in L2SP.

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
#include <pandohammer/staticdecl.h>

#include "work_stealing.h"

// ---------- Configuration ----------
#ifndef DEFAULT_VTX_PER_THREAD
#define DEFAULT_VTX_PER_THREAD 1024
#endif
#ifndef DEFAULT_DEGREE
#define DEFAULT_DEGREE 16
#endif

// Queue stores bitmap word indices, not vertices.
// Max bitmap words for 1M vertices = 32K.  QCAP=512 per core × 64 cores = 32K total.
static constexpr int QCAP = 1024;            // Per-core queue capacity (power of 2)
static constexpr int MAX_HARTS = 1024;
static constexpr int MAX_CORES = 64;
static constexpr int POP_CHUNK = 8;          // Bitmap words per batch pop
static constexpr int STEAL_K = 8;            // Bitmap words per batch steal
static constexpr int RECENT_SIZE = 4;

// Steal policy
static constexpr int STEAL_START = 4;        // Empty pops before stealing
static constexpr int STEAL_VICTIMS = 2;      // Victims to probe per episode

// ---------- Blocking atomic OR (RISC-V amoor.w) ----------
static inline int32_t atomic_or_i32(volatile int32_t *ptr, int32_t val)
{
    int32_t ret;
    asm volatile("amoor.w %0, %2, 0(%1)"
                 : "=r"(ret) : "r"(ptr), "r"(val) : "memory");
    return ret;
}

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

// ---------- L2SP Globals ----------

// Runtime config
__l2sp__ volatile int g_total_harts = 0;
__l2sp__ volatile int g_harts_per_core = 0;
__l2sp__ volatile int g_total_cores = 0;
__l2sp__ std::atomic<int> g_initialized = 0;

// Work queues store bitmap word indices (int64_t holding an int32_t word index)
__l2sp__ ws::WorkQueue<QCAP> core_queues[MAX_CORES];
__l2sp__ ws::BarrierState<MAX_HARTS> g_barrier;
__l2sp__ volatile int32_t core_has_work[MAX_CORES];
__l2sp__ std::atomic<int> core_thief[MAX_CORES];
__l2sp__ std::atomic<int64_t> g_level_remaining = 0;

// Graph parameters
__l2sp__ int32_t g_N;
__l2sp__ int32_t g_degree;
__l2sp__ int32_t g_bitmap_words;
__l2sp__ int32_t g_source;

// DRAM pointers (stored in L2SP for fast access)
__l2sp__ char    *g_file_buffer;
__l2sp__ int32_t *g_csr_offsets;
__l2sp__ int32_t *g_csr_edges;
__l2sp__ int32_t *g_dist;

// Frontier bitmaps (L2SP, dynamically placed after static data)
__l2sp__ volatile int32_t *g_frontier;
__l2sp__ volatile int32_t *g_next_frontier;

// Control
__l2sp__ volatile int g_sim_exit;

// Reduction accumulators
__l2sp__ volatile int64_t g_sum_dist;
__l2sp__ volatile int32_t g_reached;
__l2sp__ volatile int32_t g_max_dist;

// Per-hart stats
__l2sp__ volatile int64_t stat_nodes_processed[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_attempts[MAX_HARTS];
__l2sp__ volatile int64_t stat_steal_success[MAX_HARTS];

// L2SP heap boundary (from linker)
extern "C" char l2sp_end[];

// ---------- Thread ID (dense, for barrier indexing) ----------
static inline int get_thread_id() {
    return myCoreId() * (int)g_harts_per_core + myThreadId();
}

static inline void barrier() {
    ws::barrier(&g_barrier, get_thread_id(), g_total_harts);
}

// ---------- Frontier Distribution ----------
// Hart 0 scans frontier bitmap, pushes non-zero word INDICES into per-core
// queues.  Imbalanced: odd cores get 2x words.  Returns count of non-zero words.
static int64_t distribute_frontier_words_to_queues()
{
    const int total_cores = g_total_cores;
    const int bm_words = g_bitmap_words;

    // Count non-zero words
    int64_t nz_words = 0;
    for (int w = 0; w < bm_words; w++) {
        if (g_frontier[w] != 0) nz_words++;
    }

    // Reset queues
    for (int c = 0; c < total_cores; c++) {
        ws::queue_init(&core_queues[c]);
        core_has_work[c] = 0;
    }

    if (nz_words == 0) return 0;

    // Compute per-core quotas (odd cores get 2x)
    int weights[MAX_CORES];
    int64_t quotas[MAX_CORES];
    int sum_w = 0;
    for (int c = 0; c < total_cores; c++) {
        weights[c] = (c & 1) ? 2 : 1;
        sum_w += weights[c];
    }
    int64_t assigned = 0;
    for (int c = 0; c < total_cores; c++) {
        quotas[c] = (nz_words * weights[c]) / sum_w;
        assigned += quotas[c];
    }
    int64_t rem = nz_words - assigned;
    for (int i = 0; rem > 0; i++, rem--) {
        quotas[i % total_cores]++;
    }

    // Distribute non-zero word indices to queues
    int target = 0;
    while (target < total_cores && quotas[target] == 0) target++;

    int64_t pushed = 0;
    for (int w = 0; w < bm_words; w++) {
        if (g_frontier[w] == 0) continue;

        int dest = (target < total_cores) ? target : total_cores - 1;
        if (!ws::queue_push(&core_queues[dest], (int64_t)w)) {
            // Target full — try any core with space
            bool placed = false;
            for (int c = 0; c < total_cores; c++) {
                if (ws::queue_push(&core_queues[c], (int64_t)w)) {
                    core_has_work[c] = 1;
                    placed = true;
                    break;
                }
            }
            if (!placed) {
                std::printf("ERROR: all queues full, dropping word %d\n", w);
                continue;  // skip — do NOT count in pushed
            }
        } else {
            core_has_work[dest] = 1;
        }
        pushed++;

        if (dest == target) {
            quotas[target]--;
            while (target < total_cores && quotas[target] == 0) target++;
        }
    }

    return pushed;
}

// ---------- Process one bitmap word: expand all set vertices ----------
static inline int64_t process_bitmap_word(int w, int32_t level,
                                          int32_t *offsets, int32_t *edges,
                                          int32_t *dist,
                                          volatile int32_t *next_frontier,
                                          int N)
{
    int32_t word = g_frontier[w];
    if (word == 0) return 0;

    int64_t processed = 0;
    while (word) {
        int bit = __builtin_ctz(word);
        word &= word - 1;

        int v = w * 32 + bit;
        if (v >= N) break;
        processed++;

        int edge_begin = offsets[v];
        int edge_end   = offsets[v + 1];
        for (int ei = edge_begin; ei < edge_end; ei++) {
            int u = edges[ei];
            if (atomic_compare_and_swap_i32(&dist[u], -1, level + 1) == -1) {
                int wi = u / 32;
                int32_t mask = 1 << (u % 32);
                atomic_or_i32(&next_frontier[wi], mask);
            }
        }
    }
    return processed;
}

// ---------- BFS Level Processing (with work stealing) ----------
static void process_bfs_level(int tid, int32_t level)
{
    const int hpc = g_harts_per_core;
    const int total_cores = g_total_cores;
    const int my_core = tid / hpc;
    const int N = g_N;

    ws::WorkQueue<QCAP>* my_queue = &core_queues[my_core];

    // Cache DRAM pointers locally
    int32_t *offsets = g_csr_offsets;
    int32_t *edges   = g_csr_edges;
    int32_t *dist    = g_dist;
    volatile int32_t *next_frontier = g_next_frontier;

    uint32_t rng = ws::xorshift_seed(tid);
    int recently_tried[RECENT_SIZE];
    ws::clear_recent(recently_tried, RECENT_SIZE);
    int rt_idx = 0;

    int64_t local_processed = 0;
    int64_t local_steal_attempts = 0;
    int64_t local_steal_success = 0;

    int64_t chunk_buf[POP_CHUNK];
    int64_t stolen_buf[STEAL_K];

    int empty_streak = 0;
    int64_t steal_backoff = 4;
    const int64_t steal_backoff_max = 512;
    int64_t local_backoff = 4;
    const int64_t local_backoff_max = 128;

    while (g_level_remaining.load(std::memory_order_acquire) > 0) {
        // Batch pop bitmap word indices from local queue
        int64_t count = ws::queue_pop_chunk(my_queue, chunk_buf, POP_CHUNK);

        if (count > 0) {
            g_level_remaining.fetch_sub(count, std::memory_order_acq_rel);

            for (int64_t i = 0; i < count; i++) {
                int w = (int)chunk_buf[i];
                local_processed += process_bitmap_word(
                    w, level, offsets, edges, dist, next_frontier, N);
            }

            empty_streak = 0;
            steal_backoff = 4;
            local_backoff = 4;
        } else {
            // Mark own queue empty
            core_has_work[my_core] = 0;

            // Try to become this core's thief
            if (core_thief[my_core].exchange(1, std::memory_order_acquire) != 0) {
                hartsleep(local_backoff * 4);
                if (local_backoff < local_backoff_max) local_backoff <<= 1;
                continue;
            }

            empty_streak++;

            // Short local backoff before first steal attempt
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

                    // Push extras to local queue for sibling harts;
                    // if queue full, process inline to avoid losing work
                    for (int64_t s = 1; s < count; s++) {
                        if (ws::queue_push_atomic(my_queue, stolen_buf[s])) {
                            core_has_work[my_core] = 1;
                        } else {
                            // Queue full — process this word directly
                            g_level_remaining.fetch_sub(1, std::memory_order_acq_rel);
                            local_processed += process_bitmap_word(
                                (int)stolen_buf[s], level, offsets, edges, dist,
                                next_frontier, N);
                        }
                    }

                    // Process first stolen word
                    g_level_remaining.fetch_sub(1, std::memory_order_acq_rel);
                    {
                        int w = (int)stolen_buf[0];
                        local_processed += process_bitmap_word(
                            w, level, offsets, edges, dist, next_frontier, N);
                    }

                    empty_streak = 0;
                    steal_backoff = 4;
                    local_backoff = 4;
                    ws::clear_recent(recently_tried, RECENT_SIZE);
                    break;
                } else {
                    ws::record_recent(recently_tried, &rt_idx, RECENT_SIZE, victim);
                }
            }

            core_thief[my_core].store(0, std::memory_order_release);

            if (!found) {
                hartsleep(steal_backoff);
                if (steal_backoff < steal_backoff_max) steal_backoff <<= 1;
            }
        }
    }

    stat_nodes_processed[tid] += local_processed;
    stat_steal_attempts[tid]  += local_steal_attempts;
    stat_steal_success[tid]   += local_steal_success;
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

    // Only run on pod 0, pxn 0 (L2SP is per-pod)
    if (pxn_id != 0 || pod_in_pxn != 0) {
        while (g_sim_exit == 0) hartsleep(1000);
        return 0;
    }

    const int tid = core_in_pod * harts_per_core + hart_in_core;

    // Park excess threads
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
        const int bm_words = (N + 31) / 32;

        g_N            = N;
        g_degree       = degree;
        g_bitmap_words = bm_words;
        g_sim_exit     = 0;
        g_sum_dist     = 0;
        g_reached      = 0;
        g_max_dist     = 0;

        // Init queues, barriers, steal tokens
        for (int c = 0; c < cores_per_pod; c++) {
            ws::queue_init(&core_queues[c]);
            core_has_work[c] = 0;
            core_thief[c].store(0, std::memory_order_relaxed);
        }
        ws::barrier_init(&g_barrier, threads_per_pod);

        for (int i = 0; i < threads_per_pod; i++) {
            stat_nodes_processed[i] = 0;
            stat_steal_attempts[i]  = 0;
            stat_steal_success[i]   = 0;
        }

        // Allocate and bulk-load graph file from DRAM
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

        // Parse header
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

        // Allocate frontier bitmaps from L2SP heap
        uintptr_t heap = ((uintptr_t)l2sp_end + 7) & ~(uintptr_t)7;
        g_frontier      = (volatile int32_t *)heap;
        heap += (size_t)bm_words * sizeof(int32_t);
        heap  = (heap + 7) & ~(uintptr_t)7;
        g_next_frontier = (volatile int32_t *)heap;
        heap += (size_t)bm_words * sizeof(int32_t);
        heap  = (heap + 7) & ~(uintptr_t)7;

        uintptr_t l2sp_base = 0x20000000;
        uintptr_t l2sp_cap  = podL2SPSize();
        size_t l2sp_used    = heap - l2sp_base;

        std::printf("L2SP usage: static+bitmaps = %lu bytes, capacity = %lu bytes\n",
                    (unsigned long)l2sp_used, (unsigned long)l2sp_cap);

        if (l2sp_used > l2sp_cap) {
            std::printf("ERROR: L2SP overflow: need %lu bytes, have %lu\n",
                        (unsigned long)l2sp_used, (unsigned long)l2sp_cap);
            std::free(g_file_buffer);
            g_N = 0;
            g_initialized.store(1, std::memory_order_release);
            return 1;
        }

        // Allocate dist[] in DRAM
        size_t dist_bytes = (size_t)N * sizeof(int32_t);
        g_dist = (int32_t *)std::malloc(dist_bytes);
        if (!g_dist) {
            std::printf("ERROR: malloc failed for dist[] (%lu bytes)\n",
                        (unsigned long)dist_bytes);
            std::free(g_file_buffer);
            g_N = 0;
            g_initialized.store(1, std::memory_order_release);
            return 1;
        }

        // Bulk load pre-initialized dist[] and frontier bitmaps
        bulk_load("bfs_dist_init.bin", g_dist, dist_bytes);
        size_t bitmaps_bytes = 2 * (size_t)bm_words * sizeof(int32_t);
        bulk_load("bfs_frontier_init.bin", (void *)g_frontier, bitmaps_bytes);

        std::printf("=== CSR BFS with Work Stealing ===\n");
        std::printf("CSR BFS (bulk load): N=%d E=%d degree=%d source=%d\n",
                    N, hdr_E, degree, hdr_source);
        std::printf("HW: total_harts=%d, pxn=%d pods/pxn=%d cores/pod=%d harts/core=%d\n",
                    total_harts_hw, numPXN(), pods_per_pxn, cores_per_pod, harts_per_core);
        std::printf("Using: %d cores x %d harts = %d total\n",
                    cores_per_pod, harts_per_core, threads_per_pod);
        std::printf("Bitmap words: %d, queue cap/core: %d\n\n", bm_words, QCAP);

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

    const int N        = g_N;
    const int bm_words = g_bitmap_words;

    // Bitmap word range for parallel swap
    const int words_per_thread = (bm_words + g_total_harts - 1) / g_total_harts;
    const int w_lo = tid * words_per_thread;
    const int w_hi = std::min(w_lo + words_per_thread, bm_words);

    // Vertex range for parallel stats reduction
    const int vtx_per_thr = (N + g_total_harts - 1) / g_total_harts;
    const int v_lo = tid * vtx_per_thr;
    const int v_hi = std::min(v_lo + vtx_per_thr, N);

    barrier();

    // ================================================================
    // BFS Loop (level-synchronous, work-stealing within each level)
    // ================================================================
    uint64_t t_bfs_start = cycle();
    int32_t level = 0;

    while (true) {
        // Hart 0: distribute non-zero frontier word indices to per-core queues
        if (tid == 0) {
            int64_t total_work = distribute_frontier_words_to_queues();
            g_level_remaining.store(total_work, std::memory_order_release);
        }
        barrier();  // sync: queues filled, g_level_remaining published

        int64_t total_work = g_level_remaining.load(std::memory_order_acquire);
        if (total_work == 0) break;

        if (tid == 0) {
            std::printf("Level %d: nz_words=%ld\n", level, (long)total_work);
        }

        // Process level with work stealing
        ph_stat_phase(1);
        process_bfs_level(tid, level);
        ph_stat_phase(0);

        barrier();  // sync: all harts done expanding

        // Parallel swap: next_frontier → frontier, clear next_frontier
        for (int w = w_lo; w < w_hi; w++) {
            g_frontier[w] = g_next_frontier[w];
            g_next_frontier[w] = 0;
        }

        barrier();  // sync: swap complete, frontier ready for next distribute

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

    // Atomic max via CAS loop
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

        // Sanity checks
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

        // Work-stealing statistics
        int64_t total_processed = 0;
        int64_t total_attempts  = 0;
        int64_t total_success   = 0;

        std::printf("\nPer-core statistics:\n");
        std::printf("Core | Processed | Steal Att. | Steals OK\n");
        std::printf("-----|-----------|------------|----------\n");

        for (int c = 0; c < g_total_cores; c++) {
            int64_t cp = 0, ca = 0, cs = 0;
            for (int h = 0; h < harts_per_core; h++) {
                int hid = c * harts_per_core + h;
                cp += stat_nodes_processed[hid];
                ca += stat_steal_attempts[hid];
                cs += stat_steal_success[hid];
            }
            total_processed += cp;
            total_attempts  += ca;
            total_success   += cs;
            std::printf("%4d | %9ld | %10ld | %9ld\n",
                        c, (long)cp, (long)ca, (long)cs);
        }

        std::printf("\nSummary:\n");
        std::printf("  Total nodes processed: %ld\n", (long)total_processed);
        std::printf("  Total steal attempts:  %ld\n", (long)total_attempts);
        if (total_attempts > 0) {
            std::printf("  Successful steals:     %ld (%ld%%)\n",
                        (long)total_success,
                        (long)(100 * total_success / total_attempts));
        }
        if (total_processed > 0) {
            std::printf("  Cycles per node:       %lu\n",
                        (unsigned long)(bfs_cycles / total_processed));
        }

        std::free(g_dist);
        std::free(g_file_buffer);

        std::printf("\nBFS complete, signaling exit.\n");
        std::fflush(stdout);
        g_sim_exit = 1;
    }

    while (g_sim_exit == 0) hartsleep(100);
    return 0;
}
