// Level-synchronous BFS on a 1,000 x 1,000 graph
// - Each level: each hart processes a disjoint slice of frontier
// - Neighbors are "claimed" via visited[v] = amoswap.d(&visited[v], 1)
// - Appending to next_frontier uses amoadd.d on next_size to reserve a slot

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <pandohammer/mmio.h>
#include <pandohammer/cpuinfo.h>
#include <pandohammer/atomic.h>   
#include <pandohammer/hartsleep.h>

static constexpr int HARTS = 16;

static int64_t thread_phase_counter[HARTS];
static volatile int64_t global_barrier_count = 0;
static volatile int64_t global_barrier_phase = 0;

static inline void barrier(int HARTS) {
    int hid = myThreadId();
    int64_t threads_cur_phase = thread_phase_counter[hid];

    int64_t old = atomic_fetch_add_i64(&global_barrier_count, 1);

    if (old == HARTS - 1) {
        // Last: reset count and advance phase
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

static constexpr int ROWS = 100;
static constexpr int COLS = 100;
static constexpr int64_t N = int64_t(ROWS) * int64_t(COLS);

static inline int64_t id_of(int r, int c) { return int64_t(r) * COLS + c; }
static inline int row_of(int64_t id) { return int(id / COLS); }
static inline int col_of(int64_t id) { return int(id % COLS); }

// -------------------- BFS shared arrays --------------------
// Frontiers hold node IDs (fit in 32 bits for 1M nodes).
static uint32_t frontier_a[N];
static uint32_t frontier_b[N];

// Sizes are shared; updated by hart0 and by amoadd for next frontier allocation.
static volatile int64_t frontier_size = 0;
static volatile int64_t next_size = 0;

// visited is 64-bit so we can use amoswap.d directly. 1 = visited
static volatile int64_t visited[N];

// distance array is written only by the winner that successfully claims visited[v]=1.
// Others will see visited already 1 and skip writing dist[v].
static int32_t dist_arr[N];

// Pointers to current/next frontier arrays. Swapped by hart0 after each level.
static uint32_t* frontier = frontier_a;
static uint32_t* next_frontier = frontier_b;

// Discovered count for progress
static volatile int64_t discovered = 0;

// Try to claim a node; true if this hart was first to claim it.
static inline bool claim_node(int64_t v) {
    // amoswap.d: set visited[v]=1, get old value
    const int64_t old = atomic_swap_i64(&visited[v], 1);
    return old == 0;
}

// -------------------- BFS --------------------
static void bfs(int HARTS, int64_t source_id) {
    const int hid = myThreadId();

    // One-time init by hart0
    if (hid == 0) {
        for (int i = 0; i < HARTS; i++) thread_phase_counter[i] = 0;

        // Init visited/dist
        for (int64_t i = 0; i < N; i++) {
            visited[i] = 0;
            dist_arr[i] = -1;
        }

        // Seed
        visited[source_id] = 1;
        dist_arr[source_id] = 0;
        frontier[0] = (uint32_t) source_id;

        frontier_size = 1;
        next_size = 0;
        discovered = 1;

        std::printf("BFS start: source=%ld (r=%d,c=%d), N=%ld, threads=%d\n",
                    (long)source_id, row_of(source_id), col_of(source_id), (long)N, HARTS);
    }

    barrier(HARTS);

    int32_t level = 0;

    while (true) {
        barrier(HARTS);

        // We don't use atomic_load; frontier_size is volatile and only hart0 writes it between barriers.
        const int64_t fsz = frontier_size;
        if (fsz == 0) break;

        // Slice current frontier among harts
        const int64_t begin = (fsz * hid) / HARTS;
        const int64_t end   = (fsz * (hid + 1)) / HARTS;

        for (int64_t i = begin; i < end; i++) {
            const int64_t u = frontier[i];
            const int ur = row_of(u);
            const int uc = col_of(u);

            // Up
            if (ur > 0) {
                const int64_t v = u - COLS;
                if (claim_node(v)) {
                    dist_arr[v] = level + 1;
                    const int64_t idx = atomic_fetch_add_i64(&next_size, 1); // amoadd.d
                    next_frontier[idx] = (uint32_t) v;
                    atomic_fetch_add_i64(&discovered, 1); // amoadd.d (optional)
                }
            }
            // Down
            if (ur + 1 < ROWS) {
                const int64_t v = u + COLS;
                if (claim_node(v)) {
                    dist_arr[v] = level + 1;
                    const int64_t idx = atomic_fetch_add_i64(&next_size, 1);
                    next_frontier[idx] = (uint32_t) v;
                    atomic_fetch_add_i64(&discovered, 1);
                }
            }
            // Left
            if (uc > 0) {
                const int64_t v = u - 1;
                if (claim_node(v)) {
                    dist_arr[v] = level + 1;
                    const int64_t idx = atomic_fetch_add_i64(&next_size, 1);
                    next_frontier[idx] = (uint32_t) v;
                    atomic_fetch_add_i64(&discovered, 1);
                }
            }
            // Right
            if (uc + 1 < COLS) {
                const int64_t v = u + 1;
                if (claim_node(v)) {
                    dist_arr[v] = level + 1;
                    const int64_t idx = atomic_fetch_add_i64(&next_size, 1);
                    next_frontier[idx] = (uint32_t) v;
                    atomic_fetch_add_i64(&discovered, 1);
                }
            }
        }

        // Ensure all harts finished producing next_frontier
        barrier(HARTS);

        if (hid == 0) {
            // next_size was incremented via amoadd.d by all harts; grab final size and reset to 0.
            const int64_t new_fsz = atomic_swap_i64(&next_size, 0); // amoswap.d

            // Swap frontier buffers
            uint32_t* tmp = frontier;
            frontier = next_frontier;
            next_frontier = tmp;

            frontier_size = new_fsz;

            std::printf("level=%d next_frontier_size=%ld discovered=%ld\n", level, (long) new_fsz, (long) discovered);

            level++;
        }

        barrier(HARTS);
    }

    barrier(HARTS);

    if (hid == 0) {
        std::printf("BFS done. Levels=%d, discovered=%ld (grid should reach %ld)\n", level, (long) discovered, (long) N);
        const int64_t far = id_of(ROWS - 1, COLS - 1);
        std::printf("dist[(%d,%d)] = %d (expected %d)\n", ROWS - 1, COLS - 1, dist_arr[far], (ROWS - 1) + (COLS - 1));
    }
}

int main(int argc, char** argv) {
    bfs(HARTS, id_of(0, 0));
    return 0;
}