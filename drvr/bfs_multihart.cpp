// Level-synchronous BFS (NO work stealing)
// - Work distribution is imbalanced: odd-numbered cores get 2x the frontier slice of even cores
// FIXES:
//  1) Do not call barrier() until g_total_harts is published to all harts (g_initialized flag).
//  2) Use dense hid = core_id * harts_per_core + lane (NOT (core<<4)+lane), so barrier indexing is correct.

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

// -------------------- Runtime hardware config (shared) --------------------
__l2sp__ volatile int g_total_cores = 0;
__l2sp__ volatile int g_harts_per_core = 0;
__l2sp__ volatile int g_total_harts = 0;
__l2sp__ std::atomic<int> g_initialized = 0;

static inline int get_core_id() { return myCoreId(); }
static inline int get_lane_id() { return myThreadId(); }

// Dense thread id in [0, total_harts)
static inline int get_thread_id_dense() {
    return get_core_id() * (int)g_harts_per_core + get_lane_id();
}

// -------------------- Barrier --------------------
static constexpr int MAX_THREADS = 2048;
__l2sp__ static int64_t thread_phase_counter[MAX_THREADS];
__l2sp__ static volatile int64_t global_barrier_count = 0;
__l2sp__ static volatile int64_t global_barrier_phase = 0;

static inline void barrier() {
    const int harts = g_total_harts;
    const int hid = get_thread_id_dense();

    int64_t threads_cur_phase = thread_phase_counter[hid];
    int64_t old = atomic_fetch_add_i64(&global_barrier_count, 1);

    if (old == harts - 1) {
        atomic_swap_i64(&global_barrier_count, 0);
        atomic_fetch_add_i64(&global_barrier_phase, 1);
    } else {
        long w = 1;
        long wmax = 8 * 1024;
        while (global_barrier_phase == threads_cur_phase) {
            if (w < wmax) w <<= 1;
            hartsleep(w);
        }
    }

    thread_phase_counter[hid] = threads_cur_phase + 1;
}

// -------------------- Graph --------------------
static constexpr int ROWS = 100;
static constexpr int COLS = 100;
static constexpr int64_t N = int64_t(ROWS) * int64_t(COLS);

static inline int64_t id_of(int r, int c) { return int64_t(r) * COLS + c; }
static inline int row_of(int64_t id) { return int(id / COLS); }
static inline int col_of(int64_t id) { return int(id % COLS); }

// -------------------- BFS shared arrays --------------------
static uint32_t frontier_a[N];
static uint32_t frontier_b[N];

static volatile int64_t frontier_size = 0;
static volatile int64_t next_size = 0;

static volatile int64_t visited[N];
static int32_t dist_arr[N];

static uint32_t* frontier = frontier_a;
static uint32_t* next_frontier = frontier_b;

static volatile int64_t discovered = 0;

static inline bool claim_node(int64_t v) {
    const int64_t old = atomic_swap_i64(&visited[v], 1);
    return old == 0;
}

// -------------------- Weighted slice computation --------------------
static inline void weighted_frontier_slice(int64_t fsz, int64_t* out_begin, int64_t* out_end) {
    const int cores = g_total_cores;
    const int hpc   = g_harts_per_core;

    const int core = get_core_id();
    const int lane = get_lane_id(); // 0..hpc-1

    const int even = (cores + 1) / 2;
    const int odd  = cores / 2;
    const int total_w = even + 2 * odd;

    const int w_core = (core & 1) ? 2 : 1;

    const int ev_prefix = (core + 1) / 2;
    const int od_prefix = core / 2;
    const int w_prefix = ev_prefix + 2 * od_prefix;

    const int64_t core_begin = (fsz * int64_t(w_prefix)) / total_w;
    const int64_t core_end   = (fsz * int64_t(w_prefix + w_core)) / total_w;
    const int64_t core_len   = core_end - core_begin;

    const int64_t begin = core_begin + (core_len * int64_t(lane)) / hpc;
    const int64_t end   = core_begin + (core_len * int64_t(lane + 1)) / hpc;

    *out_begin = begin;
    *out_end = end;
}

