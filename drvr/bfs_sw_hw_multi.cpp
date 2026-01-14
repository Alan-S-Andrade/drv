// bfs_sw_hw_multi.cpp
// Multihart BFS on a 2D grid.
// One "software thread" per hart, up to desired_threads.
// 16 harts make a core → 16 threads per core when fully used.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <pandohammer/cpuinfo.h>   // myThreadId(), numPodCores(), myCoreThreads()
#include <pandohammer/mmio.h>      // cycle(), etc. if you want
#include <pandohammer/atomic.h>    // atomic_fetch_add_i64, atomic_fetch_add_i32, atomic_compare_and_swap_i32
#include <pandohammer/hartsleep.h> // hartsleep()

static const int MAX_HARTS = 4;
static int64_t g_local_phase_arr[MAX_HARTS];

// Simple CLI helper
static int parse_i(const char* s, int d) {
    if (!s) return d;
    char* e = nullptr;
    long v = strtol(s, &e, 10);
    return (e && *e == 0) ? (int)v : d;
}

static inline int id(int r, int c, int C) {
    return r * C + c;
}

// ----------------- Reusable barrier using AMOs -----------------

// Shared barrier state in memory
static volatile int64_t g_barrier_count = 0;
static volatile int64_t g_barrier_phase = 0;

static inline void barrier(int total_threads) {
    int hid = myThreadId();  // from pandohammer/cpuinfo.h
    if (hid < 0 || hid >= MAX_HARTS) {
        // optional: bail or clamp; but in your setup hid should be small
        // For debug you could print here if needed.
    }

    int64_t my_phase = g_local_phase_arr[hid];

    // AMO increment on count
    int64_t old = atomic_fetch_add_i64(&g_barrier_count, 1);

    if (old == total_threads - 1) {
        // Last thread: reset and advance phase
        g_barrier_count = 0;
        atomic_fetch_add_i64(&g_barrier_phase, 1);
    } else {
        // Spin / sleep until phase advances
        long w = 1;
        long wmax = 8 * 1024;
        while (atomic_load_i64(&g_barrier_phase) == my_phase) {
            if (w < wmax) w <<= 1;
            hartsleep(w);
        }
    }

    g_local_phase_arr[hid] = my_phase + 1;
}

// ----------------- BFS globals (on "global" memory) -----------------

// For simplicity, use fixed upper bound. Adjust if needed.
static const int MAXN = 250 * 350;

static int  g_R, g_C, g_N;
static int  g_desired_threads;

// BFS state
static int     g_dist[MAXN];
static uint8_t g_frontier[MAXN];
static uint8_t g_next_frontier[MAXN];
static volatile int g_bfs_done = 0;

// Reductions
static volatile int64_t g_sum_dist = 0;
static volatile int32_t g_reached  = 0;
static volatile int32_t g_max_dist = 0;

// ----------------- Main -----------------

