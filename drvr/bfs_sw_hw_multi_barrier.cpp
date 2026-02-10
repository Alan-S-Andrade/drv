// bfs_sw_hw_multi_barrier.cpp
// Multihart BFS on a 2D grid, level-synchronous, with improved barrier.
// Uses the same barrier structure as bfs_sw_hw_multi_l2sp.cpp but without L2SP for data arrays.
// This allows fair comparison of L2SP data placement vs regular memory.

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

// ---------- Memory section attributes ----------
#define __l2sp__ __attribute__((section(".dram")))

// ---------- Configuration ----------
static const int DO_FULL_VERIFY = 0;

// ---------- Helper functions ----------
static int parse_i(const char* s, int d) {
    if (!s) return d;
    char* e = nullptr;
    long v = strtol(s, &e, 10);
    return (e && *e == 0) ? (int)v : d;
}

static inline int id(int r, int c, int C) { return r * C + c; }

// ---------- Thread utilities ----------
static inline int threads() { return numPodCores() * myCoreThreads(); }
static inline int my_thread() { return myCoreThreads() * myCoreId() + myThreadId(); }

static inline void wait(volatile int x) {
    for (int i = 0; i < x; i++) {
        asm volatile("nop");
    }
}

// ---------- Barrier (L2SP-based, matching common.hpp pattern) ----------
struct barrier_data {
    int count;
    int signal;
    int num_threads;  // actual number of participating threads
};

__l2sp__ barrier_data g_barrier_data = {0, 0, 0};

class barrier_ref {
public:
    barrier_ref(barrier_data* ptr) : ptr_(ptr) {}
    barrier_data* ptr_;

    int& count() { return ptr_->count; }
    int& signal() { return ptr_->signal; }
    int& num_threads() { return ptr_->num_threads; }

    void sync() {
        sync([](){});
    }

