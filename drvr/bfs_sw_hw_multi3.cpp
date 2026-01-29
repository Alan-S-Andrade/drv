// bfs_sw_hw_multi_fixed2.cpp

// Multihart BFS on a 2D grid, level-synchronous, with self-check.

//

// Key fixes vs your original:

// 1) myThreadId() is hart-id *within the core* (mhartid), so we build a global tid:

//      tid_global = (((pxn * numPXNPods + pod) * numPodCores + core) * harts_per_core) + hart_in_core

// 2) Barrier uses only fetch_add + phase (no non-atomic stores; no i64 CAS needed).

// 3) BFS discovery uses atomic CAS on g_dist to avoid data races.

// 4) Only tid_global==0 prints and checks.



#include <cstdint>

#include <cstdio>

#include <cstdlib>

#include <cstring>



#include <pandohammer/cpuinfo.h>

#include <pandohammer/atomic.h>

#include <pandohammer/hartsleep.h>



// ---------- helpers ----------

static int parse_i(const char* s, int d) {

    if (!s) return d;

    char* e = nullptr;

    long v = strtol(s, &e, 10);

    return (e && *e == 0) ? (int)v : d;

}

static inline int id(int r, int c, int C) { return r * C + c; }



// ---------- barrier ----------

// Must be >= maximum global threads you will run with.

static const int MAX_THREADS = 1024;

static int64_t g_local_phase_arr[MAX_THREADS];



static volatile int64_t g_barrier_count = 0;

static volatile int64_t g_barrier_phase = 0;

static volatile int g_sim_exit = 0;

// Barrier that needs only fetch_add_i64 + load_i64.

// last thread resets count by fetch_add(-total_threads) atomically.

static inline void barrier(int tid, int total_threads) {

    int64_t my_phase = g_local_phase_arr[tid];



    int64_t old = atomic_fetch_add_i64(&g_barrier_count, 1); // returns previous value

    if (old == total_threads - 1) {

        // count is now total_threads; reset to 0 atomically

        atomic_fetch_add_i64(&g_barrier_count, -total_threads);

        // advance phase

        atomic_fetch_add_i64(&g_barrier_phase, 1);

    } else {

        long w = 1, wmax = 8 * 1024;

        while (atomic_load_i64(&g_barrier_phase) == my_phase) {

            if (w < wmax) w <<= 1;

            hartsleep(w);

        }

    }

    g_local_phase_arr[tid] = my_phase + 1;

}



// ---------- BFS state ----------

static const int MAXN = 250 * 350;   // adjust as needed



static int g_R, g_C, g_N;



static int32_t g_dist[MAXN];

static uint8_t g_frontier[MAXN];

static uint8_t g_next_frontier[MAXN];



static volatile int g_bfs_done = 0;



// Reductions

static volatile int64_t g_sum_dist = 0;

static volatile int32_t g_reached  = 0;

static volatile int32_t g_max_dist = 0;



