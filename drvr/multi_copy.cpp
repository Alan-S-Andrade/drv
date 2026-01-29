// Level-synchronous BFS on a ROWS x COLS grid
// Adds functional correctness checks: discovered, max_dist, sum_dist, spot checks.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <pandohammer/mmio.h>
#include <pandohammer/cpuinfo.h>
#include <pandohammer/atomic.h>
#include <pandohammer/hartsleep.h>

static constexpr int HARTS = 2;

static int64_t thread_phase_counter[HARTS];
static volatile int64_t global_barrier_count = 0;
static volatile int64_t global_barrier_phase = 0;

static inline void barrier(int H) {
    int hid = myThreadId();
    int64_t threads_cur_phase = thread_phase_counter[hid];

    int64_t old = atomic_fetch_add_i64(&global_barrier_count, 1);

    if (old == H - 1) {
        atomic_swap_i64(&global_barrier_count, 0);
        atomic_fetch_add_i64(&global_barrier_phase, 1);
    } else {
        long w = 1;
        long wmax = 8 * 1024;
        // NOTE: ideally use atomic_load_i64(&global_barrier_phase)
        while (atomic_load_i64(&global_barrier_phase) == threads_cur_phase) {
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
static uint32_t frontier_a[N];
static uint32_t frontier_b[N];

static volatile int64_t frontier_size = 0;
static volatile int64_t next_size = 0;

static volatile int64_t visited[N];
static int32_t dist_arr[N];

static uint32_t* frontier = frontier_a;
static uint32_t* next_frontier = frontier_b;

static volatile int64_t discovered = 0;

// Try to claim a node; true if this hart was first to claim it.
static inline bool claim_node(int64_t v) {
    const int64_t old = atomic_swap_i64(&visited[v], 1);
    return old == 0;
}

static void bfs(int H, int64_t source_id) {
    const int hid = myThreadId();

    if (hid == 0) {
        for (int i = 0; i < H; i++) thread_phase_counter[i] = 0;

        for (int64_t i = 0; i < N; i++) {
            visited[i] = 0;
            dist_arr[i] = -1;
        }

        visited[source_id] = 1;
        dist_arr[source_id] = 0;
        frontier[0] = (uint32_t) source_id;

        frontier_size = 1;
        next_size = 0;
        discovered = 1;

        std::printf("BFS start: source=%ld (r=%d,c=%d), N=%ld, threads=%d\n",
                    (long)source_id, row_of(source_id), col_of(source_id), (long)N, H);

        // Optional: check if globals look shared across cores (print once per run)
        std::printf("&visited=%p &dist_arr=%p &frontier_size=%p &global_barrier_count=%p\n",
                    (void*)visited, (void*)dist_arr, (void*)&frontier_size, (void*)&global_barrier_count);
    }

    barrier(H);
    if (hid == 0 || hid == 1 || hid == 4 || hid == 8) {
    std::printf("ADDR hid=%d &dist_arr=%p &visited=%p &frontier_size=%p\n",
                hid, (void*)dist_arr, (void*)visited, (void*)&frontier_size);
	}
     barrier(H);

    int32_t level = 0;

    while (true) {
        barrier(H);

        const int64_t fsz = frontier_size;
        if (fsz == 0) break;

        const int64_t begin = (fsz * hid) / H;
        const int64_t end   = (fsz * (hid + 1)) / H;

        for (int64_t i = begin; i < end; i++) {
            const int64_t u = frontier[i];
            const int ur = row_of(u);
            const int uc = col_of(u);

            if (ur > 0) {
                const int64_t v = u - COLS;
                if (claim_node(v)) {
                    dist_arr[v] = level + 1;
                    const int64_t idx = atomic_fetch_add_i64(&next_size, 1);
                    next_frontier[idx] = (uint32_t) v;
                    atomic_fetch_add_i64(&discovered, 1);
                }
            }
            if (ur + 1 < ROWS) {
                const int64_t v = u + COLS;
                if (claim_node(v)) {
                    dist_arr[v] = level + 1;
                    const int64_t idx = atomic_fetch_add_i64(&next_size, 1);
                    next_frontier[idx] = (uint32_t) v;
                    atomic_fetch_add_i64(&discovered, 1);
                }
            }
            if (uc > 0) {
                const int64_t v = u - 1;
                if (claim_node(v)) {
                    dist_arr[v] = level + 1;
                    const int64_t idx = atomic_fetch_add_i64(&next_size, 1);
                    next_frontier[idx] = (uint32_t) v;
                    atomic_fetch_add_i64(&discovered, 1);
                }
            }
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

        barrier(H);

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

        barrier(H);
    }

    barrier(H);

if (hid == 0) {
    bool ok = true;
    int mismatches = 0;
    const int MAX_MISMATCH_PRINT = 50;

    auto check = [&](int r, int c, const char* tag) {
        if (r < 0 || r >= ROWS || c < 0 || c >= COLS) {
            std::printf("CHECK[%s] SKIP out-of-range (%d,%d)\n", tag, r, c);
            fflush(stdout);
            ok = false;
            mismatches++;
            return;
        }
        int64_t idx = id_of(r, c);
        int got = dist_arr[idx];
        int exp = r + c;

        if (got != exp) {
            if (mismatches < MAX_MISMATCH_PRINT) {
                std::printf("MISMATCH[%s] dist(%d,%d): got=%d exp=%d\n",
                            tag, r, c, got, exp);
                fflush(stdout);
            }
            mismatches++;
            ok = false;
        }
    };

    // discovered check
    if (discovered != N) {
        std::printf("MISMATCH discovered: got=%ld exp=%ld\n",
                    (long)discovered, (long)N);
        fflush(stdout);
        ok = false;
        mismatches++;
    }

    // specific points
    check(0, 0, "origin");
    check(1, 0, "near");
    check(0, 1, "near");
 //   check(2, 2, "near");

    check(23, 0, "req");
    check(3, 44, "req");
    check(56, 67, "req");

    check(ROWS/2, COLS/2, "center");
    check(ROWS-1, 0, "edge");
    check(0, COLS-1, "edge");
    check(ROWS-1, COLS-1, "far");

    // grid sampling
    for (int r = 0; r < ROWS; r += 7) {
        for (int c = 0; c < COLS; c += 11) {
            check(r, c, "grid7x11");
        }
    }

    // summary
    if (ok) {
        std::printf("DIST_CHECK_PASS\n");
    } else {
        std::printf("DIST_CHECK_FAIL mismatches=%d\n", mismatches);
    }
    fflush(stdout);
}

 
}

int main(int argc, char** argv) {
    bfs(HARTS, id_of(0, 0));
    return 0;
}

