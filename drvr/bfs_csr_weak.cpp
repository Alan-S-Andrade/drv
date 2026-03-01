// bfs_csr_weak.cpp
// CSR graph BFS with weak scaling.
// Generates a random graph (fixed out-degree) at runtime, then runs
// level-synchronous parallel BFS.  Work scales with thread count:
//   N = vtx_per_thread * total_threads
//
// Memory layout:
//   DRAM  – CSR offsets[], edges[], dist[]
//   L2SP  – frontier bitmaps (2x), barrier, control variables, DRAM pointers

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#include <pandohammer/cpuinfo.h>
#include <pandohammer/atomic.h>
#include <pandohammer/hartsleep.h>
#include <pandohammer/mmio.h>
#include <pandohammer/staticdecl.h>

// ---------- Configuration defaults ----------
#ifndef DEFAULT_VTX_PER_THREAD
#define DEFAULT_VTX_PER_THREAD 1024
#endif

#ifndef DEFAULT_DEGREE
#define DEFAULT_DEGREE 16
#endif

static const int DO_FULL_VERIFY = 0;

// ---------- Section attribute ----------
#define __l2sp__ __attribute__((section(".l2sp")))

// ---------- Blocking atomic OR (standard RISC-V amoor.w) ----------
static inline int32_t atomic_or_i32(volatile int32_t *ptr, int32_t val)
{
    int32_t ret;
    asm volatile("amoor.w %0, %2, 0(%1)"
                 : "=r"(ret) : "r"(ptr), "r"(val) : "memory");
    return ret;
}

// ---------- PRNG ----------
static inline uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

// ---------- Helpers ----------
static int parse_i(const char *s, int d)
{
    if (!s) return d;
    char *e = nullptr;
    long v = strtol(s, &e, 10);
    return (e && *e == 0) ? (int)v : d;
}

static inline void wait(volatile int x)
{
    for (int i = 0; i < x; i++)
        asm volatile("nop");
}

// ---------- Barrier (L2SP, same pattern as existing BFS) ----------
struct barrier_data {
    int count;
    int signal;
    int num_threads;
};

__l2sp__ barrier_data g_barrier_data = {0, 0, 0};

class barrier_ref {
public:
    barrier_ref(barrier_data *ptr) : ptr_(ptr) {}
    barrier_data *ptr_;

    int &count()       { return ptr_->count; }
    int &signal()      { return ptr_->signal; }
    int &num_threads() { return ptr_->num_threads; }

    void sync() { sync([](){}); }

    template <typename F>
    void sync(F f) {
        int signal_ = signal();
        int count_  = atomic_fetch_add_i32(&count(), 1);
        if (count_ == num_threads() - 1) {
            count() = 0;
            f();
            signal() = !signal_;
        } else {
            static constexpr int backoff_limit = 1000;
            int backoff_counter = 8;
            while (signal() == signal_) {
                wait(backoff_counter);
                backoff_counter = std::min(backoff_counter * 2, backoff_limit);
            }
        }
    }
};

// ---------- L2SP globals ----------

// Graph parameters
__l2sp__ int32_t g_N;                  // total vertices
__l2sp__ int32_t g_degree;             // out-degree per vertex
__l2sp__ int32_t g_total_threads;      // active thread count
__l2sp__ int32_t g_bitmap_words;       // ceil(N/32)

// BFS control
__l2sp__ volatile int g_bfs_done;
__l2sp__ volatile int g_sim_exit;
__l2sp__ int32_t g_bfs_iters;

// Reduction accumulators
__l2sp__ volatile int64_t g_sum_dist;
__l2sp__ volatile int32_t g_reached;
__l2sp__ volatile int32_t g_max_dist;

// DRAM array pointers (stored in L2SP for fast access by all harts)
__l2sp__ int32_t *g_csr_offsets;       // [N+1]
__l2sp__ int32_t *g_csr_edges;         // [N * degree]
__l2sp__ int32_t *g_dist;              // [N]

// L2SP bitmap pointers (dynamically allocated)
__l2sp__ volatile int32_t *g_frontier;
__l2sp__ volatile int32_t *g_next_frontier;

// Linker symbol: first free byte in L2SP after static data
extern "C" char l2sp_end[];