extern "C" int main(int argc, char** argv) {

    int R = 100;

    int C = 16;

    int desired_threads = 16;



    for (int i = 1; i < argc; ++i) {

        if (!strcmp(argv[i], "--R") && i + 1 < argc) R = parse_i(argv[++i], R);

        else if (!strcmp(argv[i], "--C") && i + 1 < argc) C = parse_i(argv[++i], C);

        else if (!strcmp(argv[i], "--T") && i + 1 < argc) desired_threads = parse_i(argv[++i], desired_threads);

        else if (!strcmp(argv[i], "--help")) {

            std::printf("Usage: %s --R <rows> --C <cols> --T <threads>\n", argv[0]);

            return 0;

        }

    }



    g_R = R; g_C = C; g_N = R * C;

    if (g_N > MAXN) {

        std::printf("N=%d exceeds MAXN=%d\n", g_N, MAXN);

        return 1;

    }



    // Hardware topology from cpuinfo.h

    const int hart_in_core    = myThreadId();     // mhartid: 0..harts_per_core-1

    const int core_in_pod     = myCoreId();       // core id within pod

    const int pod_in_pxn      = myPodId();

    const int pxn_id          = myPXNId();

    const int harts_per_core  = myCoreThreads();



    const int cores_per_pod   = numPodCores();    // numPodCoresX()*numPodCoresY()

    const int pods_per_pxn    = numPXNPods();



    // Total harts in the *whole system*

    const int total_harts_hw =

        numPXN() * pods_per_pxn * cores_per_pod * harts_per_core;



    // Build a global unique thread id across pxn/pod/core/hart

    const int tid_global =

        ((((pxn_id * pods_per_pxn) + pod_in_pxn) * cores_per_pod + core_in_pod) * harts_per_core) + hart_in_core;



    int total_threads = desired_threads;

    if (total_threads > total_harts_hw) total_threads = total_harts_hw;

    if (total_threads > MAX_THREADS) total_threads = MAX_THREADS;



    // park non-participants

    if (tid_global < 0 || tid_global >= total_threads) {

        while (g_sim_exit == 0) {
            hartsleep(1000); 
        }
        return 0;

    }



    if (tid_global == 0) {

        std::printf("Grid BFS: R=%d C=%d N=%d\n", R, C, g_N);

        std::printf("HW: total_harts=%d, pxn=%d pods/pxn=%d cores/pod=%d harts/core=%d\n",

                    total_harts_hw, numPXN(), pods_per_pxn, cores_per_pod, harts_per_core);

        std::printf("Using total_threads=%d\n", total_threads);

        std::printf("ID EXAMPLE: pxn=%d pod=%d core=%d hart=%d => tid_global=%d\n",

                    pxn_id, pod_in_pxn, core_in_pod, hart_in_core, tid_global);

    }



    // init arrays

    for (int i = tid_global; i < g_N; i += total_threads) {

        g_dist[i] = -1;

        g_frontier[i] = 0;

        g_next_frontier[i] = 0;

    }

    barrier(tid_global, total_threads);



    // init BFS source

    if (tid_global == 0) {

        const int source = 0;

        g_dist[source] = 0;

        g_frontier[source] = 1;



        g_bfs_done = 0;

        g_sum_dist = 0;

        g_reached  = 0;

        g_max_dist = 0;

    }

    barrier(tid_global, total_threads);



    const int dr[4] = {-1, 1, 0, 0};

    const int dc[4] = { 0, 0,-1, 1};



    int iter = 0;



    while (true) {

        // expand frontier

        for (int v = tid_global; v < g_N; v += total_threads) {

            if (!g_frontier[v]) continue;



            const int ur = v / g_C;

            const int uc = v % g_C;

            const int du = g_dist[v];



            for (int k = 0; k < 4; ++k) {

                const int vr = ur + dr[k];

                const int vc = uc + dc[k];

                if (vr < 0 || vr >= g_R || vc < 0 || vc >= g_C) continue;



                const int nv = id(vr, vc, g_C);



                // claim nv exactly once

                if (atomic_compare_and_swap_i32(&g_dist[nv], -1, du + 1) == -1) {

                    g_next_frontier[nv] = 1;

                }

            }

        }



        barrier(tid_global, total_threads);



        // swap frontiers + done check

        if (tid_global == 0) {

            int any = 0;

            for (int i = 0; i < g_N; ++i) {

                uint8_t v = g_next_frontier[i];

                g_frontier[i] = v;

                if (v) any = 1;

                g_next_frontier[i] = 0;

            }

            g_bfs_done = any ? 0 : 1;

        }



        barrier(tid_global, total_threads);



        if (g_bfs_done) break;

        ++iter;

    }



    // reductions

    long long local_sum = 0;

    int local_reached = 0;

    int local_max = 0;



    for (int i = tid_global; i < g_N; i += total_threads) {

        int d = g_dist[i];

        if (d >= 0) {

            local_reached++;

            local_sum += d;

            if (d > local_max) local_max = d;

        }

    }



    atomic_fetch_add_i64(&g_sum_dist, local_sum);

    atomic_fetch_add_i32(&g_reached, local_reached);



    int32_t old_max = g_max_dist;

    while (local_max > old_max) {

        int32_t prev = atomic_compare_and_swap_i32(&g_max_dist, old_max, local_max);

        if (prev == old_max) break;

        old_max = prev;

    }



    barrier(tid_global, total_threads);





    if (tid_global == 0) {

        std::printf("BFS done in %d iterations\n", iter);

        //std::printf("reached=%d\n", (int)g_reached);

        std::printf("max_dist=%d\n", (int)g_max_dist);

        std::printf("sum_dist=%lld\n", (long long)g_sum_dist);



        std::printf("hi!\n");

        // Expected closed-form for 4-neighbor grid BFS from (0,0):

        // dist(r,c) = r + c, reached = R*C,

        // max = (R-1)+(C-1),

        // sum = C*sum_{r=0..R-1} r + R*sum_{c=0..C-1} c

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



        std::printf(ok ? "PASS\n" : "FAIL\n");

    }



    if (tid_global == 0) {
	   
	   // --- START NEW VERIFICATION ---
        std::printf("Running Full Grid Verification...\n");
        
        int error_count = 0;
        int max_print_errors = 10; // Don't spam stdout if everything fails

        for (int r = 0; r < g_R; ++r) {
            for (int c = 0; c < g_C; ++c) {
                // 1. Calculate the exact array index
                int idx = r * g_C + c;

                // 2. Get the value your BFS calculated
                int actual_dist = g_dist[idx];

                // 3. Calculate the Ground Truth (Manhattan Distance)
                int expected_dist = r + c;

                // 4. Compare
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
	    std::printf("node values %d  %d  %d for 39 43 500\n",g_dist[39], g_dist[43],g_dist[500]);
            std::printf("ALL %d NODES PASSED.\n", g_N);
        } else {
            std::printf("VERIFICATION FAILED: Found %d errors.\n", error_count);
        }
        // --- END NEW VERIFICATION ---

        std::printf("DBG: after reductions barrier\n");
        std::fflush(stdout);
   	g_sim_exit = 1;
    }

     // ... after your printing block ...

    while (g_sim_exit == 0) {
        hartsleep(100);
    }

    return 0; // All threads return 0, allowing the sim to finish


}