    template <typename F>
    void sync(F f) {
        int signal_ = signal();
        int count_ = atomic_fetch_add_i32(&count(), 1);
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

// ---------- BFS State (regular memory - NOT in L2SP) ----------

// Grid dimensions - shared across all threads
static int g_R;
static int g_C;
static int g_N;

// BFS arrays - the main data structures (NOT in L2SP for comparison)
static const int MAXN = 1000 * 1000;

static int32_t g_dist[MAXN];
static uint8_t g_frontier[MAXN];
static uint8_t g_next_frontier[MAXN];

// BFS control flags
static volatile int g_bfs_done;

// Reduction variables
static volatile int64_t g_sum_dist;
static volatile int32_t g_reached;
static volatile int32_t g_max_dist;

// Simulation exit flag
static volatile int g_sim_exit;

// ---------- Main ----------
extern "C" int main(int argc, char** argv) {
    int R = 512;
    int C = 512;
    int desired_threads = 1024;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--R") && i + 1 < argc) R = parse_i(argv[++i], R);
        else if (!strcmp(argv[i], "--C") && i + 1 < argc) C = parse_i(argv[++i], C);
        else if (!strcmp(argv[i], "--T") && i + 1 < argc) desired_threads = parse_i(argv[++i], desired_threads);
        else if (!strcmp(argv[i], "--help")) {
            std::printf("Usage: %s --R <rows> --C <cols> --T <threads>\n", argv[0]);
            return 0;
        }
    }

    // Hardware topology
    const int hart_in_core    = myThreadId();
    const int core_in_pod     = myCoreId();
    const int pod_in_pxn      = myPodId();
    const int pxn_id          = myPXNId();
    const int harts_per_core  = myCoreThreads();
    const int cores_per_pod   = numPodCores();
    const int pods_per_pxn    = numPXNPods();

    const int total_harts_hw =
        numPXN() * pods_per_pxn * cores_per_pod * harts_per_core;

    // Global thread ID
    const int tid_global =
        ((((pxn_id * pods_per_pxn) + pod_in_pxn) * cores_per_pod + core_in_pod) * harts_per_core) + hart_in_core;

    // For comparison with L2SP version, we also use threads within a single pod
    // tid_local is the thread ID within the pod
    const int tid_local = core_in_pod * harts_per_core + hart_in_core;
    const int threads_per_pod = cores_per_pod * harts_per_core;

    int total_threads = desired_threads;
    if (total_threads > threads_per_pod) total_threads = threads_per_pod;

    // Park non-participating threads
    if (tid_local >= total_threads) {
        while (g_sim_exit == 0) {
            hartsleep(1000);
        }
        return 0;
    }

    // Only run on pod 0, pxn 0 (same as L2SP version for fair comparison)
    if (pxn_id != 0 || pod_in_pxn != 0) {
        while (g_sim_exit == 0) {
            hartsleep(1000);
        }
        return 0;
    }

    // Use barrier_ref pattern (same as L2SP version)
    barrier_ref barrier(&g_barrier_data);

    // Initialize barrier thread count (must be done before any sync!)
    // Thread 0 sets it, others spin until it's set
    if (tid_local == 0) {
        barrier.num_threads() = total_threads;
    }
    // Simple spin wait for thread 0 to set num_threads
    while (barrier.num_threads() == 0) {
        wait(10);
    }

    // Initialize grid dimensions (thread 0 only, then barrier)
    barrier.sync([&]() {
        g_R = R;
        g_C = C;
        g_N = R * C;
        g_sim_exit = 0;
        g_bfs_done = 0;
        g_sum_dist = 0;
        g_reached = 0;
        g_max_dist = 0;

        if (g_N > MAXN) {
            std::printf("N=%d exceeds MAXN=%d\n", g_N, MAXN);
        }
    });

    if (g_N > MAXN) {
        return 1;
    }

    if (tid_local == 0) {
        std::printf("Grid BFS (barrier version, NO L2SP data): R=%d C=%d N=%d\n", R, C, g_N);
        std::printf("HW: total_harts=%d, pxn=%d pods/pxn=%d cores/pod=%d harts/core=%d\n",
                    total_harts_hw, numPXN(), pods_per_pxn, cores_per_pod, harts_per_core);
        std::printf("Using total_threads=%d (within single pod for comparison)\n", total_threads);
        std::printf("Full Verification: %s\n", DO_FULL_VERIFY ? "ENABLED" : "DISABLED");
    }

    // Initialize arrays (parallel across threads)
    for (int i = tid_local; i < g_N; i += total_threads) {
        g_dist[i] = -1;
        g_frontier[i] = 0;
        g_next_frontier[i] = 0;
    }
    barrier.sync();

    // Initialize BFS source (thread 0 only)
    barrier.sync([&]() {
        const int source = 0;
        g_dist[source] = 0;
        g_frontier[source] = 1;
    });

    // Direction vectors for 4-connectivity
    const int dr[4] = {-1, 1, 0, 0};
    const int dc[4] = { 0, 0, -1, 1};

    int iter = 0;

    // Main BFS loop
    while (true) {
        // Expand frontier (parallel)
        for (int v = tid_local; v < g_N; v += total_threads) {
            if (!g_frontier[v]) continue;

            const int ur = v / g_C;
            const int uc = v % g_C;
            const int du = g_dist[v];

            for (int k = 0; k < 4; ++k) {
                const int vr = ur + dr[k];
                const int vc = uc + dc[k];
                if (vr < 0 || vr >= g_R || vc < 0 || vc >= g_C) continue;

                const int nv = id(vr, vc, g_C);

                // Atomic CAS to claim vertex
                if (atomic_compare_and_swap_i32(&g_dist[nv], -1, du + 1) == -1) {
                    g_next_frontier[nv] = 1;
                }
            }
        }

        barrier.sync();

        // Swap frontiers + done check (thread 0 only)
        barrier.sync([&]() {
            int any = 0;
            for (int i = 0; i < g_N; ++i) {
                uint8_t v = g_next_frontier[i];
                g_frontier[i] = v;
                if (v) any = 1;
                g_next_frontier[i] = 0;
            }
            g_bfs_done = any ? 0 : 1;
        });

        if (g_bfs_done) break;
        ++iter;
    }

    // Parallel reduction for statistics
    long long local_sum = 0;
    int local_reached = 0;
    int local_max = 0;

    for (int i = tid_local; i < g_N; i += total_threads) {
        int d = g_dist[i];
        if (d >= 0) {
            local_reached++;
            local_sum += d;
            if (d > local_max) local_max = d;
        }
    }

    atomic_fetch_add_i64(&g_sum_dist, local_sum);
    atomic_fetch_add_i32(&g_reached, local_reached);

    // Atomic max update
    int32_t old_max = g_max_dist;
    while (local_max > old_max) {
        int32_t prev = atomic_compare_and_swap_i32(&g_max_dist, old_max, local_max);
        if (prev == old_max) break;
        old_max = prev;
    }

    barrier.sync();

    // Print results and verify (thread 0 only)
    if (tid_local == 0) {
        std::printf("BFS done in %d iterations\n", iter);
        std::printf("max_dist=%d\n", (int)g_max_dist);
        std::printf("sum_dist=%lld\n", (long long)g_sum_dist);

        if (DO_FULL_VERIFY) {
            // Expected values for 4-neighbor grid BFS from (0,0)
            const long long exp_reached = 1LL * g_R * g_C;
            const int exp_max = (g_R - 1) + (g_C - 1);
            const long long sum_r = 1LL * (g_R - 1) * g_R / 2;
            const long long sum_c = 1LL * (g_C - 1) * g_C / 2;
            const long long exp_sum = 1LL * g_C * sum_r + 1LL * g_R * sum_c;

            bool ok = true;
            ok &= ((long long)g_reached == exp_reached);
            ok &= ((int)g_max_dist == exp_max);
            ok &= ((long long)g_sum_dist == exp_sum);

            auto check = [&](int r, int c) {
                int idx = r * g_C + c;
                int got = g_dist[idx];
                int exp = r + c;
                if (got != exp) {
                    std::printf("MISMATCH dist(%d,%d): got=%d exp=%d\n", r, c, got, exp);
                    return false;
                }
                return true;
            };

            ok &= check(0, 0);
            ok &= check(g_R - 1, g_C - 1);
            ok &= check(g_R / 2, g_C / 2);
            ok &= check(g_R - 1, 0);
            ok &= check(0, g_C - 1);

            std::printf(ok ? "Spot checks: PASS\n" : "Spot checks: FAIL\n");

            // Full grid verification
            std::printf("Running Full Grid Verification...\n");
            int error_count = 0;
            int max_print_errors = 10;

            for (int r = 0; r < g_R; ++r) {
                for (int c = 0; c < g_C; ++c) {
                    int idx = r * g_C + c;
                    int actual_dist = g_dist[idx];
                    int expected_dist = r + c;

                    if (actual_dist != expected_dist) {
                        error_count++;
                        if (error_count <= max_print_errors) {
                            std::printf("FAIL at [%d,%d] (idx=%d): Got %d, Expected %d\n",
                                        r, c, idx, actual_dist, expected_dist);
                        }
                    }
                }
            }

            if (error_count == 0) {
                std::printf("ALL %d NODES PASSED.\n", g_N);
            } else {
                std::printf("VERIFICATION FAILED: Found %d errors.\n", error_count);
            }
        }

        std::printf("BFS complete, signaling exit.\n");
        std::fflush(stdout);
        g_sim_exit = 1;
    }

    // Wait for exit signal
    while (g_sim_exit == 0) {
        hartsleep(100);
    }

    return 0;
}