extern "C" int main(int argc, char** argv) {
    // Default grid size & desired software threads
    int R = 100800;
    int C = 64;
    int desired_threads = 16;    // e.g., 2 cores * 16 harts/core, adjust via --T

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--R") && i + 1 < argc) {
            R = parse_i(argv[++i], R);
        } else if (!strcmp(argv[i], "--C") && i + 1 < argc) {
            C = parse_i(argv[++i], C);
        } else if (!strcmp(argv[i], "--T") && i + 1 < argc) {
            desired_threads = parse_i(argv[++i], desired_threads);
        } else if (!strcmp(argv[i], "--help")) {
            std::printf("Usage: %s --R <rows> --C <cols> --T <threads>\n", argv[0]);
            return 0;
        }
    }

    g_R = R;
    g_C = C;
    g_N = R * C;
    if (g_N > MAXN) {
        std::printf("N=%d exceeds MAXN=%d\n", g_N, MAXN);
        return 1;
    }

    // Hardware info from PandoHammer
    int harts_per_core   = myCoreThreads();           // should be 16
    int num_cores        = numPodCores();
    int total_harts_hw   = num_cores * harts_per_core; // total hardware harts

    // Number of software threads we will actually use (1:1 with harts)
    int total_threads = desired_threads;
    if (total_threads > total_harts_hw) {
        total_threads = total_harts_hw;
    }

    // Global hart id
    int hid = myThreadId();

    // Compute core & hart-in-core mapping
    int core_id      = hid / harts_per_core;
    int hart_in_core = hid % harts_per_core;

    // Park extra harts if hid >= total_threads
    if (hid >= total_threads) {
        // This hart is unused → sleep forever
        while (true) {
            printf("FATAL: hid=%d out of range MAX_HARTS=%d\n", hid, MAX_HARTS);
            hartsleep(1 << 20);
        }
    }

    // Only threads [0 .. total_threads-1] reach here
    if (hid == 0) {
        std::printf("Grid BFS: R=%d C=%d N=%d\n", R, C, g_N);
        std::printf("HW: total_harts=%d, cores=%d, harts_per_core=%d\n",
                    total_harts_hw, num_cores, harts_per_core);
        std::printf("Using total_threads=%d software threads (1:1 with harts)\n",
                    total_threads);
    }

    // Initialize BFS state in parallel
    for (int i = hid; i < g_N; i += total_threads) {
        g_dist[i] = -1;
        g_frontier[i] = 0;
        g_next_frontier[i] = 0;
    }

    barrier(total_threads);

    // BFS from node 0
    int source = 0;
    if (hid == 0) {
        g_dist[source] = 0;
        g_frontier[source] = 1;
        g_bfs_done = 0;

        g_sum_dist = 0;
        g_reached  = 0;
        g_max_dist = 0;
    }

    barrier(total_threads);

    const int dr[4] = { -1, 1, 0, 0 };
    const int dc[4] = {  0, 0,-1, 1 };

    int iter = 0;

    while (true) {
        // Each thread processes its share of the current frontier
        for (int v = hid; v < g_N; v += total_threads) {
            if (!g_frontier[v]) continue;

            int ur = v / g_C;
            int uc = v % g_C;
            int du = g_dist[v];

            for (int k = 0; k < 4; ++k) {
                int vr = ur + dr[k];
                int vc = uc + dc[k];
                if (vr < 0 || vr >= g_R || vc < 0 || vc >= g_C) continue;

                int nv = id(vr, vc, g_C);
                if (g_dist[nv] == -1) {
                    // Level-synchronous BFS: writing the same distance is benign.
                    g_dist[nv] = du + 1;
                    g_next_frontier[nv] = 1;
                }
            }
        }

        // Wait for all threads to finish this level
        barrier(total_threads);

        // Single thread (e.g., hid == 0) prepares next frontier and checks done
        if (hid == 0) {
            int any = 0;
            for (int i = 0; i < g_N; ++i) {
                g_frontier[i] = g_next_frontier[i];
                if (g_frontier[i]) any = 1;
                g_next_frontier[i] = 0;
            }
            g_bfs_done = any ? 0 : 1;
        }

        // Let everyone see updated frontier and done flag
        barrier(total_threads);

        if (g_bfs_done) break;
        ++iter;
    }

    // Compute some summary stats in parallel and reduce on hid==0
    long long local_sum = 0;
    int local_reached = 0;
    int local_max = 0;
    for (int i = hid; i < g_N; i += total_threads) {
        int d = g_dist[i];
        if (d >= 0) {
            local_reached++;
            local_sum += d;
            if (d > local_max) local_max = d;
        }
    }

    atomic_fetch_add_i64(&g_sum_dist, local_sum);
    atomic_fetch_add_i32(&g_reached, local_reached);

    // Max reduction: CAS-style update
    int32_t old_max = g_max_dist;
    while (local_max > old_max) {
        int32_t prev = atomic_compare_and_swap_i32(&g_max_dist, old_max, local_max);
        if (prev == old_max) break;   // we successfully updated
        old_max = prev;               // someone else updated, retry
    }

    barrier(total_threads);

    if (hid == 0) {
        std::printf("BFS done in %d iterations\n", iter);
        std::printf("reached=%d\n", (int)g_reached);
        std::printf("max_dist=%d\n", (int)g_max_dist);
        std::printf("sum_dist=%lld\n", (long long)g_sum_dist);
    }

    return 0;
}

