// bfs_sw_hw_multi_fixed.cpp
// Self-checking multihart BFS on a 2D grid (4-neighbor).
// Prints PASS/FAIL based on closed-form expected results.
//
// Key fixes:
// 1) Barrier uses atomic reset (CAS loop) + per-thread local phase indexed by global tid.
// 2) BFS discovery uses atomic CAS on g_dist to avoid races.
// 3) Only tid==0 prints.
// 4) Removes redundant end-of-program abort logic.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <pandohammer/cpuinfo.h>   // myThreadId(), numPodCores(), myCoreThreads()
#include <pandohammer/atomic.h>    // atomic_fetch_add_i64, atomic_fetch_add_i32, atomic_compare_and_swap_i32, atomic_load_i64
#include <pandohammer/hartsleep.h> // hartsleep()

// ----------------- helpers -----------------

static int parse_i(const char* s, int d) {
    if (!s) return d;
    char* e = nullptr;
    long v = strtol(s, &e, 10);
    return (e && *e == 0) ? (int)v : d;
}

static inline int id(int r, int c, int C) { return r * C + c; }

// ----------------- barrier -----------------

static const int MAX_THREADS = 512; // must be >= maximum participating threads
static int64_t g_local_phase_arr[MAX_THREADS];

static volatile int64_t g_barrier_count = 0;
static volatile int64_t g_barrier_phase = 0;

// Atomic store using CAS loop (in case atomic_store_i64 isn't available)
static inline void atomic_store_i64_cas(volatile int64_t* p, int64_t v) {
    while (true) {
        int64_t old = atomic_load_i64(p);
        // We don't have compare_and_swap_i64 in your includes, but many atomic.h do.
        // If yours doesn't, replace with atomic_compare_and_swap_i64.
        // We'll attempt a best-effort using atomic_fetch_add_i64 when v==0 and old>0 is unsafe,
        // so we rely on CAS being present.
        // ----
        // If atomic_compare_and_swap_i64 is not defined in your pandohammer/atomic.h,
        // tell me and I’ll provide the exact replacement available in your repo.
        // ----
#ifdef atomic_compare_and_swap_i64
        int64_t prev = atomic_compare_and_swap_i64(p, old, v);
        if (prev == old) return;
#else
        // Fallback: if your atomic.h truly lacks CAS64, this is NOT safe.
        // We'll still do a plain store, but you should replace with the correct primitive.
        *p = v;
        return;
#endif
    }
}

static inline void barrier(int tid, int total_threads) {
    // local phase (per participating thread)
    int64_t my_phase = g_local_phase_arr[tid];

    // arrive
    int64_t old = atomic_fetch_add_i64(&g_barrier_count, 1);

    if (old == total_threads - 1) {
        // last thread: reset count and advance phase
        atomic_store_i64_cas(&g_barrier_count, 0);
        atomic_fetch_add_i64(&g_barrier_phase, 1);
    } else {
        // wait for phase to change
        long w = 1;
        long wmax = 8 * 1024;
        while (atomic_load_i64(&g_barrier_phase) == my_phase) {
            if (w < wmax) w <<= 1;
            hartsleep(w);
        }
    }

    g_local_phase_arr[tid] = my_phase + 1;
}

// ----------------- BFS state -----------------

static const int MAXN = 250 * 350; // adjust as needed

static int g_R, g_C, g_N;

static int32_t g_dist[MAXN];
static uint8_t g_frontier[MAXN];
static uint8_t g_next_frontier[MAXN];

static volatile int g_bfs_done = 0;

// Reductions
static volatile int64_t g_sum_dist = 0;
static volatile int32_t g_reached  = 0;
static volatile int32_t g_max_dist = 0;

// ----------------- main -----------------

