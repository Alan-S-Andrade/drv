// SPDX-License-Identifier: MIT
/* Copyright (c) 2023 Advanced Micro Devices, Inc. All rights reserved. */

// Multihart SpMM (CSR * dense features) with row-sliced ownership.
// - hart0 reads graph + features and builds CSR + allocates output
// - all harts barrier
// - each hart computes a disjoint range of rows (no output races, no atomics in compute)
// - barrier again, hart0 may validate/print
//
// NOTE: This intentionally does NOT use MergePath because MergePath creates boundary-row sharing,
// which would require atomic adds or a reduction.

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <vector>

#include <pandohammer/cpuinfo.h>
#include <pandohammer/mmio.h>
#include <pandohammer/atomic.h>
#include <pandohammer/hartsleep.h>

// -------------------- Globals (shared) --------------------
static std::vector<std::vector<float>> features;
static std::vector<std::vector<float>> output;

// Runtime barrier config published by global hart0
static volatile int64_t g_ready = 0;
static volatile int64_t g_total_harts = 0;
static volatile int64_t g_harts_per_core = 0;

// Barrier state
static volatile int64_t g_barrier_count = 0;
static volatile int64_t g_barrier_phase = 0;
static int64_t* g_phase_ctr = nullptr; // size = total_harts (allocated by hart0)

// -------------------- CSR --------------------
class CSRMatrix {
public:
    std::vector<float> values;     // nnz
    std::vector<int>   colIndices; // nnz
    std::vector<int>   rowOffsets; // numRows+1
    int numRows = 0;
    int numCols = 0;

    CSRMatrix(int num_rows,
              int num_cols,
              int num_edges,
              std::vector<int>& coo_row_indices,
              std::vector<int>& coo_col_indices)
        : numRows(num_rows), numCols(num_cols)
    {
        values.assign(num_edges, 1.0f);

        // Sort COO by (row, col) so each CSR row is contiguous
        std::vector<std::tuple<int,int>> t;
        t.reserve(coo_row_indices.size());
        for (size_t i = 0; i < coo_row_indices.size(); i++) {
            t.emplace_back(coo_row_indices[i], coo_col_indices[i]);
        }
        std::sort(t.begin(), t.end(),
                  [](const auto& a, const auto& b) {
                      if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) < std::get<0>(b);
                      return std::get<1>(a) < std::get<1>(b);
                  });

        colIndices.resize(t.size());
        rowOffsets.assign(numRows + 1, 0);

        // Count nnz per row
        for (size_t i = 0; i < t.size(); i++) {
            int r = std::get<0>(t[i]);
            int c = std::get<1>(t[i]);
            assert(r >= 0 && r < numRows);
            assert(c >= 0 && c < numCols);
            colIndices[i] = c;
            rowOffsets[r + 1]++; // count
        }

        // Prefix sum -> offsets
        for (int r = 0; r < numRows; r++) {
            rowOffsets[r + 1] += rowOffsets[r];
        }

        // NOTE: Since we sorted by (row,col), colIndices already appear in row-major order.
        // values[] are all 1.0f.
    }
};

static CSRMatrix* g_graph = nullptr;

// -------------------- I/O --------------------
static int readMtx(const char* fname,
                   int& num_rows, int& num_cols, int& num_nzs,
                   std::vector<int>& row_indices, std::vector<int>& col_indices)
{
    FILE* fp = fopen(fname, "r");
    std::printf("Reading file '%s'\n", fname);
    if (!fp) {
        return -1;
    }

    if (fscanf(fp, "%d %d %d", &num_rows, &num_cols, &num_nzs) != 3) {
        std::printf("Error: bad header in '%s'\n", fname);
        fclose(fp);
        return -1;
    }

    row_indices.resize(num_nzs);
    col_indices.resize(num_nzs);

    for (int i = 0; i < num_nzs; i++) {
        int src, dst;
        if (fscanf(fp, "%d %d", &src, &dst) != 2) {
            std::printf("Error: unexpected EOF in '%s'\n", fname);
            fclose(fp);
            return -1;
        }

        // If your MTX is 1-based, uncomment:
        // src--; dst--;

        row_indices[i] = src;
        col_indices[i] = dst;
    }

    fclose(fp);
    return 0;
}

static int readFeatures(const char* fname, std::vector<std::vector<float>>& feats)
{
    FILE* fp = fopen(fname, "r");
    std::printf("Reading file '%s'\n", fname);
    if (!fp) {
        return -1;
    }

    int num_rows, num_cols;
    if (fscanf(fp, "%d %d", &num_rows, &num_cols) != 2) {
        std::printf("Error: bad header in '%s'\n", fname);
        fclose(fp);
        return -1;
    }

    feats.assign(num_rows, std::vector<float>(num_cols, 0.0f));

    for (int i = 0; i < num_rows; i++) {
        for (int j = 0; j < num_cols; j++) {
            float v;
            if (fscanf(fp, "%f", &v) != 1) {
                std::printf("Error: unexpected EOF in '%s'\n", fname);
                fclose(fp);
                return -1;
            }
            feats[i][j] = v;
        }
    }

    fclose(fp);
    return 0;
}

