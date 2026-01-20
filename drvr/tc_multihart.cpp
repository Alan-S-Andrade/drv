// Multihart triangle counting on a 1,000,000-node graph using a bit-matrix ("matrix-based")
//
// WARNING / reality check:
//   - A true dense N×N adjacency matrix for N=1,000,000 is 1e12 bits = 125 GB (bit-packed),
//     or far larger if not bit-packed. That is usually not feasible.
//   - So this is "matrix-based" in the sense of a *bitset row* adjacency representation for a
//     *sparse* graph (each row stores only neighbors, but as a dense bitset over N).
//     Memory cost is still huge: N * (N/64) * 8 bytes = ~125 GB.
//   - If you actually need 1M nodes on realistic memory, you typically use CSR + sorted
//     neighbor list intersection (not a full bit-matrix).
//
// What I provide below is a correct, multihart, "matrix/bitset intersection" triangle counter
// that matches your BFS/PageRank style (disjoint vertex slices + barriers + reductions).
//
// Graph used here: 1000x1000 undirected grid (degree <= 4).
// Triangle count on a 4-neighbor grid is 0 (no diagonals). That's fine as a sanity test.
// If you want nonzero triangles, add diagonals or use a different graph generator.
//
// Triangle counting method (undirected, simple graph):
//   - Orient edges by (u < v) to avoid double counting.
//   - For each oriented edge (u,v) with u < v, compute |N+(u) ∩ N+(v)| where N+(x) = {w > x and (x,w) is edge}.
//   - Sum intersections over oriented edges. This counts each triangle exactly once.
//   - Intersection uses bitset AND + popcount.
//
// Parallelization:
//   - Each hart owns a disjoint slice of u in [0,N).
//   - For each u in its slice, iterate v in N+(u), compute bitset_intersection_popcount(row[u], row[v]) masked to w>v.
//   - Use per-hart local sums and a final reduction via barriers.
//
// If memory is a problem, ask for CSR intersection version (recommended).

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <pandohammer/mmio.h>
#include <pandohammer/cpuinfo.h>
#include <pandohammer/atomic.h>
#include <pandohammer/hartsleep.h>

static constexpr int HARTS = 16;

// -------------------- Barrier --------------------
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

// -------------------- Graph: 1000x1000 grid --------------------
static constexpr int ROWS = 1000;
static constexpr int COLS = 1000;
static constexpr int64_t N = int64_t(ROWS) * int64_t(COLS);

static inline int64_t id_of(int r, int c) { return int64_t(r) * COLS + c; }
static inline int row_of(int64_t id) { return int(id / COLS); }
static inline int col_of(int64_t id) { return int(id % COLS); }

// -------------------- Bit-matrix adjacency (bitset rows) --------------------
// Each row is N bits, packed into 64-bit words.
static constexpr int64_t WORD_BITS = 64;
static constexpr int64_t WORDS_PER_ROW = (N + WORD_BITS - 1) / WORD_BITS;

// NOTE: This allocates ~125GB for N=1,000,000 (1,000,000 * 15,625 * 8 bytes).
// It will not fit on typical machines. Kept to satisfy the "matrix-based" request.
static uint64_t* adj_bits = nullptr; // size N * WORDS_PER_ROW

static inline uint64_t* row_ptr(int64_t u) {
    return adj_bits + u * WORDS_PER_ROW;
}

static inline void bitset_set(uint64_t* row, int64_t v) {
    const int64_t w = v >> 6;          // /64
    const int64_t b = v & 63;          // %64
    row[w] |= (uint64_t(1) << b);
}

static inline bool bitset_test(const uint64_t* row, int64_t v) {
    const int64_t w = v >> 6;
    const int64_t b = v & 63;
    return (row[w] >> b) & 1ULL;
}

// Portable popcount (prefer builtin if available)
static inline int popcount64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(x);
#else
    // fallback
    int c = 0;
    while (x) { x &= (x - 1); c++; }
    return c;
#endif
}

// Count |(A ∩ B) ∩ {w > min_w}| by masking off bits <= min_w.
// We’ll call this with min_w = v (so we count w > v), which guarantees unique triangle counting.
static inline int64_t bitset_intersection_popcount_gt(const uint64_t* A,
                                                      const uint64_t* B,
                                                      int64_t min_w) {
    int64_t total = 0;

    const int64_t min_word = (min_w + 1) >> 6; // first word that may contain bits > min_w
    const int64_t min_bit  = (min_w + 1) & 63; // first bit index within that word

    // Words strictly after min_word: no masking needed
    // But handle the boundary word min_word-1? Actually min_word computed for (min_w+1),
    // so the boundary word is min_word itself (contains bits from min_word*64 .. min_word*64+63).
    // Words before min_word have only <= min_w bits => all masked out => skip them.

    if (min_word < WORDS_PER_ROW) {
        // Boundary word: mask off bits < min_bit
        uint64_t x = A[min_word] & B[min_word];
        if (min_bit != 0) {
            const uint64_t mask = ~((uint64_t(1) << min_bit) - 1ULL);
            x &= mask;
        }
        total += popcount64(x);

        for (int64_t w = min_word + 1; w < WORDS_PER_ROW; w++) {
            total += popcount64(A[w] & B[w]);
        }
    }

    return total;
}

