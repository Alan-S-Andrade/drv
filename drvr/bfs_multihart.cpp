// Level-synchronous BFS on a grid, MULTI-CORE + MULTIHART
// - Global hart id = myCoreId()*HARTS_PER_CORE + myThreadId()
// - Barrier + per-hart phase counters sized for TOTAL_HARTS at runtime
// - Same algorithm: slice frontier, claim with amoswap.d, append with amoadd.d

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <pandohammer/mmio.h>
#include <pandohammer/cpuinfo.h>
#include <pandohammer/atomic.h>
#include <pandohammer/hartsleep.h>

// -------- runtime config + barrier state --------
static volatile int64_t g_ready = 0;
static volatile int64_t g_total_harts = 0;
static volatile int64_t g_harts_per_core = 0;

static volatile int64_t g_barrier_count = 0;
static volatile int64_t g_barrier_phase = 0;
static int64_t* g_phase_ctr = nullptr;

static inline int total_harts() { return (int)g_total_harts; }
static inline int harts_per_core() { return (int)g_harts_per_core; }

static inline int global_hid() {
    return (int)(myCoreId() * (uint64_t)harts_per_core() + myThreadId());
}

static inline void wait_ready() {
    while (atomic_load_i64((volatile int64_t*)&g_ready) == 0) {
        hartsleep(128);
    }
}

static inline void barrier() {
    wait_ready();
    const int th  = total_harts();
    const int hid = global_hid();

    if (hid < 0 || hid >= th || g_phase_ctr == nullptr) {
        // Bad mapping/config → don't corrupt state, just park.
        while (1) { hartsleep(1024); }
    }

    const int64_t cur = g_phase_ctr[hid];

    const int64_t old = atomic_fetch_add_i64((volatile int64_t*)&g_barrier_count, 1);
    if (old == (int64_t)th - 1) {
        atomic_swap_i64((volatile int64_t*)&g_barrier_count, 0);
        atomic_fetch_add_i64((volatile int64_t*)&g_barrier_phase, 1);
    } else {
        long w = 1;
        long wmax = 8 * 1024;
        while (atomic_load_i64((volatile int64_t*)&g_barrier_phase) == cur) {
            if (w < wmax) w <<= 1;
            hartsleep(w);
        }
    }

    g_phase_ctr[hid] = cur + 1;
}

// -------------------- grid graph --------------------
static constexpr int ROWS = 100;
static constexpr int COLS = 1000;
static constexpr int64_t N = int64_t(ROWS) * int64_t(COLS);

static inline int64_t id_of(int r, int c) { return int64_t(r) * COLS + c; }
static inline int row_of(int64_t id) { return int(id / COLS); }
static inline int col_of(int64_t id) { return int(id % COLS); }

// -------------------- BFS shared arrays --------------------
static uint32_t frontier_a[N];
static uint32_t frontier_b[N];

static volatile int64_t frontier_size = 0;
static volatile int64_t next_size = 0;

static volatile int64_t visited[N]; // 0/1
static int32_t dist_arr[N];

static uint32_t* frontier = frontier_a;
static uint32_t* next_frontier = frontier_b;

static volatile int64_t discovered = 0;

static inline bool claim_node(int64_t v) {
    const int64_t old = atomic_swap_i64(&visited[v], 1);
    return old == 0;
}

// -------------------- BFS --------------------
static void bfs_multicore(int64_t source_id) {
    const int hid = global_hid();
    const int th  = total_harts();

    // One-time init by global hart0 (core0,tid0)
    if (hid == 0) {
        // Init visited/dist
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

        std::printf("BFS start: source=%ld (r=%d,c=%d) N=%ld total_harts=%d\n",
                    (long)source_id, row_of(source_id), col_of(source_id), (long)N, th);
    }

    barrier();

    int32_t level = 0;

    while (true) {
        barrier();

        const int64_t fsz = frontier_size;
        if (fsz == 0) break;

        // Slice frontier among ALL harts across cores
        const int64_t begin = (fsz * hid) / th;
        const int64_t end   = (fsz * (hid + 1)) / th;

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

            uint32_t* tmp = frontier;
            frontier = next_frontier;
            next_frontier = tmp;

            frontier_size = new_fsz;

            std::printf("level=%d next_frontier_size=%ld discovered=%ld\n",
                        level, (long)new_fsz, (long)discovered);

            level++;
        }

        barrier();
    }

    barrier();

    if (hid == 0) {
        std::printf("BFS done. Levels=%d discovered=%ld (grid should reach %ld)\n",
                    level, (long)discovered, (long)N);
        const int64_t far = id_of(ROWS - 1, COLS - 1);
        std::printf("dist[(%d,%d)] = %d (expected %d)\n",
                    ROWS - 1, COLS - 1, dist_arr[far], (ROWS - 1) + (COLS - 1));
    }

    barrier();
}

int main(int argc, char** argv) {
    // Args: <total_harts> <harts_per_core>
    // Example: 8 cores * 16 harts/core = 128 =>  ./bfs 128 16
    const bool is_global0 = (myCoreId() == 0 && myThreadId() == 0);

    if (is_global0) {
        const int th  = 64;
        const int hpc = 16;
        if (th <= 0 || hpc <= 0) {
            std::printf("Bad args: total_harts=%d harts_per_core=%d\n", th, hpc);
            std::exit(1);
        }

        atomic_swap_i64((volatile int64_t*)&g_total_harts, (int64_t)th);
        atomic_swap_i64((volatile int64_t*)&g_harts_per_core, (int64_t)hpc);

        g_phase_ctr = (int64_t*)std::malloc((size_t)th * sizeof(int64_t));
        if (!g_phase_ctr) {
            std::printf("malloc g_phase_ctr failed\n");
            std::exit(1);
        }
        for (int i = 0; i < th; i++) g_phase_ctr[i] = 0;

        // Release everyone LAST
        atomic_swap_i64((volatile int64_t*)&g_ready, 1);

        std::printf("BFS multicore init: total_harts=%d harts_per_core=%d\n", th, hpc);
    }

    bfs_multicore(id_of(0, 0));

    // Cleanup by global0 (optional)
    if (is_global0) {
        std::free(g_phase_ctr);
        g_phase_ctr = nullptr;
    }
    return 0;
}