// -------------------- Runtime + barrier helpers --------------------
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

    // If this trips, your (core,thread)->hid mapping or passed args are inconsistent.
    if (hid < 0 || hid >= th || g_phase_ctr == nullptr) {
        // Don't corrupt barrier state; park this hart.
        // (You can printf here if you want, but it may be noisy.)
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

// -------------------- SpMM kernel (NO atomics) --------------------
// output[row][f] = sum_{(row->nbr) in CSR} values * features[nbr][f]
static void spmm_rowsliced_noatomics() {
    const int hid = global_hid();
    const int th  = total_harts();

    const int numRows = g_graph->numRows;
    const int featDim = (int)features[0].size();

    // Row slicing among all harts (dense hid 0..th-1)
    const int r0 = (int)((int64_t)numRows * hid / th);
    const int r1 = (int)((int64_t)numRows * (hid + 1) / th);

    for (int row = r0; row < r1; row++) {
        const int start = g_graph->rowOffsets[row];
        const int stop  = g_graph->rowOffsets[row + 1];

        // For each feature dimension
        for (int f = 0; f < featDim; f++) {
            float acc = 0.0f;
            for (int off = start; off < stop; off++) {
                const int nbr = g_graph->colIndices[off];
                acc += g_graph->values[off] * features[nbr][f];
            }
            output[row][f] = acc; // exclusive ownership => safe
        }
    }
}

int main(int argc, const char** argv) {
    // Expect: <total_harts> <harts_per_core>
    // Example (8 cores * 16 harts/core): ./spmm 128 16
    const int cid = (int)myCoreId();
    const int tid = (int)myThreadId();
    const bool is_global0 = (cid == 0 && tid == 0);

    if (is_global0) {

        const int th  = 16;
        const int hpc = 16;
        if (th <= 0 || hpc <= 0) {
            std::printf("Bad args: total_harts=%d harts_per_core=%d\n", th, hpc);
            std::exit(1);
        }

        // Publish config
        atomic_swap_i64((volatile int64_t*)&g_total_harts, (int64_t)th);
        atomic_swap_i64((volatile int64_t*)&g_harts_per_core, (int64_t)hpc);

        // Allocate per-hart phase counters for barrier
        g_phase_ctr = (int64_t*)std::malloc((size_t)th * sizeof(int64_t));
        if (!g_phase_ctr) {
            std::printf("malloc g_phase_ctr failed\n");
            std::exit(1);
        }
        for (int i = 0; i < th; i++) g_phase_ctr[i] = 0;

        std::printf("SpMM multihart (row-sliced, no atomics in compute)\n");
        std::printf("total_harts=%d harts_per_core=%d\n", th, hpc);

        // Read graph and features
        int numRows, numCols, numNzs;
        std::vector<int> rowIdx, colIdx;

        if (readMtx("spmm.graph.mtx", numRows, numCols, numNzs, rowIdx, colIdx) != 0) {
            std::printf("readMtx failed\n");
            std::exit(1);
        }

        if (readFeatures("spmm.features", features) != 0) {
            std::printf("readFeatures failed\n");
            std::exit(1);
        }

        // Sanity: features must have at least numCols rows (since we index features[nbr])
        if ((int)features.size() < numCols) {
            std::printf("ERROR: features rows (%d) < graph numCols (%d)\n", (int)features.size(), numCols);
            std::exit(1);
        }
        if (features.empty() || features[0].empty()) {
            std::printf("ERROR: empty features\n");
            std::exit(1);
        }

        // Build CSR
        g_graph = new CSRMatrix(numRows, numCols, numNzs, rowIdx, colIdx);

        // Allocate output
        output.assign(numRows, std::vector<float>((int)features[0].size(), 0.0f));

        // LAST: release all harts
        atomic_swap_i64((volatile int64_t*)&g_ready, 1);
    }

    // All harts synchronize on init
    barrier();

    // Compute
    spmm_rowsliced_noatomics();

    // Sync
    barrier();

    if (is_global0) {
        std::printf("Done. Sample output:\n");
        const int sample_r = std::min(g_graph->numRows, 8);
        const int sample_c = std::min((int)features[0].size(), 4);
        for (int r = 0; r < sample_r; r++) {
            std::printf("row %d: ", r);
            for (int c = 0; c < sample_c; c++) {
                std::printf("%f ", output[r][c]);
            }
            std::printf("\n");
        }

        delete g_graph;
        g_graph = nullptr;
        std::free(g_phase_ctr);
        g_phase_ctr = nullptr;
    }

    barrier();
    return 0;
}