// -------------------- Build adjacency for the grid --------------------
// Undirected edges: each node connects to up/down/left/right (if in bounds).
// We set bits both directions.
static void build_grid_adjacency() {
    for (int64_t u = 0; u < N; u++) {
        uint64_t* ru = row_ptr(u);
        // row is already zeroed by calloc, nothing else to do here.
        (void)ru;
    }

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            const int64_t u = id_of(r, c);
            uint64_t* ru = row_ptr(u);

            if (r > 0) {
                const int64_t v = id_of(r - 1, c);
                bitset_set(ru, v);
                bitset_set(row_ptr(v), u);
            }
            if (r + 1 < ROWS) {
                const int64_t v = id_of(r + 1, c);
                bitset_set(ru, v);
                bitset_set(row_ptr(v), u);
            }
            if (c > 0) {
                const int64_t v = id_of(r, c - 1);
                bitset_set(ru, v);
                bitset_set(row_ptr(v), u);
            }
            if (c + 1 < COLS) {
                const int64_t v = id_of(r, c + 1);
                bitset_set(ru, v);
                bitset_set(row_ptr(v), u);
            }
        }
    }
}

// -------------------- Triangle count --------------------
static int64_t hart_triangles[HARTS];

static inline void enumerate_oriented_neighbors_gt_u(int64_t u,
                                                     int64_t* out, int& out_n,
                                                     int out_cap) {
    // For matrix-based code we can scan bits in row u, but scanning N bits for every u is O(N^2).
    // Since this grid is degree<=4, we just derive neighbors arithmetically and filter by v>u.
    // For a general graph you'd either:
    //   (a) store an explicit neighbor list, OR
    //   (b) scan only words that have bits set (still expensive without an index).
    out_n = 0;
    const int r = row_of(u);
    const int c = col_of(u);

    auto push_if = [&](int64_t v) {
        if (v > u && out_n < out_cap) out[out_n++] = v;
    };

    if (r > 0)        push_if(u - COLS);
    if (r + 1 < ROWS) push_if(u + COLS);
    if (c > 0)        push_if(u - 1);
    if (c + 1 < COLS) push_if(u + 1);
}

static void triangle_count_parallel(int total_harts) {
    const int hid = myThreadId();

    if (hid == 0) {
        for (int i = 0; i < total_harts; i++) thread_phase_counter[i] = 0;
        for (int i = 0; i < total_harts; i++) hart_triangles[i] = 0;

        // Allocate adjacency bit-matrix
        // calloc to zero-initialize bitsets
        const size_t total_words = size_t(N) * size_t(WORDS_PER_ROW);
        adj_bits = (uint64_t*)std::calloc(total_words, sizeof(uint64_t));
        if (!adj_bits) {
            std::printf("ERROR: failed to allocate adj_bits (%zu words ~ %zu bytes)\n",
                        total_words, total_words * sizeof(uint64_t));
            std::exit(1);
        }

        std::printf("TriangleCount (matrix/bitset) start: N=%ld, WORDS_PER_ROW=%ld, approx_mem=%0.2f GB\n",
                    (long)N, (long)WORDS_PER_ROW,
                    (double(total_words) * 8.0) / (1024.0 * 1024.0 * 1024.0));

        build_grid_adjacency();
        std::printf("Adjacency built (grid 4-neighbor). Expect 0 triangles.\n");
    }

    barrier(total_harts);

    // Slice vertices among harts
    const int64_t begin = (N * hid) / total_harts;
    const int64_t end   = (N * (hid + 1)) / total_harts;

    int64_t local = 0;

    // For each u in my slice:
    //   for each neighbor v with v>u:
    //     triangles += popcount( (row[u] & row[v]) with w>v )
    // This counts each triangle exactly once (u < v < w).
    int64_t neigh[8];
    int neigh_n = 0;

    for (int64_t u = begin; u < end; u++) {
        enumerate_oriented_neighbors_gt_u(u, neigh, neigh_n, 8);

        const uint64_t* Ru = row_ptr(u);

        for (int j = 0; j < neigh_n; j++) {
            const int64_t v = neigh[j];
            const uint64_t* Rv = row_ptr(v);

            // Count common neighbors w where w > v (so u < v < w)
            local += bitset_intersection_popcount_gt(Ru, Rv, v);
        }
    }

    hart_triangles[hid] = local;

    barrier(total_harts);

    if (hid == 0) {
        int64_t total = 0;
        for (int i = 0; i < total_harts; i++) total += hart_triangles[i];

        std::printf("TriangleCount done. triangles=%ld\n", (long)total);

        // cleanup
        std::free(adj_bits);
        adj_bits = nullptr;
    }

    barrier(total_harts);
}

int main(int argc, char** argv) {
    triangle_count_parallel(HARTS);
    return 0;
}
