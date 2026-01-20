// Multihart PageRank on a 1000x1000 grid
//
// Graph model:
//   - Nodes are a 2D grid: (r,c) -> id = r*COLS + c, with ROWS=COLS=1000 => N=1,000,000
//   - Treat edges as *bidirectional* between grid neighbors (up/down/left/right).
//     (So it's an undirected grid viewed as a directed graph with symmetric edges.)
//
// PageRank model (pull formulation, no atomics needed for rank updates):
//   PR_next[v] = (1-d)/N + d * sum_{u in InNbrs(v)} PR_curr[u] / outdeg(u)
//
// Parallelization strategy:
//   - Each iteration: each hart computes a disjoint slice of vertices [begin,end)
//   - Writes only PR_next[v] for v in its slice (no contention)
//   - Barrier between phases; hart0 swaps buffers and optionally prints convergence stats
//
// Notes:
//   - Using pull avoids atomic adds into shared PR_next[v].
//   - outdeg(u) is the grid degree (2,3,4) computed on the fly.
//   - Convergence measured as L1 diff sum |PR_next-PR_curr| via per-hart scratch + reduction.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>

#include <pandohammer/mmio.h>
#include <pandohammer/cpuinfo.h>
#include <pandohammer/atomic.h>
#include <pandohammer/hartsleep.h>

static constexpr int HARTS = 16;

static int64_t thread_phase_counter[HARTS];
static volatile int64_t global_barrier_count = 0;
static volatile int64_t global_barrier_phase = 0;

static inline void barrier(int total_harts) {
    const int hid = myThreadId();
    const int64_t cur = thread_phase_counter[hid];

    const int64_t old = atomic_fetch_add_i64(&global_barrier_count, 1);

    if (old == total_harts - 1) {
        atomic_swap_i64(&global_barrier_count, 0);
        atomic_fetch_add_i64(&global_barrier_phase, 1);
    } else {
        long w = 1;
        long wmax = 8 * 1024;
        while (global_barrier_phase == cur) {
            if (w < wmax) w <<= 1;
            hartsleep(w);
        }
    }

    thread_phase_counter[hid] = cur + 1;
}

// -------------------- Grid graph helpers --------------------
static constexpr int ROWS = 1000;
static constexpr int COLS = 1000;
static constexpr int64_t N = int64_t(ROWS) * int64_t(COLS);

static inline int64_t id_of(int r, int c) { return int64_t(r) * COLS + c; }
static inline int row_of(int64_t id) { return int(id / COLS); }
static inline int col_of(int64_t id) { return int(id % COLS); }

static inline int outdeg_of_rc(int r, int c) {
    int deg = 0;
    if (r > 0) deg++;
    if (r + 1 < ROWS) deg++;
    if (c > 0) deg++;
    if (c + 1 < COLS) deg++;
    return deg; // 2,3,4 (corners/edges/interior)
}

static inline int outdeg_of_id(int64_t u) {
    return outdeg_of_rc(row_of(u), col_of(u));
}

// -------------------- PageRank arrays --------------------
static double pr_a[N];
static double pr_b[N];

static double* pr_curr = pr_a;
static double* pr_next = pr_b;

// per-hart scratch for convergence stats
static double hart_diff[HARTS];
static double hart_sum[HARTS];

static void pagerank(int total_harts,int iters, double damping) {
    const int hid = myThreadId();

    // Init by hart0
    if (hid == 0) {
        for (int i = 0; i < total_harts; i++) thread_phase_counter[i] = 0;

        const double init = 1.0 / double(N);
        for (int64_t i = 0; i < N; i++) {
            pr_curr[i] = init;
            pr_next[i] = 0.0;
        }

        std::printf("PageRank start: N=%ld (grid %dx%d), harts=%d, iters=%d, d=%g\n", (long)N, ROWS, COLS, total_harts, iters, damping);
    }

    barrier(total_harts);

    const double base = (1.0 - damping) / double(N);

    for (int iter = 0; iter < iters; iter++) {
        // Partition vertices among harts
        const int64_t begin = (N * hid) / total_harts;
        const int64_t end   = (N * (hid + 1)) / total_harts;

        double local_diff = 0.0;
        double local_sum  = 0.0;

        // Compute PR_next[v] for v in [begin,end)
        for (int64_t v = begin; v < end; v++) {
            const int vr = row_of(v);
            const int vc = col_of(v);

            double acc = 0.0;

            // In-neighbors are the same as grid neighbors (bidirectional edges).
            // Contribution from neighbor u is PR_curr[u] / outdeg(u).
            if (vr > 0) {
                const int64_t u = v - COLS;
                acc += pr_curr[u] / double(outdeg_of_id(u));
            }
            if (vr + 1 < ROWS) {
                const int64_t u = v + COLS;
                acc += pr_curr[u] / double(outdeg_of_id(u));
            }
            if (vc > 0) {
                const int64_t u = v - 1;
                acc += pr_curr[u] / double(outdeg_of_id(u));
            }
            if (vc + 1 < COLS) {
                const int64_t u = v + 1;
                acc += pr_curr[u] / double(outdeg_of_id(u));
            }

            const double newv = base + damping * acc;
            pr_next[v] = newv;

            local_diff += std::fabs(newv - pr_curr[v]);
            local_sum  += newv;
        }

        // Publish per-hart stats
        hart_diff[hid] = local_diff;
        hart_sum[hid]  = local_sum;

        barrier(total_harts);

        if (hid == 0) {
            // Reduce stats
            double diff = 0.0;
            double sum  = 0.0;
            for (int i = 0; i < total_harts; i++) {
                diff += hart_diff[i];
                sum  += hart_sum[i];
            }

            // Swap buffers
            double* tmp = pr_curr;
            pr_curr = pr_next;
            pr_next = tmp;

            // Optional: print a few iters + convergence
            // (sum should stay ~1.0 for PageRank; minor drift can happen due to FP rounding)
            if (iter < 10 || (iter % 10) == 0 || iter == iters - 1) {
                const int64_t tl = id_of(0, 0);
                const int64_t br = id_of(ROWS - 1, COLS - 1);
                std::printf("iter=%d L1diff=%e sum=%0.15f PR(0,0)=%e PR(%d,%d)=%e\n",
                            iter, diff, sum,
                            pr_curr[tl],
                            ROWS - 1, COLS - 1, pr_curr[br]);
            }
        }

        barrier(total_harts);

        // Reuse pr_next next iteration; no need to clear it because we overwrite every v in slice.
        // (All v are covered across harts each iter.)
    }

    barrier(total_harts);

    if (hid == 0) {
        // Quick sanity: report a couple nodes + checksum
        double sum = 0.0;
        for (int i = 0; i < HARTS; i++) sum += hart_sum[i]; // last iter’s sums are already computed
        std::printf("PageRank done. (last-iter sum across harts was ~%0.15f)\n", sum);
    }
}

int main(int argc, char** argv) {
    int iters = 50;
    double d = 0.85;

    pagerank(HARTS, iters, d);
    return 0;
}