extern "C" int main(int argc, char** argv) {
    int R = 100;
    int C = 32;
    int desired_threads = 64; // will be clamped to available harts

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--R") && i + 1 < argc) {
            R = parse_i(argv[++i], R);
        } else if (!strcmp(argv[i], "--C") && i + 1 < argc) {
            C = parse_i(argv[++i], C);
        } else if (!strcmp(argv[i], "--T") && i + 1 < argc) {
            desired_threads = parse_i(argv[++i], desired_threads);
        } else if (!strcmp(argv[i], "--help")) {
            // NOTE: multiple harts may call this; it’s fine.
            std::printf("Usage: %s --R <rows> --C <cols> --T <threads>\n", argv[0]);
            return 0;
        }
    }

    g_R = R;
    g_C = C;
    g_N = R * C;

    if (g_N > MAXN) {
        // multiple harts may print unless you guard; keep simple for now
        std::printf("N=%d exceeds MAXN=%d\n", g_N, MAXN);
        return 1;
    }

    int harts_per_core = myCoreThreads();
    int num_cores      = numPodCores();
    int total_harts_hw = num_cores * harts_per_core;

    int total_threads = desired_threads;
    if (total_threads > total_harts_hw) total_threads = total_harts_hw;
    if (total_threads > MAX_THREADS) total_threads = MAX_THREADS;

    // Raw id from runtime
    int raw = myThreadId();

    // Try to derive core/hart mapping from raw id.
    // If raw is already global 0..total_harts_hw-1, this works.
    // If raw is only local-within-core, this will NOT work; we’ll detect via debug print.
    int core_id      = raw / harts_per_core;
    int hart_in_core = raw % harts_per_core;

    int tid = core_id * harts_per_core + hart_in_core;

    // Park extra harts (non-participants)
    if (tid < 0 || tid >= total_threads) {
        while (true) { hartsleep(1 << 20); }
    }

    if (tid == 0) {
        std::printf("Grid BFS: R=%d C=%d N=%d\n", R, C, g_N);
        std::printf("HW: total_harts=%d, cores=%d, harts_per_core=%d\n",
                    total_harts_hw, num_cores, harts_per_core);
        std::printf("Using total_threads=%d software threads (1:1 with harts)\n",
                    total_threads);

        // Mapping sanity line (helps confirm myThreadId semantics)
        std::printf("MAPPING: raw=%d -> core_id=%d hart_in_core=%d tid=%d\n",
                    raw, core_id, hart_in_core, tid);
    }

    // Initialize arrays in parallel
    for (int i = tid; i < g_N; i += total_threads) {
        g_dist[i] = -1;
        g_frontier[i] = 0;
        g_next_frontier[i] = 0;
    }
    barrier(tid, total_threads);

    // Source init by tid==0
    if (tid == 0) {
        int source = 0;
        g_dist[source] = 0;
        g_frontier[source] = 1;

        g_bfs_done = 0;
        g_sum_dist = 0;
        g_reached  = 0;
        g_max_dist = 0;
    }
    barrier(tid, total_threads);

    const int dr[4] = {-1, 1, 0, 0};
    const int dc[4] = { 0, 0,-1, 1};

    int iter = 0;

    while (true) {
        // Expand frontier in parallel
        for (int v = tid; v < g_N; v += total_threads) {
            if (!g_frontier[v]) continue;

            int ur = v / g_C;
            int uc = v % g_C;
            int du = g_dist[v];

            for (int k = 0; k < 4; ++k) {
                int vr = ur + dr[k];
                int vc = uc + dc[k];
                if (vr < 0 || vr >= g_R || vc < 0 || vc >= g_C) continue;

                int nv = id(vr, vc, g_C);

                // Atomic discovery: only one thread claims
                if (atomic_compare_and_swap_i32(&g_dist[nv], -1, du + 1) == -1) {
                    g_next_frontier[nv] = 1;
                }
            }
        }

        barrier(tid, total_threads);

        // Frontier swap + done check (single-thread)
        if (tid == 0) {
            int any = 0;
            for (int i = 0; i < g_N; ++i) {
                uint8_t v = g_next_frontier[i];
                g_frontier[i] = v;
                if (v) any = 1;
                g_next_frontier[i] = 0;
            }
            g_bfs_done = any ? 0 : 1;
        }

        barrier(tid, total_threads);

        if (g_bfs_done) break;
        ++iter;
    }

    // Reductions in parallel
    long long local_sum = 0;
    int local_reached = 0;
    int local_max = 0;

    for (int i = tid; i < g_N; i += total_threads) {
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

    barrier(tid, total_threads);

    if (tid == 0) {
        std::printf("BFS done in %d iterations\n", iter);
        std::printf("reached=%d\n", (int)g_reached);
        std::printf("max_dist=%d\n", (int)g_max_dist);
        std::printf("sum_dist=%lld\n", (long long)g_sum_dist);

        // Closed-form expected results for grid BFS from (0,0)
        long long exp_reached = 1LL * g_R * g_C;
        int exp_max = (g_R - 1) + (g_C - 1);

        long long sum_r = 1LL * (g_R - 1) * g_R / 2;
        long long sum_c = 1LL * (g_C - 1) * g_C / 2;
        long long exp_sum = 1LL * g_C * sum_r + 1LL * g_R * sum_c;

        std::printf("EXPECTED reached=%lld max_dist=%d sum_dist=%lld\n",
                    exp_reached, exp_max, exp_sum);

        bool ok = true;
        ok &= ((long long)g_reached == exp_reached);
        ok &= ((int)g_max_dist == exp_max);
        ok &= ((long long)g_sum_dist == exp_sum);

        // Spot checks
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

    return 0;
}