// ---------- Main ----------
extern "C" int main(int argc, char **argv)
{
    int vtx_per_thread = DEFAULT_VTX_PER_THREAD;
    int degree         = DEFAULT_DEGREE;
    int desired_threads = 1024;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--V") && i + 1 < argc)
            vtx_per_thread = parse_i(argv[++i], vtx_per_thread);
        else if (!strcmp(argv[i], "--D") && i + 1 < argc)
            degree = parse_i(argv[++i], degree);
        else if (!strcmp(argv[i], "--T") && i + 1 < argc)
            desired_threads = parse_i(argv[++i], desired_threads);
        else if (!strcmp(argv[i], "--help")) {
            std::printf("Usage: %s --V <vtx/thread> --D <degree> --T <threads>\n",
                        argv[0]);
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

    const int total_harts_hw =
        numPXN() * pods_per_pxn * cores_per_pod * harts_per_core;

    // Local thread ID within the pod
    const int tid = core_in_pod * harts_per_core + hart_in_core;
    const int threads_per_pod = cores_per_pod * harts_per_core;

    int total_threads = desired_threads;
    if (total_threads > threads_per_pod) total_threads = threads_per_pod;

    // Park non-participating threads
    if (tid >= total_threads) {
        while (g_sim_exit == 0) hartsleep(1000);
        return 0;
    }

    // Only run on pod 0, pxn 0 (L2SP is per-pod)
    if (pxn_id != 0 || pod_in_pxn != 0) {
        while (g_sim_exit == 0) hartsleep(1000);
        return 0;
    }

    barrier_ref barrier(&g_barrier_data);

    // Thread 0 sets barrier thread count; others spin until it is set
    if (tid == 0) barrier.num_threads() = total_threads;
    while (barrier.num_threads() == 0) wait(10);

    // ================================================================
    // Phase 0: Thread 0 allocates everything, others wait at barrier
    // ================================================================
    ph_stat_phase(0);
    barrier.sync([&]() {
        const int N = vtx_per_thread * total_threads;
        const int bm_words = (N + 31) / 32;
        const int64_t total_edges = (int64_t)N * degree;

        g_N             = N;
        g_degree        = degree;
        g_total_threads = total_threads;
        g_bitmap_words  = bm_words;
        g_bfs_done      = 0;
        g_bfs_iters     = 0;
        g_sim_exit      = 0;
        g_sum_dist      = 0;
        g_reached       = 0;
        g_max_dist      = 0;

        // Allocate DRAM arrays via malloc
        g_csr_offsets = (int32_t *)std::malloc((size_t)(N + 1) * sizeof(int32_t));
        g_csr_edges   = (int32_t *)std::malloc((size_t)total_edges * sizeof(int32_t));
        g_dist        = (int32_t *)std::malloc((size_t)N * sizeof(int32_t));

        if (!g_csr_offsets || !g_csr_edges || !g_dist) {
            std::printf("ERROR: DRAM malloc failed (N=%d, E=%lld)\n",
                        N, (long long)total_edges);
            g_N = 0;
            return;
        }

        // Allocate frontier bitmaps from free L2SP
        uintptr_t heap = ((uintptr_t)l2sp_end + 7) & ~(uintptr_t)7;
        g_frontier      = (volatile int32_t *)heap;
        heap += (size_t)bm_words * sizeof(int32_t);
        heap = (heap + 7) & ~(uintptr_t)7;
        g_next_frontier = (volatile int32_t *)heap;
        heap += (size_t)bm_words * sizeof(int32_t);

        uintptr_t l2sp_base = 0x20000000;
        if (heap - l2sp_base > podL2SPSize()) {
            std::printf("ERROR: L2SP overflow: need %lu bytes, have %lu\n",
                        (unsigned long)(heap - l2sp_base),
                        (unsigned long)podL2SPSize());
            g_N = 0;
            return;
        }

        std::printf("CSR BFS (weak scaling): N=%d E=%lld degree=%d vtx/thread=%d\n",
                    N, (long long)total_edges, degree, vtx_per_thread);
        std::printf("HW: total_harts=%d, pxn=%d pods/pxn=%d cores/pod=%d harts/core=%d\n",
                    total_harts_hw, numPXN(), pods_per_pxn, cores_per_pod, harts_per_core);
        std::printf("Using total_threads=%d (within single pod for L2SP)\n", total_threads);
    });

    if (g_N == 0) {
        if (tid == 0) g_sim_exit = 1;
        while (g_sim_exit == 0) hartsleep(100);
        return 1;
    }

    const int N        = g_N;
    const int D        = g_degree;
    const int bm_words = g_bitmap_words;

    // ================================================================
    // Phase 1: Parallel graph generation
    // ================================================================
    ph_stat_phase(0);

    // Each thread fills its vertex partition of the CSR
    const int v_lo = tid * vtx_per_thread;
    const int v_hi = v_lo + vtx_per_thread;

    for (int v = v_lo; v < v_hi; ++v) {
        g_csr_offsets[v] = v * D;
        for (int e = 0; e < D; ++e) {
            uint32_t seed = (uint32_t)(v * D + e + 1); // avoid seed==0
            xorshift32(&seed);
            g_csr_edges[v * D + e] = (int32_t)(seed % (uint32_t)N);
        }
    }
    // Thread 0 writes the final sentinel
    if (tid == 0) g_csr_offsets[N] = N * D;

    barrier.sync();

    // ================================================================
    // Phase 2: Parallel BFS initialization
    // ================================================================
    // Init dist[] = -1
    for (int v = v_lo; v < v_hi; ++v)
        g_dist[v] = -1;

    // Init frontier bitmaps = 0
    const int words_per_thread = (bm_words + total_threads - 1) / total_threads;
    const int w_lo = tid * words_per_thread;
    const int w_hi = std::min(w_lo + words_per_thread, bm_words);
    for (int w = w_lo; w < w_hi; ++w) {
        g_frontier[w]      = 0;
        g_next_frontier[w] = 0;
    }

    barrier.sync();

    // Set BFS source (vertex 0)
    barrier.sync([&]() {
        g_dist[0] = 0;
        g_frontier[0] = 1; // bit 0 of word 0
    });

    if (tid == 0)
        std::printf("BFS from source 0\n");

    // ================================================================
    // Phase 3: Main BFS loop (level-synchronous)
    // ================================================================
    uint64_t t_bfs_start = cycle();

    while (true) {
        // ---- Expand frontier (parallel) ----
        ph_stat_phase(1);

        for (int w = w_lo; w < w_hi; ++w) {
            int32_t word = g_frontier[w];
            if (word == 0) continue;

            // Iterate set bits
            while (word != 0) {
                int bit = __builtin_ctz(word);   // lowest set bit
                word &= word - 1;                // clear it

                int v = w * 32 + bit;
                if (v >= N) break;

                int d = g_dist[v];
                int edge_begin = g_csr_offsets[v];
                int edge_end   = g_csr_offsets[v + 1];

                for (int ei = edge_begin; ei < edge_end; ++ei) {
                    int u = g_csr_edges[ei];

                    // Atomic CAS: claim unvisited vertex
                    if (atomic_compare_and_swap_i32(&g_dist[u], -1, d + 1) == -1) {
                        // Set bit in next_frontier (blocking atomic OR in L2SP)
                        int wi = u / 32;
                        int32_t mask = 1 << (u % 32);
                        atomic_or_i32(&g_next_frontier[wi], mask);
                    }
                }
            }
        }

        ph_stat_phase(0);
        barrier.sync();

        // ---- Swap frontiers + done check (single thread in barrier lambda) ----
        barrier.sync([&]() {
            int any = 0;
            for (int w = 0; w < bm_words; ++w) {
                int32_t val = g_next_frontier[w];
                g_frontier[w] = val;
                if (val) any = 1;
                g_next_frontier[w] = 0;
            }
            if (any)
                g_bfs_iters++;
            else
                g_bfs_done = 1;
        });

        if (g_bfs_done) break;
    }

    uint64_t t_bfs_end = cycle();

    // ================================================================
    // Phase 4: Parallel reduction for statistics
    // ================================================================
    ph_stat_phase(1);

    int64_t local_sum  = 0;
    int32_t local_cnt  = 0;
    int32_t local_max  = 0;

    for (int v = v_lo; v < v_hi; ++v) {
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
    barrier.sync();

    // ================================================================
    // Phase 5: Print results and optional verification (thread 0)
    // ================================================================
    if (tid == 0) {
        uint64_t bfs_cycles = t_bfs_end - t_bfs_start;

        std::printf("BFS done in %d iterations (%llu cycles)\n",
                    g_bfs_iters, (unsigned long long)bfs_cycles);
        int pct = g_reached > 0 ? (int)(100LL * g_reached / N) : 0;
        std::printf("Reached: %d/%d (%d%%)\n", (int)g_reached, N, pct);
        long long avg_x10 = g_reached > 0 ? (10LL * g_sum_dist / g_reached) : 0;
        std::printf("max_dist=%d  sum_dist=%lld  avg_dist=%lld.%lld\n",
                    (int)g_max_dist, (long long)g_sum_dist,
                    avg_x10 / 10, avg_x10 % 10);

        // Basic sanity checks
        bool ok = true;
        if (g_dist[0] != 0) {
            std::printf("FAIL: dist[0]=%d (expected 0)\n", g_dist[0]);
            ok = false;
        }
        if (g_reached < 1) {
            std::printf("FAIL: reached=%d (expected >= 1)\n", (int)g_reached);
            ok = false;
        }

        // Full verification: triangle inequality on every edge
        if (DO_FULL_VERIFY) {
            std::printf("Running full verification (triangle inequality)...\n");
            int violations = 0;
            for (int v = 0; v < N; ++v) {
                int dv = g_dist[v];
                if (dv < 0) continue;
                int eb = g_csr_offsets[v];
                int ee = g_csr_offsets[v + 1];
                for (int ei = eb; ei < ee; ++ei) {
                    int u  = g_csr_edges[ei];
                    int du = g_dist[u];
                    if (du < 0) continue; // unreachable neighbor is ok
                    if (du > dv + 1) {
                        if (violations < 10)
                            std::printf("  VIOLATION: dist[%d]=%d > dist[%d]+1=%d\n",
                                        u, du, v, dv + 1);
                        violations++;
                    }
                }
            }
            if (violations == 0)
                std::printf("Verification: PASS (all edges satisfy triangle inequality)\n");
            else
                std::printf("Verification: FAIL (%d violations)\n", violations);
            ok = ok && (violations == 0);
        }

        std::printf(ok ? "RESULT: PASS\n" : "RESULT: FAIL\n");

        std::printf("BFS complete, signaling exit.\n");
        std::fflush(stdout);

        std::free(g_csr_offsets);
        std::free(g_csr_edges);
        std::free(g_dist);

        g_sim_exit = 1;
    }

    while (g_sim_exit == 0) hartsleep(100);
    return 0;
}