// -------------------- BFS --------------------
static void bfs(int64_t source_id) {
    const int hid = get_thread_id_dense();

    // One-time init by hart0
    if (hid == 0) {
        // Reset barrier bookkeeping for exactly g_total_harts participants
        for (int i = 0; i < g_total_harts; i++) thread_phase_counter[i] = 0;
        global_barrier_count = 0;
        global_barrier_phase = 0;

        for (int64_t i = 0; i < N; i++) {
            visited[i] = 0;
            dist_arr[i] = -1;
        }

        visited[source_id] = 1;
        dist_arr[source_id] = 0;
        frontier[0] = (uint32_t)source_id;

        frontier_size = 1;
        next_size = 0;
        discovered = 1;

        std::printf("BFS start: source=%ld (r=%d,c=%d), N=%ld, threads=%d, cores=%d, harts/core=%d\n",
                    (long)source_id, row_of(source_id), col_of(source_id),
                    (long)N, (int)g_total_harts, (int)g_total_cores, (int)g_harts_per_core);
        std::printf("Work imbalance: odd cores get 2x the work of even cores (by frontier slice size)\n");
    }

    barrier();

    int32_t level = 0;

    while (true) {
        barrier();

        const int64_t fsz = frontier_size;

        if (hid == 0) {
            std::printf("[L%d] frontier_size=%ld\n", level, (long)fsz);
        }

        if (fsz == 0) break;

        int64_t begin = 0, end = 0;
        weighted_frontier_slice(fsz, &begin, &end);

        for (int64_t i = begin; i < end; i++) {
            const int64_t u = frontier[i];
            const int ur = row_of(u);
            const int uc = col_of(u);

            if (ur > 0) {
                const int64_t v = u - COLS;
                if (claim_node(v)) {
                    dist_arr[v] = level + 1;
                    const int64_t idx = atomic_fetch_add_i64(&next_size, 1);
                    next_frontier[idx] = (uint32_t)v;
                    atomic_fetch_add_i64(&discovered, 1);
                }
            }
            if (ur + 1 < ROWS) {
                const int64_t v = u + COLS;
                if (claim_node(v)) {
                    dist_arr[v] = level + 1;
                    const int64_t idx = atomic_fetch_add_i64(&next_size, 1);
                    next_frontier[idx] = (uint32_t)v;
                    atomic_fetch_add_i64(&discovered, 1);
                }
            }
            if (uc > 0) {
                const int64_t v = u - 1;
                if (claim_node(v)) {
                    dist_arr[v] = level + 1;
                    const int64_t idx = atomic_fetch_add_i64(&next_size, 1);
                    next_frontier[idx] = (uint32_t)v;
                    atomic_fetch_add_i64(&discovered, 1);
                }
            }
            if (uc + 1 < COLS) {
                const int64_t v = u + 1;
                if (claim_node(v)) {
                    dist_arr[v] = level + 1;
                    const int64_t idx = atomic_fetch_add_i64(&next_size, 1);
                    next_frontier[idx] = (uint32_t)v;
                    atomic_fetch_add_i64(&discovered, 1);
                }
            }
        }

        barrier();

        if (hid == 0) {
            const int64_t new_fsz = atomic_swap_i64(&next_size, 0);

            std::printf("  -> produced next_size=%ld\n", (long)new_fsz);

            uint32_t* tmp = frontier;
            frontier = next_frontier;
            next_frontier = tmp;

            frontier_size = new_fsz;
            level++;
        }

        barrier();
    }

    barrier();

    if (hid == 0) {
        std::printf("BFS done. Levels=%d, discovered=%ld (grid should reach %ld)\n",
                    level, (long)discovered, (long)N);
        const int64_t far = id_of(ROWS - 1, COLS - 1);
        std::printf("dist[(%d,%d)] = %d (expected %d)\n",
                    ROWS - 1, COLS - 1, dist_arr[far], (ROWS - 1) + (COLS - 1));
    }
}

int main(int argc, char** argv) {
    const int core = get_core_id();
    const int lane = get_lane_id();

    // Publish config from (core0,lane0)
    if (core == 0 && lane == 0) {
        g_total_cores = numPodCores();
        g_harts_per_core = myCoreThreads();
        g_total_harts = g_total_cores * g_harts_per_core;
        g_initialized.store(1, std::memory_order_release);
    } else {
        while (g_initialized.load(std::memory_order_acquire) == 0) {
            hartsleep(10);
        }
    }

    // Now g_total_harts is valid for everyone; safe to use barrier.
    bfs(id_of(0, 0));
    return 0;
}